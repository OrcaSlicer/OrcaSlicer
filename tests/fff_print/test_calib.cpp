#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/libslic3r.h"
#include "test_helpers.hpp"

using namespace Slic3r;

namespace {

struct Move
{
    double feedrate; // mm/min
    double e_per_mm;
};

// Extruding moves under a role tag: only the first one after each tag, or all of them.
std::vector<Move> extrusions_of_type(const std::string &gcode, const std::string &type, bool first_only, const DynamicPrintConfig& config)
{
    const std::string  prefix = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    const std::string  tag = prefix + type;
    std::vector<Move> out;
    bool armed = false;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (line.raw().rfind(prefix, 0) == 0)
            armed = line.raw() == tag;
        const double length = line.dist_XY(self);
        if (armed && line.cmd_is("G1") && line.has_e() && length > 0.) {
            out.push_back({line.new_F(self), line.dist_E(self) / length});
            armed = !first_only;
        }
    });
    return out;
}

} // namespace

TEST_CASE("The PA line test prints with the speed and extrusion of the filament assigned to it", "[Calib]")
{
    // Filament 1: 1.75 mm, flow ratio 1, up to 21 mm3/s. Filament 2: 2.85 mm, flow ratio 0.9, up to 4 mm3/s, so its
    // volumetric limit caps the fast lines well below filament 1's.
    const int  filament      = GENERATE(1, 2);
    const bool print_numbers = GENERATE(false, true);
    CAPTURE(filament, print_numbers);
    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"filament_self_index", "1,2"},
        {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
        {"filament_diameter", "1.75,2.85"},
        {"filament_flow_ratio", "1,0.9"},
        {"filament_max_volumetric_speed", "21,4"},
        {"print_flow_ratio", "0.95"},
        {"use_relative_e_distances", 0},
        {"outer_wall_speed", "200"},
        {"initial_layer_print_height", "0.2"},
        {"enable_prime_tower", 0},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    Model model;
    ModelObject* object = model.add_object("handle", "", make_cube(5, 5, 0.4));
    object->config.set_key_value("extruder", new ConfigOptionInt(filament));
    object->add_instance();
    Calib_Params params;
    params.mode          = CalibMode::Calib_PA_Line;
    params.start         = 0.;
    params.end           = 0.02;
    params.step          = 0.002;
    params.print_numbers = print_numbers;
    CalibPressureAdvanceLine line(params, config, false, *object, Vec3d::Zero());
    const CustomGCode::Info custom = line.generate_custom_gcodes(config, false, *object, Vec3d::Zero());
    REQUIRE(custom.gcodes.size() == 2);
    const std::string       gcode    = "M82\n" + custom.gcodes.front().extra;
    const std::vector<Move> segments = extrusions_of_type(gcode, "Outer wall", true, config);
    REQUIRE_FALSE(segments.empty());
    // The box under the numbers is drawn with the same line width and height as the PA lines.
    const std::vector<Move> box = extrusions_of_type(gcode, "Bottom surface", false, config);
    REQUIRE_FALSE(box.empty());

    // The test prints 1.5 nozzle-wide lines on the 0.2 mm first layer (CalibPressureAdvanceLine).
    const size_t idx          = size_t(filament - 1);
    const double mm3_per_mm   = Flow(0.4 * 1.5, 0.2, 0.4).mm3_per_mm();
    const double max_vol      = config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(idx);
    const double diameter     = config.option<ConfigOptionFloats>("filament_diameter")->get_at(idx);
    const double flow_ratio   = config.option<ConfigOptionFloatsNullable>("filament_flow_ratio")->get_at(idx);
    // Fast lines run at the outer wall speed (at least 100 mm/s), capped by the filament's volumetric speed.
    const double fast_speed   = std::floor(std::min(std::max(100., 200.), max_vol / mm3_per_mm));
    const double e_per_mm     = mm3_per_mm * flow_ratio * config.get_abs_value("print_flow_ratio") / (PI * diameter * diameter / 4.);
    CAPTURE(fast_speed, e_per_mm, segments.front().feedrate, segments.front().e_per_mm, box.front().e_per_mm);
    CHECK(std::all_of(segments.begin(), segments.end(), [&](const Move& move) {
        return Catch::Matchers::WithinAbs(fast_speed * 60., 1.).match(move.feedrate);
    }));
    CHECK(std::all_of(segments.begin(), segments.end(), [&](const Move& move) {
        return Catch::Matchers::WithinRel(e_per_mm, 0.005).match(move.e_per_mm);
    }));
    CHECK(std::all_of(box.begin(), box.end(), [&](const Move& move) {
        return Catch::Matchers::WithinRel(e_per_mm, 0.005).match(move.e_per_mm);
    }));
}

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
        {"filament_max_volumetric_speed", "30"},
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
