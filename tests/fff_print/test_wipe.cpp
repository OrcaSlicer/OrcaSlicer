#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include <cstddef>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp"
#include "libslic3r/Polyline.hpp"

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

TEST_CASE("Wipe retraction preserves fractional speed with inward wipe disabled", "[Wipe][Regression]")
{
    const char *retraction_speed = GENERATE("25.25", "25.5", "25.75");
    const char *relative_e = GENERATE("0", "1");
    INFO("retraction speed: " << retraction_speed);
    INFO("relative E: " << relative_e);
    DynamicPrintConfig config = wipe_config("classic", false);
    config.set_deserialize_strict({
        {"gcode_flavor", "marlin2"},
        {"use_relative_e_distances", relative_e},
        {"retraction_speed", retraction_speed},
        {"retraction_length", "0.8"},
        {"retract_before_wipe", "0%"},
        {"retract_after_wipe", "0%"},
        {"role_based_wipe_speed", "0"},
        {"wipe_speed", "100"},
        {"wipe_distance", "2"},
    });
    const std::string output = slice({make_cube(10., 10., 1.)}, config);
    const auto &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    const auto &end_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End);
    double before_wipe = 0.;
    double during_wipe = 0.;
    bool in_wipe = false;
    bool complete = false;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (complete)
            return;
        if (line.comment().find(start_tag) != std::string_view::npos) {
            in_wipe = true;
        } else if (in_wipe && line.comment().find(end_tag) != std::string_view::npos) {
            complete = true;
        } else if (line.retracting(self)) {
            (in_wipe ? during_wipe : before_wipe) -= line.dist_E(self);
        } else if (line.extruding(self)) {
            before_wipe = 0.;
        }
    });

    REQUIRE(complete);
    // At 100 mm/s, the 2 mm wipe lasts 0.02 seconds. The remaining part of
    // the configured 0.8 mm retraction must be emitted before that wipe.
    const double expected_during = std::stod(retraction_speed) * 2. / 100.;
    CHECK_THAT(during_wipe, Catch::Matchers::WithinAbs(expected_during, 0.00005));
    CHECK_THAT(before_wipe, Catch::Matchers::WithinAbs(0.8 - expected_during, 0.00005));
}

TEST_CASE("Inward wipe preserves retraction splits and minimum travel", "[Wipe][RetractionSplit][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *relative_e = GENERATE("0", "1");
    const char *reduce_crossing_wall = GENERATE("0", "1");
    // Retract after outer wall ignores the threshold when leaving the outer wall, so
    // with a 5 mm threshold it must retract at every transition, like a zero threshold.
    const auto [minimum_travel, retract_after_outer_wall] = GENERATE(table<const char *, bool>({
        {"5", false}, {"1", false}, {"0", false}, {"5", true}
    }));
    const bool wipe_on_loops = GENERATE(false, true);
    const auto split = GENERATE(table<const char *, const char *>({
        {"0%", "50%"}, {"25%", "25%"}, {"100%", "0%"}, {"0%", "100%"}, {"0%", "0%"}
    }));
    const char *before_percent = std::get<0>(split);
    const char *after_percent = std::get<1>(split);
    CAPTURE(wall_generator, relative_e, reduce_crossing_wall, minimum_travel, retract_after_outer_wall,
            wipe_on_loops, before_percent, after_percent);
    DynamicPrintConfig config = wipe_config(
        wall_generator, true, "50%", "10%", wipe_on_loops, "3", "inner-outer-inner wall");
    config.set_deserialize_strict({
        {"gcode_flavor", "marlin2"},
        {"use_relative_e_distances", relative_e},
        {"reduce_crossing_wall", reduce_crossing_wall},
        {"retraction_minimum_travel", minimum_travel},
        {"retract_after_outer_wall", retract_after_outer_wall ? "1" : "0"},
        {"retract_when_changing_layer", "0"},
        {"use_firmware_retraction", "0"},
        {"retract_before_wipe", before_percent},
        {"retract_after_wipe", after_percent},
        {"retraction_speed", "25.5"},
        {"role_based_wipe_speed", "0"},
        {"wipe_speed", "100"},
        {"z_hop", "0.4"},
        {"retract_lift_above", "0"},
        {"retract_lift_below", "0"},
    });
    config.set_key_value("z_hop_types", new ConfigOptionEnumsGeneric{zhtNormal});
    config.set_key_value("retract_lift_enforce", new ConfigOptionEnumsGeneric{rletAllSurfaces});
    std::vector<bool> regular_wipes;
    for (bool inward : {false, true}) {
        config.set_key_value("wipe_inward", new ConfigOptionBool(inward));
        const std::string output = slice({make_cube(10., 10., 1.)}, config);
        const auto &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
        const auto &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
        const auto &end_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End);
        ExtrusionRole role = erNone;
        bool after_outer_wall = false;
        bool in_wipe = false;
        bool wipe_seen = false;
        size_t transitions = 0;
        size_t same_layer_transitions = 0;
        size_t outer_wipes = 0;
        double before_wipe = 0.;
        double during_wipe = 0.;
        double after_wipe = 0.;
        double wipe_length = 0.;
        double lift = 0.;
        double outer_z = 0.;
        GCodeReader parser;
        parser.apply_config(config);
        parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.comment().find(role_tag) == 0)
                role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
            if (line.comment().find(start_tag) == 0) {
                in_wipe = true;
                wipe_seen = true;
                if (after_outer_wall)
                    ++outer_wipes;
            } else if (line.comment().find(end_tag) == 0) {
                in_wipe = false;
            }
            if (line.extruding(self) && line.dist_XY(self) > EPSILON) {
                if (role == erExternalPerimeter) {
                    after_outer_wall = true;
                    before_wipe = during_wipe = after_wipe = wipe_length = lift = 0.;
                    wipe_seen = false;
                    outer_z = line.new_Z(self);
                } else if (after_outer_wall) {
                    REQUIRE(role == erPerimeter);
                    const bool retract = inward ? regular_wipes.at(transitions) : wipe_seen;
                    if (!inward)
                        regular_wipes.push_back(wipe_seen);
                    CHECK(wipe_seen == retract);
                    if (retract)
                        REQUIRE(wipe_length > EPSILON);
                    else
                        CHECK_THAT(wipe_length, Catch::Matchers::WithinAbs(0., EPSILON));
                    ++transitions;
                    const double layer_rise = std::max(0., double(self.z()) - outer_z);
                    if (layer_rise < EPSILON)
                        ++same_layer_transitions;
                    // Inward wiping must use the same retraction decisions as
                    // regular wiping, including a longer first-layer transition.
                    // The 1 mm threshold also catches a wipe endpoint that would
                    // make an otherwise short travel long enough to retract.
                    // Otherwise the configured split is limited by the time available
                    // at 100 mm/s, with any excess retracted before the wipe.
                    const double expected_after = retract ? 0.8 * std::stod(after_percent) / 100. : 0.;
                    const double requested_before = retract ? 0.8 * std::stod(before_percent) / 100. : 0.;
                    const double expected_during = retract ? std::min(0.8 - requested_before - expected_after,
                                                                      25.5 * wipe_length / 100.) : 0.;
                    const double expected_before = retract ? 0.8 - expected_during - expected_after : 0.;
                    CHECK_THAT(before_wipe, Catch::Matchers::WithinAbs(expected_before, 0.0001));
                    CHECK_THAT(during_wipe, Catch::Matchers::WithinAbs(expected_during, 0.0001));
                    CHECK_THAT(after_wipe, Catch::Matchers::WithinAbs(expected_after, 0.0001));
                    CHECK_THAT(lift - layer_rise, Catch::Matchers::WithinAbs(retract ? 0.4 : 0., 0.001));
                    after_outer_wall = false;
                }
            } else if (after_outer_wall) {
                if (line.retracting(self)) {
                    double &portion = in_wipe ? during_wipe : (wipe_seen ? after_wipe : before_wipe);
                    portion -= line.dist_E(self);
                }
                if (in_wipe)
                    wipe_length += line.dist_XY(self);
                lift += std::max(0., double(line.dist_Z(self)));
            }
        });
        // The 1 mm cube has five 0.2 mm layers. Inward wiping follows the
        // ordinary retraction decision at every outer-to-inner wall transition.
        REQUIRE(transitions == 5);
        REQUIRE(same_layer_transitions >= 4);
        REQUIRE(outer_wipes == std::count(regular_wipes.begin(), regular_wipes.end(), true));
        if (std::stod(minimum_travel) == 0. || retract_after_outer_wall)
            REQUIRE(outer_wipes == transitions);
        else
            REQUIRE(outer_wipes < transitions);
    }
}

TEST_CASE("Changing inward wipe settings preserves the sliced geometry", "[Wipe][Regression]")
{
    const char *key = GENERATE("wipe_inward", "wipe_inward_distance");
    DynamicPrintConfig config = wipe_config("classic", false);
    Print print;
    Model model;
    init_print({make_cube(10., 10., 1.)}, print, model, config);
    gcode(print);
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.is_step_done(posPerimeters));
    REQUIRE(object.is_step_done(posInfill));
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE(print.is_step_done(psGCodeExport));

    DynamicPrintConfig changed = config;
    changed.set_deserialize_strict({{key, std::string(key) == "wipe_inward" ? "1" : "75%"}});
    print.apply(model, changed);

    CHECK(print.objects().front()->is_step_done(posPerimeters));
    CHECK(print.objects().front()->is_step_done(posInfill));
    CHECK(print.is_step_done(psWipeTower));
    CHECK_FALSE(print.is_step_done(psGCodeExport));
}

TEST_CASE("Retraction and pressure advance calibration suppress inward wipe overrides", "[Wipe][Regression]")
{
    const auto mode = GENERATE(CalibMode::Calib_None, CalibMode::Calib_PA_Tower,
                              CalibMode::Calib_Auto_PA_Line, CalibMode::Calib_Retraction_tower,
                              CalibMode::Calib_Flow_Rate);
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool per_object = GENERATE(false, true);
    INFO("calibration mode: " << int(mode) << ", wall generator: " << wall_generator
         << ", per-object override: " << per_object);

    const auto trajectories = [&](bool inward) {
        DynamicPrintConfig config = wipe_config(wall_generator, inward && !per_object);
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
            {{"wipe_inward", inward ? "1" : "0"}}
        };
        Print print;
        Model model;
        init_print({make_cube(10., 10., 1.)}, print, model, config, per_object ? &overrides : nullptr);
        Calib_Params params;
        params.mode = mode;
        params.start = 0.2;
        params.end = 0.4;
        params.step = 0.1;
        print.set_calib_params(params);
        return wipe_destinations(gcode(print));
    };

    const auto regular = trajectories(false);
    const auto inward = trajectories(true);
    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    // Other calibration modes and ordinary prints must still honor the option.
    const bool should_differ = mode == CalibMode::Calib_None || mode == CalibMode::Calib_Flow_Rate;
    CHECK(trajectories_differ(regular, inward) == should_differ);
}

TEST_CASE("Inactive inward wipe settings preserve the exported trajectory", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool disable_wiping = GENERATE(false, true);
    DynamicPrintConfig regular = wipe_config(wall_generator, false);
    DynamicPrintConfig inward = wipe_config(wall_generator, true, disable_wiping ? "50%" : "0");
    if (disable_wiping) {
        regular.set_deserialize_strict({{"wipe", "0"}});
        inward.set_deserialize_strict({{"wipe", "0"}});
    }
    const auto regular_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, regular));
    const auto inward_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, inward));
    if (!disable_wiping)
        REQUIRE_FALSE(regular_paths.empty());
    CHECK_FALSE(trajectories_differ(regular_paths, inward_paths));
}

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

TEST_CASE("Inward wipe recognizes an external wall starting on an overhang", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool inward = GENERATE(false, true);
    CAPTURE(wall_generator, inward);
    const auto config = wipe_config(wall_generator, inward, "50%", "0%", false,
                                    "3", "inner-outer-inner wall", false, "0%", "back");
    Print print;
    Model model;
    init_print({make_cube(10., 10., 1.)}, print, model, config);
    print.process();
    size_t mixed_loops = 0;
    double external_width = 0.;
    const auto mark_overhangs = [&](auto &&self, ExtrusionEntity *entity) -> void {
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            for (ExtrusionEntity *child : collection->entities)
                self(self, child);
        } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity); loop && is_external_perimeter(loop->role())) {
            // Keep the printed geometry intact and give the back seam overhang
            // roles. The front edge remains an ordinary external-wall segment.
            ExtrusionPaths paths;
            bool has_overhang = false;
            bool has_external = false;
            for (const ExtrusionPath &source : loop->paths) {
                for (size_t i = 1; i < source.polyline.points.size(); ++i) {
                    ExtrusionPath path = source;
                    path.polyline.points = {source.polyline.points[i - 1], source.polyline.points[i]};
                    const bool overhang = path.polyline.points.front().y() > 0 || path.polyline.points.back().y() > 0;
                    path.set_extrusion_role(overhang ? erOverhangPerimeter : erExternalPerimeter);
                    // A wider overhang bead at the seam must not widen the inward offset.
                    if (overhang)
                        path.width *= 2.f;
                    else
                        external_width = std::max(external_width, double(path.width));
                    has_overhang |= overhang;
                    has_external |= !overhang;
                    paths.push_back(std::move(path));
                }
            }
            REQUIRE(has_overhang);
            REQUIRE(has_external);
            loop->paths = std::move(paths);
            ++mixed_loops;
        }
    };
    for (const PrintObject *object : print.objects())
        for (Layer *layer : object->layers())
            for (LayerRegion *region : layer->regions())
                mark_overhangs(mark_overhangs, &region->perimeters);
    REQUIRE(mixed_loops > 0);

    bool has_inward_wipe = false;
    for (const WipeTrajectory &trajectory : wipe_trajectories(gcode(print))) {
        if (trajectory.destinations.empty())
            continue;
        const Vec2d move = trajectory.destinations.front() - trajectory.start;
        if (trajectory.start.x() > 4. && trajectory.start.y() > 4. && move.x() < -0.05 && move.y() < -0.05) {
            has_inward_wipe = true;
            // The 50% offset of the external wall reaches at most its miter at this square corner.
            CHECK(move.norm() < 0.5 * external_width * std::sqrt(2.) + 0.01);
        }
    }
    CHECK(has_inward_wipe == inward);
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

    DynamicPrintConfig inward_config = wipe_config(
        wall_generator, true, "50%", "0%", false, "3", "inner-outer-inner wall", false, "0%", "back");
    // Exercise the offset on every layer independently of the travel threshold.
    inward_config.set_deserialize_strict({{"retraction_minimum_travel", "0"}});
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

TEST_CASE("Inward wipe leaves a sandwich wall corner toward the printed material",
          "[Wipe][InwardCornerRegression][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *layer_height = GENERATE("0.1", "0.2");
    const char *seam_gap = GENERATE("0%", "10%");
    const bool wipe_on_loops = GENERATE(false, true);
    CAPTURE(wall_generator, layer_height, seam_gap, wipe_on_loops);
    DynamicPrintConfig config = wipe_config(
        wall_generator, true, "50%", seam_gap, wipe_on_loops, "3", "inner-outer-inner wall", false, "0%", "back");
    config.set_deserialize_strict({
        {"layer_height", layer_height},
        {"initial_layer_print_height", layer_height},
        {"initial_layer_line_width", "0.5"},
        {"outer_wall_line_width", "0.42"},
        {"inner_wall_line_width", "0.45"},
        {"elefant_foot_compensation", "0"},
        {"retraction_minimum_travel", "0"},
    });
    size_t outer_wipes = 0;
    const double requested_offset = 0.42 * 0.5;
    for (const WipeTrajectory &trajectory : wipe_trajectories(slice({make_cube(27., 27., 0.6)}, config))) {
        // This back seam lies above x=y=13; the inner walls lie below it.
        if (trajectory.start.x() <= 13. || trajectory.start.y() <= 13.)
            continue;
        ++outer_wipes;
        REQUIRE_FALSE(trajectory.destinations.empty());
        const Vec2d first_move = trajectory.destinations.front() - trajectory.start;
        CHECK(first_move.x() < -0.2 * requested_offset);
        CHECK(first_move.y() < -0.2 * requested_offset);
        CHECK(first_move.norm() > 0.5 * requested_offset);
    }
    REQUIRE(outer_wipes == size_t(std::lround(0.6 / std::stod(layer_height))));
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

TEST_CASE("Sandwich wall order limits inward wipe to half the outer wall width", "[Wipe][InwardWallOrder][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool wipe_on_loops = GENERATE(false, true);
    const auto [outer_width, distance, limited] = GENERATE(table<const char *, const char *, bool>({
        {"0", "25%", false}, {"0", "50%", true}, {"0", "50.1%", true}, {"0", "100%", true},
        {"0.4", "0.15", false}, {"0.4", "0.21", true}, {"0.6", "0.21", false}, {"0.6", "0.4", true}
    }));
    CAPTURE(wall_generator, wipe_on_loops, outer_width, distance);
    DynamicPrintConfig regular = wipe_config(
        wall_generator, false, "50%", "10%", wipe_on_loops, "3", "inner-outer-inner wall");
    regular.set_deserialize_strict({{"outer_wall_line_width", outer_width}, {"retraction_minimum_travel", "0"}});
    DynamicPrintConfig inward = regular;
    inward.set_deserialize_strict({{"wipe_inward", "1"}});
    const auto regular_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, regular));
    const auto half_width_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, inward));
    inward.set_deserialize_strict({{"wipe_inward_distance", distance}});
    const auto requested_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, inward));
    REQUIRE_FALSE(regular_paths.empty());
    REQUIRE_FALSE(half_width_paths.empty());
    REQUIRE_FALSE(requested_paths.empty());
    // Oversized requests retain the inward wipe instead of falling back to the
    // regular path. The same 0.21 mm request reaches the limit for a 0.4 mm
    // wall but remains below it for a 0.6 mm wall, using actual extrusion widths.
    REQUIRE(trajectories_differ(regular_paths, half_width_paths));
    CHECK(trajectories_differ(half_width_paths, requested_paths) == !limited);
}

TEST_CASE("Inward wipe is not applied without an adjacent wall", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *wall_sequence = GENERATE("inner wall/outer wall", "outer wall/inner wall", "inner-outer-inner wall");
    INFO("wall generator: " << wall_generator << ", wall sequence: " << wall_sequence);

    const std::vector<Vec2d> regular = wipe_destinations(slice({make_cube(10., 10., 1.)},
        wipe_config(wall_generator, false, "50%", "10%", false, "1", wall_sequence)));
    const std::vector<Vec2d> inward = wipe_destinations(slice({make_cube(10., 10., 1.)},
        wipe_config(wall_generator, true, "50%", "10%", false, "1", wall_sequence)));

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

TEST_CASE("Outer wall first limits inward wipe to half the outer wall width", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    // The adjacent inner wall is printed after the outer wall. It still sets the
    // inward side, but only the outer bead's own inner half is available.
    const auto destinations = [wall_generator](bool wipe_inward, const char *distance) {
        DynamicPrintConfig config = wipe_config(
            wall_generator, wipe_inward, distance, "10%", false, "2", "outer wall/inner wall");
        // Wipe after every outer wall, including the short travel to its inner wall.
        config.set_deserialize_strict({{"retraction_minimum_travel", "0"}});
        return wipe_destinations(slice({make_cube(10., 10., 1.)}, config));
    };
    const std::vector<Vec2d> regular = destinations(false, "50%");
    const std::vector<Vec2d> half_width = destinations(true, "50%");

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(half_width.empty());
    REQUIRE(trajectories_differ(regular, half_width));
    CHECK_FALSE(trajectories_differ(half_width, destinations(true, "100%")));
}

TEST_CASE("Wipe on loops preserves the corner move with inward wipe disabled", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *nozzle_diameter = GENERATE("0.4", "0.8");
    const char *comments = GENERATE("0", "1");
    CAPTURE(comments);
    INFO("wall generator: " << wall_generator << ", nozzle diameter: " << nozzle_diameter);
    // A closed square gives a 90-degree material-side corner at the seam.
    DynamicPrintConfig config = wipe_config(wall_generator, false, "50%", "0", true);
    config.set_deserialize_strict({{"nozzle_diameter", nozzle_diameter}, {"seam_position", "nearest"},
                                   {"gcode_comments", comments}});
    const std::string output = slice({make_cube(10., 10., 1.)}, config);
    const auto &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    const auto &wipe_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    ExtrusionRole role = erNone;
    std::vector<Vec2d> loop;
    bool after_extrusion = false;
    size_t moves = 0;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.comment().find(role_tag) == 0) {
            role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
            loop.clear();
            after_extrusion = false;
        }
        if (line.comment().find(wipe_tag) == 0)
            after_extrusion = false;
        if (role != erExternalPerimeter)
            return;
        if (line.extruding(self) && line.dist_XY(self) > EPSILON) {
            if (loop.empty())
                loop.emplace_back(self.x(), self.y());
            loop.emplace_back(line.new_X(self), line.new_Y(self));
            after_extrusion = true;
            return;
        }
        // The loop move is the first non-extruding XY move after the external
        // wall and before the reserved wipe marker, regardless of comment text.
        if (!after_extrusion || line.dist_XY(self) <= EPSILON)
            return;
        after_extrusion = false;

        ++moves;
        INFO("layer Z: " << self.z());
        REQUIRE(loop.size() >= 4);
        const Vec2d seam = loop.front();
        REQUIRE_THAT((loop.back() - seam).norm(), Catch::Matchers::WithinAbs(0., 0.003));
        const Vec2d outgoing = (loop[1] - seam).normalized();
        const Vec2d into_corner = (loop[loop.size() - 2] - seam).normalized();
        REQUIRE_THAT(outgoing.dot(into_corner), Catch::Matchers::WithinAbs(0., 0.01));
        const Vec2d move = Vec2d(line.new_X(self), line.new_Y(self)) - seam;
        // The legacy corner move is 20% of the nozzle diameter, turned 30 degrees
        // from the outgoing edge into the square. Check both components independently.
        const double distance = 0.2 * std::stod(nozzle_diameter);
        CHECK_THAT(move.dot(outgoing), Catch::Matchers::WithinAbs(distance * std::sqrt(3.) / 2., 0.003));
        CHECK_THAT(move.dot(into_corner), Catch::Matchers::WithinAbs(distance / 2., 0.003));
    });
    REQUIRE(moves == 5);
}

TEST_CASE("Inward wipe remains valid after wipe on loops moves the nozzle", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *comments = GENERATE("0", "1");
    CAPTURE(comments);
    INFO("wall generator: " << wall_generator);

    DynamicPrintConfig config = wipe_config(wall_generator, false, "50%", "10%", true);
    config.set_deserialize_strict({{"gcode_comments", comments}});
    const std::string loop_move = slice({make_cube(10., 10., 1.)}, config);
    config.set_deserialize_strict({{"wipe_inward", "1"}});
    const std::string combined = slice({make_cube(10., 10., 1.)}, config);
    config.set_deserialize_strict({{"wipe_on_loops", "0"}});
    const std::string inward_only = slice({make_cube(10., 10., 1.)}, config);

    for (const std::string *output : {&loop_move, &combined}) {
        INFO("wipe_inward: " << (output == &combined));
        std::map<double, std::vector<Vec2d>> loop_moves_by_layer;
        const auto &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
        const auto &wipe_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
        ExtrusionRole role = erNone;
        bool after_extrusion = false;
        GCodeReader parser;
        parser.apply_config(config);
        parser.parse_buffer(*output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.comment().find(role_tag) == 0) {
                role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
                after_extrusion = false;
            }
            if (line.comment().find(wipe_tag) == 0)
                after_extrusion = false;
            if (role != erExternalPerimeter || line.dist_XY(self) <= EPSILON)
                return;
            if (line.extruding(self)) {
                after_extrusion = true;
            } else if (after_extrusion) {
                loop_moves_by_layer[line.new_Z(self)].emplace_back(line.new_X(self), line.new_Y(self));
                after_extrusion = false;
            }
        });

        // The 1 mm cube at 0.2 mm layer height has one external loop on each of five layers.
        const auto trajectories = wipe_trajectories(*output);
        REQUIRE(loop_moves_by_layer.size() == 5);
        for (size_t layer = 1; layer <= 5; ++layer) {
            const double z = layer * 0.2;
            const auto moves = std::find_if(loop_moves_by_layer.begin(), loop_moves_by_layer.end(),
                [z](const auto &entry) { return std::abs(entry.first - z) < 0.001; });
            REQUIRE(moves != loop_moves_by_layer.end());
            REQUIRE(moves->second.size() == 1);
            const auto wipe = std::find_if(trajectories.begin(), trajectories.end(), [&](const WipeTrajectory &trajectory) {
                return std::abs(trajectory.z - z) < 0.001 &&
                       (trajectory.start - moves->second.front()).norm() < 0.001;
            });
            REQUIRE(wipe != trajectories.end());
            // The configured 2 mm wipe must be measured from the inward move's
            // endpoint, including when wipe_inward is off (set_last_pos regression).
            CHECK_THAT(trajectory_length(*wipe), Catch::Matchers::WithinAbs(2., 0.003));
        }
    }

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
