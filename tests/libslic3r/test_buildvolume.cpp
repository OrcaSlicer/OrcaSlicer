#include <catch2/catch_all.hpp>
#include <vector>
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

static std::vector<Vec2d> rect_area(double w, double d)
{
    return { { 0., 0. }, { w, 0. }, { w, d }, { 0., d } };
}

// extruder_printable_height and extruder_printable_area are independent config options, so a
// profile can leave the heights short. BuildVolume must not index past the end of the heights.
TEST_CASE("BuildVolume falls back to the bed height when extruder_printable_height is short", "[BuildVolume]")
{
    const std::vector<Vec2d>              bed     = rect_area(200., 200.);
    const std::vector<std::vector<Vec2d>> areas   = { rect_area(200., 200.), rect_area(100., 200.) };
    const std::vector<double>             heights = { 180. };

    const BuildVolume build_volume(bed, 250., areas, heights);

    REQUIRE(build_volume.get_extruder_area_count() == 2);
    // The extruder with a height of its own keeps it, and differs from the bed, so it gets its own volume.
    CHECK_THAT(build_volume.get_extruder_area_volume(0).bboxf.max.z(), Catch::Matchers::WithinAbs(180., 1e-6));
    // The extruder without one falls back to the bed's printable_height instead of reading out of range.
    CHECK_THAT(build_volume.get_extruder_area_volume(1).bboxf.max.z(), Catch::Matchers::WithinAbs(250., 1e-6));
}

TEST_CASE("BuildVolume keeps per-extruder heights when both vectors match", "[BuildVolume]")
{
    const std::vector<Vec2d>              bed     = rect_area(200., 200.);
    const std::vector<std::vector<Vec2d>> areas   = { rect_area(120., 200.), rect_area(100., 200.) };
    const std::vector<double>             heights = { 180., 200.5 };

    const BuildVolume build_volume(bed, 250., areas, heights);

    REQUIRE(build_volume.get_extruder_area_count() == 2);
    CHECK_THAT(build_volume.get_extruder_area_volume(0).bboxf.max.z(), Catch::Matchers::WithinAbs(180., 1e-6));
    CHECK_THAT(build_volume.get_extruder_area_volume(1).bboxf.max.z(), Catch::Matchers::WithinAbs(200.5, 1e-6));
}

// A belt carries the part toward +Y, so a bed open along Y accepts a part running past its far Y edge,
// while the front edge, the belt width (X) and the printable height still apply.
TEST_CASE("BuildVolume open along Y ignores only the far Y edge", "[BuildVolume]")
{
    BuildVolume build_volume(rect_area(170., 200.), 200., {}, {});
    REQUIRE(build_volume.type() == BuildVolume_Type::Rectangle);

    // 140 x 1650 x 1 mm, starting on the bed and running past its far Y edge, and a part past the near Y edge.
    const BoundingBoxf3 long_part({ 10., 50., 0. }, { 150., 1700., 1. });
    const BoundingBoxf3 before_bed({ 10., -50., 0. }, { 150., 100., 1. });
    const BoundingBoxf3 too_wide({ -10., 50., 0. }, { 150., 100., 1. });
    const BoundingBoxf3 too_tall({ 10., 50., 0. }, { 150., 100., 250. });

    const indexed_triangle_set long_mesh = its_make_cube(140., 1650., 1.);
    Transform3f                long_trafo = Transform3f::Identity();
    long_trafo.translate(Vec3f(10.f, 50.f, 0.f));

    CHECK(build_volume.volume_state_bbox(long_part) == BuildVolume::ObjectState::Colliding);
    CHECK(build_volume.object_state(long_mesh, long_trafo, false) == BuildVolume::ObjectState::Colliding);

    build_volume.set_open_y(true);
    CHECK(build_volume.volume_state_bbox(long_part) == BuildVolume::ObjectState::Inside);
    CHECK(build_volume.object_state(long_mesh, long_trafo, false) == BuildVolume::ObjectState::Inside);
    CHECK(build_volume.volume_state_bbox(before_bed) == BuildVolume::ObjectState::Colliding);
    CHECK(build_volume.volume_state_bbox(too_wide) == BuildVolume::ObjectState::Colliding);
    CHECK(build_volume.volume_state_bbox(too_tall) == BuildVolume::ObjectState::Colliding);
    // The bed geometry itself stays finite.
    CHECK_THAT(build_volume.bounding_volume().max.y(), Catch::Matchers::WithinAbs(200., 1e-6));
}
