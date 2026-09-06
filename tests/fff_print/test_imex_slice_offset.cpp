#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

// The IMEX slice offset is the firmware-managed handoff: an extra XY shift applied at emission
// so the primary tool's zone lands centred on the bed origin and the firmware can fan copies /
// mirrors out from there. `Print::update_imex_slice_offset()` DERIVES it from the applied
// config -- nobody hands it in. That matters because the only thing that used to hand it in was
// the plater, so a headless `orca-slicer --slice` produced slicer-frame coordinates while the
// mode G-code told the firmware to apply its own offsets on top.
//
// This file covers both halves:
//
//   * Derivation -- which config makes the offset non-zero, what value it takes, and (the part
//     the GUI push got wrong) that it is in PLATE-LOCAL mm and therefore does not move when the
//     plate origin does. `compute_imex_slice_offset()` and `compute_imex_zone_layout()` are
//     covered as pure functions in tests/libslic3r/; what is covered here is the Print reaching
//     them with the right inputs.
//   * Consumption -- that a slice actually comes out shifted. Two sites consume the offset:
//     GCode::set_gcode_offset_with_imex_shift() (GCode.hpp), which augments the writer offset
//     so every emitted coordinate moves, and Print::translate_to_print_space() (Print.cpp),
//     which feeds the first_layer_print_min/max placeholders a start-G-code template uses to
//     declare the print area to firmware. The two must stay in one frame, so the last test
//     cross-checks them against each other.
//
// The offset is applied at emission, after slicing, so it is a pure translation: assertions are
// on the SHIFT between two slices of the same plate, never on absolute coordinates or on a
// golden file. That keeps them independent of where the arranger puts the cube, and immune to
// the run-to-run variation in this slicer's parallel infill generation.

// ---------------------------------------------------------------------------------------------
// Helpers (unnamed namespace: every suite links into one binary, so nothing here may collide
// with a same-named helper in a sibling test file)
// ---------------------------------------------------------------------------------------------
namespace {

// The bed the derivation divides up, and the answer it must reach.
//
// With `imex_gantry_count` 1, `imex_tools_per_gantry` 2 and `imex_tool_layout` front-left, T0
// owns the left column and T1 the right one, so a mode declaring `0:P,1:C` gives the primary
// the left half of the bed: x [0, 150], y [0, 200]. Its centre is the offset. Both numbers are
// set by the config below, not inherited from a default.
constexpr double kBedWidth  = 300.0;
constexpr double kBedDepth  = 200.0;
const Vec2d      kPrimaryZoneCentre(kBedWidth / 4.0, kBedDepth / 2.0);

// XY extent of a set of emitted moves. `empty()` means nothing matched.
struct XYBounds
{
    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();

    bool   empty() const { return min_x > max_x; }
    double size_x() const { return max_x - min_x; }
    double size_y() const { return max_y - min_y; }
};

// Extent of the END points of every extruding move commented "perimeter", up to `z_max`.
//
// Perimeters only: they are the outermost extrusions, so they carry the plate's extent, and
// unlike infill their geometry and ordering are stable run to run. End points only: the parse
// callback runs BEFORE the reader advances, so the start point of the first extrusion of a
// polyline is whatever the preceding travel left behind -- and a cube's perimeters are closed
// loops, whose last point is their first, so the end points alone already give the true extent.
static XYBounds perimeter_bounds(const std::string &gcode, double z_max = std::numeric_limits<double>::max())
{
    XYBounds    bounds;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&bounds, z_max](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (!line.extruding(self))
            return;
        if (line.comment().find("perimeter") == std::string_view::npos)
            return;
        if (double(line.new_Z(self)) > z_max)
            return;
        const double x = line.new_X(self);
        const double y = line.new_Y(self);
        bounds.min_x   = std::min(bounds.min_x, x);
        bounds.max_x   = std::max(bounds.max_x, x);
        bounds.min_y   = std::min(bounds.min_y, y);
        bounds.max_y   = std::max(bounds.max_y, y);
    });
    return bounds;
}

// Read "<tag>x,y" out of `gcode`. The config block the exporter appends restates
// machine_start_gcode verbatim, so the tag also occurs there with the placeholder still
// unexpanded; occurrences are tried in turn and the first one that parses as two numbers wins.
static bool read_marker(const std::string &gcode, const std::string &tag, Vec2d &out)
{
    for (size_t at = gcode.find(tag); at != std::string::npos; at = gcode.find(tag, at + 1)) {
        const size_t      from    = at + tag.size();
        const size_t      to      = gcode.find('\n', from);
        const std::string payload = gcode.substr(from, to == std::string::npos ? std::string::npos : to - from);
        const size_t      comma   = payload.find(',');
        if (comma == std::string::npos)
            continue;
        try {
            out = Vec2d(std::stod(payload.substr(0, comma)), std::stod(payload.substr(comma + 1)));
        } catch (const std::exception &) {
            continue;
        }
        return true;
    }
    return false;
}

// An IMEX machine: 7 logical extruders across 4 physical heads, mirroring the geometry the IMEX
// cases in test_multifilament.cpp use, on a 2-tool single-gantry grid.
//
// physical_extruder_map is only honoured when its length matches the nozzle count (PrintApply
// hands effective_physical_extruder_map the nozzle_diameter size), so the nozzle keys have to be
// sized to 7 as well -- otherwise the map is silently replaced with the identity and the plate
// stops being an IMEX plate at all.
//
// The grid, the tool layout and the bed are all set explicitly: they are exactly the inputs the
// zone layout divides to reach kPrimaryZoneCentre, so none of them may come from a default.
// `imex_firmware_managed_zones` is deliberately NOT set here -- it is the switch under test, and
// every case states it for itself.
//
// Everything that would put extrusions outside the object footprint is off (skirt, brim, prime
// tower, infill, top/bottom shells): that leaves perimeters as the only extrusions, so the
// emitted extent and the first-layer convex hull both reduce to the cube's own outline.
static void imex_printer(DynamicPrintConfig &config)
{
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4,0.4,0.4,0.4,0.4,0.4" },
        { "printer_extruder_id",       "1,2,3,4,5,6,7" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard,Direct Drive Standard,"
                                       "Direct Drive Standard,Direct Drive Standard,Direct Drive Standard,"
                                       "Direct Drive Standard" },
        { "extruder_printable_height", "0,0,0,0,0,0,0" },
        { "physical_extruder_map",     "0,0,0,0,1,2,3" },
        { "printable_area",            "0x0,300x0,300x200,0x200" },
        { "is_imex",                   "1" },
        { "imex_gantry_count",         "1" },
        { "imex_tools_per_gantry",     "2" },
        { "imex_tool_layout",          "front-left" },
        { "imex_mode_names",           "primary;copy" },
        { "imex_mode_active_tools",    "0:P;0:P,1:C" },
        // `copy` declares head 0 Primary, and filament 1 (logical slot 0) routes there, so the
        // plate is well-formed and Print::validate() lets it through.
        { "imex_parallel_mode",        "copy" },
        { "skirt_loops",               "0" },
        { "brim_type",                 "no_brim" },
        { "enable_prime_tower",        "0" },
        { "sparse_infill_density",     "0%" },
        { "top_shell_layers",          "0" },
        { "bottom_shell_layers",       "0" },
        { "wall_loops",                "2" },
        { "layer_height",              "0.2" },
        { "initial_layer_print_height","0.2" },
        { "gcode_flavor",              "klipper" },
    });
}

// Route every region to one filament. An unset *_filament_id is not "inherit":
// clamp_feature_filament_to_valid rewrites <=0 to 1, so leaving them unset would drag extra
// tools into tool_ordering. PrintObject.cpp's call to that function is the source of truth for
// this key list.
static void all_regions_on_filament(DynamicPrintConfig &config, int filament_1based)
{
    for (const char *key : { "outer_wall_filament_id", "inner_wall_filament_id",
                             "sparse_infill_filament_id", "internal_solid_filament_id",
                             "top_surface_filament_id", "bottom_surface_filament_id" })
        config.set_deserialize_strict({ { key, std::to_string(filament_1based) } });
}

// A ready-to-slice firmware-managed IMEX plate. `firmware_managed` is the one thing that
// varies between a baseline slice and a shifted one.
static DynamicPrintConfig imex_config(bool firmware_managed)
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_printer(config);
    all_regions_on_filament(config, 1); // filament 1 => logical slot 0 => physical head 0
    config.set_deserialize_strict({ { "imex_firmware_managed_zones", firmware_managed ? "1" : "0" } });
    return config;
}

// The offset the Print works out for itself, with nothing pushed in. Applying the config is
// enough -- the derivation reads only the config and the objects, so this needs no slice.
static Vec2d derived_offset(const DynamicPrintConfig &config, const Vec3d &plate_origin = Vec3d::Zero())
{
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    print.set_plate_origin(plate_origin);
    print.update_imex_slice_offset();
    return print.get_imex_slice_offset();
}

// Slice one 20mm cube. Note what is NOT here: no offset is handed to the Print. Whatever shift
// the G-code comes out with, the Print derived on its own from `config`.
static std::string slice_cube(const DynamicPrintConfig &config, const Vec3d &plate_origin = Vec3d::Zero())
{
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    print.set_plate_origin(plate_origin);
    return gcode(print);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Derivation
// ---------------------------------------------------------------------------------------------

// The bug this file exists for: the offset used to arrive only from PartPlate, which a headless
// slice never runs, so `--slice` on a firmware-managed plate emitted slicer-managed coordinates
// while the mode G-code told the firmware to fan copies out from them. Nothing pushes anything
// here; the Print is expected to reach the primary zone's centre from the config alone.
TEST_CASE("A firmware-managed IMEX plate derives its slice offset with nothing pushed in",
          "[IMEXSliceOffset][IMEX]")
{
    const Vec2d offset = derived_offset(imex_config(true));

    CHECK_THAT(offset.x(), WithinAbs(kPrimaryZoneCentre.x(), 1e-9));
    CHECK_THAT(offset.y(), WithinAbs(kPrimaryZoneCentre.y(), 1e-9));
}

// The offset is a PLATE-LOCAL quantity. Both consumers already subtract the plate origin
// separately, so an offset that moved with the plate would subtract it twice and put every
// plate after the first a full plate stride out. This is precisely what the old plater-side
// computation got wrong: it divided the plate's world-frame outline, not the bed.
TEST_CASE("The derived IMEX slice offset is plate-local and does not move with the plate origin",
          "[IMEXSliceOffset][IMEX]")
{
    const DynamicPrintConfig config = imex_config(true);

    const Vec2d at_origin = derived_offset(config);
    const Vec2d on_plate3 = derived_offset(config, Vec3d(kBedWidth * 2.0, -kBedDepth * 2.0, 0.0));

    CHECK_THAT(on_plate3.x(), WithinAbs(at_origin.x(), 1e-9));
    CHECK_THAT(on_plate3.y(), WithinAbs(at_origin.y(), 1e-9));
}

// The offset tracks the bed it divides, so it is not a constant that happens to match one
// printer. Halving the bed halves the primary zone and its centre with it.
TEST_CASE("The derived IMEX slice offset follows the printable area", "[IMEXSliceOffset][IMEX]")
{
    DynamicPrintConfig config = imex_config(true);
    config.set_deserialize_strict({ { "printable_area", "0x0,150x0,150x100,0x100" } });

    const Vec2d offset = derived_offset(config);

    CHECK_THAT(offset.x(), WithinAbs(kPrimaryZoneCentre.x() / 2.0, 1e-9));
    CHECK_THAT(offset.y(), WithinAbs(kPrimaryZoneCentre.y() / 2.0, 1e-9));
}

// Every case that must leave the offset at exactly Vec2d::Zero(). This is what protects
// existing users: an exactly-zero offset is what makes the firmware-managed path reduce to the
// old set_gcode_offset() behaviour, to the last digit.
TEST_CASE("The derived IMEX slice offset is zero unless firmware-managed zones are in play",
          "[IMEXSliceOffset][IMEX]")
{
    // A Print nobody has applied anything to starts at zero. This is the value every
    // non-IMEX printer keeps, and the reason nothing else in the exporter had to change.
    Print fresh;
    REQUIRE_THAT(fresh.get_imex_slice_offset().x(), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(fresh.get_imex_slice_offset().y(), WithinAbs(0.0, 1e-12));

    Vec2d offset = Vec2d::Zero();

    SECTION("an ordinary printer with no IMEX configuration at all") {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "sparse_infill_density",      "0%" },
            { "layer_height",               "0.2" },
            { "initial_layer_print_height", "0.2" },
        });
        offset = derived_offset(config);
    }
    SECTION("an IMEX printer in a parallel mode, but firmware-managed zones off") {
        offset = derived_offset(imex_config(false));
    }
    SECTION("firmware-managed zones on, but the plate is in Primary mode") {
        DynamicPrintConfig config = imex_config(true);
        config.set_deserialize_strict({ { "imex_parallel_mode", "primary" } });
        offset = derived_offset(config);
    }
    SECTION("firmware-managed zones on, but the plate has no mode at all") {
        DynamicPrintConfig config = imex_config(true);
        config.set_deserialize_strict({ { "imex_parallel_mode", "" } });
        offset = derived_offset(config);
    }
    // A plate can name a mode this printer does not define -- a mode renamed or deleted after
    // the plate was set to it, or a project opened against a different printer preset. The
    // exporter falls back to Primary and warns; the offset has to make the same choice, or the
    // file would be shifted for a mode nothing ever activates.
    SECTION("firmware-managed zones on, but the plate's mode is not one this printer defines") {
        DynamicPrintConfig config = imex_config(true);
        config.set_deserialize_strict({ { "imex_parallel_mode", "renamed-since" } });
        offset = derived_offset(config);
    }
    // The layout hangs off the primary tool's cell. `imex_tools_per_gantry` 1 on a single
    // gantry is a one-cell grid: there is nothing to divide, so there is nothing to shift by.
    SECTION("firmware-managed zones on, but the tool grid holds a single tool") {
        DynamicPrintConfig config = imex_config(true);
        config.set_deserialize_strict({ { "imex_tools_per_gantry",  "1" },
                                        { "imex_mode_active_tools", "0:P;0:P" } });
        offset = derived_offset(config);
    }

    CHECK_THAT(offset.x(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(offset.y(), WithinAbs(0.0, 1e-12));
}

// ---------------------------------------------------------------------------------------------
// Consumption
// ---------------------------------------------------------------------------------------------

// The whole point of the firmware-managed path: the emitted toolpaths move, and they move by the
// derived offset. The writer subtracts plate origin + IMEX shift from every point it formats, so
// the firmware-managed slice comes out at (baseline - offset). Asserting the shift rather than
// absolute coordinates keeps this independent of wherever the arranger drops the cube.
TEST_CASE("A firmware-managed IMEX plate emits coordinates shifted by the offset it derived",
          "[IMEXSliceOffset][IMEX]")
{
    const XYBounds baseline = perimeter_bounds(slice_cube(imex_config(false)));
    const XYBounds shifted  = perimeter_bounds(slice_cube(imex_config(true)));
    REQUIRE_FALSE(baseline.empty());
    REQUIRE_FALSE(shifted.empty());

    CHECK_THAT(shifted.min_x, WithinAbs(baseline.min_x - kPrimaryZoneCentre.x(), 1e-3));
    CHECK_THAT(shifted.max_x, WithinAbs(baseline.max_x - kPrimaryZoneCentre.x(), 1e-3));
    CHECK_THAT(shifted.min_y, WithinAbs(baseline.min_y - kPrimaryZoneCentre.y(), 1e-3));
    CHECK_THAT(shifted.max_y, WithinAbs(baseline.max_y - kPrimaryZoneCentre.y(), 1e-3));

    // A translation, not a re-slice: the plate keeps its size.
    CHECK_THAT(shifted.size_x(), WithinAbs(baseline.size_x(), 1e-3));
    CHECK_THAT(shifted.size_y(), WithinAbs(baseline.size_y(), 1e-3));
}

// The IMEX shift is added to the plate origin, not substituted for it. A multi-plate project
// already carries a non-zero plate origin, so dropping either term from
// set_gcode_offset_with_imex_shift() would put every emitted coordinate on the wrong plate --
// invisibly to a test that only ever slices plate 1 at the origin. The expected total is
// origin + offset precisely because the derived offset does NOT itself contain the origin.
TEST_CASE("An IMEX slice offset composes with the plate origin rather than replacing it",
          "[IMEXSliceOffset][IMEX]")
{
    const Vec3d plate_origin(kBedWidth * 1.1, -kBedDepth * 1.1, 0.0);

    const XYBounds baseline = perimeter_bounds(slice_cube(imex_config(false)));
    const XYBounds shifted  = perimeter_bounds(slice_cube(imex_config(true), plate_origin));
    REQUIRE_FALSE(baseline.empty());
    REQUIRE_FALSE(shifted.empty());

    const double expected_x = plate_origin.x() + kPrimaryZoneCentre.x();
    const double expected_y = plate_origin.y() + kPrimaryZoneCentre.y();
    CHECK_THAT(shifted.min_x, WithinAbs(baseline.min_x - expected_x, 1e-3));
    CHECK_THAT(shifted.max_x, WithinAbs(baseline.max_x - expected_x, 1e-3));
    CHECK_THAT(shifted.min_y, WithinAbs(baseline.min_y - expected_y, 1e-3));
    CHECK_THAT(shifted.max_y, WithinAbs(baseline.max_y - expected_y, 1e-3));
}

// The guard for everyone who is not using this feature. `imex_firmware_managed_zones` is read
// by nothing else in the engine, so setting it on a printer the derivation refuses to shift must
// leave the slice where it was -- it must short-circuit on `is_imex` before it ever looks at a
// bed or a mode. The zero-offset sections above cover an ordinary printer that never mentions
// the option; this covers the option turned ON where nothing may act on it.
//
// Asserted as the derived offset plus the extent of the emitted perimeters, NOT as a line-by-line
// comparison of the two exports. This slicer's output is not stable run to run -- parallel infill
// generation is the documented source -- and the M73 time estimates, the seam placement and the
// travel ordering all ride on that variation, so comparing every emitted command would flake in
// CI rather than catch a shift. The offset is applied at emission as a pure translation, so a
// non-zero one moves the extent and this catches it; that it is exactly zero is what the
// derivation check states.
TEST_CASE("Turning on firmware-managed zones changes nothing on a non-IMEX printer",
          "[IMEXSliceOffset][IMEX]")
{
    auto ordinary_printer = [](bool firmware_managed) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "sparse_infill_density",         "0%" },
            { "top_shell_layers",              "0" },
            { "bottom_shell_layers",           "0" },
            { "wall_loops",                    "2" },
            { "layer_height",                  "0.2" },
            { "initial_layer_print_height",    "0.2" },
            { "skirt_loops",                   "0" },
            { "brim_type",                     "no_brim" },
            { "enable_prime_tower",            "0" },
            { "imex_firmware_managed_zones",   firmware_managed ? "1" : "0" },
        });
        return config;
    };

    // Nothing to apply in the first place.
    const Vec2d offset = derived_offset(ordinary_printer(true));
    CHECK_THAT(offset.x(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(offset.y(), WithinAbs(0.0, 1e-12));

    // ...and the toolpaths bear that out: same plate, same place. Infill, skirt, brim and the
    // prime tower are all off in this config, so the perimeters carry the whole extent.
    const XYBounds baseline  = perimeter_bounds(slice_cube(ordinary_printer(false)));
    const XYBounds with_flag = perimeter_bounds(slice_cube(ordinary_printer(true)));
    REQUIRE_FALSE(baseline.empty());
    REQUIRE_FALSE(with_flag.empty());

    CHECK_THAT(with_flag.min_x, WithinAbs(baseline.min_x, 1e-3));
    CHECK_THAT(with_flag.max_x, WithinAbs(baseline.max_x, 1e-3));
    CHECK_THAT(with_flag.min_y, WithinAbs(baseline.min_y, 1e-3));
    CHECK_THAT(with_flag.max_y, WithinAbs(baseline.max_y, 1e-3));
}

// first_layer_print_min/max are what a start-G-code template hands the firmware to declare the
// print area (bed mesh bounds, PRINT_MIN/PRINT_MAX). They come from the first-layer convex hull
// pushed through Print::translate_to_print_space(), which subtracts the same IMEX shift the
// writer does, so they have to travel with the toolpaths. If they did not, a firmware-managed
// plate would probe one area and print in another.
TEST_CASE("first_layer_print_min/max track the derived IMEX slice offset",
          "[IMEXSliceOffset][IMEX]")
{
    auto with_markers = [](bool firmware_managed) {
        DynamicPrintConfig config = imex_config(firmware_managed);
        config.set_deserialize_strict({
            { "machine_start_gcode",
              ";FLMIN:{first_layer_print_min[0]},{first_layer_print_min[1]}\n"
              ";FLMAX:{first_layer_print_max[0]},{first_layer_print_max[1]}\n" },
        });
        return config;
    };

    const std::string baseline_gcode = slice_cube(with_markers(false));
    const std::string shifted_gcode  = slice_cube(with_markers(true));

    Vec2d baseline_min, baseline_max, shifted_min, shifted_max;
    REQUIRE(read_marker(baseline_gcode, ";FLMIN:", baseline_min));
    REQUIRE(read_marker(baseline_gcode, ";FLMAX:", baseline_max));
    REQUIRE(read_marker(shifted_gcode, ";FLMIN:", shifted_min));
    REQUIRE(read_marker(shifted_gcode, ";FLMAX:", shifted_max));

    CHECK_THAT(shifted_min.x(), WithinAbs(baseline_min.x() - kPrimaryZoneCentre.x(), 1e-3));
    CHECK_THAT(shifted_min.y(), WithinAbs(baseline_min.y() - kPrimaryZoneCentre.y(), 1e-3));
    CHECK_THAT(shifted_max.x(), WithinAbs(baseline_max.x() - kPrimaryZoneCentre.x(), 1e-3));
    CHECK_THAT(shifted_max.y(), WithinAbs(baseline_max.y() - kPrimaryZoneCentre.y(), 1e-3));

    // Declared area and toolpaths must be in ONE frame. The declared bounds come from the
    // first-layer convex hull, which Print::first_layer_islands() builds from the object's SLICE
    // CONTOUR (lslices) -- not from any extrusion path -- so it sits outside the emitted
    // centrelines by whatever wall geometry lies between the two. That distance is a property of
    // the wall generator, not of this feature, so it is not asserted as a constant here.
    //
    // What IS asserted: the declared bounds enclose the toolpaths, and the gap between the two is
    // the SAME in both frames. A frame divergence -- one of the two consumers shifted, the other
    // not -- moves the declared box relative to the toolpaths and breaks this by the offset.
    const XYBounds baseline_layer = perimeter_bounds(baseline_gcode, 0.3); // initial_layer_print_height 0.2
    const XYBounds shifted_layer  = perimeter_bounds(shifted_gcode, 0.3);
    REQUIRE_FALSE(baseline_layer.empty());
    REQUIRE_FALSE(shifted_layer.empty());

    CHECK(shifted_min.x() <= shifted_layer.min_x + 1e-3);
    CHECK(shifted_min.y() <= shifted_layer.min_y + 1e-3);
    CHECK(shifted_max.x() >= shifted_layer.max_x - 1e-3);
    CHECK(shifted_max.y() >= shifted_layer.max_y - 1e-3);

    CHECK_THAT(shifted_layer.min_x - shifted_min.x(),
               WithinAbs(baseline_layer.min_x - baseline_min.x(), 1e-3));
    CHECK_THAT(shifted_layer.min_y - shifted_min.y(),
               WithinAbs(baseline_layer.min_y - baseline_min.y(), 1e-3));
    CHECK_THAT(shifted_max.x() - shifted_layer.max_x,
               WithinAbs(baseline_max.x() - baseline_layer.max_x, 1e-3));
    CHECK_THAT(shifted_max.y() - shifted_layer.max_y,
               WithinAbs(baseline_max.y() - baseline_layer.max_y, 1e-3));
}
