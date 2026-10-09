#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <catch2/catch_message.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;

TEST_CASE("Klipper object labels name each copy without the characters Klipper cannot parse", "[GCode]")
{
    const auto [name, label] = GENERATE(table<std::string, std::string>({
        {"my part (2)", "my_part_2"},
        {"(cube)", "cube"},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"exclude_object", "1"}});
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.)}, print, model, config, nullptr, false, 2);
    model.objects.front()->name = name;
    arrange_objects(model, BoundingBox{Point::new_scale(0., 0.), Point::new_scale(500., 500.)},
                    ArrangeParams{scaled(min_object_distance(config))});
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    for (const char *copy : {"0", "1"}) {
        const std::string instance_label = label + "_id_0_copy_" + copy;
        INFO(instance_label);
        CHECK(gcode.find("EXCLUDE_OBJECT_DEFINE NAME=" + instance_label + " ") != std::string::npos);
        CHECK(gcode.find("EXCLUDE_OBJECT_START NAME=" + instance_label + "\n") != std::string::npos);
    }
}

namespace {

constexpr double filament_diameter = 1.75;
constexpr double max_volumetric_speed = 20.;
constexpr double layer_height = 0.2;

// A 20mm cube sliced at 0.2mm layers with volumetric speeds `enabled` and `extra` applied last.
std::string volumetric_speed_gcode(bool enabled, std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_volumetric_speeds", enabled ? "1" : "0"},
        {"filament_diameter", filament_diameter},
        {"filament_max_volumetric_speed", max_volumetric_speed},
        {"layer_height", layer_height},
        {"initial_layer_print_height", layer_height},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"},
        {"enable_arc_fitting", "0"},
    });
    config.set_deserialize_strict(extra);
    return Test::slice({Test::cube(20.)}, config);
}

// For each extrusion move longer than 1mm of `role` above the first layer: its feed rate (mm/s) and extruded flow (mm³/s).
std::vector<std::pair<double, double>> role_speeds_and_flows(const std::string &gcode, std::string_view role)
{
    const double filament_area = 0.25 * M_PI * filament_diameter * filament_diameter;
    std::vector<std::pair<double, double>> moves;
    bool in_role = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find("TYPE:") != std::string_view::npos)
            in_role = comment.substr(comment.find("TYPE:") + 5) == role;
        if (in_role && line.extruding(self) && line.dist_XY(self) > 1. && line.new_Z(self) > 1.5 * layer_height) {
            const double speed = line.new_F(self) / Test::MM_PER_MIN;
            moves.emplace_back(speed, line.dist_E(self) * filament_area / line.dist_XY(self) * speed);
        }
    });
    return moves;
}

} // namespace

TEST_CASE("Volumetric speeds print each feature at its configured flow", "[GCode]")
{
    const auto [key, role] = GENERATE(table<std::string, std::string>({
        {"outer_wall_volumetric_flow", "Outer wall"},
        {"inner_wall_volumetric_flow", "Inner wall"},
        {"sparse_infill_volumetric_flow", "Sparse infill"},
        {"internal_solid_infill_volumetric_flow", "Internal solid infill"},
        {"top_surface_volumetric_flow", "Top surface"},
    }));
    INFO(key);
    const double flow = 6.;
    const auto moves = role_speeds_and_flows(volumetric_speed_gcode(true, {{key, flow}}), role);

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves)
        CHECK_THAT(extruded_flow, Catch::Matchers::WithinRel(flow, 0.01));
}

TEST_CASE("A volumetric speed in percent is a share of the filament's max volumetric speed, which caps it", "[GCode]")
{
    const auto [value, expected_flow] = GENERATE(table<std::string, double>({
        {"50%", 0.5 * max_volumetric_speed},
        {"300%", max_volumetric_speed},
        {"40", max_volumetric_speed},
    }));
    INFO(value);
    const auto moves = role_speeds_and_flows(volumetric_speed_gcode(true, {{"outer_wall_volumetric_flow", value}}), "Outer wall");

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves)
        CHECK_THAT(extruded_flow, Catch::Matchers::WithinRel(expected_flow, 0.01));
}

TEST_CASE("Volumetric speeds are ignored while disabled", "[GCode]")
{
    const double outer_wall_speed = 50.;
    const auto moves = role_speeds_and_flows(
        volumetric_speed_gcode(false, {{"outer_wall_speed", outer_wall_speed}, {"outer_wall_volumetric_flow", "2"}}), "Outer wall");

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves)
        CHECK_THAT(speed, Catch::Matchers::WithinRel(outer_wall_speed, 0.001));
}

TEST_CASE("The max external volumetric speed limits outer walls but not inner walls", "[GCode]")
{
    // At 200mm/s both walls would extrude well above the external limit and below the filament limit.
    const double max_external = 5.;
    const std::string gcode = volumetric_speed_gcode(false, {{"filament_max_external_volumetric_speed", max_external},
                                                             {"outer_wall_speed", 200.},
                                                             {"inner_wall_speed", 200.}});
    const auto outer_wall = role_speeds_and_flows(gcode, "Outer wall");
    const auto inner_wall = role_speeds_and_flows(gcode, "Inner wall");

    REQUIRE_FALSE(outer_wall.empty());
    REQUIRE_FALSE(inner_wall.empty());
    for (const auto &[speed, extruded_flow] : outer_wall)
        CHECK_THAT(extruded_flow, Catch::Matchers::WithinRel(max_external, 0.01));
    for (const auto &[speed, extruded_flow] : inner_wall)
        CHECK(extruded_flow > 2. * max_external);
}

TEST_CASE("The max external volumetric speed limits top surfaces only when they are not ironed", "[GCode]")
{
    const auto [ironing_type, limited] = GENERATE(table<std::string, bool>({
        {"no ironing", true},
        {"top", false},
    }));
    INFO(ironing_type);
    const double max_external = 5.;
    const auto moves = role_speeds_and_flows(volumetric_speed_gcode(false, {{"filament_max_external_volumetric_speed", max_external},
                                                                            {"top_surface_speed", 200.},
                                                                            {"ironing_type", ironing_type}}),
                                             "Top surface");

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves) {
        if (limited)
            CHECK_THAT(extruded_flow, Catch::Matchers::WithinRel(max_external, 0.01));
        else
            CHECK(extruded_flow > 2. * max_external);
    }
}
