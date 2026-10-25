#include <catch2/catch_all.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Circle.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
// Orca: Exercise the inline preview conversion with metadata produced by the real G-code pipeline.
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include <cstddef>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Print settings the assertions below are derived from.
constexpr double caged_layer_height     = 0.2;  // mm
constexpr double caged_wall_width       = 0.42; // mm, outer wall line width
constexpr double caged_outer_wall_speed = 200.; // mm/s
constexpr double caged_slow_speed       = 100.; // mm/s, between every configured overhang speed (<= 50) and the wall speed

// A wall running 0.2mm out over a previous layer whose edge dishes 0.03mm away from it in the middle,
// standing in for the endpoint readings a caged overhang perimeter takes: enough of a difference to
// print at another speed, but only a fraction of the distance at which slowdown begins.
constexpr double dished_wall_gap     = 0.2;   // mm, how far the wall runs out past the previous layer's edge
constexpr double dished_layer_depth  = 0.03;  // mm, how much further out the middle of it reads
constexpr double dished_min_distance = 0.042; // mm, the reading at which the configured speeds begin to slow down
// Every reading here is past that, so the whole wall is slowed and only the amount is in question.
constexpr float  dished_end_reading  = float(dished_wall_gap + 0.5 * caged_wall_width);
constexpr float  dished_mid_reading  = float(dished_end_reading + dished_layer_depth);
// The two readings are dished_layer_depth apart, so half of that tells them apart while still allowing
// for the points the passes after sampling add, which read a little further out than the ends do.
constexpr double dished_reading_tolerance = 0.5 * dished_layer_depth;

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
// The sloped face spans this x range; outside it the box walls run full height.
constexpr double caged_slope_x_min = 5.0859995;
constexpr double caged_slope_x_max = 34.914257;
constexpr double caged_slope_span  = caged_slope_x_max - caged_slope_x_min; // ~29.8 mm
// The z range the sloped face occupies, from the same fixture vertices.
constexpr double caged_slope_z_min = 5.711731;
constexpr double caged_slope_z_max = 15.878796;
// The lowest slope layer still sits on the solid body below the notch, so it is fully supported and
// runs at the outer wall speed by design. The caged span proper begins one layer above it.
constexpr double caged_span_z_min = caged_slope_z_min + caged_layer_height;

// A layer printed at z is sliced at z - layer_height / 2, and the outer wall centreline sits half a
// line width inside the contour, so the wall on the slope satisfies y + z = 16.189.
constexpr double caged_slope_wall_sum = caged_slope_face_sum + 0.5 * caged_layer_height + 0.5 * caged_wall_width;
// Same inset on the fully supported y = 20 face, vertical over the whole height.
constexpr double caged_back_wall_y = caged_box_depth - 0.5 * caged_wall_width;
// And on the y = 0 face, which runs full height only outside the slope's x range.
constexpr double caged_front_wall_y = 0.5 * caged_wall_width;
// Arachne varies the wall width along a face, and the centreline inset is half that width, so a
// wall sits within about half a line width of where the nominal inset alone would put it. The
// faces being selected are millimetres apart, so this stays far from ambiguous.
constexpr double caged_wall_tolerance = 0.5 * caged_wall_width;

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

// The caged 45 degree overhang: outer walls crossing the sloped face for most of its width, on the
// layers where the face genuinely overhangs.
// Both ends are tested against the slope plane rather than requiring a constant Y. Arachne's
// variable-width walls drift slightly in Y along the same slope (Y6.186 -> Y6.189 on one move), so
// a constant-Y filter matches almost nothing under Arachne and silently reduces its coverage.
// The length test excludes the cage walls: they are only as wide as the box is either side of the
// slope, but being vertical their y + z sweeps through the slope plane as z rises, so a couple of
// their fully supported moves would otherwise be counted as part of the span.
std::vector<double> caged_slope_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const double z = line.new_Z(self);
        return z > caged_span_z_min && z < caged_slope_z_max &&
               line.dist_XY(self) > 0.5 * caged_slope_span &&
               std::abs(self.y() + z - caged_slope_wall_sum) < caged_wall_tolerance &&
               std::abs(line.new_Y(self) + z - caged_slope_wall_sum) < caged_wall_tolerance;
    });
}

// The opposite, fully supported face, skipping the initial layer and its own speed settings.
std::vector<double> back_wall_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        return line.new_Z(self) > 1.5 * caged_layer_height &&
               std::abs(self.y() - caged_back_wall_y) < caged_wall_tolerance &&
               std::abs(line.new_Y(self) - caged_back_wall_y) < caged_wall_tolerance;
    });
}

// The first layer printed entirely above the slope. Its y = 0 wall runs the full width of the box.
const double caged_layer_above_slope_z = std::ceil(caged_slope_z_max / caged_layer_height) * caged_layer_height;

// The parts of that wall standing on the cage rather than the slope, so on a contour identical to their own.
// Where the support changes is found by bisection, which stops at spans of 2mm, so the move spanning each end of
// the slope reaches a little way into the cage. Taking only the moves lying wholly outside the slope's x range
// leaves the wall that is unambiguously supported, without asserting how closely the bisection converged.
std::vector<double> cage_shoulder_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        return std::abs(line.new_Z(self) - caged_layer_above_slope_z) < 0.5 * caged_layer_height &&
               std::abs(self.y() - caged_front_wall_y) < caged_wall_tolerance &&
               std::abs(line.new_Y(self) - caged_front_wall_y) < caged_wall_tolerance &&
               (std::max(self.x(), line.new_X(self)) <= caged_slope_x_min ||
                std::min(self.x(), line.new_X(self)) >= caged_slope_x_max);
    });
}

// The readings a 40mm wall takes over a previous layer whose edge falls away by 0.03mm towards the
// middle: both ends read the same, and the middle reads slightly further out over air. Whether that
// middle reading survives is what decides the speed the wall is printed at.
std::vector<ExtendedPoint<2>> sampled_wall_over_dished_layer(const std::function<float(float)>& distance_to_speed)
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {20., -dished_layer_depth}},
        {{20., -dished_layer_depth}, {40., 0.}},
        {{40., 0.}, {40., -10.}},
        {{40., -10.}, {0., -10.}},
        {{0., -10.}, {0., 0.}},
    });
    const Points wall{Point::new_scale(0., dished_wall_gap), Point::new_scale(40., dished_wall_gap)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f,
                                                              dished_min_distance, distance_to_speed);
}

// A straight, otherwise supported wall over a previous-layer boundary with a 2mm-wide pocket. Moving the
// pocket between x = 10 and x = 20 covers both discovery away from the wall's midpoint and refinement around
// a midpoint that has already been discovered. The current wall is inset half its width from the flat boundary,
// so its supported readings are zero after the estimator applies its boundary offset.
constexpr double narrow_pocket_wall_length = 40.;
constexpr double narrow_pocket_width       = 2.;
constexpr double narrow_pocket_depth       = 0.3;

std::vector<ExtendedPoint<2>> sampled_wall_over_narrow_pocket(
    double pocket_center, const std::function<float(float)>& distance_to_speed)
{
    const double pocket_left  = pocket_center - 0.5 * narrow_pocket_width;
    const double pocket_right = pocket_center + 0.5 * narrow_pocket_width;
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {pocket_left, 0.}},
        {{pocket_left, 0.}, {pocket_left, -narrow_pocket_depth}},
        {{pocket_left, -narrow_pocket_depth}, {pocket_right, -narrow_pocket_depth}},
        {{pocket_right, -narrow_pocket_depth}, {pocket_right, 0.}},
        {{pocket_right, 0.}, {narrow_pocket_wall_length, 0.}},
        {{narrow_pocket_wall_length, 0.}, {narrow_pocket_wall_length, -10.}},
        {{narrow_pocket_wall_length, -10.}, {0., -10.}},
        {{0., -10.}, {0., 0.}},
    });
    const double wall_y = -0.5 * caged_wall_width;
    const Points wall{Point::new_scale(0., wall_y), Point::new_scale(narrow_pocket_wall_length, wall_y)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f,
                                                               dished_min_distance, distance_to_speed);
}

// A cross section that grows a layer's worth on the two faces meeting at either end of a wall, as any
// 45 degree overhang does. The wall itself stands on a contour identical to its own, but its ends sit
// where the growing faces cut the corners off, and the previous layer's edge there is nearer than the
// half line width the centreline is inset by. Both ends therefore read an overhang while everything
// between them reads supported: the reverse of the caged span, and the case the sampling above must
// leave to the passes after it.
constexpr double stepped_wall_inset = 0.5 * caged_wall_width;                       // mm, centreline inset from the contour
constexpr double stepped_end_gap    = stepped_wall_inset - caged_layer_height;      // mm, how far inside the corner ends up
constexpr double stepped_wall_span  = 30.;                                          // mm, the length of the wall

std::vector<ExtendedPoint<2>> sampled_wall_between_growing_corners(const std::function<float(float)>& distance_to_speed)
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {32., 0.}},
        {{32., 0.}, {32., -stepped_wall_span}},
        {{32., -stepped_wall_span}, {0., -stepped_wall_span}},
        {{0., -stepped_wall_span}, {0., 0.}},
    });
    const Points wall{Point::new_scale(stepped_wall_inset, -stepped_end_gap),
                      Point::new_scale(stepped_wall_inset, stepped_end_gap - stepped_wall_span)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f,
                                                              dished_min_distance, distance_to_speed);
}

// How much of a path is printed below the speed a fully supported reading gives. A segment is printed
// at the lower of the speeds its ends read.
double slowed_length(const std::vector<ExtendedPoint<2>>& points, const std::function<float(float)>& distance_to_speed)
{
    double length = 0.;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (std::min(distance_to_speed(points[i].distance), distance_to_speed(points[i + 1].distance)) < distance_to_speed(0.f))
            length += (points[i + 1].position - points[i].position).norm();
    return length;
}

float furthest_reading(const std::vector<ExtendedPoint<2>>& points)
{
    return std::max_element(points.begin(), points.end(), [](const ExtendedPoint<2>& l, const ExtendedPoint<2>& r) {
               return l.distance < r.distance;
           })->distance;
}

// A wall along a supported edge of the previous layer, ending past or just short of the edge's end. Crossing the edge's
// end reads half a line width out.
constexpr double edge_run_length = 64.;   // mm, wall start, measured from the end of the previous layer's edge
constexpr double edge_step       = 0.384; // mm, how far this layer's contour extends past the previous layer's end
// The centreline is inset half a line width from the contour.
constexpr double edge_wall_end_past  = edge_step - 0.5 * caged_wall_width;
constexpr double edge_wall_end_short = 0.05; // mm short of the edge, reading 0.21 - 0.05 = 0.16mm out
// Segmentation splits 1.5 line widths plus the end's reading from an end, so an end's slowdown and cooling stay within this.
constexpr double edge_affected_length = 3. * caged_wall_width;

std::vector<ExtendedPoint<2>> sampled_wall_along_edge(double                              wall_end_x,
                                                      const std::function<float(float)>& distance_to_speed,
                                                      float                               min_distance,
                                                      float                               fan_overlap_threshold)
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {edge_run_length + 10., 0.}},
        {{edge_run_length + 10., 0.}, {edge_run_length + 10., -10.}},
        {{edge_run_length + 10., -10.}, {0., -10.}},
        {{0., -10.}, {0., 0.}},
    });
    const double wall_y = -0.5 * caged_wall_width;
    const Points wall{Point::new_scale(edge_run_length, wall_y), Point::new_scale(wall_end_x, wall_y)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f, min_distance,
                                                              distance_to_speed, fan_overlap_threshold);
}

// Length printed with the overhang fan on: segments with either end's overlap at or below the threshold.
double cooled_length(const std::vector<ExtendedPoint<2>>& points, float fan_overlap_threshold)
{
    double length = 0.;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (1.f - std::max(points[i].distance, points[i + 1].distance) / float(caged_wall_width) <= fan_overlap_threshold)
            length += (points[i + 1].position - points[i].position).norm();
    return length;
}

DynamicPrintConfig caged_overhang_config(const char* wall_generator){
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

constexpr double shallow_layer_height = 0.02;
constexpr double shallow_wall_width = 0.23;
constexpr double shallow_outer_wall_speed = 60.;
constexpr double shallow_overhang_speed = 30.;
// Orca: Exercise the existing 10-25% slowdown band without depending on a mild-overhang option.
constexpr double shallow_slowed_top_offset = 0.2 * shallow_wall_width / shallow_layer_height;

// A 10 x 10 x 1mm prism whose front face moves outwards by top_offset over its height,
// while the back face remains vertical and fully supported.
TriangleMesh shallow_overhang_mesh(double top_offset = 0.5)
{
    const float top_y = -float(top_offset);
    return TriangleMesh(
        {
            {0.f, 0.f, 0.f},    {10.f, 0.f, 0.f},   {10.f, 10.f, 0.f}, {0.f, 10.f, 0.f},
            {0.f, top_y, 1.f},  {10.f, top_y, 1.f}, {10.f, 10.f, 1.f}, {0.f, 10.f, 1.f},
        },
        {
            {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
            {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
            {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
        });
}

// Orca: Share the print settings between fixed-height and adaptive-height overhang regressions.
DynamicPrintConfig shallow_overhang_config(const char *wall_generator, bool enable_overhang_speed, bool gcode_overhangs)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.2"},
        {"initial_layer_print_height", shallow_layer_height},
        {"layer_height", shallow_layer_height},
        {"line_width", shallow_wall_width},
        {"outer_wall_line_width", shallow_wall_width},
        {"inner_wall_line_width", shallow_wall_width},
        {"wall_loops", "1"},
        {"wall_generator", wall_generator},
        {"sparse_infill_density", "0%"},
        {"top_shell_layers", "1"},
        {"bottom_shell_layers", "1"},
        {"detect_overhang_wall", "1"},
        {"enable_overhang_speed", enable_overhang_speed ? "1" : "0"},
        {"gcode_overhangs", gcode_overhangs ? "1" : "0"},
        {"slowdown_for_curled_perimeters", "0"},
        {"zaa_enabled", "0"},
        {"outer_wall_speed", shallow_outer_wall_speed},
        {"inner_wall_speed", shallow_outer_wall_speed},
        {"overhang_1_4_speed", shallow_overhang_speed},
        {"overhang_2_4_speed", "0"},
        {"overhang_3_4_speed", "0"},
        {"overhang_4_4_speed", "0"},
        {"filament_max_volumetric_speed", "5"},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"},
    });

    return config;
}

// Orca: Let the fixture independently toggle speed handling and optional preview metadata.
std::string shallow_overhang_gcode(const char *wall_generator, double top_offset = 0.5,
                                  bool enable_overhang_speed = true, bool gcode_overhangs = false)
{
    Print print;
    Model model;
    init_print({shallow_overhang_mesh(top_offset)}, print, model,
        shallow_overhang_config(wall_generator, enable_overhang_speed, gcode_overhangs), nullptr, false);
    return gcode(print);
}

// Orca: Extract every emitted percentage without coupling the regression test to G-code line positions.
std::vector<float> overhang_percentages(const std::string &gcode)
{
    std::vector<float> percentages;
    size_t position = 0;
    while ((position = gcode.find("OVERHANG:", position)) != std::string::npos) {
        position += sizeof("OVERHANG:") - 1;
        char *end = nullptr;
        const float percentage = std::strtof(gcode.c_str() + position, &end);
        if (end != gcode.c_str() + position)
            percentages.push_back(percentage);
        position = end != gcode.c_str() + position ? size_t(end - gcode.c_str()) : position;
    }
    return percentages;
}

std::vector<double> shallow_face_feed_rates(const std::string &gcode, bool overhanging_face)
{
    return outer_wall_feed_rates(gcode, [overhanging_face](const GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.new_Z(self) < 3. * shallow_layer_height || line.new_Z(self) > 0.9)
            return false;
        const double middle_y = 0.5 * (self.y() + line.new_Y(self));
        return overhanging_face ? middle_y < 1. : middle_y > 9.;
    });
}

// Reports the matched move count alongside the extremes, so a filter that selected nothing is
// distinguishable from a span that simply was not slowed.
void info_feed_rates(const char* span, const std::vector<double>& feed_rates)
{
    UNSCOPED_INFO("matched " << feed_rates.size() << " " << span << " moves");
    if (!feed_rates.empty()) {
        const auto extremes = std::minmax_element(feed_rates.begin(), feed_rates.end());
        UNSCOPED_INFO("slowest " << *extremes.first / MM_PER_MIN << " mm/s, fastest " << *extremes.second / MM_PER_MIN << " mm/s");
    }
}

// Orca: Compare executable output independently of optional preview comments, including arc parameters.
std::vector<std::string> printer_commands(const std::string &gcode)
{
    std::vector<std::string> result;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (!line.cmd().empty()) {
            std::string command = line.raw().substr(0, line.raw().find(';'));
            command.erase(command.find_last_not_of(" \t") + 1);
            result.push_back(std::move(command));
        }
    });
    return result;
}

// Orca: The local readings the exporter writes for one move: bead-wide cells on walls, quarter-bead cells on infill.
std::vector<float> line_overhang_profile(ExtrusionQualityEstimator &estimator, const Vec3d &start, const Vec3d &end,
                                         float width, bool wall)
{
    const double cell_length = wall ? std::max(0.1, double(width)) : std::max(0.05, 0.25 * width);
    const size_t intervals = std::max<size_t>(1, size_t(std::ceil((end - start).head<2>().norm() / cell_length)));
    return estimator.estimate_overhang_line_percentages(start, end, width, intervals, wall);
}

// Orca: The scalar the exporter writes for one move, the largest of its local readings.
float line_overhang_maximum(ExtrusionQualityEstimator &estimator, const Vec3d &start, const Vec3d &end, float width,
                            bool wall)
{
    const std::vector<float> profile = line_overhang_profile(estimator, start, end, width, wall);
    return profile.empty() ? 0.f : *std::max_element(profile.begin(), profile.end());
}

} // namespace

// Orca: The middle of a quarter-circle is not the middle of its chord. Check both directions
// against an analytic vertical support edge to catch sampling of the pre-fitting polyline instead.
TEST_CASE("Overhang arc samples follow fitted geometry in both directions", "[ExtrusionProcessor][Overhang][ArcFitting][Regression]")
{
    const bool clockwise = GENERATE(false, true);
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    // Orca: Mirror the support-layer link normally established by slicing.
    upper->lower_layer = lower;
    lower->lslices = {ExPolygon(Polygon{Point::new_scale(-20, -20), Point::new_scale(7, -20),
        Point::new_scale(7, 20), Point::new_scale(-20, 20)})};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    ArcSegment arc(Point::new_scale(0, 0), scale_(10.0),
        Point::new_scale(clockwise ? 0 : 10, clockwise ? 10 : 0),
        Point::new_scale(clockwise ? 10 : 0, clockwise ? 0 : 10),
        clockwise ? ArcDirection::Arc_Dir_CW : ArcDirection::Arc_Dir_CCW);
    const auto samples = estimator.estimate_overhang_arc_percentages(arc, 0.4f, 40);
    REQUIRE(samples.size() == 41);
    for (size_t i = 0; i < samples.size(); ++i) {
        const double angle = (clockwise ? 40 - i : i) * PI / 80.0;
        // Orca: Walls keep the contour offset past a full bead, up to ten widths.
        const double expected = 100.0 * std::clamp((10.0 * std::cos(angle) - 7.0 + 0.2) / 0.4, 0.0, 10.0);
        CHECK_THAT(samples[i], Catch::Matchers::WithinAbs(expected, 1e-3));
    }
    CHECK(samples[20] > 50.0f);
}

// Orca: The mid-chord sagitta of a faceted cylinder is approximation error, not overhang. A genuine
// radial contour shift of only 0.001 mm must survive even though it is much smaller than resolution.
TEST_CASE("Fitted arc correction preserves small real overhangs and supported inner walls", "[ExtrusionProcessor][Overhang][ArcFitting][Regression]")
{
    const double shift = GENERATE(0.0, 0.001, 0.08, 1.0);
    const bool inner_wall = GENERATE(false, true);
    CAPTURE(shift, inner_wall);
    constexpr size_t facets = 96;
    constexpr double radius = 10.0;
    constexpr float width = 0.4f;
    const auto contour = [](double r) {
        Polygon polygon;
        for (size_t i = 0; i < facets; ++i) {
            const double angle = 2.0 * PI * i / facets;
            polygon.points.push_back(Point::new_scale(r * std::cos(angle), r * std::sin(angle)));
        }
        return ExPolygon(polygon);
    };
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.02, 0.02, 0.01);
    Layer *upper = object->add_layer(1, 0.02, 0.04, 0.03);
    // Orca: Mirror the support-layer link normally established by slicing.
    upper->lower_layer = lower;
    lower->lslices = {contour(radius - shift)};
    upper->lslices = {contour(radius)};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    const double arc_radius = radius - (inner_wall ? 1.5 : 0.5) * width;
    const double end_angle = 2.0 * PI / facets;
    ArcSegment arc(Point::new_scale(0, 0), scale_(arc_radius), Point::new_scale(arc_radius, 0),
        Point::new_scale(arc_radius * std::cos(end_angle), arc_radius * std::sin(end_angle)), ArcDirection::Arc_Dir_CCW);
    const auto samples = estimator.estimate_overhang_arc_percentages(arc, width, 2);
    REQUIRE(samples.size() == 3);
    // Orca: Parallel polygon edges shift by shift*cos(half-angle); at mid-arc this is the exact
    // unsupported outer-wall width. The deeper inner wall, which needs no placement correction,
    // stays supported until its bead edge passes the lower facet, which only the 1 mm shift does.
    const double lower_apothem = (radius - shift) * std::cos(PI / facets);
    const double expected = inner_wall ?
        100.0 * std::clamp((arc_radius - lower_apothem + 0.5 * width) / width, 0.0, 10.0) :
        100.0 * std::clamp(shift * std::cos(PI / facets) / width, 0.0, 10.0);
    CHECK_THAT(samples[1], Catch::Matchers::WithinAbs(expected, 1e-3));
}

TEST_CASE("Printed material distinguishes supported lines from sparse infill gaps inside a solid outline",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    const ExtrusionRole role = GENERATE(erInternalInfill, erSolidInfill);
    const double shift = GENERATE(0.0, 0.2, 1.0);
    constexpr float width = 0.4f;
    constexpr float height = 0.2f;
    Print print;
    Model model;
    init_print({make_cube(6.0, 6.0, 0.4)}, print, model, shallow_overhang_config("classic", false, true));
    print.process();
    PrintObject *object = print.get_object(0);
    Layer *lower = object->get_layer(0);
    REQUIRE_FALSE(lower->regions().empty());
    for (LayerRegion *region : lower->regions()) {
        region->perimeters.clear();
        region->fills.clear();
    }
    ExtrusionPath base(erInternalInfill, width * height, width, height);
    base.polyline.points = {Point3::new_scale(-2, 0, 0), Point3::new_scale(2, 0, 0)};
    ExtrusionEntityCollection island;
    island.append(base);
    lower->get_region(0)->fills.append(std::move(island));
    ExtrusionPath path(role, width * height, width, height);
    path.polyline.points = {Point3::new_scale(-1, shift, 0), Point3::new_scale(1, shift, 0)};
    const auto original_points = path.polyline.points;
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_infill_layer(object, lower);
    const float percentage = line_overhang_maximum(estimator, unscaled(path.polyline.points.front()),
        unscaled(path.polyline.points.back()), width, false);
    const double expected = std::min(100.0, 100.0 * shift / width);
    CHECK_THAT(percentage, Catch::Matchers::WithinAbs(expected, 0.1));
    CHECK(path.polyline.points == original_points);
    libvgcode::PathVertex vertex;
    vertex.role = libvgcode::EGCodeExtrusionRole::SolidInfill;
    vertex.width = width;
    vertex.height = height;
    vertex.overhang_z_distance = height;
    vertex.overhang_percentage = percentage;
    // Infill is measured up to a full bead, so a fully unsupported line is a bridge.
    const double expected_angle = shift == 0.0 ? 0.0 : (shift < width ? 45.0 : 90.0);
    CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(expected_angle, 0.1));
    // A travel-only path must not fill the gap in the next reference.
    ExtrusionPath travel = path;
    travel.set_force_no_extrusion(true);
    lower->get_region(0)->fills.append(travel);
    estimator.forget_infill_footprints();
    estimator.prepare_for_infill_layer(object, lower);
    CHECK_THAT(line_overhang_maximum(estimator, unscaled(path.polyline.points.front()),
        unscaled(path.polyline.points.back()), width, false), Catch::Matchers::WithinAbs(expected, 0.1));
}

TEST_CASE("Infill arc metadata samples local coverage", "[ExtrusionProcessor][Overhang][ArcFitting][Regression]")
{
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    print.process();
    PrintObject *object = print.get_object(0);
    Layer *lower = object->get_layer(0);
    for (LayerRegion *region : lower->regions()) {
        region->perimeters.clear();
        region->fills.clear();
    }
    // A wide vertical extrusion gives the analytic support edge x = 7 at every arc sample.
    ExtrusionPath material(erSolidInfill, 27.0 * 0.2, 27.0f, 0.2f);
    material.polyline.points = {Point3::new_scale(-6.5, -20, 0), Point3::new_scale(-6.5, 20, 0)};
    lower->get_region(0)->fills.append(material);
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_infill_layer(object, lower);
    ArcSegment arc(Point::new_scale(0, 0), scale_(10.0), Point::new_scale(10, 0),
        Point::new_scale(0, 10), ArcDirection::Arc_Dir_CCW);
    const auto samples = estimator.estimate_overhang_arc_percentages(arc, 0.4f, 40, false);
    REQUIRE(samples.size() == 41);
    for (size_t i = 0; i < samples.size(); ++i) {
        const double x = 10.0 * std::cos(i * PI / 80.0);
        const double unsupported_width = x - 7.0 + 0.2;
        CHECK_THAT(samples[i], Catch::Matchers::WithinAbs(100.0 * std::clamp(unsupported_width / 0.4, 0.0, 1.0), 0.1));
    }
}

TEST_CASE("Sparse infill exports unsupported gaps inside unchanged model outlines without changing commands",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    const double shift = GENERATE(0.0, 0.2, 1.0);
    const bool slowdown = GENERATE(false, true);
    constexpr float width = 0.4f;
    std::vector<std::string> baseline;
    for (bool enabled : {false, true}) {
        DynamicPrintConfig config = shallow_overhang_config("classic", slowdown, enabled);
        config.set_deserialize_strict({
            {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
            {"sparse_infill_density", "25%"}, {"top_shell_layers", "0"}, {"bottom_shell_layers", "1"},
            {"enable_overhang_bridge_fan", "1"}, {"enable_arc_fitting", "0"},
        });
        Print print;
        Model model;
        init_print({make_cube(6.0, 6.0, 0.8)}, print, model, config, nullptr, false);
        print.process();
        PrintObject *object = print.get_object(0);
        REQUIRE(object->layer_count() >= 3);
        Layer *lower = object->get_layer(1);
        Layer *upper = object->get_layer(2);
        REQUIRE_FALSE(lower->regions().empty());
        REQUIRE_FALSE(upper->regions().empty());
        const auto set_fill = [](Layer &layer, const ExtrusionPath &path) {
            for (LayerRegion *region : layer.regions()) {
                region->perimeters.clear();
                region->fills.clear();
            }
            ExtrusionEntityCollection island;
            island.append(path);
            layer.get_region(0)->fills.append(std::move(island));
        };
        ExtrusionPath base(erInternalInfill, width * 0.2, width, 0.2f);
        base.polyline.points = {Point3::new_scale(-2, 0, 0), Point3::new_scale(2, 0, 0)};
        set_fill(*lower, base);
        ExtrusionPath path = base;
        path.polyline.points = {Point3::new_scale(-1, shift, 0), Point3::new_scale(1, shift, 0)};
        set_fill(*upper, path);
        REQUIRE(lower->lslices.front().contains(path.first_point()));
        REQUIRE(upper->lslices.front().contains(path.first_point()));
        GCodeProcessorResult result;
        const std::string exported = gcode(print, &result);
        const auto commands = printer_commands(exported);
        REQUIRE_FALSE(commands.empty());
        if (!enabled) {
            baseline = commands;
            continue;
        }
        CHECK(commands == baseline);
        size_t measured = 0;
        for (const auto &move : result.moves) {
            if (move.type != EMoveType::Extrude || move.extrusion_role != erInternalInfill ||
                std::abs(move.position.z() - upper->print_z) > 1e-4)
                continue;
            CHECK_THAT(move.overhang_percentage, Catch::Matchers::WithinAbs(std::min(100.0, 100.0 * shift / width), 0.1));
            ++measured;
        }
        REQUIRE(measured > 0);
    }
}

TEST_CASE("Infill overhang coloring stays local to unsupported ends and interior gaps",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    const ExtrusionRole role = GENERATE(erSolidInfill, erInternalInfill, erInternalBridgeInfill, erBridgeInfill);
    const bool interior = GENERATE(false, true);
    const bool reverse = GENERATE(false, true);
    constexpr float width = 0.4f;
    constexpr double y = 0.0;
    std::vector<std::string> baseline;
    double baseline_time = 0.0;
    for (bool enabled : {false, true}) {
        auto config = shallow_overhang_config("classic", true, enabled);
        config.set_deserialize_strict({{"initial_layer_print_height", "0.2"}, {"layer_height", "0.2"},
            {"gcode_comments", "0"}, {"nozzle_diameter", "0.4"}, {"line_width", "0.4"}, {"enable_arc_fitting", "0"},
            {"sparse_infill_density", "25%"}, {"top_shell_layers", "0"}, {"bottom_shell_layers", "1"}});
        config.erase("nozzle_type");
        config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
        Print print;
        Model model;
        init_print({make_cube(6.0, 6.0, 0.8)}, print, model, config, nullptr, false);
        print.apply(model, config); // init_print enables ordinary comments; exercise their disabled state.
        print.process();
        PrintObject *object = print.get_object(0);
        REQUIRE(object->layer_count() >= 3);
        Layer *lower = object->get_layer(1);
        Layer *upper = object->get_layer(2);
        for (Layer *layer : {lower, upper})
            for (LayerRegion *region : layer->regions()) {
                region->perimeters.clear();
                region->fills.clear();
            }
        ExtrusionEntityCollection material;
        const auto add_material = [&](double x0, double x1) {
            ExtrusionPath base(erInternalInfill, width * 0.2, width, 0.2f);
            base.polyline.points = {Point3::new_scale(x0, y, 0), Point3::new_scale(x1, y, 0)};
            material.append(base);
        };
        add_material(-3.0, interior ? -0.8 : 1.0);
        if (interior)
            add_material(0.8, 3.0);
        ExtrusionPath path(role, width * 0.2, width, 0.2f);
        path.polyline.points = {Point3::new_scale(reverse ? 2 : -2, y, 0), Point3::new_scale(reverse ? -2 : 2, y, 0)};
        lower->get_region(0)->fills.append(std::move(material));
        ExtrusionEntityCollection island;
        island.append(path);
        upper->get_region(0)->fills.append(std::move(island));
        GCodeProcessorResult result;
        const std::string exported = gcode(print, &result);
        const auto commands = printer_commands(exported);
        const double time = result.print_statistics.modes[0].time;
        if (!enabled) {
            baseline = commands;
            baseline_time = time;
            CHECK(result.overhang_line_profiles.empty());
            continue;
        }
        CHECK(commands == baseline);
        CHECK_THAT(time, Catch::Matchers::WithinAbs(baseline_time, 1e-5));
        REQUIRE_FALSE(result.overhang_line_profiles.empty());
        const auto check_local_colors = [&](const GCodeProcessorResult &preview) {
            bool supported = false;
            bool unsupported = false;
            double length = 0.0;
            preview.for_each_preview_move([&](const auto &prev, const auto &curr, bool) {
                if (curr.type != EMoveType::Extrude || curr.extrusion_role != role ||
                    std::abs(curr.position.z() - upper->print_z) > 1e-4)
                    return;
                const double middle = 0.5 * (prev.position.x() + curr.position.x()) -
                    unscale<double>(object->instances().front().shift.x());
                length += (curr.position - prev.position).norm();
                if (interior ? std::abs(middle) > 1.3 : middle < 0.0) {
                    CHECK_THAT(curr.overhang_percentage, Catch::Matchers::WithinAbs(0.0, 0.1));
                    supported = true;
                }
                if (interior ? std::abs(middle) < 0.3 : middle > 1.4) {
                    CHECK_THAT(curr.overhang_percentage, Catch::Matchers::WithinAbs(100.0, 0.1));
                    unsupported = true;
                }
            });
            CHECK(supported);
            CHECK(unsupported);
            CHECK_THAT(length, Catch::Matchers::WithinAbs(4.0, 1e-4));
        };
        check_local_colors(result);
        GCodeProcessor imported;
        imported.apply_config(FullPrintConfig());
        imported.initialize_result_moves();
        imported.process_buffer(exported);
        imported.finalize(false);
        check_local_colors(imported.get_result());
    }
}

// Orca: A straight wall is one G1 when neither speed nor cooling splits it. Its unsupported span over a notch in
// the layer below must stay local in the preview instead of coloring the whole move with its maximum.
TEST_CASE("Straight walls export local overhang profiles without changing commands",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    std::vector<std::string> baseline;
    for (bool enabled : {false, true}) {
        auto config = shallow_overhang_config("classic", false, enabled);
        config.set_deserialize_strict({{"initial_layer_print_height", "0.2"}, {"layer_height", "0.2"},
            {"gcode_comments", "0"}, {"nozzle_diameter", "0.4"}, {"line_width", "0.4"}, {"enable_arc_fitting", "0"},
            {"enable_overhang_bridge_fan", "0"}, {"top_shell_layers", "0"}, {"bottom_shell_layers", "1"}});
        config.erase("nozzle_type");
        config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
        Print print;
        Model model;
        init_print({make_cube(6.0, 6.0, 0.8)}, print, model, config, nullptr, false);
        print.apply(model, config); // init_print enables ordinary comments; exercise their disabled state.
        print.process();
        PrintObject *object = print.get_object(0);
        REQUIRE(object->layer_count() >= 3);
        Layer *lower = object->get_layer(1);
        Layer *upper = object->get_layer(2);
        for (Layer *layer : {lower, upper})
            for (LayerRegion *region : layer->regions()) {
                region->perimeters.clear();
                region->fills.clear();
            }
        // The layer below recedes by one width behind the middle 2 mm of the front face.
        lower->lslices = {ExPolygon(Polygon{Point::new_scale(-3, -3), Point::new_scale(-1, -3),
            Point::new_scale(-1, -3 + width), Point::new_scale(1, -3 + width), Point::new_scale(1, -3),
            Point::new_scale(3, -3), Point::new_scale(3, 3), Point::new_scale(-3, 3)})};
        ExtrusionPath wall(erExternalPerimeter, width * 0.2, width, 0.2f);
        wall.polyline.points = {Point3::new_scale(-2, -3 + 0.5 * width, 0), Point3::new_scale(2, -3 + 0.5 * width, 0)};
        ExtrusionEntityCollection island;
        island.append(wall);
        upper->get_region(0)->perimeters.append(std::move(island));
        GCodeProcessorResult result;
        const auto commands = printer_commands(gcode(print, &result));
        if (!enabled) {
            baseline = commands;
            continue;
        }
        CHECK(commands == baseline);
        REQUIRE_FALSE(result.overhang_line_profiles.empty());
        bool supported = false;
        bool unsupported = false;
        result.for_each_preview_move([&](const auto &prev, const auto &curr, bool) {
            if (curr.type != EMoveType::Extrude || curr.extrusion_role != erExternalPerimeter ||
                std::abs(curr.position.z() - upper->print_z) > 1e-4)
                return;
            const double middle = 0.5 * (prev.position.x() + curr.position.x()) -
                unscale<double>(object->instances().front().shift.x());
            if (std::abs(middle) > 1.3) {
                CHECK_THAT(curr.overhang_percentage, Catch::Matchers::WithinAbs(0.0, 0.1));
                supported = true;
            } else if (std::abs(middle) < 0.5) {
                CHECK_THAT(curr.overhang_percentage, Catch::Matchers::WithinAbs(100.0, 0.1));
                unsupported = true;
            }
        });
        CHECK(supported);
        CHECK(unsupported);
    }
}

// Orca: Infill combination prints one bead for a group of layers and leaves the layers under it void. Grid lines
// repeat at the same place, so each combined bead rests on the one printed for the group below.
TEST_CASE("Combined infill is measured against the infill it rests on", "[ExtrusionProcessor][Overhang][Regression]")
{
    // Over a raft without bottom shells, the lowest bead combines the first two object layers and rests on the raft.
    const bool raft = GENERATE(false, true);
    CAPTURE(raft);
    std::vector<std::string> baseline;
    for (bool enabled : {false, true}) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // A 0.4 mm nozzle combines pairs of 0.2 mm layers into 0.4 mm beads.
        config.set_deserialize_strict({
            {"nozzle_diameter", "0.4"}, {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
            {"wall_loops", "1"}, {"sparse_infill_density", "20%"}, {"sparse_infill_pattern", "grid"},
            {"infill_combination", "1"}, {"top_shell_layers", "0"}, {"bottom_shell_layers", raft ? "0" : "1"},
            {"raft_layers", raft ? "2" : "0"}, {"enable_arc_fitting", "0"}, {"gcode_overhangs", enabled ? "1" : "0"},
        });
        Print print;
        Model model;
        init_print({make_cube(10.0, 10.0, 2.0)}, print, model, config);
        GCodeProcessorResult result;
        const auto commands = printer_commands(gcode(print, &result));
        if (!enabled) {
            baseline = commands;
            continue;
        }
        CHECK(commands == baseline);
        double combined_length = 0.0;
        double supported_length = 0.0;
        result.for_each_preview_move([&](const auto &prev, const auto &curr, bool) {
            if (curr.type != EMoveType::Extrude || curr.extrusion_role != erInternalInfill || curr.height < 0.3f)
                return;
            // A bead on the raft has no object layer under it, so no spacing either.
            if (!raft || curr.overhang_z_distance > EPSILON)
                CHECK_THAT(curr.overhang_z_distance, Catch::Matchers::WithinAbs(0.4, 1e-3));
            const double length = (curr.position - prev.position).norm();
            combined_length += length;
            if (curr.overhang_percentage < 50.0f)
                supported_length += length;
        });
        REQUIRE(combined_length > 0.0);
        // The layer directly below the bead is void; measured against it, nearly all of it would read unsupported.
        CHECK(supported_length > 0.8 * combined_length);
    }
}

// Orca: The generator builds the footprint for the layer below in its parallel stage. It must describe the same
// material as the footprint built on demand, and only be built while metadata is exported.
TEST_CASE("Precomputed infill footprints match the footprint built on demand", "[ExtrusionProcessor][Overhang]")
{
    Print print;
    Model model;
    init_print({make_cube(6.0, 6.0, 1.0)}, print, model, {{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"sparse_infill_density", "20%"}, {"gcode_overhangs", 1}});
    print.process();
    const PrintObject *object = print.objects().front();
    REQUIRE(object->layer_count() >= 3);
    const Layer *layer = object->get_layer(2);
    CHECK_FALSE(precompute_overhang_layer(object, *layer).lower_infill_footprint);
    PrecomputedOverhangLayer precomputed = precompute_overhang_layer(object, *layer, false, true);
    const auto footprint = precomputed.lower_infill_footprint;
    REQUIRE(footprint);

    ExtrusionQualityEstimator generator;
    std::vector<PrecomputedOverhangLayer> precomputed_layers;
    precomputed_layers.push_back(std::move(precomputed));
    generator.set_precomputed_layers(std::move(precomputed_layers));
    REQUIRE(generator.precomputed_layer(object, layer) != nullptr);
    CHECK(generator.precomputed_layer(object, layer)->lower_infill_footprint == footprint);
    CHECK(generator.precomputed_layer(object, layer->lower_layer) == nullptr);

    ExtrusionQualityEstimator built;
    built.set_current_object(object);
    built.prepare_for_infill_layer(object, layer->lower_layer);
    generator.set_current_object(object);
    generator.prepare_for_infill_layer(object, layer->lower_layer, footprint);
    constexpr float width = 0.4f;
    for (double y = -3.5; y <= 3.5; y += 0.7) {
        const Vec3d start(-3.5, y, 0.);
        const Vec3d end(3.5, y + 0.3, 0.);
        const auto expected = line_overhang_profile(built, start, end, width, false);
        const auto reused = line_overhang_profile(generator, start, end, width, false);
        REQUIRE(reused.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            CHECK_THAT(reused[i], Catch::Matchers::WithinAbs(expected[i], 1e-6));
    }

    // A footprint handed over is measured against as given: here only the half of the layer at x < 0 is printed.
    ExtrusionQualityEstimator handed;
    handed.set_current_object(object);
    handed.prepare_for_infill_layer(object, layer->lower_layer, std::make_shared<const AABBTreeLines::LinesDistancer<Linef3>>(
        to_unscaled_linesf3(ExPolygons{ExPolygon(Polygon{Point::new_scale(-3, -3), Point::new_scale(0, -3),
            Point::new_scale(0, 3), Point::new_scale(-3, 3)})})));
    CHECK_THAT(line_overhang_maximum(handed, Vec3d(-2.5, 0., 0.), Vec3d(-0.5, 0., 0.), width, false),
        Catch::Matchers::WithinAbs(0.0, 1e-6));
    CHECK_THAT(line_overhang_maximum(handed, Vec3d(0.5, 0., 0.), Vec3d(2.5, 0., 0.), width, false),
        Catch::Matchers::WithinAbs(100.0, 1e-6));
}

// Orca: A wall bead fully over air slopes only from a lower edge of its own, met along its inward normal inside the
// current layer's material. Without one it reads as horizontal rather than as a slope towards unrelated geometry.
TEST_CASE("Unsupported walls without a lower contour of their own read as horizontal",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    enum class Case { IslandNearWall, IslandFarWall, Shelf, FlangeRight, FlangeLeft };
    const Case wall = GENERATE(Case::IslandNearWall, Case::IslandFarWall, Case::Shelf, Case::FlangeRight, Case::FlangeLeft);
    CAPTURE(int(wall));
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    const auto rectangle = [](double x0, double y0, double x1, double y1) {
        return ExPolygon(Polygon{Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1),
            Point::new_scale(x0, y1)});
    };
    Vec3d start;
    Vec3d end;
    switch (wall) {
    case Case::IslandNearWall:
    case Case::IslandFarWall:
        // An island starting 0.5 mm to the right of the body: its left wall faces the body, its right wall faces
        // away across the island and the gap.
        lower->lslices = {rectangle(-10, -5, -0.5, 5)};
        upper->lslices = {lower->lslices.front(), rectangle(0, -5, 3, 5)};
        start = Vec3d(wall == Case::IslandNearWall ? 0.5 * width : 3. - 0.5 * width, -3., 0.);
        end   = Vec3d(start.x(), 3., 0.);
        break;
    case Case::Shelf:
        // A shelf 5 mm long sticking out of the body's right face; its side wall runs away from the face.
        lower->lslices = {rectangle(-10, -5, 0, 5)};
        upper->lslices = {ExPolygon(Polygon{Point::new_scale(-10, -5), Point::new_scale(0, -5), Point::new_scale(0, -1),
            Point::new_scale(5, -1), Point::new_scale(5, 1), Point::new_scale(0, 1), Point::new_scale(0, 5),
            Point::new_scale(-10, 5)})};
        start = Vec3d(1., 1. - 0.5 * width, 0.);
        end   = Vec3d(4., 1. - 0.5 * width, 0.);
        break;
    case Case::FlangeRight:
    case Case::FlangeLeft: {
        // A flange flush with the body's top and bottom faces, on either side of it: its top wall meets the body
        // at the body's corner, where the aligned and the crosswise body face are equally near.
        const double side = wall == Case::FlangeRight ? 1. : -1.;
        lower->lslices = {wall == Case::FlangeRight ? rectangle(-10, -5, 0, 5) : rectangle(0, -5, 10, 5)};
        upper->lslices = {wall == Case::FlangeRight ? rectangle(-10, -5, 3, 5) : rectangle(-3, -5, 10, 5)};
        start = Vec3d(side * 0.5, 5. - 0.5 * width, 0.);
        end   = Vec3d(side * 2.5, 5. - 0.5 * width, 0.);
        break;
    }
    }
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    const auto profile = line_overhang_profile(estimator, start, end, width, true);
    REQUIRE_FALSE(profile.empty());
    for (float percentage : profile)
        CHECK_THAT(percentage, Catch::Matchers::WithinAbs(GCodeProcessor::MAX_OVERHANG_PERCENTAGE, 1e-3));
    libvgcode::PathVertex vertex;
    vertex.role = libvgcode::EGCodeExtrusionRole::ExternalPerimeter;
    vertex.width = width;
    vertex.height = 0.2f;
    vertex.overhang_z_distance = 0.2f;
    vertex.overhang_percentage = profile.front();
    CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(90.0, 1e-6));
}

// Orca: At a concave corner of a steep face, the lower copy of the face it meets is nearer than the face's own lower
// edge. The face's own edge decides, so no cell reads horizontal or steeper than the face.
TEST_CASE("Steep faces keep their angle into a concave corner", "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float  width = 0.45f;
    constexpr double depth = 0.8; // The face leans 0.8 mm out per 0.2 mm layer: about 76 degrees.
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    // The material fills x < 0 or y < 0 above and x < 0 or y < -depth below: face y = 0 overhangs, face x = 0 stands.
    lower->lslices = {ExPolygon(Polygon{Point::new_scale(-10, -10), Point::new_scale(10, -10), Point::new_scale(10, -depth),
        Point::new_scale(0, -depth), Point::new_scale(0, 10), Point::new_scale(-10, 10)})};
    upper->lslices = {ExPolygon(Polygon{Point::new_scale(-10, -10), Point::new_scale(10, -10), Point::new_scale(10, 0),
        Point::new_scale(0, 0), Point::new_scale(0, 10), Point::new_scale(-10, 10)})};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_lower_layer(object, upper);
    const auto profile = line_overhang_profile(estimator, Vec3d(-0.5 * width, -0.5 * width, 0.),
        Vec3d(2. - 0.5 * width, -0.5 * width, 0.), width, true);
    REQUIRE(profile.size() > 2);
    for (float percentage : profile)
        CHECK(percentage <= float(100. * depth / width) + 0.5f);
    CHECK_THAT(profile.back(), Catch::Matchers::WithinAbs(100. * depth / width, 0.5));
}

TEST_CASE("Infill preview metadata preserves commands and excludes support paths",
          "[ExtrusionProcessor][Overhang][Support][Regression]")
{
    const auto [role, support, tree] = GENERATE(table<ExtrusionRole, bool, bool>({
        {erInternalInfill, false, false}, {erSolidInfill, false, false}, {erTopSolidInfill, false, false},
        {erBottomSurface, false, false}, {erGapFill, false, false}, {erIroning, false, false},
        {erSupportMaterial, true, false}, {erSupportMaterialInterface, true, false},
        {erSupportTransition, true, false}, {erIroning, true, false},
        {erSupportMaterial, true, true}, {erSupportMaterialInterface, true, true},
        {erSupportTransition, true, true}, {erIroning, true, true},
    }));
    const bool slowdown = GENERATE(false, true);
    CAPTURE(role, support, tree, slowdown);
    const bool has_metadata = !support && role != erIroning;
    constexpr float width = 0.4f;
    std::vector<std::string> baseline_commands;
    GCodeProcessorResult baseline_result;
    for (bool enabled : {false, true}) {
        DynamicPrintConfig config = shallow_overhang_config("classic", slowdown, enabled);
        config.set_deserialize_strict({
            {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
            {"line_width", width}, {"enable_support", "1"},
            {"support_type", tree ? "tree(auto)" : "normal(auto)"}, {"support_style", tree ? "organic" : "default"},
            {"enable_overhang_bridge_fan", "1"}, {"gcode_comments", "0"}, {"enable_arc_fitting", "0"},
        });
        config.erase("nozzle_type");
        config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
        Print print;
        Model model;
        init_print({make_cube(6.0, 6.0, 0.8)}, print, model, config, nullptr, false);
        print.apply(model, config); // init_print enables comments; exercise the requested disabled-comment mode.
        print.set_status_silent();
        print.process();
        PrintObject *object = print.get_object(0);
        REQUIRE(object->layer_count() > 1);
        Layer *lower = object->get_layer(0);
        Layer *upper = object->get_layer(1);
        // Model infill keeps the half-width reading. Support roles, including support ironing,
        // stay at zero even above an unsupported path and after model metadata has been emitted.
        const BoundingBox bounds = get_extents(lower->lslices);
        const double x = unscale<double>(bounds.min.x()) + 1.0;
        const double y = unscale<double>(bounds.max.y());
        // Support tops deliberately fall between model layers, with a different layer height.
        const double target_print_z = upper->print_z + (support ? 0.1 : 0.0);
        const float extrusion_height = support ? 0.3f : 0.2f;
        ExtrusionPath path(role, width * extrusion_height, width, extrusion_height);
        path.polyline.points = {Point3::new_scale(x, y, 0), Point3::new_scale(x + 2.0, y, 0)};
        object->clear_support_layers();
        if (support) {
            SupportLayer *base = object->add_support_layer(0, 0, 0.2, lower->print_z);
            SupportLayer *support_layer = object->add_support_layer(1, 1, extrusion_height, target_print_z);
            base->support_type = support_layer->support_type = tree ? stInnerTree : stInnerNormal;
            // Place support outside the model: model contours must not be used as support contours.
            const double support_y = y + 5.0;
            ExtrusionPath base_path(erSupportMaterial, width * 0.2, width, 0.2f);
            base_path.polyline.points = {Point3::new_scale(x - 1.0, support_y, 0),
                Point3::new_scale(x + 3.0, support_y, 0)};
            base->support_fills.append(base_path);
            path.polyline.points = {Point3::new_scale(x, support_y + width / 2, 0),
                Point3::new_scale(x + 2.0, support_y + width / 2, 0)};
            if (role == erIroning) {
                ExtrusionPath interface_path(erSupportMaterialInterface, width * extrusion_height, width, extrusion_height);
                interface_path.polyline = path.polyline;
                support_layer->support_fills.append(interface_path);
            }
            support_layer->support_fills.append(path);
        } else {
            REQUIRE_FALSE(upper->regions().empty());
            REQUIRE_FALSE(lower->regions().empty());
            for (LayerRegion *region : lower->regions()) {
                region->perimeters.clear();
                region->fills.clear();
            }
            ExtrusionPath base_path(erInternalInfill, width * 0.2, width, 0.2f);
            base_path.polyline.points = {Point3::new_scale(x - 1.0, y - width / 2, 0),
                Point3::new_scale(x + 3.0, y - width / 2, 0)};
            ExtrusionEntityCollection material;
            material.append(base_path);
            lower->get_region(0)->fills.append(std::move(material));
            upper->get_region(0)->fills.clear();
            ExtrusionEntityCollection island;
            island.append(path);
            upper->get_region(0)->fills.append(std::move(island));
        }

        GCodeProcessorResult result;
        const std::string exported = gcode(print, &result);
        const auto commands = printer_commands(exported);
        REQUIRE_FALSE(commands.empty());
        if (!enabled) {
            baseline_commands = commands;
            baseline_result = std::move(result);
            CHECK(overhang_percentages(exported).empty());
            continue;
        }
        REQUIRE(result.has_overhang_metadata);
        CHECK(commands == baseline_commands);
        REQUIRE(result.moves.size() == baseline_result.moves.size());
        size_t measured_moves = 0;
        for (size_t i = 0; i < result.moves.size(); ++i) {
            const auto &move = result.moves[i];
            const auto &baseline = baseline_result.moves[i];
            CHECK(move.type == baseline.type);
            CHECK(move.extrusion_role == baseline.extrusion_role);
            CHECK_THAT((move.position - baseline.position).norm(), Catch::Matchers::WithinAbs(0.0, 1e-6));
            CHECK_THAT(move.feedrate, Catch::Matchers::WithinAbs(baseline.feedrate, 1e-6));
            CHECK_THAT(move.fan_speed, Catch::Matchers::WithinAbs(baseline.fan_speed, 1e-6));
            if (move.type == EMoveType::Extrude && move.extrusion_role == role &&
                std::abs(move.position.z() - target_print_z) < 1e-4) {
                CHECK_THAT(move.overhang_percentage,
                    Catch::Matchers::WithinAbs(has_metadata ? 50.0 : GCodeProcessor::NO_OVERHANG_PERCENTAGE, 0.1));
                if (has_metadata)
                    CHECK_THAT(move.overhang_z_distance, Catch::Matchers::WithinAbs(extrusion_height, 1e-5));
                else
                    CHECK(move.overhang_profile_id == 0);
                ++measured_moves;
            }
        }
        REQUIRE(measured_moves > 0);
        GCodeProcessor imported;
        imported.apply_config(FullPrintConfig());
        imported.initialize_result_moves();
        imported.process_buffer(exported);
        imported.finalize(false);
        const bool imported_metadata = std::any_of(imported.get_result().moves.begin(), imported.get_result().moves.end(), [role = role](const auto &move) {
            return move.type == EMoveType::Extrude && move.extrusion_role == role && move.overhang_percentage > 1.0f;
        });
        CHECK(imported_metadata == has_metadata);
    }
}

// Orca: Both ends of an unsplit wall are supported, but a recessed contour leaves half its width
// unsupported inside the span. Sampling for metadata must find that pocket without editing the path.
TEST_CASE("Overhang metadata detects unsupported interiors without changing the path", "[ExtrusionProcessor][Overhang][Regression]")
{
    const double pocket_start = GENERATE(10.0, 18.0);
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    // Orca: Mirror the support-layer link normally established by slicing.
    upper->lower_layer = lower;
    lower->lslices = {ExPolygon(Slic3r::Polygon{
        Point::new_scale(0, 0), Point::new_scale(pocket_start, 0),
        Point::new_scale(pocket_start, 0.2), Point::new_scale(pocket_start + 4, 0.2),
        Point::new_scale(pocket_start + 4, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    ExtrusionPath path(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline.points = {Point3::new_scale(1, 0.2, 0), Point3::new_scale(39, 0.2, 0)};
    const auto original_points = path.polyline.points;
    const Vec3d start = unscaled(path.polyline.points.front());
    const Vec3d end = unscaled(path.polyline.points.back());
    const auto profile = line_overhang_profile(estimator, start, end, 0.4f, true);
    REQUIRE(profile.size() > 1);
    CHECK_THAT(*std::max_element(profile.begin(), profile.end()), Catch::Matchers::WithinAbs(50.0, 1e-3));
    // Orca: The reading stays on the cells over the pocket instead of covering the whole move.
    for (size_t i = 0; i < profile.size(); ++i) {
        const double x = start.x() + (end.x() - start.x()) * (double(i) + 0.5) / double(profile.size());
        if (x < pocket_start - 0.5 || x > pocket_start + 4.5)
            CHECK_THAT(profile[i], Catch::Matchers::WithinAbs(0.0, 1e-3));
        else if (x > pocket_start + 0.5 && x < pocket_start + 3.5)
            CHECK_THAT(profile[i], Catch::Matchers::WithinAbs(50.0, 1e-3));
    }
    CHECK(path.polyline.points == original_points);
}

// Orca: Arachne may place an outer-wall center closer than half its width to the sliced contour.
// Compare layers relative to that actual inset so identical contours remain supported while a real
// outward shift retains its geometric percentage in both fixed- and variable-speed metadata paths.
TEST_CASE("Overhang metadata uses the current contour as its wall placement baseline",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    // Orca: 0.6 mm is one and a half widths: walls report it whole, which the angle preview needs.
    const double outward_shift = GENERATE(0.0, 0.001, 0.1, 0.6);
    const double z_offset = GENERATE(0.0, -0.1, -0.2);
    // Orca: A uniform sub-quantization shift is still a real shallow slope and must be retained.
    const float expected_percentage = float(100.0 * outward_shift / width);
    CAPTURE(outward_shift, z_offset);

    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    lower->lslices = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    upper->lslices = {ExPolygon(Polygon{Point::new_scale(0, -outward_shift), Point::new_scale(40, -outward_shift),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};

    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    ExtrusionPath path(erExternalPerimeter, 0.08, width, 0.2f);
    const double path_y = -outward_shift + 0.1;
    // Z contouring changes the extrusion elevation, not the horizontal slice-contour displacement.
    path.z_contoured = z_offset != 0.0;
    path.polyline.points = {Point3::new_scale(1, path_y, z_offset), Point3::new_scale(39, path_y, z_offset)};

    const auto profile = line_overhang_profile(estimator, unscaled(path.polyline.points.front()),
        unscaled(path.polyline.points.back()), width, true);
    REQUIRE_FALSE(profile.empty());
    for (float percentage : profile)
        CHECK_THAT(percentage, Catch::Matchers::WithinAbs(expected_percentage, 1e-3));

    // Orca: Variable-speed output samples each emitted span the same way.
    const auto variable_speed_points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false);
    REQUIRE(variable_speed_points.size() >= 2);
    for (size_t i = 0; i + 1 < variable_speed_points.size(); ++i)
        CHECK_THAT(line_overhang_maximum(estimator, unscaled(variable_speed_points[i].p),
            unscaled(variable_speed_points[i + 1].p), width, true), Catch::Matchers::WithinAbs(expected_percentage, 1e-3));

    // Orca: The angle follows the whole offset over the 0.2 mm slice spacing, with no jump at a full bead.
    libvgcode::PathVertex vertex;
    vertex.role = libvgcode::EGCodeExtrusionRole::ExternalPerimeter;
    vertex.width = width;
    vertex.height = 0.2f;
    vertex.overhang_z_distance = 0.2f;
    vertex.overhang_percentage = profile.front();
    CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(std::atan(outward_shift / 0.2) * 180.0 / PI, 1e-2));
}

TEST_CASE("Variable speed overhang metadata retains shallow pockets between speed changes",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    constexpr double shallow_depth = 0.04;
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    lower->lslices = {ExPolygon(Polygon{
        Point::new_scale(0, 0), Point::new_scale(8, 0),
        Point::new_scale(8, shallow_depth), Point::new_scale(12, shallow_depth),
        Point::new_scale(12, 0), Point::new_scale(28, 0),
        Point::new_scale(28, 0.3), Point::new_scale(32, 0.3),
        Point::new_scale(32, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    upper->lslices = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_lower_layer(object, upper);
    ExtrusionPath path(erExternalPerimeter, 0.08, width, 0.2f);
    path.polyline.points = {Point3::new_scale(1, 0.2, 0), Point3::new_scale(39, 0.2, 0)};
    const ConfigOptionPercents overlaps({75, 50, 0});
    const ConfigOptionFloatsOrPercents speeds({FloatOrPercent{100, false}, FloatOrPercent{100, false}, FloatOrPercent{20, false}});
    const auto points = estimator.estimate_extrusion_quality(path, overlaps, speeds, 100.f, 100.f, false);
    REQUIRE(std::any_of(points.begin(), points.end(), [](const auto &point) { return point.speed < 99.f; }));
    bool checked_shallow_pocket = false;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (unscale_(points[i].p.x()) <= 10.0 && unscale_(points[i + 1].p.x()) >= 10.0) {
            CHECK_THAT(line_overhang_maximum(estimator, unscaled(points[i].p), unscaled(points[i + 1].p), width, true),
                Catch::Matchers::WithinAbs(100.0 * shallow_depth / width, 1e-3));
            checked_shallow_pocket = true;
        }
    }
    REQUIRE(checked_shallow_pocket);
}

// Orca: A curled edge may require slower motion and extra cooling even on a supported vertical wall.
// Its artificial slowdown distance must not be exported as geometric overhang metadata.
TEST_CASE("Overhang geometry is independent of curled edge slowdown", "[ExtrusionProcessor][Overhang][Regression]")
{
    const bool curled_slowdown = GENERATE(false, true);
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    // Orca: Mirror the support-layer link normally established by slicing.
    upper->lower_layer = lower;
    lower->lslices = {ExPolygon(Slic3r::Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    lower->curled_lines = {CurledLine(Point::new_scale(1, 0.2), Point::new_scale(39, 0.2), 1.0f)};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    ExtrusionPath path(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline.points = {Point3::new_scale(1, 0.2, 0), Point3::new_scale(39, 0.2, 0)};
    const auto points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, curled_slowdown);
    REQUIRE(points.size() >= 2);
    CHECK((points.front().speed < 99.0f) == curled_slowdown);
    CHECK((points.front().overlap < 0.99f) == curled_slowdown);
    for (size_t i = 0; i + 1 < points.size(); ++i)
        CHECK_THAT(line_overhang_maximum(estimator, unscaled(points[i].p), unscaled(points[i + 1].p), 0.4f, true),
            Catch::Matchers::WithinAbs(0.0, 1e-3));
}

// Orca: Metadata measures a wall against the layer directly below it, however the estimator was prepared before:
// fresh, after a stale layer, continuously, or when the same layer is prepared twice.
TEST_CASE("Overhang metadata measures walls against the layer directly below", "[ExtrusionProcessor][Overhang][Regression]")
{
    const int prepared_count = GENERATE(0, 1, 2);
    CAPTURE(prepared_count);
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *older = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *lower = object->add_layer(1, 0.2, 0.4, 0.3);
    Layer *upper = object->add_layer(2, 0.2, 0.6, 0.5);
    lower->lower_layer = older;
    upper->lower_layer = lower;
    // Orca: The stale contour cannot support the wall, whereas the actual lower layer supports it fully.
    older->lslices = {ExPolygon(Polygon{Point::new_scale(0, 1), Point::new_scale(40, 1),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    lower->lslices = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    if (prepared_count >= 1)
        estimator.prepare_for_lower_layer(object, older);
    if (prepared_count >= 2)
        estimator.prepare_for_lower_layer(object, lower);
    const Vec3d start(1, 0.2, 0);
    const Vec3d end(39, 0.2, 0);
    for (int attempt = 0; attempt < 2; ++attempt) {
        CAPTURE(attempt);
        estimator.prepare_for_lower_layer(object, upper);
        CHECK_THAT(line_overhang_maximum(estimator, start, end, 0.4f, true), Catch::Matchers::WithinAbs(0.0, 1e-3));
    }
    // The outline handed over from the precomputed data is the one measured against.
    estimator.prepare_for_lower_layer(object, upper,
        std::make_shared<const AABBTreeLines::LinesDistancer<Linef3>>(to_unscaled_linesf3(older->lslices)));
    CHECK(line_overhang_maximum(estimator, start, end, 0.4f, true) > 100.0f);
}

// Orca: Metadata may add comments, but neither fixed-speed nor variable-speed export may change a
// printer command. This includes extrusion amounts, feed rates, cooling and acceleration commands.
TEST_CASE("Overhang metadata leaves printer commands unchanged", "[ExtrusionProcessor][Overhang][Regression]")
{
    const bool overhang_speed = GENERATE(false, true);
    const char *wall_generator = GENERATE("classic", "arachne");
    const std::string without_metadata = shallow_overhang_gcode(wall_generator, shallow_slowed_top_offset, overhang_speed, false);
    const std::string with_metadata = shallow_overhang_gcode(wall_generator, shallow_slowed_top_offset, overhang_speed, true);
    // Orca: Ensure the enabled case really exercises variable-speed output on this branch.
    const auto slope_speeds = shallow_face_feed_rates(without_metadata, true);
    REQUIRE_FALSE(slope_speeds.empty());
    CHECK(std::any_of(slope_speeds.begin(), slope_speeds.end(), [](double feed_rate) {
        return feed_rate / MM_PER_MIN < shallow_outer_wall_speed - 1.0;
    }) == overhang_speed);
    REQUIRE_FALSE(overhang_percentages(with_metadata).empty());
    REQUIRE(overhang_percentages(without_metadata).empty());
    CHECK(printer_commands(with_metadata) == printer_commands(without_metadata));
}

// Orca: Height modifiers can first enable slowdown or interrupt it for several layers, so the speed and fan
// estimate skips layers. In both directions, toggling preview metadata must preserve every printer command,
// including speeds: metadata prepares its own estimators for every layer.
TEST_CASE("Overhang metadata preserves printer commands across height modifier transitions", "[ExtrusionProcessor][Overhang][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool initial_slowdown = GENERATE(false, true);
    CAPTURE(wall_generator, initial_slowdown);
    std::vector<std::string> baseline_commands;
    for (bool enabled : {false, true}) {
        CAPTURE(enabled);
        DynamicPrintConfig config = shallow_overhang_config(wall_generator, initial_slowdown, enabled);
        // Orca: Disable overhang cooling so it cannot populate the cache while slowdown is disabled.
        config.set_deserialize_strict("enable_overhang_bridge_fan", "0");
        Print print;
        Model model;
        init_print({shallow_overhang_mesh(shallow_slowed_top_offset)}, print, model, config, nullptr, false);
        DynamicPrintConfig range_config;
        range_config.set_key_value("layer_height", new ConfigOptionFloat(shallow_layer_height));
        range_config.set_deserialize_strict("enable_overhang_speed", initial_slowdown ? "0" : "1");
        model.objects.front()->layer_config_ranges[{0.3, 0.7}].assign_config(std::move(range_config));
        print.apply(model, config);
        const std::string exported = gcode(print);
        CHECK(overhang_percentages(exported).empty() == !enabled);
        // Orca: The height modifier must exercise both slowed and normal-speed slope segments.
        const auto slope_speeds = shallow_face_feed_rates(exported, true);
        REQUIRE_FALSE(slope_speeds.empty());
        const auto extremes = std::minmax_element(slope_speeds.begin(), slope_speeds.end());
        CHECK(*extremes.first / MM_PER_MIN < shallow_outer_wall_speed - 1.0);
        CHECK_THAT(*extremes.second / MM_PER_MIN, Catch::Matchers::WithinAbs(shallow_outer_wall_speed, 0.1));
        const auto commands = printer_commands(exported);
        REQUIRE_FALSE(commands.empty());
        if (!enabled)
            baseline_commands = commands;
        else {
            CHECK(commands.size() == baseline_commands.size());
            CHECK(std::equal(commands.begin(), commands.end(), baseline_commands.begin(), baseline_commands.end()));
        }
    }
}

// Orca: A sheared cylinder has fitted circular walls with support changing along each layer.
// Verify the real exporter, timing and reopened preview without changing a single printer command.
TEST_CASE("Fitted arc overhang profiles round trip without changing printer commands", "[ExtrusionProcessor][Overhang][ArcFitting][Regression]")
{
    TriangleMesh mesh = make_cylinder(4.0, 1.2);
    for (auto &vertex : mesh.its.vertices)
        vertex.x() += 0.5f * vertex.z();
    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict({{"enable_arc_fitting", "1"}, {"enable_overhang_speed", "0"},
        {"enable_overhang_bridge_fan", "0"}, {"wall_loops", "1"}, {"sparse_infill_density", "0%"},
        {"top_shell_layers", "1"}, {"bottom_shell_layers", "1"}, {"gcode_comments", "0"}});
    // Orca: Import requires a complete nozzle enum, including its name map, and a known hardness.
    config.erase("nozzle_type");
    config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
    std::vector<std::string> baseline_commands;
    float baseline_time = 0.0f;
    for (bool enabled : {false, true}) {
        config.set_deserialize_strict("gcode_overhangs", enabled ? "1" : "0");
        Print print;
        Model model;
        init_print({mesh}, print, model, config, nullptr, false);
        // Orca: Exercise arc profiles across variable-height transitions as well as constant layers.
        DynamicPrintConfig range_config;
        range_config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
        model.objects.front()->layer_config_ranges[{0.4, 0.8}].assign_config(std::move(range_config));
        print.apply(model, config);
        print.set_status_silent();
        print.process();
        ScopedTemporaryFile file(".gcode");
        GCodeProcessorResult result;
        print.export_gcode(file.string(), &result);
        std::ifstream stream(file.string());
        const std::string exported((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto commands = printer_commands(exported);
        REQUIRE(std::any_of(commands.begin(), commands.end(), [](const std::string &command) {
            return command.find("G2 ") == 0 || command.find("G3 ") == 0;
        }));
        CHECK((exported.find("OVERHANG_ARC:") != std::string::npos) == enabled);
        CHECK((exported.find("OVERHANG_ARC_APPLY") != std::string::npos) == enabled);
        CHECK(result.has_overhang_metadata == enabled);
        const float time = result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        if (!enabled) {
            baseline_commands = commands;
            baseline_time = time;
            continue;
        }
        CHECK(commands == baseline_commands);
        CHECK_THAT(time, Catch::Matchers::WithinAbs(baseline_time, 1e-5));
        // Orca: Keep metadata lines short even when many samples are needed for a single arc.
        GCodeReader reader;
        size_t marker_count = 0;
        reader.parse_buffer(exported, [&marker_count](GCodeReader &, const GCodeReader::GCodeLine &line) {
            if (line.comment().find("OVERHANG_ARC:") != std::string_view::npos)
                CHECK(line.raw().size() < 160);
            if (line.comment().find("OVERHANG_ARC_APPLY") != std::string_view::npos) {
                ++marker_count;
                CHECK((line.cmd() == "G2") != (line.cmd() == "G3"));
            }
        });
        REQUIRE(marker_count > 0);
        GCodeProcessor imported;
        imported.process_file(file.string());
        REQUIRE(imported.get_result().has_overhang_metadata);
        std::vector<std::pair<float, float>> generated_metadata;
        std::vector<std::pair<float, float>> imported_metadata;
        bool saw_local_variation = false;
        const GCodeProcessorResult::MoveVertex *previous = nullptr;
        for (const auto &move : result.moves) {
            if (move.type != EMoveType::Extrude || move.internal_only)
                continue;
            generated_metadata.emplace_back(move.overhang_percentage, move.overhang_z_distance);
            // Orca: A single G-code arc must produce several distinct local preview readings.
            if (previous != nullptr && move.gcode_id == previous->gcode_id &&
                move.overhang_percentage != previous->overhang_percentage)
                saw_local_variation = true;
            previous = &move;
        }
        for (const auto &move : imported.get_result().moves)
            if (move.type == EMoveType::Extrude && !move.internal_only)
                imported_metadata.emplace_back(move.overhang_percentage, move.overhang_z_distance);
        REQUIRE(saw_local_variation);
        REQUIRE(imported_metadata.size() == generated_metadata.size());
        for (size_t i = 0; i < generated_metadata.size(); ++i) {
            CHECK_THAT(imported_metadata[i].first, Catch::Matchers::WithinAbs(generated_metadata[i].first, 1e-5));
            CHECK_THAT(imported_metadata[i].second, Catch::Matchers::WithinAbs(generated_metadata[i].second, 1e-5));
        }
    }
}

// Orca: Spiral-vase and scarf-joint walls deliberately bypass fitted G2/G3 output because their Z
// changes along the extrusion. Exercise both forced-linear paths with locally varying support, and
// verify that optional comments neither alter printer commands nor lose their line association.
TEST_CASE("Overhang metadata follows spiral and scarf linear extrusions", "[ExtrusionProcessor][Overhang][Regression]")
{
    const bool spiral_mode = GENERATE(false, true);
    // Bambu printers use tags starting with a space, which the spiral vase must not split from their ';' when it
    // rewrites a move's parameters.
    const bool bbl_printer = GENERATE(false, true);
    CAPTURE(spiral_mode, bbl_printer);
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });

    // Orca: Shearing a cylinder makes support vary around every wall while retaining geometry that
    // arc fitting would normally convert to G2/G3, so each mode must be the reason G1 is retained.
    TriangleMesh mesh = make_cylinder(4.0, 1.2);
    for (auto &vertex : mesh.its.vertices)
        vertex.x() += 0.5f * vertex.z();

    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict({
        {"enable_arc_fitting", "1"},
        {"enable_overhang_speed", "0"},
        {"enable_overhang_bridge_fan", "0"},
        {"wall_loops", "1"},
        {"sparse_infill_density", "0%"},
        {"top_shell_layers", "0"},
        {"bottom_shell_layers", "1"},
        {"gcode_comments", "0"},
        {"spiral_mode", spiral_mode ? "1" : "0"},
        // Smoothing and relative extrusion make the spiral vase rewrite X, Y and E of every move.
        {"spiral_mode_smooth", spiral_mode ? "1" : "0"},
        {"use_relative_e_distances", "1"},
        {"seam_slope_type", spiral_mode ? "none" : "external"},
        {"seam_slope_conditional", "0"},
        {"seam_slope_entire_loop", spiral_mode ? "0" : "1"},
        {"seam_slope_start_height", "50%"},
        {"seam_gap", "0"},
    });
    // Orca: Export validates nozzle hardness, so provide the complete nullable nozzle enum.
    config.erase("nozzle_type");
    config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});

    // Orca: Establish the executable output without metadata before exporting the instrumented case.
    config.set_deserialize_strict("gcode_overhangs", "0");
    Print baseline_print;
    Model baseline_model;
    init_print({mesh}, baseline_print, baseline_model, config, nullptr, false);
    baseline_print.is_BBL_printer() = bbl_printer;
    const std::string baseline = gcode(baseline_print);

    config.set_deserialize_strict("gcode_overhangs", "1");
    Print print;
    Model model;
    init_print({mesh}, print, model, config, nullptr, false);
    print.is_BBL_printer() = bbl_printer;
    print.set_status_silent();
    print.process();
    ScopedTemporaryFile file(".gcode");
    GCodeProcessorResult generated;
    print.export_gcode(file.string(), &generated);
    std::ifstream stream(file.string());
    const std::string exported((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    REQUIRE(generated.has_overhang_metadata);
    const auto commands = printer_commands(exported);
    CHECK(commands == printer_commands(baseline));
    for (const std::string &command : commands)
        CHECK(command.find("OVERHANG") == std::string::npos);
    if (!spiral_mode)
        REQUIRE(generated.print_statistics.total_seam_scarf_distance > 0.0f);

    // Orca: A relevant extrusion must carry both an explicit Z coordinate and a nonzero percentage.
    // Spiral mode uses this throughout each vase turn; scarf mode uses it along the sloped seam.
    float current_percentage = 0.0f;
    size_t linear_z_extrusions = 0;
    size_t profiled_linear_z_extrusions = 0;
    size_t arc_extrusions = 0;
    GCodeReader reader;
    reader.parse_buffer(exported, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        const size_t overhang = comment.find("OVERHANG:");
        if (overhang != std::string_view::npos) {
            const std::string value(comment.substr(overhang + sizeof("OVERHANG:") - 1));
            char *end = nullptr;
            const float parsed = std::strtof(value.c_str(), &end);
            if (end != value.c_str())
                current_percentage = parsed;
        }
        if ((line.cmd_is("G2") || line.cmd_is("G3")) && line.extruding(self))
            ++arc_extrusions;
        if (line.cmd_is("G1") && line.has_z() && line.extruding(self)) {
            ++linear_z_extrusions;
            if (current_percentage > 0.0f)
                ++profiled_linear_z_extrusions;
        }
    });
    REQUIRE(linear_z_extrusions > 0);
    REQUIRE(profiled_linear_z_extrusions > 0);
    if (spiral_mode)
        CHECK(arc_extrusions == 0);
}

// Orca: Identical cylindrical layers have no geometric overhang, including with fitted arcs.
// Thin layers expose tiny arc-versus-polygon errors as visible angles at the reported resolution.
TEST_CASE("Fitted cylindrical walls keep zero overhang on identical layers", "[ExtrusionProcessor][Overhang][ArcFitting][Regression]")
{
    const bool arc_fitting = GENERATE(false, true);
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(arc_fitting, wall_generator);
    DynamicPrintConfig config = shallow_overhang_config(wall_generator, false, true);
    config.set_deserialize_strict({{"enable_arc_fitting", arc_fitting ? "1" : "0"}, {"resolution", "0.012"},
        {"enable_overhang_bridge_fan", "0"}, {"wall_loops", "3"}});
    // Orca: Supply a complete nozzle definition for the exporter's hardware validation.
    config.erase("nozzle_type");
    config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
    Print print;
    Model model;
    init_print({make_cylinder(10.0, 0.12)}, print, model, config, nullptr, false);
    print.set_status_silent();
    print.process();
    ScopedTemporaryFile file(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(file.string(), &result);
    std::ifstream stream(file.string());
    const std::string exported((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto commands = printer_commands(exported);
    const bool has_arcs = std::any_of(commands.begin(), commands.end(), [](const std::string &command) {
        return command.find("G2 ") == 0 || command.find("G3 ") == 0;
    });
    REQUIRE(has_arcs == arc_fitting);
    float maximum_percentage = 0.0f;
    float maximum_angle = 0.0f;
    size_t checked = 0;
    for (const auto &move : result.moves) {
        if (move.type != EMoveType::Extrude || !is_perimeter(move.extrusion_role) || move.position.z() <= shallow_layer_height + EPSILON)
            continue;
        maximum_percentage = std::max(maximum_percentage, move.overhang_percentage);
        libvgcode::PathVertex vertex;
        vertex.height = move.height;
        vertex.width = move.width;
        vertex.overhang_percentage = move.overhang_percentage;
        vertex.overhang_z_distance = move.overhang_z_distance;
        maximum_angle = std::max(maximum_angle, vertex.overhang_degree());
        ++checked;
    }
    REQUIRE(checked > 0);
    CAPTURE(maximum_percentage, maximum_angle);
    CHECK_THAT(maximum_percentage, Catch::Matchers::WithinAbs(0.0, 0.05));
}

// Orca: Check exported and parsed metadata against the actual sliced object, including both directions
// of a thickness transition. Disabling metadata must leave every spacing unset and hide Overhang.
TEST_CASE("Overhang reference spacing follows variable layer heights", "[ExtrusionProcessor][Overhang][Regression]")
{
    const bool enabled = GENERATE(false, true);
    DynamicPrintConfig config = shallow_overhang_config("classic", false, enabled);
    // Orca: File import also validates nozzle hardness. Recreate the nullable enum from its definition
    // (static defaults lack its name map), and avoid relying on installed nozzle-hardness resources.
    config.erase("nozzle_type");
    config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
    Print print;
    Model model;
    init_print({shallow_overhang_mesh()}, print, model, config, nullptr, false);
    DynamicPrintConfig range_config;
    range_config.set_key_value("layer_height", new ConfigOptionFloat(0.08));
    model.objects.front()->layer_config_ranges[{0.3, 0.7}].assign_config(std::move(range_config));
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    ScopedTemporaryFile file(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(file.string(), &result);
    CHECK(result.has_overhang_metadata == enabled);
    // Orca: The opt-out must omit even zero-valued spacing comments, not merely leave parsed fields at zero.
    std::ifstream stream(file.string());
    const std::string exported((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK((exported.find("OVERHANG_Z_DISTANCE:") != std::string::npos) == enabled);

    // Orca: Reopening the final file takes the producer-aware parser path, unlike live preview.
    // Both paths must preserve the same per-extrusion metadata on adaptive layers.
    GCodeProcessor imported;
    imported.process_file(file.string());
    CHECK(imported.get_result().has_overhang_metadata == enabled);
    std::vector<std::pair<float, float>> generated_metadata;
    std::vector<std::pair<float, float>> imported_metadata;
    for (const auto &move : result.moves)
        if (move.type == EMoveType::Extrude && !move.internal_only)
            generated_metadata.emplace_back(move.overhang_percentage, move.overhang_z_distance);
    for (const auto &move : imported.get_result().moves)
        if (move.type == EMoveType::Extrude && !move.internal_only)
            imported_metadata.emplace_back(move.overhang_percentage, move.overhang_z_distance);
    REQUIRE(imported_metadata.size() == generated_metadata.size());
    for (size_t i = 0; i < generated_metadata.size(); ++i) {
        CHECK_THAT(imported_metadata[i].first, Catch::Matchers::WithinAbs(generated_metadata[i].first, 1e-5));
        CHECK_THAT(imported_metadata[i].second, Catch::Matchers::WithinAbs(generated_metadata[i].second, 1e-5));
    }

    bool saw_increase = false;
    bool saw_decrease = false;
    size_t checked_moves = 0;
    for (const auto &move : result.moves) {
        if (move.type != EMoveType::Extrude || !is_perimeter(move.extrusion_role))
            continue;
        const auto &layers = print.objects().front()->layers();
        const auto layer_it = std::find_if(layers.begin(), layers.end(), [&](const Layer *layer) {
            return std::abs(layer->print_z - move.position.z()) < 1e-4;
        });
        REQUIRE(layer_it != layers.end());
        const Layer *layer = *layer_it;
        const Layer *lower = layer->lower_layer;
        const double expected = enabled && lower != nullptr ? layer->slice_z - lower->slice_z : 0.0;
        CHECK_THAT(move.overhang_z_distance, Catch::Matchers::WithinAbs(expected, 1e-5));
        if (lower != nullptr) {
            saw_increase |= layer->height > lower->height + EPSILON;
            saw_decrease |= layer->height < lower->height - EPSILON;
        }
        ++checked_moves;
    }
    REQUIRE(checked_moves > 0);
    REQUIRE(saw_increase);
    REQUIRE(saw_decrease);
}

// Orca: A raft is support material rather than the preceding model contour, so the first object
// layer above it must not receive a geometric overhang estimate. Later sloped layers still use their
// immediate model layer and prove that metadata remains enabled after the raft boundary.
TEST_CASE("Object layers directly over a raft exclude overhang estimates", "[ExtrusionProcessor][Overhang][Regression]")
{
    DynamicPrintConfig config = shallow_overhang_config("classic", false, true);
    config.set_deserialize_strict({{"enable_support", "1"}, {"raft_layers", "1"},
        {"enable_overhang_bridge_fan", "0"}});
    // Orca: Supply the complete nozzle definition required while exporting the processed print.
    config.erase("nozzle_type");
    config.set_deserialize_strict({{"nozzle_type", "stainless_steel"}, {"nozzle_hrc", "20"}});
    Print print;
    Model model;
    init_print({shallow_overhang_mesh()}, print, model, config, nullptr, false);
    print.set_status_silent();
    print.process();

    const PrintObject *object = print.objects().front();
    REQUIRE(object->slicing_parameters().raft_layers() == 1);
    REQUIRE_FALSE(object->layers().empty());
    const Layer *layer_over_raft = object->layers().front();
    REQUIRE(layer_over_raft->id() == object->slicing_parameters().raft_layers());

    ScopedTemporaryFile file(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(file.string(), &result);
    REQUIRE(result.has_overhang_metadata);

    size_t over_raft_moves = 0;
    bool saw_later_overhang = false;
    for (const auto &move : result.moves) {
        if (move.type != EMoveType::Extrude || !is_perimeter(move.extrusion_role))
            continue;
        if (std::abs(move.position.z() - layer_over_raft->print_z) < 1e-4) {
            CHECK_THAT(move.overhang_percentage, Catch::Matchers::WithinAbs(0.0, 1e-5));
            CHECK_THAT(move.overhang_z_distance, Catch::Matchers::WithinAbs(0.0, 1e-5));
            ++over_raft_moves;
        } else if (move.position.z() > layer_over_raft->print_z + EPSILON && move.overhang_percentage > 0.1f) {
            saw_later_overhang = true;
        }
    }
    REQUIRE(over_raft_moves > 0);
    REQUIRE(saw_later_overhang);
}

// Classic reproduces the endpoint-sampling bug: it emits the span as one long move whose endpoints
// both read as supported, so endpoint-only sampling never slows it. Arachne's endpoints already read
// as overhanging, but their placement near the cage makes the inferred support vary by layer. Arachne
// parity is therefore part of this regression's scope: both generators must classify the unsupported
// interior of the same 45-degree span consistently.
TEST_CASE("Caged external overhangs are slowed along their span", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = caged_slope_feed_rates(caged_overhang_gcode(wall_generator));
    info_feed_rates("caged slope", feed_rates);

    REQUIRE_FALSE(feed_rates.empty());

    // The endpoint bug left Classic at the full wall speed, while Arachne's cage-adjacent endpoint
    // samples selected much faster bands on some layers. The whole span must stay in the slowed range
    // for both generators, without requiring their different path segmentations to match.
    const double fastest = *std::max_element(feed_rates.begin(), feed_rates.end());
    REQUIRE(fastest < caged_slow_speed * MM_PER_MIN);
}

// The other side of the fix: the midpoint probe fires on every long external perimeter, so a
// regression that over-slows would leave the test above green. A fully supported wall must keep the
// speed it was configured with.
TEST_CASE("Supported vertical walls keep their normal speed", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = back_wall_feed_rates(caged_overhang_gcode(wall_generator));
    info_feed_rates("back wall", feed_rates);

    REQUIRE_FALSE(feed_rates.empty());

    const double slowest = *std::min_element(feed_rates.begin(), feed_rates.end());
    REQUIRE(slowest >= caged_slow_speed * MM_PER_MIN);
}

// Orca: Cover both the default size-preserving path and metadata generation without speed slowdown.
TEST_CASE("Overhang preview metadata is optional and independent of overhang speed",
          "[ExtrusionProcessor][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    constexpr double ten_percent_top_offset = 0.1 * shallow_wall_width / shallow_layer_height;
    const std::vector<float> disabled_percentages = overhang_percentages(
        shallow_overhang_gcode(wall_generator, ten_percent_top_offset, false, false));
    const std::vector<float> percentages = overhang_percentages(
        shallow_overhang_gcode(wall_generator, ten_percent_top_offset, false, true));

    REQUIRE(disabled_percentages.empty());
    REQUIRE_FALSE(percentages.empty());
    REQUIRE(std::all_of(percentages.begin(), percentages.end(), [](float percentage) {
        return percentage >= 0.f && percentage <= GCodeProcessor::MAX_OVERHANG_PERCENTAGE;
    }));
    REQUIRE(std::any_of(percentages.begin(), percentages.end(), [](float percentage) { return std::abs(percentage - 10.f) <= 0.2f; }));
}

// The slope's top edge falls mid layer, so the first layer above it still stands 0.179mm proud of the layer
// below wherever that layer was still on the slope. That is a real overhang and is slowed, but it ends with the
// slope: outside the slope's x range the box runs full height, so the same wall stands on a contour identical to
// its own. Sampling the interior of that wall at a single point reported one support reading for all of it and
// slowed these fully supported ends along with the rest.
TEST_CASE("Wall sections beside a caged overhang keep their normal speed", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = cage_shoulder_feed_rates(caged_overhang_gcode(wall_generator));
    info_feed_rates("cage shoulder", feed_rates);

    REQUIRE_FALSE(feed_rates.empty());

    const double slowest = *std::min_element(feed_rates.begin(), feed_rates.end());
    REQUIRE_THAT(slowest / MM_PER_MIN, Catch::Matchers::WithinRel(caged_outer_wall_speed, 0.01));
}

// A wall is printed at the lower of the speeds its ends read, so a reading only earns a point in the
// path where it prints at a different speed from the readings around it. Judging that on the readings
// themselves rather than the speeds they produce was too coarse: the configured speeds interpolate
// between their sections, so readings a fraction of the slowdown threshold apart still print more than
// 10% apart, and a real 45 degree overhang had its true reading dropped as if it agreed with its ends.
// The ends then chose the speed on their own, and being next to the walls either side of the overhang
// they read differently from layer to layer, banding an overhang that should have been uniform.
TEST_CASE("An overhang reading is kept whenever it changes the speed", "[ExtrusionProcessor][Regression]")
{
    // A steep speed curve, of the kind the configured overhang speeds interpolate across.
    const std::vector<ExtendedPoint<2>> points =
        sampled_wall_over_dished_layer([](float distance) { return std::round(200.f - 400.f * distance); });

    REQUIRE_THAT(furthest_reading(points), Catch::Matchers::WithinAbs(dished_mid_reading, dished_reading_tolerance));
}

// The complement, and why the readings alone were tempting: a reading that prints at the same speed as
// its neighbours cannot change the G-code, so sampling must leave the path alone however far out it is.
TEST_CASE("An overhang reading is dropped when the speed is unchanged", "[ExtrusionProcessor]")
{
    // A flat speed curve, of the kind a single configured overhang speed produces.
    const std::vector<ExtendedPoint<2>> points = sampled_wall_over_dished_layer([](float) { return 50.f; });

    REQUIRE_THAT(furthest_reading(points), Catch::Matchers::WithinAbs(dished_end_reading, dished_reading_tolerance));
}

TEST_CASE("Coarse probing detects an unsupported pocket away from the wall midpoint",
          "[ExtrusionProcessor][Regression]")
{
    const std::function<float(float)> distance_to_speed = [](float distance) { return distance <= 0.2f ? 100.f : 50.f; };
    const std::vector<ExtendedPoint<2>> points =
        sampled_wall_over_narrow_pocket(0.25 * narrow_pocket_wall_length, distance_to_speed);
    const double slowed = slowed_length(points, distance_to_speed);

    REQUIRE(slowed > 0.);
    REQUIRE(slowed < 5.);
}

TEST_CASE("Coarse probing brackets a narrow slowdown at the wall midpoint",
          "[ExtrusionProcessor][Regression]")
{
    // Half of the pocket reading still maps to full speed. A matching probe in either half therefore must not
    // prune that half before a supported point has been found close enough to bracket the slow midpoint.
    const std::function<float(float)> distance_to_speed = [](float distance) { return distance <= 0.2f ? 100.f : 50.f; };
    const std::vector<ExtendedPoint<2>> points =
        sampled_wall_over_narrow_pocket(0.5 * narrow_pocket_wall_length, distance_to_speed);
    const double slowed = slowed_length(points, distance_to_speed);

    REQUIRE(slowed > 0.);
    REQUIRE(slowed < 5.);
}

// Sampling probes the interior, so it must not answer for the ends. On a supported wall between two
// corners that read an overhang, the reading that differs is the end's own, and the pass that ends a
// slowdown an end reads places its point from how far out that end is. Sampling took the difference as
// its own to report and put a point at the nearest position bisection had reached instead, which both
// sits further along the wall and leaves too little of it for that pass to run on, so the corner
// slowdown ran millimetres up an otherwise supported wall. Its length grows with the wall, so on a
// model whose cross section keeps growing it reads as a stair stepped band up the corner.
TEST_CASE("A supported wall between overhanging corners is slowed no further than its ends require",
          "[ExtrusionProcessor][Regression]")
{
    // A steep speed curve, so the ends and the interior between them print at clearly different speeds.
    const std::function<float(float)> distance_to_speed = [](float distance) {
        return std::round(float(caged_outer_wall_speed) - 400.f * distance);
    };

    const double sampled   = slowed_length(sampled_wall_between_growing_corners(distance_to_speed), distance_to_speed);
    // The same wall with sampling switched off: what the endpoint driven passes alone make of the corners.
    const double unsampled = slowed_length(sampled_wall_between_growing_corners({}), distance_to_speed);

    // The corners do read an overhang, so there is a slowdown for sampling to have lengthened.
    REQUIRE(unsampled > 0.);
    REQUIRE(sampled <= unsampled);
}

// Regression: the line up to a step past the previous layer was not split, so the step's slowdown and cooling covered the
// whole wall. The split required an end reading beyond where the slowdown begins, and an edge crossing reads exactly
// there when the wall speed is held below the reference speed (e.g. resonance avoidance).
TEST_CASE("A wall stepping past the previous layer is slowed and cooled only beside the step", "[ExtrusionProcessor][Regression]")
{
    const float crossing_reading = 0.5f * float(caged_wall_width);
    const std::function<float(float)> distance_to_speed = [crossing_reading](float distance) {
        return distance < crossing_reading ? 70.f : 15.f;
    };
    const float fan_overlap_threshold = 0.75f; // The fan switches on at a 25% overhang

    const std::vector<ExtendedPoint<2>> points = sampled_wall_along_edge(-edge_wall_end_past, distance_to_speed, crossing_reading,
                                                                         fan_overlap_threshold);
    const double slowed = slowed_length(points, distance_to_speed);
    const double cooled = cooled_length(points, fan_overlap_threshold);

    REQUIRE(slowed > 0.);
    REQUIRE(cooled > 0.);
    REQUIRE(slowed < edge_affected_length);
    REQUIRE(cooled < edge_affected_length);
}

// Regression: the fan can switch on at a smaller overhang than the first slowdown. Splitting only on speed changes left
// the whole wall cooled when its end read between the two.
TEST_CASE("A wall is split where only the overhang fan changes", "[ExtrusionProcessor][Regression]")
{
    const float crossing_reading = 0.5f * float(caged_wall_width);
    const std::function<float(float)> distance_to_speed = [crossing_reading](float distance) {
        return distance < crossing_reading ? 70.f : 15.f;
    };
    // The end reads 0.16mm out (overlap 0.62): cooled at a 25% threshold, but not slowed.
    const float fan_overlap_threshold = 0.75f;

    const std::vector<ExtendedPoint<2>> points = sampled_wall_along_edge(edge_wall_end_short, distance_to_speed, crossing_reading,
                                                                         fan_overlap_threshold);
    const double cooled = cooled_length(points, fan_overlap_threshold);

    REQUIRE_THAT(slowed_length(points, distance_to_speed), Catch::Matchers::WithinAbs(0., 1e-9));
    REQUIRE(cooled > 0.);
    REQUIRE(cooled < edge_affected_length);
}

// With one speed and a fan threshold no reading reaches, only the wall's ends and the edge crossing remain.
TEST_CASE("A wall is left whole where neither its speed nor its cooling changes", "[ExtrusionProcessor]")
{
    const std::function<float(float)> distance_to_speed = [](float) { return 70.f; };
    // 95% overhang; the step reads 0.384mm out (overlap 0.09).
    const float fan_overlap_threshold = 0.05f;

    const std::vector<ExtendedPoint<2>> points = sampled_wall_along_edge(-edge_wall_end_past, distance_to_speed, -1.f,
                                                                         fan_overlap_threshold);

    REQUIRE(points.size() == 3);
}

namespace {

// The caged overhang box, sliced, and a layer on its slope.
struct SlicedCage
{
    Print              print;
    Model              model;
    const PrintObject *object{nullptr};
    const Layer       *layer{nullptr};

    explicit SlicedCage(const DynamicPrintConfig &config = caged_overhang_config("classic"))
    {
        init_print(std::vector<TriangleMesh>{caged_overhang_mesh()}, print, model, config, nullptr, false);
        print.process();
        object = print.objects().front();
        layer  = object->get_layer(int(std::lround((caged_slope_z_min + caged_slope_z_max) / 2. / caged_layer_height)));
    }
};

using Walls = std::vector<std::vector<ProcessedPoint>>;

bool has_curled_lines(const PrintObject &object)
{
    const auto layers = object.layers();
    return std::any_of(layers.begin(), layers.end(), [](const Layer *layer) { return !layer->curled_lines.empty(); });
}

// Estimates every wall of `layer` against the layer below the one `estimator` was last prepared for.
Walls estimate_walls(ExtrusionQualityEstimator &estimator, const PrintObject *object, const Layer &layer)
{
    const ConfigOptionPercents         overlaps({90, 75, 50, 25, 13, 0});
    const ConfigOptionFloatsOrPercents speeds({FloatOrPercent{100, true}, FloatOrPercent{50, true}, FloatOrPercent{30, true},
                                               FloatOrPercent{20, true}, FloatOrPercent{10, true}, FloatOrPercent{5, true}});
    Walls walls;
    estimator.set_current_object(object);
    for (const LayerRegion *region : layer.regions())
        for_each_extrusion_path(region->perimeters, [&](const ExtrusionPath &path) {
            if (is_perimeter(path.role()))
                walls.push_back(estimator.estimate_extrusion_quality(path, overlaps, speeds, caged_outer_wall_speed, caged_outer_wall_speed,
                                                                     true, 0.5f));
        });
    return walls;
}

uint32_t float_bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool same_point(const ProcessedPoint &a, const ProcessedPoint &b)
{
    return a.p == b.p && float_bits(a.speed) == float_bits(b.speed) && float_bits(a.overlap) == float_bits(b.overlap);
}

// Requires the walls to match point for point, bit for bit.
void check_identical(const Walls &actual, const Walls &expected)
{
    REQUIRE(actual.size() == expected.size());
    for (size_t wall = 0; wall < actual.size(); ++wall) {
        INFO("wall " << wall);
        REQUIRE(actual[wall].size() == expected[wall].size());
        for (size_t i = 0; i < actual[wall].size(); ++i) {
            const ProcessedPoint &a = actual[wall][i];
            const ProcessedPoint &e = expected[wall][i];
            INFO("point " << i << ": speed " << a.speed << " vs " << e.speed << ", overlap " << a.overlap << " vs " << e.overlap);
            CHECK(a.p == e.p);
            CHECK(float_bits(a.speed) == float_bits(e.speed));
            CHECK(float_bits(a.overlap) == float_bits(e.overlap));
        }
    }
}

bool any_difference(const Walls &a, const Walls &b)
{
    return !std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const std::vector<ProcessedPoint> &wa, const std::vector<ProcessedPoint> &wb) {
        return std::equal(wa.begin(), wa.end(), wb.begin(), wb.end(), same_point);
    });
}

bool any_slowed(const Walls &walls)
{
    return std::any_of(walls.begin(), walls.end(), [](const std::vector<ProcessedPoint> &wall) {
        return std::any_of(wall.begin(), wall.end(), [](const ProcessedPoint &point) { return point.speed < caged_outer_wall_speed; });
    });
}

} // namespace

TEST_CASE("Overhang data computed ahead of the generator gives the same wall speeds", "[ExtrusionProcessor]")
{
    const SlicedCage cage;
    REQUIRE(cage.layer->lower_layer != nullptr);

    ExtrusionQualityEstimator queried;
    queried.prepare_for_new_layer(cage.object, cage.layer->lower_layer);
    queried.prepare_for_new_layer(cage.object, cage.layer);
    const Walls expected = estimate_walls(queried, cage.object, *cage.layer);
    REQUIRE(any_slowed(expected));

    ExtrusionQualityEstimator precomputed;
    precomputed.prepare_for_new_layer(cage.object, cage.layer->lower_layer);
    precomputed.set_precomputed_layers({precompute_overhang_layer(cage.object, *cage.layer)});
    precomputed.prepare_for_new_layer(cage.object, cage.layer);
    check_identical(estimate_walls(precomputed, cage.object, *cage.layer), expected);
}

// process_layer() prepares the estimator only for the layers that need overhang detection, so the layer it was prepared
// for before may be a lower one than the layer below, the same layer again, or none at all. Whether the distances are
// queried or computed ahead, the walls must come out as if the estimator had been prepared for every layer.
TEST_CASE("Walls are measured against the layer below them whichever layer was prepared before", "[ExtrusionProcessor][Regression]")
{
    // Nothing, the layer two below, or the same layer.
    const int  prepared_before = GENERATE(0, 1, 2);
    const bool precomputed     = GENERATE(false, true);
    CAPTURE(prepared_before, precomputed);
    const SlicedCage cage;
    const Layer     *one_below = cage.layer->lower_layer;
    REQUIRE(one_below != nullptr);
    const Layer     *two_below = one_below->lower_layer;
    REQUIRE(two_below != nullptr);

    ExtrusionQualityEstimator continuous;
    continuous.prepare_for_new_layer(cage.object, one_below);
    continuous.prepare_for_new_layer(cage.object, cage.layer);
    const Walls expected = estimate_walls(continuous, cage.object, *cage.layer);
    // Measured against the layer two below, the walls differ, so matching `expected` below is no coincidence.
    ExtrusionQualityEstimator stale;
    stale.prepare_for_new_layer(cage.object, one_below);
    REQUIRE(any_difference(estimate_walls(stale, cage.object, *cage.layer), expected));

    const Layer *const prepared_layers[] = {nullptr, two_below, cage.layer};
    ExtrusionQualityEstimator estimator;
    if (prepared_layers[prepared_before] != nullptr)
        estimator.prepare_for_new_layer(cage.object, prepared_layers[prepared_before]);
    if (precomputed)
        estimator.set_precomputed_layers({precompute_overhang_layer(cage.object, *cage.layer)});
    estimator.prepare_for_new_layer(cage.object, cage.layer);
    check_identical(estimate_walls(estimator, cage.object, *cage.layer), expected);
}

TEST_CASE("Overhang data computed for another layer is not used", "[ExtrusionProcessor]")
{
    const SlicedCage cage;
    const Layer     *one_below = cage.layer->lower_layer;
    REQUIRE(one_below != nullptr);
    REQUIRE(one_below->lower_layer != nullptr);

    ExtrusionQualityEstimator queried;
    queried.prepare_for_new_layer(cage.object, one_below);
    queried.prepare_for_new_layer(cage.object, cage.layer);
    const Walls expected = estimate_walls(queried, cage.object, *cage.layer);
    // Measures the walls against the layer below one_below, the layer the data computed for one_below holds.
    ExtrusionQualityEstimator two_below;
    two_below.prepare_for_new_layer(cage.object, one_below);
    REQUIRE(any_difference(estimate_walls(two_below, cage.object, *cage.layer), expected));

    ExtrusionQualityEstimator precomputed;
    precomputed.set_precomputed_layers({precompute_overhang_layer(cage.object, *one_below)});
    precomputed.prepare_for_new_layer(cage.object, one_below);
    precomputed.prepare_for_new_layer(cage.object, cage.layer);
    check_identical(estimate_walls(precomputed, cage.object, *cage.layer), expected);
}

// The curled edges that slow a wall down come from the layer below it too, not from a layer prepared before.
TEST_CASE("Walls are slowed for the curled edges of the layer below them whichever layer was prepared before", "[ExtrusionProcessor][Regression]")
{
    // Nothing, a stale layer, or the stale layer and then the layer below.
    const int prepared_before = GENERATE(0, 1, 2);
    CAPTURE(prepared_before);
    Print print;
    Model model;
    init_print({cube(20.)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *stale = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *below = object->add_layer(1, 0.2, 0.4, 0.3);
    Layer *layer = object->add_layer(2, 0.2, 0.6, 0.5);
    below->lower_layer = stale;
    layer->lower_layer = below;
    // The 0.4mm wide wall runs 0.2mm inside the edge of the layer below, so it is fully supported, but along an edge that
    // curled up 1mm. The stale layer ends 0.8mm short of the wall and has no curled edges: measured against it, the wall
    // would be a full overhang.
    below->lslices      = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
                                             Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    below->curled_lines = {CurledLine(Point::new_scale(1, 0.2), Point::new_scale(39, 0.2), 1.f)};
    stale->lslices      = {ExPolygon(Polygon{Point::new_scale(0, 1), Point::new_scale(40, 1),
                                             Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    ExtrusionPath wall(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    wall.polyline.points = {Point3::new_scale(1, 0.2, 0), Point3::new_scale(39, 0.2, 0)};

    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    if (prepared_before >= 1)
        estimator.prepare_for_new_layer(object, stale);
    if (prepared_before >= 2)
        estimator.prepare_for_new_layer(object, below);
    estimator.prepare_for_new_layer(object, layer);
    // 100mm/s when fully supported, 20mm/s when a whole width out.
    const std::vector<ProcessedPoint> points = estimator.estimate_extrusion_quality(wall, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}), 100.f, 100.f, true);

    // A 1mm curl under a 0.2mm layer reads as half the width, 0.2mm, out: halfway between the two speeds.
    REQUIRE_FALSE(points.empty());
    for (const ProcessedPoint &point : points) {
        CHECK_THAT(point.speed, Catch::Matchers::WithinAbs(60., 0.5));
        CHECK_THAT(point.overlap, Catch::Matchers::WithinAbs(0.5, 1e-3));
    }
}

TEST_CASE("Precomputed overhang data has the curled-line tree exactly when a region slows down for curled perimeters", "[ExtrusionProcessor]")
{
    const bool         slowdown = GENERATE(false, true);
    DynamicPrintConfig config   = caged_overhang_config("classic");
    config.set_deserialize_strict("slowdown_for_curled_perimeters", slowdown ? "1" : "0");
    const SlicedCage cage(config);

    GCode::LayerToPrint layer;
    layer.object_layer    = cage.layer;
    layer.original_object = cage.object;
    const std::vector<PrecomputedOverhangLayer> precomputed = precompute_overhang_layers({layer}, false);
    REQUIRE(precomputed.size() == 1);
    CHECK((precomputed.front().lower_curled_lines != nullptr) == slowdown);
    // Orca: The infill footprint is built only for overhang metadata.
    CHECK_FALSE(precomputed.front().lower_infill_footprint);
    const std::vector<PrecomputedOverhangLayer> with_metadata = precompute_overhang_layers({layer}, false, true);
    REQUIRE(with_metadata.size() == 1);
    CHECK(with_metadata.front().lower_infill_footprint);
}

TEST_CASE("Curled walls are estimated only when overhang speed and the slowdown for curled perimeters are both on", "[ExtrusionProcessor]")
{
    const auto [overhang_speed, slowdown, estimated] = GENERATE(table<bool, bool, bool>({
        {true, true, true},
        {true, false, false},
        {false, true, false},
    }));
    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict({{"enable_overhang_speed", overhang_speed ? "1" : "0"},
                                   {"slowdown_for_curled_perimeters", slowdown ? "1" : "0"}});
    const SlicedCage cage(config);

    CHECK(has_curled_lines(*cage.object) == estimated);
}

TEST_CASE("Curled walls are estimated when overhang speed and the slowdown for curled perimeters are on in different objects", "[ExtrusionProcessor]")
{
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"enable_overhang_speed", "1"}, {"slowdown_for_curled_perimeters", "0"}},
        {{"enable_overhang_speed", "0"}, {"slowdown_for_curled_perimeters", "1"}},
    };
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{caged_overhang_mesh(), caged_overhang_mesh()}, print, model, caged_overhang_config("classic"),
               &overrides);
    print.process();

    REQUIRE(print.objects().size() == 2);
    for (const PrintObject *object : print.objects())
        CHECK(has_curled_lines(*object));
}

TEST_CASE("Curled walls from an earlier slice are dropped once overhang speed is off", "[ExtrusionProcessor]")
{
    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict("slowdown_for_curled_perimeters", "1");
    SlicedCage cage(config);
    const Layer *first_layer = cage.print.objects().front()->layers().front();
    REQUIRE(has_curled_lines(*cage.print.objects().front()));

    config.set_deserialize_strict("enable_overhang_speed", "0");
    cage.print.apply(cage.model, config);
    cage.print.process();
    REQUIRE(cage.print.objects().front()->layers().front() == first_layer);
    CHECK_FALSE(has_curled_lines(*cage.print.objects().front()));
}

TEST_CASE("Caged external overhangs are slowed when printed by object or through the pressure equalizer", "[ExtrusionProcessor]")
{
    const auto [key, value] = GENERATE(table<const char *, const char *>({
        {"print_sequence", "by object"},
        {"max_volumetric_extrusion_rate_slope", "10"},
    }));
    INFO(key << " = " << value);
    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict(key, value);
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{caged_overhang_mesh()}, print, model, config, nullptr, false);

    const std::vector<double> feed_rates = caged_slope_feed_rates(gcode(print));
    info_feed_rates("caged slope", feed_rates);
    REQUIRE_FALSE(feed_rates.empty());
    REQUIRE(*std::max_element(feed_rates.begin(), feed_rates.end()) < caged_slow_speed * MM_PER_MIN);
}

namespace {

// A 20 x 10 x 2mm wedge whose front face leans out by one layer height per layer (45 degrees), from y = 2 at the bed
// to y = 0 at the top, while its back face at y = 12 stands vertical and fully supported.
constexpr double wedge_height       = 2.;
constexpr double wedge_base_front_y = 2.;
constexpr double wedge_back_y       = 12.;

TriangleMesh wedge_mesh()
{
    const float length = 20.f, top = float(wedge_height), front = float(wedge_base_front_y), back = float(wedge_back_y);
    return TriangleMesh(
        {
            {0.f, front, 0.f}, {length, front, 0.f}, {length, back, 0.f}, {0.f, back, 0.f},
            {0.f, 0.f, top},   {length, 0.f, top},   {length, back, top},   {0.f, back, top},
        },
        {
            {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
            {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
            {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
        });
}

// A height range modifier over these heights. Layers are sliced half a layer below their print_z, at 0.1, 0.3, 0.5 and
// so on, so the bounds fall between slice planes: the layers printed at 0.6 and 0.8 take the range's settings.
constexpr double wedge_range_min = 0.4;
constexpr double wedge_range_max = 0.8;

bool in_wedge_range(double print_z)
{
    const double slice_z = print_z - 0.5 * caged_layer_height;
    return slice_z > wedge_range_min && slice_z < wedge_range_max;
}

// The wedge with overhang speed set to `object_speed`, and to `range_speed` by the height range modifier.
std::string wedge_gcode(bool object_speed, bool range_speed)
{
    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict({
        {"enable_overhang_speed", object_speed ? "1" : "0"},
        // The overhang fan has every layer prepared for overhang detection, whether overhang speed is on or not.
        {"enable_overhang_bridge_fan", "0"},
    });
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{wedge_mesh()}, print, model, config, nullptr, false);
    DynamicPrintConfig range_config;
    // Every layer range must carry a layer_height (see layer_height_profile_from_ranges).
    range_config.set_key_value("layer_height", new ConfigOptionFloat(caged_layer_height));
    range_config.set_deserialize_strict("enable_overhang_speed", range_speed ? "1" : "0");
    model.objects.front()->layer_config_ranges[{wedge_range_min, wedge_range_max}].assign_config(std::move(range_config));
    print.apply(model, config);
    return gcode(print);
}

// Feed rates of the outer wall moves printed at print_z along the wedge's sloped front face, or along its back face.
// The side walls run from one face to the other, so neither filter takes them.
std::vector<double> wedge_wall_feed_rates(const std::string &gcode, double print_z, bool front)
{
    return outer_wall_feed_rates(gcode, [print_z, front](const GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const auto on_face = [front](double y) { return front ? y < wedge_base_front_y + 1. : y > wedge_back_y - 1.; };
        return std::abs(line.new_Z(self) - print_z) < 0.5 * caged_layer_height && on_face(self.y()) && on_face(line.new_Y(self));
    });
}

} // namespace

// Regression: the estimator measured each layer against the layer it was prepared for before. process_layer() skips the
// layers with overhang speed off, so the first layer above a range turning it off was measured against the layer below
// the range, and the first layer of a range turning it on against no layer at all, which slowed even supported walls.
TEST_CASE("Walls printed with overhang speed on match a print with it always on when a height range toggles it", "[ExtrusionProcessor][Regression]")
{
    const bool object_speed = GENERATE(true, false);
    INFO("overhang speed " << (object_speed ? "on" : "off") << " for the object, " << (object_speed ? "off" : "on")
                           << " for the range");
    const std::string toggled   = wedge_gcode(object_speed, !object_speed);
    const std::string always_on = wedge_gcode(true, true);

    // The first layers printed with the other setting, at either end of the range.
    for (const double print_z : {wedge_range_min + caged_layer_height, wedge_range_max + caged_layer_height}) {
        INFO("back wall at z = " << print_z);
        const std::vector<double> feed_rates = wedge_wall_feed_rates(toggled, print_z, false);
        REQUIRE_FALSE(feed_rates.empty());
        for (const double feed_rate : feed_rates)
            CHECK_THAT(feed_rate / MM_PER_MIN, Catch::Matchers::WithinRel(caged_outer_wall_speed, 0.01));
    }

    // Every layer printed with overhang speed on, above the first, which is never slowed for overhangs.
    const int layer_count = int(std::lround(wedge_height / caged_layer_height));
    for (int layer = 2; layer <= layer_count; ++layer) {
        const double print_z = layer * caged_layer_height;
        if (in_wedge_range(print_z) == object_speed)
            continue;
        INFO("slope at z = " << print_z);
        const std::vector<double> expected = wedge_wall_feed_rates(always_on, print_z, true);
        REQUIRE_FALSE(expected.empty());
        // The slope is slowed down, so matching speeds are not merely the wall speed.
        REQUIRE(*std::max_element(expected.begin(), expected.end()) < caged_slow_speed * MM_PER_MIN);
        const std::vector<double> actual = wedge_wall_feed_rates(toggled, print_z, true);
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i)
            CHECK_THAT(actual[i], Catch::Matchers::WithinAbs(expected[i], 1.));
    }
}

TEST_CASE("Benchmark caged overhang interior sampling", "[ExtrusionProcessor][!benchmark]"){
    const char* wall_generator = GENERATE("classic", "arachne");

    BENCHMARK(wall_generator)
    {
        return caged_overhang_gcode(wall_generator);
    };
}


// Orca: An expanding neighboring wall is closer to points near the corner of this unchanged wall.
// Associate each reading with the current wall first so the perpendicular edge cannot create a false
// overhang anywhere on the supported span, including its first and last width-scale cells.
TEST_CASE("Overhang metadata does not spread a corner overhang along a supported wall",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    constexpr double outward_shift = 0.1;

    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    lower->lslices = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    upper->lslices = {ExPolygon(Polygon{Point::new_scale(-outward_shift, 0), Point::new_scale(40 + outward_shift, 0),
        Point::new_scale(40 + outward_shift, 10), Point::new_scale(-outward_shift, 10)})};

    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    ExtrusionPath path(erExternalPerimeter, 0.08, width, 0.2f);
    path.polyline.points = {Point3::new_scale(-outward_shift, 0.2, 0),
                            Point3::new_scale(40 + outward_shift, 0.2, 0)};

    for (float percentage : line_overhang_profile(estimator, unscaled(path.polyline.points.front()),
             unscaled(path.polyline.points.back()), width, true))
        CHECK_THAT(percentage, Catch::Matchers::WithinAbs(0.0, 1e-3));

    const auto variable_speed_points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false);
    REQUIRE_FALSE(variable_speed_points.empty());
    for (size_t i = 0; i + 1 < variable_speed_points.size(); ++i)
        CHECK_THAT(line_overhang_maximum(estimator, unscaled(variable_speed_points[i].p),
            unscaled(variable_speed_points[i + 1].p), width, true), Catch::Matchers::WithinAbs(0.0, 1e-3));
}

// Orca: Closing a hole can put a perimeter over air despite an unchanged outer contour. Test both
// sides of the support edge: the centerline may be inside or outside the lower layer's material.
TEST_CASE("Perimeter overhang metadata preserves unsupported width above a nearby hole",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    const double supported_width = GENERATE(0.1, 0.3);
    const ExtrusionRole role = GENERATE(erExternalPerimeter, erOverhangPerimeter);
    CAPTURE(supported_width, role);
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    ExPolygon lower_slice(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 20), Point::new_scale(0, 20)});
    upper->lslices = {lower_slice};
    lower_slice.holes.emplace_back(Polygon{Point::new_scale(1, supported_width), Point::new_scale(1, 10),
        Point::new_scale(39, 10), Point::new_scale(39, supported_width)});
    lower_slice.holes.back().make_clockwise();
    lower->lslices = {std::move(lower_slice)};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_lower_layer(object, upper);
    ExtrusionPath path(role, 0.08, width, 0.2f);
    path.polyline.points = {Point3::new_scale(2, 0.5 * width, 0), Point3::new_scale(38, 0.5 * width, 0)};
    const double expected_percentage = 100.0 * (width - supported_width) / width;
    CHECK_THAT(line_overhang_maximum(estimator, unscaled(path.polyline.points.front()),
        unscaled(path.polyline.points.back()), width, true), Catch::Matchers::WithinAbs(expected_percentage, 1e-3));
    // Orca: Variable-speed output uses the same geometric estimate for each emitted span.
    const auto points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false);
    REQUIRE(points.size() >= 2);
    for (size_t i = 0; i + 1 < points.size(); ++i)
        CHECK_THAT(line_overhang_maximum(estimator, unscaled(points[i].p), unscaled(points[i + 1].p), width, true),
            Catch::Matchers::WithinAbs(expected_percentage, 1e-3));
}
