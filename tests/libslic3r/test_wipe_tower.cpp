#include <catch2/catch_all.hpp>

#include <cmath>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// A Bambu P1S project that reproduced the off-plate brim: two PLAs priming 30 and 45 mm3 in
// separate adhesiveness categories on a 35 mm tower, 0.21 mm layers, 0.4 nozzle (0.5 mm lines),
// 150 % infill gap (0.75 mm line pitch), rib width 8, 16 mm tall.
static std::vector<WipeTower::PurgeEstimate> cube_purges(int first_category = 100)
{
    return {{30.f, first_category}, {45.f, 0}};
}

TEST_CASE("Cone base polygon bulges past the body box", "[WipeTower]") {
    // Zero angle: plain body box.
    const Polygon box = WipeTower2::cone_base_polygon(35., 20., 100., 0.);
    CHECK(box.points.size() == 4);
    CHECK(get_extents(box).size() == Point::new_scale(Vec2d(35., 20.)));
    // A 25-degree cone on a 100 mm tower: base radius R = tan(12.5deg)*100 = 22.2 mm,
    // which exceeds the body half-depth, so the footprint bulges to center +- R in y
    // (support_scale keeps the x extent compressed near the body).
    const Polygon     base = WipeTower2::cone_base_polygon(35., 20., 100., 25.);
    const BoundingBox bb   = get_extents(base);
    const double      R    = std::tan(25. / 2. * M_PI / 180.) * 100.;
    CHECK_THAT(unscaled(bb.min.y()), WithinAbs(10. - R, 0.1));
    CHECK_THAT(unscaled(bb.max.y()), WithinAbs(10. + R, 0.1));
    // The footprint always contains the body box.
    CHECK(diff(Polygons{box}, Polygons{base}).empty());
}

TEST_CASE("Type1 block-stack depth quantizes each purge to whole lines", "[WipeTower]") {
    // A 0.5 mm line at 0.21 mm carries 0.0955 mm3 per mm, so across the 34 mm between the
    // perimeters 30 mm3 is 10 lines and 45 mm3 is 14: 7.5 + 10.5 at the 0.75 mm pitch behind
    // one perimeter width. The generated mesh of the project measured exactly this.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(18.5f, 0.01f));
    // Sharing one category, a layer can never purge into every filament (one of them starts
    // the layer), so the block is sized by its worst layer and the 10-line purge drops out.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(11.0f, 0.01f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({}, 35.f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
    // A width narrower than two perimeter widths cannot hold purge lines.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({{45.f, 0}}, 0.9f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A nozzle change adds its ramming lines to the block", "[WipeTower]") {
    // 10 mm of 1.75 mm filament (24.05 mm3) laid as 1.0 mm nozzle-change lines at 0.2 mm
    // (0.1914 mm2 each) is 125.7 mm; across the 48.5 mm available that is 3 lines of 1.0 mm.
    std::vector<WipeTower::PurgeEstimate> purges{{100.f, 0}, {100.f, 0}};
    const float without_change = WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f);
    purges.front().filament_change_length = 10.f;
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f) - without_change, WithinAbs(3.f, 1e-4f));
}

TEST_CASE("Rib tower footprint estimate covers the generated footprint", "[WipeTower]") {
    // The generated first-layer wall bbox of the project measured 29.56 mm from the sliced
    // G-code; the volume-only estimate said 23.585 mm.
    const float side = WipeTower::estimate_rib_tower_bbox_side(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    CHECK(side >= 29.56f);
    CHECK(side <= 29.56f + 4.f); // without grossly over-reserving plate space
    // Separate categories stack their blocks, so the footprint must not shrink when they differ.
    CHECK(side >= WipeTower::estimate_rib_tower_bbox_side(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f));
    CHECK_THAT(WipeTower::estimate_rib_tower_bbox_side({}, 35.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 16.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Rib footprint extends the ribs, not the body, below the stability minimum", "[WipeTower]") {
    // A 10 mm body under a 90 mm print: the ribs stretch to the minimum depth's diagonal, and
    // the rib width is capped at half the body, so the square grows to minimum + 5 / sqrt(2).
    const float min_depth = WipeTower::get_limit_depth_by_height(90.f);
    REQUIRE(min_depth > 10.f);
    CHECK_THAT(WipeTower::rib_footprint_side(10.f, 10.f, 8.f, 0.f, 90.f), WithinAbs(min_depth + 5.f / std::sqrt(2.f), 1e-4f));
    // The extra rib length runs along the diagonal, so it shows as its projection on each axis.
    const float plain = WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 0.f, 5.f);
    CHECK_THAT(plain, WithinAbs(30.f + 8.f / std::sqrt(2.f), 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 4.f, 5.f) - plain, WithinAbs(4.f / std::sqrt(2.f), 1e-4f));
    // A negative extra length cannot pull the ribs inside the diagonal.
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, -4.f, 5.f), WithinAbs(plain, 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(0.f, 30.f, 8.f, 0.f, 5.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Brim width estimate matches each generator's loop quantization", "[WipeTower]") {
    // 3 mm configured, 0.4 nozzle, 0.2 first layer: 0.4571 mm spacing, 7 loops. WipeTower2
    // prints and reports the 7 loops; WipeTower reports half a spacing of line width on top.
    const float spacing = 0.5f - 0.2f * float(1. - M_PI_4);
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, true), WithinAbs(7.f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, false), WithinAbs(7.5f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(0.f, 0.4f, 0.2f, true), WithinAbs(0.f, 1e-6f));
}
