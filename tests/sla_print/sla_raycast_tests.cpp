#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/SLA/IndexedMesh.hpp>
#include <libslic3r/SLA/Hollowing.hpp>

#include "sla_test_utils.hpp"

using namespace Slic3r;

// First do a simple test of the hole raycaster.
TEST_CASE("Raycaster - find intersections of a line and cylinder")
{
    sla::DrainHole hole{Vec3f(0,0,0), Vec3f(0,0,1), 5, 10};
    std::array<std::pair<float, Vec3d>, 2> out;
    Vec3f s;
    Vec3f dir;

    // Start inside the hole and cast perpendicular to its axis.
    s = {-1.f, 0, 5.f};
    dir = {1.f, 0, 0};
    hole.get_intersections(s, dir, out);
    REQUIRE(out[0].first == Catch::Approx(-4.f));
    REQUIRE(out[1].first == Catch::Approx(6.f));

    // Start outside and cast parallel to axis.
    s = {0, 0, -1.f};
    dir = {0, 0, 1.f};
    hole.get_intersections(s, dir, out);
    REQUIRE(std::abs(out[0].first - 1.f) < 0.001f);
    REQUIRE(std::abs(out[1].first - 11.f) < 0.001f);

    // Start outside and cast so that entry is in base and exit on the cylinder
    s = {0, -1.f, -1.f};
    dir = {0, 1.f, 1.f};
    dir.normalize();
    hole.get_intersections(s, dir, out);
    REQUIRE(std::abs(out[0].first - std::sqrt(2.f)) < 0.001f);
    REQUIRE(std::abs(out[1].first - std::sqrt(72.f)) < 0.001f);
}

#ifdef SLIC3R_HOLE_RAYCASTER
// Create a simple scene with a 20mm cube and a big hole in the front wall 
// with 5mm radius. Then shoot rays from interesting positions and see where
// they land.
TEST_CASE("Raycaster with loaded drillholes", "[sla_raycast]") 
{
    // Load the cube and make it hollow.
    TriangleMesh cube = load_model("20mm_cube.obj");
    sla::HollowingConfig hcfg;
    std::unique_ptr<TriangleMesh> cube_inside = sla::generate_interior(cube, hcfg);
    REQUIRE(cube_inside);
    
    // Helper bb
    auto boxbb = cube.bounding_box();
    
    // Create the big 10mm long drainhole in the front wall.
    Vec3f center = boxbb.center().cast<float>();
    Vec3f p = {center.x(), 0., center.z()};
    Vec3f normal = {0.f, 1.f, 0.f};
    float radius = 5.f;
    float hole_length = 10.;
    sla::DrainHoles holes = { sla::DrainHole{p, normal, radius, hole_length} };
    
    cube.merge(*cube_inside);
    
    sla::IndexedMesh emesh{cube};
    emesh.load_holes(holes);
    
    Vec3d s = center.cast<double>();
    // Fire from center, should hit the interior wall
    auto hit = emesh.query_ray_hit(s, {0, 1., 0.});
    REQUIRE(hit.distance() == Catch::Approx(boxbb.size().x() / 2 - hcfg.min_thickness));
    
    // Fire upward from hole center, hit distance equals the radius (hits the
    // side of the hole cut.
    s.y() = hcfg.min_thickness / 2;
    hit = emesh.query_ray_hit(s, {0, 0., 1.});
    REQUIRE(hit.distance() == Catch::Approx(radius));

    // Fire from outside, hit the back side of the cube interior
    s.y() = -1.;
    hit = emesh.query_ray_hit(s, {0, 1., 0.});
    REQUIRE(hit.distance() == Catch::Approx(boxbb.max.y() - hcfg.min_thickness - s.y()));
    
    // Fire downwards from above the hole cylinder. Has to go through the cyl.
    // as it was not there.
    s = center.cast<double>();
    s.z() = boxbb.max.z() - hcfg.min_thickness - 1.;
    hit = emesh.query_ray_hit(s, {0, 0., -1.});
    REQUIRE(hit.distance() == Catch::Approx(s.z() - boxbb.min.z() - hcfg.min_thickness));

    // Check for support tree correctness
    test_support_model_collision("20mm_cube.obj", {}, hcfg, holes);
}
#endif
