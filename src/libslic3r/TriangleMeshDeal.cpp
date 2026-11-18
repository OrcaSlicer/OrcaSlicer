#include "TriangleMeshDeal.hpp"
#include "TriangleMesh.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>
#include "Point.hpp"
#include <igl/loop.h>
#undef NDEBUG
#include <assert.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {

// igl::loop() pairs exactly two faces per edge and sizes its vertex adjacency by the largest vertex index in use,
// so a repeated face, an edge of more than two faces or an unused vertex makes it read out of bounds.
// Drop repeated faces (in either winding) and unused vertices, and refuse the other edges.
static bool prepare_for_loop(indexed_triangle_set &its)
{
    std::vector<std::array<int, 3>> keys(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i) {
        keys[i] = {its.indices[i](0), its.indices[i](1), its.indices[i](2)};
        std::sort(keys[i].begin(), keys[i].end());
    }
    std::vector<size_t> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&keys](size_t a, size_t b) { return keys[a] < keys[b]; });
    std::vector<bool> repeated(keys.size(), false);
    for (size_t i = 1; i < order.size(); ++i)
        repeated[order[i]] = keys[order[i]] == keys[order[i - 1]];
    size_t kept = 0;
    for (size_t i = 0; i < its.indices.size(); ++i)
        if (!repeated[i])
            its.indices[kept++] = its.indices[i];
    its.indices.resize(kept);
    its_compactify_vertices(its);
    if (its.indices.empty())
        return false;

    std::vector<std::pair<int, int>> edges;
    edges.reserve(3 * its.indices.size());
    for (const stl_triangle_vertex_indices &f : its.indices)
        for (int j = 0; j < 3; ++j)
            edges.emplace_back(std::minmax(f(j), f((j + 1) % 3)));
    std::sort(edges.begin(), edges.end());
    for (size_t i = 2; i < edges.size(); ++i)
        if (edges[i] == edges[i - 2])
            return false;
    return true;
}

TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok, TriangleMesh* unsmoothed)
{
    {
        using namespace igl;
        typedef Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor> RowMatrixX3f;
        typedef Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor>   RowMatrixX3i;

        indexed_triangle_set input = mesh.its;
        ok = prepare_for_loop(input);
        if (!ok)
            return TriangleMesh();

        auto vertices_count = input.vertices.size();
        auto indices_count  = input.indices.size();
        // Use Map to map the vertices and indicies into Matrixes without requiring a copy.
        const Eigen::Map<const RowMatrixX3f> OV(input.vertices[0].data(), vertices_count, 3);
        const Eigen::Map<const RowMatrixX3i> OF(input.indices[0].data(), indices_count, 3);
        Eigen::MatrixX3f                     V;
        Eigen::MatrixX3i                     F;

        loop(OV, OF, V, F);

        indexed_triangle_set its;
        auto                 iterv = V.rowwise();
        auto                 iterf = F.rowwise();
        its.vertices.assign(iterv.cbegin(), iterv.cend());
        its.indices.assign(iterf.cbegin(), iterf.cend());

        if (unsmoothed) {
            // igl::loop() keeps the input vertices at their indices and replaces input triangle i by triangles 4i..4i+3:
            // (v0, m01, m20), (v1, m12, m01), (m01, m12, m20), (m12, v2, m20), where mXY is a new vertex on edge vX vY.
            indexed_triangle_set flat;
            flat.indices = its.indices;
            flat.vertices.assign(its.vertices.size(), Vec3f::Zero());
            std::copy(input.vertices.begin(), input.vertices.end(), flat.vertices.begin());
            for (size_t i = 0; i < indices_count; ++i) {
                const stl_triangle_vertex_indices &f = input.indices[i];
                const Vec3f &v0 = input.vertices[f(0)], &v1 = input.vertices[f(1)], &v2 = input.vertices[f(2)];
                flat.vertices[its.indices[4 * i](1)]     = 0.5f * (v0 + v1);
                flat.vertices[its.indices[4 * i + 1](1)] = 0.5f * (v1 + v2);
                flat.vertices[its.indices[4 * i](2)]     = 0.5f * (v2 + v0);
            }
            *unsmoothed = TriangleMesh(std::move(flat));
        }
        TriangleMesh result_mesh(its);
        return result_mesh;
    }
}
} // namespace Slic3r
