#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "libslic3r/calib.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"

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

// calib_band_count() is the single source of truth shared by the tower geometry (block count / cut height) and
// the G-code stepping (interpolate_value_across_layers, used by the temperature / VFA / PA towers). If the two
// ever computed the band count differently, the printed blocks would drift out of sync with the stepped values.
TEST_CASE("Calibration band count matches the number of distinct stepped values", "[Calib][Regression]")
{
    // Counts the values start, start+step, ... that do not pass end (start included), using floor so the last
    // band never overshoots end.
    CHECK(calib_band_count(40.0, 200.0, 10.0) == 17);   // exact multiple: 40, 50, ..., 200
    CHECK(calib_band_count(40.0, 200.0, 7.0)  == 23);   // 160/7 = 22.85 -> 22 whole steps past the start
    CHECK(calib_band_count(0.0, 0.1, 0.002)   == 51);   // float division lands just under 50; the epsilon keeps it exact
    CHECK(calib_band_count(0.0, 1.0, 0.02)    == 51);
    CHECK(calib_band_count(250.0, 200.0, 5.0) == 11);   // descending range (temperature tower)
    CHECK(calib_band_count(0.0, 0.1, 0.006)   == 17);   // rounding up would give 18 and overshoot 0.1
    CHECK(calib_band_count(5.0, 5.0, 1.0)     == 1);    // zero-length range
    CHECK(calib_band_count(0.0, 100.0, 0.0)   == 1);    // guarded against zero / negative step
}

TEST_CASE("Calibration band stepping never passes the requested end value", "[Calib][Regression]")
{
    const auto [start, end, step] = GENERATE(table<double, double, double>({
        {40.0, 200.0, 10.0},   // VFA default (exact)
        {40.0, 200.0, 7.0},    // non-integer number of steps
        {40.0, 200.0, 13.0},
        {0.0, 0.1, 0.002},     // PA direct-drive default
        {0.0, 0.1, 0.006},     // rounding up here would overshoot the end
        {0.0, 1.0, 0.02},      // PA bowden default
        {250.0, 200.0, 5.0},   // temperature tower (descending)
        {230.0, 190.0, 5.0},
    }));

    const int    n    = calib_band_count(start, end, step);
    const double span = std::abs(end - start);

    REQUIRE(n >= 1);
    // The top band sits (n - 1) steps from the start and must not pass the end...
    REQUIRE((n - 1) * step <= span + 1e-9);
    // ...and it is the last band that fits: one more step would pass the end.
    REQUIRE(n * step > span + 1e-9);
}

namespace {

// A calibration tower/sweep as the G-code layer stepping sees it: the per-layer value driver plus how many
// layers the sliced object has. `step == 0` is the gradual (non-banded) form used by input shaping / cornering.
struct CalibSweep { const char* name; float start; float end; float step; unsigned int layers; };

std::vector<float> simulate_calibration(const CalibSweep& s)
{
    std::vector<float> values;
    values.reserve(s.layers);
    for (unsigned int i = 0; i < s.layers; ++i)
        values.push_back(calib_interpolate_value(s.start, s.end, s.step, static_cast<int>(i), s.layers));
    return values;
}

} // namespace

// Range coverage for every calibration that steps a value across the tower's layers via interpolate_value_across_layers:
// temperature, VFA, PA tower (stepped) and input-shaping / cornering (gradual). The (start, end) values below are the
// dialog defaults; the layer counts are representative sliced heights (they only need to exceed the band count).
TEST_CASE("Layer-stepped calibrations sweep their whole range and stay within it", "[Calib][Regression]")
{
    const auto sweep = GENERATE(values<CalibSweep>({
        {"temperature", 230.f, 190.f, 5.f,     450},   // descending, stepped
        {"vfa",          40.f, 200.f, 10.f,     425},   // ascending, stepped
        {"pa_tower",      0.f,   0.1f, 0.002f,  255},   // small stepped range
        {"input_shaping", 10.f,  60.f, 0.f,     120},   // gradual (step == 0)
        {"cornering",      0.f,   1.f, 0.f,     100},   // gradual (step == 0)
    }));
    INFO("calibration: " << sweep.name);

    const std::vector<float> values = simulate_calibration(sweep);
    REQUIRE(values.size() == sweep.layers);

    const float lo = std::min(sweep.start, sweep.end);
    const float hi = std::max(sweep.start, sweep.end);
    const bool  ascending = sweep.end >= sweep.start;

    // Starts at the start value.
    REQUIRE_THAT(values.front(), Catch::Matchers::WithinAbs(sweep.start, 1e-4));

    for (std::size_t i = 0; i < values.size(); ++i) {
        // Never leaves the requested range (no overshoot past either end).
        REQUIRE(values[i] >= lo - 1e-4);
        REQUIRE(values[i] <= hi + 1e-4);
        // Progresses monotonically toward the end value.
        if (i > 0) {
            if (ascending)
                REQUIRE(values[i] >= values[i - 1] - 1e-4);
            else
                REQUIRE(values[i] <= values[i - 1] + 1e-4);
        }
    }

    // Reaches the end value at the top of the tower (within one step for the banded forms).
    REQUIRE(std::abs(values.back() - sweep.end) <= std::abs(sweep.step) + 1e-4);
}

// The stepped towers must actually print every discrete value, one band after another, so the label at a given
// height matches the value that was applied there.
TEST_CASE("Stepped calibrations visit each discrete value in order", "[Calib][Regression]")
{
    const auto sweep = GENERATE(values<CalibSweep>({
        {"temperature", 230.f, 190.f, 5.f,    450},
        {"vfa",          40.f, 200.f, 10.f,    425},
        {"pa_tower",      0.f,   0.1f, 0.002f, 255},
    }));
    INFO("calibration: " << sweep.name);

    const std::vector<float> values = simulate_calibration(sweep);

    // Collapse runs of equal layers into the sequence of distinct band values.
    std::vector<float> bands;
    for (float v : values)
        if (bands.empty() || std::abs(v - bands.back()) > 1e-4f)
            bands.push_back(v);

    const int    expected = calib_band_count(sweep.start, sweep.end, sweep.step);
    const double dir      = (sweep.end >= sweep.start) ? 1.0 : -1.0;

    // Exactly one band per stepped value, no more and no fewer.
    REQUIRE(static_cast<int>(bands.size()) == expected);
    // And each band is start + k*step, in order, never passing the end.
    for (int k = 0; k < expected; ++k)
        REQUIRE_THAT(bands[k], Catch::Matchers::WithinAbs(sweep.start + dir * k * sweep.step, 1e-4));
}
