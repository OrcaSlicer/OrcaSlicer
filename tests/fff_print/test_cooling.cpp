#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// The fan is held off for the first close_fan_the_first_x_layers layers, so an explicit
// fan-off command is emitted.
TEST_CASE("Fan is held off for the initial layers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "cooling",                      true },
        { "close_fan_the_first_x_layers", 5 },
    });
    CHECK(gcode.find("M106 S0") != std::string::npos);
}

// The cooling pass resolves and strips its internal speed placeholders; none leak into
// the final G-code.
TEST_CASE("Cooling consumes its internal speed markers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, { { "layer_height", 0.2 } });
    CHECK(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
}

TEST_CASE("Arc fitting remains active with overhang fan control", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_arc_fitting",             "enabled" },
        { "enable_overhang_bridge_fan",     true },
        { "enable_overhang_speed",          true },
        { "initial_layer_print_height",     0.2 },
        { "layer_height",                   0.2 },
        { "slow_down_for_layer_cooling",    false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    config.set_key_value("overhang_fan_threshold", new ConfigOptionEnumsGeneric{Overhang_threshold_2_4});
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER=[layer_num]"});

    const auto has_arc_after_first_layer = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line;
        size_t layers_seen = 0;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER=", 0) == 0)
                ++layers_seen;
            else if (layers_seen > 1 && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };
    const auto has_arc_with_overhang_fan = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line;
        bool overhang_fan_active = false;
        bool outer_wall = false;
        while (std::getline(input, line)) {
            if (line.rfind("M106 S255", 0) == 0)
                overhang_fan_active = true;
            else if (line.rfind("M106 S0", 0) == 0 || line.rfind("M107", 0) == 0)
                overhang_fan_active = false;
            else if (line.rfind(";TYPE:", 0) == 0)
                outer_wall = line == ";TYPE:Outer wall";
            else if (overhang_fan_active && outer_wall && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };

    const std::string with_overhang_speed = slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_after_first_layer(with_overhang_speed));

    config.set_deserialize_strict({ { "enable_overhang_speed", false } });
    const std::string without_overhang_speed = slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_after_first_layer(without_overhang_speed));

    const std::string uniform_overhang_gcode = slice({ make_sphere(50.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_with_overhang_fan(uniform_overhang_gcode));
}

TEST_CASE("Overhang fan transitions do not depend on overhang speed", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bridge_speed",                   2.0 },
        { "enable_arc_fitting",             "disabled" },
        { "enable_overhang_bridge_fan",     true },
        { "enable_overhang_speed",          false },
        { "initial_layer_print_height",     0.3 },
        { "inner_wall_speed",              30.0 },
        { "layer_height",                   0.3 },
        { "outer_wall_speed",              30.0 },
        { "overhang_1_4_speed",            "30" },
        { "overhang_2_4_speed",            "29" },
        { "overhang_3_4_speed",             "6" },
        { "overhang_4_4_speed",             "3" },
        { "slow_down_for_layer_cooling",    false },
        { "slowdown_for_curled_perimeters", false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    config.set_key_value("overhang_fan_threshold", new ConfigOptionEnumsGeneric{Overhang_threshold_2_4});
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER_Z=[layer_z]"});

    const auto fan_commands = [](const std::string &gcode) {
        std::vector<std::pair<std::string, std::string>> commands;
        std::istringstream input(gcode);
        std::string layer;
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER_Z=", 0) == 0)
                layer = line;
            else if (!layer.empty() && (line.rfind("M106", 0) == 0 || line.rfind("M107", 0) == 0))
                commands.emplace_back(layer, line);
        }
        return commands;
    };
    const auto feedrates = [](const std::string &gcode) {
        std::vector<std::string> values;
        std::istringstream input(gcode);
        std::string word;
        while (input >> word)
            if (!word.empty() && word.front() == 'F')
                values.push_back(word);
        return values;
    };

    constexpr double sphere_radius = 50.0; // 100 mm diameter.
    const std::string without_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);
    config.set_deserialize_strict({{"enable_overhang_speed", true}});
    const std::string with_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);

    const auto without_speed_fan = fan_commands(without_speed_gcode);
    const auto with_speed_fan = fan_commands(with_speed_gcode);
    const auto without_speed_feedrates = feedrates(without_speed_gcode);
    const auto with_speed_feedrates = feedrates(with_speed_gcode);

    REQUIRE_FALSE(without_speed_fan.empty());
    REQUIRE(std::any_of(without_speed_fan.begin(), without_speed_fan.end(),
                        [](const auto &command) { return command.second.find("S255") != std::string::npos; }));
    REQUIRE(with_speed_feedrates != without_speed_feedrates);
    CHECK(with_speed_fan == without_speed_fan);
}

// Per layer: the time of its XY moves at their feed rates, and the speeds of its outer wall extrusions, in mm/s.
struct LayerMoves
{
    double              time = 0.;
    std::vector<double> outer_wall_speeds;
};

static std::vector<LayerMoves> layer_moves(const std::string &gcode)
{
    std::vector<LayerMoves> layers;
    bool outer_wall = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers, &outer_wall](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment == "TEST_LAYER")
            layers.emplace_back();
        else if (comment.rfind("TYPE:", 0) == 0)
            outer_wall = comment == "TYPE:Outer wall";
        const double speed = line.new_F(self) / MM_PER_MIN;
        if (layers.empty() || speed <= 0. || !(line.cmd_is("G0") || line.cmd_is("G1")))
            return;
        layers.back().time += line.dist_XY(self) / speed;
        if (outer_wall && line.extruding(self))
            layers.back().outer_wall_speeds.push_back(speed);
    });
    return layers;
}

TEST_CASE("Layer cooling slowdown keeps outer walls out of the resonance avoidance range", "[Cooling]")
{
    constexpr double min_speed = 40.;
    constexpr double max_speed = 90.;
    // With inner walls and infill to slow down instead, the outer walls stay at the top of the range. A lone outer
    // wall cannot pass its time on, so it drops to the bottom; its layers are quick, hence the shorter layer time.
    const bool   lone_wall  = GENERATE(false, true);
    const double held_speed = lone_wall ? min_speed : max_speed;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bottom_shell_layers",           lone_wall ? 0 : 3 },
        { "dont_slow_down_outer_wall",     false },
        { "enable_arc_fitting",            "disabled" },
        { "enable_overhang_speed",         false },
        { "filament_max_volumetric_speed", 100 },
        { "initial_layer_print_height",    0.3 },
        // Below the range, so resonance avoidance leaves it alone.
        { "initial_layer_speed",           30 },
        { "layer_height",                  0.3 },
        { "max_resonance_avoidance_speed", max_speed },
        { "min_resonance_avoidance_speed", min_speed },
        { "outer_wall_speed",              200 },
        { "slow_down_for_layer_cooling",   true },
        { "slow_down_layer_time",          lone_wall ? 2 : 10 },
        { "slow_down_layers",              0 },
        { "slow_down_min_speed",           10 },
        { "sparse_infill_density",         lone_wall ? "0%" : "15%" },
        { "top_shell_layers",              lone_wall ? 0 : 3 },
        { "wall_loops",                    lone_wall ? 1 : 3 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER"});

    // A cone's layers shrink upwards, so the slowdown deepens through the range.
    config.set_deserialize_strict({ { "resonance_avoidance", false } });
    const std::vector<LayerMoves> without = layer_moves(slice({ make_cone(15., 15.) }, config));
    config.set_deserialize_strict({ { "resonance_avoidance", true } });
    const std::vector<LayerMoves> with = layer_moves(slice({ make_cone(15., 15.) }, config));

    std::vector<double> without_in_range;
    std::vector<double> with_in_range;
    bool                held = false;
    for (const LayerMoves &layer : without)
        for (double speed : layer.outer_wall_speeds)
            if (speed > min_speed + 0.5 && speed < max_speed - 0.5)
                without_in_range.push_back(speed);
    for (const LayerMoves &layer : with)
        for (double speed : layer.outer_wall_speeds) {
            if (speed > min_speed + 0.5 && speed < max_speed - 0.5)
                with_in_range.push_back(speed);
            held |= std::abs(speed - held_speed) < 0.1;
        }
    REQUIRE_FALSE(without_in_range.empty());
    CHECK(with_in_range.empty());
    CHECK(held);

    // No layer gets shorter than the slowdown made it without resonance avoidance.
    REQUIRE(with.size() == without.size());
    for (size_t i = 0; i < with.size(); ++i)
        CHECK(with[i].time > 0.995 * without[i].time);
}

// Speeds of the extruding moves, in mm/s, in print order.
static std::vector<double> extrusion_speeds(const std::string &gcode, const DynamicPrintConfig &config)
{
    std::vector<double> speeds;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(gcode, [&speeds](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.dist_XY(self) > 0.)
            speeds.push_back(line.new_F(self) / MM_PER_MIN);
    });
    return speeds;
}

// A line that only sets the feed rate takes no time, however the extrusion distances are written.
TEST_CASE("Layer cooling slowdown does not depend on relative extrusion distances", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        // High enough that the defaults print at their own speeds.
        { "filament_max_volumetric_speed", 100 },
        { "initial_layer_print_height",    0.3 },
        { "layer_height",                  0.3 },
        { "slow_down_for_layer_cooling",   true },
        // Longer than the cube's layers take, so they are slowed down.
        { "slow_down_layer_time",          10 },
        { "slow_down_min_speed",           1 },
    });
    config.set_deserialize_strict({ { "use_relative_e_distances", false } });
    const std::vector<double> absolute = extrusion_speeds(slice({ cube(20) }, config), config);
    config.set_deserialize_strict({ { "use_relative_e_distances", true } });
    const std::vector<double> relative = extrusion_speeds(slice({ cube(20) }, config), config);

    REQUIRE(relative.size() == absolute.size());
    double largest_difference = 0.;
    for (size_t i = 0; i < relative.size(); ++i)
        largest_difference = std::max(largest_difference, std::abs(relative[i] - absolute[i]));
    // Feed rates are written in whole mm/min.
    CHECK_THAT(largest_difference, Catch::Matchers::WithinAbs(0., 1. / MM_PER_MIN));
}
