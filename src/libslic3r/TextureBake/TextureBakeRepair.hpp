#pragma once

// T-junction resolution and edge-defect accounting.
//
// Decimation can collapse a long edge whose interior still carries neighbouring triangles' vertices.
// Those then sit *on* an edge rather than at an end: watertight vertex-for-vertex, but the edge has
// one incident face on one side, which a slicer reads as an open boundary. This splits the offending
// face into a fan so every on-edge vertex becomes a real corner.

#include <cstdint>
#include <vector>

#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

struct EdgeDefects
{
    size_t open = 0, non_manifold = 0, triangles = 0;
};

// Welds at the export grid first: counting on the un-snapped mesh reports defects the file does not
// have and misses ones it does.
EdgeDefects count_edge_defects(const TriSoup &geometry, double quant = WELD_GRID_EXPORT);

// Triangles a slicer would drop as degenerate. Each one, removed, punches a hole - so a non-zero
// count means watertight only on paper.
size_t count_area_slivers(const TriSoup &geometry);

struct RepairOptions
{
    // Coordinates are snapped onto this grid, matching the precision files are written with.
    double weld_quant = WELD_GRID_EXPORT;

    // How far off an edge a vertex may sit and still count as on it. Well above the harvest
    // tolerance, since harvesting leaves a region flat only to within that, making a collapsed edge a
    // chord the on-edge vertices deviate from by about as much. Still far below the weld grid.
    double on_seg_tol = 0.02;

    // Splitting one face can expose another behind it, so the pass cascades.
    int max_iters = 16;
};

TriSoup resolve_t_junctions(const TriSoup &geometry, const RepairOptions &opts = {});

} // namespace TextureBake
} // namespace Slic3r
