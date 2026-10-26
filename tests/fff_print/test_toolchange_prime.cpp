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
};

std::vector<PrimeBlock> prime_blocks(const std::string& gcode)
{
    std::vector<PrimeBlock> blocks;
    std::regex  start(";" + GCodeProcessor::Toolchange_Prime_Start_Tag + " T\\d+ P\\d+ L[0-9.]+( FIRST)?.*");
    std::regex  move("(; )?G1 E([0-9.]+) F300 ; prime");
    std::istringstream in(gcode);
    std::string line;
    bool        in_block = false;
    std::smatch m;
    while (std::getline(in, line)) {
        if (std::regex_match(line, m, start)) {
            blocks.push_back({m[1].matched, false, 0., line.find("(after pause") != std::string::npos});
            in_block = true;
        } else if (in_block && std::regex_match(line, m, move)) {
            blocks.back().skipped = m[1].matched;
            blocks.back().e       = std::stod(m[2].str());
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
        { "toolchange_prime_gcode",       "G1 E{prime_length} F300 ; prime\n" },
        { "toolchange_prime_length_min",  1 },
        { "toolchange_prime_length_max",  10 },
        { "toolchange_prime_idle_time",   36000 },
        { "toolchange_prime_skip_time",   0 },
    });
    return config;
}

} // namespace

TEST_CASE("Tool change prime is sized by idle time", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    std::vector<PrimeBlock> blocks = prime_blocks(slice({TestMesh::cube_20x20x20}, config));

    REQUIRE(blocks.size() > 2);
    // Only the second tool's first use gets the full length (the start G-code primes the first tool);
    // later primes after a short park are scaled toward the minimum.
    size_t first_uses = 0;
    for (const PrimeBlock& block : blocks) {
        REQUIRE_FALSE(block.skipped);
        if (block.first_use) {
            ++first_uses;
            REQUIRE_THAT(block.e, Catch::Matchers::WithinAbs(10., 1e-3));
        } else {
            REQUIRE(block.e >= 1.);
            REQUIRE(block.e < 10.);
        }
    }
    REQUIRE(first_uses == 1);
}

TEST_CASE("Tool change prime is skipped below the skip time", "[ToolchangePrime]")
{
    DynamicPrintConfig config = two_tool_config();
    config.set_deserialize_strict({{ "toolchange_prime_skip_time", 36000 }});
    std::vector<PrimeBlock> blocks = prime_blocks(slice({TestMesh::cube_20x20x20}, config));

    REQUIRE(blocks.size() > 2);
    for (const PrimeBlock& block : blocks)
        REQUIRE(block.skipped == !block.first_use);
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
