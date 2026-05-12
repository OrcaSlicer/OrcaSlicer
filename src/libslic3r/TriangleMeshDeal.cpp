#include "TriangleMeshDeal.hpp"

#include <igl/loop.h>
#undef NDEBUG
#include <assert.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {
TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok)
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
        TriangleMesh result_mesh(its);
        return result_mesh;
    }
}
} // namespace Slic3r
