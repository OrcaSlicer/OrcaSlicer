#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

DynamicPrintConfig two_extruder_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.4 });
    config.set_key_value("extruder_offset", new ConfigOptionPoints{ Vec2d(0.0, 0.0), Vec2d(20.0, 5.0) });
    config.set_key_value("master_extruder_id", new ConfigOptionInt(1));
    config.set_key_value("printable_height", new ConfigOptionFloat(200.0));
    config.set_deserialize_strict("bed_exclude_area", "10..30;0x0,10x0,10x10,0x10");
    return config;
}

BoundingBox region_bounds(const BedExcludeRegion &region)
{
    return get_extents(region.polygon);
}

} // namespace

TEST_CASE("Shared exclusion volumes resolve identically for every extruder", "[PrintConfig][BedExclude]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::Shared));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 1);
    REQUIRE(groups[1].size() == 1);
    CHECK(groups[0][0].polygon.points == groups[1][0].polygon.points);
    CHECK(groups[0][0].z_min == Catch::Approx(10.0));
    CHECK(groups[0][0].z_max == Catch::Approx(30.0));

    // The global compatibility view returns a shared definition only once.
    CHECK(get_bed_excluded_regions(config).size() == 1);
}

TEST_CASE("Toolhead-relative exclusion volumes follow nozzle offset deltas", "[PrintConfig][BedExclude]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::ToolheadOffset));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 1);
    REQUIRE(groups[1].size() == 1);

    const BoundingBox reference = region_bounds(groups[0][0]);
    const BoundingBox offset = region_bounds(groups[1][0]);
    CHECK(unscale<double>(offset.min.x() - reference.min.x()) == Catch::Approx(20.0));
    CHECK(unscale<double>(offset.min.y() - reference.min.y()) == Catch::Approx(5.0));
    CHECK(groups[1][0].z_min == Catch::Approx(groups[0][0].z_min));
    CHECK(groups[1][0].z_max == Catch::Approx(groups[0][0].z_max));
}

TEST_CASE("Individual exclusion volumes are authoritative per extruder", "[PrintConfig][BedExclude]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::PerExtruder));
    config.set_key_value("extruder_bed_exclude_area", new ConfigOptionStrings{
        "0..25;0x0,8x0,8x8,0x8|40..60;20x20,30x20,30x30,20x30",
        ""
    });

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].size() == 2);
    CHECK(groups[1].empty());
    CHECK(groups[0][0].z_min == Catch::Approx(0.0));
    CHECK(groups[0][0].z_max == Catch::Approx(25.0));
    CHECK(groups[0][1].z_min == Catch::Approx(40.0));
    CHECK(groups[0][1].z_max == Catch::Approx(60.0));
}

TEST_CASE("Offset mode honours a non-zero reference extruder", "[PrintConfig][BedExclude]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("master_extruder_id", new ConfigOptionInt(2));
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::ToolheadOffset));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    const BoundingBox first = region_bounds(groups[0][0]);
    const BoundingBox reference = region_bounds(groups[1][0]);
    CHECK(unscale<double>(first.min.x() - reference.min.x()) == Catch::Approx(-20.0));
    CHECK(unscale<double>(first.min.y() - reference.min.y()) == Catch::Approx(-5.0));
}
