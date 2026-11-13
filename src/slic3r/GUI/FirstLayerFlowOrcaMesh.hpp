#pragma once
// Adapter only: no GUI, configuration, file I/O, or ownership side effects.
// Include after Orca's TriangleMesh.hpp in the integration translation unit.
#include "FirstLayerFlowDigits.hpp"
#include "FirstLayerFlowMeshChecks.hpp"
#include <limits>
#include <stdexcept>

namespace flf {
// Converts checked, self-contained label geometry into an Orca TriangleMesh.
// No scaling or placement is hidden here; caller passes an already placed mesh.
inline Slic3r::TriangleMesh to_orca_mesh(const Mesh& label, float z_offset = 0.0f) {
    validate_single_layer(label);
    if (label.vertices.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("label has too many vertices");
    ::indexed_triangle_set its;
    its.vertices.reserve(label.vertices.size());
    its.indices.reserve(label.triangles.size());
    for (const auto& v : label.vertices)
        its.vertices.emplace_back(v.x, v.y, v.z + z_offset);
    for (const auto& t : label.triangles)
        its.indices.emplace_back(static_cast<int>(t.a), static_cast<int>(t.b), static_cast<int>(t.c));
    return Slic3r::TriangleMesh(std::move(its));
}
} // namespace flf
