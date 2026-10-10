#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <cstddef>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

float processed_belt_tilt(const std::string &gcode)
{
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode;
    }
    GCodeProcessor proc;
    proc.apply_config(FullPrintConfig{});
    proc.process_file(temp.string());
    return proc.get_result().belt_tilt_angle;
}

constexpr const char *body = "G1 X10 Y10 Z0.2 F3000\nG1 X20 Y10 E1 F1200\n";

} // namespace

TEST_CASE("The config block's belt angle does not mark G-code as belt G-code", "[GCodeProcessor][belt]")
{
    // Every printer's config block lists belt_slice_rotation_angle (default 45), belt or not.
    const std::string gcode = std::string("; CONFIG_BLOCK_START\n; belt_printer = 0\n; belt_slice_rotation_angle = 45\n; CONFIG_BLOCK_END\n") + body;
    CHECK_THAT(processed_belt_tilt(gcode), WithinAbs(0., 1e-6));
}

TEST_CASE("The belt header's angle marks G-code as belt G-code", "[GCodeProcessor][belt]")
{
    const std::string gcode = std::string("; belt_slice_rotation_angle = -45.0\n") + body +
                              "; CONFIG_BLOCK_START\n; belt_printer = 1\n; belt_slice_rotation_angle = -45\n; CONFIG_BLOCK_END\n";
    CHECK_THAT(processed_belt_tilt(gcode), WithinAbs(45., 1e-6));
}

TEST_CASE("Non-belt start G-code moves keep the first-layer Z in the processor", "[GCodeProcessor]")
{
    // The belt path (GCodeWriter tests: "start-gcode prepare-stage moves keep their real Z")
    // stores the real Z of a move inside the start G-code. Every other printer must keep
    // the historical behaviour: a prepare-stage move is pinned to the first-layer height
    // so the preview does not draw the start sequence's travel. The gate is the belt
    // header, so a file without one, whatever its config block says, takes this path.
    struct BBLPrinterGuard {
        bool prev = GCodeProcessor::s_IsBBLPrinter;
        BBLPrinterGuard()  { GCodeProcessor::s_IsBBLPrinter = false; }
        ~BBLPrinterGuard() { GCodeProcessor::s_IsBBLPrinter = prev; }
    } bbl_guard;

    const std::string gcode =
        "G90\n"
        "G21\n"
        "M83\n"
        ";TYPE:Custom\n"
        "G1 E-1.5 F2100\n"
        "G1 X45 Y0.3 Z50 F12000\n"   // prepare-stage travel to a high Z
        "G1 E1.5 F1800\n"
        ";TYPE:Outer wall\n"
        "G1 X46 Y0.3 Z50 E0.05\n"
        "; CONFIG_BLOCK_START\n; belt_printer = 0\n; belt_slice_rotation_angle = 45\n; CONFIG_BLOCK_END\n";

    GCodeProcessor processor;
    FullPrintConfig config;
    config.initial_layer_print_height.value = 0.3;
    processor.apply_config(config);
    processor.process_buffer(gcode);
    const GCodeProcessorResult &result = processor.get_result();
    REQUIRE_THAT(result.belt_tilt_angle, WithinAbs(0., 1e-6));

    size_t first_extrude = result.moves.size();
    for (size_t i = 0; i < result.moves.size(); ++i)
        if (result.moves[i].type == EMoveType::Extrude) { first_extrude = i; break; }
    REQUIRE(first_extrude < result.moves.size());
    REQUIRE(first_extrude > 0);

    // The extrusion keeps its real Z; the prepare-stage move before it is pinned to the
    // first-layer height.
    CHECK_THAT(result.moves[first_extrude].position.z(),     WithinAbs(50., 1e-3));
    CHECK_THAT(result.moves[first_extrude - 1].position.z(), WithinAbs(0.3, 1e-3));
}
