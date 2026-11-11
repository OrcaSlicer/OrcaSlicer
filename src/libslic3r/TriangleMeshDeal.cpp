#include "TriangleMeshDeal.hpp"
#include "TriangleMesh.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cstddef>
#include <utility>
#include "Point.hpp"
#include <igl/loop.h>
#undef NDEBUG
#include <assert.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {
TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok, TriangleMesh* unsmoothed)
{
    {
        using namespace igl;
        typedef Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor> RowMatrixX3f;
        typedef Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor>   RowMatrixX3i;

        auto vertices_count = mesh.its.vertices.size();
        auto indices_count  = mesh.its.indices.size();
        // Use Map to map the vertices and indicies into Matrixes without requiring a copy.
        const Eigen::Map<const RowMatrixX3f> OV(mesh.its.vertices[0].data(), vertices_count, 3);
        const Eigen::Map<const RowMatrixX3i> OF(mesh.its.indices[0].data(), indices_count, 3);
        Eigen::MatrixX3f                     V;
        Eigen::MatrixX3i                     F;

        ok = true;
        // TODO: add validation checks for the input mesh? Is this really necessary?
        // if ( <not OK> ) {
        //    ok = false;
        //    return TriangleMesh();
        // }
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
            std::copy(mesh.its.vertices.begin(), mesh.its.vertices.end(), flat.vertices.begin());
            for (size_t i = 0; i < indices_count; ++i) {
                const stl_triangle_vertex_indices &f = mesh.its.indices[i];
                const Vec3f &v0 = mesh.its.vertices[f(0)], &v1 = mesh.its.vertices[f(1)], &v2 = mesh.its.vertices[f(2)];
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
