#pragma once
#include <admesh/stl.h>
#include "Point.hpp"

namespace Slic3r { namespace tex2color {

using TriVertex    = stl_vertex;
using TriVertices  = std::vector<stl_vertex>;
using TriFace      = stl_triangle_vertex_indices;
using TriFaces     = std::vector<stl_triangle_vertex_indices>;

struct TriMesh : ::indexed_triangle_set {
    TriMesh() = default;
    TriMesh(const TriMesh&) = default;
    TriMesh& operator=(const TriMesh&) = default;
    TriMesh(TriMesh&&) = default;
    TriMesh& operator=(TriMesh&&) = default;
    TriMesh(const ::indexed_triangle_set& d) : ::indexed_triangle_set(d) {}
    TriMesh(::indexed_triangle_set&& d) : ::indexed_triangle_set(std::move(d)) {}
    TriMesh(std::vector<stl_triangle_vertex_indices> indices_,
            std::vector<stl_vertex> vertices_)
        : ::indexed_triangle_set(std::move(indices_), std::move(vertices_)) {}

    std::size_t facets_count() const { return indices.size(); }
};

} // namespace tex2color
} // namespace Slic3r
