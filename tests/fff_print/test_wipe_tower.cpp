#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PrintConfig.hpp"

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
        { "wipe_tower_x",               50 }, // inside the 200x200 test bed
        { "wipe_tower_y",               50 }, // (the default y, 220, is not)
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

// What Print feeds the shared estimate. The libslic3r WipeTowerEstimate cases cannot see this:
// they call the estimator directly. The estimate counts the filaments the print really uses,
// so the two-filament shape gives the outer wall the second one.
static DynamicPrintConfig tower_estimate_config(const char *wall_type, unsigned int filaments = 2)
{
    // 100 mm3 per purge on a 50 mm wide tower: one purge is 100/(layer_height * 50) of depth.
    return multifilament_config(filaments, {
        { "outer_wall_filament_id",         filaments == 2 ? "2" : "1" },
        { "enable_prime_tower",             "1"       },
        { "wipe_tower_wall_type",           wall_type },
        { "prime_tower_width",              "50"      },
        { "prime_volume",                   "100"     },
        { "prime_tower_infill_gap",         "100%"    },
        { "prime_tower_brim_width",         "3"       },
        { "purge_in_prime_tower",           "0"       },
        { "single_extruder_multi_material", "0"       },
        { "timelapse_type",                 "0"       },
        { "layer_height",                   "0.2"     },
        { "enable_wrapping_detection",      "0"       },
        { "raft_layers",                    "0"       } });
}

TEST_CASE("The tower is sized for the thinnest layer any object on the plate is sliced at", "[WipeTower]")
{
    // The tower has to survive its thinnest layer, so an override finer than the preset drives
    // the estimate even on the second object. Two 20 mm cubes, the second at 0.1 mm.
    const DynamicPrintConfig config = tower_estimate_config("rectangle");
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {}, { { "layer_height", "0.1" } } };

    Print print;
    Model model;
    init_print({ cube(20), cube(20) }, print, model, config, &overrides);

    // One purge at 0.1 mm: 100 / (0.1 * 50) = 20 mm, above the 20 mm-tall tower's stability
    // floor. At the preset's 0.2 mm it would be half that, so the two are easy to tell apart.
    const float floor_20mm = WipeTower::get_limit_depth_by_height(20.f);
    REQUIRE(floor_20mm < 10.f);
    CHECK_THAT(print.wipe_tower_data(2).depth, Catch::Matchers::WithinAbs(20., 1e-4));
}

TEST_CASE("Validation is given the tower's effective width, not the configured one", "[WipeTower]")
{
    // A rib wall squares the tower, so its width is its depth. Validation reads this rather
    // than re-deriving the rule from the wall type.
    Print print;
    Model model;

    SECTION("a rectangle wall keeps the configured width") {
        const DynamicPrintConfig config = tower_estimate_config("rectangle");
        init_print({ cube(20) }, print, model, config);
        const WipeTowerData &data = print.wipe_tower_data(2);
        CHECK_THAT(data.width, Catch::Matchers::WithinAbs(50., 1e-4));
        CHECK(data.depth < data.width);
    }

    SECTION("a rib wall reports the squared footprint") {
        const DynamicPrintConfig config = tower_estimate_config("rib");
        init_print({ cube(20) }, print, model, config);
        const WipeTowerData &data = print.wipe_tower_data(2);
        CHECK_THAT(data.width, Catch::Matchers::WithinAbs(data.depth, 1e-4));
        CHECK(data.width > 0.f);
    }
}

TEST_CASE("Generating the tower keeps its reported width current", "[WipeTower]")
{
    // width is handed out after the slice, so leaving it at the estimate reports a zero-width
    // tower to every post-generation consumer.
    const DynamicPrintConfig config = wipe_tower_toolchange_config("marlin");
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    REQUIRE(print.wipe_tower_data(2).width > 0.f);

    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    const WipeTowerData &data = print.wipe_tower_data();
    // A width the generator never wrote reads as zero. A rib wall squares the tower, so the
    // generated width is the body square: under the configured 50 mm, and inside the depth.
    CHECK(data.width > 0.f);
    CHECK(data.width < 50.f);
    CHECK(data.width <= data.depth + EPSILON);
}

TEST_CASE("A single-filament plate reserves a tower only when one is actually printed", "[WipeTower]")
{
    // The estimate has to answer this the way Print::apply does: reporting no tower for one
    // that is built collapses the validation hull to a point, and reporting one for a tower
    // that is not built takes that bed area away from the arranger and draws a preview box
    // over nothing.
    Print print;
    Model model;

    SECTION("no tool change and nothing else that prints one") {
        const DynamicPrintConfig config = tower_estimate_config("rib", 1);
        init_print({ cube(20) }, print, model, config);
        REQUIRE_FALSE(print.has_wipe_tower());
        CHECK_THAT(print.wipe_tower_data(1).depth, Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // A raft puts the tower on every layer below the object, but only where there is a tower:
    // Print::apply runs normalize_fdm_2, which clears enable_prime_tower for a plate that
    // purges one filament and has neither smooth timelapse nor wrapping detection on.
    SECTION("a raft alone does not print one") {
        DynamicPrintConfig config = tower_estimate_config("rib", 1);
        config.set_deserialize_strict({ { "raft_layers", "3" } });
        init_print({ cube(20) }, print, model, config);
        REQUIRE_FALSE(print.config().enable_prime_tower.value);
        REQUIRE_FALSE(print.has_wipe_tower());
        CHECK_THAT(print.wipe_tower_data(1).depth, Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("smooth timelapse prints one, and keeps enable_prime_tower on") {
        DynamicPrintConfig config = tower_estimate_config("rib", 1);
        config.set_deserialize_strict({ { "timelapse_type", "1" } });
        init_print({ cube(20) }, print, model, config);
        REQUIRE(print.has_wipe_tower());
        CHECK(print.wipe_tower_data(1).depth > 0.f);
    }
}

TEST_CASE("A tower printed without a tool change is still validated against the bed", "[WipeTower]")
{
    // Wrapping detection prints a tower on a plate that purges one filament. Neither the old
    // estimate (which read the wall type and smooth timelapse) nor the old containment gate (the
    // filament count or smooth timelapse) knew about it, so between them that tower was never
    // checked against the bed.
    Print print;
    Model model;
    DynamicPrintConfig config = tower_estimate_config("rectangle", 1);
    // Relative E without a per-layer G92 is rejected before the tower is ever looked at, and
    // has_wipe_tower() wants a real exclusion polygon before it honours wrapping detection.
    config.set_deserialize_strict({ { "enable_wrapping_detection", "1" },
                                    { "wrapping_exclude_area", "180x180,190x180,190x190,180x190" },
                                    { "wipe_tower_x", "500" }, { "wipe_tower_y", "500" },                                    { "use_relative_e_distances", "0" } });

    init_print({ cube(20) }, print, model, config);
    REQUIRE(print.extruders(true).size() == 1);
    REQUIRE(print.has_wipe_tower());
    CHECK(print.wipe_tower_data(1).depth > 0.f);
    CHECK_THAT(print.validate().string, Catch::Matchers::ContainsSubstring("printable area"));
}
