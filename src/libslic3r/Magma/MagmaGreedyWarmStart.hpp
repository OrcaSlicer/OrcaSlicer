#ifndef slic3r_Magma_MagmaGreedyWarmStart_hpp_
#define slic3r_Magma_MagmaGreedyWarmStart_hpp_

#include "MagmaTriangleCell.hpp"
#include "MagmaTubeMap.hpp"
#include "MagmaTubeSolver.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace magma {

/// Per-cell per-layer difficulty score. Indexed by [layer_id - first_layer],
/// matching CellPresence's indexing. Values in microns:
///   0 = easiest (3 unconstrained neighbors at max tube height)
///   3 × max_h_um = hardest (no fillable neighbors)
using CellDifficultyMap = std::unordered_map<
    TriangleCell, std::vector<int64_t>, TriangleCellHash>;

/// Greedy warm start: assign tubes using a most-constrained-first heuristic.
/// Populates committed (must be pre-sized to edges.size()) with layer-aligned
/// CommittedSegments that respect min/max height, run boundaries, and per-cell
/// NoOverlap. Fast (100-300ms for 1000+ cells) and deterministic.
///
/// If out_difficulty is non-null, captures per-cell per-layer difficulty scores
/// from the initial (unconstrained) heap build. Used by CP-SAT to decide which
/// runs are safe for grid domain restriction (stagger).
void greedy_warm_start(
    const MagmaLattice                                                      &lattice,
    const std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> &cells,
    const std::vector<EdgeData>                                             &edges,
    const std::unordered_map<TriangleCell, std::vector<size_t>, TriangleCellHash> &cell_edges,
    const MicronTables                                                      &um,
    int64_t min_h_um,
    int64_t max_h_um,
    std::vector<std::vector<CommittedSegment>>                              &committed,
    CellDifficultyMap                                                       *out_difficulty = nullptr);

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaGreedyWarmStart_hpp_
