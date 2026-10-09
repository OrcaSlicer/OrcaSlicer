#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <vector>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Catch::Approx(sphere.volume()));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

TEST_CASE("mcut difference handles source splits between cuts", "[MeshBoolean]") {
    TriangleMesh body = make_cube(30., 10., 10.);

    TriangleMesh tool;

    // First cut splits the source into two disconnected components.
    TriangleMesh slab = make_cube(2., 12., 20.);
    slab.translate(Vec3f(14.f, -1.f, -5.f));
    its_merge(tool.its, slab.its);

    // These cuts must still be applied after the source has been split.
    TriangleMesh left_hole = make_cube(4., 4., 20.);
    left_hole.translate(Vec3f(3.f, 3.f, -5.f));
    its_merge(tool.its, left_hole.its);

    TriangleMesh right_hole = make_cube(4., 4., 20.);
    right_hole.translate(Vec3f(21.f, 3.f, -5.f));
    its_merge(tool.its, right_hole.its);

    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(body, tool, result, "A_NOT_B");

    REQUIRE(result.size() == 1);

    const std::vector<indexed_triangle_set> components =
        its_split(result.front().its);

    REQUIRE(components.size() == 2);

    // 3000 - 200 - 160 - 160 = 2480.
    REQUIRE_THAT(
        result.front().volume(),
        Catch::Matchers::WithinRel(2480., 1e-3)
    );
}
