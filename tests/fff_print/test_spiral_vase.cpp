#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

#include "libslic3r/GCode/SpiralVase.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

namespace {

// The extrusion moves of `gcode`.
std::vector<GCodeReader::GCodeLine> extrusions(const PrintConfig &config, const std::string &gcode)
{
    std::vector<GCodeReader::GCodeLine> out;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&out](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self))
            out.push_back(line);
    });
    return out;
}

} // namespace

TEST_CASE("Spiral vase keeps the selected coordinate and extrusion precision", "[SpiralVase][Regression]")
{
    const bool high_precision = GENERATE(false, true);
    CAPTURE(high_precision);
    PrintConfig config;
    config.high_precision_gcode.value        = high_precision;
    config.use_relative_e_distances.value    = true;
    config.spiral_mode_smooth.value          = false;
    config.spiral_starting_flow_ratio.value  = 0.5;
    config.spiral_finishing_flow_ratio.value = 0.5;
    config.resolution.value                  = 0.001;

    SpiralVase vase(config);
    vase.process_layer("G1 X0 Y0 Z0.2\n", false);

    // The first spiral layer ramps Z up from 0.2 and the flow up from half. A third of the way along,
    // Z is 0.2 + 0.02 / 3 and E is 0.001234 * (0.5 + 0.5 / 3).
    vase.enable(true);
    const std::vector<GCodeReader::GCodeLine> first = extrusions(config,
        vase.process_layer("G1 Z0.22\nG1 X1 Y0 E0.001234 F3600\nG1 X1 Y2 E0.002468\n", false));
    REQUIRE(first.size() == 2);
    CHECK_THAT(first.front().precise_value(Z), Catch::Matchers::WithinAbs(high_precision ? 0.2067 : 0.207, 1e-9));
    CHECK_THAT(first.front().precise_value(E), Catch::Matchers::WithinAbs(high_precision ? 0.000823 : 0.00082, 1e-9));

    // The last layer is followed by a copy of its moves whose flow ramps down to half.
    // Halfway along, E is 0.001236 * (0.5 + 0.5 * 0.5).
    vase.enable(true);
    const std::vector<GCodeReader::GCodeLine> last = extrusions(config,
        vase.process_layer("G1 Z0.24\nG1 X1 Y0 E0.001236\nG1 X1 Y2 E0.002468\n", true));
    REQUIRE(last.size() == 4);
    CHECK_THAT(last[2].precise_value(E), Catch::Matchers::WithinAbs(high_precision ? 0.000927 : 0.00093, 1e-9));
}
