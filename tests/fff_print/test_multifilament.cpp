#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <cctype>
#include <iostream>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

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

// Regression for #14753: the single-extruder branch of set_extruder emits T0
// without a follow-up M104, so firmware/CFS temperature overrides on T-commands
// win. The fix emits M104 after T0, matching the multi-extruder branch.
// Uses 2 filaments (single-extruder MM) with all features on filament 1 (T0),
// so the single-extruder branch fires and emits a T0 toolchange at print start.
TEST_CASE("Single-extruder MM emits M104 after T0 toolchange", "[MultiFilament]")
{
    const int expected_temp = 240;
    const std::string gcode = slice({ cube(20) },
        multifilament_config(2, {
            { "nozzle_temperature",               std::to_string(expected_temp) + "," + std::to_string(expected_temp) },
            { "nozzle_temperature_initial_layer", std::to_string(expected_temp) + "," + std::to_string(expected_temp) },
            { "machine_start_gcode",              "" },
            { "filament_start_gcode",             "" },
            { "enable_prime_tower",               "0" },
        }));

    bool found_t0 = false;
    bool found_temp_after_t0 = false;
    int  temp_after_t0 = -1;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader&, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (!found_t0) {
            if (cmd == "T0")
                found_t0 = true;
        } else if (!found_temp_after_t0 && (cmd == "M104" || cmd == "M109")) {
            found_temp_after_t0 = true;
            float s_val = 0;
            if (line.has_value('S', s_val))
                temp_after_t0 = int(s_val);
        }
    });

    REQUIRE(found_t0);
    REQUIRE(found_temp_after_t0);
    REQUIRE(temp_after_t0 == expected_temp);
}
