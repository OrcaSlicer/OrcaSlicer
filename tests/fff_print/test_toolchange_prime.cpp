#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "test_data.hpp"

#include <regex>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

struct PrimeBlock
{
    bool        first_use;
    bool        skipped;
    double      e;
    bool        after_pause;
    double      restore;
};

std::vector<PrimeBlock> prime_blocks(const std::string& gcode)
{
    std::vector<PrimeBlock> blocks;
    std::regex  start(";" + GCodeProcessor::Toolchange_Prime_Start_Tag + " T\\d+ P\\d+ L[0-9.]+( FIRST)?.*");
    // The processor drops a feedrate equal to the previous line's.
    std::regex  move("(; )?G1 E([0-9.]+)( F300)? ; prime");
    std::regex  restore("G1 E([0-9.]+)( F300)? ; restore retraction");
    std::istringstream in(gcode);
    std::string line;
    bool        in_block = false;
    std::smatch m;
    while (std::getline(in, line)) {
        if (std::regex_match(line, m, start)) {
            blocks.push_back({m[1].matched, false, 0., line.find("(after pause") != std::string::npos, -1.});
            in_block = true;
        } else if (in_block && std::regex_match(line, m, move)) {
            blocks.back().skipped = m[1].matched;
            blocks.back().e       = std::stod(m[2].str());
        } else if (in_block && std::regex_match(line, m, restore)) {
            blocks.back().restore = std::stod(m[1].str());
        } else if (line.rfind(";" + GCodeProcessor::Toolchange_Prime_End_Tag, 0) == 0)
            in_block = false;
    }
    return blocks;
}

DynamicPrintConfig two_tool_config()
{
    DynamicPrintConfig config = default_print_config();
    config.set_num_extruders(2);
    config.set_deserialize_strict({
        { "filament_diameter",            "1.75,1.75" }, // the filament count comes from this, as in a loaded preset
        { "single_extruder_multi_material", false }, // a toolchanger, like the U1
        { "enable_prime_tower",           false },
        { "use_relative_e_distances",     true },
        { "wall_filament",                1 },
        { "sparse_infill_filament",       2 },
        { "solid_infill_filament",        2 },
        { "toolchange_prime_enable",      true },
        { "toolchange_prime_gcode",       "G1 E{retract_restore_length} F300 ; restore retraction\nG1 E{prime_length} F300 ; prime\n" },
        { "retract_length_toolchange",    "10,10" },
        { "retraction_length",            "1,1" },
        { "toolchange_prime_length_min",  1 },
        { "toolchange_prime_length_max",  10 },
        { "toolchange_prime_idle_time",   36000 },
        { "toolchange_prime_skip_time",   0 },
    });
    return config;
}

} // namespace

TEST_CASE("Tool change prime is a step by idle time", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    SECTION("Short parks get the minimum") {
        std::vector<PrimeBlock> blocks = prime_blocks(slice({TestMesh::cube_20x20x20}, config));
        REQUIRE(blocks.size() > 2);
        // Only the second tool's first use gets the full length (the start G-code primes the first tool).
        size_t first_uses = 0;
        for (const PrimeBlock& block : blocks) {
            REQUIRE_FALSE(block.skipped);
            if (block.first_use) {
                ++first_uses;
                REQUIRE_THAT(block.e, Catch::Matchers::WithinAbs(10., 1e-3));
            } else
                REQUIRE_THAT(block.e, Catch::Matchers::WithinAbs(1., 1e-3));
        }
        REQUIRE(first_uses == 1);
    }
    SECTION("Parks of at least the idle time get the maximum") {
        config.set_deserialize_strict({{ "toolchange_prime_idle_time", 0.001 }});
        std::vector<PrimeBlock> blocks = prime_blocks(slice({TestMesh::cube_20x20x20}, config));
        REQUIRE(blocks.size() > 2);
        for (const PrimeBlock& block : blocks)
            REQUIRE_THAT(block.e, Catch::Matchers::WithinAbs(10., 1e-3));
    }
}

TEST_CASE("Tool change prime restores the retraction in the chute", "[ToolchangePrime]")
{
    std::string gcode = slice({TestMesh::cube_20x20x20}, two_tool_config());
    std::vector<PrimeBlock> blocks = prime_blocks(gcode);
    REQUIRE(blocks.size() > 2);
    // A reused tool was parked with the tool change retraction, which the block restores in the chute.
    for (const PrimeBlock& block : blocks)
        if (!block.first_use)
            REQUIRE_THAT(block.restore, Catch::Matchers::WithinAbs(10., 1e-3));

    // So the writer must not unretract the tool again on the part: no extrusion-only move longer than a
    // normal retraction between the end of a block and the next extrusion with XY movement.
    std::istringstream in(gcode);
    std::string        line;
    std::regex         e_only("G1 E([0-9.]+)( F[0-9.]+)?\\s*(;.*)?");
    std::smatch        m;
    bool               after_block = false;
    while (std::getline(in, line)) {
        if (line.rfind(";" + GCodeProcessor::Toolchange_Prime_End_Tag, 0) == 0)
            after_block = true;
        else if (after_block && std::regex_match(line, m, e_only)) {
            INFO(line);
            REQUIRE(std::stod(m[1].str()) <= 1. + 1e-3);
        } else if (after_block && line.rfind("G1 X", 0) == 0 && line.find(" E") != std::string::npos)
            after_block = false;
    }
}

TEST_CASE("Tool change prime is skipped below the skip time", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    config.set_deserialize_strict({{ "toolchange_prime_skip_time", 36000 }});
    std::vector<PrimeBlock> blocks = prime_blocks(slice({TestMesh::cube_20x20x20}, config));

    REQUIRE(blocks.size() > 2);
    for (const PrimeBlock& block : blocks) {
        REQUIRE(block.skipped == !block.first_use);
        // Only the prime is dropped: the retraction is still restored.
        REQUIRE(block.restore >= 0.);
    }
}

TEST_CASE("Tool change prime is not emitted for a single extruder multi material printer", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    config.set_deserialize_strict({{ "single_extruder_multi_material", true }});
    REQUIRE(prime_blocks(slice({TestMesh::cube_20x20x20}, config)).empty());
}

TEST_CASE("Tool change prime is off unless enabled", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    config.set_deserialize_strict({{ "toolchange_prime_enable", false }});
    REQUIRE(prime_blocks(slice({TestMesh::cube_20x20x20}, config)).empty());
}

TEST_CASE("Tool change prime is full length after a pause", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    // A pause on every layer: the slicer can't know how long it lasts, so every reused tool gets the full prime.
    config.set_deserialize_strict({{ "layer_change_gcode", "M600" }, { "toolchange_prime_skip_time", 36000 }});
    std::vector<PrimeBlock> blocks = prime_blocks(slice({TestMesh::cube_20x20x20}, config));

    REQUIRE(blocks.size() > 2);
    for (const PrimeBlock& block : blocks) {
        REQUIRE_FALSE(block.skipped);
        REQUIRE_THAT(block.e, Catch::Matchers::WithinAbs(10., 1e-3));
        if (!block.first_use)
            REQUIRE(block.after_pause);
    }
}

TEST_CASE("Tool change prime is left to the printer when timed on the printer", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    config.set_deserialize_strict({
        { "toolchange_prime_on_printer", true },
        { "toolchange_prime_skip_time",  36000 },
        { "toolchange_prime_gcode",      "SMART_TOOLCHANGE_PRIME TOOL=[next_extruder] PREV=[previous_extruder] "
                                         "FIRST={if prime_first_use}1{else}0{endif} RESTORE={retract_restore_length} "
                                         "MIN=[toolchange_prime_length_min] MAX=[toolchange_prime_length_max] "
                                         "SKIP=[toolchange_prime_skip_time] ; restore retraction\n" },
    });
    std::string gcode = slice({TestMesh::cube_20x20x20}, config);

    // No tags, so the slicer neither scales nor skips the block (the skip time would otherwise drop every prime).
    CHECK(prime_blocks(gcode).empty());
    std::regex  call("SMART_TOOLCHANGE_PRIME TOOL=(\\d) PREV=(\\d) FIRST=([01]) RESTORE=([0-9.]+) MIN=1 MAX=10 SKIP=36000");
    size_t      calls = 0, first_uses = 0;
    for (auto it = std::sregex_iterator(gcode.begin(), gcode.end(), call); it != std::sregex_iterator(); ++it, ++calls) {
        CHECK((*it)[1].str() != (*it)[2].str());
        first_uses += (*it)[3].str() == "1";
        if ((*it)[3].str() == "0")
            CHECK_THAT(std::stod((*it)[4].str()), Catch::Matchers::WithinAbs(10., 1e-3));
    }
    CHECK(calls > 2);
    CHECK(first_uses == 1);
}
