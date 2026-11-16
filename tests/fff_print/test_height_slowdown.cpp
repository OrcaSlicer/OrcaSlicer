#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <map>
#include <string>

#include "libslic3r/GCode/HeightSlowdown.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

TEST_CASE("Height slowdown factors follow the ramp", "[HeightSlowdown]")
{
    // The ramp runs from z = 1 to z = 11, from 120% to 50%.
    const auto at = [](double z) { return height_slowdown_factors(z, 1., 11., 1.2, 0.5); };

    CHECK(at(0.6).speed == 1.);
    CHECK(at(0.6).motion == 1.);
    CHECK_THAT(at(1.).speed, WithinAbs(1.2, 1e-9));
    CHECK_THAT(at(1.).motion, WithinAbs(1., 1e-9));
    CHECK_THAT(at(6.).speed, WithinAbs(0.85, 1e-9));
    CHECK_THAT(at(6.).motion, WithinAbs(0.75, 1e-9));
    CHECK_THAT(at(11.).speed, WithinAbs(0.5, 1e-9));
    CHECK_THAT(at(11.).motion, WithinAbs(0.5, 1e-9));

    // Acceleration is never raised, even when the ramp ends above 100%.
    CHECK(height_slowdown_factors(11., 1., 11., 1.2, 1.5).motion == 1.);
    // A ramp of a single layer gets the end speed.
    CHECK_THAT(height_slowdown_factors(1., 1., 1., 1.2, 0.5).speed, WithinAbs(0.5, 1e-9));
}

namespace {

// The highest feedrate of the extrusion moves (or of the travel moves) of every layer, by Z.
std::map<double, double> top_feedrate_per_layer(const PrintConfig &config, const std::string &gcode, bool travel = false)
{
    std::map<double, double> out;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&out, travel](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if ((travel ? line.travel() : line.extruding(self)) && line.dist_XY(self) > 0.) {
            double &top = out[double(int(self.z() * 1000. + 0.5)) / 1000.];
            top         = std::max(top, double(line.new_F(self)));
        }
    });
    return out;
}

std::map<double, double> slice_cube_feedrates(bool slowdown, bool travel = false)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "automatic_height_slowdown", slowdown ? "1" : "0" },
        { "height_slowdown_initial_layers", "3" },
        { "height_slowdown_start_speed", "100" },
        { "height_slowdown_end_speed", "50" },
    });
    const std::string gcode = slice({ cube(20) }, config);
    PrintConfig print_config;
    print_config.apply(config, true);
    return top_feedrate_per_layer(print_config, gcode, travel);
}

} // namespace

TEST_CASE("Height slowdown slows the upper layers only when enabled", "[HeightSlowdown]")
{
    const std::map<double, double> off = slice_cube_feedrates(false);
    const std::map<double, double> on  = slice_cube_feedrates(true);
    REQUIRE(off.size() == on.size());
    REQUIRE(off.size() > 10);

    // The first layers keep their speed. The last layer is clearly slower than without the feature.
    auto off_it = off.begin();
    auto on_it  = on.begin();
    for (int i = 0; i < 3; ++i, ++off_it, ++on_it)
        CHECK_THAT(on_it->second, WithinAbs(off_it->second, 1e-6));
    CHECK(on.rbegin()->second < 0.75 * off.rbegin()->second);

    // Travel moves to the printed paths are slowed as well.
    const std::map<double, double> travel_off = slice_cube_feedrates(false, true);
    const std::map<double, double> travel_on  = slice_cube_feedrates(true, true);
    REQUIRE_FALSE(travel_off.empty());
    REQUIRE_FALSE(travel_on.empty());
    CHECK(travel_on.rbegin()->second < 0.75 * travel_off.rbegin()->second);
}
