#include <catch2/catch_all.hpp>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
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

} // namespace

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
