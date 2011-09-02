#include <iostream>
#include <fstream>
#include <string>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/SLA/EigenMesh3D.hpp>

#include <Shiny/Shiny.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4267)
#endif
#include <igl/ray_mesh_intersect.h>
#include <igl/point_mesh_squared_distance.h>
#include <igl/remove_duplicate_vertices.h>
#include <igl/signed_distance.h>
#include <igl/random_dir.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

const std::string USAGE_STR = {
    "Usage: aabb-evaluation stlfilename.stl"
};

using namespace Slic3r;

void profile(const TriangleMesh &mesh)
{
    Eigen::MatrixXd V;
    Eigen::MatrixXi F;
    Eigen::MatrixXd vertex_normals;
    sla::to_eigen_mesh(mesh, V, F);
    igl::per_vertex_normals(V, F, vertex_normals);

    static constexpr int num_samples = 100;
    const int num_vertices = std::min(10000, int(mesh.its.vertices.size()));
    const Eigen::MatrixXd dirs = igl::random_dir_stratified(num_samples).cast<double>();

    Eigen::MatrixXd occlusion_output0;
    {
        AABBTreeIndirect::Tree3f tree;
        {
            PROFILE_BLOCK(AABBIndirect_Init);
            tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.its.vertices, mesh.its.indices);
        }
        {
            PROFILE_BLOCK(EigenMesh3D_AABBIndirectF_AmbientOcclusion);
            occlusion_output0.resize(num_vertices, 1);
            for (int ivertex = 0; ivertex < num_vertices; ++ ivertex) {
                const Eigen::Vector3d origin = mesh.its.vertices[ivertex].template cast<double>();
                const Eigen::Vector3d normal = vertex_normals.row(ivertex).template cast<double>();
                int num_hits = 0;
                for (int s = 0; s < num_samples; s++) {
                    Eigen::Vector3d d = dirs.row(s);
                    if(d.dot(normal) < 0) {
                        // reverse ray
                        d *= -1;
                    }
                    igl::Hit hit;
                    if (AABBTreeIndirect::intersect_ray_first_hit(mesh.its.vertices, mesh.its.indices, tree, (origin + 1e-4 * d).eval(), d, hit))
                        ++ num_hits;
                }
                occlusion_output0(ivertex) = (double)num_hits/(double)num_samples;
            }
        }

        {
            PROFILE_BLOCK(EigenMesh3D_AABBIndirectFF_AmbientOcclusion);
            occlusion_output0.resize(num_vertices, 1);
            for (int ivertex = 0; ivertex < num_vertices; ++ ivertex) {
                const Eigen::Vector3d origin = mesh.its.vertices[ivertex].template cast<double>();
                const Eigen::Vector3d normal = vertex_normals.row(ivertex).template cast<double>();
                int num_hits = 0;
                for (int s = 0; s < num_samples; s++) {
                    Eigen::Vector3d d = dirs.row(s);
                    if(d.dot(normal) < 0) {
                        // reverse ray
                        d *= -1;
                    }
                    igl::Hit hit;
                    if (AABBTreeIndirect::intersect_ray_first_hit(mesh.its.vertices, mesh.its.indices, tree, 
                            Eigen::Vector3f((origin + 1e-4 * d).template cast<float>()),
                            Eigen::Vector3f(d.template cast<float>()), hit))
                        ++ num_hits;
                }
                occlusion_output0(ivertex) = (double)num_hits/(double)num_samples;
            }
        }
    }

    Eigen::MatrixXd occlusion_output1;
    {
        std::vector<Vec3d> vertices;
        std::vector<Vec3i32> triangles;
        for (int i = 0; i < V.rows(); ++ i)
            vertices.emplace_back(V.row(i).transpose());
        for (int i = 0; i < F.rows(); ++ i)
            triangles.emplace_back(F.row(i).transpose());
        AABBTreeIndirect::Tree3d tree;
        {
            PROFILE_BLOCK(AABBIndirectD_Init);
            tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(vertices, triangles);
        }

        {
            PROFILE_BLOCK(EigenMesh3D_AABBIndirectD_AmbientOcclusion);
            occlusion_output1.resize(num_vertices, 1);
            for (int ivertex = 0; ivertex < num_vertices; ++ ivertex) {
                const Eigen::Vector3d origin = V.row(ivertex).template cast<double>();
                const Eigen::Vector3d normal = vertex_normals.row(ivertex).template cast<double>();
                int num_hits = 0;
                for (int s = 0; s < num_samples; s++) {
                    Eigen::Vector3d d = dirs.row(s);
                    if(d.dot(normal) < 0) {
                        // reverse ray
                        d *= -1;
                    }
                    igl::Hit hit;
                    if (AABBTreeIndirect::intersect_ray_first_hit(vertices, triangles, tree, Eigen::Vector3d(origin + 1e-4 * d), d, hit))
                        ++ num_hits;
                }
                occlusion_output1(ivertex) = (double)num_hits/(double)num_samples;
            }
        }
    }

    // Build the AABB accelaration tree

    Eigen::MatrixXd occlusion_output2;
    {
        igl::AABB<Eigen::MatrixXd, 3> AABB;
        {
            PROFILE_BLOCK(EigenMesh3D_AABB_Init);
            AABB.init(V, F);
        }
        {
            PROFILE_BLOCK(EigenMesh3D_AABB_AmbientOcclusion);
            occlusion_output2.resize(num_vertices, 1);
            for (int ivertex = 0; ivertex < num_vertices; ++ ivertex) {
                const Eigen::Vector3d origin = V.row(ivertex).template cast<double>();
                const Eigen::Vector3d normal = vertex_normals.row(ivertex).template cast<double>();
                int num_hits = 0;
                for (int s = 0; s < num_samples; s++) {
                    Eigen::Vector3d d = dirs.row(s);
                    if(d.dot(normal) < 0) {
                        // reverse ray
                        d *= -1;
                    }
                    igl::Hit hit;
                    if (AABB.intersect_ray(V, F, origin + 1e-4 * d, d, hit))
                        ++ num_hits;
                }
                occlusion_output2(ivertex) = (double)num_hits/(double)num_samples;
            }
        }
    }

    Eigen::MatrixXd occlusion_output3;
    {
        typedef Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>> MapMatrixXfUnaligned;
        typedef Eigen::Map<const Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>> MapMatrixXiUnaligned;
        igl::AABB<MapMatrixXfUnaligned, 3> AABB;
        auto vertices = MapMatrixXfUnaligned(mesh.its.vertices.front().data(), mesh.its.vertices.size(), 3);
        auto faces = MapMatrixXiUnaligned(mesh.its.indices.front().data(), mesh.its.indices.size(), 3);
        {
            PROFILE_BLOCK(EigenMesh3D_AABBf_Init);
            AABB.init(
                vertices,
                faces);
        }

        {
            PROFILE_BLOCK(EigenMesh3D_AABBf_AmbientOcclusion);
            occlusion_output3.resize(num_vertices, 1);
            for (int ivertex = 0; ivertex < num_vertices; ++ ivertex) {
                const Eigen::Vector3d origin = mesh.its.vertices[ivertex].template cast<double>();
                const Eigen::Vector3d normal = vertex_normals.row(ivertex).template cast<double>();
                int num_hits = 0;
                for (int s = 0; s < num_samples; s++) {
                    Eigen::Vector3d d = dirs.row(s);
                    if(d.dot(normal) < 0) {
                        // reverse ray
                        d *= -1;
                    }
                    igl::Hit hit;
                    if (AABB.intersect_ray(vertices, faces, (origin + 1e-4 * d).eval().template cast<float>(), d.template cast<float>(), hit))
                        ++ num_hits;
                }
                occlusion_output3(ivertex) = (double)num_hits/(double)num_samples;
            }
        }
    }

    PROFILE_UPDATE();
    PROFILE_OUTPUT(nullptr);
}

int main(const int argc, const char *argv[])
{
    if(argc < 2) {
        std::cout << USAGE_STR << std::endl;
        return EXIT_SUCCESS;
    }

    TriangleMesh mesh;
    if (! mesh.ReadSTLFile(argv[1])) {
        std::cerr << "Error loading " << argv[1] << std::endl;
        return -1;
    }

    if (mesh.empty()) {
        std::cerr << "Error loading " << argv[1] << " . It is empty." << std::endl;
        return -1;
    }

    profile(mesh);    

    return EXIT_SUCCESS;
}
