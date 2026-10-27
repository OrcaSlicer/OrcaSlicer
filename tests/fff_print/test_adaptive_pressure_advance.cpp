#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// The speeds, in mm/s, the adaptive pressure advance comments report for the extrusions above the first layer.
static std::vector<double> adaptive_pa_speeds_above_first_layer(const std::string &gcode)
{
    const std::string token = "; APA Speed Used: ";
    std::vector<double> speeds;
    int layer = 0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0)
            ++layer;
        else if (layer > 1 && line.rfind(token, 0) == 0)
            speeds.push_back(std::stod(line.substr(token.size())));
    }
    return speeds;
}

// The cooling buffer drops a G1 F that repeats the feedrate the preceding travel set, so the wall
// extrusion then inherits the travel feedrate without a G1 F line of its own.
TEST_CASE("Adaptive pressure advance reads the speed of an extrusion that inherits the travel feedrate", "[AdaptivePressureAdvance]")
{
    const int travel_speed = GENERATE(120, 150);
    DYNAMIC_SECTION("travel speed " << travel_speed) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "gcode_flavor",                      "marlin2" },
            { "gcode_comments",                    1 },
            { "enable_pressure_advance",           "1" },
            { "adaptive_pressure_advance",         "1" },
            { "adaptive_pressure_advance_model",   "0.08,1,4000\n0.04,20,4000\n0.08,1,100000\n0.04,20,100000" },
            { "pressure_advance",                  "0.08" },
            { "wall_loops",                        1 },
            { "sparse_infill_density",             0 },
            { "top_shell_layers",                  0 },
            { "bottom_shell_layers",               0 },
            { "skirt_loops",                       0 },
            { "brim_type",                         "no_brim" },
            { "initial_layer_speed",               30 },
            // the wall speed must not be capped by the volumetric limit
            { "filament_max_volumetric_speed",     "200" },
            { "outer_wall_speed",                  150 },
            { "inner_wall_speed",                  150 },
            { "travel_speed",                      travel_speed },
            { "initial_layer_travel_speed",        travel_speed },
            { "slow_down_for_layer_cooling",       "0" },
            { "slow_down_layer_time",              "0" },
            { "enable_overhang_speed",             "0" },
            { "wipe",                              "0" },
            { "retraction_length",                 "0" },
        });
        const std::string gcode = slice({ cube(20) }, config);

        const std::vector<double> speeds = adaptive_pa_speeds_above_first_layer(gcode);
        REQUIRE(!speeds.empty());
        // every wall above the first layer prints at the wall speed, whatever the travel speed
        for (const double speed : speeds)
            CHECK_THAT(speed, Catch::Matchers::WithinAbs(150.0, 0.01));
    }
}
