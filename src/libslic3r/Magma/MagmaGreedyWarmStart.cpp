#include "MagmaGreedyWarmStart.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <queue>
#include <set>
#include <vector>

namespace Slic3r {
namespace magma {

// ============================================================================
// Greedy Warm Start — design rationale
// ============================================================================
//
// WHY GREEDY BEFORE CP-SAT:
// CP-SAT with a warm start converges much faster than cold start. The greedy
// heuristic produces ~77-81% coverage in milliseconds, giving CP-SAT a strong
// initial solution. Without it, CP-SAT blocks often time out at UNKNOWN with
// poor solutions because the search space is too large to explore from scratch
// within the per-block timeout.
//
// WHY MOST-CONSTRAINED-FIRST:
// Cells at object boundaries have fewer neighbors (1 vs 3 for interior cells).
// If interior cells are processed first, they consume all available Z ranges,
// leaving boundary cells stranded with no valid pairing. Processing the most
// constrained cells first (fewest viable neighbors, then shortest best tube)
// prevents stranding. This is the classic CSP heuristic applied greedily.
//
// LOCALITY-AWARE NEIGHBOR SELECTION:
// When choosing which neighbor to pair with, the greedy picks the neighbor
// yielding the longest tube from this layer (not the globally most constrained
// neighbor). Among equal-height candidates, it prefers the most constrained
// neighbor to reduce stranding risk. This avoids wasting cells on short tubes
// when a longer tube is available with a different neighbor.
//
// WHY NO STAGGER:
// The greedy stage produces the longest possible tubes with no stagger logic.
// "Natural stagger" from the priority ordering is incidental — it's not designed
// or guaranteed. Stagger is the CP-SAT stage's responsibility: the domain
// restriction gives the solver grid-aligned boundary options that greedy never
// used, allowing it to shorten some tubes for better boundary alignment.
//

// ============================================================================
// CellConsumed — tracks consumed Z-ranges per cell (sorted, non-overlapping)
// ============================================================================

namespace {

struct CellConsumed {
    // Sorted non-overlapping intervals (start_um, end_um)
    std::vector<std::pair<int64_t, int64_t>> intervals;

    bool overlaps(int64_t start_um, int64_t end_um) const {
        // Binary search for first interval that could overlap
        auto it = std::lower_bound(intervals.begin(), intervals.end(),
            std::make_pair(start_um, start_um),
            [](const std::pair<int64_t, int64_t> &a,
               const std::pair<int64_t, int64_t> &b) {
                return a.second <= b.first;
            });
        return it != intervals.end() && it->first < end_um;
    }

    void add(int64_t start_um, int64_t end_um) {
        // Find merge range: all existing intervals that overlap or touch [start, end]
        auto lo = std::lower_bound(intervals.begin(), intervals.end(),
            std::make_pair(start_um, start_um),
            [](const std::pair<int64_t, int64_t> &a,
               const std::pair<int64_t, int64_t> &b) {
                return a.second < b.first;
            });
        auto hi = lo;
        while (hi != intervals.end() && hi->first <= end_um) {
            start_um = std::min(start_um, hi->first);
            end_um   = std::max(end_um, hi->second);
            ++hi;
        }
        auto pos = intervals.erase(lo, hi);
        intervals.insert(pos, {start_um, end_um});
    }
};

// ============================================================================
// CellLayerScore — priority queue entry (min-heap, lexicographic)
// ============================================================================
//
// Two-key priority:
//   Primary:   num_viable_neighbors (fewer = higher priority, stranding risk)
//   Secondary: best_tube_height_um  (shorter = higher priority, limited outcome)

struct CellLayerScore {
    TriangleCell cell;
    int          layer;
    int64_t      layer_bottom_um;
    int64_t      layer_top_um;
    int          num_viable;        // fewer neighbors = more urgent
    int64_t      best_tube_h_um;    // shorter best tube = more urgent

    bool operator>(const CellLayerScore &o) const {
        if (num_viable != o.num_viable) return num_viable > o.num_viable;
        return best_tube_h_um > o.best_tube_h_um;
    }
};

// Find the run in an edge that contains a given layer. Returns nullptr if none.
const Run *find_run_containing(const EdgeData &ed, int layer, int64_t bottom_um, int64_t top_um)
{
    for (const Run &r : ed.runs) {
        if (layer >= r.start_layer && layer <= r.end_layer &&
            bottom_um >= r.start_um && top_um <= r.end_um)
            return &r;
    }
    return nullptr;
}

// Expand a tube centered on center_layer to its maximum valid extent.
// Expands downward then upward within the run, stopping at consumed
// intervals, max_h_um, or run boundaries. Returns (start_um, end_um).
std::pair<int64_t, int64_t> expand_tube(
    int center_layer, const Run &run,
    const CellConsumed &consumed_a, const CellConsumed &consumed_b,
    const MicronTables &um, int64_t max_h_um)
{
    int64_t tube_start = um.bottom_um[center_layer];
    int64_t tube_end   = um.top_um[center_layer];

    for (int lo = center_layer - 1; lo >= run.start_layer; --lo) {
        int64_t bot = um.bottom_um[lo];
        int64_t top = um.top_um[lo];
        if (tube_end - bot > max_h_um) break;
        if (consumed_a.overlaps(bot, top) || consumed_b.overlaps(bot, top)) break;
        tube_start = bot;
    }
    for (int hi = center_layer + 1; hi <= run.end_layer; ++hi) {
        int64_t bot = um.bottom_um[hi];
        int64_t top = um.top_um[hi];
        if (top - tube_start > max_h_um) break;
        if (consumed_a.overlaps(bot, top) || consumed_b.overlaps(bot, top)) break;
        tube_end = top;
    }
    return {tube_start, tube_end};
}

// Count unconsumed layers in a run for a given cell
int count_unconsumed_layers(const Run &run,
                            const CellConsumed &consumed_a,
                            const CellConsumed &consumed_b,
                            const MicronTables &um)
{
    int count = 0;
    for (int L = run.start_layer; L <= run.end_layer; ++L) {
        int64_t bot = um.bottom_um[L];
        int64_t top = um.top_um[L];
        if (!consumed_a.overlaps(bot, top) && !consumed_b.overlaps(bot, top))
            ++count;
    }
    return count;
}

} // anonymous namespace

// ============================================================================
// greedy_warm_start
// ============================================================================

void greedy_warm_start(
    const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &cells,
    const std::vector<EdgeData>                                             &edges,
    const std::unordered_map<TriangleCell, std::vector<size_t>, TriangleCellHash> &cell_edges,
    const MicronTables                                                      &um,
    int64_t min_h_um,
    int64_t max_h_um,
    std::vector<std::vector<CommittedSegment>>                              &committed,
    CellDifficultyMap                                                       *out_difficulty)
{
    auto t_start = std::chrono::high_resolution_clock::now();

    // Consumed intervals per cell
    std::unordered_map<TriangleCell, CellConsumed, TriangleCellHash> consumed;

    // ------------------------------------------------------------------
    // Difficulty map: per-cell per-layer score of neighborhood flexibility.
    //
    // Computed BEFORE any assignments (consumed is empty) so it captures the
    // unconstrained potential of each cell×layer. The CP-SAT solver uses this
    // to decide which runs are safe for grid domain restriction (stagger):
    // easy neighborhoods get restricted, hard ones keep full domains.
    //
    // Score = sum of achievable tube heights across all neighbors (same as
    // build_heap below). Inverted to difficulty: 0 = easiest, higher = harder.
    //   difficulty = max_possible - score
    //   max_possible = 3 × max_h_um  (3 neighbors, each offering max height)
    // ------------------------------------------------------------------
    if (out_difficulty) {
        const int64_t max_possible = 3 * max_h_um;
        CellConsumed empty_consumed; // empty — unconstrained baseline

        for (const auto &[cell, presence] : cells) {
            auto ce_it = cell_edges.find(cell);
            if (ce_it == cell_edges.end()) continue;

            auto &diff_vec = (*out_difficulty)[cell];
            diff_vec.assign(presence.last_layer - presence.first_layer + 1, max_possible);

            for (int L = presence.first_layer; L <= presence.last_layer; ++L) {
                if (!presence.present(L)) continue;

                int64_t layer_bot = um.bottom_um[L];
                int64_t layer_top = um.top_um[L];
                double score = 0.0;

                for (size_t ei : ce_it->second) {
                    const EdgeData &ed = edges[ei];
                    const Run *run = find_run_containing(ed, L, layer_bot, layer_top);
                    if (!run) continue;

                    auto [tube_start, tube_end] = expand_tube(
                        L, *run, empty_consumed, empty_consumed, um, max_h_um);
                    int64_t potential = tube_end - tube_start;
                    if (potential >= min_h_um)
                        score += double(potential);
                }

                int64_t difficulty = max_possible - llround(score);
                if (difficulty < 0) {
                    BOOST_LOG_TRIVIAL(warning) << "MagmaGreedy: score " << llround(score)
                        << " exceeds max_possible " << max_possible
                        << " for cell(" << cell.a << "," << cell.b << ") layer " << L;
                    difficulty = 0;
                }
                diff_vec[L - presence.first_layer] = difficulty;
            }
        }
    }

    using MinHeap = std::priority_queue<CellLayerScore, std::vector<CellLayerScore>,
                                         std::greater<CellLayerScore>>;

    // Periodic re-scoring interval. As tubes are assigned, heap entries become
    // stale — cell×layers that were "easy" (high score) may now be constrained
    // because their neighbors were consumed. Rebuilding the heap periodically
    // corrects the priority ordering so most-constrained-first stays effective.
    //
    // Tradeoff: too frequent = wasted time rebuilding heaps (O(cells × layers ×
    // edges per cell)). Too infrequent = stale priorities cause suboptimal
    // assignments (easy cells processed before hard ones, leading to stranding).
    // The edges/3 heuristic triggers ~3-10 re-scores during the main assignment
    // wave, empirically balancing quality and speed. Floor of 200 prevents churn
    // on tiny models where each rebuild is cheap anyway.
    const int rescore_every = std::max(200, static_cast<int>(edges.size()) / 3);

    // ------------------------------------------------------------------
    // Build heap: score unconsumed cell×layers
    // ------------------------------------------------------------------
    // Lexicographic scoring: (num_viable_neighbors, best_tube_height).
    //   - Fewer viable neighbors = higher priority (stranding risk)
    //   - Among equal neighbor counts, shorter best tube = higher priority
    //     (limited outcome — do it now while options exist)

    auto build_heap = [&]() -> MinHeap {
        MinHeap heap;
        for (const auto &[cell, presence] : cells) {
            auto ce_it = cell_edges.find(cell);
            if (ce_it == cell_edges.end()) continue;

            for (int L = presence.first_layer; L <= presence.last_layer; ++L) {
                if (!presence.present(L)) continue;

                int64_t layer_bot = um.bottom_um[L];
                int64_t layer_top = um.top_um[L];

                if (consumed[cell].overlaps(layer_bot, layer_top)) continue;

                int num_viable = 0;
                int64_t best_h = 0;

                for (size_t ei : ce_it->second) {
                    const EdgeData &ed = edges[ei];
                    const Run *run = find_run_containing(ed, L, layer_bot, layer_top);
                    if (!run) continue;

                    const TriangleCell &neighbor =
                        (ed.edge.a == cell) ? ed.edge.b : ed.edge.a;
                    if (consumed[neighbor].overlaps(layer_bot, layer_top)) continue;

                    auto [tube_start, tube_end] = expand_tube(
                        L, *run, consumed[cell], consumed[neighbor], um, max_h_um);
                    int64_t potential = tube_end - tube_start;
                    if (potential >= min_h_um) {
                        ++num_viable;
                        best_h = std::max(best_h, potential);
                    }
                }

                if (num_viable > 0)
                    heap.push({cell, L, layer_bot, layer_top, num_viable, best_h});
            }
        }
        return heap;
    };

    // ------------------------------------------------------------------
    // Greedy assignment with periodic + drift-adaptive re-scoring
    // ------------------------------------------------------------------
    //
    // Fixed-interval rescoring (rescore_every) provides a floor. Between
    // scheduled rescores, an EWMA of prediction error detects heap drift:
    // when the heap consistently overpredicts tube heights (because the
    // landscape changed since scoring), an early rescore is triggered.
    // The EWMA prevents pathological re-scoring on adversarial geometry —
    // at alpha=0.1, it takes ~10 consecutive bad predictions to trigger.

    int total_assigned = 0, total_skipped = 0, rescores = 0;
    int since_rescore = rescore_every; // trigger initial build
    double error_ewma = 0.0;
    const double ewma_alpha = 0.1;
    const double ewma_rescore_threshold = 0.4;  // rescore when avg error > 40%

    MinHeap heap;

    for (;;) {
        // Rebuild heap when stale (scheduled or drift-triggered)
        if (since_rescore >= rescore_every || error_ewma > ewma_rescore_threshold) {
            heap = build_heap();
            ++rescores;
            since_rescore = 0;
            error_ewma = 0.0;
            if (heap.empty()) break;
        }

        if (heap.empty()) break;

        CellLayerScore entry = heap.top();
        heap.pop();

        // Skip if consumed since last scoring
        if (consumed[entry.cell].overlaps(entry.layer_bottom_um, entry.layer_top_um)) {
            ++total_skipped;
            continue;
        }

        // Find the best neighbor: longest tube from this layer, with most-
        // constrained (fewest free layers) as tiebreaker for equal heights.
        auto ce_it = cell_edges.find(entry.cell);
        if (ce_it == cell_edges.end()) continue;

        size_t best_edge_idx = SIZE_MAX;
        const Run *best_run  = nullptr;
        int64_t best_tube_h  = 0;
        int best_neighbor_free = INT_MAX;
        TriangleCell best_neighbor;

        for (size_t ei : ce_it->second) {
            const EdgeData &ed = edges[ei];
            const TriangleCell &neighbor =
                (ed.edge.a == entry.cell) ? ed.edge.b : ed.edge.a;

            auto pres_it = cells.find(neighbor);
            if (pres_it == cells.end() || !pres_it->second.present(entry.layer))
                continue;
            if (consumed[neighbor].overlaps(entry.layer_bottom_um, entry.layer_top_um))
                continue;

            const Run *run = find_run_containing(ed, entry.layer,
                                                  entry.layer_bottom_um, entry.layer_top_um);
            if (!run) continue;

            // Pick the neighbor yielding the longest tube from this layer.
            // Among equal-height candidates, prefer the most constrained
            // neighbor (fewest free layers) to reduce stranding risk.
            auto [ts, te] = expand_tube(entry.layer, *run,
                consumed[entry.cell], consumed[neighbor], um, max_h_um);
            int64_t tube_h = te - ts;
            int free_layers = count_unconsumed_layers(*run,
                consumed[entry.cell], consumed[neighbor], um);

            if (tube_h > best_tube_h ||
                (tube_h == best_tube_h && free_layers < best_neighbor_free)) {
                best_tube_h        = tube_h;
                best_neighbor_free = free_layers;
                best_edge_idx      = ei;
                best_run           = run;
                best_neighbor      = neighbor;
            }
        }

        if (best_edge_idx == SIZE_MAX) continue;

        // Expand the longest valid tube containing this layer
        auto [tube_start, tube_end] = expand_tube(
            entry.layer, *best_run,
            consumed[entry.cell], consumed[best_neighbor], um, max_h_um);

        if (tube_end - tube_start < min_h_um) continue;

        int64_t tube_h = tube_end - tube_start;
        BOOST_LOG_TRIVIAL(debug) << "MagmaGreedy: assign #" << total_assigned
            << " cell(" << entry.cell.a << "," << entry.cell.b << "," << (entry.cell.is_up()?"U":"D") << ")"
            << " + nbr(" << best_neighbor.a << "," << best_neighbor.b << "," << (best_neighbor.is_up()?"U":"D") << ")"
            << " edge=" << best_edge_idx
            << " h=" << tube_h/1000.0 << "mm"
            << " [" << tube_start/1000.0 << "-" << tube_end/1000.0 << "]"
            << " run=[" << best_run->start_layer << "-" << best_run->end_layer << "]"
            << " viable=" << entry.num_viable
            << " best_h=" << entry.best_tube_h_um/1000.0
            << " nbr_free=" << best_neighbor_free;

        // Track heap prediction drift: how much shorter is the actual tube
        // vs what the heap predicted when this entry was scored?
        if (entry.best_tube_h_um > 0) {
            double rel_err = 1.0 - double(tube_h) / double(entry.best_tube_h_um);
            error_ewma = ewma_alpha * std::max(0.0, rel_err)
                       + (1.0 - ewma_alpha) * error_ewma;
        }

        // Commit tube and mark both cells as consumed. No stagger logic here:
        // greedy always picks the longest possible tube. CP-SAT will later
        // re-optimize with grid-aligned boundary options for stagger.
        committed[best_edge_idx].push_back({tube_start, tube_end});
        consumed[entry.cell].add(tube_start, tube_end);
        consumed[best_neighbor].add(tube_start, tube_end);
        ++total_assigned;
        ++since_rescore;
    }

    // Summary: height distribution of assigned tubes
    int64_t min_assigned_h = INT64_MAX, max_assigned_h = 0, sum_h = 0;
    int total_segs = 0;
    for (const auto &segs : committed) {
        for (const auto &seg : segs) {
            int64_t h = seg.end_um - seg.start_um;
            min_assigned_h = std::min(min_assigned_h, h);
            max_assigned_h = std::max(max_assigned_h, h);
            sum_h += h;
            ++total_segs;
        }
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - t_start).count();
    BOOST_LOG_TRIVIAL(info) << "MagmaGreedy: " << total_assigned << " tubes assigned, "
        << total_skipped << " skipped, " << rescores << " rescores, " << ms << "ms";
    if (total_segs > 0)
        BOOST_LOG_TRIVIAL(info) << "MagmaGreedy heights: min=" << min_assigned_h/1000.0
            << "mm max=" << max_assigned_h/1000.0
            << "mm avg=" << (sum_h/total_segs)/1000.0
            << "mm (min_h=" << min_h_um/1000.0
            << " max_h=" << max_h_um/1000.0 << ")";
}

} // namespace magma
} // namespace Slic3r
