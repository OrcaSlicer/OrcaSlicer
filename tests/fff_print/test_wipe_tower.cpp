#include <algorithm>
#include <cmath>
#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include <cstddef>
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/TriangleMesh.hpp"

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

// change_filament_gcode for the Klipper sections: a marker comment, so no slicer output changes.
static const char *const toolchange_mark = "; TOOLCHANGE_MARK\n";

// On Klipper the tower syncs motion only right before a temperature command and, with
// `sync_before_change`, once per toolchange at the end of the unload, so change_filament_gcode
// can't run while the unload is still queued. Any other M400 just stalls the toolhead.
static bool m400_only_where_needed(const std::string &gcode, bool sync_before_change)
{
    bool synced = false; // a bare M400 since the last change_filament_gcode
    std::istringstream lines(gcode);
    for (std::string line, prev; std::getline(lines, line); prev = line) {
        if (prev == "M400" && line.rfind("M104", 0) != 0 && line.rfind("M109", 0) != 0) {
            if (!sync_before_change || synced)
                return false;
            synced = true;
        }
        if (line.find("; TOOLCHANGE_MARK") != std::string::npos) {
            if (sync_before_change && !synced)
                return false;
            synced = false;
        }
    }
    return true;
}

// The cooldown back to the base temperature after an interface wipe closes the toolchange, so a
// wipe tower region ends right after it. Returns how many there are, or 0 if any of them is synced.
static size_t unsynced_interface_cooldowns(const std::string &tower)
{
    const std::string next_region = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    size_t cooldowns = 0;
    for (size_t pos = tower.find("\nM104 "); pos != std::string::npos; pos = tower.find("\nM104 ", pos + 1)) {
        const size_t eol = tower.find('\n', pos + 1);
        if (eol == std::string::npos
            || (tower.compare(eol + 1, std::string::npos, ";") != 0 && tower.compare(eol + 1, next_region.size(), next_region) != 0))
            continue; // not the last line of its region
        if (pos >= 4 && tower.compare(pos - 4, 4, "M400") == 0)
            return 0;
        ++cooldowns;
    }
    return cooldowns;
}

// The wipe tower regions of a slice under `config`. Post-processed tower moves start with "G1  "
// (two spaces); a bare "G1 Y" is a move glued to a placeholder line, which skipped
// post_process_wipe_tower_moves and kept tower-local coordinates.
static std::string sliced_wipe_tower(const DynamicPrintConfig &config)
{
    const std::string tower = wipe_tower_regions(slice_with_prime_tower(config));
    REQUIRE_FALSE(tower.empty());
    CHECK_THAT(tower, !Catch::Matchers::ContainsSubstring("\nG1 Y"));
    return tower;
}

TEST_CASE("Tower consumption is classified separately for each printer kind", "[WipeTower][Regression]")
{
    const auto [is_bbl, tower_type] = GENERATE(table<bool, std::string>({
        { false, "type1" }, { false, "type2" }, { true, "type1" } }));
    CAPTURE(is_bbl, tower_type);
    const bool previous_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl([previous_bbl] { GCodeProcessor::s_IsBBLPrinter = previous_bbl; });

    DynamicPrintConfig config = wipe_tower_toolchange_config("marlin");
    config.set_deserialize_strict({ { "wipe_tower_type", tower_type } });
    Print print;
    print.is_BBL_printer() = is_bbl;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);

    // Cover the initial non-BBL slice (the global defaults to true) and a change of printer kind.
    GCodeProcessor::s_IsBBLPrinter = !is_bbl;
    print.process();

    GCodeProcessorResult result;
    gcode(print, &result);
    const auto &statistics = result.print_statistics;
    REQUIRE(statistics.wipe_tower_volumes_per_extruder.size() == 2);
    for (size_t filament_id : { size_t(0), size_t(1) }) {
        CAPTURE(filament_id);
        REQUIRE(statistics.wipe_tower_volumes_per_extruder.count(filament_id) == 1);
        CHECK(statistics.wipe_tower_volumes_per_extruder.at(filament_id) > 0.);
        REQUIRE(statistics.model_volumes_per_extruder.count(filament_id) == 1);
        CHECK(statistics.model_volumes_per_extruder.at(filament_id) > 0.);
    }
}

TEST_CASE("The wipe tower's toolchange planner flush follows the gcode flavor", "[WipeTower]")
{
    SECTION("klipper") {
        const std::string tower_type = GENERATE("type1", "type2");
        CAPTURE(tower_type);
        DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
        config.set_deserialize_strict({ { "wipe_tower_type", tower_type }, { "change_filament_gcode", toolchange_mark } });
        const std::string tower = sliced_wipe_tower(config);
        CHECK_THAT(tower, !Catch::Matchers::ContainsSubstring("G4 S0"));
        // Only type2 syncs at the end of the unload; type1's toolchange_Unload is compiled out.
        CHECK(m400_only_where_needed(tower, tower_type == std::string("type2")));
    }
    SECTION("marlin") {
        const std::string tower = sliced_wipe_tower(wipe_tower_toolchange_config("marlin"));
        CHECK_THAT(tower, Catch::Matchers::ContainsSubstring("G4 S0"));
        CHECK_THAT(tower, !Catch::Matchers::ContainsSubstring("M400"));
    }
    SECTION("klipper, single extruder multi material") {
        // The SEMM path changes the temperature inside the tower; the sync before M104 must stay.
        DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
        config.set_deserialize_strict({ { "single_extruder_multi_material", "1" }, { "change_filament_gcode", toolchange_mark } });
        const std::string tower = sliced_wipe_tower(config);
        CHECK_THAT(tower, Catch::Matchers::ContainsSubstring("M400\nM104"));
        CHECK(m400_only_where_needed(tower, true));
    }
    SECTION("klipper, tower interface features") {
        // The interface M109 is stripped from the tower by GCode.cpp; its sync line must go with it.
        // The cooldown back to the base temperature after the wipe isn't synced either; other M104s
        // (the single extruder multi material path of this config) still are.
        DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
        config.set_deserialize_strict({ { "enable_tower_interface_features", "1" },
                                        { "nozzle_temperature_range_high", "250,250" },
                                        { "change_filament_gcode", toolchange_mark } });
        const std::string tower = sliced_wipe_tower(config);
        CHECK(m400_only_where_needed(tower, true));
        CHECK(unsynced_interface_cooldowns(tower) > 0);
    }
}

TEST_CASE("The wipe tower sets Klipper's acceleration and accel_to_decel on one line", "[WipeTower]")
{
    // On separate lines, current Klipper finds nothing it reads on the ACCEL_TO_DECEL one and 0.13
    // derives the cruise ratio from the acceleration before the change.
    DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
    // Type 2 towers leave the acceleration to the main G-code writer.
    config.set_deserialize_strict({ { "wipe_tower_type", "type1" },
                                    { "accel_to_decel_enable", "1" }, { "accel_to_decel_factor", "50%" },
                                    { "default_acceleration", "1000" }, { "travel_acceleration", "2000" },
                                    { "machine_max_acceleration_extruding", "5000" } });
    const std::string tower = wipe_tower_regions(slice_with_prime_tower(config));
    REQUIRE_THAT(tower, Catch::Matchers::ContainsSubstring("SET_VELOCITY_LIMIT ACCEL="));
    CHECK_THAT(tower, Catch::Matchers::ContainsSubstring(" MINIMUM_CRUISE_RATIO=0.5\n"));
    CHECK_THAT(tower, !Catch::Matchers::ContainsSubstring("SET_VELOCITY_LIMIT ACCEL_TO_DECEL") &&
                      !Catch::Matchers::ContainsSubstring("M204"));
}

TEST_CASE("The wipe tower's G-code moves keep a decimal point in a comma-decimal locale", "[WipeTower]")
{
    // The GUI slices on a thread that keeps the user's numeric locale.
    CommaNumericLocale comma;
    CNumericLocalesSetter restore;
    if (! comma.apply())
        SKIP("no locale with a comma decimal separator is installed");
    DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
    // Type 1 towers join their toolchange blocks with a travel move of their own.
    config.set_deserialize_strict({ { "wipe_tower_type", "type1" } });
    const std::string gcode = slice_with_prime_tower(config);
    REQUIRE_FALSE(wipe_tower_regions(gcode).empty());
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);)
        if (! line.empty() && line.front() == 'G' && line.find(',') != std::string::npos)
            FAIL_CHECK(line);
}

TEST_CASE("The wipe tower's flow tags keep a decimal point in a comma-decimal locale", "[WipeTower]")
{
    const bool is_bbl = GENERATE(false, true);
    CAPTURE(is_bbl);
    const bool previous_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl([previous_bbl] { GCodeProcessor::s_IsBBLPrinter = previous_bbl; });
    // GCodeProcessor parses these values with the C locale: a comma makes it log an error and
    // ignore the tag, leaving the extrusion width and height it assumes to its own defaults.
    CommaNumericLocale comma;
    CNumericLocalesSetter restore;
    if (! comma.apply())
        SKIP("no locale with a comma decimal separator is installed");
    DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
    config.set_deserialize_strict({ { "wipe_tower_type", "type1" } });
    Print print;
    print.is_BBL_printer() = is_bbl;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    print.process();
    std::string tower_gcode;
    for (const auto &layer : print.wipe_tower_data().tool_changes)
        for (const auto &change : layer)
            tower_gcode += change.gcode;
    // The width and height tags are both written by the writer's constructor, before the
    // tower's start tag, so scan the complete tower blocks without the model's tags.
    const std::string height_tag = is_bbl ? "; LAYER_HEIGHT: " : ";HEIGHT:";
    const std::string width_tag  = is_bbl ? "; LINE_WIDTH: " : ";WIDTH:";
    size_t heights = 0;
    size_t widths  = 0;
    std::istringstream lines(tower_gcode);
    for (std::string line; std::getline(lines, line);) {
        const bool is_height = line.compare(0, height_tag.size(), height_tag) == 0;
        const bool is_width  = line.compare(0, width_tag.size(), width_tag) == 0;
        if (! is_height && ! is_width)
            continue;
        if (is_height)
            ++heights;
        else
            ++widths;
        if (line.find(',') != std::string::npos)
            FAIL_CHECK(line);
    }
    CHECK(heights > 0);
    CHECK(widths > 0);
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

TEST_CASE("A variable layer height thinner than the preset deepens the estimate as much as the tower", "[WipeTower]")
{
    // A 20 mm cube under a 0.2 mm preset, printed at 0.1 mm above its first layer by its variable layer
    // height profile. Each layer purges the same volume, so the generated tower is about twice as deep
    // as at the preset's height: 100 / (0.2 * 50) = 10 mm there.
    DynamicPrintConfig config = tower_estimate_config("rectangle");
    config.set_deserialize_strict({ { "initial_layer_print_height", "0.2" },
                                    { "min_layer_height",           "0.07" },
                                    { "wipe_tower_x",               "120" },
                                    { "wipe_tower_y",               "120" } });
    TriangleMesh mesh = cube(20);
    mesh.translate(30.f, 30.f, 0.f);

    Print print;
    Model model;
    init_print({ std::move(mesh) }, print, model, config, nullptr, false);
    // (z, layer height) pairs: the 0.2 mm first layer, then 0.1 mm up to the top.
    model.objects.front()->layer_height_profile.set({ 0., 0.2, 0.2, 0.1, 20., 0.1 });
    print.apply(model, config);

    const float estimate = print.wipe_tower_data(2).depth;
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    const float generated = print.wipe_tower_data().depth;
    INFO("estimated depth " << estimate << ", generated depth " << generated);
    CHECK(generated > 15.f);
    // The estimate leaves out the line shape and the perimeter, so it is close to the tower, not equal.
    CHECK_THAT(estimate, Catch::Matchers::WithinRel(generated, 0.2f));
}

// A 50 mm tall object gives the tower an auto brim of 4 mm, which is only sized when the tower is
// generated: before slicing, validation checks the body alone against the bed.
static DynamicPrintConfig auto_brim_tower_config()
{
    DynamicPrintConfig config = tower_estimate_config("rectangle");
    // The G92 E0 lets validate() accept the relative E of the test printer.
    config.set_deserialize_strict({ { "prime_tower_brim_width", "-1" }, { "layer_change_gcode", "G92 E0\n" } });
    return config;
}

static TriangleMesh auto_brim_tower_object()
{
    TriangleMesh mesh = make_cube(20., 20., 50.);
    mesh.translate(30.f, 30.f, 0.f);
    return mesh;
}

TEST_CASE("A tower that does not fit where it was placed is moved to the nearest place where it fits", "[WipeTower]")
{
    DynamicPrintConfig config = auto_brim_tower_config();
    Print print;
    Model model;
    init_print({ auto_brim_tower_object() }, print, model, config, nullptr, false);
    print.apply(model, config);
    REQUIRE(print.wipe_tower_data(2).brim_width > 2.f);

    // The body 1 mm inside the back edge of the 200 mm bed, so its brim reaches past it.
    const double x = 100.;
    const double y = 200. - print.wipe_tower_data(2).depth - 1.;
    config.set_deserialize_strict({ { "wipe_tower_x", std::to_string(x) }, { "wipe_tower_y", std::to_string(y) } });
    print.apply(model, config);
    REQUIRE(print.validate().string == "");

    const std::string gcode_out = gcode(print);
    CHECK_FALSE(gcode_out.empty());
    REQUIRE(print.wipe_tower_data().wipe_tower_mesh_data.has_value());

    // Straight towards the bed, which is the nearest place, and the whole generated footprint on it.
    const double moved_y = print.config().wipe_tower_y.get_at(0);
    CHECK(moved_y < y);
    CHECK_THAT(print.config().wipe_tower_x.get_at(0), Catch::Matchers::WithinAbs(x, 1e-3));
    Polygon footprint = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
    footprint.translate(Point(scale_(x), scale_(moved_y)));
    CHECK(unscaled(get_extents(footprint).max.y()) <= 200.);
    CHECK(print.validate().string == "");

    // The plate still asking for the old place keeps the slice, so it is not redone over and over.
    print.apply(model, config);
    CHECK(print.is_step_done(psSkirtBrim));
    CHECK_THAT(print.config().wipe_tower_y.get_at(0), Catch::Matchers::WithinAbs(moved_y, 1e-9));
}

TEST_CASE("A tower moved to fit stays clear of the clumping detection area", "[WipeTower]")
{
    // The tower of the test above, with wrapping detection probing a strip just in front of it, where
    // the nearest place for the tower would be. The strip clears the tower body, which is what
    // validate() checks before slicing, but not its brim.
    DynamicPrintConfig config = auto_brim_tower_config();
    Print print;
    Model model;
    init_print({ auto_brim_tower_object() }, print, model, config, nullptr, false);
    print.apply(model, config);
    REQUIRE(print.wipe_tower_data(2).brim_width > 2.f);

    const double x = 100.;
    const double y = 200. - print.wipe_tower_data(2).depth - 1.;
    const double strip_min_y = y - 11.;
    const double strip_max_y = y - 1.;
    const std::string strip = "90x" + std::to_string(strip_min_y) + ",160x" + std::to_string(strip_min_y) + ",160x" +
                              std::to_string(strip_max_y) + ",90x" + std::to_string(strip_max_y);
    config.set_deserialize_strict({ { "wipe_tower_x", std::to_string(x) },
                                    { "wipe_tower_y", std::to_string(y) },
                                    { "enable_wrapping_detection", "1" },
                                    { "wrapping_exclude_area", strip } });
    print.apply(model, config);
    REQUIRE(print.validate().string == "");

    const std::string gcode_out = gcode(print);
    CHECK_FALSE(gcode_out.empty());
    REQUIRE(print.wipe_tower_data().wipe_tower_mesh_data.has_value());

    Polygon footprint = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
    footprint.translate(Point(scale_(print.config().wipe_tower_x.get_at(0)), scale_(print.config().wipe_tower_y.get_at(0))));
    const Polygon strip_area({ Point(scale_(90.), scale_(strip_min_y)), Point(scale_(160.), scale_(strip_min_y)),
                               Point(scale_(160.), scale_(strip_max_y)), Point(scale_(90.), scale_(strip_max_y)) });
    CHECK(intersection(Polygons{ footprint }, Polygons{ strip_area }).empty());
    CHECK(unscaled(get_extents(footprint).max.y()) <= 200.);
}

TEST_CASE("A tower that fits nowhere on the plate is still refused", "[WipeTower]")
{
    // A 55 mm wide bed cannot hold the 50 mm wide tower with a 4 mm brim on each side.
    DynamicPrintConfig config = auto_brim_tower_config();
    config.set_deserialize_strict({ { "printable_area", "0x0,55x0,55x200,0x200" },
                                    { "wipe_tower_x",   "2" },
                                    { "wipe_tower_y",   "100" } });
    Print print;
    Model model;
    init_print({ auto_brim_tower_object() }, print, model, config, nullptr, false);
    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());
    CHECK_THROWS_WITH(print.process(), Catch::Matchers::ContainsSubstring("printable area"));
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

// Filament 2 on the top surface only, so every layer below it is a toolchange-free tower layer: the
// run "Combine sparse layers" folds. The two heights decide whether anything folds, so they are the
// caller's business.
static DynamicPrintConfig sparse_run_config(double layer_height, const char *max_layer_height, bool combine)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "top_surface_filament_id",        2     },
        { "enable_prime_tower",             true  },
        { "wipe_tower_x",                   50    }, // inside the 200x200 test bed
        { "wipe_tower_y",                   50    },
        { "prime_tower_width",              35    },
        { "min_layer_height",               "0.08"},
        { "single_extruder_multi_material", true  },
        { "timelapse_type",                 "0"   },
        { "enable_wrapping_detection",      false },
        { "raft_layers",                    "0"   } });
    // A taller first layer would top the plan and hide what the run does, so slice at one height.
    config.set_deserialize_strict({ { "layer_height",               std::to_string(layer_height) },
                                    { "initial_layer_print_height", std::to_string(layer_height) },
                                    { "max_layer_height", max_layer_height },
                                    { "wipe_tower_sparse_layers_combination", combine ? "1" : "0" } });
    return config;
}

// What a sliced tower did with its sparse run.
struct SparseRunResult { size_t planned, sparse, folded; float tallest_printed, printed_height; std::string gcode; };

static SparseRunResult slice_sparse_run(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));

    SparseRunResult r{};
    for (const std::vector<WipeTower::ToolChangeResult> &layer : print.wipe_tower_data().tool_changes) {
        if (layer.empty())
            continue;
        ++r.planned;
        if (wipe_tower_layer_is_sparse(layer))
            ++r.sparse;
        if (wipe_tower_layer_is_combined_away(layer)) {
            ++r.folded;
        } else {
            r.tallest_printed = std::max(r.tallest_printed, layer.front().layer_height);
            r.printed_height += layer.front().layer_height;
        }
    }
    r.gcode = Slic3r::Test::gcode(print);
    return r;
}

// How often the G-code declares `height` in the tag this printer's processor reads. The dialect is a
// global the exporter sets from the printer, so this is only correct after a slice - the point below.
static size_t count_height_tags(const std::string &gcode, const char *height)
{
    const std::string tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + height + "\n";
    size_t n = 0;
    for (size_t p = gcode.find(tag); p != std::string::npos; p = gcode.find(tag, p + 1))
        ++n;
    return n;
}

TEST_CASE("Combining sparse layers folds a run into whole layers the nozzle can lay down", "[WipeTower]")
{
    // 0.1 mm layers under a 0.32 mm cap: three fit (0.3), a fourth does not, so a run prints once
    // every three layers at 0.3 mm.
    const SparseRunResult plain    = slice_sparse_run(sparse_run_config(0.1, "0.32", false));
    const SparseRunResult combined = slice_sparse_run(sparse_run_config(0.1, "0.32", true));

    REQUIRE(plain.planned == combined.planned); // the plan still has one layer per object layer
    REQUIRE(plain.sparse > 10);
    CHECK(plain.folded == 0);
    CHECK_THAT(plain.tallest_printed, Catch::Matchers::WithinAbs(0.1f, 1e-4f));

    CHECK(combined.folded > 0);
    CHECK_THAT(combined.tallest_printed, Catch::Matchers::WithinAbs(0.3f, 1e-4f));
    // Two of every three sparse layers fold away, leaving the toolchange layers untouched.
    CHECK(combined.folded <= plain.sparse);
    CHECK(combined.folded >= plain.sparse / 2);
    // What folds away comes back as height on the layer that prints the run: no gap, nothing twice.
    CHECK_THAT(combined.printed_height, Catch::Matchers::WithinAbs(plain.printed_height, 1e-3f));
}

TEST_CASE("A run too thin to reach the nozzle's layer height is left alone", "[WipeTower]")
{
    // Only whole layers merge, so two 0.2 mm layers (0.4) do not fit a 0.32 mm maximum and the tower
    // prints as if the option were off. This is the common 0.4 nozzle case; the tooltip says so.
    const SparseRunResult plain    = slice_sparse_run(sparse_run_config(0.2, "0.32", false));
    const SparseRunResult combined = slice_sparse_run(sparse_run_config(0.2, "0.32", true));

    REQUIRE(plain.sparse > 10);
    CHECK(combined.folded == 0);
    CHECK(combined.planned == plain.planned);
    CHECK_THAT(combined.tallest_printed, Catch::Matchers::WithinAbs(0.2f, 1e-4f));
}

TEST_CASE("A merged tower layer declares its own height to the G-code processor", "[WipeTower]")
{
    // The processor must read the merged tower's height rather than the object's layer height.
    // 0.2 mm layers under a 0.42 mm maximum merge in pairs.
    const SparseRunResult plain    = slice_sparse_run(sparse_run_config(0.2, "0.42", false));
    const SparseRunResult combined = slice_sparse_run(sparse_run_config(0.2, "0.42", true));

    REQUIRE(combined.folded > 0);
    CHECK_THAT(combined.tallest_printed, Catch::Matchers::WithinAbs(0.4f, 1e-4f));
    // Every layer that prints a merged run has to say so, and nothing may say so without the option.
    CHECK(count_height_tags(combined.gcode, "0.4") - count_height_tags(plain.gcode, "0.4") == combined.folded);
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

TEST_CASE("Only a cone wall gives the tower a stabilization cone", "[WipeTower]")
{
    // The cone angle stays in the config for every wall type, but only a cone wall prints the
    // cone. A 120 mm object makes the tower tall enough that a 30 degree cone would reach the
    // thin plate in front of the tower, which the real rectangle or rib tower stays clear of.
    const std::string wall_type = GENERATE("rectangle", "rib", "cone");
    DynamicPrintConfig config = tower_estimate_config(wall_type.c_str());
    config.set_deserialize_strict({ { "wipe_tower_type",       "type2" },
                                    { "wipe_tower_cone_angle", "30" },
                                    { "wipe_tower_x",          "30" },
                                    { "wipe_tower_y",          "80" },
                                    { "layer_height",          "0.3" } });

    TriangleMesh tall = make_cube(20., 20., 120.);
    tall.translate(150.f, 150.f, 0.f);
    TriangleMesh plate = make_cube(30., 32., 2.);
    plate.translate(40.f, 30.f, 0.f);
    const double plate_max_y = 30. + 32.;

    Print print;
    Model model;
    init_print({ std::move(tall), std::move(plate) }, print, model, config, nullptr, false);
    print.apply(model, config);
    print.process();
    REQUIRE(print.has_wipe_tower());

    const BoundingBox footprint(print.first_layer_wipe_tower_corners());
    INFO("wall type " << wall_type << ", tower depth " << print.wipe_tower_data().depth << ", footprint min y "
                      << unscaled(footprint.min.y()));
    if (wall_type == "cone") {
        CHECK(unscaled(footprint.min.y()) < plate_max_y);
        CHECK(print.get_conflict_result().has_value());
    } else {
        CHECK(unscaled(footprint.min.y()) > plate_max_y);
        CHECK_FALSE(print.get_conflict_result().has_value());
    }
}

TEST_CASE("Without sparse layers the filament change starts at the tallest printed layer", "[WipeTower]")
{
    // Filament 2 on top surfaces only, so the layers between the short cube's top and the tall
    // cube's top have no toolchange and the tower skips them: at the tall cube's top it is still
    // about 2 mm high. A custom change_filament_gcode may travel anywhere, and so may the firmware
    // Tn of a switch to another physical extruder, so neither may start that low.
    const std::string tower_type   = GENERATE("type1", "type2");
    const bool        two_nozzles  = GENERATE(false, true);
    const bool        custom_macro = GENERATE(true, false);
    DynamicPrintConfig config = multifilament_config(2, {
        { "top_surface_filament_id",     2     },
        { "enable_prime_tower",          true  },
        { "wipe_tower_type",             tower_type },
        { "wipe_tower_no_sparse_layers", true  },
        { "wipe_tower_x",                30    },
        { "wipe_tower_y",                30    },
        { "layer_height",                0.2   },
        { "change_filament_gcode",       custom_macro ? "; TOOLCHANGE_MARK\n" : "" },
        { "gcode_comments",              true  },
        { "timelapse_type",              "0"   },
        { "enable_wrapping_detection",   false },
        { "raft_layers",                 "0"   } });
    if (two_nozzles) {
        config.set_num_extruders(2);
        config.set_deserialize_strict({
            { "single_extruder_multi_material", false },
            { "nozzle_diameter",                "0.4,0.4" },
            { "printer_extruder_id",            "1,2" },
            { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height",      "0,0" },
            { "filament_map_mode",              "Manual" },
            { "filament_map",                   "1,2" } });
        size_flush_to_nozzles(config);
    }

    const double short_height = 2., tall_height = 10.;
    TriangleMesh tall = make_cube(10., 10., tall_height);
    tall.translate(120.f, 120.f, 0.f);
    TriangleMesh low = make_cube(10., 10., short_height);
    low.translate(150.f, 120.f, 0.f);

    Print print;
    Model model;
    init_print({ std::move(tall), std::move(low) }, print, model, config, nullptr, false);
    print.apply(model, config);
    const std::string gcode = Slic3r::Test::gcode(print);

    // Z where each change_filament_gcode or Tn starts, next to the highest Z of the objects
    // extruded before it; and the Z of each tower extrusion once the objects pass the short cube.
    std::vector<std::pair<double, double>> changes;
    std::vector<std::pair<double, double>> tower_extrusions;
    double object_z = 0.;
    bool   in_tower = false;
    size_t lifts    = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        // Track the extrusion role using the tag format emitted for the printer.
        if (raw.rfind(";TYPE:", 0) == 0 || raw.rfind("; FEATURE: ", 0) == 0)
            in_tower = raw.find("Prime tower") != std::string_view::npos;
        else if (line.extruding(self) && (line.has_x() || line.has_y())) { // not an unretract
            if (!in_tower)
                object_z = std::max(object_z, double(self.z()));
            // Above the tall cube is the final unload, printed over the finished parts on purpose.
            else if (object_z > short_height + EPSILON && self.z() <= tall_height + EPSILON)
                tower_extrusions.emplace_back(self.z(), object_z);
        } else if (raw.find("; TOOLCHANGE_MARK") != std::string_view::npos
                   || (raw.size() > 1 && raw[0] == 'T' && raw[1] >= '0' && raw[1] <= '9'))
            changes.emplace_back(self.z(), object_z);
        if (raw.find("Restore layer Z before toolchange") != std::string_view::npos)
            ++lifts;
    });
    INFO(tower_type << (two_nozzles ? ", two nozzles" : ", one nozzle") << (custom_macro ? ", custom macro" : ", empty macro"));

    if (!custom_macro && !two_nozzles) {
        // Nothing travels during the change, so the toolhead stays down at the tower.
        CHECK(lifts == 0);
    } else {
        size_t above_short = 0;
        for (const auto &[change_z, printed_z] : changes) {
            if (printed_z <= short_height + EPSILON)
                continue;
            ++above_short;
            INFO("toolchange at Z " << change_z << ", printed up to Z " << printed_z);
            CHECK(change_z >= printed_z - EPSILON);
        }
        // The tall cube's top surface needs at least one change above the short cube.
        CHECK(above_short > 0);
    }

    // Every lift has to come back down: the tower keeps printing at its compacted height.
    REQUIRE_FALSE(tower_extrusions.empty());
    for (const auto &[tower_z, printed_z] : tower_extrusions) {
        INFO("tower extrusion at Z " << tower_z << ", printed up to Z " << printed_z);
        CHECK(tower_z < printed_z - EPSILON);
    }
}

// Two cubes at 0.2 mm layers with precise Z height on: the 20 mm cube stays on the 0.2 mm grid, and
// precise Z height spreads the other's gap to `short_height` over its last five layers, moving them
// off the grid. Slices and exports the plate; the emitter throws if it loses step with the tower.
static std::string slice_precise_z_pair(DynamicPrintConfig config, const char *tower_type, double short_height, Print &print,
                                        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> *overrides = nullptr)
{
    // The default position puts the 50 mm tower plus its 3 mm brim past the 200 mm test bed.
    config.set_deserialize_strict({ { "precise_z_height", "1" }, { "wipe_tower_type", tower_type },
                                    { "wipe_tower_x", "130" }, { "wipe_tower_y", "140" } });
    Model model;
    init_print({ cube(20), make_cube(20, 20, short_height) }, print, model, config, overrides);
    print.apply(model, config); // see slice_with_prime_tower
    REQUIRE(print.has_wipe_tower());
    std::string out;
    REQUIRE_NOTHROW(out = gcode(print));
    CHECK_FALSE(out.empty());
    return out;
}

// Z of every extruding move under the tower's feature tag.
static std::vector<double> tower_extrusion_z(const std::string &gcode)
{
    std::vector<double> zs;
    bool                in_tower = false;
    GCodeReader         parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0 || raw.rfind("; FEATURE: ", 0) == 0)
            in_tower = raw.find("Prime tower") != std::string_view::npos;
        else if (in_tower && line.extruding(self) && (line.has_x() || line.has_y()))
            zs.emplace_back(self.z());
    });
    return zs;
}

static size_t count_joined_layers(const Print &print)
{
    size_t joined = 0;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering.layer_tools())
        joined += lt.joins_wipe_tower_layer_below() ? 1 : 0;
    return joined;
}

TEST_CASE("A layer less than the minimum layer height above another prints its tool changes on the tower layer below", "[WipeTower]")
{
    // Both filaments print on every layer of both cubes, so every off-grid layer changes tools. At
    // 10.004 mm the last five layers grow by 0.0008 mm each and sit 0.0008 to 0.004 mm above the 20 mm
    // cube's; at 10.15 mm they shrink by 0.01 mm each, leaving the 20 mm cube's layers 0.01 to 0.05 mm
    // above them. All of that is under the 0.07 mm minimum layer height. A purge sized for a 0.0008 mm
    // layer is 100 / (0.0008 * 50) = 2500 mm deep, for a 0.05 mm one 40 mm; on a 0.19 mm layer it is
    // about 10.5 mm, and a joined layer holds two.
    const char  *tower_type   = GENERATE("type1", "type2");
    const double short_height = GENERATE(10.004, 10.15);
    DynamicPrintConfig config = tower_estimate_config("rectangle");
    config.set_deserialize_strict({ { "min_layer_height", "0.07" } });

    Print print;
    const std::string out = slice_precise_z_pair(config, tower_type, short_height, print);

    CHECK(count_joined_layers(print) > 0);
    std::vector<double> tower_layer_z;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering.layer_tools())
        if (lt.joins_wipe_tower_layer_below())
            CHECK(lt.print_z - lt.wipe_tower_print_z < 0.07);
        else if (lt.has_wipe_tower)
            tower_layer_z.emplace_back(lt.print_z);
    CHECK(print.wipe_tower_data().depth < 30.f);

    // A joined layer's purges go down to the tower layer they join, so the tower is only ever
    // extruded at its own layers' z, never up to 0.05 mm above one (G-code Z has 3 decimals).
    const std::vector<double> zs = tower_extrusion_z(out);
    REQUIRE_FALSE(zs.empty());
    size_t off_layer = 0;
    for (double z : zs)
        if (std::none_of(tower_layer_z.begin(), tower_layer_z.end(), [z](double t) { return std::abs(z - t) < 0.002; }))
            ++off_layer;
    CHECK(off_layer == 0);

    // The nozzle goes below the object layer only over the tower: descending anywhere else drags it
    // through the layer just printed.
    BoundingBox tower_box(print.first_layer_wipe_tower_corners());
    tower_box.offset(scaled(5.));
    size_t descents = 0, off_tower = 0;
    double layer_z  = 0.;
    GCodeReader parser;
    parser.parse_buffer(out, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";Z:", 0) == 0)
            layer_z = std::stod(std::string(raw.substr(3)));
        else if (line.has_z() && ! line.has_x() && ! line.has_y() && line.z() < layer_z - 1e-3) {
            ++descents;
            if (! tower_box.contains(Point(scaled(self.x()), scaled(self.y())))) {
                INFO("descent to Z " << line.z() << " at X " << self.x() << " Y " << self.y() << ", layer Z " << layer_z);
                ++off_tower;
            }
        }
    });
    CHECK(descents > 0);
    CHECK(off_tower == 0);
}

TEST_CASE("Layers sliced at the printer's minimum layer height keep their own tower layers", "[WipeTower]")
{
    // 0.08 mm layers summed in floating point sit 0.0799999 mm apart now and then, just under the
    // 0.08 mm minimum. Those are whole layers, not off-grid ones: none may join the layer below.
    const char *tower_type = GENERATE("type1", "type2");
    DynamicPrintConfig config = tower_estimate_config("rectangle");
    config.set_deserialize_strict({ { "layer_height", "0.08" }, { "min_layer_height", "0.08" }, { "wipe_tower_type", tower_type },
                                    { "wipe_tower_x", "130" }, { "wipe_tower_y", "140" } });

    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    print.apply(model, config); // see slice_with_prime_tower
    REQUIRE(print.has_wipe_tower());
    REQUIRE_NOTHROW(gcode(print));
    CHECK(count_joined_layers(print) == 0);
}

TEST_CASE("A joined layer that keeps its filament leaves finishing the tower layer to the one that changed", "[WipeTower]")
{
    // The 20 mm cube prints both filaments on every layer, the short one only `filament`. Whenever the
    // 20 mm cube's layer ends on that filament, the off-grid layer joined to it changes nothing, and the
    // tower layer's results are all taken by the time it would finish the layer.
    const char *tower_type = GENERATE("type1", "type2");
    const int   filament   = GENERATE(1, 2);
    // An unset feature filament reads as filament 1, so the short cube names its filament for each.
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {},
        { { "extruder", filament }, { "outer_wall_filament_id", filament }, { "inner_wall_filament_id", filament },
          { "sparse_infill_filament_id", filament }, { "internal_solid_filament_id", filament },
          { "top_surface_filament_id", filament }, { "bottom_surface_filament_id", filament } } };
    const DynamicPrintConfig config = tower_estimate_config("rectangle");

    Print print;
    slice_precise_z_pair(config, tower_type, 10.004, print, &overrides);

    // At least one joined layer must change no tools, or this does not test the case.
    size_t       keeps_filament = 0;
    unsigned int last           = (unsigned int) -1;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering.layer_tools()) {
        if (lt.joins_wipe_tower_layer_below() && lt.extruders.size() == 1 && lt.extruders.front() == last)
            ++keeps_filament;
        if (! lt.extruders.empty())
            last = lt.extruders.back();
    }
    CHECK(keeps_filament > 0);
}

TEST_CASE("Smooth timelapse prints each tower layer's wall once when a layer joins it", "[WipeTower]")
{
    // Smooth timelapse prints a tower on every layer even with one filament, and the Type 1 tower opens
    // each of its layers with the timelapse wall. A layer joined to the one below must not print it again.
    DynamicPrintConfig config = tower_estimate_config("rectangle", 1);
    config.set_deserialize_strict({ { "timelapse_type", "1" } });

    Print print;
    slice_precise_z_pair(config, "type1", 10.004, print);
    CHECK(count_joined_layers(print) > 0);
}
