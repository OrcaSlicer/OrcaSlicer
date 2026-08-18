#include "MagmaInjectionOrder.hpp"

#include "../libslic3r.h"      // unscale<>
#include "../ShortestPath.hpp" // chain_points

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace magma {

// ---------------------------------------------------------------------------
// Heat-spread injection ordering.
//
// The objective: keep spatially-near injections far apart in *time* so the heat
// of one has dissipated before a neighbour is touched. We model that directly
// with a decay kernel  exp(-Δt/τ) · exp(-dist/λ)  summed over near pairs, where
// Δt is the real elapsed injection time between the two events.
//
// Pipeline (all O(n^2) or better, n is per-layer injection count, deterministic):
//   1. time-decay dispersion greedy  -> a good order, full-history-aware
//   2. violation-directed local search (swaps) -> dissolves residual clusters,
//      including any end-of-pass "painted into a corner".
//
// We measured CP-SAT here (warm-started from the polished order) and it returned
// the identical order while costing seconds of solve time, so it was removed:
// greedy+polish reaches ~0 crowded near-pairs in microseconds. CP-SAT still earns
// its place in tube *placement* (MagmaTubeSolver), just not in sequencing.
// ---------------------------------------------------------------------------

std::vector<size_t> order_injection_points(
    const Points& pts,
    const std::vector<double>& vol_mm3,
    const InjectionTiming& timing,
    bool spread_heat,
    const std::function<void()>& throw_if_canceled)
{
    const int n = static_cast<int>(pts.size());
    std::vector<size_t> identity(std::max(0, n));
    std::iota(identity.begin(), identity.end(), size_t(0));
    if (n <= 2)
        return identity;

    // Travel-optimal baseline (also the greedy's starting node anchor).
    std::vector<size_t> tsp = chain_points(pts);
    if ((int) tsp.size() != n) {
        // A short tour means chain_points dropped points. Returning it -- which this used to
        // do, directly under a comment saying it would not -- drops those injections entirely:
        // the caller emits one target per returned index, so the missing tubes print as
        // lattice and are never filled, with no diagnostic. Fall back to the identity order,
        // which is worse routing but injects every tube.
        BOOST_LOG_TRIVIAL(error)
            << "Magma: injection point ordering returned " << tsp.size() << " of " << n
            << " points; using unordered injection order so no tube is skipped.";
        return identity;
    }
    if (!spread_heat)
        return tsp;

    if (n > 4000) {
        BOOST_LOG_TRIVIAL(warning) << "Magma: " << n << " injection points on a layer "
            "exceeds the spread-heat solver limit; used travel-optimal order";
        return tsp;
    }
    if (throw_if_canceled) throw_if_canceled();

    // ---- geometry + timing primitives -------------------------------------
    auto dist_mm = [&](int i, int j) -> double {
        double dx = unscale<double>(double(pts[i].x() - pts[j].x()));
        double dy = unscale<double>(double(pts[i].y() - pts[j].y()));
        return std::sqrt(dx * dx + dy * dy);
    };
    auto travel_time = [&](int i, int j) -> double {
        double v = timing.travel_speed_mm_s > 1.0 ? timing.travel_speed_mm_s : 1.0;
        return dist_mm(i, j) / v;
    };
    auto inject_time = [&](int c) -> double {
        double v = timing.vol_speed_mm3_s > 0.01 ? timing.vol_speed_mm3_s : 0.01;
        double vol = (c < (int) vol_mm3.size() && vol_mm3[c] > 0.0) ? vol_mm3[c] : 0.0;
        return vol / v + timing.per_injection_fixed_s;
    };

    // Nearest-neighbour spacing (median) sets the thermal length scale λ.
    std::vector<double> nn(n, std::numeric_limits<double>::max());
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j) nn[i] = std::min(nn[i], dist_mm(i, j));
    std::vector<double> nn_sorted = nn;
    std::sort(nn_sorted.begin(), nn_sorted.end());
    const double median_nn = std::max(0.01, nn_sorted[n / 2]);
    const double LAMBDA = median_nn;             // spatial decay length (immediate ring)
    const double R = median_nn * 1.8;            // only this ring couples thermally

    // Temporal decay τ: aim to separate near pairs by ~SEP_TARGET injections'
    // worth of time. Derive the characteristic step time from the TSP order so τ
    // adapts to the print's pace instead of being a hand-set constant.
    const double SEP_TARGET = 8.0;
    std::vector<double> tsp_steps;
    tsp_steps.reserve(n);
    for (int k = 0; k + 1 < n; ++k)
        tsp_steps.push_back(travel_time((int) tsp[k], (int) tsp[k + 1]) + inject_time((int) tsp[k + 1]));
    std::sort(tsp_steps.begin(), tsp_steps.end());
    const double median_step = tsp_steps.empty() ? 1.0 : std::max(0.01, tsp_steps[tsp_steps.size() / 2]);
    const double TAU = SEP_TARGET * median_step;

    // Near pairs + adjacency. sp = spatial coupling weight exp(-dist/λ).
    struct NearPair { int i, j; double sp; };
    std::vector<NearPair> near;
    std::vector<std::vector<std::pair<int, double>>> adj(n);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            double d = dist_mm(i, j);
            if (d <= R) {
                double sp = std::exp(-d / LAMBDA);
                near.push_back({ i, j, sp });
                adj[i].push_back({ j, sp });
                adj[j].push_back({ i, sp });
            }
        }

    // ---- objective evaluators (shared by greedy, polish, metrics) ----------
    auto inverse = [&](const std::vector<size_t>& order) {
        std::vector<int> rk(n, 0);
        for (int t = 0; t < n; ++t) rk[(int) order[t]] = t;
        return rk;
    };
    // Real-time cumulative timeline for an order (seconds at each visit).
    auto timeline = [&](const std::vector<size_t>& order) {
        std::vector<double> t(n, 0.0);
        for (int k = 1; k < n; ++k)
            t[k] = t[k - 1] + travel_time((int) order[k - 1], (int) order[k]) + inject_time((int) order[k]);
        return t;
    };
    // The true objective: decay heat over near pairs using real elapsed time.
    auto decay_heat = [&](const std::vector<size_t>& order) {
        std::vector<int> rk = inverse(order);
        std::vector<double> t = timeline(order);
        double h = 0.0;
        for (const NearPair& np : near) {
            double dt = std::fabs(t[rk[np.i]] - t[rk[np.j]]);
            h += np.sp * std::exp(-dt / TAU);
        }
        return h;
    };
    auto travel_cmm = [&](const std::vector<size_t>& order) {
        double t = 0.0;
        for (int k = 0; k + 1 < n; ++k) t += dist_mm((int) order[k], (int) order[k + 1]);
        t += dist_mm((int) order[n - 1], (int) order[0]);
        return (int64_t) std::llround(t * 100.0);
    };
    // Human-readable proxy: immediate-ring pairs injected < WINDOW events apart.
    const int WINDOW = std::min(n, 8);
    auto crowded_pairs = [&](const std::vector<size_t>& order) {
        std::vector<int> rk = inverse(order);
        int c = 0;
        for (const NearPair& np : near) {
            int gap = rk[np.i] - rk[np.j]; if (gap < 0) gap = -gap;
            if (gap < WINDOW) ++c;
        }
        return c;
    };

    // ---- 1. time-decay dispersion greedy ----------------------------------
    // G[c] = residual heat at remaining candidate c as of the current nozzle
    // time. Each step pick the coolest reachable spot (heat decayed by the time
    // it takes to get there), with a mild travel tiebreak so we don't wander.
    const double BETA = 1.0;  // travel tiebreak weight (seconds -> heat units)
    std::vector<size_t> greedy;
    {
        greedy.reserve(n);
        std::vector<char> placed(n, 0);
        std::vector<double> G(n, 0.0);
        int cur = (int) tsp[0];
        greedy.push_back((size_t) cur);
        placed[cur] = 1;
        for (auto& nb : adj[cur]) if (!placed[nb.first]) G[nb.first] += nb.second;

        for (int step = 1; step < n; ++step) {
            int best = -1;
            double best_score = std::numeric_limits<double>::max();
            for (int c = 0; c < n; ++c) {
                if (placed[c]) continue;
                double tt = travel_time(cur, c);
                double score = G[c] * std::exp(-tt / TAU) + BETA * tt;
                if (score < best_score) { best_score = score; best = c; }
            }
            double step_t = travel_time(cur, best) + inject_time(best);
            double decay = std::exp(-step_t / TAU);
            for (int c = 0; c < n; ++c) if (!placed[c]) G[c] *= decay;
            for (auto& nb : adj[best]) if (!placed[nb.first]) G[nb.first] += nb.second;
            placed[best] = 1;
            greedy.push_back((size_t) best);
            cur = best;
        }
    }
    if (throw_if_canceled) throw_if_canceled();

    // ---- 2. violation-directed local-search polish (swaps) ----------------
    // Hill-climb on the rank-gap proxy: swapping two positions only changes the
    // two moved tubes' ranks, so each delta is O(degree). Dissolves clusters the
    // greedy may have left (including any end-of-pass "painted into a corner").
    std::vector<size_t> polished = greedy;
    {
        std::vector<int> rk = inverse(polished);
        auto incident = [&](int u) {
            double s = 0.0;
            for (auto& nb : adj[u]) {
                int gap = rk[u] - rk[nb.first]; if (gap < 0) gap = -gap;
                int pen = WINDOW - gap; if (pen > 0) s += nb.second * pen;
            }
            return s;
        };
        auto pair_cost = [&](int u, int v) {
            for (auto& nb : adj[u])
                if (nb.first == v) {
                    int gap = rk[u] - rk[v]; if (gap < 0) gap = -gap;
                    int pen = WINDOW - gap; return pen > 0 ? nb.second * pen : 0.0;
                }
            return 0.0;
        };
        const int MAX_PASSES = 20;
        bool improved = true;
        for (int pass = 0; pass < MAX_PASSES && improved; ++pass) {
            if (throw_if_canceled) throw_if_canceled();
            improved = false;
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    int u = (int) polished[i], v = (int) polished[j];
                    double before = incident(u) + incident(v) - pair_cost(u, v);
                    rk[u] = j; rk[v] = i;
                    double after = incident(u) + incident(v) - pair_cost(u, v);
                    if (after + 1e-9 < before) {
                        std::swap(polished[i], polished[j]);  // ranks already updated
                        improved = true;
                    } else {
                        rk[u] = i; rk[v] = j;                 // revert
                    }
                }
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Magma spread-heat: n=" << n
        << " | crowded near-pairs " << crowded_pairs(tsp) << "->" << crowded_pairs(polished)
        << " of " << near.size()
        << " | decay-heat " << decay_heat(tsp) << "->" << decay_heat(polished)
        << " | travel(cmm) " << travel_cmm(tsp) << "->" << travel_cmm(polished);

    return polished;
}

} // namespace magma
} // namespace Slic3r
