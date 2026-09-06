#include <catch2/catch_all.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig wipe_config(const char *wall_generator, bool wipe_inward,
                               const char *wipe_inward_distance = "50%",
                               const char *seam_gap = "10%", bool wipe_on_loops = false,
                               const char *wall_loops = "2",
                               const char *wall_sequence = "inner wall/outer wall",
                               bool alternate_extra_wall = false,
                               const char *sparse_infill_density = "0%",
                               const char *seam_position = "aligned")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",             "0.4" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "line_width",                  "0.45" },
        { "outer_wall_line_width",       "0" }, // Orca: Auto must use the actual path width.
        { "wall_loops",                  wall_loops },
        { "wall_generator",              wall_generator },
        { "wall_sequence",               wall_sequence },
        { "top_shell_layers",            "0" },
        { "bottom_shell_layers",         "0" },
        { "sparse_infill_density",       sparse_infill_density },
        { "seam_position",               seam_position },
        { "seam_gap",                    seam_gap },
        { "wipe",                        "1" },
        { "wipe_distance",               "2" },
        { "retraction_length",           "0.8" },
        { "retract_when_changing_layer", "1" },
        { "wipe_inward",                 wipe_inward ? "1" : "0" },
        { "wipe_inward_distance",        wipe_inward_distance },
        { "wipe_on_loops",               wipe_on_loops ? "1" : "0" },
        { "alternate_extra_wall",        alternate_extra_wall ? "1" : "0" },
        { "gcode_comments",              "1" },
        { "machine_start_gcode",         "" },
        { "machine_end_gcode",           "" },
    });
    return config;
}

struct WipeTrajectory {
    Vec2d start;
    double z;
    std::vector<Vec2d> destinations;
};

std::vector<WipeTrajectory> wipe_trajectories(const std::string &gcode)
{
    const std::string &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    const std::string &end_tag   = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End);
    std::vector<WipeTrajectory> trajectories;
    bool in_wipe = false;

    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find(start_tag) != std::string_view::npos) {
            in_wipe = true;
            trajectories.push_back({Vec2d(self.x(), self.y()), self.z(), {}});
            return;
        }
        if (comment.find(end_tag) != std::string_view::npos) {
            in_wipe = false;
            return;
        }
        if (in_wipe && line.dist_XY(self) > EPSILON)
            trajectories.back().destinations.emplace_back(line.new_X(self), line.new_Y(self));
    });
    return trajectories;
}

std::vector<Vec2d> wipe_destinations(const std::string &gcode)
{
    std::vector<Vec2d> destinations;
    for (const WipeTrajectory &trajectory : wipe_trajectories(gcode))
        destinations.insert(destinations.end(), trajectory.destinations.begin(), trajectory.destinations.end());
    return destinations;
}

bool trajectories_differ(const std::vector<Vec2d> &lhs, const std::vector<Vec2d> &rhs)
{
    if (lhs.size() != rhs.size())
        return true;
    for (size_t i = 0; i < lhs.size(); ++i)
        if ((lhs[i] - rhs[i]).norm() > 0.01)
            return true;
    return false;
}

double trajectory_length(const WipeTrajectory &trajectory)
{
    double length = 0.;
    Vec2d previous = trajectory.start;
    for (const Vec2d &destination : trajectory.destinations) {
        length += (destination - previous).norm();
        previous = destination;
    }
    return length;
}

} // namespace

TEST_CASE("Inward wipe changes the exported trajectory when outer wall width is Auto", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false)));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true)));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    REQUIRE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe keeps its offset when seam gap is zero", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false, "50%", "0%")));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "50%", "0%")));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    REQUIRE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe is retained across layers with a back seam", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const DynamicPrintConfig inward_config = wipe_config(
        wall_generator, true, "50%", "0%", false, "3", "inner-outer-inner wall", false, "0%", "back");
    const std::vector<WipeTrajectory> inward = wipe_trajectories(slice({make_cube(27., 27., 1.)}, inward_config));

    REQUIRE_FALSE(inward.empty());
    std::map<double, bool> inward_wipe_by_layer;
    for (const WipeTrajectory &trajectory : inward) {
        bool &has_inward_wipe = inward_wipe_by_layer[trajectory.z];
        if (trajectory.destinations.empty())
            continue;
        const Vec2d first_move = trajectory.destinations.front() - trajectory.start;
        // Orca: a back seam lands on the cube's positive-X/positive-Y corner.
        // Its inward wipe must move diagonally away from both external faces.
        has_inward_wipe = has_inward_wipe ||
            (trajectory.start.x() > 13. && trajectory.start.y() > 13. &&
             first_move.x() < -0.05 && first_move.y() < -0.05);
    }
    REQUIRE(inward_wipe_by_layer.size() == 5);
    for (const auto &[z, has_inward_wipe] : inward_wipe_by_layer) {
        INFO("layer Z: " << z);
        REQUIRE(has_inward_wipe);
    }
}

TEST_CASE("Literal inward wipe distance is clamped to the outer wall width", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false)));
    const std::vector<Vec2d> full_width = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "100%")));
    const std::vector<Vec2d> oversized = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "2")));

    REQUIRE_FALSE(full_width.empty());
    REQUIRE(trajectories_differ(regular, full_width));
    REQUIRE(oversized.size() == full_width.size());
    for (size_t i = 0; i < full_width.size(); ++i)
        REQUIRE_THAT((oversized[i] - full_width[i]).norm(), Catch::Matchers::WithinAbs(0., 0.01));
}

TEST_CASE("Inward wipe is not applied without an adjacent wall", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false, "50%", "10%", false, "1")));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "50%", "10%", false, "1")));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe uses an alternate extra wall when the configured wall count is one", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const DynamicPrintConfig regular_config = wipe_config(
        wall_generator, false, "50%", "10%", false, "1", "inner wall/outer wall", true, "15%");
    const DynamicPrintConfig inward_config = wipe_config(
        wall_generator, true, "50%", "10%", false, "1", "inner wall/outer wall", true, "15%");
    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, regular_config));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, inward_config));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    REQUIRE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe is not applied before the adjacent wall is printed", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(
            wall_generator, false, "50%", "10%", false, "2", "outer wall/inner wall")));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(
            wall_generator, true, "50%", "10%", false, "2", "outer wall/inner wall")));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe remains valid after wipe on loops moves the nozzle", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::string loop_move = slice(
        {make_cube(10., 10., 1.)}, wipe_config(wall_generator, false, "50%", "10%", true));
    const std::string combined = slice(
        {make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "50%", "10%", true));
    const std::string inward_only = slice(
        {make_cube(10., 10., 1.)}, wipe_config(wall_generator, true));

    REQUIRE(loop_move.find("move inwards before travel") != std::string::npos);
    REQUIRE(combined.find("move inwards before travel") != std::string::npos);

    const std::vector<WipeTrajectory> combined_trajectories = wipe_trajectories(combined);
    const std::vector<WipeTrajectory> inward_trajectories = wipe_trajectories(inward_only);
    REQUIRE_FALSE(combined_trajectories.empty());
    REQUIRE(combined_trajectories.size() == inward_trajectories.size());
    REQUIRE(trajectories_differ(wipe_destinations(combined), wipe_destinations(loop_move)));

    bool start_changed = false;
    for (size_t i = 0; i < combined_trajectories.size(); ++i) {
        start_changed = start_changed ||
            (combined_trajectories[i].start - inward_trajectories[i].start).norm() > 0.01;
        REQUIRE_THAT(trajectory_length(combined_trajectories[i]),
                     Catch::Matchers::WithinAbs(trajectory_length(inward_trajectories[i]), 0.01));
    }
    REQUIRE(start_changed);
}
