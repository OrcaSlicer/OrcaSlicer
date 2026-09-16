#include <catch2/catch_all.hpp>

#include "libslic3r/BuildVolume.hpp"

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
