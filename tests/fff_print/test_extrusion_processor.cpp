#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Print settings the assertions below are derived from.
constexpr double caged_layer_height     = 0.2;  // mm
constexpr double caged_wall_width       = 0.42; // mm, outer wall line width
constexpr double caged_outer_wall_speed = 200.; // mm/s
constexpr double caged_slow_speed       = 100.; // mm/s, between every configured overhang speed (<= 50) and the wall speed

// A 40 x 20 x 20 mm box with a 45 degree overhang cut into the y = 0 side. The sloped face spans
// x = 5.086 .. 34.914 only, so the full-height walls of the box cage both ends of every overhang
// perimeter: the endpoints look supported even though the span between them is not.
TriangleMesh caged_overhang_mesh()
{
    return TriangleMesh(
        {
            {5.0859987f, 10.167065f, 5.711731f}, {34.914257f, 10.167065f, 5.711731f},
            {34.914257f, 0.f, 15.878796f},       {5.0859995f, 0.f, 15.878796f},
            {0.f, 0.f, 0.f},                      {0.f, 0.f, 20.f},
            {0.f, 20.f, 20.f},                    {0.f, 20.f, 0.f},
            {40.f, 20.f, 20.f},                   {40.f, 20.f, 0.f},
            {40.f, 0.f, 20.f},                    {40.f, 0.f, 0.f},
            {34.914257f, 0.f, 0.f},               {5.0859995f, 0.f, 0.f},
            {34.914257f, 10.167065f, 0.f},        {5.0859995f, 10.167065f, 0.f},
        },
        {
            {0, 1, 2},   {0, 2, 3},   {4, 5, 6},   {4, 6, 7},   {7, 6, 8},   {7, 8, 9},
            {9, 8, 10},  {9, 10, 11}, {12, 11, 10}, {5, 4, 13}, {5, 13, 3},  {2, 12, 10},
            {5, 3, 2},   {10, 5, 2},  {9, 11, 12}, {9, 12, 14}, {13, 4, 7},  {9, 14, 15},
            {15, 13, 7}, {7, 9, 15},  {8, 6, 5},   {8, 5, 10},  {14, 1, 0},  {14, 0, 15},
            {2, 1, 14},  {2, 14, 12}, {15, 0, 3},  {15, 3, 13},
        });
}

// Mesh geometry the wall filters below are derived from.
constexpr double caged_box_depth      = 20.;       // mm, the box spans y = 0 .. 20
constexpr double caged_slope_face_sum = 15.878796; // mm, y + z of the sloped face, from its corners

// A layer printed at z is sliced at z - layer_height / 2, and the outer wall centreline sits half a
// line width inside the contour, so the wall on the slope satisfies y + z = 16.189.
constexpr double caged_slope_wall_sum = caged_slope_face_sum + 0.5 * caged_layer_height + 0.5 * caged_wall_width;
// Same inset on the fully supported y = 20 face, vertical over the whole height.
constexpr double caged_back_wall_y = caged_box_depth - 0.5 * caged_wall_width;

// Feed rates in mm/min of the long outer wall extrusions `keep_line` selects.
template<typename KeepLine> std::vector<double> outer_wall_feed_rates(const std::string& gcode, KeepLine keep_line)
{
    std::vector<double> feed_rates;
    bool outer_wall = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&feed_rates, &outer_wall, &keep_line](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            outer_wall = comment.find("Outer wall") != std::string_view::npos ||
                         comment.find("External perimeter") != std::string_view::npos;

        if (outer_wall && line.extruding(self) && line.dist_XY(self) > 1.0 && keep_line(self, line))
            feed_rates.push_back(line.new_F(self));
    });

    return feed_rates;
}

// The caged 45 degree overhang: walls that run along x at a constant y, on the sloped face.
std::vector<double> caged_slope_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        return std::abs(line.new_Y(self) - self.y()) < 0.001 &&
               std::abs(line.new_Y(self) + self.z() - caged_slope_wall_sum) < 0.01;
    });
}

// The opposite, fully supported face, skipping the initial layer and its own speed settings.
std::vector<double> back_wall_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        return self.z() > 1.5 * caged_layer_height &&
               std::abs(line.new_Y(self) - self.y()) < 0.001 &&
               std::abs(line.new_Y(self) - caged_back_wall_y) < 0.1;
    });
}

DynamicPrintConfig caged_overhang_config(const char* wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4"},
        {"initial_layer_print_height", caged_layer_height},
        {"layer_height", caged_layer_height},
        {"line_width", caged_wall_width},
        {"outer_wall_line_width", caged_wall_width},
        {"inner_wall_line_width", "0.45"},
        {"wall_loops", "2"},
        {"wall_generator", wall_generator},
        {"wall_sequence", "inner wall/outer wall"},
        {"sparse_infill_density", "15%"},
        {"detect_overhang_wall", "1"},
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", "0"},
        {"zaa_enabled", "0"},
        {"outer_wall_speed", caged_outer_wall_speed},
        {"inner_wall_speed", "300"},
        {"overhang_1_4_speed", "0"},
        {"overhang_2_4_speed", "50"},
        {"overhang_3_4_speed", "30"},
        {"overhang_4_4_speed", "10"},
        {"bridge_speed", "50"},
        {"filament_max_volumetric_speed", "22"},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"}, // Nothing but the overhang settings may lower a wall speed
    });
    return config;
}

std::string caged_overhang_gcode(const char* wall_generator)
{
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{caged_overhang_mesh()}, print, model, caged_overhang_config(wall_generator), nullptr,
               false);
    return gcode(print);
}

} // namespace

TEST_CASE("Caged external overhangs are slowed along their span", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = caged_slope_feed_rates(caged_overhang_gcode(wall_generator));

    REQUIRE_FALSE(feed_rates.empty());
    REQUIRE(std::any_of(feed_rates.begin(), feed_rates.end(), [](double feed_rate) {
        return feed_rate < caged_slow_speed * MM_PER_MIN;
    }));
}

TEST_CASE("Supported vertical walls keep their normal speed", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = back_wall_feed_rates(caged_overhang_gcode(wall_generator));

    REQUIRE_FALSE(feed_rates.empty());
    REQUIRE(std::none_of(feed_rates.begin(), feed_rates.end(), [](double feed_rate) {
        return feed_rate < caged_slow_speed * MM_PER_MIN;
    }));
}
