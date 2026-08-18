#ifndef slic3r_Magma_MagmaTubeSolver_hpp_
#define slic3r_Magma_MagmaTubeSolver_hpp_

#include "MagmaTriangleCell.hpp"
#include "MagmaTubeMap.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Slic3r {
namespace magma {

// ============================================================================
// CellEdge — canonical adjacent cell pair (a < b)
// ============================================================================

struct CellEdge {
    TriangleCell a, b;

    CellEdge() = default;
    CellEdge(const TriangleCell &x, const TriangleCell &y)
        : a(std::min(x, y)), b(std::max(x, y)) {}

    bool operator==(const CellEdge &o) const { return a == o.a && b == o.b; }
    bool operator<(const CellEdge &o) const {
        if (a != o.a) return a < o.a;
        return b < o.b;
    }
};

struct CellEdgeHash {
    size_t operator()(const CellEdge &e) const {
        TriangleCellHash h;
        size_t seed = h(e.a);
        seed ^= h(e.b) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

// ============================================================================
// Run — contiguous shared-presence range in microns
// ============================================================================

struct Run {
    int     start_layer, end_layer; // inclusive layer range
    int64_t start_um, end_um;       // bottom_um[start_layer], top_um[end_layer]
};

// ============================================================================
// EdgeData — pre-computed per-edge data
// ============================================================================

struct EdgeData {
    CellEdge         edge;
    std::vector<Run> runs; // contiguous shared-presence ranges (split where either cell's presence breaks)
};

// ============================================================================
// CommittedSegment — a tube assignment in micron space
// ============================================================================

struct CommittedSegment {
    int64_t start_um, end_um;
};

// ============================================================================
// MicronTables — layer boundary Z values in integer microns
// ============================================================================
//
// Built once from LayerData. Contiguous by construction:
//   bottom_um[L+1] == top_um[L]    (no floating-point gaps)

struct MicronTables {
    std::vector<int64_t> top_um;    // top_um[L] = llround(print_z * 1000)
    std::vector<int64_t> bottom_um; // bottom_um[0] from bottom_z; L>0: top_um[L-1]

    std::unordered_map<int64_t, int> bottom_to_layer; // reverse lookup
    std::unordered_map<int64_t, int> top_to_layer;    // reverse lookup
};

// ============================================================================
// Block — 3D region of cells x layers, solved independently
// ============================================================================

struct Block {
    std::vector<size_t>                                    edge_indices; // into m_edges
    std::unordered_set<TriangleCell, TriangleCellHash>     cells;
    int     z_start_layer, z_end_layer;
    int64_t z_start_um, z_end_um;
};

// ============================================================================
// BlockResult — output of solving one block
// ============================================================================

struct BlockResult {
    bool solved = false; // true if solver found FEASIBLE or OPTIMAL
    std::vector<std::pair<size_t, CommittedSegment>> segments; // (edge_idx, segment)
};

// ============================================================================
// ValidationResult — output of validate_committed()
// ============================================================================

struct ValidationResult {
    int bad_short = 0, bad_long = 0, bad_range = 0, bad_presence = 0;
    int bad_edge = 0, overlap_cell = 0, overlap_edge = 0;
    int total_issues() const {
        return bad_range + bad_short + bad_long + bad_edge +
               bad_presence + overlap_cell + overlap_edge;
    }
};

/// Validate committed segments. Checks height bounds, edge validity,
/// presence, per-cell/per-edge overlap. Logs warnings and a coverage summary.
ValidationResult validate_committed(
    const std::vector<EdgeData>                                            &edges,
    const std::unordered_map<CellEdge, size_t, CellEdgeHash>              &edge_index,
    const std::vector<std::vector<CommittedSegment>>                       &committed,
    const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &cells,
    const MicronTables                                                     &um,
    const std::vector<LayerData>                                           &layer_data,
    double min_h_mm, double max_h_mm, int num_layers,
    const char *label);

// ============================================================================
// MagmaTubeSolver — CP-SAT interval scheduling solver for tube assignment
// ============================================================================

class MagmaTubeSolver {
public:
    using ProgressFn      = std::function<void(int, int)>; // (current, total)
    using ThrowIfCanceled = std::function<void()>;

    MagmaTubeSolver(
        const MagmaLattice &lattice,
        const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &cells,
        const std::vector<LayerData> &layer_data,
        int    first_layer,
        double min_tube_height_mm,
        double max_tube_height_mm,
        int    num_layers,
        double dodge_distance_mm = 0.0,
        MagmaTubeSolverMode mode = MagmaTubeSolverMode::Refined,
        double solver_timeout_sec = 20.0);

    /// Run the solver. Populates out_pairs and out_cell_pair_index.
    void solve(
        std::vector<UTubePair> &out_pairs,
        std::unordered_map<TriangleCell, std::vector<int>, TriangleCellHash> &out_cell_pair_index,
        ProgressFn progress_fn = nullptr,
        ThrowIfCanceled throw_if_canceled = nullptr);

    /// Number of blocks that returned UNKNOWN (no solution found within timeout).
    /// Greedy solution is preserved for these blocks.
    int unknown_block_count() const { return m_unknown_blocks; }

private:
    // Pre-computation
    void build_micron_tables();
    void build_edges();
    void build_blocks(int off_a, int off_b, int off_z,
                      std::vector<Block> &out) const;

    // Solving
    BlockResult solve_block(const Block &block) const;
    int         solve_pass(int off_a, int off_b, int off_z,
                           std::function<void(int)> block_done_fn = nullptr);
    void        commit_results(const std::vector<Block> &blocks,
                               const std::vector<BlockResult> &results);

    // Output conversion
    void extract_results(
        std::vector<UTubePair> &out_pairs,
        std::unordered_map<TriangleCell, std::vector<int>,
                           TriangleCellHash> &out_index) const;

    // Input (const references — caller owns the data)
    // Lattice supplies cell topology (neighbors/is_up); offset-independent, so
    // any layer's lattice works for the solver's connectivity queries.
    const MagmaLattice &m_lattice;
    const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &m_cells;
    const std::vector<LayerData> &m_layer_data;

    // Config
    double m_min_h_mm;
    double m_max_h_mm;
    int    m_num_layers;
    // First index in m_layer_data backed by a real layer. Non-zero with a raft, since
    // Layer::id() is absolute. Rows below this are placeholders and must never be read.
    int    m_first_layer;
    int    m_z_window; // Z block size in layers
    double m_dodge_mm; // boundary dodge distance (0 = stagger disabled)
    MagmaTubeSolverMode m_mode;
    double m_timeout_sec;

    // Pre-computed
    MicronTables                                            m_um;
    std::vector<EdgeData>                                   m_edges;
    std::unordered_map<CellEdge, size_t, CellEdgeHash>     m_edge_index;
    // Cell → edge indices involving that cell (reverse lookup for stagger)
    std::unordered_map<TriangleCell, std::vector<size_t>, TriangleCellHash> m_cell_edges;

    // Committed assignments (updated between passes)
    std::vector<std::vector<CommittedSegment>> m_committed; // indexed by edge idx
    int m_unknown_blocks = 0; // blocks that timed out with no solution

    // Per-cell per-layer difficulty from greedy's initial unconstrained scoring.
    // 0 = easiest (max_neighbors at max_h), max_neighbors×max_h_um = hardest (no neighbors).
    // Kept for future use (not currently used for domain restriction).
    std::unordered_map<TriangleCell, std::vector<int64_t>, TriangleCellHash> m_cell_difficulty;

    // Computed at solve time
    double m_per_block_timeout = 10.0;  // = total budget / block count
    int    m_cpsat_workers = 8;         // all available cores

    // Constants
    static constexpr int    R              = 16;  // XY block size
    static constexpr int    R_OVERLAP      = 2;   // XY overlap between adjacent blocks
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaTubeSolver_hpp_
