#include "TriangleMeshDeal.hpp"

#include <igl/read_triangle_mesh.h>
#include <igl/loop.h>
#include <igl/upsample.h>
#include <igl/false_barycentric_subdivision.h>

namespace Slic3r {
TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh &mesh, bool &ok)
{
        {
            using namespace std;
            using namespace igl;
            Eigen::MatrixXi OF, F;
            Eigen::MatrixXd OV, V;
            auto            vertices_count = mesh.its.vertices.size();
            OV                             = Eigen::MatrixXd(vertices_count, 3);
            for (int i = 0; i < vertices_count; i++) {
                auto v = mesh.its.vertices[i];
                OV.row(i) << v[0], v[1], v[2];
            }
            auto indices_count = mesh.its.indices.size();
            OF                 = Eigen::MatrixXi(indices_count, 3);
            for (int i = 0; i < indices_count; i++) {
                auto face = mesh.its.indices[i];
                OF.row(i) << face[0], face[1], face[2];
            }
            //igl:: read_triangle_mesh( "E:/Download/libigl-2.6.0/out/build/x64-Debug/_deps/libigl_tutorial_data-src/decimated-knight.off", OV, OF);
            V = OV;
            F = OF;

            //igl::upsample(Eigen::MatrixXd(V), Eigen::MatrixXi(F), V, F);
            ok = true;
            if (!igl::loop(Eigen::MatrixXd(V), Eigen::MatrixXi(F), V, F)) {
                ok = false;
                return TriangleMesh();
            }
            //igl::false_barycentric_subdivision(Eigen::MatrixXd(V), Eigen::MatrixXi(F), V, F);
            indexed_triangle_set its;
            int         vertex_count = V.rows();
            its.vertices.resize(vertex_count);
            for (int i = 0; i < vertex_count; i++) {
                its.vertices[i] = V.row(i).cast<float>();
            }
            int indice_count = F.rows();
            its.indices.resize(indice_count);
            for (int i = 0; i < indice_count; i++) {
                auto cur                   = F.row(i);
                its.indices[i] = Slic3r::Vec3i(cur[0], cur[1], cur[2]);
            }
            TriangleMesh result_mesh(its);
            return result_mesh;
        }
    }
} // namespace Slic3r
