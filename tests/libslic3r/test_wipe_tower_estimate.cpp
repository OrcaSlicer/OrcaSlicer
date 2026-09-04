#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Rectangle wall, one nozzle, 100 mm3 prime volume on a 50 mm wide tower at 0.2 mm layers: one
// purge is 10 mm of depth. The flush matrix is off here; the shipped-default case covers it.
static DynamicPrintConfig make_config(WipeTowerWallType wall_type = WipeTowerWallType::wtwRectangle)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("prime_tower_width", new ConfigOptionFloat(50.));
    config.set_key_value("prime_volume", new ConfigOptionFloat(100.));
    config.set_key_value("prime_tower_infill_gap", new ConfigOptionPercent(100.));
    config.set_key_value("prime_tower_brim_width", new ConfigOptionFloat(3.));
    config.set_key_value("wipe_tower_wall_type", new ConfigOptionEnum<WipeTowerWallType>(wall_type));
    config.set_key_value("wipe_tower_rib_width", new ConfigOptionFloat(8.));
    config.set_key_value("wipe_tower_extra_rib_length", new ConfigOptionFloat(0.));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(TimelapseType::tlTraditional));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));
    config.set_key_value("raft_layers", new ConfigOptionInt(0));
    config.set_key_value("purge_in_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    return config;
}

TEST_CASE("A rectangle wall tower is sized by the purge volume", "[WipeTowerEstimate]") {
    const DynamicPrintConfig config = make_config();
    // Three filaments purge twice per layer; a 5 mm object keeps the stability floor at 5 mm.
    const WipeTowerFootprint fp = estimate_wipe_tower_footprint(config, 3, 0.2, 5., false);
    CHECK_THAT(fp.width, WithinAbs(50., 1e-9));
    CHECK_THAT(fp.depth, WithinAbs(20., 1e-9));
    CHECK_THAT(fp.height, WithinAbs(5., 1e-9));
    CHECK_THAT(fp.brim_width, WithinAbs(3., 1e-9));
    // Thinner layers need more depth for the same volume.
    CHECK_THAT(estimate_wipe_tower_footprint(config, 3, 0.1, 5., false).depth, WithinAbs(40., 1e-9));
    // The infill gap spaces the purge lines.
    DynamicPrintConfig spaced = config;
    spaced.set_key_value("prime_tower_infill_gap", new ConfigOptionPercent(150.));
    CHECK_THAT(estimate_wipe_tower_footprint(spaced, 3, 0.2, 5., false).depth, WithinAbs(30., 1e-9));
}

TEST_CASE("Object height sets the stability floor and the auto brim", "[WipeTowerEstimate]") {
    DynamicPrintConfig config = make_config();
    // Two filaments purge once: 10 mm, lifted to the 20 mm floor of a 100 mm tower.
    CHECK_THAT(estimate_wipe_tower_footprint(config, 2, 0.2, 100., false).depth, WithinAbs(20., 1e-9));
    config.set_key_value("prime_tower_brim_width", new ConfigOptionFloat(-1.));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 2, 0.2, 50., false).brim_width, WithinAbs(WipeTower::get_auto_brim_by_height(50.f), 1e-6));
}

TEST_CASE("A single filament only gets a tower when one is printed anyway", "[WipeTowerEstimate]") {
    DynamicPrintConfig config = make_config();
    CHECK_THAT(estimate_wipe_tower_footprint(config, 1, 0.2, 100., false).depth, WithinAbs(0., 1e-9));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 0, 0.2, 100., false).width, WithinAbs(0., 1e-9));

    // Wrapping detection prints a tower on the first layers whatever the filament count.
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 1, 0.2, 100., false).depth, WithinAbs(20., 1e-9));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    // So does a raft. raft_layers is a per-object key, so it arrives as a resolved flag and
    // is deliberately not read off the config.
    CHECK_THAT(estimate_wipe_tower_footprint(config, 1, 0.2, 100., true).depth, WithinAbs(20., 1e-9));
    config.set_key_value("raft_layers", new ConfigOptionInt(3));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 1, 0.2, 100., false).depth, WithinAbs(0., 1e-9));
    config.set_key_value("raft_layers", new ConfigOptionInt(0));

    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(TimelapseType::tlSmooth));
    // Smooth timelapse primes the single filament once: 10 mm, lifted to the floor.
    CHECK_THAT(estimate_wipe_tower_footprint(config, 1, 0.2, 100., false).depth, WithinAbs(20., 1e-9));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 1, 0.2, 5., false).depth, WithinAbs(10., 1e-9));
}

TEST_CASE("Both wall types agree on whether there is a tower at all", "[WipeTowerEstimate]") {
    // A wall type may only change the shape of the tower, never whether one is reserved:
    // reporting no tower for one that is built collapses the validation hull to a point.
    const double height = GENERATE(5., 100.);
    DynamicPrintConfig rect = make_config();
    DynamicPrintConfig rib  = make_config(WipeTowerWallType::wtwRib);

    // No tool change and nothing else that prints a tower - neither wall type reserves one.
    CHECK_THAT(estimate_wipe_tower_footprint(rect, 1, 0.2, height, false).depth, WithinAbs(0., 1e-9));
    CHECK_THAT(estimate_wipe_tower_footprint(rib, 1, 0.2, height, false).depth, WithinAbs(0., 1e-9));

    // Not even on a dual-nozzle printer, where a lone filament still needs no purge.
    rect.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    rib.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    CHECK_THAT(estimate_wipe_tower_footprint(rect, 1, 0.2, height, false).depth, WithinAbs(0., 1e-9));
    CHECK_THAT(estimate_wipe_tower_footprint(rib, 1, 0.2, height, false).depth, WithinAbs(0., 1e-9));

    // With a tool change both reserve one, and both respect the stability floor.
    CHECK(estimate_wipe_tower_footprint(rect, 2, 0.2, height, false).depth >= WipeTower::get_limit_depth_by_height(float(height)));
    CHECK(estimate_wipe_tower_footprint(rib, 2, 0.2, height, false).depth >= WipeTower::get_limit_depth_by_height(float(height)));
}

TEST_CASE("A rib wall squares the tower and caps the rib width", "[WipeTowerEstimate]") {
    DynamicPrintConfig config = make_config(WipeTowerWallType::wtwRib);
    // sqrt(200 / 0.2) = 31.62 mm square, plus the 8 mm rib bulge along the diagonal.
    const double body = std::sqrt(1000.);
    WipeTowerFootprint fp = estimate_wipe_tower_footprint(config, 3, 0.2, 5., false);
    CHECK_THAT(fp.depth, WithinAbs(8. / std::sqrt(2.) + body, 1e-9));
    CHECK_THAT(fp.width, WithinAbs(fp.depth, 1e-9));
    // The extra rib length grows the footprint.
    config.set_key_value("wipe_tower_extra_rib_length", new ConfigOptionFloat(4.));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 3, 0.2, 5., false).depth, WithinAbs(8. / std::sqrt(2.) + body + 4., 1e-9));
    // A tiny tower caps the rib width at half its depth: 5 mm body, 2.5 mm rib.
    config.set_key_value("wipe_tower_extra_rib_length", new ConfigOptionFloat(0.));
    config.set_key_value("prime_volume", new ConfigOptionFloat(5.));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 2, 0.2, 5., false).depth, WithinAbs(2.5 / std::sqrt(2.) + 5., 1e-9));
}

TEST_CASE("A dual nozzle purges every filament plus the filament change", "[WipeTowerEstimate]") {
    DynamicPrintConfig config = make_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("filament_change_length", new ConfigOptionFloats({10., 10.}));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    // Two purges of 100 mm3 plus one 10 mm filament change: (200 + 10 * pi * 1.75^2 / 4) / (0.2 * 50).
    const double change_volume = 10. * PI * 1.75 * 1.75 / 4.;
    CHECK_THAT(estimate_wipe_tower_footprint(config, 2, 0.2, 5., false).depth, WithinAbs((200. + change_volume) / 10., 1e-9));
}

TEST_CASE("The shipped defaults size the tower from the flush matrix", "[WipeTowerEstimate]") {
    // Both keys default to true, so the shipped configuration purges the flush volumes rather
    // than the prime volume, with no infill gap on top - the flush volumes already hold it.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.opt_bool("purge_in_prime_tower"));
    REQUIRE(config.opt_bool("single_extruder_multi_material"));
    config.set_key_value("prime_tower_width", new ConfigOptionFloat(50.));
    config.set_key_value("wipe_tower_wall_type", new ConfigOptionEnum<WipeTowerWallType>(WipeTowerWallType::wtwRectangle));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));

    const double flush_volume = WipeTower2::estimate_semm_flush_volume(config, 2);
    const double expected     = std::max(double(WipeTower::get_limit_depth_by_height(5.f)), flush_volume / (0.2 * 50.));
    CHECK_THAT(estimate_wipe_tower_footprint(config, 2, 0.2, 5., false).depth, WithinAbs(expected, 1e-6));
}

TEST_CASE("A config missing a tower key falls back to that key's default", "[WipeTowerEstimate]") {
    // The signature takes any ConfigBase: an absent key must read as its declared default.
    const DynamicPrintConfig full = make_config();
    DynamicPrintConfig       partial = full;
    partial.erase("prime_tower_infill_gap");
    REQUIRE(partial.option("prime_tower_infill_gap") == nullptr);

    DynamicPrintConfig defaulted = full;
    defaulted.set_key_value("prime_tower_infill_gap",
                            print_config_def.get("prime_tower_infill_gap")->default_value->clone());
    CHECK_THAT(estimate_wipe_tower_footprint(partial, 3, 0.2, 5., false).depth,
               WithinAbs(estimate_wipe_tower_footprint(defaulted, 3, 0.2, 5., false).depth, 1e-9));
}
