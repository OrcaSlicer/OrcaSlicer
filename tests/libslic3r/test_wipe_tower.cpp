#include <catch2/catch_all.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"

using namespace Slic3r;

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
    CHECK(std::abs(unscaled(bb.min.y()) - (10. - R)) < 0.1);
    CHECK(std::abs(unscaled(bb.max.y()) - (10. + R)) < 0.1);
    // The footprint always contains the body box.
    CHECK(diff(Polygons{box}, Polygons{base}).empty());
}

TEST_CASE("Rib tower footprint estimate covers the generated footprint", "[WipeTower]") {
    // Parameters taken from a real project that reproduced the off-plate brim (Bambu P1S,
    // two painted PLAs): prime volumes 30/45 mm3, adhesiveness categories 100/0 (two stacked
    // purge blocks), tower width 35, layer height 0.21, nozzle 0.4, infill gap 150%,
    // rib width 8, no extra rib length, ~16 mm print height. The generated first-layer wall
    // bbox measured 29.56 mm from the sliced G-code; the pre-fix estimate said 23.585 mm.
    const float side = WipeTower::estimate_rib_tower_bbox_side({30.f, 45.f}, {100, 0},
                                                               35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    CHECK(side >= 29.56f);       // must cover the real footprint
    CHECK(side <= 29.56f + 4.f); // without grossly over-reserving plate space
}

TEST_CASE("Rib tower footprint estimate grows blocks per category", "[WipeTower]") {
    // Same total purge volume, one shared category vs two: separate categories stack their
    // blocks, so the footprint must not shrink when categories differ.
    const float one_block  = WipeTower::estimate_rib_tower_bbox_side({30.f, 45.f}, {0, 0},
                                                                     35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    const float two_blocks = WipeTower::estimate_rib_tower_bbox_side({30.f, 45.f}, {100, 0},
                                                                     35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    CHECK(two_blocks >= one_block);
}

TEST_CASE("Type1 block-stack depth matches the planner's math", "[WipeTower]") {
    // Cube.3mf params at width 35, layer 0.21, nozzle 0.4 (perimeter width 0.5), gap 0.75:
    // 30 mm3 -> 9 lines -> 6.75; 45 mm3 -> 13 lines -> 9.75; one finish gap per block;
    // stacked + one perimeter = 18.5 — the generated mesh footprint measured exactly this.
    const float depth = WipeTower::estimate_tower_blocks_depth({30.f, 45.f}, {100, 0}, 35.f, 0.21f, 0.4f, 1.5f);
    CHECK(std::abs(depth - 18.5f) < 0.01f);
    // Same volumes sharing one category: a layer can never purge into every filament (one of
    // them starts the layer), so the block is sized by the worst layer, not the sum — the
    // smallest purge (9 lines, 6.75) drops out: 9.75 + finish gap + perimeter = 11.0.
    const float shared = WipeTower::estimate_tower_blocks_depth({30.f, 45.f}, {100, 100}, 35.f, 0.21f, 0.4f, 1.5f);
    CHECK(std::abs(shared - 11.0f) < 0.01f);
    CHECK(WipeTower::estimate_tower_blocks_depth({}, {}, 35.f, 0.2f, 0.4f, 1.f) == 0.f);
    // A width narrower than two perimeter widths cannot hold purge lines.
    CHECK(WipeTower::estimate_tower_blocks_depth({45.f}, {0}, 0.9f, 0.2f, 0.4f, 1.f) == 0.f);
}

TEST_CASE("Brim width estimate matches the generator's loop quantization", "[WipeTower]") {
    // 3 mm configured, 0.4 nozzle, 0.2 first layer: spacing 0.4571, 7 loops -> 3.4286 mm,
    // the exact width measured from a generated mesh footprint.
    CHECK(std::abs(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f) - 3.4286f) < 0.01f);
    CHECK(WipeTower::estimate_brim_real_width(0.f, 0.4f, 0.2f) == 0.f);
    // Never below the configured width.
    CHECK(WipeTower::estimate_brim_real_width(5.f, 0.4f, 0.2f) >= 5.f);
}

TEST_CASE("Rib tower footprint estimate handles degenerate inputs", "[WipeTower]") {
    CHECK(WipeTower::estimate_rib_tower_bbox_side({}, {}, 35.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 16.f) == 0.f);
    CHECK(WipeTower::estimate_rib_tower_bbox_side({45.f}, {0}, 0.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 16.f) == 0.f);
    // A short tower must still reserve the stability rib extension.
    const float short_side = WipeTower::estimate_rib_tower_bbox_side({15.f}, {0},
                                                                     35.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 90.f);
    const float min_depth  = WipeTower::get_limit_depth_by_height(90.f);
    CHECK(short_side >= min_depth);
}
