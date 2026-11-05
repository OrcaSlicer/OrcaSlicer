#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
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

// Extruding moves under a ";TYPE:<type>" tag: only the first one after each tag, or all of them. The PA line test prints
// no object, so its fast segments are the first move after each ";TYPE:Outer wall" and the box under its numbers is
// all of ";TYPE:Bottom surface".
std::vector<Move> extrusions_of_type(const std::string &gcode, const std::string &type, bool first_only)
{
    const std::string  tag = ";TYPE:" + type;
    std::vector<Move>  out;
    std::istringstream lines(gcode);
    std::string        line;
    bool               relative = false;
    bool               armed    = false;
    double             x = 0., y = 0., e = 0., f = 0.;
    while (std::getline(lines, line)) {
        if (line.rfind(";TYPE:", 0) == 0) {
            armed = line == tag;
            continue;
        }
        if (line.rfind("M83", 0) == 0)
            relative = true;
        else if (line.rfind("M82", 0) == 0)
            relative = false;
        const bool is_move = line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0;
        if (!is_move && line.rfind("G92 ", 0) != 0)
            continue;

        std::istringstream words(line.substr(0, line.find(';')));
        std::string        word;
        words >> word; // the command
        double nx = x, ny = y, ev = 0.;
        bool   has_e = false;
        while (words >> word) {
            const double v = std::stod(word.substr(1));
            switch (word[0]) {
            case 'X': nx = v; break;
            case 'Y': ny = v; break;
            case 'E': ev = v; has_e = true; break;
            case 'F': f = v; break;
            default: break;
            }
        }
        if (!is_move) { // G92
            if (has_e)
                e = ev;
            continue;
        }
        const double length = std::hypot(nx - x, ny - y);
        if (has_e && armed && length > 0.) {
            out.push_back({f, (relative ? ev : ev - e) / length});
            armed = !first_only;
        }
        if (has_e)
            e = relative ? e + ev : ev;
        x = nx;
        y = ny;
    }
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
        {"print_flow_ratio", "1"},
        {"outer_wall_speed", "200"},
        {"initial_layer_print_height", "0.2"},
        {"enable_prime_tower", 0},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"extruder", std::to_string(filament)}}};
    Test::init_print({Test::cube(20)}, print, model, config, &overrides);
    Calib_Params params;
    params.mode          = CalibMode::Calib_PA_Line;
    params.start         = 0.;
    params.end           = 0.02;
    params.step          = 0.002;
    params.print_numbers = print_numbers;
    print.set_calib_params(params);
    const std::string       gcode    = Test::gcode(print);
    const std::vector<Move> segments = extrusions_of_type(gcode, "Outer wall", true);
    REQUIRE_FALSE(segments.empty());
    // The box under the numbers is drawn with the same line width and height as the PA lines.
    const std::vector<Move> box = extrusions_of_type(gcode, "Bottom surface", false);
    CHECK(box.empty() != print_numbers);

    // The test prints 1.5 nozzle-wide lines on the 0.2 mm first layer (CalibPressureAdvanceLine).
    const size_t idx          = size_t(filament - 1);
    const double mm3_per_mm   = Flow(0.4 * 1.5, 0.2, 0.4).mm3_per_mm();
    const double max_vol      = config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(idx);
    const double diameter     = config.option<ConfigOptionFloats>("filament_diameter")->get_at(idx);
    const double flow_ratio   = config.option<ConfigOptionFloatsNullable>("filament_flow_ratio")->get_at(idx);
    // Fast lines run at the outer wall speed (at least 100 mm/s), capped by the filament's volumetric speed.
    const double fast_speed   = std::floor(std::min(std::max(100., 200.), max_vol / mm3_per_mm));
    const double e_per_mm     = mm3_per_mm * flow_ratio / (PI * diameter * diameter / 4.);
    for (const Move &move : segments) {
        CHECK_THAT(move.feedrate, Catch::Matchers::WithinAbs(fast_speed * 60., 1.));
        CHECK_THAT(move.e_per_mm, Catch::Matchers::WithinRel(e_per_mm, 0.005));
    }
    for (const Move &move : box)
        CHECK_THAT(move.e_per_mm, Catch::Matchers::WithinRel(e_per_mm, 0.005));
}
