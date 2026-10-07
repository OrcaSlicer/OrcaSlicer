#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
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

TEST_CASE("mcut difference cuts every disconnected component of the tool mesh", "[MeshBoolean]") {
    TriangleMesh body = make_cube(30., 10., 10.);

    // Separate through-holes, like the letters of an embossed text turned into an object.
    const std::vector<float> xs = {3.f, 12.f, 21.f};
    TriangleMesh             tool;
    for (float x : xs) {
        TriangleMesh letter = make_cube(4., 4., 20.);
        letter.translate(Vec3f(x, 3.f, -5.f));
        its_merge(tool.its, letter.its);
    }

    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(body, tool, result, "A_NOT_B");

    REQUIRE(result.size() == 1);
    const double expected = body.volume() - double(xs.size()) * 4. * 4. * 10.;
    REQUIRE_THAT(result.front().volume(), Catch::Matchers::WithinRel(expected, 1e-3));
}
