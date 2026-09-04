#include <catch2/catch_all.hpp>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
// Orca: Exercise the inline preview conversion with metadata produced by the real G-code pipeline.
#include "libvgcode/include/PathVertex.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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
        if (!line.cmd().empty())
            result.push_back(line.raw().substr(0, line.raw().find(';')));
    });
    return result;
}

// Orca: Attach the same standalone inline marker emitted by GCode.cpp without coupling tests to
// comment spacing. Callers may retain an existing description before the added semicolon-delimited field.
std::string bind_overhang_arc_profile(std::string arc)
{
    assert(!arc.empty() && arc.back() == '\n');
    arc.insert(arc.size() - 1, ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Arc_Apply));
    return arc;
}

// Orca: Tests that select a tag dialect must restore the process-wide setting so randomized test
// ordering cannot make another G-code parser test depend on which case ran immediately before it.
class ScopedBblPrinterFlag
{
public:
    explicit ScopedBblPrinterFlag(bool value) : m_previous(GCodeProcessor::s_IsBBLPrinter)
    {
        GCodeProcessor::s_IsBBLPrinter = value;
    }

    ~ScopedBblPrinterFlag() { GCodeProcessor::s_IsBBLPrinter = m_previous; }

private:
    bool m_previous;
};

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
        const double expected = 100.0 * std::clamp((10.0 * std::cos(angle) - 7.0 + 0.2) / 0.4, 0.0, 1.0);
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
    // unsupported outer-wall width. The deeper inner wall stays supported until the 1 mm shift.
    const double expected = inner_wall ? (shift >= 1.0 ? 100.0 : 0.0) :
        100.0 * std::clamp(shift * std::cos(PI / facets) / width, 0.0, 1.0);
    CHECK_THAT(samples[1], Catch::Matchers::WithinAbs(expected, 1e-3));
}

// Orca: Both firmware tessellations retain their original move count and timing inputs. A monotone
// profile has an exact local maximum at one segment endpoint, independent of its geometric direction.
TEST_CASE("Overhang arc profiles color local segments without changing motion", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    const auto flavor = GENERATE(gcfMarlinFirmware, gcfKlipper);
    const bool clockwise = GENERATE(false, true);
    const bool decreasing = GENERATE(false, true);
    FullPrintConfig config;
    config.gcode_flavor.value = flavor;
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const std::string setup = "G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n" +
        tag(GCodeProcessor::ETags::Height, "0.2") + tag(GCodeProcessor::ETags::Width, "0.4") +
        tag(GCodeProcessor::ETags::Overhang_Z_Distance, "0.2") + tag(GCodeProcessor::ETags::Overhang, "100");
    const std::string arc = std::string(clockwise ? "G2" : "G3") + " X0 Y10 I-10 J0 E1 F600 ; arc description\n";
    const std::string marked_arc = bind_overhang_arc_profile(arc);
    // Orca: Arbitrary non-motion commands may appear between chunks and the explicitly marked arc;
    // they require no whitelist. A lower-case Klipper command also exercises case-insensitive handling.
    const std::string control = "M106 S128\nM73 P50\nM107\nG4 P1\nM104 S200\nM204 S1000\nM400\n;VM104 S200\n" +
        std::string(flavor == gcfKlipper ?
            "set_velocity_limit SQUARE_CORNER_VELOCITY=5\nSET_PRESSURE_ADVANCE ADVANCE=0.04\nRESPOND TYPE=echo MSG=preview\n" : "");
    GCodeProcessor baseline;
    baseline.apply_config(config);
    baseline.process_buffer(setup + control + arc);
    GCodeProcessor profiled;
    profiled.apply_config(config);
    profiled.process_buffer(setup);
    // Orca: Chunk boundaries may coincide with streaming-buffer boundaries.
    profiled.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, decreasing ? "5,0,100,75,50" : "5,0,0,25,50"));
    profiled.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, decreasing ? "5,3,25,0" : "5,3,75,100") + control + marked_arc);
    const auto &original = baseline.get_result().moves;
    const auto &moves = profiled.get_result().moves;
    REQUIRE(moves.size() == original.size());
    // Orca: Timing may insert extra speed markers; profile progress follows the original arc segments.
    const size_t count = std::count_if(moves.begin(), moves.end(), [](const auto &move) {
        return move.type == EMoveType::Extrude && !move.internal_only;
    });
    REQUIRE(count > 2);
    size_t segment = 0;
    for (size_t i = 0; i < moves.size(); ++i) {
        CHECK_THAT((moves[i].position - original[i].position).norm(), Catch::Matchers::WithinAbs(0.0, 1e-6));
        CHECK_THAT(moves[i].delta_extruder, Catch::Matchers::WithinAbs(original[i].delta_extruder, 1e-6));
        CHECK_THAT(moves[i].feedrate, Catch::Matchers::WithinAbs(original[i].feedrate, 1e-6));
        if (moves[i].type == EMoveType::Extrude && !moves[i].internal_only) {
            const double expected = 100.0 * (decreasing ? count - segment : segment + 1) / count;
            ++segment;
            CHECK_THAT(moves[i].overhang_percentage, Catch::Matchers::WithinAbs(expected, 1e-3));
            CHECK_THAT(moves[i].overhang_z_distance, Catch::Matchers::WithinAbs(0.2, 1e-6));
        }
    }
    // Orca: Neither the next line nor the next untagged arc may inherit the local profile.
    profiled.process_buffer("G1 X1 Y10 E1\n");
    CHECK_THAT(profiled.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
    profiled.process_buffer("G3 X11 Y0 I10 J0 E1\n");
    CHECK_THAT(profiled.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
}

// Orca: A tiny arc becomes one preview segment, but an unsupported interior sample must still survive
// even when both endpoints are fully supported. The following move keeps the independent scalar value.
TEST_CASE("Overhang arc profiles preserve interior peaks on short arcs", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    GCodeProcessor processor;
    processor.apply_config(config);
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    processor.process_buffer("G90\nM83\nT0\nG1 X0.05 Y0 Z0.2 F600\n" + tag(GCodeProcessor::ETags::Overhang, "33") +
        tag(GCodeProcessor::ETags::Overhang_Arc, "5,0,0,0,100,0,0") +
        bind_overhang_arc_profile("G3 X0 Y0.05 I-0.05 J0 E0.01\n"));
    const auto &moves = processor.get_result().moves;
    REQUIRE(std::count_if(moves.begin(), moves.end(), [](const auto &move) { return move.type == EMoveType::Extrude; }) == 1);
    CHECK_THAT(moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
    processor.process_buffer("G1 X10 E1\n");
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
    // Orca: Complete samples without a standalone marker must not color or leak past an unrelated arc;
    // merely mentioning the marker inside human-readable text is intentionally insufficient.
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, "3,0,0,50,100") +
        "G3 X9.95 Y0 I-0.05 J0 E0.01 ; do not apply OVERHANG_ARC_APPLY here\n");
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
}

// Orca: Corrupt, incomplete or stale chunks must fall back to the scalar tag, never partially color
// an arc or carry a previous profile across an intervening command or a parser reset.
TEST_CASE("Overhang arc profiles reject invalid chunks and stale associations", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    // Orca: Empty headers and samples must be invalid with both std::from_chars and the legacy
    // floating-point backend used by older macOS standard libraries, not silently become zero.
    const std::string data = GENERATE("", "3", "3,0", "1,0,0", "65537,0,0", "3,-1,0", "3,1,50,100", "3,0,0,nan,100", "3,0,0,inf,100",
        "3,0,0,101,100", "3,0,0,-1,100", "3,0,0,50,100junk", "3,0,0,50,100,", "3,0,0,50", "3,0,0,,100",
        ",0,0,50,100", "3,,0,50,100", "3,0,,50,100",
        "3,0,0,50,100,0", "3,0,0,50,100\nG2 X1 Y1 E1");
    CAPTURE(data);
    GCodeProcessor processor;
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const auto setup = [&]() {
        processor.apply_config(FullPrintConfig());
        processor.process_buffer("G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n" + tag(GCodeProcessor::ETags::Overhang, "33"));
    };
    setup();
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, data) +
        bind_overhang_arc_profile("G3 X0 Y10 I-10 J0 E1\n"));
    size_t checked = 0;
    for (const auto &move : processor.get_result().moves)
        if (move.type == EMoveType::Extrude) {
            CHECK_THAT(move.overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
            ++checked;
        }
    REQUIRE(checked > 1);
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, "3,0,0,50,100"));
    processor.reset();
    setup();
    processor.process_buffer("G3 X0 Y10 I-10 J0 E1\n");
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
}

// Orca: Post-processing may retain an arc's marker after removing or damaging its chunks. Such an
// arc must keep the preview unavailable unless a complete, valid profile was actually supplied.
TEST_CASE("Overhang arc markers require complete profiles to enable the preview", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    const bool bbl_printer = GENERATE(false, true);
    const std::string profile = GENERATE("", "3,0,0,50", "3,0,0,,100", "3,0,0,50,100");
    CAPTURE(bbl_printer, profile);
    const ScopedBblPrinterFlag scoped_printer_flag(bbl_printer);
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n");
    if (!profile.empty())
        processor.process_buffer(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Arc) + profile + "\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(bind_overhang_arc_profile("G3 X0 Y10 I-10 J0 E1\n"));
    const bool complete = profile == "3,0,0,50,100";
    CHECK(processor.get_result().has_overhang_metadata == complete);
    REQUIRE_FALSE(processor.get_result().moves.empty());
    REQUIRE(processor.get_result().moves.back().type == EMoveType::Extrude);
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage,
        Catch::Matchers::WithinAbs(complete ? 100.0 : 0.0, 1e-5));
}

TEST_CASE("Overhang metadata availability follows valid G-code tags", "[GCodeProcessor][Overhang]")
{
    const bool bbl_printer = GENERATE(false, true);
    CAPTURE(bbl_printer);
    const ScopedBblPrinterFlag scoped_printer_flag(bbl_printer);
    // Orca: Drive the streaming parser used for freshly generated G-code and verify that only a
    // valid tag advertises the optional preview mode in both supported tag dialects.
    GCodeProcessor processor;
    const std::string tag_prefix = bbl_printer ? "; OVERHANG: " : ";OVERHANG:";
    CHECK(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang) == tag_prefix.substr(1));
    // Orca: Reference spacing alone is not sufficient to offer the Overhang view.
    processor.process_buffer(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Z_Distance) + "0.2\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(tag_prefix + "37.5\n");
    CHECK(processor.get_result().has_overhang_metadata);

    // Orca: Resetting the processor must hide the mode again until another valid tag is parsed.
    processor.reset();
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(tag_prefix + "invalid\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);

    // Orca: Complete samples alone remain inert; only the inline marker on their exact arc publishes them.
    processor.reset();
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n;" +
        GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Arc) + "3,0,0,50,100\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(bind_overhang_arc_profile("G3 X0 Y10 I-10 J0 E1\n"));
    CHECK(processor.get_result().has_overhang_metadata);
}

// Orca: Malformed or non-finite percentages must neither enable the view nor leak into its color lookup.
TEST_CASE("Overhang percentages reject malformed and non-finite metadata", "[GCodeProcessor][Overhang][Regression]")
{
    // Orca: An empty scalar tag must not advertise metadata, just like an empty arc sample.
    const std::string value = GENERATE("", "nan", "inf", "-inf", "37.5garbage", "invalid");
    const bool preceding_valid_tag = GENERATE(false, true);
    CAPTURE(value, preceding_valid_tag);
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    const std::string tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang);
    processor.process_buffer("G90\nM83\nT0\n");
    if (preceding_valid_tag)
        processor.process_buffer(tag + "50\n");
    processor.process_buffer(tag + value + "\nG1 X10 Z0.2 E1 F600\n");
    CHECK(processor.get_result().has_overhang_metadata == preceding_valid_tag);
    REQUIRE_FALSE(processor.get_result().moves.empty());
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(0.0, 1e-6));
}

// Orca: Guard values are part of the public preview conversion contract: fully unsupported remains
// a ceiling even without dimensions, while supported or geometrically incomplete data remains a wall.
TEST_CASE("Overhang angle conversion handles boundary and incomplete metadata", "[ExtrusionProcessor][Overhang]")
{
    struct Case {
        const char *name;
        float percentage;
        float width;
        float height;
        float z_distance;
        float expected_degree;
    };
    const std::array<Case, 6> cases{{
        {"fully unsupported without dimensions", 100.0f, 0.0f, 0.0f, 0.0f, 90.0f},
        {"percentage above the supported range", 120.0f, 0.4f, 0.2f, 0.2f, 90.0f},
        {"fully supported", 0.0f, 0.4f, 0.2f, 0.2f, 0.0f},
        {"percentage below the supported range", -10.0f, 0.4f, 0.2f, 0.2f, 0.0f},
        {"missing width", 50.0f, 0.0f, 0.2f, 0.2f, 0.0f},
        {"missing vertical separation", 50.0f, 0.4f, 0.0f, 0.0f, 0.0f},
    }};

    for (const Case &test : cases) {
        DYNAMIC_SECTION(test.name) {
            libvgcode::PathVertex vertex;
            vertex.overhang_percentage = test.percentage;
            vertex.width = test.width;
            vertex.height = test.height;
            vertex.overhang_z_distance = test.z_distance;
            CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(test.expected_degree, 1e-6));
        }
    }
}

// Orca: A role change must clear perimeter-only metadata before unrelated extrusions, but a bridge
// deliberately consumes the same unsupported-width value and therefore retains it.
TEST_CASE("Overhang metadata follows perimeter and bridge extrusion roles", "[GCodeProcessor][Overhang][Regression]")
{
    struct Case {
        ExtrusionRole role;
        float expected_percentage;
    };
    const std::array<Case, 3> cases{{
        {erExternalPerimeter, 42.0f},
        {erBridgeInfill, 42.0f},
        {erInternalInfill, 0.0f},
    }};

    for (const Case &test : cases) {
        DYNAMIC_SECTION(ExtrusionEntity::role_to_string(test.role)) {
            GCodeProcessor processor;
            processor.apply_config(FullPrintConfig());
            const std::string overhang_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang) + "42\n";
            const std::string role_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) +
                ExtrusionEntity::role_to_string(test.role) + "\n";
            processor.process_buffer("G90\nM83\nT0\n" + overhang_tag + role_tag + "G1 X10 Z0.2 E1 F600\n");

            REQUIRE_FALSE(processor.get_result().moves.empty());
            const auto &move = processor.get_result().moves.back();
            REQUIRE(move.type == EMoveType::Extrude);
            CHECK(move.extrusion_role == test.role);
            CHECK_THAT(move.overhang_percentage, Catch::Matchers::WithinAbs(test.expected_percentage, 1e-6));
        }
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
    const auto percentages = estimator.estimate_overhang_percentages(path);
    REQUIRE(percentages.size() == 1);
    CHECK_THAT(percentages.front(), Catch::Matchers::WithinAbs(50.0, 1e-3));
    CHECK(path.polyline.points == original_points);
}

// Orca: Arachne may place an outer-wall center closer than half its width to the sliced contour.
// Compare layers relative to that actual inset so identical contours remain supported while a real
// outward shift retains its geometric percentage in both fixed- and variable-speed metadata paths.
TEST_CASE("Overhang metadata uses the current contour as its wall placement baseline",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    const double outward_shift = GENERATE(0.0, 0.001, 0.1);
    // Orca: A uniform sub-quantization shift is still a real shallow slope and must be retained.
    const float expected_percentage = float(100.0 * outward_shift / width);
    CAPTURE(outward_shift);

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
    path.polyline.points = {Point3::new_scale(1, path_y, 0), Point3::new_scale(39, path_y, 0)};

    const auto fixed_speed_percentages = estimator.estimate_overhang_percentages(path);
    REQUIRE(fixed_speed_percentages.size() == 1);
    CHECK_THAT(fixed_speed_percentages.front(), Catch::Matchers::WithinAbs(expected_percentage, 1e-3));

    const auto variable_speed_points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false, true);
    REQUIRE_FALSE(variable_speed_points.empty());
    for (const ProcessedPoint &point : variable_speed_points)
        CHECK_THAT(point.overhang_percentage, Catch::Matchers::WithinAbs(expected_percentage, 1e-3));
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
        100.0f, 100.0f, curled_slowdown, true);
    REQUIRE(points.size() >= 2);
    CHECK((points.front().speed < 99.0f) == curled_slowdown);
    CHECK((points.front().overlap < 0.99f) == curled_slowdown);
    for (const auto &point : points)
        CHECK_THAT(point.overhang_percentage, Catch::Matchers::WithinAbs(0.0, 1e-3));
}

// Orca: A freshly enabled estimator and one resuming after a gap must see the same geometry and
// curled edges as a continuously prepared estimator, including when a layer is prepared twice.
TEST_CASE("Overhang estimation restores the immediate support layer after skipped preparation", "[ExtrusionProcessor][Overhang][Regression]")
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
    // Orca: The stale contour cannot support the wall, whereas the actual lower layer supports it
    // fully but has a curled edge. Both caches must therefore be restored, independently of metadata.
    older->lslices = {ExPolygon(Polygon{Point::new_scale(0, 1), Point::new_scale(40, 1),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    lower->lslices = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    lower->curled_lines = {CurledLine(Point::new_scale(1, 0.2), Point::new_scale(39, 0.2), 1.0f)};
    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    if (prepared_count >= 1)
        estimator.prepare_for_new_layer(object, older);
    if (prepared_count >= 2)
        estimator.prepare_for_new_layer(object, lower);
    ExtrusionPath path(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline.points = {Point3::new_scale(1, 0.2, 0), Point3::new_scale(39, 0.2, 0)};
    for (int attempt = 0; attempt < 2; ++attempt) {
        CAPTURE(attempt);
        estimator.prepare_for_new_layer(object, upper);
        const auto percentages = estimator.estimate_overhang_percentages(path);
        REQUIRE(percentages.size() == 1);
        CHECK_THAT(percentages.front(), Catch::Matchers::WithinAbs(0.0, 1e-3));
        const auto points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
            ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
            100.0f, 100.0f, true, true);
        REQUIRE(points.size() >= 2);
        CHECK(points.front().speed < 99.0f);
        CHECK(points.front().overlap < 0.99f);
        CHECK_THAT(points.front().overhang_percentage, Catch::Matchers::WithinAbs(0.0, 1e-3));
    }
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

// Orca: Height modifiers can first enable slowdown or interrupt it for several layers. In both
// directions, toggling preview metadata must preserve every printer command, including speeds.
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
        // Orca: Equality alone could accept identical stale caches. The vertical back wall must also
        // retain its configured speed on the first layer after each transition, not a false slowdown.
        const auto supported_speeds = shallow_face_feed_rates(exported, false);
        REQUIRE_FALSE(supported_speeds.empty());
        for (double feed_rate : supported_speeds)
            CHECK_THAT(feed_rate / MM_PER_MIN, Catch::Matchers::WithinAbs(shallow_outer_wall_speed, 0.1));
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
    CAPTURE(spiral_mode);

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
    const std::string baseline = gcode(baseline_print);

    config.set_deserialize_strict("gcode_overhangs", "1");
    Print print;
    Model model;
    init_print({mesh}, print, model, config, nullptr, false);
    print.set_status_silent();
    print.process();
    ScopedTemporaryFile file(".gcode");
    GCodeProcessorResult generated;
    print.export_gcode(file.string(), &generated);
    std::ifstream stream(file.string());
    const std::string exported((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    REQUIRE(generated.has_overhang_metadata);
    CHECK(printer_commands(exported) == printer_commands(baseline));
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

// Orca: A 0.2 mm contour offset over a 0.2 mm slice-plane gap remains 45 degrees even when
// the current extrusion is 0.1 or 0.3 mm tall. Missing metadata retains the old height-based estimate.
TEST_CASE("Overhang angles use slice spacing rather than extrusion height", "[ExtrusionProcessor][Overhang][Regression]")
{
    const float height = GENERATE(0.1f, 0.3f);
    GCodeProcessor processor;
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const auto setup = [&]() {
        processor.apply_config(FullPrintConfig());
        processor.process_buffer("G90\nM83\nT0\n" + tag(GCodeProcessor::ETags::Height, std::to_string(height)) +
            tag(GCodeProcessor::ETags::Width, "0.4") + tag(GCodeProcessor::ETags::Overhang, "50"));
    };
    setup();
    const auto check_angle = [&](float expected_distance, double expected_angle) {
        const auto &move = processor.get_result().moves.back();
        REQUIRE(move.type == EMoveType::Extrude);
        CHECK_THAT(move.overhang_z_distance, Catch::Matchers::WithinAbs(expected_distance, 1e-6));
        libvgcode::PathVertex vertex;
        vertex.height = move.height;
        vertex.width = move.width;
        vertex.overhang_percentage = move.overhang_percentage;
        vertex.overhang_z_distance = move.overhang_z_distance;
        CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(expected_angle, 1e-4));
    };
    const double legacy_angle = std::atan(0.2 / height) * 180.0 / PI;
    processor.process_buffer("G1 X10 Z0.3 E1 F600\n");
    check_angle(0.0f, legacy_angle);
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Z_Distance, "0.2") + "G1 X20 E1\n");
    check_angle(0.2f, 45.0);

    // Orca: Explicit zero, malformed values and non-finite values must not retain a preceding override.
    for (const std::string &value : {"0", "-0.1", "nan", "inf", "invalid"}) {
        processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Z_Distance, "0.2"));
        processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Z_Distance, value) + "G91\nG1 X10 E1\n");
        check_angle(0.0f, legacy_angle);
    }
    // Orca: Reusing a parser for legacy G-code must clear a valid override from its previous input.
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Z_Distance, "0.2"));
    processor.reset();
    setup();
    processor.process_buffer("G1 X10 Z0.3 E1 F600\n");
    check_angle(0.0f, legacy_angle);
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
    REQUIRE(std::all_of(percentages.begin(), percentages.end(), [](float percentage) { return percentage >= 0.f && percentage <= 100.f; }));
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

    const auto fixed_speed_percentages = estimator.estimate_overhang_percentages(path);
    REQUIRE(fixed_speed_percentages.size() == 1);
    CHECK_THAT(fixed_speed_percentages.front(), Catch::Matchers::WithinAbs(0.0, 1e-3));

    const auto variable_speed_points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false, true);
    REQUIRE_FALSE(variable_speed_points.empty());
    for (size_t i = 0; i + 1 < variable_speed_points.size(); ++i)
        CHECK_THAT(variable_speed_points[i].overhang_percentage, Catch::Matchers::WithinAbs(0.0, 1e-3));
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
    estimator.prepare_for_new_layer(object, upper);
    ExtrusionPath path(role, 0.08, width, 0.2f);
    path.polyline.points = {Point3::new_scale(2, 0.5 * width, 0), Point3::new_scale(38, 0.5 * width, 0)};
    const double expected_percentage = 100.0 * (width - supported_width) / width;
    const auto percentages = estimator.estimate_overhang_percentages(path);
    REQUIRE(percentages.size() == 1);
    CHECK_THAT(percentages.front(), Catch::Matchers::WithinAbs(expected_percentage, 1e-3));
    // Orca: Variable-speed output uses the same geometric estimate for each emitted span.
    const auto points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false, true);
    REQUIRE(points.size() >= 2);
    for (size_t i = 0; i + 1 < points.size(); ++i)
        CHECK_THAT(points[i].overhang_percentage, Catch::Matchers::WithinAbs(expected_percentage, 1e-3));
}

// Orca: A bridge may be close to an unchanged outer contour while spanning a hole in the lower layer.
// Its support must come from the lower-layer area, never from perimeter-oriented contour association.
TEST_CASE("Overhang metadata keeps area-based support for bridges near an outer contour",
          "[ExtrusionProcessor][Overhang][Regression]")
{
    constexpr float width = 0.4f;
    Print print;
    Model model;
    init_print({cube(1.0)}, print, model);
    PrintObject *object = print.get_object(0);
    Layer *lower = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *upper = object->add_layer(1, 0.2, 0.4, 0.3);
    upper->lower_layer = lower;
    ExPolygon lower_slice(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 20), Point::new_scale(0, 20)});
    lower_slice.holes.emplace_back(Polygon{Point::new_scale(1, 0.001), Point::new_scale(1, 10),
        Point::new_scale(39, 10), Point::new_scale(39, 0.001)});
    lower_slice.holes.back().make_clockwise();
    lower->lslices = {std::move(lower_slice)};
    upper->lslices = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
        Point::new_scale(40, 20), Point::new_scale(0, 20)})};

    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    estimator.prepare_for_new_layer(object, lower);
    estimator.prepare_for_new_layer(object, upper);
    ExtrusionPath path(erBridgeInfill, 0.08, width, 0.2f);
    path.polyline.points = {Point3::new_scale(2, 0.201, 0), Point3::new_scale(38, 0.201, 0)};

    const auto percentages = estimator.estimate_overhang_percentages(path);
    REQUIRE(percentages.size() == 1);
    CHECK_THAT(percentages.front(), Catch::Matchers::WithinAbs(100.0, 1e-3));

    const auto variable_speed_points = estimator.estimate_extrusion_quality(path, ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}),
        100.0f, 100.0f, false, true);
    REQUIRE_FALSE(variable_speed_points.empty());
    for (size_t i = 0; i + 1 < variable_speed_points.size(); ++i)
        CHECK_THAT(variable_speed_points[i].overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-3));
}
