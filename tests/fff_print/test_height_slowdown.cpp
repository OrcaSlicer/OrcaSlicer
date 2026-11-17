#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/GCode/HeightSlowdown.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
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

TEST_CASE("Height slowdown ramps the travel acceleration on its own", "[HeightSlowdown]")
{
    const auto at = [](double z) { return height_slowdown_factors(z, 1., 11., 1., 0.5, 0.2); };

    CHECK_THAT(at(1.).travel_acceleration, WithinAbs(1., 1e-9));
    CHECK_THAT(at(6.).travel_acceleration, WithinAbs(0.6, 1e-9));
    CHECK_THAT(at(11.).travel_acceleration, WithinAbs(0.2, 1e-9));
    CHECK_THAT(at(11.).motion, WithinAbs(0.5, 1e-9));
    // The travel acceleration is never raised, and is not changed unless asked for.
    CHECK(height_slowdown_factors(11., 1., 11., 1., 0.5, 1.5).travel_acceleration == 1.);
    CHECK(height_slowdown_factors(11., 1., 11., 1., 0.5).travel_acceleration == 1.);
}

TEST_CASE("Height slowdown does not go below the minimum unless the value was below it already", "[HeightSlowdown]")
{
    CHECK_THAT(height_slowdown_limited(25., 50., 40.), WithinAbs(40., 1e-9));
    CHECK_THAT(height_slowdown_limited(45., 50., 40.), WithinAbs(45., 1e-9));
    CHECK_THAT(height_slowdown_limited(15., 30., 40.), WithinAbs(30., 1e-9));
    CHECK_THAT(height_slowdown_limited(25., 50., 0.), WithinAbs(25., 1e-9));
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

std::map<double, double> slice_cube_feedrates(bool slowdown, bool travel = false, const std::vector<std::pair<std::string, std::string>> &extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "automatic_height_slowdown", slowdown ? "1" : "0" },
        { "height_slowdown_initial_layers", "3" },
        { "height_slowdown_start_speed", "100" },
        { "height_slowdown_end_speed", "50" },
        { "height_slowdown_end_height", "20" }, // the top of the cube
    });
    for (const auto &[key, value] : extra)
        config.set_deserialize_strict(key, value);
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

TEST_CASE("Height slowdown keeps the speed above the minimum speed", "[HeightSlowdown]")
{
    const std::map<double, double> off = slice_cube_feedrates(false);
    const std::map<double, double> on  = slice_cube_feedrates(true);
    // A minimum above every normal speed keeps all of them, as they are below it already.
    const std::map<double, double> kept = slice_cube_feedrates(true, false, {{ "height_slowdown_min_speed", "1000" }});
    REQUIRE(off.size() == kept.size());
    CHECK_THAT(kept.rbegin()->second, WithinAbs(off.rbegin()->second, 1e-6));
    CHECK(on.rbegin()->second < kept.rbegin()->second);
}

TEST_CASE("Height slowdown can be set for a single object", "[HeightSlowdown]")
{
    const auto top_layer_feedrate = [](bool global, bool object) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "layer_height", "0.2" },
            { "initial_layer_print_height", "0.2" },
            { "automatic_height_slowdown", global ? "1" : "0" },
            { "height_slowdown_initial_layers", "3" },
            { "height_slowdown_start_speed", "100" },
            { "height_slowdown_end_speed", "50" },
            { "height_slowdown_end_height", "20" },
        });
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{ "automatic_height_slowdown", object ? "1" : "0" }}};
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{ cube(20) }, print, model, config, &overrides);
        PrintConfig print_config;
        print_config.apply(config, true);
        return top_feedrate_per_layer(print_config, gcode(print)).rbegin()->second;
    };

    const double normal = top_layer_feedrate(false, false);
    // The object setting wins over the project setting, in both directions.
    CHECK(top_layer_feedrate(false, true) < 0.75 * normal);
    CHECK_THAT(top_layer_feedrate(true, false), WithinAbs(normal, 1e-6));
}

TEST_CASE("Height slowdown hardly slows down a low object by default", "[HeightSlowdown]")
{
    // With the end height left at 0, the ramp ends at the printable height of the printer, so a 20 mm cube only gets a small part of it.
    const std::map<double, double> off     = slice_cube_feedrates(false);
    const std::map<double, double> default_height = slice_cube_feedrates(true, false, {{ "height_slowdown_end_height", "0" }});
    const std::map<double, double> cube_height    = slice_cube_feedrates(true);
    CHECK(default_height.rbegin()->second > 0.9 * off.rbegin()->second);
    CHECK(cube_height.rbegin()->second < 0.75 * off.rbegin()->second);
}
