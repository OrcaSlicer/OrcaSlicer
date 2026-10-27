#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace Slic3r {

class TriangleMesh;

struct MeshRepairStats
{
    std::size_t welded_vertices    = 0;
    std::size_t removed_triangles  = 0;
    std::size_t reversed_shells    = 0;
    std::size_t filled_holes       = 0;
    std::size_t removed_shells     = 0;
    std::size_t self_intersections = 0;
    // False when the input already was closed, consistently oriented and free of self-intersections.
    bool        modified           = false;
};

// Returns false, leaving the mesh untouched, if no solid can be built. progress gets the percentage done and may
// throw to cancel. See docs/HLSD/mesh-repair.md.
bool repair_mesh(TriangleMesh &mesh, MeshRepairStats *stats = nullptr, std::string *error = nullptr,
                 const std::function<void(int)> &progress = {});

} // namespace Slic3r
