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

std::vector<double> caged_overhang_feed_rates(const std::string& gcode)
{
    std::vector<double> feed_rates;
    bool outer_wall = false;
    // The caged slope lies on this Y + Z plane after perimeter offsets.
    constexpr double caged_slope_y_plus_z = 16.189;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&feed_rates, &outer_wall, caged_slope_y_plus_z](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            outer_wall = comment.find("Outer wall") != std::string_view::npos ||
                         comment.find("External perimeter") != std::string_view::npos;

        if (outer_wall && line.extruding(self) && line.dist_XY(self) > 1.0 &&
            std::abs(line.new_Y(self) - self.y()) < 0.001 &&
            std::abs(line.new_Y(self) + self.z() - caged_slope_y_plus_z) < 0.01)
            feed_rates.push_back(line.new_F(self));
    });

    return feed_rates;
}

DynamicPrintConfig caged_overhang_config(const char* wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4"},
        {"initial_layer_print_height", "0.2"},
        {"layer_height", "0.2"},
        {"line_width", "0.42"},
        {"outer_wall_line_width", "0.42"},
        {"inner_wall_line_width", "0.45"},
        {"wall_loops", "2"},
        {"wall_generator", wall_generator},
        {"wall_sequence", "inner wall/outer wall"},
        {"sparse_infill_density", "15%"},
        {"detect_overhang_wall", "1"},
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", "0"},
        {"zaa_enabled", "0"},
        {"outer_wall_speed", "200"},
        {"inner_wall_speed", "300"},
        {"overhang_1_4_speed", "0"},
        {"overhang_2_4_speed", "50"},
        {"overhang_3_4_speed", "30"},
        {"overhang_4_4_speed", "10"},
        {"bridge_speed", "50"},
        {"filament_max_volumetric_speed", "22"},
        {"slow_down_for_layer_cooling", "0"},
    });
    return config;
}

} // namespace

TEST_CASE("Caged external overhangs are slowed along their span", "[ExtrusionProcessor][Regression]")
{
    DynamicPrintConfig config = caged_overhang_config("classic");

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{caged_overhang_mesh()}, print, model, config, nullptr, false);
    const std::vector<double> feed_rates = caged_overhang_feed_rates(gcode(print));

    REQUIRE_FALSE(feed_rates.empty());
    REQUIRE(std::any_of(feed_rates.begin(), feed_rates.end(), [](double feed_rate) {
        return feed_rate < 100.0 * MM_PER_MIN;
    }));
}

TEST_CASE("Arachne caged external overhangs are slowed along their span", "[ExtrusionProcessor][Regression]")
{
    DynamicPrintConfig config = caged_overhang_config("arachne");

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{caged_overhang_mesh()}, print, model, config, nullptr, false);
    const std::vector<double> feed_rates = caged_overhang_feed_rates(gcode(print));

    REQUIRE_FALSE(feed_rates.empty());
    REQUIRE(std::any_of(feed_rates.begin(), feed_rates.end(), [](double feed_rate) {
        return feed_rate < 100.0 * MM_PER_MIN;
    }));
}
