#include "MagmaTubeSolver.hpp"
#include "MagmaGreedyWarmStart.hpp"

#include <boost/log/trivial.hpp>
#include <tbb/task_arena.h> // for max_concurrency()

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>

// OR-Tools CP-SAT solver
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/cp_model_checker.h"

namespace Slic3r {
namespace magma {

// ============================================================================
// Constructor
// ============================================================================

MagmaTubeSolver::MagmaTubeSolver(
    const MagmaLattice &lattice,
    const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &cells,
    const std::vector<LayerData> &layer_data,
    int    first_layer,
    double min_tube_height_mm,
    double max_tube_height_mm,
    int    num_layers,
    double dodge_distance_mm,
    MagmaTubeSolverMode mode,
    double solver_timeout_sec)
    : m_lattice(lattice)
    , m_cells(cells)
    , m_layer_data(layer_data)
    , m_first_layer(std::max(0, first_layer))
    , m_min_h_mm(min_tube_height_mm)
    , m_max_h_mm(max_tube_height_mm)
    , m_num_layers(num_layers)
    , m_z_window(0)
    , m_dodge_mm(dodge_distance_mm)
    , m_mode(mode)
    , m_timeout_sec(solver_timeout_sec)
{}

// ============================================================================
// solve — 3-pass driver
// ============================================================================

void MagmaTubeSolver::solve(
    std::vector<UTubePair> &out_pairs,
    std::unordered_map<TriangleCell, std::vector<int>, TriangleCellHash> &out_cell_pair_index,
    ProgressFn progress_fn,
    ThrowIfCanceled throw_if_canceled)
{
    auto t_start = std::chrono::high_resolution_clock::now();

    build_micron_tables();
    build_edges();
    if (throw_if_canceled) throw_if_canceled();

    // Greedy warm start: fast heuristic that populates m_committed with a good
    // initial solution. CP-SAT then refines it via warm start hints.
    {
        const int64_t min_h_um = llround(m_min_h_mm * 1000.0);
        const int64_t max_h_um = llround(m_max_h_mm * 1000.0);
        greedy_warm_start(m_lattice, m_cells, m_edges, m_cell_edges, m_um,
                          min_h_um, max_h_um, m_committed,
                          &m_cell_difficulty);
        validate_committed(m_edges, m_edge_index, m_committed, m_cells,
                           m_um, m_layer_data, m_min_h_mm, m_max_h_mm,
                           m_num_layers, "GREEDY");
    }
    if (throw_if_canceled) throw_if_canceled();

    // Z window and stride, derived from max tube height:
    //   window  = 4 × max_h_layers  — room for 3-4 stacked tubes
    //   overlap = 2 × max_h_layers  — every tube fully in ≥2 Z levels
    //   stride  = 2 × max_h_layers  — window minus overlap
    // Uses smallest layer height for conservative layer count.
    int z_stride;
    {
        // Seed from the first REAL layer. Seeding from index 0 meant seeding with a raft
        // placeholder's 0.0, which the `> 0` guard can never displace -- so min_lh stayed 0,
        // m_max_h_mm / 0.0 was +inf, and the cast to int was undefined behaviour that
        // collapsed max_h_layers to 1 and disabled CP-SAT on every raft print.
        double min_lh = 0.0;
        for (int L = m_first_layer; L < int(m_layer_data.size()); ++L)
            if (m_layer_data[L].height > 0 && (min_lh <= 0.0 || m_layer_data[L].height < min_lh))
                min_lh = m_layer_data[L].height;
        // Every layer from m_first_layer up is a real Layer with a real height, so this can
        // only fire if the map was built with no layers at all -- in which case there is
        // nothing to solve. Bail rather than invent a height and produce a plausible answer
        // from data we do not have.
        if (min_lh <= 0.0) {
            BOOST_LOG_TRIVIAL(error) << "Magma solver: no layer with a positive height; "
                                        "skipping tube assignment.";
            return;
        }
        int max_h_layers = std::max(1, static_cast<int>(std::ceil(m_max_h_mm / min_lh)));
        m_z_window = 4 * max_h_layers;
        z_stride   = std::max(1, 2 * max_h_layers);
    }

    // Z extent of actual edge data (not all layers may have cells)
    int max_edge_layer = 0;
    for (const auto &[cell, presence] : m_cells)
        max_edge_layer = std::max(max_edge_layer, presence.last_layer);

    int num_z_levels = 0;
    for (int z = 0; z <= max_edge_layer; z += z_stride)
        ++num_z_levels;

    // Give CP-SAT all available cores (blocks run sequentially).
    m_cpsat_workers = tbb::this_task_arena::max_concurrency();

    // Pre-count blocks to compute per-block timeout from total budget.
    // Single XY pass per Z level with R_OVERLAP-cell overlap between blocks.
    int total_blocks = 0;
    for (int z_off = 0; z_off <= max_edge_layer; z_off += z_stride) {
        std::vector<Block> temp;
        build_blocks(0, 0, z_off, temp);
        total_blocks += static_cast<int>(temp.size());
    }
    m_per_block_timeout = m_timeout_sec / std::max(1, total_blocks);

    BOOST_LOG_TRIVIAL(info) << "MagmaTubeSolver: " << m_edges.size() << " edges"
        << ", Z_window=" << m_z_window << " layers"
        << ", Z_stride=" << z_stride
        << ", R=" << R
        << ", " << num_z_levels << " Z levels"
        << ", " << total_blocks << " total blocks"
        << ", per_block_timeout=" << std::fixed << std::setprecision(1)
        << m_per_block_timeout << "s"
        << ", cpsat_workers=" << m_cpsat_workers
        << ", max_layer=" << max_edge_layer;

    // CP-SAT refinement (Refined mode only)
    // Single XY pass per Z level with overlapping blocks (R_OVERLAP cells).
    // Blocks solved sequentially — CP-SAT uses all cores per block.
    if (m_mode == MagmaTubeSolverMode::Refined) {
        int blocks_completed = 0;
        if (progress_fn) progress_fn(0, total_blocks);
        for (int z_off = 0; z_off <= max_edge_layer; z_off += z_stride) {
            solve_pass(0, 0, z_off,
                [&](int) {
                    ++blocks_completed;
                    if (progress_fn) progress_fn(blocks_completed, total_blocks);
                    if (throw_if_canceled) throw_if_canceled();
                });
        }

        validate_committed(m_edges, m_edge_index, m_committed, m_cells,
                           m_um, m_layer_data, m_min_h_mm, m_max_h_mm,
                           m_num_layers, "CPSAT");
    }

    extract_results(out_pairs, out_cell_pair_index);

    auto t_end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    BOOST_LOG_TRIVIAL(info) << "MagmaTubeSolver: "
        << (m_mode == MagmaTubeSolverMode::Refined ? "refined" : "basic")
        << " in " << ms << "ms, " << out_pairs.size() << " pairs placed";
}

// ============================================================================
// build_micron_tables
// ============================================================================

void MagmaTubeSolver::build_micron_tables()
{
    int n = m_num_layers;
    m_um.top_um.resize(n);
    m_um.bottom_um.resize(n);

    // Only rows from m_first_layer up are backed by a real layer. Filling the rest would
    // give several layers an identical Z of 0, collapsing the reverse maps below to
    // last-writer-wins.
    for (int L = m_first_layer; L < n; ++L)
        m_um.top_um[L] = llround(m_layer_data[L].print_z * 1000.0);

    // The first real layer's bottom comes from its own bottom_z -- the raft top, when there
    // is a raft. Everything above is contiguous by construction.
    if (m_first_layer < n)
        m_um.bottom_um[m_first_layer] = llround(m_layer_data[m_first_layer].bottom_z() * 1000.0);
    for (int L = m_first_layer + 1; L < n; ++L)
        m_um.bottom_um[L] = m_um.top_um[L - 1];

    // Reverse maps (same integers — guaranteed to match)
    for (int L = m_first_layer; L < n; ++L) {
        m_um.bottom_to_layer[m_um.bottom_um[L]] = L;
        m_um.top_to_layer[m_um.top_um[L]] = L;
    }
}

// ============================================================================
// build_edges
// ============================================================================

void MagmaTubeSolver::build_edges()
{
    const int64_t min_h_um = llround(m_min_h_mm * 1000.0);

    std::unordered_set<CellEdge, CellEdgeHash> seen;

    for (const auto &[cell, presence] : m_cells) {
        for (const TriangleCell &nbr : m_lattice.neighbors(cell)) {
            if (m_cells.find(nbr) == m_cells.end())
                continue;
            CellEdge edge(cell, nbr);
            if (!seen.insert(edge).second)
                continue;

            EdgeData ed;
            ed.edge = edge;

            const CellPresence &pa = m_cells.at(edge.a);
            const CellPresence &pb = m_cells.at(edge.b);
            int range_start = std::max(pa.first_layer, pb.first_layer);
            int range_end   = std::min(pa.last_layer, pb.last_layer);

            // Walk shared presence, split into contiguous runs
            Run current{-1, -1, 0, 0};
            auto flush_run = [&]() {
                if (current.start_layer < 0)
                    return;
                current.start_um = m_um.bottom_um[current.start_layer];
                current.end_um   = m_um.top_um[current.end_layer];
                if (current.end_um - current.start_um >= min_h_um)
                    ed.runs.push_back(current);
                current = {-1, -1, 0, 0};
            };

            for (int i = range_start; i <= range_end; ++i) {
                if (pa.present(i) && pb.present(i)) {
                    if (current.start_layer < 0)
                        current.start_layer = i;
                    current.end_layer = i;
                } else {
                    flush_run();
                }
            }
            flush_run();

            if (!ed.runs.empty()) {
                size_t idx = m_edges.size();
                m_edge_index[edge] = idx;
                m_edges.push_back(std::move(ed));
            }
        }
    }

    // Initialize committed segments (empty for all edges)
    m_committed.resize(m_edges.size());

    // Build reverse lookup: cell → edge indices
    for (size_t ei = 0; ei < m_edges.size(); ++ei) {
        m_cell_edges[m_edges[ei].edge.a].push_back(ei);
        m_cell_edges[m_edges[ei].edge.b].push_back(ei);
    }

    BOOST_LOG_TRIVIAL(debug) << "MagmaTubeSolver: built " << m_edges.size() << " edges";
}

// ============================================================================
// build_blocks
// ============================================================================

void MagmaTubeSolver::build_blocks(int off_a, int off_b, int off_z,
                                    std::vector<Block> &out) const
{
    out.clear();

    // Assign each cell to an XY block
    struct XYKey {
        int bx, by;
        bool operator==(const XYKey &o) const { return bx == o.bx && by == o.by; }
    };
    struct XYKeyHash {
        size_t operator()(const XYKey &k) const {
            return std::hash<int>()(k.bx) ^ (std::hash<int>()(k.by) << 16);
        }
    };

    // Assign each cell to its primary block, plus adjacent blocks if it falls
    // in the R_OVERLAP overlap zone. Stride = R - R_OVERLAP.
    const int stride = std::max(1, R - R_OVERLAP);

    auto floor_div = [](int a, int s) { return (a >= 0) ? a / s : (a - s + 1) / s; };

    std::unordered_map<XYKey, std::vector<TriangleCell>, XYKeyHash> xy_groups;
    for (const auto &[cell, _] : m_cells) {
        int sa = cell.a + off_a;
        int sb = cell.b + off_b;
        int bx = floor_div(sa, stride);
        int by = floor_div(sb, stride);
        // Primary block
        xy_groups[{bx, by}].push_back(cell);
        // Overlap: if within R_OVERLAP of the next block boundary, also add to neighbor blocks
        bool overlap_a = (sa - bx * stride) < R_OVERLAP && bx > floor_div(sa - 1, stride);
        bool overlap_b = (sb - by * stride) < R_OVERLAP && by > floor_div(sb - 1, stride);
        if (overlap_a)              xy_groups[{bx - 1, by}].push_back(cell);
        if (overlap_b)              xy_groups[{bx, by - 1}].push_back(cell);
        if (overlap_a && overlap_b) xy_groups[{bx - 1, by - 1}].push_back(cell);
    }

    // Single Z slice: off_z to off_z + z_window - 1
    // (Z levels are iterated by the caller in solve())
    int z_start = std::max(m_first_layer, off_z);   // never descend into placeholder rows
    int z_end   = std::min(off_z + m_z_window - 1, m_num_layers - 1);
    if (z_start >= m_num_layers || z_start > z_end) return;

    int64_t z_start_um = m_um.bottom_um[z_start];
    int64_t z_end_um   = m_um.top_um[z_end];

    // Build blocks: one per XY group at this Z slice
    for (const auto &[xy_key, cells] : xy_groups) {
        std::unordered_set<TriangleCell, TriangleCellHash> cell_set(cells.begin(), cells.end());

        Block block;
        block.cells = cell_set;
        block.z_start_layer = z_start;
        block.z_end_layer   = z_end;
        block.z_start_um    = z_start_um;
        block.z_end_um      = z_end_um;

        // Collect edges via cell reverse lookup (avoids scanning all edges)
        std::unordered_set<size_t> seen_edges;
        for (const TriangleCell &cell : cells) {
            auto it = m_cell_edges.find(cell);
            if (it == m_cell_edges.end()) continue;
            for (size_t ei : it->second) {
                if (!seen_edges.insert(ei).second) continue;
                const EdgeData &ed = m_edges[ei];
                if (cell_set.count(ed.edge.a) == 0 || cell_set.count(ed.edge.b) == 0)
                    continue;

                bool overlaps_z = false;
                for (const Run &r : ed.runs) {
                    if (r.end_um > z_start_um && r.start_um < z_end_um) {
                        overlaps_z = true;
                        break;
                    }
                }
                if (overlaps_z)
                    block.edge_indices.push_back(ei);
            }
        }

        if (!block.edge_indices.empty())
            out.push_back(std::move(block));
    }

    BOOST_LOG_TRIVIAL(debug) << "MagmaTubeSolver: pass offset(" << off_a << ","
        << off_b << "," << off_z << ") -> " << out.size() << " blocks";
}

// ============================================================================
// solve_block — build and solve CP-SAT model for one block
// ============================================================================

BlockResult MagmaTubeSolver::solve_block(const Block &block) const
{
    using namespace operations_research::sat;

    const int64_t min_h_um = llround(m_min_h_mm * 1000.0);
    const int64_t max_h_um = llround(m_max_h_mm * 1000.0);
    // Dodge zone from config (0 = stagger disabled)
    const int64_t dodge_um = llround(m_dodge_mm * 1000.0);

    CpModelBuilder model;

    // Track intervals per cell for NoOverlap
    std::unordered_map<TriangleCell, std::vector<IntervalVar>, TriangleCellHash> cell_intervals;

    // Track segment variables for objective and result extraction
    struct SegVars {
        BoolVar     active;
        IntVar      start;
        IntVar      end;
        IntVar      size;
        IntervalVar interval;
        size_t      edge_idx;
        int64_t     run_start_um;  // which run this slot belongs to
        int64_t     run_end_um;
        operations_research::Domain contrib_dom; // {0} ∪ feasible sizes
    };
    std::vector<SegVars> all_segments;

    // ------------------------------------------------------------------
    // 1. Create decision variables for each edge's runs
    //
    // All micron-space variables use discrete domains derived from a
    // unified layer boundary list. Since bottom_um[L+1] == top_um[L],
    // each boundary is both the end of one layer and the start of the
    // next. One sorted list drives all domains:
    //   position (start/end) ∈ boundaries
    //   size    ∈ {b[j] - b[i] | j > i} ∩ [min_h, max_h]
    //   contrib ∈ {0} ∪ sizes
    // ------------------------------------------------------------------
    for (size_t ei : block.edge_indices) {
        const EdgeData &ed = m_edges[ei];

        for (const Run &run : ed.runs) {
            // Skip runs that don't overlap block Z range
            if (run.end_um <= block.z_start_um || run.start_um >= block.z_end_um)
                continue;

            // Restrict to the intersection of run and block Z range.
            int eff_start = std::max(run.start_layer, block.z_start_layer);
            int eff_end   = std::min(run.end_layer, block.z_end_layer);
            if (eff_start > eff_end)
                continue;

            int64_t eff_h = m_um.top_um[eff_end] - m_um.bottom_um[eff_start];
            if (eff_h < min_h_um)
                continue; // Clipped range too short for a tube

            // K = greedy tube count in this run × 1.3 + 1.
            // Gives 30% headroom for stagger splits beyond greedy's placement.
            // No cap — bounded by geometry (run height / min tube height).
            int64_t run_start_um = m_um.bottom_um[eff_start];
            int64_t run_end_um   = m_um.top_um[eff_end];
            int K_from_greedy = 0;
            for (const CommittedSegment &seg : m_committed[ei])
                if (seg.start_um >= run_start_um && seg.end_um <= run_end_um)
                    ++K_from_greedy;
            int K = std::max(2, K_from_greedy * 13 / 10 + 1);

            // Unified layer boundary list for this run
            std::vector<int64_t> boundaries;
            boundaries.push_back(m_um.bottom_um[eff_start]);
            for (int L = eff_start; L <= eff_end; ++L)
                boundaries.push_back(m_um.top_um[L]);
            // boundaries is sorted: each value is the end of one layer
            // and the start of the next

            // Feasible sizes: all boundary-pair differences in height range
            std::set<int64_t> size_set;
            for (size_t i = 0; i < boundaries.size(); ++i)
                for (size_t j = i + 1; j < boundaries.size(); ++j) {
                    int64_t s = boundaries[j] - boundaries[i];
                    if (s > max_h_um) break; // sorted, no point continuing
                    if (s >= min_h_um)
                        size_set.insert(s);
                }

            if (size_set.empty())
                continue; // No valid tube height for this run

            std::vector<int64_t> size_vals(size_set.begin(), size_set.end());
            std::vector<int64_t> contrib_vals = {0};
            contrib_vals.insert(contrib_vals.end(), size_vals.begin(), size_vals.end());

            auto boundary_dom = operations_research::Domain::FromValues(boundaries);
            auto size_dom     = operations_research::Domain::FromValues(size_vals);
            auto contrib_dom  = operations_research::Domain::FromValues(contrib_vals);

            BoolVar prev_active;
            bool has_prev = false;

            for (int k = 0; k < K; ++k) {
                BoolVar active = model.NewBoolVar();
                IntVar  start  = model.NewIntVar(boundary_dom);
                IntVar  end    = model.NewIntVar(boundary_dom);
                IntVar  size   = model.NewIntVar(size_dom);

                // size = end - start
                model.AddEquality(size, end - start);

                // Optional interval (half-open: [start, start+size) = [start, end))
                IntervalVar interval = model.NewOptionalIntervalVar(
                    start, size, end, active);

                // Add to BOTH cells' NoOverlap
                cell_intervals[ed.edge.a].push_back(interval);
                cell_intervals[ed.edge.b].push_back(interval);

                // Symmetry breaking within run
                if (has_prev) {
                    // Segment k can only be active if k-1 is active
                    model.AddImplication(active, prev_active);
                    // When both active, k must come after k-1 (gap of at least 1 um)
                    model.AddLessThan(all_segments.back().end, start)
                        .OnlyEnforceIf({prev_active, active});
                }

                int64_t rs = boundaries.front();
                int64_t re = boundaries.back();
                all_segments.push_back({active, start, end, size, interval,
                                        ei, rs, re, contrib_dom});
                prev_active = active;
                has_prev = true;
            }
        }
    }

    // ------------------------------------------------------------------
    // 2. Add frozen intervals from committed tubes outside the block
    // ------------------------------------------------------------------

    // Frozen boundaries for stagger penalty
    std::unordered_map<TriangleCell, std::vector<int64_t>, TriangleCellHash> frozen_boundaries;

    // Scan boundary edges via cell reverse lookup (avoids scanning all edges)
    std::unordered_set<size_t> seen_boundary_edges;
    for (const TriangleCell &cell : block.cells) {
        auto it = m_cell_edges.find(cell);
        if (it == m_cell_edges.end()) continue;
        for (size_t ei : it->second) {
            if (!seen_boundary_edges.insert(ei).second) continue;
            const EdgeData &ed = m_edges[ei];
            bool a_in = block.cells.count(ed.edge.a) > 0;
            bool b_in = block.cells.count(ed.edge.b) > 0;

            // Skip edges fully inside (decision edges) or fully outside (irrelevant)
            if (a_in == b_in)
                continue;

            const TriangleCell &inside_cell = a_in ? ed.edge.a : ed.edge.b;

            for (const CommittedSegment &seg : m_committed[ei]) {
                // Clip to block Z range
                int64_t clip_start = std::max(seg.start_um, block.z_start_um);
                int64_t clip_end   = std::min(seg.end_um, block.z_end_um);
                if (clip_start >= clip_end)
                    continue;

                IntervalVar frozen = model.NewFixedSizeIntervalVar(
                    clip_start, clip_end - clip_start);
                cell_intervals[inside_cell].push_back(frozen);

                // Record boundaries for stagger (use actual, not clipped)
                frozen_boundaries[inside_cell].push_back(seg.start_um);
                frozen_boundaries[inside_cell].push_back(seg.end_um);
            }
        }
    }

    // Decision-edge segments that extend outside the block's Z range are frozen
    // (the block can't fully see them). Segments fully inside Z range become
    // warm start hints — the solver re-optimizes them.
    for (size_t ei : block.edge_indices) {
        for (const CommittedSegment &seg : m_committed[ei]) {
            if (seg.start_um >= block.z_start_um && seg.end_um <= block.z_end_um)
                continue; // Fully inside — warm start, not frozen

            int64_t clip_start = std::max(seg.start_um, block.z_start_um);
            int64_t clip_end   = std::min(seg.end_um, block.z_end_um);
            if (clip_start >= clip_end)
                continue;

            IntervalVar frozen = model.NewFixedSizeIntervalVar(
                clip_start, clip_end - clip_start);
            const EdgeData &ed = m_edges[ei];
            cell_intervals[ed.edge.a].push_back(frozen);
            cell_intervals[ed.edge.b].push_back(frozen);

            frozen_boundaries[ed.edge.a].push_back(seg.start_um);
            frozen_boundaries[ed.edge.a].push_back(seg.end_um);
            frozen_boundaries[ed.edge.b].push_back(seg.start_um);
            frozen_boundaries[ed.edge.b].push_back(seg.end_um);
        }
    }

    // Early out: no decision variables means nothing to solve
    if (all_segments.empty())
        return BlockResult{};

    // ------------------------------------------------------------------
    // 3. NoOverlap per cell
    // ------------------------------------------------------------------
    for (auto &[cell, intervals] : cell_intervals) {
        if (intervals.size() > 1)
            model.AddNoOverlap(intervals);
    }

    // ------------------------------------------------------------------
    // 4. Objective: three-tier lexicographic via weighted sum
    //
    //   Tier 1 — Coverage (fill): maximize total tube µm
    //   Tier 2 — Tube length:     prefer fewer, longer tubes
    //   Tier 3 — Stagger:         spread tube boundaries apart
    //
    // Each tier's minimum contribution exceeds the next tier's maximum
    // total, so higher tiers always dominate.  This prevents the solver
    // from splitting a long tube into short ones for stagger benefit.
    //
    // Overflow budget:  max objective ≈ 5×10¹³, int64_max ≈ 9.2×10¹⁸
    //                   (184,000× headroom)
    // ------------------------------------------------------------------

    // Tier 1: coverage — 1µm of fill outweighs all activation + stagger.
    constexpr int64_t W_COVERAGE = 1000000;

    // Tier 2: activation cost — fixed penalty per active tube segment.
    // A linear length bonus (W*size) can't prevent splitting because
    // W*S == W*(S/2) + W*(S/2).  The activation cost makes 1 long tube
    // strictly cheaper than 2 short tubes with the same total coverage.
    // Must exceed max stagger benefit of one split (~36).
    constexpr int64_t W_ACTIVATION = 100;

    // Tier 3: stagger — tiebreaker among equal-coverage, equal-count solutions.
    // (W_STAGGER_TIGHT and W_STAGGER_WIDE defined below, values 2 and 1)

    LinearExpr objective;

    for (const auto &seg : all_segments) {
        // Coverage: W_COVERAGE * size when active, 0 when not
        IntVar contrib = model.NewIntVar(seg.contrib_dom);
        model.AddEquality(contrib, seg.size).OnlyEnforceIf(seg.active);
        model.AddEquality(contrib, 0).OnlyEnforceIf(seg.active.Not());
        objective += W_COVERAGE * contrib;

        // Activation cost: penalize each active segment to prefer fewer tubes
        objective -= W_ACTIVATION * seg.active;
    }

    // Stagger: per-cell cumulative penalty (skipped when dodge_um == 0).
    //
    // For each cell C, create two cumulative constraints (tight + wide scale)
    // covering boundaries from C's Ring-0 edges (demand=2) and Ring-1
    // neighbor edges (demand=1). Each boundary creates an exclusion zone
    // interval centered on its position. The cumulative capacity variable
    // measures peak boundary concentration — the solver minimizes it.
    //
    // Two scales provide graduated penalty: very close boundaries trigger
    // both tight and wide penalties, moderately close only trigger wide.

    constexpr int64_t W_STAGGER_TIGHT = 2;
    constexpr int64_t W_STAGGER_WIDE  = 1;

    if (dodge_um > 0) {

    const int64_t wide_zone_um  = dodge_um;
    const int64_t tight_zone_um = std::max<int64_t>(1, dodge_um / 2);

    std::unordered_set<size_t> block_edge_set(block.edge_indices.begin(),
                                               block.edge_indices.end());

    // Build edge → segment indices index
    std::unordered_map<size_t, std::vector<size_t>> edge_to_segs;
    for (size_t si = 0; si < all_segments.size(); ++si)
        edge_to_segs[all_segments[si].edge_idx].push_back(si);

    // Pre-create zone intervals for each segment boundary (reused across
    // multiple cells' cumulatives — same IntervalVar, different demands)
    struct BoundaryZones {
        IntervalVar tight_start, tight_end;
        IntervalVar wide_start, wide_end;
    };
    std::vector<BoundaryZones> seg_zones(all_segments.size());
    for (size_t si = 0; si < all_segments.size(); ++si) {
        const auto &seg = all_segments[si];
        seg_zones[si].tight_start = model.NewOptionalFixedSizeIntervalVar(
            seg.start - tight_zone_um / 2, tight_zone_um, seg.active);
        seg_zones[si].tight_end = model.NewOptionalFixedSizeIntervalVar(
            seg.end - tight_zone_um / 2, tight_zone_um, seg.active);
        seg_zones[si].wide_start = model.NewOptionalFixedSizeIntervalVar(
            seg.start - wide_zone_um / 2, wide_zone_um, seg.active);
        seg_zones[si].wide_end = model.NewOptionalFixedSizeIntervalVar(
            seg.end - wide_zone_um / 2, wide_zone_um, seg.active);
    }

    // Pre-create zone intervals for frozen boundaries (keyed by position
    // to avoid duplicates, since the same frozen boundary may appear in
    // multiple cells' maps)
    struct FrozenZones {
        IntervalVar tight, wide;
    };
    std::unordered_map<int64_t, FrozenZones> frozen_zone_cache;
    auto get_frozen_zones = [&](int64_t pos) -> const FrozenZones & {
        auto it = frozen_zone_cache.find(pos);
        if (it != frozen_zone_cache.end())
            return it->second;
        FrozenZones fz;
        fz.tight = model.NewFixedSizeIntervalVar(
            pos - tight_zone_um / 2, tight_zone_um);
        fz.wide = model.NewFixedSizeIntervalVar(
            pos - wide_zone_um / 2, wide_zone_um);
        return frozen_zone_cache.emplace(pos, fz).first->second;
    };

    // For each cell, build cumulative constraints over its Ring-1 neighborhood
    for (const TriangleCell &cell : block.cells) {
        // Collect segment indices with demand weight:
        //   Ring-0 (cell's own edges): demand = 2
        //   Ring-1 (neighbor edges):   demand = 1
        struct SegDemand { size_t seg_idx; int demand; };
        std::vector<SegDemand> seg_demands;
        std::unordered_set<size_t> seen_edges;

        auto add_edges_for_cell = [&](const TriangleCell &c, int demand) {
            auto it = m_cell_edges.find(c);
            if (it == m_cell_edges.end()) return;
            for (size_t ei : it->second) {
                if (!block_edge_set.count(ei)) continue;
                if (!seen_edges.insert(ei).second) continue;
                auto seg_it = edge_to_segs.find(ei);
                if (seg_it == edge_to_segs.end()) continue;
                for (size_t si : seg_it->second)
                    seg_demands.push_back({si, demand});
            }
        };

        // Ring-0: edges involving this cell
        add_edges_for_cell(cell, 2);
        // Ring-1: edges involving neighbor cells
        for (const TriangleCell &nbr : m_lattice.neighbors(cell))
            add_edges_for_cell(nbr, 1);

        // Collect frozen boundaries: Ring-0 (demand=2) + Ring-1 (demand=1)
        struct FrozenDemand { int64_t pos; int demand; };
        std::vector<FrozenDemand> frozen_demands;
        std::unordered_set<int64_t> seen_frozen;

        auto add_frozen_for_cell = [&](const TriangleCell &c, int demand) {
            auto it = frozen_boundaries.find(c);
            if (it == frozen_boundaries.end()) return;
            for (int64_t fb : it->second)
                if (seen_frozen.insert(fb).second)
                    frozen_demands.push_back({fb, demand});
        };

        add_frozen_for_cell(cell, 2);
        // Ring-1: frozen boundaries from neighbor cells
        for (const TriangleCell &nbr : m_lattice.neighbors(cell))
            add_frozen_for_cell(nbr, 1);

        // Skip cells with too few boundaries to have meaningful stagger
        if (seg_demands.size() + frozen_demands.size() < 2)
            continue;

        // Build cumulative at each scale
        auto build_cumulative = [&](bool tight, int64_t weight) {
            // Capacity is purely soft — any value is feasible, higher = more
            // penalty in the objective. Domain must never cause INFEASIBLE.
            IntVar capacity = model.NewIntVar(
                operations_research::Domain(0, 10000));
            auto cum = model.AddCumulative(capacity);

            for (const auto &sd : seg_demands) {
                const auto &z = seg_zones[sd.seg_idx];
                cum.AddDemand(tight ? z.tight_start : z.wide_start, sd.demand);
                cum.AddDemand(tight ? z.tight_end   : z.wide_end,   sd.demand);
            }
            for (const auto &fd : frozen_demands) {
                const auto &fz = get_frozen_zones(fd.pos);
                cum.AddDemand(tight ? fz.tight : fz.wide, fd.demand);
            }

            objective -= weight * capacity;
        };

        build_cumulative(true,  W_STAGGER_TIGHT);
        build_cumulative(false, W_STAGGER_WIDE);
    }
    } // if (dodge_um > 0)

    model.Maximize(objective);

    // ------------------------------------------------------------------
    // 5. Complete solution hint (warm start)
    // ------------------------------------------------------------------
    {
        std::vector<bool> hinted(all_segments.size(), false);

        // Collect and sort committed segments per edge (within Z range)
        std::unordered_map<size_t, std::vector<const CommittedSegment *>> edge_hints;
        for (size_t ei : block.edge_indices) {
            for (const CommittedSegment &seg : m_committed[ei]) {
                if (seg.start_um >= block.z_start_um && seg.end_um <= block.z_end_um)
                    edge_hints[ei].push_back(&seg);
            }
        }
        for (auto &[ei, hints] : edge_hints)
            std::sort(hints.begin(), hints.end(),
                      [](const CommittedSegment *a, const CommittedSegment *b) {
                          return a->start_um < b->start_um;
                      });

        // Match committed segments to the correct run's slot
        for (auto &[ei, hints] : edge_hints) {
            if (hints.empty()) continue;
            size_t ci = 0;
            for (size_t si = 0; si < all_segments.size(); ++si) {
                if (ci >= hints.size()) break;
                const auto &sv = all_segments[si];
                if (sv.edge_idx != ei) continue;
                const CommittedSegment *cs = hints[ci];
                if (cs->start_um >= sv.run_start_um && cs->end_um <= sv.run_end_um) {
                    model.AddHint(sv.active, true);
                    model.AddHint(sv.start, cs->start_um);
                    model.AddHint(sv.end, cs->end_um);
                    hinted[si] = true;
                    ++ci;
                }
            }
        }

        // Hint remaining segments as inactive (completes the initial solution)
        for (size_t si = 0; si < all_segments.size(); ++si) {
            if (!hinted[si])
                model.AddHint(all_segments[si].active, false);
        }
    }

    // ------------------------------------------------------------------
    // 6. Solve
    // ------------------------------------------------------------------
    SatParameters params;
    params.set_max_time_in_seconds(m_per_block_timeout);
    params.set_num_workers(m_cpsat_workers);
    // NOTE: linearization_level and relative_gap_limit removed — defaults
    // perform better with our discrete domains. gap_limit was causing
    // medium blocks to stop early, losing 1-2 tubes per block.
    // NOTE: repair_hint and hint_conflict_limit removed — triggered
    // CP-SAT fixed_search crash (integer_search.cc:1217)

    // Validate model before solving (catches structural bugs)
    auto proto = model.Build();
    std::string validation_error = ValidateCpModel(proto);
    if (!validation_error.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MagmaTubeSolver: MODEL INVALID: "
            << validation_error.substr(0, 200);
    }

    operations_research::sat::Model sat_model;
    sat_model.Add(NewSatParameters(params));

    // Cancellation: checked between passes via check_canceled().
    // Mid-solve abort not wired — TimeLimit::RegisterExternalBooleanAsLimit
    // conflicts with NewSatParameters (creates TimeLimit without params).

    CpSolverResponse response = SolveCpModel(proto, &sat_model);

    // ------------------------------------------------------------------
    // 7. Extract results
    // ------------------------------------------------------------------
    BlockResult result;
    if (response.status() == CpSolverStatus::OPTIMAL ||
        response.status() == CpSolverStatus::FEASIBLE) {
        result.solved = true;
        for (const auto &seg : all_segments) {
            if (SolutionBooleanValue(response, seg.active)) {
                int64_t s = SolutionIntegerValue(response, seg.start);
                int64_t e = SolutionIntegerValue(response, seg.end);
                result.segments.push_back({seg.edge_idx, {s, e}});
            }
        }
        // Per-block coverage comparison
        int64_t solver_cov = 0;
        for (const auto &[eidx, seg] : result.segments)
            solver_cov += seg.end_um - seg.start_um;
        int64_t greedy_cov = 0;
        for (size_t ei : block.edge_indices)
            for (const CommittedSegment &cs : m_committed[ei])
                if (cs.start_um >= block.z_start_um && cs.end_um <= block.z_end_um)
                    greedy_cov += cs.end_um - cs.start_um;

        BOOST_LOG_TRIVIAL(debug) << "MagmaTubeSolver: block solved "
            << (response.status() == CpSolverStatus::OPTIMAL ? "OPTIMAL" : "FEASIBLE")
            << ", obj=" << response.objective_value()
            << ", gap=" << (response.best_objective_bound() > 0
                ? (1.0 - response.objective_value() / response.best_objective_bound()) * 100.0
                : 0.0) << "%"
            << ", segs=" << result.segments.size()
            << "/" << all_segments.size()
            << ", wall=" << std::fixed << std::setprecision(2)
            << response.wall_time() << "s"
            << " | cov=" << solver_cov/1000.0
            << "mm greedy=" << greedy_cov/1000.0 << "mm";
        // Check if secondary constraints (activation + dodge) consumed
        // more than 1µm of coverage equivalent in objective value.
        int64_t coverage_obj = W_COVERAGE * solver_cov;
        int64_t total_obj = llround(response.objective_value());
        int64_t secondary_penalty = coverage_obj - total_obj;
        if (secondary_penalty > W_COVERAGE / 2)
            BOOST_LOG_TRIVIAL(warning) << "  PENALTY EXCEEDS 0.5µm: secondary="
                << secondary_penalty << " (coverage_obj=" << coverage_obj
                << " total_obj=" << total_obj << ")";

        if (solver_cov < greedy_cov)
            BOOST_LOG_TRIVIAL(warning) << "  COVERAGE DROP: lost "
                << (greedy_cov - solver_cov)/1000.0 << "mm";
    } else {
        // Count frozen intervals per cell for diagnostics
        int total_frozen = 0;
        int max_frozen_per_cell = 0;
        for (const auto &[cell, intervals] : cell_intervals) {
            // Count non-decision intervals (frozen = total - decision)
            int n = static_cast<int>(intervals.size());
            if (n > max_frozen_per_cell) max_frozen_per_cell = n;
            total_frozen += n;
        }
        BOOST_LOG_TRIVIAL(warning) << "MagmaTubeSolver: block solve status="
            << static_cast<int>(response.status())
            << " (" << (response.status() == CpSolverStatus::INFEASIBLE ? "INFEASIBLE" :
                        response.status() == CpSolverStatus::MODEL_INVALID ? "MODEL_INVALID" :
                        response.status() == CpSolverStatus::UNKNOWN ? "UNKNOWN" : "OTHER")
            << "), edges=" << block.edge_indices.size()
            << ", segments=" << all_segments.size()
            << ", cells=" << block.cells.size()
            << ", cell_interval_lists=" << cell_intervals.size()
            << ", total_intervals=" << total_frozen
            << ", max_per_cell=" << max_frozen_per_cell;
    }

    return result;
}

// ============================================================================
// solve_pass
// ============================================================================

int MagmaTubeSolver::solve_pass(int off_a, int off_b, int off_z,
                                 std::function<void(int)> block_done_fn)
{
    auto t_start = std::chrono::high_resolution_clock::now();

    std::vector<Block> blocks;
    build_blocks(off_a, off_b, off_z, blocks);

    // Solve blocks sequentially. CP-SAT uses all available cores internally.
    // Overlapping blocks are safe — each block commits immediately so the next
    // block sees the latest state.
    for (size_t i = 0; i < blocks.size(); ++i) {
        BlockResult result = solve_block(blocks[i]);
        commit_results({blocks[i]}, {result});
        if (block_done_fn) block_done_fn(static_cast<int>(i));
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    BOOST_LOG_TRIVIAL(debug) << "MagmaTubeSolver: pass(" << off_a << "," << off_b << ","
        << off_z << "): " << blocks.size() << " blocks, " << ms << "ms";

    return static_cast<int>(blocks.size());
}

// ============================================================================
// commit_results
// ============================================================================

void MagmaTubeSolver::commit_results(const std::vector<Block> &blocks,
                                      const std::vector<BlockResult> &results)
{
    // Only update edges from blocks that solved successfully.
    // If a block failed (INFEASIBLE/UNKNOWN), preserve prior committed segments
    // rather than destroying them — don't make things worse.
    //
    // For successful blocks: only remove segments FULLY INSIDE the block's Z
    // range. Segments extending outside Z belong to other Z levels and must be
    // preserved. This is the key invariant: a block only modifies what it can
    // fully see.
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        if (!results[bi].solved) {
            ++m_unknown_blocks;
            continue;
        }

        const Block &block = blocks[bi];
        for (size_t ei : block.edge_indices) {
            auto &segs = m_committed[ei];
            segs.erase(std::remove_if(segs.begin(), segs.end(),
                [&](const CommittedSegment &s) {
                    return s.start_um >= block.z_start_um &&
                           s.end_um   <= block.z_end_um;
                }), segs.end());
        }
        for (const auto &[edge_idx, seg] : results[bi].segments)
            m_committed[edge_idx].push_back(seg);
    }
}

// ============================================================================
// extract_results — convert micron-space assignments to UTubePair format
// ============================================================================

void MagmaTubeSolver::extract_results(
    std::vector<UTubePair> &out_pairs,
    std::unordered_map<TriangleCell, std::vector<int>, TriangleCellHash> &out_index) const
{
    out_pairs.clear();
    out_index.clear();

    for (size_t ei = 0; ei < m_edges.size(); ++ei) {
        const EdgeData &ed = m_edges[ei];
        for (const CommittedSegment &seg : m_committed[ei]) {
            auto start_it = m_um.bottom_to_layer.find(seg.start_um);
            auto end_it   = m_um.top_to_layer.find(seg.end_um);
            if (start_it == m_um.bottom_to_layer.end() ||
                end_it   == m_um.top_to_layer.end()) {
                BOOST_LOG_TRIVIAL(warning) << "MagmaTubeSolver: invalid micron boundary "
                    << seg.start_um << "-" << seg.end_um;
                continue;
            }

            UTubePair pair;
            pair.cell_a           = ed.edge.a;
            pair.cell_b           = ed.edge.b;
            pair.pair_start_layer = start_it->second;
            pair.pair_end_layer   = end_it->second;
            pair.volume_mm3       = 0.0; // computed later by MagmaTubeMap
            pair.window_end_z     = 0.0; // computed later by MagmaTubeMap

            int pair_idx = static_cast<int>(out_pairs.size());
            out_pairs.push_back(pair);
            out_index[ed.edge.a].push_back(pair_idx);
            out_index[ed.edge.b].push_back(pair_idx);
        }
    }

    // Ensure all cells have an entry (empty = solid fill)
    for (const auto &[cell, _] : m_cells) {
        if (out_index.find(cell) == out_index.end())
            out_index[cell]; // insert empty vector
    }

    BOOST_LOG_TRIVIAL(info) << "MagmaTubeSolver: extracted " << out_pairs.size() << " pairs";

    validate_committed(m_edges, m_edge_index, m_committed, m_cells,
                       m_um, m_layer_data, m_min_h_mm, m_max_h_mm,
                       m_num_layers, "FINAL");
}

// ============================================================================
// validate_committed — reusable validation for committed segments
// ============================================================================

ValidationResult validate_committed(
    const std::vector<EdgeData>                                            &edges,
    const std::unordered_map<CellEdge, size_t, CellEdgeHash>              &edge_index,
    const std::vector<std::vector<CommittedSegment>>                       &committed,
    const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &cells,
    const MicronTables                                                     &um,
    const std::vector<LayerData>                                           &layer_data,
    double min_h_mm, double max_h_mm, int num_layers,
    const char *label)
{
    ValidationResult result;

    // Build per-cell segment list for overlap checking
    std::unordered_map<TriangleCell, std::vector<std::pair<int, int>>,
                       TriangleCellHash> cell_segments; // (start_layer, end_layer)

    int total_segs = 0;
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const EdgeData &ed = edges[ei];
        for (const CommittedSegment &seg : committed[ei]) {
            ++total_segs;
            auto start_it = um.bottom_to_layer.find(seg.start_um);
            auto end_it   = um.top_to_layer.find(seg.end_um);
            if (start_it == um.bottom_to_layer.end() ||
                end_it   == um.top_to_layer.end()) {
                if (++result.bad_range <= 3)
                    BOOST_LOG_TRIVIAL(warning) << label << ": invalid micron boundary "
                        << seg.start_um << "-" << seg.end_um;
                continue;
            }

            int sL = start_it->second;
            int eL = end_it->second;

            // 1. Valid layer range
            if (sL < 0 || eL >= num_layers || sL > eL) {
                if (++result.bad_range <= 3)
                    BOOST_LOG_TRIVIAL(warning) << label << ": tube invalid range L"
                        << sL << "-" << eL;
                continue;
            }

            // 2. Height bounds
            double h_mm = layer_data[eL].print_z
                        - (layer_data[sL].print_z - layer_data[sL].height);
            if (h_mm < min_h_mm - 0.01) {
                if (++result.bad_short <= 3)
                    BOOST_LOG_TRIVIAL(warning) << label << ": tube too short: "
                        << h_mm << "mm (min=" << min_h_mm << ") L" << sL << "-" << eL;
            }
            if (h_mm > max_h_mm + 0.01) {
                if (++result.bad_long <= 3)
                    BOOST_LOG_TRIVIAL(warning) << label << ": tube too long: "
                        << h_mm << "mm (max=" << max_h_mm << ") L" << sL << "-" << eL;
            }

            // 3. Edge exists
            CellEdge ce(ed.edge.a, ed.edge.b);
            if (edge_index.find(ce) == edge_index.end()) {
                if (++result.bad_edge <= 3)
                    BOOST_LOG_TRIVIAL(warning) << label << ": cells are not a valid edge";
            }

            // 4. Both cells present at every layer
            auto it_a = cells.find(ed.edge.a);
            auto it_b = cells.find(ed.edge.b);
            if (it_a != cells.end() && it_b != cells.end()) {
                for (int L = sL; L <= eL; ++L) {
                    if (!it_a->second.present(L) || !it_b->second.present(L)) {
                        if (++result.bad_presence <= 3)
                            BOOST_LOG_TRIVIAL(warning) << label << ": cell not present at L"
                                << L << " (range L" << sL << "-" << eL << ")";
                        break;
                    }
                }
            }

            cell_segments[ed.edge.a].push_back({sL, eL});
            cell_segments[ed.edge.b].push_back({sL, eL});
        }
    }

    // 5. Per-cell overlap
    for (const auto &[cell, segs] : cell_segments) {
        for (size_t i = 0; i < segs.size(); ++i) {
            for (size_t j = i + 1; j < segs.size(); ++j) {
                if (segs[i].first <= segs[j].second &&
                    segs[j].first <= segs[i].second) {
                    ++result.overlap_cell;
                    if (result.overlap_cell <= 3)
                        BOOST_LOG_TRIVIAL(warning) << label << ": cell ("
                            << cell.a << "," << cell.b << "," << cell.c
                            << ") overlap [L" << segs[i].first << "-" << segs[i].second
                            << "] vs [L" << segs[j].first << "-" << segs[j].second << "]";
                }
            }
        }
    }

    // 6. Per-edge overlap
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto &segs = committed[ei];
        for (size_t i = 0; i < segs.size(); ++i) {
            for (size_t j = i + 1; j < segs.size(); ++j) {
                if (segs[i].start_um < segs[j].end_um &&
                    segs[j].start_um < segs[i].end_um) {
                    ++result.overlap_edge;
                    if (result.overlap_edge <= 3)
                        BOOST_LOG_TRIVIAL(warning) << label << ": edge " << ei
                            << " overlap [" << segs[i].start_um << "-" << segs[i].end_um
                            << "] vs [" << segs[j].start_um << "-" << segs[j].end_um << "]";
                }
            }
        }
    }

    // Summary
    if (result.total_issues())
        BOOST_LOG_TRIVIAL(warning) << label << ": " << result.total_issues() << " issues ("
            << result.bad_short << " short, " << result.bad_long << " long, "
            << result.bad_range << " bad-range, " << result.bad_edge << " bad-edge, "
            << result.bad_presence << " not-present, "
            << result.overlap_cell << " cell-overlap, "
            << result.overlap_edge << " edge-overlap)";
    else
        BOOST_LOG_TRIVIAL(info) << label << ": all " << total_segs << " segments OK";

    // Coverage summary
    {
        double total_presence_um = 0, total_covered_um = 0;
        int cells_below_50 = 0, cells_below_25 = 0, cells_zero = 0;
        for (const auto &[cell, presence] : cells) {
            int64_t presence_um = um.top_um[presence.last_layer]
                                - um.bottom_um[presence.first_layer];
            if (presence_um <= 0) continue;

            int64_t covered_um = 0;
            auto it = cell_segments.find(cell);
            if (it != cell_segments.end()) {
                for (const auto &[sL, eL] : it->second) {
                    covered_um += um.top_um[eL] - um.bottom_um[sL];
                }
                // Each segment counted twice (once per cell), but we're iterating
                // per-cell so each cell sees its own segments. However segments
                // appear in cell_segments for BOTH cells of the edge, so each
                // cell correctly sees all its tubes.
            }
            double pct = double(covered_um) / double(presence_um);
            total_presence_um += presence_um;
            total_covered_um  += covered_um;
            if (covered_um == 0) ++cells_zero;
            else if (pct < 0.25) ++cells_below_25;
            else if (pct < 0.50) ++cells_below_50;
        }
        double overall_pct = total_presence_um > 0
            ? total_covered_um / total_presence_um * 100.0 : 0.0;
        BOOST_LOG_TRIVIAL(info) << label << " COVERAGE: " << std::fixed << std::setprecision(1)
            << overall_pct << "% overall, "
            << cells_zero << " cells unfilled, "
            << cells_below_25 << " cells <25%, "
            << cells_below_50 << " cells <50%";
    }

    return result;
}

} // namespace magma
} // namespace Slic3r
