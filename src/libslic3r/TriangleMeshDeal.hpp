#pragma once

#include "TriangleMesh.hpp"

namespace Slic3r {
class TriangleMeshDeal
{
public:
    // Loop subdivision: every triangle is split into four and every vertex is moved to smooth the surface.
    // `unsmoothed`, when given, receives the same triangles before the vertices are moved, so it lies on the input surface.
    static TriangleMesh smooth_triangle_mesh(const TriangleMesh &mesh, bool &ok, TriangleMesh *unsmoothed = nullptr);
};
} // namespace Slic3r
