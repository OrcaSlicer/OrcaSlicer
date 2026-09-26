#include <catch2/catch_all.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExclusionVolumeGeometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

Polygon rectangle(double min_x, double min_y, double max_x, double max_y)
{
    Polygon polygon({
        Point::new_scale(min_x, min_y),
        Point::new_scale(max_x, min_y),
        Point::new_scale(max_x, max_y),
        Point::new_scale(min_x, max_y),
    });
    polygon.make_counter_clockwise();
    return polygon;
}

BedExcludeRegion region(double min_x, double min_y, double max_x, double max_y,
                        double z_min, double z_max, bool has_z_range = true)
{
    return {rectangle(min_x, min_y, max_x, max_y), z_min, z_max,
            BedExcludeRegion::Purpose::CollisionVolume, has_z_range};
}

DynamicPrintConfig two_extruder_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.6});
    config.set_key_value("extruder_offset", new ConfigOptionPoints{Vec2d(0.0, 0.0), Vec2d(20.0, 5.0)});
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable{200.0, 200.0});
    config.set_key_value("master_extruder_id", new ConfigOptionInt(1));
    config.set_key_value("printable_height", new ConfigOptionFloat(200.0));
    config.set_key_value("bed_exclude_volumes", new ConfigOptionString("10..30;0x0,10x0,10x10,0x10"));
    return config;
}

BoundingBox bounds(const BedExcludeRegion &region)
{
    return get_extents(region.polygon);
}

double unscaled_x(coord_t value)
{
    return unscale<double>(value);
}

bool contains(const ExPolygons &polygons, double x, double y)
{
    const Point point = Point::new_scale(x, y);
    return std::any_of(polygons.begin(), polygons.end(), [&](const ExPolygon &polygon) {
        return polygon.contains(point);
    });
}

struct CubeModel
{
    Model model;
    ModelInstance *instance {nullptr};

    explicit CubeModel(const Vec3d &offset = Vec3d::Zero(), const Vec3d &size = Vec3d(20.0, 20.0, 20.0))
    {
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(size.x(), size.y(), size.z()));
        instance = object->add_instance();
        instance->set_offset(offset);
    }
};

} // namespace

TEST_CASE("Legacy exclusion polygons keep their point representation", "[ExclusionVolume][PrintConfig]")
{
    ConfigOptionPoints option;
    REQUIRE(option.deserialize("0x0,10x0,10x10,0x10"));
    REQUIRE(option.values.size() == 4);
    CHECK(option.serialize() == "0x0,10x0,10x10,0x10");
    CHECK_FALSE(is_bed_exclusion_volume_syntax(option.serialize()));
}

TEST_CASE("Legacy point options reject collision-volume syntax", "[ExclusionVolume][PrintConfig]")
{
    const std::string definition = "0..10;0x0,10x0,10x10,0x10|20x20,30x20,30x30,20x30";
    ConfigOptionPoints option;
    CHECK_FALSE(option.deserialize(definition));
    CHECK(option.values.empty());
    CHECK(is_bed_exclusion_volume_syntax(definition));
}

TEST_CASE("Legacy rectangle groups convert into separate collision volumes", "[ExclusionVolume][PrintConfig]")
{
    ConfigOptionPoints legacy;
    REQUIRE(legacy.deserialize("0x0,10x0,10x10,0x10,20x20,30x20,30x30,20x30"));
    CHECK(legacy_bed_exclude_area_to_volumes(legacy.values) ==
        "0x0,10x0,10x10,0x10|20x20,30x20,30x30,20x30");

    legacy.values.pop_back();
    CHECK(legacy_bed_exclude_area_to_volumes(legacy.values).empty());
}

TEST_CASE("Converted volumes preserve existing definitions and nozzle mode", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    const std::string converted = "40x40,50x40,50x50,40x50";

    SECTION("toolhead-relative definitions remain shared") {
        DynamicPrintConfig config = two_extruder_config();
        config.set_key_value("bed_exclude_volume_mode",
            new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::ToolheadOffset));
        append_bed_exclude_volumes(config, converted);

        CHECK(config.opt_enum<BedExcludeVolumeMode>("bed_exclude_volume_mode") == BedExcludeVolumeMode::ToolheadOffset);
        CHECK(config.opt_string("bed_exclude_volumes") ==
            "10..30;0x0,10x0,10x10,0x10|40x40,50x40,50x50,40x50");
    }

    SECTION("individual definitions receive the former shared keep-out") {
        DynamicPrintConfig config = two_extruder_config();
        config.set_key_value("bed_exclude_volume_mode",
            new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::PerExtruder));
        config.set_key_value("extruder_bed_exclude_volumes", new ConfigOptionStrings{"", "60x60,70x60,70x70,60x70"});
        append_bed_exclude_volumes(config, converted);

        CHECK(config.opt_enum<BedExcludeVolumeMode>("bed_exclude_volume_mode") == BedExcludeVolumeMode::PerExtruder);
        const ConfigOptionStrings *per_extruder = config.option<ConfigOptionStrings>("extruder_bed_exclude_volumes");
        REQUIRE(per_extruder != nullptr);
        REQUIRE(per_extruder->values.size() == 2);
        CHECK(per_extruder->values[0] == converted);
        CHECK(per_extruder->values[1] == "60x60,70x60,70x70,60x70|40x40,50x40,50x50,40x50");
    }
}

TEST_CASE("Exclusion syntax validation accepts supported forms and rejects malformed regions", "[ExclusionVolume][PrintConfig]")
{
    const auto [definition, valid] = GENERATE(table<std::string, bool>({
        {"", true},
        {"0x0,10x0,10x10,0x10", true},
        {"0..10;0x0,10x0,10x10,0x10", true},
        {"0.5..10.5;0x0,10x0,10x10,0x10", true},
        {"..10;0x0,10x0,10x10,0x10", true},
        {"10..;0x0,10x0,10x10,0x10", true},
        {"0x0,10x0,10x10,0x10|20x20,30x20,30x30,20x30", true},
        {"20..10;0x0,10x0,10x10,0x10", false},
        {"10..10;0x0,10x0,10x10,0x10", false},
        {"300..400;0x0,10x0,10x10,0x10", false},
        {"zero..10;0x0,10x0,10x10,0x10", false},
        {"0..10;0x0,10x0", false},
        {"0..10;", false},
        {"|", false},
    }));

    DYNAMIC_SECTION(definition) {
        CHECK(is_valid_bed_exclude_volumes_string(definition, 200.0) == valid);
    }
}

TEST_CASE("Extended exclusion Z ranges are defaulted and clamped to printable height", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_height", new ConfigOptionFloat(100.0));
    config.set_key_value("bed_exclude_volumes", new ConfigOptionString(
        "-20..25;0x0,10x0,10x10,0x10|75..150;20x0,30x0,30x10,20x10|..;40x0,50x0,50x10,40x10"));

    const std::vector<BedExcludeRegion> regions = get_bed_excluded_regions(config);
    REQUIRE(regions.size() == 3);
    CHECK_THAT(regions[0].z_min, WithinAbs(0.0, 1e-9));
    CHECK_THAT(regions[0].z_max, WithinAbs(25.0, 1e-9));
    CHECK_THAT(regions[1].z_min, WithinAbs(75.0, 1e-9));
    CHECK_THAT(regions[1].z_max, WithinAbs(100.0, 1e-9));
    CHECK_THAT(regions[2].z_min, WithinAbs(0.0, 1e-9));
    CHECK_THAT(regions[2].z_max, WithinAbs(100.0, 1e-9));
}

TEST_CASE("CLI keeps legacy areas and collision volumes on separate options", "[ExclusionVolume][PrintConfig][CLI]")
{
    const char *legacy_area_argv[] = {
        "orca-slicer", "--bed-exclude-area", "0x0,10x0,10x10,0x10"
    };
    DynamicPrintAndCLIConfig legacy_area_config;
    t_config_option_keys extra;
    t_config_option_keys keys;
    REQUIRE(legacy_area_config.read_cli(3, legacy_area_argv, &extra, &keys));
    REQUIRE(legacy_area_config.opt<ConfigOptionPoints>("bed_exclude_area") != nullptr);
    CHECK(legacy_area_config.opt<ConfigOptionPoints>("bed_exclude_area")->values.size() == 4);

    const char *volume_argv[] = {
        "orca-slicer", "--bed-exclude-volumes", "0..10;0x0,10x0,10x10,0x10"
    };
    DynamicPrintAndCLIConfig volume_config;
    extra.clear();
    keys.clear();
    REQUIRE(volume_config.read_cli(3, volume_argv, &extra, &keys));
    CHECK(volume_config.opt_string("bed_exclude_volumes") == volume_argv[2]);

    const char *legacy_argv[] = {
        "orca-slicer", "--bed-exclude-area", "0..10;0x0,10x0,10x10,0x10"
    };
    DynamicPrintAndCLIConfig legacy_config;
    extra.clear();
    keys.clear();
    CHECK_FALSE(legacy_config.read_cli(3, legacy_argv, &extra, &keys));
}

TEST_CASE("Legacy areas and collision volumes remain additive", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_height", new ConfigOptionFloat(100.0));
    config.set_deserialize_strict("bed_exclude_area", "0x0,10x0,10x10,0x10");

    auto regions = get_bed_excluded_regions(config);
    REQUIRE(regions.size() == 1);
    CHECK_FALSE(regions.front().is_collision_volume());
    CHECK_FALSE(has_bed_exclude_volumes(config));

    config.set_key_value("bed_exclude_volumes", new ConfigOptionString("20x20,30x20,30x30,20x30"));
    regions = get_bed_excluded_regions(config);
    REQUIRE(regions.size() == 2);
    CHECK_FALSE(regions[0].is_collision_volume());
    CHECK(regions[0].polygon.contains(Point::new_scale(5.0, 5.0)));
    CHECK(regions[1].is_collision_volume());
    CHECK(regions[1].polygon.contains(Point::new_scale(25.0, 25.0)));
}

TEST_CASE("An invalid collision-volume definition does not suppress the legacy area", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("bed_exclude_area", "0x0,10x0,10x10,0x10");
    config.set_key_value("bed_exclude_volumes", new ConfigOptionString("not a polygon"));

    CHECK(has_bed_exclude_volumes(config));
    const auto regions = get_bed_excluded_regions(config);
    REQUIRE(regions.size() == 1);
    CHECK_FALSE(regions.front().is_collision_volume());
    CHECK_FALSE(is_valid_bed_exclude_volumes_string(config.opt_string("bed_exclude_volumes"), 100.0));
}

TEST_CASE("An empty collision-volume configuration falls back to the legacy area", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_deserialize_strict("bed_exclude_area", "0x0,10x0,10x10,0x10");
    config.set_key_value("bed_exclude_volume_mode",
        new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::PerExtruder));
    config.set_key_value("extruder_bed_exclude_volumes", new ConfigOptionStrings{"", ""});

    CHECK_FALSE(has_bed_exclude_volumes(config));
    CHECK(active_bed_exclude_volume_mode(config) == BedExcludeVolumeMode::Shared);
    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 1);
    REQUIRE(groups[1].size() == 1);
    CHECK_FALSE(groups[0][0].is_collision_volume());
}

TEST_CASE("Shared exclusion volumes resolve identically for every extruder", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::Shared));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 1);
    REQUIRE(groups[1].size() == 1);
    CHECK(groups[0][0].polygon.points == groups[1][0].polygon.points);
    CHECK_THAT(groups[0][0].z_min, WithinAbs(10.0, 1e-9));
    CHECK_THAT(groups[0][0].z_max, WithinAbs(30.0, 1e-9));

    // A shared compatibility view contains one definition, not one duplicate per nozzle.
    CHECK(get_bed_excluded_regions(config).size() == 1);
}

TEST_CASE("Toolhead-relative exclusion volumes follow nozzle offset deltas", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::ToolheadOffset));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 1);
    REQUIRE(groups[1].size() == 1);
    const BoundingBox reference = bounds(groups[0][0]);
    const BoundingBox shifted   = bounds(groups[1][0]);
    CHECK_THAT(unscaled_x(shifted.min.x() - reference.min.x()), WithinAbs(20.0, 1e-6));
    CHECK_THAT(unscaled_x(shifted.min.y() - reference.min.y()), WithinAbs(5.0, 1e-6));
}

TEST_CASE("Toolhead-relative mode offsets collision volumes but not the shared legacy area", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_deserialize_strict("bed_exclude_area", "70x70,80x70,80x80,70x80");
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::ToolheadOffset));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 2);
    REQUIRE(groups[1].size() == 2);
    CHECK_FALSE(groups[0][0].is_collision_volume());
    CHECK_FALSE(groups[1][0].is_collision_volume());
    CHECK(groups[0][0].polygon.points == groups[1][0].polygon.points);
    CHECK(groups[0][1].polygon.points != groups[1][1].polygon.points);

    const auto flattened = get_bed_excluded_regions(config);
    CHECK(flattened.size() == 3);
    CHECK(std::count_if(flattened.begin(), flattened.end(),
        [](const BedExcludeRegion &region) { return !region.is_collision_volume(); }) == 1);
}

TEST_CASE("Toolhead-relative exclusions honour a non-default reference extruder", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("master_extruder_id", new ConfigOptionInt(2));
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::ToolheadOffset));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    const BoundingBox first     = bounds(groups[0][0]);
    const BoundingBox reference = bounds(groups[1][0]);
    CHECK_THAT(unscaled_x(first.min.x() - reference.min.x()), WithinAbs(-20.0, 1e-6));
    CHECK_THAT(unscaled_x(first.min.y() - reference.min.y()), WithinAbs(-5.0, 1e-6));
}

TEST_CASE("Individual exclusion volumes remain authoritative per extruder", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::PerExtruder));
    config.set_key_value("extruder_bed_exclude_volumes", new ConfigOptionStrings{
        "0..25;0x0,8x0,8x8,0x8|40..60;20x20,30x20,30x30,20x30",
        "",
    });

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].size() == 2);
    CHECK(groups[1].empty());
    CHECK(get_bed_excluded_regions(config).size() == 2);
}

TEST_CASE("Individual collision volumes retain the shared legacy area", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_deserialize_strict("bed_exclude_area", "70x70,80x70,80x80,70x80");
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::PerExtruder));
    config.set_key_value("extruder_bed_exclude_volumes", new ConfigOptionStrings{
        "0..25;0x0,8x0,8x8,0x8", "0..25;20x0,28x0,28x8,20x8"
    });

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 2);
    REQUIRE(groups[1].size() == 2);
    CHECK_FALSE(groups[0][0].is_collision_volume());
    CHECK(groups[0][0].polygon.points == groups[1][0].polygon.points);
    CHECK(groups[0][1].is_collision_volume());
    CHECK(groups[1][1].is_collision_volume());
}

TEST_CASE("Individual exclusion validation accepts omitted trailing extruders", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig dynamic = two_extruder_config();
    dynamic.set_key_value("bed_exclude_volume_mode",
        new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::PerExtruder));
    dynamic.set_key_value("extruder_bed_exclude_volumes",
        new ConfigOptionStrings{"0..25;0x0,8x0,8x8,0x8"});

    FullPrintConfig config;
    config.apply(dynamic, true);
    const std::map<std::string, std::string> errors = validate(config);
    CHECK(errors.find("extruder_bed_exclude_volumes") == errors.end());

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].size() == 1);
    CHECK(groups[1].empty());
}

TEST_CASE("Legacy bed helpers include only regions touching the first layer", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig dynamic = DynamicPrintConfig::full_print_config();
    dynamic.set_key_value("printable_height", new ConfigOptionFloat(100.0));
    dynamic.set_key_value("bed_exclude_volumes", new ConfigOptionString(
        "0..5;0x0,10x0,10x10,0x10|20..30;20x0,30x0,30x10,20x10"));
    PrintConfig config;
    config.apply(dynamic, true);

    const Polygons bed_regions = get_bed_excluded_area(config);
    REQUIRE(bed_regions.size() == 1);
    CHECK(bed_regions.front().contains(Point::new_scale(5.0, 5.0)));
    CHECK_FALSE(bed_regions.front().contains(Point::new_scale(25.0, 5.0)));
}

TEST_CASE("Filament mapping resolves physical exclusion extruders conservatively", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    CHECK(bed_exclusion_extruder_for_filament(0, {2, 1}, fmmManual, false, true, 2) == 1);
    CHECK(bed_exclusion_extruder_for_filament(1, {2, 1}, fmmManual, false, true, 2) == 0);

    // Generic automatic multi-tool printers use matching logical and physical ids.
    CHECK(bed_exclusion_extruder_for_filament(1, {1, 1}, fmmAutoForMatch, false, false, 2) == 1);

    // Until Bambu automatic grouping has produced a concrete mapping, every nozzle remains possible.
    CHECK(bed_exclusion_extruder_for_filament(0, {1}, fmmAutoForMatch, true, false, 2) == -1);
    CHECK(bed_exclusion_extruder_for_filament(0, {2}, fmmAutoForMatch, true, true, 2) == 1);
    CHECK(bed_exclusion_extruder_for_filament(3, {}, fmmManual, false, true, 2) == -1);
}

TEST_CASE("Exclusion Z overlap treats touching slabs conservatively", "[ExclusionVolume][Geometry]")
{
    CHECK(bed_exclusion_z_ranges_overlap(0.0, 10.0, 10.0, 20.0));
    CHECK(bed_exclusion_z_ranges_overlap(10.0, 0.0, 20.0, 10.0));
    CHECK_FALSE(bed_exclusion_z_ranges_overlap(0.0, 9.0, 10.0, 20.0));
}

TEST_CASE("Arrange keeps convex exclusions exact and bounds concave exclusions", "[ExclusionVolume][Geometry][Arrange]")
{
    const Polygon convex = rectangle(0.0, 0.0, 10.0, 10.0);
    CHECK(bed_exclusion_arrange_polygon(convex).points == convex.points);

    Polygon concave({
        Point::new_scale(0.0, 0.0), Point::new_scale(10.0, 0.0),
        Point::new_scale(10.0, 4.0), Point::new_scale(4.0, 4.0),
        Point::new_scale(4.0, 10.0), Point::new_scale(0.0, 10.0),
    });
    concave.make_counter_clockwise();
    REQUIRE_FALSE(concave.contains(Point::new_scale(8.0, 8.0)));

    const Polygon blocker = bed_exclusion_arrange_polygon(concave);
    CHECK(blocker.points.size() == 4);
    CHECK(blocker.contains(Point::new_scale(8.0, 8.0)));
}

TEST_CASE("Active exclusion footprints select Z range nozzle translation and clearance", "[ExclusionVolume][Geometry][MultiNozzle]")
{
    const std::vector<std::vector<BedExcludeRegion>> regions_by_extruder{
        {region(0.0, 0.0, 10.0, 10.0, 0.0, 5.0)},
        {region(20.0, 0.0, 30.0, 10.0, 10.0, 20.0)},
    };

    ExPolygons active = active_bed_exclusion_footprints(
        regions_by_extruder, {1, 1, 99}, 12.0, 13.0, Point::new_scale(5.0, 2.0), scale_(1.0));
    REQUIRE(active.size() == 1);
    CHECK(contains(active, 26.0, 3.0));
    CHECK(contains(active, 35.5, 3.0));
    CHECK_FALSE(contains(active, 5.0, 5.0));

    CHECK(active_bed_exclusion_footprints(regions_by_extruder, {0}, 6.0, 9.0).empty());
}

TEST_CASE("Exact model intersection respects XY Z and preview collection", "[ExclusionVolume][Model]")
{
    CubeModel cube(Vec3d(30.0, 40.0, 0.0));

    // Cross the cube's outer surface so this case can also verify the red
    // preview, which is built from clipped surface triangles. The enclosed
    // prism topology is covered separately below.
    const BedExcludeRegion crossing = region(45.0, 45.0, 55.0, 55.0, 5.0, 15.0);
    const BedExcludeRegion above    = region(45.0, 45.0, 55.0, 55.0, 25.0, 30.0);
    const BedExcludeRegion outside  = region(60.0, 70.0, 70.0, 80.0, 0.0, 20.0);

    CHECK(cube.instance->intersects_bed_exclude_region(crossing));
    CHECK_FALSE(cube.instance->intersects_bed_exclude_region(above));
    CHECK_FALSE(cube.instance->intersects_bed_exclude_region(outside));

    indexed_triangle_set preview;
    CHECK(cube.instance->intersects_bed_exclude_region(crossing, &preview));
    CHECK_FALSE(preview.empty());
    CHECK_FALSE(cube.instance->intersects_bed_exclude_region(above, &preview));
    CHECK(preview.empty());
}

TEST_CASE("Exact model intersection follows instance and volume transforms", "[ExclusionVolume][Model]")
{
    CubeModel cube(Vec3d(50.0, 50.0, 0.0));
    cube.instance->set_rotation(Vec3d(0.0, 0.0, PI / 4.0));
    cube.instance->set_scaling_factor(Vec3d(1.5, 0.5, 1.0));

    CHECK(cube.instance->intersects_bed_exclude_region(region(50.0, 50.0, 55.0, 55.0, 0.0, 20.0)));
    CHECK_FALSE(cube.instance->intersects_bed_exclude_region(region(100.0, 100.0, 110.0, 110.0, 0.0, 20.0)));
}

TEST_CASE("Exact model intersection detects an exclusion prism enclosed by a solid", "[ExclusionVolume][Model]")
{
    // A surface-only test misses this topology: the exclusion prism is fully
    // enclosed, so none of the block's surface triangles enters the prism.
    CubeModel block(Vec3d(200.0, 300.0, 0.0), Vec3d(100.0, 100.0, 60.0));
    const BedExcludeRegion enclosed  = region(230.0, 330.0, 260.0, 358.0, 8.0, 22.0);
    const BedExcludeRegion enclosing = region(190.0, 290.0, 310.0, 410.0, -5.0, 65.0);
    const BedExcludeRegion above     = region(230.0, 330.0, 260.0, 358.0, 65.0, 75.0);

    CHECK(block.instance->intersects_bed_exclude_region(enclosed));
    CHECK_FALSE(block.instance->intersects_bed_exclude_region(above));

    indexed_triangle_set preview;
    CHECK(block.instance->intersects_bed_exclude_region(enclosed, &preview));
    // There is no model surface inside a fully enclosed prism from which to
    // build the usual red clipped-surface preview.
    CHECK(preview.empty());

    // Pin the opposite containment topology too. Here the model surface lies
    // inside the prism, so the existing clipped preview remains available.
    CHECK(block.instance->intersects_bed_exclude_region(enclosing, &preview));
    CHECK_FALSE(preview.empty());

    // The multi-region wrapper must preserve the collision result even when
    // that optional surface preview is empty.
    CHECK(block.instance->intersects_bed_exclude_regions({above, enclosed}, &preview));
    CHECK(preview.empty());
}
