#include "MagmaInjectionOrder.hpp"

#include "../libslic3r.h"      // unscale<>
#include "../ShortestPath.hpp" // chain_points

#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace magma {

std::vector<size_t> order_injection_points(const Points& pts, bool spread_heat,
        double time_limit_s, const std::function<void()>& throw_if_canceled)
{
    const int n = static_cast<int>(pts.size());
    std::vector<size_t> identity(std::max(0, n));
    std::iota(identity.begin(), identity.end(), size_t(0));
    if (n <= 2)
        return identity;

    // Travel-optimal baseline — also the CP-SAT warm start.
    std::vector<size_t> tsp = chain_points(pts);
    if (!spread_heat)
        return tsp;

    // Very large layers: the routing model gets expensive to build; keep travel
    // order (warn). The user-visible cost is no heat spread on that layer only.
    if (n > 250) {
        BOOST_LOG_TRIVIAL(warning) << "Magma: " << n << " injection points on a layer "
            "exceeds the spread-heat solver limit; used travel-optimal order";
        return tsp;
    }
    if (throw_if_canceled) throw_if_canceled();

    using operations_research::Domain;
    using namespace operations_research::sat;

    auto dist_cmm = [&](int i, int j) -> int64_t {
        double dx = unscale<double>(double(pts[i].x() - pts[j].x()));
        double dy = unscale<double>(double(pts[i].y() - pts[j].y()));
        return (int64_t) std::llround(std::sqrt(dx * dx + dy * dy) * 100.0);  // centi-mm
    };

    // Nearest-neighbour spacing (median) sets the thermal radius scale.
    std::vector<int64_t> nn(n, std::numeric_limits<int64_t>::max());
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j) nn[i] = std::min(nn[i], dist_cmm(i, j));
    std::vector<int64_t> nn_sorted = nn;
    std::sort(nn_sorted.begin(), nn_sorted.end());
    const int64_t median_nn = std::max<int64_t>(1, nn_sorted[n / 2]);

    // Heat coupling is short-range: only the immediate ring of neighbours.
    const int64_t R = median_nn * 9 / 5;  // ~1.8x spacing

    // Distance-tiered near pairs: immediately-adjacent injections punished 2x.
    struct NearPair { int i, j; int64_t w; };
    std::vector<NearPair> near;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            int64_t d = dist_cmm(i, j);
            if (d <= R)
                near.push_back({ i, j, (d <= median_nn * 6 / 5) ? int64_t(2) : int64_t(1) });
        }

    CpModelBuilder model;

    // Hamiltonian-circuit arcs + travel cost.
    std::vector<std::vector<BoolVar>> arc(n, std::vector<BoolVar>(n));
    CircuitConstraint circuit = model.AddCircuitConstraint();
    LinearExpr travel;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j) {
                arc[i][j] = model.NewBoolVar();
                circuit.AddArc(i, j, arc[i][j]);
                travel += dist_cmm(i, j) * arc[i][j];
            }

    // Visiting position (rank) via MTZ. The tour start is the TSP's first node
    // so the warm-start below (rank[tsp[t]] = t) is consistent with rank[start]=0;
    // hardcoding node 0 as the start would conflict with the hint whenever the
    // TSP doesn't begin at node 0, causing CP-SAT to discard the warm start.
    const int start = (int) tsp[0];
    std::vector<IntVar> rank(n);
    for (int i = 0; i < n; ++i)
        rank[i] = model.NewIntVar(Domain(0, n - 1));
    model.AddEquality(rank[start], 0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j && j != start)  // the closing arc into `start` must not bump its rank
                model.AddEquality(rank[j], LinearExpr(rank[i]) + 1).OnlyEnforceIf(arc[i][j]);

    // Penalty: near pairs should be >= WINDOW injections apart in time. pen is
    // minimized in the objective so it settles to max(0, WINDOW - gap).
    const int WINDOW = std::min(n, 8);
    LinearExpr heat;
    for (const NearPair& np : near) {
        IntVar gap = model.NewIntVar(Domain(0, n - 1));
        model.AddAbsEquality(gap, LinearExpr(rank[np.i]) - rank[np.j]);
        IntVar pen = model.NewIntVar(Domain(0, WINDOW));
        model.AddGreaterOrEqual(pen, LinearExpr(WINDOW) - gap);
        heat += (median_nn * np.w) * pen;  // one rank-unit of crowding ~ w nearest-neighbour hops
    }

    model.Minimize(travel + heat);

    // Warm start from the TSP order: ranks + consecutive arcs (+ closing arc).
    for (int t = 0; t < n; ++t)
        model.AddHint(rank[(int) tsp[t]], t);
    for (int t = 0; t + 1 < n; ++t)
        model.AddHint(arc[(int) tsp[t]][(int) tsp[t + 1]], true);
    model.AddHint(arc[(int) tsp[n - 1]][(int) tsp[0]], true);

    SatParameters params;
    params.set_max_time_in_seconds(time_limit_s);
    params.set_num_workers(4);

    operations_research::sat::Model sat_model;
    sat_model.Add(NewSatParameters(params));
    CpSolverResponse response = SolveCpModel(model.Build(), &sat_model);

    if (response.status() == CpSolverStatus::OPTIMAL ||
        response.status() == CpSolverStatus::FEASIBLE) {
        std::vector<size_t> result(n, 0);
        std::vector<bool> filled(n, false);
        bool ok = true;
        for (int i = 0; i < n && ok; ++i) {
            int r = (int) SolutionIntegerValue(response, rank[i]);
            if (r < 0 || r >= n || filled[r]) { ok = false; break; }
            result[r] = (size_t) i;
            filled[r] = true;
        }
        if (ok)
            return result;
    }
    BOOST_LOG_TRIVIAL(warning) << "Magma: spread-heat injection solve found no usable "
        "order for " << n << " points; used travel-optimal order";
    return tsp;
}

} // namespace magma
} // namespace Slic3r
