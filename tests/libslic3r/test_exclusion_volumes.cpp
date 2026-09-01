#include <catch2/catch_all.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExclusionVolumeGeometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
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
    return {rectangle(min_x, min_y, max_x, max_y), z_min, z_max, true, has_z_range};
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
    config.set_deserialize_strict("bed_exclude_area", "10..30;0x0,10x0,10x10,0x10");
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

    explicit CubeModel(const Vec3d &offset = Vec3d::Zero())
    {
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20.0, 20.0, 20.0));
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
    CHECK_FALSE(has_bed_exclusion_volume_syntax(option));
}

TEST_CASE("Extended exclusion definitions retain their serialized representation", "[ExclusionVolume][PrintConfig]")
{
    const std::string definition = "0..10;0x0,10x0,10x10,0x10|20x20,30x20,30x30,20x30";
    ConfigOptionPoints option;
    REQUIRE(option.deserialize(definition));
    CHECK(option.values.empty());
    CHECK(option.serialize() == definition);
    CHECK(option.vserialize() == std::vector<std::string>{definition});
    CHECK(has_bed_exclusion_volume_syntax(option));

    std::unique_ptr<ConfigOption> cloned(option.clone());
    REQUIRE(cloned != nullptr);
    CHECK(*cloned == option);
    CHECK(cloned->hash() == option.hash());

    ConfigOptionPoints assigned;
    assigned.set(&option);
    CHECK(assigned == option);
}

TEST_CASE("Exclusion syntax validation accepts supported forms and rejects malformed regions", "[ExclusionVolume][PrintConfig]")
{
    const auto [definition, valid] = GENERATE(table<std::string, bool>({
        {"", true},
        {"0x0,10x0,10x10,0x10", true},
        {"0..10;0x0,10x0,10x10,0x10", true},
        {"..10;0x0,10x0,10x10,0x10", true},
        {"10..;0x0,10x0,10x10,0x10", true},
        {"0x0,10x0,10x10,0x10|20x20,30x20,30x30,20x30", true},
        {"20..10;0x0,10x0,10x10,0x10", false},
        {"zero..10;0x0,10x0,10x10,0x10", false},
        {"0..10;0x0,10x0", false},
        {"0..10;", false},
        {"|", false},
    }));

    DYNAMIC_SECTION(definition) {
        CHECK(is_valid_bed_exclude_area_string(definition, 200.0) == valid);
    }
}

TEST_CASE("Extended exclusion Z ranges are defaulted and clamped to printable height", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_height", new ConfigOptionFloat(100.0));
    config.set_deserialize_strict(
        "bed_exclude_area",
        "-20..25;0x0,10x0,10x10,0x10|75..150;20x0,30x0,30x10,20x10|..;40x0,50x0,50x10,40x10");

    const std::vector<BedExcludeRegion> regions = get_bed_excluded_regions(config);
    REQUIRE(regions.size() == 3);
    CHECK_THAT(regions[0].z_min, WithinAbs(0.0, 1e-9));
    CHECK_THAT(regions[0].z_max, WithinAbs(25.0, 1e-9));
    CHECK_THAT(regions[1].z_min, WithinAbs(75.0, 1e-9));
    CHECK_THAT(regions[1].z_max, WithinAbs(100.0, 1e-9));
    CHECK_THAT(regions[2].z_min, WithinAbs(0.0, 1e-9));
    CHECK_THAT(regions[2].z_max, WithinAbs(100.0, 1e-9));
}

TEST_CASE("Shared exclusion volumes resolve identically for every extruder", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::Shared));

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
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::ToolheadOffset));

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].size() == 1);
    REQUIRE(groups[1].size() == 1);
    const BoundingBox reference = bounds(groups[0][0]);
    const BoundingBox shifted   = bounds(groups[1][0]);
    CHECK_THAT(unscaled_x(shifted.min.x() - reference.min.x()), WithinAbs(20.0, 1e-6));
    CHECK_THAT(unscaled_x(shifted.min.y() - reference.min.y()), WithinAbs(5.0, 1e-6));
}

TEST_CASE("Toolhead-relative exclusions honour a non-default reference extruder", "[ExclusionVolume][PrintConfig][MultiNozzle]")
{
    DynamicPrintConfig config = two_extruder_config();
    config.set_key_value("master_extruder_id", new ConfigOptionInt(2));
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::ToolheadOffset));

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
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(BedExcludeAreaMode::PerExtruder));
    config.set_key_value("extruder_bed_exclude_area", new ConfigOptionStrings{
        "0..25;0x0,8x0,8x8,0x8|40..60;20x20,30x20,30x30,20x30",
        "",
    });

    const auto groups = get_bed_excluded_regions_by_extruder(config);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].size() == 2);
    CHECK(groups[1].empty());
    CHECK(get_bed_excluded_regions(config).size() == 2);
}

TEST_CASE("Legacy bed helpers include only regions touching the first layer", "[ExclusionVolume][PrintConfig]")
{
    DynamicPrintConfig dynamic = DynamicPrintConfig::full_print_config();
    dynamic.set_key_value("printable_height", new ConfigOptionFloat(100.0));
    dynamic.set_deserialize_strict(
        "bed_exclude_area",
        "0..5;0x0,10x0,10x10,0x10|20..30;20x0,30x0,30x10,20x10");
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

    // Cross the cube's outer surface rather than placing the prism wholly
    // inside the solid; the preview is built from clipped surface triangles.
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
