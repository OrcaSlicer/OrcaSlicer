#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/calib.hpp"

using namespace Slic3r;

TEST_CASE("PA Line preserves selected parameters when regenerating G-code", "[Calib][PA Line][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", "klipper"},
        {"outer_wall_speed", "120"},
        {"outer_wall_acceleration", "4000"},
        {"machine_max_acceleration_extruding", "10000"},
        {"machine_max_acceleration_x", "10000"},
        {"machine_max_acceleration_y", "10000"},
        {"initial_layer_print_height", "0.2"},
        {"initial_layer_speed", "30"},
        {"layer_height", "0.2"},
        {"nozzle_diameter", "0.4"},
        {"filament_diameter", "1.75"},
        {"filament_flow_ratio", "1"},
        {"z_offset", "0"},
    });

    Model model;
    ModelObject* object = model.add_object("handle", "", make_cube(5, 5, 0.4));
    object->add_instance();

    Calib_Params params;
    params.mode          = CalibMode::Calib_PA_Line;
    params.start         = 0.02;
    params.end           = 0.03;
    params.step          = 0.005;
    params.print_numbers = false;

    CalibPressureAdvanceLine line(params, config, false, *object, Vec3d::Zero());
    const CustomGCode::Info initial = line.generate_custom_gcodes(config, false, *object, Vec3d::Zero());

    REQUIRE(initial.gcodes.size() == 2);
    CHECK(initial.gcodes.front().extra.find("SET_PRESSURE_ADVANCE ADVANCE=0.02") != std::string::npos);
    CHECK(initial.gcodes.front().extra.find("SET_PRESSURE_ADVANCE ADVANCE=0.03") != std::string::npos);
    CHECK(initial.gcodes.front().extra.find("ACCEL=4000") != std::string::npos);
    CHECK(initial.gcodes.front().extra.find("F7200") != std::string::npos);
    const std::string background = initial.gcodes.front().extra.substr(initial.gcodes.front().extra.find("Bottom surface"));
    CHECK(background.find("\nG1 F1800") != std::string::npos);
    CHECK(initial.gcodes.back().extra.find("Glyph:") != std::string::npos);

    config.set_deserialize_strict({
        {"initial_layer_print_height", "0.3"},
        {"layer_height", "0.24"},
        {"nozzle_diameter", "0.6"},
    });
    const CustomGCode::Info regenerated = line.generate_custom_gcodes(config, false, *object, Vec3d::Zero());

    REQUIRE(regenerated.gcodes.size() == 2);
    CHECK_THAT(regenerated.gcodes.front().print_z, Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK_THAT(regenerated.gcodes.back().print_z, Catch::Matchers::WithinAbs(0.3 + 0.24, 1e-9));
    CHECK_THAT(line.line_width(), Catch::Matchers::WithinAbs(0.6 * 1.05, 1e-9));
}
