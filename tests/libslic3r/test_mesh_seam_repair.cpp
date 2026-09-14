#include <catch2/catch_all.hpp>
#include "libslic3r/MeshSeamRepair.hpp"

using namespace Slic3r;

static indexed_triangle_set disconnected_tetrahedron()
{
    const Vec3f points[] = { {0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10} };
    const Vec3i32 faces[] = { {0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3} };
    indexed_triangle_set mesh;
    for (const auto& face : faces) {
        const int start = int(mesh.vertices.size());
        for (int corner = 0; corner < 3; ++corner)
            mesh.vertices.push_back(points[face[corner]]);
        mesh.indices.emplace_back(start, start + 1, start + 2);
    }
    return mesh;
}

TEST_CASE("Exact texture seams close without changing painted face coordinates", "[MeshSeamRepair][Regression]")
{
    TriangleMesh mesh(disconnected_tetrahedron());
    const auto original = mesh.its;
    mesh.set_init_shift({7, -3, 2});
    REQUIRE(its_num_open_edges(mesh.its) == 12);
    const auto volume = mesh.volume();

    REQUIRE(stitch_exact_mesh_seams(mesh));
    REQUIRE(its_num_open_edges(mesh.its) == 0);
    REQUIRE(mesh.its.vertices.size() == 4);
    REQUIRE(mesh.its.indices.size() == original.indices.size());
    REQUIRE(mesh.get_init_shift() == Vec3d(7, -3, 2));
    REQUIRE_THAT(mesh.volume(), Catch::Matchers::WithinAbs(volume, 1e-5));
    for (size_t face = 0; face < original.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner)
            REQUIRE(mesh.its.vertices[mesh.its.indices[face][corner]] == original.vertices[original.indices[face][corner]]);
    REQUIRE_FALSE(stitch_exact_mesh_seams(mesh));
}

TEST_CASE("A real missing face is not mistaken for a closed texture seam", "[MeshSeamRepair][Regression]")
{
    auto source = disconnected_tetrahedron();
    source.indices.pop_back();
    TriangleMesh mesh(source);
    REQUIRE_FALSE(stitch_exact_mesh_seams(mesh));
    REQUIRE(mesh.its.vertices == source.vertices);
    REQUIRE(mesh.its.indices == source.indices);
}
