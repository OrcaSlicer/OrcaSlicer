#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
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

static bool is_motion_command(std::string_view line)
{
    return line.substr(0, 3) == "G0 " || line.substr(0, 3) == "G1 " ||
           line.substr(0, 3) == "G2 " || line.substr(0, 3) == "G3 ";
}

static size_t xy_motion_count(std::string_view gcode)
{
    size_t count = 0;
    while (!gcode.empty()) {
        const size_t line_end = gcode.find('\n');
        std::string_view line = gcode.substr(0, line_end);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos)
            line.remove_prefix(first);
        if (is_motion_command(line) && (line.find(" X") != std::string_view::npos || line.find(" Y") != std::string_view::npos))
            ++count;
        if (line_end == std::string_view::npos)
            break;
        gcode.remove_prefix(line_end + 1);
    }
    return count;
}

static double max_motion_z(std::string_view gcode)
{
    double max_z = -std::numeric_limits<double>::infinity();
    while (!gcode.empty()) {
        const size_t line_end = gcode.find('\n');
        std::string_view line = gcode.substr(0, line_end);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos)
            line.remove_prefix(first);
        const size_t z_pos = is_motion_command(line) ? line.find(" Z") : std::string_view::npos;
        if (z_pos != std::string_view::npos) {
            const std::string value(line.substr(z_pos + 2));
            max_z = std::max(max_z, std::strtod(value.c_str(), nullptr));
        }
        if (line_end == std::string_view::npos)
            break;
        gcode.remove_prefix(line_end + 1);
    }
    return max_z;
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
static std::string slice_with_prime_tower(const DynamicPrintConfig &config, bool is_bbl_printer = false)
{
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.is_BBL_printer() = is_bbl_printer;
    print.apply(model, config);
    return gcode(print);
}

TEST_CASE("A Type 2 wipe tower reaches the object height for smooth timelapse", "[WipeTower][Timelapse]")
{
    DynamicPrintConfig config = multifilament_config(1, {
        { "enable_prime_tower", true },
        { "layer_height",       0.2 },
        { "timelapse_type",     "1" },
        { "wipe_tower_type",    "type2" },
    });

    Print print;
    init_and_process_print({ cube(10) }, print, config);

    REQUIRE(print.has_wipe_tower());
    const WipeTowerData &tower = print.wipe_tower_data();
    CHECK_THAT(tower.height, Catch::Matchers::WithinAbs(10., 1e-3));
    CHECK_THAT(tower.depth, Catch::Matchers::WithinAbs(WipeTower::get_limit_depth_by_height(10.), 1e-3));
}

TEST_CASE("A Type 2 smooth timelapse tower prints before object paths", "[WipeTower][Timelapse]")
{
    DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlSmooth));
    config.set_key_value("wipe_tower_type", new ConfigOptionEnum<WipeTowerType>(WipeTowerType::Type2));
    config.set_key_value("wipe_tower_no_sparse_layers", new ConfigOptionBool(true));
    config.set_key_value("time_lapse_gcode", new ConfigOptionString("; TIMELAPSE_SNAPSHOT"));

    const std::string gcode_output = slice_with_prime_tower(config);
    const std::string tower_start  = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string snapshot     = "; TIMELAPSE_SNAPSHOT";
    size_t            checked      = 0;
    size_t            infilled     = 0;
    size_t            layer_start  = gcode_output.find(snapshot);

    while (layer_start != std::string::npos) {
        const size_t layer_end = gcode_output.find(snapshot, layer_start + snapshot.size());
        const size_t tower     = gcode_output.find(tower_start, layer_start);
        size_t       object    = std::string::npos;
        for (const char *role : {"TYPE:Inner wall", "TYPE:Outer wall", "TYPE:Sparse infill", "TYPE:Internal solid infill",
                                 "TYPE:Bottom surface", "TYPE:Top surface", "FEATURE: Inner wall", "FEATURE: Outer wall",
                                 "FEATURE: Sparse infill", "FEATURE: Internal solid infill", "FEATURE: Bottom surface",
                                 "FEATURE: Top surface"}) {
            const size_t pos = gcode_output.find(role, layer_start);
            if (pos != std::string::npos && (object == std::string::npos || pos < object))
                object = pos;
        }
        const size_t infill = gcode_output.find("CP EMPTY GRID START", layer_start);
        const bool tower_on_layer  = tower != std::string::npos && (layer_end == std::string::npos || tower < layer_end);
        const bool object_on_layer = object != std::string::npos && (layer_end == std::string::npos || object < layer_end);
        if (infill != std::string::npos && (layer_end == std::string::npos || infill < layer_end))
            ++infilled;
        if (object_on_layer) {
            REQUIRE(tower_on_layer);
            CHECK(tower < object);
            ++checked;
        }
        layer_start = layer_end;
    }

    REQUIRE(checked > 1);
    REQUIRE(infilled > 1);
}

TEST_CASE("A smooth timelapse does not spiral back to the object before either wipe tower", "[WipeTower][Timelapse]")
{
    const WipeTowerType tower_type = GENERATE(WipeTowerType::Type1, WipeTowerType::Type2);
    INFO("wipe tower type: " << int(tower_type));

    DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlSmooth));
    config.set_key_value("wipe_tower_type", new ConfigOptionEnum<WipeTowerType>(tower_type));
    config.set_key_value("prime_tower_skip_points", new ConfigOptionBool(true));
    config.set_key_value("time_lapse_gcode", new ConfigOptionString(
        "G90\nG1 X5 Y195 F20000\nG1 Z25\n; TIMELAPSE_SNAPSHOT\nG1 X10 Y190 F20000\nG1 Z[layer_z]\n; TIMELAPSE_PATH_END"));
    config.option<ConfigOptionFloats>("z_hop", true)->values = {0.4, 0.4};
    config.option<ConfigOptionEnumsGeneric>("z_hop_types", true)->values = {zhtAuto, zhtAuto};

    const std::string gcode_output = slice_with_prime_tower(config);
    const std::string tower_start  = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string path_end     = "; TIMELAPSE_PATH_END";
    size_t            checked      = 0;
    size_t            pos          = gcode_output.find(path_end);

    while (pos != std::string::npos) {
        const size_t tower      = gcode_output.find(tower_start, pos + path_end.size());
        const size_t next_layer = gcode_output.find(path_end, pos + path_end.size());
        if (tower != std::string::npos && (next_layer == std::string::npos || tower < next_layer)) {
            const std::string_view approach = std::string_view(gcode_output).substr(pos + path_end.size(), tower - pos - path_end.size());
            if (checked > 0)
                CHECK(xy_motion_count(approach) == 1);
            ++checked;
        }
        pos = next_layer;
    }

    REQUIRE(checked > 1);
}

TEST_CASE("A smooth timelapse lift stays within the printable height", "[WipeTower][Timelapse]")
{
    constexpr double printable_height = 10.2;
    DynamicPrintConfig config = wipe_tower_toolchange_config("klipper");
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlSmooth));
    config.set_key_value("printable_height", new ConfigOptionFloat(printable_height));
    config.set_key_value("time_lapse_gcode", new ConfigOptionString(
        "; TIMELAPSE_PATH_START\nG2 Z{min(max_print_height, layer_z + 0.4)} I0.86 J0.86 P1 F20000\n"
        "; TIMELAPSE_SNAPSHOT\nG1 Z[layer_z]\n; TIMELAPSE_PATH_END"));
    config.option<ConfigOptionFloats>("z_hop", true)->values = {0.4, 0.4};
    config.option<ConfigOptionEnumsGeneric>("z_hop_types", true)->values = {zhtAuto, zhtAuto};

    const std::string gcode_output = slice_with_prime_tower(config);
    const std::string tower_start  = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string path_start   = "; TIMELAPSE_PATH_START";
    size_t            checked      = 0;
    size_t            pos          = gcode_output.find(path_start);

    while (pos != std::string::npos) {
        const size_t tower      = gcode_output.find(tower_start, pos + path_start.size());
        const size_t next_layer = gcode_output.find(path_start, pos + path_start.size());
        if (tower != std::string::npos && (next_layer == std::string::npos || tower < next_layer)) {
            const size_t layer_start = gcode_output.rfind(";LAYER_CHANGE", pos);
            REQUIRE(layer_start != std::string::npos);
            const std::string_view layer_gcode = std::string_view(gcode_output).substr(layer_start, tower - layer_start);
            INFO(std::string(layer_gcode));
            CHECK(max_motion_z(layer_gcode) <= printable_height);
            ++checked;
        }
        pos = next_layer;
    }

    REQUIRE(checked > 1);
}

TEST_CASE("A BBL timelapse retains its explicit final Z", "[WipeTower][Timelapse]")
{
    DynamicPrintConfig config = wipe_tower_toolchange_config("marlin");
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlSmooth));
    config.set_key_value("time_lapse_gcode", new ConfigOptionString("G1 Z25\n; BBL_TIMELAPSE_END"));

    const std::string gcode_output = slice_with_prime_tower(config, true);
    const std::string marker       = "; BBL_TIMELAPSE_END";
    const size_t      marker_pos   = gcode_output.find(marker);
    REQUIRE(marker_pos != std::string::npos);
    std::string_view following = std::string_view(gcode_output).substr(marker_pos + marker.size());
    const size_t first = following.find_first_not_of("\r\n");
    REQUIRE(first != std::string_view::npos);
    following.remove_prefix(first);
    CHECK(following.substr(0, 4) != "G1 Z");
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
