#include <algorithm>
#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include <string>
#include "libslic3r/Model.hpp"
#include <vector>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include <string_view>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <boost/algorithm/string/predicate.hpp>
#include <initializer_list>
#include <utility>

#include "test_helpers.hpp" // get access to init_print, etc
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/BeltBrim.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

// Distinct brim regions (combine_brims merges touching brims into one covering >1 object).
static int brim_count(const Print &print)
{
    int n = 0;
    for (const auto &group : print.skirt_brim_groups())
        n += (int) group.brims.size();
    return n;
}

// Total brim loops across all objects.
static size_t brim_loop_count(Print &print)
{
    size_t n = 0;
    for (const auto &kv : print.get_brimMap())
        n += kv.second.items_count();
    return n;
}

static bool brim_enters_first_layer_hole(Print &print)
{
    const PrintObject *object = print.get_object(0);
    Polygons holes;
    for (const ExPolygon &slice : object->layers().front()->lslices)
        holes.insert(holes.end(), slice.holes.begin(), slice.holes.end());

    const Vec3d plate_origin = print.get_plate_origin();
    Point shift = object->instances().front().shift_without_plate_offset();
    shift += Point(scaled(plate_origin.x()), scaled(plate_origin.y()));
    for (Polygon &hole : holes)
        hole.translate(shift);

    for (const auto &kv : print.get_brimMap()) {
        Polylines brim_paths;
        kv.second.collect_polylines(brim_paths);
        for (const Polyline &path : brim_paths)
            for (const Point &point : path.points)
                if (contains(holes, point, false))
                    return true;
    }
    return false;
}

// The span is skirt_height layers, or every layer when a draft shield is on (forced even at
// height 0); per-object skirts are rejected in By object printing (no room between objects).
TEST_CASE("Skirt is emitted once per layer it spans", "[SkirtBrim]")
{
    const int object_layers = 100; // 20mm cube at 0.2mm layers
    const char *skirt_type   = GENERATE("combined", "perobject");
    const char *print_seq    = GENERATE("by layer", "by object");
    const char *draft_shield = GENERATE("disabled", "enabled");
    const int   skirt_height = GENERATE(0, 1, 3);

    DYNAMIC_SECTION(skirt_type << " | " << print_seq << " | draft=" << draft_shield << " | height=" << skirt_height) {
        auto do_slice = [&] {
            return slice_two_cubes_arranged({
                { "skirt_loops",    1 },
                { "skirt_height",   skirt_height },
                { "skirt_distance", 3 },
                { "skirt_type",     skirt_type },
                { "draft_shield",   draft_shield },
                { "print_sequence", print_seq },
                { "layer_height",   0.2 },
            });
        };
        const bool draft = std::string(draft_shield) == "enabled";
        const bool has_skirt = draft || skirt_height > 0;
        const bool unsafe_by_object = std::string(skirt_type) == "perobject"
                                   && std::string(print_seq) == "by object" && has_skirt;

        if (unsafe_by_object) {
            REQUIRE_THROWS(do_slice());
        } else {
            const int expected_layers = draft ? object_layers : skirt_height;
            CHECK(role_passes(do_slice(), "skirt") == expected_layers);
        }
    }
}

// Each per-object skirt prints right before its own object, so distant objects yield two
// non-contiguous skirt passes; close objects group into a single skirt.
TEST_CASE("Per-object skirts group when objects are close", "[SkirtBrim]")
{
    auto [gap, expected_skirts] = GENERATE(table<double, int>({ { 5.0, 1 }, { 60.0, 2 } }));
    DYNAMIC_SECTION("gap=" << gap) {
        const std::string gcode = slice_two_cubes_apart(gap, {
            { "skirt_loops",    1 },
            { "skirt_height",   1 },
            { "skirt_distance", 3 },
            { "skirt_type",     "perobject" },
            { "print_sequence", "by layer" },
            { "layer_height",   0.2 },
        });
        CHECK(role_passes(gcode, "skirt") == expected_skirts);
    }
}

TEST_CASE("Per-object skirt is generated per instance", "[SkirtBrim]")
{
    Print print;
    Model model;
    place_two_cube_instances_apart(60, {
        { "skirt_type",     "perobject" },
        { "skirt_height",   1 },
        { "skirt_distance", 2 },
        { "skirt_loops",    1 },
        { "brim_type",      "no_brim" },
    }, print, model);
    print.process();

    REQUIRE(print.skirt_brim_groups().size() == 2);
    REQUIRE(print.skirt().items_count() == 2);
    for (const Print::SkirtBrimGroup &group : print.skirt_brim_groups()) {
        REQUIRE(group.instances.size() == 1);
        REQUIRE(group.instances.front().object_id == print.get_object(0)->id());
    }
}

TEST_CASE("Combine brims merges touching brims", "[SkirtBrim]")
{
    auto [gap, combine, expected_brims] = GENERATE(table<double, int, int>({
        { 5.0,  1, 1 },   // touching + combine -> one merged brim
        { 5.0,  0, 2 },   // touching, no combine -> separate
        { 60.0, 1, 2 },   // far apart -> nothing to merge
    }));
    DYNAMIC_SECTION("gap=" << gap << " combine_brims=" << combine) {
        Print print;
        Model model;
        place_two_cubes_apart(gap, {
            { "skirt_loops",    1 },
            { "skirt_height",   1 },
            { "skirt_distance", 3 },
            { "skirt_type",     "perobject" },
            { "print_sequence", "by layer" },
            { "brim_type",      "outer_only" },
            { "brim_width",     5 },
            { "combine_brims",  combine },
            { "layer_height",   0.2 },
        }, print, model);
        print.process();
        CHECK(brim_count(print) == expected_brims);
    }
}

TEST_CASE("Object brims are generated per instance", "[SkirtBrim]")
{
    Print print;
    Model model;
    place_two_cube_instances_apart(60, {
        { "skirt_loops",   0 },
        { "brim_type",     "outer_only" },
        { "brim_width",    5 },
        { "combine_brims", 0 },
    }, print, model);
    print.process();

    REQUIRE(print.skirt_brim_groups().size() == 1);
    REQUIRE(print.skirt_brim_groups().front().brims.size() == 2);
    for (const Print::SkirtBrimGroup::Brim &brim : print.skirt_brim_groups().front().brims) {
        REQUIRE(brim.instances.size() == 1);
        REQUIRE(brim.instances.front().object_id == print.get_object(0)->id());
    }
}

TEST_CASE("Brim groups the parts of an object by contact", "[SkirtBrim]")
{
    // 10 mm cubes along X as the parts of one object: A and B overlap, C and D touch, and E stands 0.5 mm clear of D.
    const std::vector<double> part_x{ 0., 8., 30., 40., 50.5 };
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "skirt_loops", 0 }, { "brim_type", "outer_only" }, { "brim_width", 3 }, { "layer_height", 0.2 } });
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config, nullptr, false);
    for (size_t i = 1; i < part_x.size(); ++ i) {
        TriangleMesh part = cube(10);
        part.translate(float(part_x[i]), 0.f, 0.f);
        model.objects.front()->add_volume(std::move(part), ModelVolumeType::MODEL_PART, false);
    }
    print.apply(model, config);
    print.process();

    const PrintObject &object = *print.objects().front();
    std::set<std::set<size_t>> groups;
    for (const groupedVolumeSlices &group : object.firstLayerObjGroups()) {
        std::set<size_t> parts;
        for (const ObjectID &volume_id : group.volume_ids)
            for (size_t i = 0; i < object.model_object()->volumes.size(); ++ i)
                if (object.model_object()->volumes[i]->id() == volume_id)
                    parts.insert(i);
        groups.insert(parts);
    }
    CHECK(groups == std::set<std::set<size_t>>{ { 0, 1 }, { 2, 3 }, { 4 } });
}

TEST_CASE("Uncombined neighboring brims precede their respective objects", "[SkirtBrim]")
{
    Print print;
    Model model;
    place_two_cubes_apart(0, {
        { "skirt_loops",   0 },
        { "brim_type",     "outer_only" },
        { "brim_width",    5 },
        { "combine_brims", 0 },
    }, print, model);
    print.process();

    REQUIRE(print.skirt_brim_groups().size() == 1);
    REQUIRE(print.skirt_brim_groups().front().brims.size() == 2);
    CHECK(role_sequence(gcode(print), { "brim", "perimeter" }) ==
          std::vector<std::string>{ "brim", "perimeter", "brim", "perimeter" });
}

TEST_CASE("Combine brims merges neighboring object instances", "[SkirtBrim]")
{
    Print print;
    Model model;
    place_two_cube_instances_apart(5, {
        { "skirt_loops",   0 },
        { "brim_type",     "outer_only" },
        { "brim_width",    5 },
        { "combine_brims", 1 },
    }, print, model);
    print.process();

    REQUIRE(print.skirt_brim_groups().size() == 1);
    REQUIRE(print.skirt_brim_groups().front().brims.size() == 1);
    REQUIRE(print.skirt_brim_groups().front().brims.front().instances.size() == 2);
    const std::vector<std::string> expected{ "brim", "perimeter" };
    CHECK(role_sequence(gcode(print), { "brim", "perimeter" }) == expected);
}

// Each object's skirt and brim come right before that object, not all skirts then all brims first.
TEST_CASE("By-layer per-object skirt and brim precede each object", "[SkirtBrim]")
{
    const std::string gcode = slice_two_cubes_apart(60, { // far apart: a skirt+brim per object
        { "skirt_loops",    1 },
        { "skirt_height",   1 },
        { "skirt_distance", 3 },
        { "skirt_type",     "perobject" },
        { "print_sequence", "by layer" },
        { "brim_type",      "outer_only" },
        { "brim_width",     5 },
        { "layer_height",   0.2 },
    });
    const std::vector<std::string> expected{ "skirt", "brim", "perimeter", "skirt", "brim", "perimeter" };
    CHECK(role_sequence(gcode, { "skirt", "brim", "perimeter" }) == expected);
}

// A square's corners are 90 degrees, so they get ears only when brim_ears_max_angle is above 90.
TEST_CASE("Brim ears appear only at corners within the max angle", "[SkirtBrim]")
{
    auto [max_angle, expect_ears] = GENERATE(table<int, bool>({ { 91, true }, { 90, false }, { 89, false } }));
    DYNAMIC_SECTION("brim_ears_max_angle=" << max_angle) {
        Print print;
        init_and_process_print({ cube(20) }, print, {
            { "skirt_loops",              0 },
            { "brim_type",                "brim_ears" },
            { "brim_width",               1 },
            { "brim_ears_max_angle",      max_angle },
            { "initial_layer_line_width", 0.5 },
        });
        if (expect_ears) CHECK(brim_loop_count(print) > 0);
        else             CHECK(brim_loop_count(print) == 0);
    }
}

TEST_CASE("Outer-only brim ears stay out of model holes", "[SkirtBrim]")
{
    const bool outer_only = GENERATE(false, true);
    DYNAMIC_SECTION("brim_ears_outer_only=" << outer_only) {
        Print print;
        init_and_process_print({ TestMesh::cube_with_concave_hole }, print, {
            { "skirt_loops",                0 },
            { "brim_type",                  "brim_ears" },
            { "brim_width",                 2 },
            { "brim_ears_max_angle",        125 },
            { "brim_ears_detection_length", 0 },
            { "brim_ears_outer_only",       outer_only },
            { "initial_layer_line_width",   0.5 },
        });

        REQUIRE(brim_loop_count(print) > 0);
        CHECK(brim_enters_first_layer_hole(print) != outer_only);
    }
}

TEST_CASE("Painted brim ear radius controls sliced size", "[SkirtBrim]")
{
    constexpr double ear_radius = 10.0;

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",                0 },
        { "brim_type",                  "painted" },
        { "brim_width",                 15 },
        { "brim_object_gap",            0.1 },
        { "brim_ears_outer_only",       true },
        { "initial_layer_line_width",   0.5 },
    });

    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    print.process();

    const PrintObject *object = print.get_object(0);
    REQUIRE(!object->layers().front()->lslices.empty());
    const Point ear_center = object->layers().front()->lslices.front().contour.points.front();

    Transform3d model_transform = model.objects.front()->instances.front()->get_transformation().get_matrix_no_offset();
    const Point &center_offset = object->center_offset();
    model_transform = model_transform.pretranslate(
        Vec3d(-unscale<double>(center_offset.x()), -unscale<double>(center_offset.y()), 0));
    Vec3d model_pos = model_transform.inverse() *
        Vec3d(unscale<double>(ear_center.x()), unscale<double>(ear_center.y()), 0);
    model_pos.z() = model.objects.front()->raw_mesh_bounding_box().min.z() - 0.0001;
    model.objects.front()->brim_points = {
        BrimPoint(model_pos.cast<float>(), float(ear_radius)),
    };

    print.apply(model, config);
    print.process();

    const Vec3d plate_origin = print.get_plate_origin();
    Point path_center = ear_center + object->instances().front().shift_without_plate_offset();
    path_center += Point(scaled(plate_origin.x()), scaled(plate_origin.y()));

    double max_path_radius = 0.0;
    for (const auto &kv : print.get_brimMap()) {
        Polylines brim_paths;
        kv.second.collect_polylines(brim_paths);
        for (const Polyline &path : brim_paths)
            for (const Point &point : path.points)
                max_path_radius = std::max(max_path_radius, unscale<double>((point - path_center).cast<double>().norm()));
    }

    REQUIRE(max_path_radius > 0.0);
    INFO("Outermost painted-ear path radius: " << max_path_radius << " mm");
    CHECK(max_path_radius > ear_radius - 0.5);
    CHECK(max_path_radius < ear_radius);
}

TEST_CASE("Outer-only painted brim ears stay out of model holes", "[SkirtBrim]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",                0 },
        { "brim_type",                  "painted" },
        { "brim_ears_outer_only",       true },
        { "initial_layer_line_width",   0.5 },
    });

    Print print;
    Model model;
    init_print({ TestMesh::cube_with_concave_hole }, print, model, config);

    // Slice once to obtain exact outer and inner contour points in print
    // coordinates, then express them in the model coordinates painted ears store.
    print.process();
    const PrintObject *object = print.get_object(0);
    REQUIRE(!object->layers().front()->lslices.empty());
    REQUIRE(!object->layers().front()->lslices.front().holes.empty());

    Transform3d model_transform = model.objects.front()->instances.front()->get_transformation().get_matrix_no_offset();
    const Point &center_offset = object->center_offset();
    model_transform = model_transform.pretranslate(
        Vec3d(-unscale<double>(center_offset.x()), -unscale<double>(center_offset.y()), 0));
    const double bottom_z = model.objects.front()->raw_mesh_bounding_box().min.z() - 0.0001;
    auto painted_point = [&model_transform, bottom_z](const Point &point) {
        Vec3d model_pos = model_transform.inverse() *
            Vec3d(unscale<double>(point.x()), unscale<double>(point.y()), 0);
        model_pos.z() = bottom_z;
        return BrimPoint(model_pos.cast<float>(), 3.f);
    };

    const ExPolygon &first_slice = object->layers().front()->lslices.front();
    Polygon inner_contour = first_slice.holes.front();
    inner_contour.reverse();
    const Points inner_ear_points = inner_contour.concave_points(55. * PI / 180.);
    REQUIRE(!inner_ear_points.empty());
    model.objects.front()->brim_points = {
        painted_point(first_slice.contour.points.front()),
        painted_point(inner_ear_points.front()),
    };
    print.apply(model, config);
    print.process();

    REQUIRE(brim_loop_count(print) > 0);
    CHECK_FALSE(brim_enters_first_layer_hole(print));
}

SCENARIO("Skirt has the configured number of loops", "[SkirtBrim]") {
    GIVEN("20mm cube and default config") {
        WHEN("skirt_loops is set to 2")  {
            Print print;
            init_and_process_print({cube(20)}, print, {
                { "skirt_height",   1 },
                { "skirt_distance", 1 },
                { "skirt_loops",    2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Brim has the configured number of loops", "[SkirtBrim]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 6mm")  {
	        Print print;
	        init_and_process_print({cube(20)}, print, {
                    { "brim_type",                "outer_only" },
                    { "initial_layer_line_width", 1 },
                    { "brim_width",               6 }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                REQUIRE(brim_loop_count(print) == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Print print;
	        init_and_process_print({cube(20)}, print, {
                    { "brim_type",                "outer_only" },
                    { "brim_width",               6 },
                    { "initial_layer_line_width", 0.5 }
	        });
            THEN("Brim Extrusion collection has 12 loops in it") {
                REQUIRE(brim_loop_count(print) == 12);
            }
        }
    }
}

static double first_extrusion_feedrate_for_feature(const std::string &gcode, const std::string_view feature)
{
    double feedrate = 0.0;
    bool feature_active = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&feedrate, &feature_active, feature] (GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            feature_active = comment.find(feature) != std::string_view::npos;

        if (feature_active && line.extruding(self) && line.dist_XY(self) > 0) {
            feedrate = line.new_F(self);
            self.quit_parsing();
        }
    });
    return feedrate;
}

TEST_CASE("Skirt height is honored", "[SkirtBrim]") {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",  1 },
        { "skirt_height", 5 },
        { "wall_loops",   0 },
    });

    std::string gcode;
    SECTION("printing a single object") {
        gcode = slice({ cube(20) }, config);
    }
    SECTION("printing multiple objects") {
        gcode = slice({ cube(20), cube(20) }, config);
    }

    REQUIRE(layers_with_role(gcode, "skirt").size() == (size_t) config.opt_int("skirt_height"));
}

TEST_CASE("Brim uses first layer speed", "[SkirtBrim]") {
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "brim_type",                    "outer_only" },
        { "brim_width",                   5 },
        { "gcode_comments",               true },
        { "initial_layer_speed",          10 },
        { "initial_layer_infill_speed",   20 },
        { "machine_start_gcode",          "" },
        { "skirt_loops",                  0 },
        { "slow_down_for_layer_cooling",  false },
        { "z_hop",                        0 }
    });

    const std::string gcode = Slic3r::Test::slice({cube(20)}, config);

    const double brim_feedrate = first_extrusion_feedrate_for_feature(gcode, "Brim");
    REQUIRE(brim_feedrate > 0.0);
    REQUIRE_THAT(brim_feedrate, Catch::Matchers::WithinAbs(600.0, 1e-3));

    const double bottom_surface_feedrate = first_extrusion_feedrate_for_feature(gcode, "Bottom surface");
    REQUIRE(bottom_surface_feedrate > 0.0);
    REQUIRE_THAT(bottom_surface_feedrate, Catch::Matchers::WithinAbs(1200.0, 1e-3));
}

SCENARIO("Skirt and brim generation", "[SkirtBrim]") {
    GIVEN("A default configuration") {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(4);
        config.set_deserialize_strict({
            { "initial_layer_print_height", 0.3 },
            // avoid altering speeds unexpectedly
            { "slow_down_for_layer_cooling", false },
            { "initial_layer_speed",         "100%" },
            // remove noise from top/solid layers
            { "top_shell_layers",    0 },
            { "bottom_shell_layers", 1 },
            { "machine_start_gcode", "T[initial_tool]\n" },
        });

        WHEN("Brim width is set to 5") {
            config.set_deserialize_strict({
                { "wall_loops",  0 },
                { "skirt_loops", 0 },
                { "brim_type",   "outer_only" },
                { "brim_width",  5 },
            });
            THEN("Brim is generated") {
                std::string gcode = slice({ cube(20) }, config);
                REQUIRE(! layers_with_role(gcode, "brim").empty());
            }
        }

        WHEN("brim width to 1 with layer_width of 0.5") {
            config.set_deserialize_strict({
                { "skirt_loops",              0 },
                { "initial_layer_line_width", 0.5 },
                { "brim_type",                "outer_only" },
                { "brim_width",               1 },
            });
            THEN("2 brim lines") {
                Print print;
                init_and_process_print({ cube(20) }, print, config);
                REQUIRE(brim_loop_count(print) == 2);
            }
        }

        WHEN("Object is plated with overhang support and a brim") {
            config.set_deserialize_strict({
                { "layer_height",               0.4 },
                { "initial_layer_print_height", 0.4 },
                { "skirt_loops",                1 },
                { "skirt_distance",             0 },
                { "enable_support",             1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            });
            THEN("Support and brim are both emitted") {
                std::string gcode = slice({ TestMesh::overhang }, config);
                REQUIRE(! layers_with_role(gcode, "support").empty());
                REQUIRE(! layers_with_role(gcode, "brim").empty());
            }
        }

        WHEN("an object with support is surrounded by a skirt") {
            config.set_deserialize_strict({
                { "enable_support", 1 },
                { "skirt_loops",    1 },
                { "skirt_distance", 2 },
                { "brim_type",      "no_brim" },
                { "z_hop",          0 },
            });
            THEN("the skirt is long enough to enclose the object and its support") {
                std::string gcode = slice({ TestMesh::overhang }, config);
                const double first_layer_z = config.opt_float("initial_layer_print_height");

                // On the first layer, accumulate the skirt loop length and collect the
                // object + support extrusion points; the skirt must enclose them.
                double skirt_length = 0.0;
                Points footprint;
                GCodeReader parser;
                parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
                    if (! line.extruding(self) || line.dist_XY(self) <= 0 || std::abs(self.z() - first_layer_z) > 0.01)
                        return;
                    if (line.comment().find("skirt") != std::string_view::npos)
                        skirt_length += line.dist_XY(self);
                    else
                        footprint.push_back(Point::new_scale(line.new_X(self), line.new_Y(self)));
                });

                const double hull_perimeter = unscale<double>(Geometry::convex_hull(footprint).split_at_first_point().length());
                REQUIRE(hull_perimeter > 0.0); // guard against an empty footprint passing trivially
                REQUIRE(skirt_length > hull_perimeter);
            }
        }

        WHEN("Large minimum skirt length is used.") {
            // One skirt loop around a 20mm cube is ~88mm, so 500mm forces extra loops.
            config.set_deserialize_strict({
                { "skirt_loops",      1 },
                { "min_skirt_length", 500 },
            });
            THEN("The skirt is extended to at least the minimum length") {
                std::string gcode = slice({ cube(20) }, config);
                double skirt_length = 0.0;
                GCodeReader parser;
                parser.parse_buffer(gcode, [&skirt_length](GCodeReader &self, const GCodeReader::GCodeLine &line) {
                    if (line.extruding(self) && line.comment().find("skirt") != std::string_view::npos)
                        skirt_length += line.dist_XY(self);
                });
                REQUIRE(skirt_length >= 500.0);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Declarative cut planes: a segment that does not rest on the build plate must neither receive a
// brim nor disturb anyone else's.
// ---------------------------------------------------------------------------------------------

// A post carrying a wide cap, as one mesh. Object-local: the post fills [0,10] x [0,10] x [0,15],
// the cap [-15,25] x [2,8] x [15,20]. Cutting at z = 15 makes the cap a NON-ROOT segment that still
// builds along +Z, so its slice frame is exactly the identity and the tilted-frame test cannot see
// it. The cap's X overhang is the whole point: it reaches far enough past the post to cover the
// post's entire brim collar on the -X and +X sides. With a plain cube (the fixture in
// test_cut_plane_apply.cpp) the upper segment's cross-section matches the root's footprint exactly,
// so subtracting it removes only what the root's own island already removed -- net effect zero,
// which is why that fixture never caught this.
static TriangleMesh post_and_cap_mesh()
{
    TriangleMesh mesh = make_cube(10., 10., 15.);
    TriangleMesh cap  = make_cube(40., 6., 5.);
    cap.translate(-15.f, 2.f, 15.f);
    mesh.merge(cap);
    return mesh;
}

static void make_post_and_cap_model(Model &model, double cut_z)
{
    ModelObject *object = model.add_object();
    object->name = "post_and_cap";
    object->add_volume(post_and_cap_mesh());
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));

    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane plane;
    plane.point  = Vec3d(5., 5., cut_z); // on the post's axis, at the joint
    plane.normal = Vec3d::UnitZ();
    stack.planes.push_back(plane);
    object->cut_planes = stack;
    object->ensure_on_bed();
}

static DynamicPrintConfig cutplane_brim_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",   "1" },
        // Print::validate() refuses cut-plane segments in any other print sequence.
        { "print_sequence",     "by group" },
        // Unrelated validate() requirement of the stock test config (relative E addressing).
        { "layer_change_gcode", "G92 E0" },
        { "brim_type",          "outer_only" },
        { "brim_width",         "5" },
        { "brim_object_gap",    "0.1" },
        { "skirt_loops",        "0" },
    });
    return config;
}

// Fraction of azimuth bins around `centre` holding brim. Sampled ALONG each segment rather than at
// its vertices: an offset collar around a rectangular footprint carries only two vertices per
// straight edge, so a vertex-only metric would report a perfectly closed ring as full of holes.
// Deliberately not an area or a byte comparison -- brim width is quantised to an even number of
// flow spacings, so any absolute area is a moving target, while "the collar closes" is the property
// under test.
static double brim_azimuth_coverage(Print &print, ObjectID object_id, const Point &centre, int bins = 72)
{
    const auto it = print.get_brimMap().find(object_id);
    if (it == print.get_brimMap().end())
        return 0.;
    Polylines paths;
    it->second.collect_polylines(paths);

    std::vector<bool> hit(size_t(bins), false);
    auto mark = [&hit, bins, &centre](const Point &p) {
        const Vec2d d(unscale<double>(p.x() - centre.x()), unscale<double>(p.y() - centre.y()));
        if (d.norm() < EPSILON)
            return;
        const int b = int(std::floor((std::atan2(d.y(), d.x()) + PI) / (2. * PI) * double(bins)));
        hit[size_t(std::min(std::max(b, 0), bins - 1))] = true;
    };

    const double step = scaled<double>(0.2);
    for (const Polyline &path : paths)
        for (size_t i = 0; i + 1 < path.points.size(); ++ i) {
            const Vec2d a  = path.points[i].cast<double>();
            const Vec2d ab = path.points[i + 1].cast<double>() - a;
            const int   n  = std::max(1, int(std::ceil(ab.norm() / step)));
            for (int k = 0; k <= n; ++ k) {
                const Vec2d s = a + ab * (double(k) / double(n));
                mark(Point(coord_t(s.x()), coord_t(s.y())));
            }
        }
    return double(std::count(hit.begin(), hit.end(), true)) / double(bins);
}

// The first layer's centre in the frame the brim lives in: slice coordinates plus the instance
// shift plus the plate origin -- the same mapping brim_enters_first_layer_hole() uses above.
static Point first_layer_centre_on_plate(const Print &print, const PrintObject &object)
{
    BoundingBox bb;
    for (const ExPolygon &ex : object.layers().front()->lslices)
        bb.merge(ex.contour.bounding_box());
    const Vec3d plate_origin = print.get_plate_origin();
    return bb.center() + object.instances().front().shift_without_plate_offset()
         + Point(scaled(plate_origin.x()), scaled(plate_origin.y()));
}

SCENARIO("A cut-plane segment off the build plate must not carve the root's brim", "[cutplanes][brim][SkirtBrim]") {
    GIVEN("a post with a wide cap, split by one horizontal cut plane at the joint") {
        Print print;
        Model model;
        make_post_and_cap_model(model, 15.);
        print.apply(model, cutplane_brim_config());

        const auto validation = print.validate();
        INFO("validate: " << validation.string);
        REQUIRE(validation.string.empty());
        print.process();

        REQUIRE(print.objects().size() == 2);
        const PrintObject *root = nullptr, *upper = nullptr;
        for (const PrintObject *object : print.objects())
            (object->is_root_segment() ? root : upper) = object;
        REQUIRE(root  != nullptr);
        REQUIRE(upper != nullptr);
        REQUIRE(! root->layers().empty());
        REQUIRE(! upper->layers().empty());

        THEN("the upper segment builds straight up, so the tilted-frame test cannot exclude it") {
            // Without this the scenario would silently degrade into a re-test of the tilt guard:
            // a TILTED upper segment is caught by the older post_trafo_inverse() check and the
            // brim would come out whole even with the bug present.
            CHECK(upper->post_trafo_inverse().linear().isApprox(Matrix3d::Identity()));
        }

        THEN("the upper segment gets no brim of its own") {
            // It floats 15 mm above the plate. PrintObject::has_brim() has always said so; this
            // pins that Brim.cpp agrees, instead of computing a brim that only _make_skirt's root
            // gate happens to stop from ever being emitted.
            CHECK(! upper->has_brim());
            CHECK(print.get_brimMap().count(upper->id()) == 0);
        }

        THEN("the root keeps a complete brim collar") {
            // The defect: the cap's first-layer cross-section -- physically at z = 15 -- was
            // inflated by brim_object_gap into the SHARED no_brim_area and then subtracted from
            // every object's brim, cutting the post's collar into disconnected arcs.
            REQUIRE(print.get_brimMap().count(root->id()) == 1);
            const double coverage = brim_azimuth_coverage(print, root->id(),
                                                          first_layer_centre_on_plate(print, *root));
            INFO("brim azimuth coverage " << coverage);
            CHECK(coverage == 1.);
        }
    }
}

// A tilted cut-plane segment that lands on the plate: a 20 mm cube at (120, 120) whose steep plane
// (70 degrees from vertical) meets the plate along object-local x = 14. The root keeps the plate
// strip x < 134; the tilted far piece prints later, after the table swings, and lands on x > 134.
// The root's brim used to be laid straight across that strip; it must leave it free and wrap it.
TEST_CASE("The root brim wraps a tilted segment's plate footprint instead of covering it", "[SkirtBrim]")
{
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
    object->add_instance()->set_offset(Vec3d(120., 120., 0.));
    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane cp;
    cp.point  = Vec3d(14., 10., 0.);
    cp.normal = Vec3d(std::cos(Geometry::deg2rad(20.)), 0., std::sin(Geometry::deg2rad(20.)));
    stack.planes.push_back(cp);
    object->cut_planes = stack;

// On a shallow belt (25 degrees, 0.3 mm layers) the brim band on the belt is wider than one
// bead, so its lines go on the nominal lattice instead of at one fixed clearance. A lattice
// line can then land where the belt is almost at the band's print_z; it must be moved to the
// clearance of the object's outer wall, where every line of a steep belt sits, not laid
// scraping the belt with its flow clamped to half a layer.
TEST_CASE("Belt brim lattice lines keep their clearance above the belt", "[SkirtBrim][belt]")
{
    const double layer_height = 0.3;
    const double wall_width   = 0.42;
    const double tilt_deg     = 25.;
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "belt_slice_rotation_angle",  tilt_deg },
        { "layer_height",               layer_height },
        { "initial_layer_print_height", layer_height },
        { "outer_wall_line_width",      wall_width },
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",           "1" },
        { "print_sequence",             "by group" },
        { "layer_change_gcode",         "G92 E0" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "brim_type",                  "outer_only" },
        { "brim_width",                 3 },
        { "brim_object_gap",            0 },
    });
    const std::string gcode = slice({ cube(20) }, config);
    // Along slice Z: half a layer down to where the slice meets the belt, then half a wall
    // width inwards, where the belt lies lower by that times tan(tilt).
    const double wall_clearance = 0.5 * layer_height + 0.5 * wall_width * std::tan(Geometry::deg2rad(tilt_deg));

    // Heights of the brim extrusions, from the ;HEIGHT: tags inside ;TYPE:Brim sections.
    std::vector<double> brim_heights;
    bool                in_brim = false;
    std::istringstream  lines(gcode);
    for (std::string line; std::getline(lines, line); ) {
        if (boost::starts_with(line, ";TYPE:"))
            in_brim = boost::starts_with(line, ";TYPE:Brim");
        else if (in_brim && boost::starts_with(line, ";HEIGHT:"))
            brim_heights.push_back(std::stod(line.substr(8)));
    }
    REQUIRE(! brim_heights.empty());
    // Lattice lines have clearances of their own; a single clearance would mean the band
    // took the steep-belt branch and the lattice went untested.
    std::set<long> distinct_heights;
    for (const double h : brim_heights)
        distinct_heights.insert(std::lround(h * 1000.));
    REQUIRE(distinct_heights.size() > 1);
    for (const double h : brim_heights) {
        CHECK(h >= wall_clearance - 1e-3);
        CHECK(h <= layer_height + 1e-3);
    }
}

// Lowest gantry Y of the `type` extrusions on each layer, keyed by machine Z, which is the
// belt position and therefore one value per tilted layer.  Under the belt remap the gantry is
// the height above the belt, so for a role resting on the belt this is its nozzle clearance.
static std::map<long, double> belt_layer_lowest_y(const std::string &gcode, const std::string &type)
{
    std::map<long, double> lowest;
    bool                   in_type = false;
    GCodeReader            reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (boost::starts_with(line.raw(), ";TYPE:")) {
            in_type = line.raw().substr(6) == type;
            return;
        }
        if (! in_type || ! line.extruding(self) || line.dist_XY(self) <= EPSILON)
            return;
        const double y = std::min(self.y(), line.new_Y(self));
        auto [it, inserted] = lowest.emplace(std::lround(line.new_Z(self) * 1000.), y);
        if (! inserted)
            it->second = std::min(it->second, y);
    });
    return lowest;
}

// The brim is the first thing laid on the belt, so it is what a user levels the belt height
// against (endstop, extruder offset).  Its lines must then sit exactly as high above the belt
// as the object's outer wall does where it rests on the belt, or a belt levelled for the brim
// lays the part too high to stick.
TEST_CASE("Belt brim sits as high above the belt as the object's outer wall", "[SkirtBrim][belt][Regression]")
{
    const double layer_height = GENERATE(0.2, 0.32);
    // A wider first-layer line widens only the walls of object layer 0, so it must not lift
    // the brim off the walls of the layers that rest on the belt.  0 keeps the outer wall's.
    const double first_width  = GENERATE(0., 0.6);
    // XY contour compensation moves the slice's edge, and with it the wall, toward or away
    // from the belt, also by more than half the wall's width.  Shrinking it by half a layer or
    // more lifts it off the belt's contact band, and the object gets no brim.
    const double compensation = GENERATE(0., 0.05, -0.05, 0.25);
    DYNAMIC_SECTION("layer_height=" << layer_height << " initial_layer_line_width=" << first_width
                    << " xy_contour_compensation=" << compensation) {
        const double wall_width = 0.4;
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "layer_height",               layer_height },
            { "initial_layer_print_height", layer_height },
            { "outer_wall_line_width",      wall_width },
            { "initial_layer_line_width",   first_width },
            { "xy_contour_compensation",    compensation },
            { "print_flow_ratio",           1 },
            { "brim_flow_ratio",            1 },
            { "filament_diameter",          "1.75" },
            { "brim_type",                  "outer_only" },
            { "brim_width",                 4 },
            { "brim_object_gap",            0 },
        });
        const std::string gcode = slice({ cube(20) }, config);
        const std::map<long, double> brim = belt_layer_lowest_y(gcode, "Brim");
        const std::map<long, double> wall = belt_layer_lowest_y(gcode, "Outer wall");
        REQUIRE(! brim.empty());
        REQUIRE(! wall.empty());

        // The wall's clearance on the layers it rests on the belt, i.e. the most common lowest
        // Y.  Not the lowest one: the sliver at the first contact can carry a narrower wall,
        // which sits lower; and layers past the part's bottom face do not reach the belt.
        std::map<long, int> votes;
        for (const auto &[z, y] : wall)
            ++ votes[std::lround(y * 1000.)];
        const auto   typical = std::max_element(votes.begin(), votes.end(),
                                                [](const auto &a, const auto &b) { return a.second < b.second; });
        const double wall_y  = 0.001 * typical->first;
        for (const auto &[z, y] : brim)
            CHECK_THAT(y, Catch::Matchers::WithinAbs(wall_y, 0.005));

        // Each brim line fills the cell between it and the next, one band apart on the belt
        // (the layer height / sin(45 deg)) and as tall as its clearance (its ;HEIGHT:), also
        // where the clearance is small.
        const double filament_area = 0.25 * PI * 1.75 * 1.75;
        const double pitch         = layer_height * std::sqrt(2.);
        size_t       long_moves    = 0;
        double       clearance     = 0.;
        bool         in_brim       = false;
        GCodeReader  reader;
        reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (boost::starts_with(line.raw(), ";TYPE:"))
                in_brim = line.raw().substr(6) == "Brim";
            else if (boost::starts_with(line.raw(), ";HEIGHT:"))
                clearance = std::stod(line.raw().substr(8));
            else if (in_brim && line.extruding(self) && line.dist_XY(self) >= 1.) {
                ++ long_moves;
                CHECK_THAT(line.dist_E(self) / line.dist_XY(self) * filament_area,
                           Catch::Matchers::WithinRel(pitch * clearance, 0.01));
            }
        });
        CHECK(long_moves > 20);
    }
}

// A modifier that gives the part a narrower outer wall away from the belt shares the tilted
// layers with the wall that rests on the belt.  The brim must be levelled with the wall that
// rests on the belt, not with the narrowest wall in the layer.
TEST_CASE("Belt brim follows the wall that rests on the belt rather than a modifier's", "[SkirtBrim][belt][Regression]")
{
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "gcode_comments",        1 },
        { "outer_wall_line_width", 0.6 },
        { "brim_type",             "outer_only" },
        { "brim_width",            4 },
        { "brim_object_gap",       0 },
    });
    // The top half of the cube, 10 mm and more above the belt, gets a 0.4 mm outer wall.
    DynamicPrintConfig modifier_config;
    modifier_config.set_deserialize_strict({ { "outer_wall_line_width", 0.4 } });
    TriangleMesh modifier = make_cube(22., 22., 11.);
    modifier.translate(-1.f, -1.f, 10.f);
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config, nullptr, false);
    model.objects.front()->add_volume(std::move(modifier), ModelVolumeType::PARAMETER_MODIFIER, false)->config.apply(modifier_config);
    print.apply(model, config);
    const std::string out = gcode(print);

    // Both walls print.
    std::set<long> wall_widths;
    bool           in_wall = false;
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line); ) {
        if (boost::starts_with(line, ";TYPE:"))
            in_wall = line.substr(6) == "Outer wall";
        else if (in_wall && boost::starts_with(line, ";WIDTH:"))
            wall_widths.insert(std::lround(std::stod(line.substr(7)) * 100.));
    }
    REQUIRE(wall_widths.count(40) == 1);
    REQUIRE(wall_widths.count(60) == 1);

    const std::map<long, double> brim = belt_layer_lowest_y(out, "Brim");
    const std::map<long, double> wall = belt_layer_lowest_y(out, "Outer wall");
    REQUIRE(! brim.empty());
    std::map<long, int> votes;
    for (const auto &[z, y] : wall)
        ++ votes[std::lround(y * 1000.)];
    const auto   typical = std::max_element(votes.begin(), votes.end(),
                                            [](const auto &a, const auto &b) { return a.second < b.second; });
    const double wall_y  = 0.001 * typical->first;
    // Half a 0.2 mm layer plus half the 0.6 mm wall, x tan(45 deg).
    CHECK_THAT(wall_y, Catch::Matchers::WithinAbs(0.1 + 0.3, 0.005));
    for (const auto &[z, y] : brim)
        CHECK_THAT(y, Catch::Matchers::WithinAbs(wall_y, 0.005));
}

// The brim rests on the belt, so it prints, and is reached, with first-layer settings, even
// though raising it to the outer wall's clearance puts the nozzle further above the belt than
// one first layer.  With thin layers its lines are also far narrower than a bead, as narrow as
// the bands are far apart, so their width must not decide it either.
TEST_CASE("Belt brim lines print with first-layer settings", "[SkirtBrim][belt][Regression]")
{
    const double layer_height = GENERATE(0.2, 0.08);
    CAPTURE(layer_height);
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "layer_height",                  layer_height },
        { "initial_layer_print_height",    0.2 },
        { "outer_wall_line_width",         0.45 },
        { "brim_type",                     "outer_only" },
        { "brim_width",                    4 },
        { "brim_object_gap",               0 },
        { "z_hop",                         0 },
        { "travel_speed",                  200 },
        { "initial_layer_travel_speed",    50 },
        { "initial_layer_speed",           15 },
        { "filament_max_volumetric_speed", 20 },
        { "slow_down_for_layer_cooling",   0 },
        // The fan is on everywhere but in the band above the belt.
        { "close_fan_the_first_x_layers",  1 },
        { "full_fan_speed_layer",          0 },
        { "fan_min_speed",                 100 },
        { "fan_max_speed",                 100 },
        { "fan_cooling_layer_time",        1000 },
        { "reduce_fan_stop_start_freq",    0 },
    });
    const std::string gcode = slice({ cube(20) }, config);

    // The feedrate of the last travel before each brim line, and the feedrate and fan of
    // each of its moves.
    size_t      brim_lines = 0, fast_travels = 0, brim_moves = 0, brim_fast = 0, brim_fan_on = 0, fan_on_moves = 0;
    std::string type;
    double      travel_f  = 0.;
    bool        travelled = false, fan_on = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (boost::starts_with(line.raw(), ";TYPE:")) {
            type = line.raw().substr(6);
            return;
        }
        if (line.cmd_is("M107")) {
            fan_on = false;
            return;
        }
        if (line.cmd_is("M106")) {
            const size_t s = line.raw().find('S');
            fan_on = s != std::string::npos && std::atof(line.raw().c_str() + s + 1) > 0.;
            return;
        }
        if (! line.cmd_is("G1") || line.dist_XY(self) <= EPSILON)
            return;
        if (! line.extruding(self)) {
            travel_f  = line.new_F(self);
            travelled = true;
            return;
        }
        fan_on_moves += fan_on;
        if (type == "Brim") {
            if (travelled) {
                ++ brim_lines;
                fast_travels += std::abs(travel_f - 50. * 60.) > 1.;
            }
            ++ brim_moves;
            brim_fast   += std::abs(line.new_F(self) - 15. * 60.) > 1.;
            brim_fan_on += fan_on;
        }
        travelled = false;
    });
    REQUIRE(brim_lines > 20);
    REQUIRE(fan_on_moves > 0);
    CHECK(fast_travels == 0);
    CHECK(brim_fast == 0);
    CHECK(brim_fan_on == 0);
}

// The brim prints in the object's outer wall filament even when every extrusion of the object
// is offered to purging (flush_into_objects): the tool ordering registers the brim filament
// itself, so the writer always knows it.
TEST_CASE("Belt brim slices when every object is a flush target", "[SkirtBrim][belt]")
{
    DynamicPrintConfig config = belt_brim_multifilament_config(2, {
        { "brim_type",          "outer_only" },
        { "brim_width",         4 },
        { "brim_object_gap",    0 },
        { "flush_into_objects", 1 },
        { "flush_into_infill",  1 },
    });
    const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> overrides {
        { { "extruder", 1 } }, { { "extruder", 2 } },
    print.apply(model, config);
    REQUIRE(print.objects().size() == 2);
    print.process();

    // Brim centrelines back in bed coordinates (the brim map carries the plate origin), sampled
    // every 0.25 mm: a brim loop round a box is four long segments, so its vertices alone would
    // miss almost every window below.
    const Vec3d plate_origin = print.get_plate_origin();
    const Point plate_shift(scaled(plate_origin.x()), scaled(plate_origin.y()));
    Points brim;
    for (const auto &kv : print.get_brimMap()) {
        Polylines paths;
        kv.second.collect_polylines(paths);
        for (const Polyline &path : paths)
            for (const Line &line : path.lines()) {
                const int steps = std::max(1, int(unscale<double>(line.length()) / 0.25));
                for (int i = 0; i <= steps; ++ i)
                    brim.push_back(line.a + ((line.b - line.a).cast<double>() * (double(i) / steps)).cast<coord_t>() - plate_shift);
            }
    }
    REQUIRE(! brim.empty());
    const auto count_in = [&brim](double x0, double x1, double y0, double y1) {
        return std::count_if(brim.begin(), brim.end(), [=](const Point &pt) {
            const double x = unscale<double>(pt.x()), y = unscale<double>(pt.y());
            return x > x0 && x < x1 && y > y0 && y < y1;
        });
    };
    // Nothing on the strip the tilted segment lands on...
    CHECK(count_in(134.5, 139.5, 120.5, 139.5) == 0);
    // ...the collar goes round its far side instead...
    CHECK(count_in(140.1, 143.5, 121., 139.) > 0);
    // ...and still round the root itself.
    CHECK(count_in(116.5, 119.9, 121., 139.) > 0);
}
