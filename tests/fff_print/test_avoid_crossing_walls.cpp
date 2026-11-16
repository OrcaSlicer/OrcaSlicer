#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Test;

// "Avoid crossing walls" plans the travel moves of a layer inside a region built from that
// layer's contours, so a travel that has to reach the far side of an opening is routed around
// it. The planning is switched off for a travel whose start position the writer does not trust,
// which is right after custom G-code that moved the toolhead. A machine's timelapse block is
// inserted at every layer change, and it is inert most of the time (no photo taken), so it must
// not be treated as a move.

namespace {

// One travel move chain: the points it visits. A planned path has several legs, a straight
// (unplanned) travel has two.
struct TravelRun {
    std::vector<Vec2d> points;
    // False for the travel that leaves the print's start position, which is not part of the
    // object's region model.
    bool after_extrusion = false;
};

// The travel moves of a G-code, in order, as point chains. A move with an E word extrudes or
// wipes and ends the current chain; a move without XY is a lift, which leaves it open.
std::vector<TravelRun> travel_runs(const std::string & gcode)
{
    std::vector<TravelRun> runs;
    Vec2d                  pos { 0., 0. };
    bool                   have_pos        = false;
    bool                   prev_was_travel = false;
    bool                   extruded        = false;
    std::istringstream     in(gcode);
    std::string            line;
    auto number = [](const std::string & word, double & out) {
        try {
            size_t used = 0;
            out         = std::stod(word.substr(1), &used);
            return used == word.size() - 1;
        } catch (...) {
            return false;
        }
    };
    while (std::getline(in, line)) {
        line.erase(std::find(line.begin(), line.end(), ';'), line.end());
        std::istringstream words(line);
        std::string w;
        words >> w;
        if (w != "G0" && w != "G1")
            continue;
        Vec2d to     = pos;
        bool  has_xy = false;
        bool  has_e  = false;
        while (words >> w) {
            if (w.empty())
                continue;
            const char c = w.front();
            if (c != 'X' && c != 'Y' && c != 'E')
                continue;
            double v = 0.;
            if (!number(w, v))
                continue;
            if (c == 'E')
                has_e = true;
            else {
                to[c == 'X' ? 0 : 1] = v;
                has_xy = true;
            }
        }
        if (has_e) {
            // An extrusion, or a retraction with a wipe; both end the travel chain.
            if (has_xy)
                pos = to;
            extruded        = true;
            prev_was_travel = false;
            continue;
        }
        if (!has_xy)
            continue;
        if (!prev_was_travel) {
            runs.emplace_back();
            runs.back().after_extrusion = extruded;
            if (have_pos)
                runs.back().points.emplace_back(pos);
        }
        runs.back().points.emplace_back(to);
        pos             = to;
        have_pos        = true;
        prev_was_travel = true;
    }
    return runs;
}

// True when the segment visits the interior of the open square around `centre`.
bool enters_open_square(const Vec2d & a, const Vec2d & b, const Vec2d & centre, double half)
{
    const Vec2d lo = centre - Vec2d(half, half);
    const Vec2d hi = centre + Vec2d(half, half);
    double      t0 = 0.;
    double      t1 = 1.;
    for (int axis = 0; axis < 2; ++axis) {
        const double p  = b[axis] - a[axis];
        const double oa = a[axis];
        if (std::abs(p) < 1e-9) {
            if (oa <= lo[axis] + 1e-9 || oa >= hi[axis] - 1e-9)
                return false;
            continue;
        }
        double ta = (lo[axis] - oa) / p;
        double tb = (hi[axis] - oa) / p;
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return false;
    }
    return t1 > t0 + 1e-9;
}

size_t travels_over_opening(const std::vector<TravelRun> & runs, const Vec2d & centre, double half)
{
    size_t over = 0;
    for (const TravelRun & run : runs) {
        if (!run.after_extrusion || run.points.size() < 2)
            continue;
        for (size_t i = 1; i < run.points.size(); ++i)
            if (enters_open_square(run.points[i - 1], run.points[i], centre, half)) {
                ++over;
                break;
            }
    }
    return over;
}

size_t planned_runs(const std::vector<TravelRun> & runs)
{
    return std::count_if(runs.begin(), runs.end(),
                         [](const TravelRun & run) { return run.after_extrusion && run.points.size() > 2; });
}

DynamicPrintConfig opening_config(const std::string & time_lapse_gcode)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height",                        "0.3" },
        {"first_layer_height",                  "0.3" },
        {"wall_loops",                          "2" },
        {"top_shell_layers",                    "0" },
        {"bottom_shell_layers",                 "0" },
        {"sparse_infill_density",               "0" },
        // The setting under test, with the detour length left unlimited as it is by default.
        {"reduce_crossing_wall",                "1" },
        {"max_travel_detour_distance",          "0" },
        // Scattered seams make the first point of a layer land opposite to the last point of
        // the layer below, so the travel at the layer change has to cross the opening.
        {"seam_position",                       "random" },
        {"z_hop",                               "0" },
        {"retract_when_changing_layer",         "1" },
        {"time_lapse_gcode",                    time_lapse_gcode },
        {"single_extruder_multi_material",      "1" },
        {"print_sequence",                      "by layer" },
        {"enable_support",                      "0" },
    });
    return config;
}

// Slice a 20 x 20 mm cube with a 10 x 10 mm opening through it, walls only, on a machine that
// inserts the timelapse G-code at every layer change.
struct OpeningSlice {
    std::vector<TravelRun> runs;
    Vec2d                  centre;
};

OpeningSlice slice_cube_with_opening(const std::string & time_lapse_gcode)
{
    Print print;
    Model model;
    Test::init_print({ TestMesh::cube_with_hole }, print, model, opening_config(time_lapse_gcode));
    // The timelapse block is inserted by the layer change of the Bambu Lab machines.
    print.is_BBL_printer() = true;
    const std::string gcode = Test::gcode(print);
    INFO(gcode.substr(0, std::min<size_t>(gcode.size(), 4000)));
    const BoundingBoxf3 box = model.objects.front()->instance_bounding_box(0);
    const Vec2d         centre{ 0.5 * (box.min.x() + box.max.x()), 0.5 * (box.min.y() + box.max.y()) };
    return { travel_runs(gcode), centre };
}

// The middle of the 10 x 10 mm opening, kept well clear of the walls and of the rim a planned
// path is allowed to follow.
constexpr double OPENING_HALF = 2.0;

} // namespace

TEST_CASE("Inert G-code at a layer change keeps travels from crossing walls", "[AvoidCrossingWalls][GCode][Regression]")
{
    // A timelapse block that commands no motion: a comment, a flag, a photo marker.
    const OpeningSlice slice = slice_cube_with_opening("; take a photo\nM1002 judge_flag timelapse_record_flag\nM400 P300\n");
    REQUIRE(slice.runs.size() > 5);
    // The planning is running: travel moves are routed, not straight lines.
    CHECK(planned_runs(slice.runs) > 0);
    CHECK(travels_over_opening(slice.runs, slice.centre, OPENING_HALF) == 0);
}

TEST_CASE("G-code that moves the toolhead at a layer change keeps the conservative travel", "[AvoidCrossingWalls][GCode][Compatibility]")
{
    // The same block with the photo move a machine performs when a timelapse is recorded. The
    // toolhead is then somewhere the slice does not track, so the first travel of the layer
    // stays a straight line - the straight moves over the opening are the evidence.
    const OpeningSlice slice =
        slice_cube_with_opening("; take a photo\nG1 X0 Y0 F18000\nM400 P300\nG1 X0 F18000\n");
    REQUIRE(slice.runs.size() > 5);
    CHECK(travels_over_opening(slice.runs, slice.centre, OPENING_HALF) > 0);
}

TEST_CASE("Motion detection tells an inert custom G-code block from one that moves", "[AvoidCrossingWalls][GCodeProcessor]")
{
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead(""));
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead("; take a photo\nM1002 judge_flag timelapse_record_flag\nM400 P300\nM624 AQ==\n"));
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead("M73 L3\nM991 S0 P2\n; layer num/total_layer_count: 3/145\n"));
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead("G92 E0\nG17\nG1 E-.6 F1800\n"));
    // A comment that names a move is not a move.
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead("; G1 X0 Y0 photo position\nM971 S11\n"));
    // An undotted G word and a move carrying only E do not move the toolhead either.
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead("G1E-.6\nG0\n"));
    // G10 is a polyline definition, not a move word.
    CHECK_FALSE(GCodeProcessor::gcode_moves_toolhead("G10 P1 X0 Y0 Z0\n"));

    CHECK(GCodeProcessor::gcode_moves_toolhead("G1 X0 Y0 F18000\n"));
    CHECK(GCodeProcessor::gcode_moves_toolhead("G1 Z.72\n"));
    CHECK(GCodeProcessor::gcode_moves_toolhead("G0 X187 Y178 F20000\n"));
    // An arc moves in XY through its centre offsets.
    CHECK(GCodeProcessor::gcode_moves_toolhead("G2 Z1.2 I0.86 J0.86 P1 F20000\n"));
    // The A1 mini's timelapse block, with the photo move compiled in.
    CHECK(GCodeProcessor::gcode_moves_toolhead(
        "; date\nM622.1 S1\nM1002 judge_flag timelapse_record_flag\nM622 J1\nG92 E0\nG17\n"
        "G2 Z1.72 I0.86 J0.86 P1 F20000\nG1 Z18.4\nG1 X0 Y93.999 F18000\nG1 X-13.0 F3000\nM971 S11 C11 O0\nM623\n"));
}
