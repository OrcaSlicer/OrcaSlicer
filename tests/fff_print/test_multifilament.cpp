#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <cctype>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// A 1mm-thick upright plate: one wall loop per side plus a gap too narrow for any
// infill, so layers have gap fill (classic wall generator) and no sparse infill.
static TriangleMesh thin_plate() { return make_cube(20.0, 1.0, 5.0); }

// 0-based tool indices used by extrusions whose role comment contains `role` (needs gcode_comments).
static std::set<int> tools_for_role(const std::string& gcode, const std::string& role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// Tool index = filament id - 1; brim and skirt follow the wall filament.
TEST_CASE("Each feature prints with its assigned filament", "[MultiFilament]")
{
    auto [infill_filament, wall_filament] = GENERATE(table<int, int>({ {1, 1}, {1, 2}, {2, 1}, {2, 2} }));
    DYNAMIC_SECTION("infill filament " << infill_filament << ", wall filament " << wall_filament) {
        const std::string gcode = slice({ cube(20) },
            multifilament_config(2, {
                { "sparse_infill_filament_id",  infill_filament },
                { "internal_solid_filament_id", infill_filament },
                { "top_surface_filament_id",    infill_filament },
                { "bottom_surface_filament_id", infill_filament },
                { "outer_wall_filament_id",     wall_filament },
                { "inner_wall_filament_id",     wall_filament },
                { "skirt_loops",                1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            }));
        const std::set<int> wall_tool{ wall_filament - 1 };
        const std::set<int> infill_tool{ infill_filament - 1 };
        CHECK(tools_for_role(gcode, "perimeter") == wall_tool);
        CHECK(tools_for_role(gcode, "infill")    == infill_tool); // sparse + solid + top/bottom
        CHECK(tools_for_role(gcode, "brim")      == wall_tool);
        CHECK(tools_for_role(gcode, "skirt")     == wall_tool);
    }
}

TEST_CASE("Each feature prints with its assigned filament (three filaments)", "[MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(3, {
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
            { "outer_wall_filament_id",     3 },
            { "inner_wall_filament_id",     3 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 2 }); // filament 3
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 }); // filament 2
}

// Tool ordering may schedule a filament that ends up printing nothing on a layer
// (gap fill is classified as sparse infill when scheduling). The object-skip flush
// encoder must tolerate that instead of aborting the export with
// "Label object id error!".
TEST_CASE("Object-skip flush encoding survives a scheduled filament that prints nothing", "[MultiFilament][Regression]")
{
    Print print;
    Model model;
    init_print({ thin_plate() }, print, model,
        multifilament_config(2, {
            { "wall_generator",            "classic" },
            { "wall_loops",                1 },
            { "sparse_infill_filament_id", 2 },
            { "skirt_loops",               0 },
            { "brim_type",                 "no_brim" },
            { "support_object_skip_flush", 1 },
        }));
    print.is_BBL_printer() = true;
    std::string out;
    REQUIRE_NOTHROW(out = Test::gcode(print));
    REQUIRE(tools_by_feature(out).count("Gap infill") == 1); // geometry must exercise gap fill
}

// Gap fill is printed with the wall filament, so scheduling must not bring in the
// sparse-infill filament (a pointless filament change) on layers without sparse infill.
TEST_CASE("Gap fill does not activate the sparse infill filament", "[MultiFilament][Regression]")
{
    const std::string gcode = slice({ thin_plate() },
        multifilament_config(2, {
            { "wall_generator",            "classic" },
            { "wall_loops",                1 },
            { "sparse_infill_filament_id", 2 },
            { "skirt_loops",               0 },
            { "brim_type",                 "no_brim" },
        }));
    const auto features = tools_by_feature(gcode);
    REQUIRE(features.count("Gap infill") == 1);    // geometry must exercise gap fill
    REQUIRE(features.count("Sparse infill") == 0); // and produce no true sparse infill
    CHECK(features.at("Gap infill") == std::set<int>{ 0 });
    CHECK(selected_tools(gcode) == std::set<int>{ 0 }); // filament 2 never activated
}

// Redirecting a single feature to filament 2 must route exactly that feature's
// extrusions to tool 1, and every filament the print activates must extrude something.
// Concentric top/bottom surfaces leave gaps, so their fill collections also contain
// gap fill; classification of those mixed collections must agree between scheduling
// (ToolOrdering::collect_extruders) and assignment (LayerTools::extruder()).
TEST_CASE("A feature filament override routes only that feature and never activates an idle filament", "[MultiFilament][Regression]")
{
    const char* generator = GENERATE("classic", "arachne");
    auto [key, feature] = GENERATE(table<const char*, const char*>({
        { "sparse_infill_filament_id",  "Sparse infill" },
        { "internal_solid_filament_id", "Internal solid infill" },
        { "top_surface_filament_id",    "Top surface" },
        { "bottom_surface_filament_id", "Bottom surface" },
        { "outer_wall_filament_id",     "Outer wall" },
        { "inner_wall_filament_id",     "Inner wall" },
    }));
    DYNAMIC_SECTION(generator << " | " << key << " = 2") {
        const std::string gcode = slice({ TestMesh::cube_with_hole },
            multifilament_config(2, {
                { "wall_generator",      generator },
                { "gap_fill_target",     "everywhere" },
                { "top_surface_pattern", "concentric" },
                { "infill_wall_overlap", 0 },
                { key,                      2 },
                { "skirt_loops",            0 },
                { "brim_type",              "no_brim" },
            }));
        const auto features = tools_by_feature(gcode);
        REQUIRE(features.count("Gap infill") == 1); // geometry must exercise gap fill inside solid surfaces
        REQUIRE(features.count(feature) == 1);      // and must print the redirected feature

        // The redirected feature prints on tool 1.
        CHECK(features.at(feature) == std::set<int>{ 1 });

        // Every filament the print switches to extrudes something.
        std::set<int> extruding;
        for (const auto& [name, tools] : features)
            extruding.insert(tools.begin(), tools.end());
        const std::set<int> selected = selected_tools(gcode);
        CAPTURE(selected, extruding);
        CHECK(std::includes(extruding.begin(), extruding.end(), selected.begin(), selected.end()));
    }
}

// Gap fill generated inside a top surface (gap_fill_target) lands in the same extrusion
// collection as the surface fill, turning the collection's role into erMixed. Assignment
// must still follow the top-surface filament, not fall back to the internal-solid one,
// which would print the top surface with the wrong filament and leave the top-surface
// filament scheduled with nothing to print.
TEST_CASE("Top surface filament override survives gap fill inside the top surface", "[MultiFilament][Regression]")
{
    const char* generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(generator) {
        const std::string gcode = slice({ TestMesh::cube_with_hole },
            multifilament_config(2, {
                { "wall_generator",             generator },
                { "gap_fill_target",            "everywhere" },
                { "top_surface_pattern",        "concentric" },
                { "infill_wall_overlap",        0 },
                { "top_surface_filament_id",    2 },
                { "internal_solid_filament_id", 1 }, // explicit, so it does not inherit the top filament
                { "skirt_loops",                0 },
                { "brim_type",                  "no_brim" },
            }));
        const auto features = tools_by_feature(gcode);
        REQUIRE(features.count("Top surface") == 1);
        CHECK(features.at("Top surface") == std::set<int>{ 1 });
        CHECK(features.at("Internal solid infill") == std::set<int>{ 0 });

        // Every filament the print switches to extrudes something.
        std::set<int> extruding;
        for (const auto& [name, tools] : features)
            extruding.insert(tools.begin(), tools.end());
        const std::set<int> selected = selected_tools(gcode);
        CAPTURE(selected, extruding);
        CHECK(std::includes(extruding.begin(), extruding.end(), selected.begin(), selected.end()));
    }
}

// Scheduling used to request the inner wall filament whenever wall_loops > 1, but on
// geometry too narrow for a second loop no inner wall is ever printed, activating the
// filament for nothing (and, on object-skip-flush printers, aborting the export with
// "Label object id error!" -- reported as issue #14225).
TEST_CASE("Inner wall filament is not activated when only the outer loop fits", "[MultiFilament][Regression]")
{
    const std::string gcode = slice({ thin_plate() },
        multifilament_config(2, {
            { "wall_generator",         "classic" },
            { "wall_loops",             2 },
            { "inner_wall_filament_id", 2 },
            { "skirt_loops",            0 },
            { "brim_type",              "no_brim" },
        }));
    const auto features = tools_by_feature(gcode);
    REQUIRE(features.count("Outer wall") == 1);
    REQUIRE(features.count("Inner wall") == 0); // the plate only fits the outer loop
    CHECK(selected_tools(gcode) == std::set<int>{ 0 }); // filament 2 never activated
}

// The override must survive tool ordering: object 1's walls print on their filament's
// tool, object 0 stays on the first. If dropped, every wall prints on tool 0.
TEST_CASE("Per-object wall filament override is honored", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "skirt_loops",    0 },
            { "brim_type",      "no_brim" },
            { "print_sequence", "by object" },
        }),
        { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } } });
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 0 }); // infill not overridden: stays on F1
}

// max_layer_height can be shorter than the extruder count (normalization sizes it to the
// filament count under single_extruder_multi_material). calc_max_layer_height() in ToolOrdering
// indexed it per-nozzle and read past the end. Shortened directly here to isolate that read;
// the other per-extruder keys stay extruder-length so slicing reaches the code under test.
TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[MultiFilament]")
{
    DynamicPrintConfig config = multifilament_config(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4" },
        { "printer_extruder_id",       "1,2" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height", "0,0" },
        { "max_layer_height",          "0.3" }, // deliberately one entry short
    });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}
