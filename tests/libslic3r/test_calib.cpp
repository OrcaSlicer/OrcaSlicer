#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include "libslic3r/CustomGCode.hpp"
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/calib.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/path.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <tuple>

using namespace Slic3r;

namespace {

// The width-resolution getters are protected; expose them so the resolution can be asserted directly.
struct PaPatternProbe : public CalibPressureAdvancePattern
{
    using CalibPressureAdvancePattern::CalibPressureAdvancePattern;
    using CalibPressureAdvancePattern::line_width;
    using CalibPressureAdvancePattern::line_width_first_layer;
};

} // namespace

TEST_CASE("Zero calibration line width resolves to a positive default", "[Calib][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"line_width", "0"},
        {"initial_layer_line_width", "0"},
    });

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode = CalibMode::Calib_PA_Pattern;

    PaPatternProbe pattern(params, config, /* is_bbl_machine */ true, *model.objects.front(), Vec3d(0, 0, 0));

    REQUIRE(pattern.line_width() > 0.);
    REQUIRE(pattern.line_width_first_layer() > 0.);
}

namespace {

struct EndState { double final_e; double max_e; };

EndState simulate_absolute_e(const std::string &gcode)
{
    double final_e = 0.;
    double max_e   = 0.;

    std::istringstream lines(gcode);
    std::string        line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string        op;
        if (!(words >> op))
            continue;
        if (op != "G1" && op != "G0" && op != "G92")
            continue;

        std::string word;
        while (words >> word) {
            if (word.size() >= 2 && word[0] == 'E') {
                final_e = std::stod(word.substr(1));
                max_e   = std::max(max_e, final_e);
                break;
            }
        }
    }

    return {final_e, max_e};
}

} // namespace

TEST_CASE("PA pattern resets the extruder after the final layer in absolute E mode", "[Calib][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"use_relative_e_distances", "0"},
        {"line_width", "0.45"},
        {"initial_layer_line_width", "0.45"},
    });

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode  = CalibMode::Calib_PA_Pattern;
    params.start = 0.;
    params.end   = 0.08;
    params.step  = 0.002;

    CalibPressureAdvancePattern pattern(params, config, /* is_bbl_machine */ false, *model.objects.front(), Vec3d(0, 0, 0));
    const CustomGCode::Info     info = pattern.generate_custom_gcodes(config, /* is_bbl_machine */ false, *model.objects.front(),
                                                                      Vec3d(0, 0, 0));

    std::string gcode;
    for (const CustomGCode::Item &item : info.gcodes)
        gcode += item.extra;

    const EndState state = simulate_absolute_e(gcode);

    REQUIRE(state.max_e > 1.);
    REQUIRE_THAT(state.final_e, Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("a temperature tower's temperatures are brought into 155-500 and rounded down to 5 degree blocks", "[Calib]")
{
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(clamp_calib_temp(154.), WithinAbs(155., 1e-9));
    CHECK_THAT(clamp_calib_temp(155.), WithinAbs(155., 1e-9));
    CHECK_THAT(clamp_calib_temp(230.), WithinAbs(230., 1e-9));
    CHECK_THAT(clamp_calib_temp(500.), WithinAbs(500., 1e-9));
    CHECK_THAT(clamp_calib_temp(501.), WithinAbs(500., 1e-9));

    CHECK_THAT(round_calib_temp(232.), WithinAbs(230., 1e-9));
    CHECK_THAT(round_calib_temp(234.9), WithinAbs(230., 1e-9));
    CHECK_THAT(round_calib_temp(235.), WithinAbs(235., 1e-9));
    CHECK_THAT(round_calib_temp(clamp_calib_temp(999.)), WithinAbs(500., 1e-9));
    CHECK_THAT(round_calib_temp(clamp_calib_temp(1.)), WithinAbs(155., 1e-9));
}

namespace {

// Filament fed per mm over all extruding moves of a PA pattern printed with `filament_id`, in relative E mode.
double pa_pattern_e_per_mm(const DynamicPrintConfig &config, int filament_id)
{
    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode        = CalibMode::Calib_PA_Pattern;
    params.start       = 0.;
    params.end         = 0.02;
    params.step        = 0.002;
    params.filament_id = filament_id;

    CalibPressureAdvancePattern pattern(params, config, /* is_bbl_machine */ false, *model.objects.front(), Vec3d(0, 0, 0));
    const CustomGCode::Info     info = pattern.generate_custom_gcodes(config, /* is_bbl_machine */ false, *model.objects.front(),
                                                                      Vec3d(0, 0, 0));

    double x = 0., y = 0., e = 0., length = 0.;
    for (const CustomGCode::Item &item : info.gcodes) {
        std::istringstream lines(item.extra);
        std::string        line;
        while (std::getline(lines, line)) {
            std::istringstream words(line);
            std::string        op;
            if (!(words >> op) || (op != "G0" && op != "G1"))
                continue;
            double      nx = x, ny = y, de = 0.;
            std::string word;
            while (words >> word && word[0] != ';') {
                if (word[0] == 'X')
                    nx = std::stod(word.substr(1));
                else if (word[0] == 'Y')
                    ny = std::stod(word.substr(1));
                else if (word[0] == 'E')
                    de = std::stod(word.substr(1));
            }
            const double dist = std::hypot(nx - x, ny - y);
            if (de > 0. && dist > 0.) {
                e += de;
                length += dist;
            }
            x = nx;
            y = ny;
        }
    }
    return length > 0. ? e / length : 0.;
}

} // namespace

TEST_CASE("PA pattern extrudes with the diameter and flow ratio of its test filament", "[Calib]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"use_relative_e_distances", "1"},
        {"line_width", "0.45"},
        {"initial_layer_line_width", "0.45"},
        {"filament_diameter", "1.75,2.85"},
        {"filament_flow_ratio", "1,0.9"},
    });

    const double first  = pa_pattern_e_per_mm(config, 0);
    const double second = pa_pattern_e_per_mm(config, 1);
    REQUIRE(first > 0.);
    // Same lines, so the feed scales with the flow ratio over the filament cross-section.
    const double expected = (1.75 * 1.75) / (2.85 * 2.85) * 0.9;
    REQUIRE_THAT(second / first, Catch::Matchers::WithinRel(expected, 0.005));
}

TEST_CASE("Ironing flow marks use the smallest step that keeps them 10 mm apart", "[Calib]")
{
    // min flow, max flow, band length, expected step (0 when even 20% marks are closer than 10 mm)
    const auto [min_flow, max_flow, length, step] = GENERATE(table<double, double, double, double>({
        {10., 60., 100., 5.},
        {0., 10., 100., 1.},
        {0., 100., 100., 10.},
        {0., 100., 50., 20.},
        {0., 100., 40., 0.},
    }));
    CAPTURE(min_flow, max_flow, length);
    CHECK_THAT(ironing_flow_calib_mark_step(min_flow, max_flow, length), Catch::Matchers::WithinAbs(step, 1e-9));
}

TEST_CASE("Ironing flow test meshes are a closed strip on a closed base, with closed labels on the base in front", "[Calib]")
{
    // The text needs the font in resources/fonts.
    const std::string previous = resources_dir();
    set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string());

    const double length  = 100.;
    const double base_z  = 0.32;
    const double marks_z = 0.56;
    const double top_z   = 0.68;
    const IroningFlowCalibMeshes meshes = ironing_flow_calib_meshes(10., 60., length, "Generic PLA", 0.4, base_z, marks_z, top_z);
    set_resources_dir(previous);

    // The band plus 5 mm at each end, on top of the base.
    const BoundingBoxf3 strip = bounding_box(meshes.strip);
    CHECK_THAT(strip.size().x(), Catch::Matchers::WithinAbs(length + 10., 1e-3));
    CHECK_THAT(strip.min.z(), Catch::Matchers::WithinAbs(base_z, 1e-6));
    CHECK_THAT(strip.max.z(), Catch::Matchers::WithinAbs(top_z, 1e-6));
    CHECK(its_num_open_edges(meshes.strip) == 0);

    // The base lies under the strip and reaches in front of it, under the labels.
    const BoundingBoxf3 base = bounding_box(meshes.base);
    CHECK_THAT(base.min.z(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT(base.max.z(), Catch::Matchers::WithinAbs(base_z, 1e-6));
    CHECK_THAT(base.max.y(), Catch::Matchers::WithinAbs(strip.max.y(), 1e-6));
    CHECK(its_num_open_edges(meshes.base) == 0);

    // The marks and the name lie on the base, in front of the band.
    const BoundingBoxf3 labels = bounding_box(meshes.labels);
    CHECK_THAT(labels.min.z(), Catch::Matchers::WithinAbs(base_z, 1e-6));
    CHECK_THAT(labels.max.z(), Catch::Matchers::WithinAbs(marks_z, 1e-6));
    CHECK(labels.max.y() <= 1e-6);
    CHECK(labels.min.y() < -10.);
    CHECK(labels.min.y() > base.min.y());
    CHECK(its_num_open_edges(meshes.labels) == 0);
}
