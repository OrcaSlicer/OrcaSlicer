#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

Polygon rectangle(float width, float depth)
{
    return Polygon({
        Point::new_scale(0.f, 0.f),
        Point::new_scale(width, 0.f),
        Point::new_scale(width, depth),
        Point::new_scale(0.f, depth)
    });
}

} // namespace

TEST_CASE("Wipe tower preview filament ids are normalized", "[WipeTower]")
{
    CHECK(normalize_wipe_tower_preview_filaments(4, {3, 7, 1, 3}, false) ==
          std::vector<unsigned int>{1, 3});
    CHECK(normalize_wipe_tower_preview_filaments(4, {2}, false).empty());
    CHECK(normalize_wipe_tower_preview_filaments(4, {2}, true) ==
          std::vector<unsigned int>{2});
    CHECK(normalize_wipe_tower_preview_filaments(4, {}, true) ==
          std::vector<unsigned int>{0});
    CHECK(normalize_wipe_tower_preview_filaments(0, {}, true).empty());
}

TEST_CASE("Wipe tower footprint includes the fixed brim offset", "[WipeTower]")
{
    const WipeTowerFootprint footprint = make_wipe_tower_footprint_with_brim(
        rectangle(20.f, 10.f), 12.f, 0.6f,
        WipeTowerFootprint::Accuracy::Estimated, 7.f);

    REQUIRE_FALSE(footprint.empty());
    CHECK(footprint.accuracy == WipeTowerFootprint::Accuracy::Estimated);
    CHECK_THAT(footprint.height, WithinAbs(12.f, 1e-5f));
    CHECK_THAT(footprint.brim_width, WithinAbs(0.6f, 1e-5f));
    CHECK_THAT(footprint.width, WithinAbs(21.2f, 1e-5f));
    CHECK_THAT(footprint.depth, WithinAbs(11.2f, 1e-5f));
    CHECK_THAT(footprint.planned_depth, WithinAbs(7.f, 1e-5f));
    CHECK_THAT(footprint.bbox.min.x(), WithinAbs(-0.6, 1e-5));
    CHECK_THAT(footprint.bbox.min.y(), WithinAbs(-0.6, 1e-5));
    CHECK_THAT(footprint.bbox.max.x(), WithinAbs(20.6, 1e-5));
    CHECK_THAT(footprint.bbox.max.y(), WithinAbs(10.6, 1e-5));
}

TEST_CASE("Wipe tower post-rotation offset is baked into local footprint coordinates", "[WipeTower]")
{
    constexpr double rotation = 0.5 * M_PI;
    const Vec2f post_rotation_offset(5.f, 3.f);
    const Vec2f local_offset = local_wipe_tower_offset(post_rotation_offset, rotation);
    CHECK_THAT(local_offset.x(), WithinAbs(3.f, 1e-5f));
    CHECK_THAT(local_offset.y(), WithinAbs(-5.f, 1e-5f));

    Polygon outline = rectangle(20.f, 10.f);
    outline.translate(Point::new_scale(-5.f, -3.f));
    outline.translate(Point::new_scale(local_offset));
    const WipeTowerFootprint footprint = finalize_wipe_tower_footprint(
        outline, outline, 5.f, 0.f, WipeTowerFootprint::Accuracy::Exact, 10.f);

    const Polygon transformed =
        transformed_wipe_tower_outline(footprint, rotation, Vec2d(100., 100.));
    const BoundingBox bbox = get_extents(transformed);
    CHECK_THAT(unscale<double>(bbox.min.x()), WithinAbs(98., 1e-5));
    CHECK_THAT(unscale<double>(bbox.min.y()), WithinAbs(98., 1e-5));
    CHECK_THAT(unscale<double>(bbox.max.x()), WithinAbs(108., 1e-5));
    CHECK_THAT(unscale<double>(bbox.max.y()), WithinAbs(118., 1e-5));
}

TEST_CASE("Wipe tower footprint rotation and clamping preserve its margin", "[WipeTower]")
{
    const WipeTowerFootprint footprint = finalize_wipe_tower_footprint(
        rectangle(20.f, 10.f), rectangle(20.f, 10.f), 5.f, 0.f,
        WipeTowerFootprint::Accuracy::Exact);
    const BoundingBoxf rotated = rotated_wipe_tower_bbox(footprint, 0.5 * M_PI);

    CHECK_THAT(rotated.min.x(), WithinAbs(-10., 1e-5));
    CHECK_THAT(rotated.min.y(), WithinAbs(0., 1e-5));
    CHECK_THAT(rotated.max.x(), WithinAbs(0., 1e-5));
    CHECK_THAT(rotated.max.y(), WithinAbs(20., 1e-5));

    const Vec2d clamped = clamp_wipe_tower_position(
        rotated, BoundingBoxf(Vec2d::Zero(), Vec2d(100., 100.)), Vec2d(-50., 200.), 2.);
    CHECK_THAT(clamped.x(), WithinAbs(12., 1e-5));
    CHECK_THAT(clamped.y(), WithinAbs(78., 1e-5));

    const Polygon transformed =
        transformed_wipe_tower_outline(footprint, 0.5 * M_PI, Vec2d(12., 78.));
    const BoundingBox transformed_bbox = get_extents(transformed);
    CHECK_THAT(unscale<double>(transformed_bbox.min.x()), WithinAbs(2., 1e-5));
    CHECK_THAT(unscale<double>(transformed_bbox.min.y()), WithinAbs(78., 1e-5));
    CHECK_THAT(unscale<double>(transformed_bbox.max.x()), WithinAbs(12., 1e-5));
    CHECK_THAT(unscale<double>(transformed_bbox.max.y()), WithinAbs(98., 1e-5));
}

TEST_CASE("Type1 conservative footprint uses per-filament change volume", "[WipeTower]")
{
    PrintConfig config;
    config.filament_type.values = {"PLA", "PLA"};
    config.filament_map.values = {1, 1};
    config.filament_nozzle_map.values = {1, 1};
    config.filament_adhesiveness_category.values = {0, 0};
    config.filament_change_length.values = {0., 0.};
    config.prime_tower_width.value = 40.;
    config.prime_tower_brim_width.value = 0.;
    config.prime_volume.value = 10.;
    config.filament_prime_volume.values = {10., 10.};
    config.filament_prime_volume_nc.values = {10., 10.};

    const WipeTowerFootprint small =
        WipeTower::make_conservative_footprint(config, {0, 1}, 1.f, 0.2f, false);

    config.filament_prime_volume.values[1] = 1000.;
    const WipeTowerFootprint large =
        WipeTower::make_conservative_footprint(config, {0, 1}, 1.f, 0.2f, false);

    REQUIRE_FALSE(small.empty());
    REQUIRE_FALSE(large.empty());
    CHECK(large.depth > small.depth);
}

TEST_CASE("Type1 conservative footprint uses nozzle-change prime volume", "[WipeTower]")
{
    PrintConfig config;
    config.filament_type.values = {"PLA", "PLA"};
    config.filament_map.values = {1, 1};
    config.filament_nozzle_map.values = {1, 1};
    config.filament_adhesiveness_category.values = {0, 0};
    config.filament_change_length.values = {500., 500.};
    config.prime_tower_width.value = 40.;
    config.prime_tower_brim_width.value = 0.;
    config.prime_volume.value = 10.;
    config.filament_prime_volume.values = {10., 10.};
    config.filament_prime_volume_nc.values = {10., 10.};

    const WipeTowerFootprint small =
        WipeTower::make_conservative_footprint(config, {0, 1}, 1.f, 0.2f, false);

    config.filament_prime_volume_nc.values[1] = 1000.;
    const WipeTowerFootprint large =
        WipeTower::make_conservative_footprint(config, {0, 1}, 1.f, 0.2f, false);

    REQUIRE_FALSE(small.empty());
    REQUIRE_FALSE(large.empty());
    CHECK(large.depth > small.depth);
}
