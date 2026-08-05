#pragma once

#include "TriangleMesh.hpp"

namespace Slic3r {
class TriangleMeshDeal
{
public:
    static TriangleMesh smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok);
};
} // namespace Slic3r
