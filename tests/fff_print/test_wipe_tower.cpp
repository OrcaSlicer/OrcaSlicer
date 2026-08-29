#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Taken from the config enum map rather than hand-listed, so a flavor added to GCodeFlavor later
// is covered here without editing this file.
static std::vector<GCodeFlavor> non_klipper_flavors()
{
    std::vector<GCodeFlavor> flavors;
    for (const auto &[name, value] : ConfigOptionEnum<GCodeFlavor>::get_enum_values())
        if (GCodeFlavor(value) != gcfKlipper)
            flavors.push_back(GCodeFlavor(value));
    return flavors;
}

static std::string flavor_name(GCodeFlavor flavor)
{
    return ConfigOptionEnum<GCodeFlavor>::get_enum_names()[int(flavor)];
}

TEST_CASE("Klipper flushes the wipe tower planner queue with M400", "[WipeTower]")
{
    CHECK(std::string(flush_planner_queue_command(gcfKlipper)) == "M400\n");
}

TEST_CASE("Other flavors flush the wipe tower planner queue with a zero dwell", "[WipeTower]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(std::string(flush_planner_queue_command(flavor)) == "G4 S0\n");
}

// 1.5s is exactly representable as a float, so neither form can drift when rounded.
TEST_CASE("Klipper waits in the wipe tower with a millisecond dwell", "[WipeTower]")
{
    CHECK(wait_command(gcfKlipper, 1.5f) == "G4 P1500\n");
}

TEST_CASE("Other flavors wait in the wipe tower with a seconds dwell", "[WipeTower]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(wait_command(flavor, 1.5f) == "G4 S1.500\n");
}

// The prime tower is validated against the real printable outline, so the placement clamps have to
// agree with it wherever that outline is not a rectangle. A regular hexagon inscribed in a 200mm
// circle stands in for the shipped delta beds.
TEST_CASE("The wipe tower placement clamp follows a non-rectangular bed outline", "[WipeTower]")
{
    const coord_t margin = scaled<coord_t>(1.);
    auto square_at = [](double x, double y, double side) {
        return BoundingBox(Point::new_scale(x, y), Point::new_scale(x + side, y + side));
    };
    // Does the footprint, padded by pad, sit inside the outline once the returned move is applied?
    auto lands_inside = [](BoundingBox box, const Polygons &bed, const Vec2f &move, coord_t pad) {
        box.translate(Point::new_scale(move.x(), move.y()));
        return diff(Polygons{box.inflated(pad).polygon()}, bed).empty();
    };

    const Polygons hex_bed{make_circle_num_segments(scaled<double>(100.), 6)};
    const Polygons square_bed{Polygon::new_scale(Pointfs{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}})};

    SECTION("a rectangular bed is left to the bounding box clamp") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(50., 50., 30.), square_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // Dragging the tower off one edge may not pull it away from the other, or it would jump out from
    // under the cursor instead of sliding along the edge.
    SECTION("only the violated axis is clamped") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(185., 50., 30.), square_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(-16., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("a footprint already inside the outline is left alone") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(-15., -15., 30.), hex_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("a footprint in the bounding box corner is pulled onto the bed") {
        const BoundingBox box = square_at(55., 50., 30.);
        REQUIRE_FALSE(lands_inside(box, hex_bed, Vec2f::Zero(), margin)); // in the bbox, off the hexagon
        CHECK(lands_inside(box, hex_bed, WipeTower::move_box_inside_polygon(box, hex_bed, margin), margin));
    }

    // An unresolved auto brim width reaches the drag clamp as a negative margin. Padding by it would
    // shrink the footprint and hand back a position the slice validation still rejects.
    SECTION("a negative margin still lands the footprint inside the outline") {
        const BoundingBox box = square_at(55., 50., 30.);
        const coord_t     brim = scaled<coord_t>(-0.5);
        CHECK(lands_inside(box, hex_bed, WipeTower::move_box_inside_polygon(box, hex_bed, brim), 0));
    }

    SECTION("a footprint too large for the bed is left alone") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(-200., -200., 400.), hex_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }
}

// The cases above only exercise the helpers in isolation. The one below slices a real
// two-filament print, so it also covers the binding constraint of both changes: that the
// configured `gcode_flavor` reaches the wipe tower writer and lands in the exported G-code.

// The G-code inside each WIPE_TOWER_START/WIPE_TOWER_END pair, concatenated, so an M400 emitted
// outside the tower (e.g. GCodeProcessor's pre-heat injector) cannot create a false match.
static std::string wipe_tower_regions(const std::string &gcode)
{
    const std::string &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string &end_tag   = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End);
    std::string regions;
    size_t pos = 0;
    while (true) {
        size_t start = gcode.find(start_tag, pos);
        if (start == std::string::npos)
            break;
        size_t end = gcode.find(end_tag, start);
        if (end == std::string::npos)
            break;
        regions.append(gcode, start, end - start);
        pos = end + 1;
    }
    return regions;
}

// A per-layer toolchange between the wall and infill filaments, same shape as
// test_multifilament.cpp's "Each feature prints with its assigned filament", so the wipe tower
// runs its toolchange path (and so `flush_planner_queue()`) on every layer.
static DynamicPrintConfig wipe_tower_toolchange_config(const std::string &gcode_flavor)
{
    return multifilament_config(2, {
        { "sparse_infill_filament_id",  1 },
        { "internal_solid_filament_id", 1 },
        { "top_surface_filament_id",    1 },
        { "bottom_surface_filament_id", 1 },
        { "outer_wall_filament_id",     2 },
        { "inner_wall_filament_id",     2 },
        { "enable_prime_tower",         true },
        { "layer_height",               0.3 },
        { "gcode_flavor",               gcode_flavor },
    });
}

// Slices a 10mm cube under `config`. Not plain Test::slice: a brand-new Print's first `apply()`
// counts one filament in use, and DynamicPrintConfig::normalize_fdm_2's single-filament rule then
// clears `enable_prime_tower`. A second apply, once init_print's regions have settled, sees both
// filaments and the tower survives.
static std::string slice_with_prime_tower(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    return gcode(print);
}

TEST_CASE("The wipe tower's toolchange planner flush follows the gcode flavor", "[WipeTower]")
{
    auto [flavor, expected, unexpected] = GENERATE(table<std::string, std::string, std::string>({
        { "klipper", "M400",  "G4 S0" },
        { "marlin",  "G4 S0", "M400"  } }));
    DYNAMIC_SECTION(flavor) {
        const std::string tower = wipe_tower_regions(slice_with_prime_tower(wipe_tower_toolchange_config(flavor)));
        REQUIRE_FALSE(tower.empty());
        CHECK_THAT(tower, Catch::Matchers::ContainsSubstring(expected));
        CHECK_THAT(tower, !Catch::Matchers::ContainsSubstring(unexpected));
    }
}

// Orca: on typical Cartesian/CoreXY kinematics the X-axis gantry beam spans the full bed
// width and only moves in Y, so reaching the wipe tower's Y coordinate sweeps the beam
// across that entire row - a tall object anywhere in that row can be hit by the gantry
// even if it is nowhere near the tower's XY footprint (see layered_print_cleareance_valid()
// in Print.cpp). This is a validate()-time (not slice-time) geometry/config check, so these
// tests only need Print::apply(), not a real multi-filament slice.

static Slic3r::DynamicPrintConfig two_filament_config()
{
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    // full_print_config() only carries PrintRegionConfig keys; filament_diameter (PrintConfig)
    // is not among them, so DynamicPrintConfig::set_num_filaments() silently no-ops on it
    // (it resizes via option(key, false), which finds nothing to resize). Materialize it
    // directly instead. nozzle_diameter is deliberately left at its single-nozzle default
    // (single_extruder_multi_material) - that is what needs a wipe tower in the first place.
    config.set_deserialize_strict({
        { "filament_diameter",               "1.75,1.75" },
        { "enable_prime_tower",              "1" },
        { "wipe_tower_no_sparse_layers",     "1" },
        { "wipe_tower_x",                    "10" },
        { "wipe_tower_y",                    "100" },
        { "prime_tower_width",               "30" },
        { "extruder_clearance_height_to_rod","40" },
        { "extruder_clearance_radius",       "40" },
        { "printable_area",                  "0x0,300x0,300x300,0x300" }
    });
    return config;
}

// A tiny object on filament 2, so the plate genuinely references a second filament rather
// than just carrying an unused config slot.
static void add_filament2_object(Model &model, const Vec3d &offset)
{
    ModelObject *object = model.add_object();
    object->name += "filament2.stl";
    object->add_volume(Slic3r::make_cube(5, 5, 5));
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->add_instance()->set_offset(offset);
}

TEST_CASE("Wipe tower collision detection flags a tall object sharing its gantry row", "[WipeTower][Collision]")
{
    Slic3r::DynamicPrintConfig config = two_filament_config();

    Print print;
    Model model;

    // Tall object (60mm > extruder_clearance_height_to_rod), positioned far away in X from
    // the tower's X span ([10, 40]) but at the same Y (100) - same gantry row, no XY overlap.
    // extruder_clearance_radius is small on purpose, so the (already-existing) radius-proximity
    // check does not also trip, isolating the Y-band one.
    ModelObject *object = model.add_object();
    object->name += "tall.stl";
    object->add_volume(Slic3r::make_cube(20, 20, 60));
    object->add_instance()->set_offset(Vec3d(200, 100, 0));

    add_filament2_object(model, Vec3d(50, 250, 0));

    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());

    std::vector<StringObjectException> warnings;
    Polygons collison_polygons;
    std::vector<std::pair<Polygon, float>> height_polygons;
    StringObjectException err = print.validate(&warnings, &collison_polygons, &height_polygons);

    INFO("err.string: " << err.string);
    REQUIRE_FALSE(err.string.empty());
    REQUIRE(err.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT);
    REQUIRE_THAT(err.string, Catch::Matchers::ContainsSubstring("gantry"));
    // The visual keep-out zone (the swept row) must be populated, not just the error text.
    REQUIRE_FALSE(collison_polygons.empty());
}

TEST_CASE("Wipe tower collision detection ignores a tall object in a different row", "[WipeTower][Collision]")
{
    Slic3r::DynamicPrintConfig config = two_filament_config();

    Print print;
    Model model;

    // Same tall object, but far away in Y too (250 vs the tower's 100) and far in X -
    // different gantry row and outside the clearance radius, so neither check should fire.
    ModelObject *object = model.add_object();
    object->name += "tall.stl";
    object->add_volume(Slic3r::make_cube(20, 20, 60));
    object->add_instance()->set_offset(Vec3d(200, 250, 0));

    add_filament2_object(model, Vec3d(150, 50, 0));

    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());

    std::vector<StringObjectException> warnings;
    Polygons collison_polygons;
    std::vector<std::pair<Polygon, float>> height_polygons;
    StringObjectException err = print.validate(&warnings, &collison_polygons, &height_polygons);

    INFO("err.string: " << err.string);
    REQUIRE(err.string.empty());
}
