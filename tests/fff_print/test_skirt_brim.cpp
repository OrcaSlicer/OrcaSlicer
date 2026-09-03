#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Layer.hpp"

#include <boost/algorithm/string.hpp>

#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "test_helpers.hpp" // get access to init_print, etc

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

// Belt printers ---------------------------------------------------------------
//
// On a tilted belt the brim is laid onto the belt PLANE rather than into the Z=0
// bed plane, so it is spread across many layers instead of living on the first
// one.  The discriminating measurement is the number of contiguous brim runs in
// the G-code: a flat plate brim gives a single run, a belt brim gives one per
// layer that carries a band.  Distinct Z values are useless here, because the
// machine-frame transform couples Y into Z so every belt move has its own Z.
static DynamicPrintConfig belt_brim_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "belt_slice_rotation_global", 1 },
        { "gcode_remap_x",              "rev_x" },
        { "gcode_remap_y",              "pos_z" },
        { "gcode_remap_z",              "pos_y" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "top_shell_layers",           0 },
        { "bottom_shell_layers",        1 },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    return config;
}

// Same belt as belt_brim_config(), but with `filaments` distinct filaments so the brim's
// tool selection can be observed.  Kept separate from belt_brim_config() so the existing
// single-filament belt tests are untouched.
static DynamicPrintConfig belt_brim_multifilament_config(unsigned int filaments,
    std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = multifilament_config(filaments);
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "belt_slice_rotation_global", 1 },
        { "gcode_remap_x",              "rev_x" },
        { "gcode_remap_y",              "pos_z" },
        { "gcode_remap_z",              "pos_y" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "top_shell_layers",           0 },
        { "bottom_shell_layers",        1 },
        { "machine_start_gcode",        "T[initial_tool]\n" },
        { "layer_change_gcode",         "G92 E0\n" },
    });
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

// 0-based tool indices used by extrusions whose role comment contains `role` (needs
// gcode_comments).  Mirrors tools_for_role in test_multifilament.cpp; statics do not cross
// translation units, so it is repeated here.
static std::set<int> belt_tools_for_role(const std::string &gcode, const std::string &role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// Machine Z of the first extruding move whose role comment contains `role`, in file order;
// numeric_limits<double>::max() when the role never extrudes.
static double first_role_z(const std::string &gcode, const std::string &role)
{
    double z = std::numeric_limits<double>::max();
    GCodeReader parser;
    parser.parse_buffer(gcode, [&z, &role](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.comment().find(role) != std::string_view::npos) {
            z = self.z();
            self.quit_parsing();
        }
    });
    return z;
}

// Number of object layers that carry a belt brim band.  Each such band is emitted as one
// contiguous brim pass, so for a single object whose first-contact layer carries a band
// (the apron prologue folds into that layer's pass) this equals role_passes(gcode, "brim").
static int nonempty_belt_brim_layers(const PrintObject &object)
{
    int n = 0;
    for (const ExtrusionEntityCollection &band : object.belt_brim_by_layer())
        if (! band.empty())
            ++ n;
    return n;
}

// For each active tool, the ordinal (1-based, over extruding moves) of the FIRST move whose
// role comment contains `role`.  Lets a per-object ordering check key off the object's
// unique wall filament.
static std::map<int, long> first_move_by_tool(const std::string &gcode, const std::string &role)
{
    std::map<int, long> first;
    int  tool = 0;
    long idx  = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1])) {
            tool = std::stoi(cmd.substr(1));
            return;
        }
        if (! line.extruding(self))
            return;
        ++ idx;
        if (std::string(line.comment()).find(role) != std::string::npos && ! first.count(tool))
            first[tool] = idx;
    });
    return first;
}

// C - the band coincident with the object's FIRST contact with the belt must not be dropped:
// a belt brim has to appear at or below the object's first perimeter.  On the unfixed feature
// the first-contact band is dropped and the first brim then appears only at a later (higher)
// layer.  Machine Z is meaningful and shared between roles under the belt remap, so the first
// brim's Z must not exceed the first perimeter's.  Both with and without support.
TEST_CASE("Belt brim is laid at the object's first belt contact", "[SkirtBrim][belt]")
{
    const bool support = GENERATE(false, true);
    DYNAMIC_SECTION("enable_support=" << support) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "brim_type",           "outer_only" },
            { "brim_width",          4 },
            { "leading_brim_length", 0 },
            { "extra_brim_width",    0 },
            { "brim_object_gap",     0 },
            { "enable_support",      support ? 1 : 0 },
        });
        const std::string gcode = slice({ cube(20) }, config);

        const double brim_z = first_role_z(gcode, "brim");
        const double peri_z = first_role_z(gcode, "perimeter");
        REQUIRE(brim_z < std::numeric_limits<double>::max());
        REQUIRE(peri_z < std::numeric_limits<double>::max());
        CHECK(brim_z <= peri_z + EPSILON);
    }
}

// C control - when the band's own object layer has extrusion (any interior layer of a solid
// cube), the band takes the ordinary process_layer() path and must be drawn immediately
// before that layer's perimeters, and exactly once: never dropped, never double-emitted.
TEST_CASE("Belt brim on an object layer precedes its perimeters, once", "[SkirtBrim][belt]")
{
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",       "outer_only" },
        { "brim_width",      4 },
        { "brim_object_gap", 0 },
    });
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    const std::string gc = gcode(print);

    // Ordering: the first thing extruded is brim, then perimeter.
    const std::vector<std::string> seq = role_sequence(gc, { "brim", "perimeter" });
    REQUIRE(seq.size() >= 2);
    CHECK(seq[0] == "brim");
    CHECK(seq[1] == "perimeter");

    // Exactly once: every band is one contiguous pass (the apron prologue folds into the
    // first layer's), so the pass count equals the number of layers carrying a band - not
    // twice it, which double-emission would give, nor fewer, which a dropped band would.
    const int bands = nonempty_belt_brim_layers(*print.objects().front());
    REQUIRE(bands > 0);
    CHECK(role_passes(gc, "brim") == bands);
}

// B - single extruder (filament id 1).  Every band must survive the 1-based -> 0-based
// filament-id conversion the apron path performs: a wrong conversion drops all single-extruder
// bands, so the pass count would collapse.  The expected count is derived from the sliced
// layers, not a ratio.
TEST_CASE("Belt brim on a single extruder emits every band once", "[SkirtBrim][belt]")
{
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",       "outer_only" },
        { "brim_width",      4 },
        { "brim_object_gap", 0 },
    });
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    const std::string gc = gcode(print);

    const int expected = nonempty_belt_brim_layers(*print.objects().front());
    REQUIRE(expected > 0);
    CHECK(role_passes(gc, "brim") == expected);
    CHECK(belt_tools_for_role(gc, "brim") == std::set<int>{ 0 });   // filament 1 -> tool 0
}

// B - multi extruder (wall filament id 2).  Every belt-brim line must print on the object's
// wall filament (index 2 -> tool 1), and the total number of passes must equal the
// single-extruder baseline: no per-filament doubling.
TEST_CASE("Belt brim on a multi-extruder object uses the wall filament, no doubling", "[SkirtBrim][belt]")
{
    // Single-extruder baseline built the same way (same nozzle/flow), so the band geometry -
    // and thus the band count - is identical and only the filament assignment differs.
    DynamicPrintConfig base = belt_brim_multifilament_config(1, {
        { "brim_type",       "outer_only" },
        { "brim_width",      4 },
        { "brim_object_gap", 0 },
    });
    const int baseline = role_passes(slice({ cube(20) }, base), "brim");
    REQUIRE(baseline > 0);

    DynamicPrintConfig config = belt_brim_multifilament_config(2, {
        { "brim_type",              "outer_only" },
        { "brim_width",             4 },
        { "brim_object_gap",        0 },
        { "outer_wall_filament_id", 2 },
        { "inner_wall_filament_id", 2 },
    });
    const std::string gc = slice({ cube(20) }, config);

    CHECK(belt_tools_for_role(gc, "brim") == std::set<int>{ 1 });   // filament 2 -> tool 1
    CHECK(role_passes(gc, "brim") == baseline);
}

// B - two objects offset ALONG the belt (Y, since the tilt is about X), each with its own
// wall filament.  Each object's brim/apron must print on that object's filament AND before
// that object's own perimeters.  The object is identified by its unique tool.
TEST_CASE("Belt brim of each object precedes its perimeters on its own filament", "[SkirtBrim][belt]")
{
    DynamicPrintConfig config = belt_brim_multifilament_config(2, {
        { "brim_type",           "outer_only" },
        { "brim_width",          4 },
        { "leading_brim_length", 6 },
        { "brim_object_gap",     0 },
    });

    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(cube(20));
    TriangleMesh second = cube(20);
    second.translate(0.f, 40.f, 0.f);   // offset along the belt so it lands well after the first
    meshes.emplace_back(std::move(second));

    const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> overrides {
        { { "outer_wall_filament_id", 1 }, { "inner_wall_filament_id", 1 } },
        { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } },
    };
    Print print;
    Model model;
    init_print(std::move(meshes), print, model, config, &overrides, /*arrange=*/false);
    print.process();
    const std::string gc = gcode(print);

    // Both brims appear, each on its object's wall filament (1 -> T0, 2 -> T1).
    CHECK(belt_tools_for_role(gc, "brim") == std::set<int>{ 0, 1 });

    const std::map<int, long> brim_first = first_move_by_tool(gc, "brim");
    const std::map<int, long> peri_first = first_move_by_tool(gc, "perimeter");
    for (int tool : { 0, 1 }) {
        REQUIRE(brim_first.count(tool) == 1);
        REQUIRE(peri_first.count(tool) == 1);
        CHECK(brim_first.at(tool) < peri_first.at(tool));
    }
}

// D - the belt-brim predicate must not fire on a request that produces no belt brim.
// leading_brim_length / extra_brim_width only feed the OUTER ring, so inner_only with zero
// brim_width yields nothing and must not claim the layers the prime tower / spiral vase need.
TEST_CASE("Belt inner-only leading brim does not reject the prime tower or spiral vase", "[SkirtBrim][belt]")
{
    auto inner_leading = [](std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "brim_type",           "inner_only" },
            { "brim_width",          0 },
            { "leading_brim_length", 6 },
            { "brim_object_gap",     0 },
        });
        config.set_deserialize_strict(extra);
        return config;
    };
    auto init_inner_leading_with_prime_tower = [](Print &print, Model &model, double brim_width) {
        DynamicPrintConfig config = belt_brim_multifilament_config(2, {
            { "brim_type",                 "inner_only" },
            { "brim_width",                brim_width },
            { "leading_brim_length",       6 },
            { "brim_object_gap",           0 },
            { "enable_prime_tower",        1 },
        });
        const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> overrides {
            { { "extruder", 1 } }, { { "extruder", 2 } },
        };
        init_print({ cube(20), cube(20) }, print, model, config, &overrides);
    };

    SECTION("prime tower is left alone") {
        Print print;
        Model model;
        init_inner_leading_with_prime_tower(print, model, 0);
        CHECK_FALSE(print.objects().front()->has_belt_brim());
        CHECK(print.validate().string.empty());
    }
    SECTION("spiral vase is left alone") {
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, inner_leading({ { "spiral_mode", 1 } }));
        CHECK_FALSE(print.objects().front()->has_belt_brim());
        CHECK(print.validate().string.empty());
    }
    SECTION("a real inner brim still rejects the prime tower") {
        Print print;
        Model model;
        init_inner_leading_with_prime_tower(print, model, 4);
        CHECK(print.objects().front()->has_belt_brim());
        CHECK_FALSE(print.validate().string.empty());
    }
}

TEST_CASE("Belt brim spans many layers instead of one", "[SkirtBrim][belt]")
{
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",  "outer_only" },
        { "brim_width", 5 },
    });
    const std::string gcode = slice({ cube(20) }, config);
    // A plate-brim implementation would score 1 here.
    CHECK(role_passes(gcode, "brim") > 10);
}

TEST_CASE("Belt brim is absent when both widths are zero", "[SkirtBrim][belt]")
{
    // The "no effect when disabled" guard: brim_type Auto is the shipped default and
    // reports has_brim() even at width 0, so this also pins the gate that keeps the
    // flat plate brim from running on a tilted belt.
    const char *brim_type = GENERATE("auto_brim", "outer_only", "no_brim");
    DYNAMIC_SECTION("brim_type " << brim_type) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "brim_type",        brim_type },
            { "brim_width",       0 },
            { "leading_brim_length", 0 },
            { "extra_brim_width", 0 },
        });
        const std::string gcode = slice({ cube(20) }, config);
        CHECK(role_passes(gcode, "brim") == 0);
    }
}

TEST_CASE("Leading brim length alone produces a belt brim", "[SkirtBrim][belt]")
{
    // Exercises the leading_brim_length-only enablement path and the downhill sweep.
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",        "outer_only" },
        { "brim_width",       0 },
        { "leading_brim_length", 5 },
        { "brim_object_gap",  0 },
    });
    const std::string gcode = slice({ cube(20) }, config);
    CHECK(role_passes(gcode, "brim") > 0);
}

TEST_CASE("Leading brim length reaches further ahead of the object", "[SkirtBrim][belt]")
{
    // Compared between two runs rather than against an absolute coordinate, so the
    // assertion survives any change of origin or axis remap.
    auto brim_extent = [](double extra) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "brim_type",        "outer_only" },
            { "brim_width",       3 },
            { "leading_brim_length", extra },
            { "brim_object_gap",  0 },
        });
        const std::string gcode = slice({ cube(20) }, config);
        // The apron prints before the object reaches the belt, so it shows up as brim
        // extrusion at the lowest machine Z of any brim move.
        double min_z = std::numeric_limits<double>::max();
        GCodeReader parser;
        parser.parse_buffer(gcode, [&min_z](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.extruding(self) && line.comment().find("brim") != std::string_view::npos)
                min_z = std::min(min_z, static_cast<double>(self.z()));
        });
        return min_z;
    };
    const double without = brim_extent(0.);
    const double with    = brim_extent(10.);
    REQUIRE(without < std::numeric_limits<double>::max());
    REQUIRE(with    < std::numeric_limits<double>::max());
    CHECK(with < without);
}

TEST_CASE("Every brim type slices on a belt printer", "[SkirtBrim][belt]")
{
    // Auto / Mouse ear / Painted collapse to outer-only rather than crashing or
    // silently producing nothing.
    const char *brim_type = GENERATE("auto_brim", "brim_ears", "painted", "outer_only",
                                     "inner_only", "outer_and_inner", "no_brim");
    DYNAMIC_SECTION("brim_type " << brim_type) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "brim_type",  brim_type },
            { "brim_width", 5 },
        });
        const std::string gcode = slice({ cube(20) }, config);
        REQUIRE(! gcode.empty());
        if (std::string(brim_type) == "no_brim")
            CHECK(role_passes(gcode, "brim") == 0);
        else if (std::string(brim_type) != "inner_only")
            // A solid cube has no holes, so inner_only legitimately yields nothing.
            CHECK(role_passes(gcode, "brim") > 0);
    }
}

TEST_CASE("An untilted belt printer gets no brim", "[SkirtBrim][belt]")
{
    // Belt brim needs a tilt to have a belt plane to lie on, and the flat plate brim
    // cannot reach the G-code on any belt printer: it is emitted out of
    // skirt_brim_groups(), which _make_skirt() builds, and that returns early for every
    // belt printer.  So an untilted belt printer gets nothing - unchanged by this
    // feature.  Making the flat brim work here would mean reopening the belt skirt gate,
    // which is a separate change; Print::validate() warns instead.
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "belt_slice_rotation", "none" },
        { "brim_type",           "outer_only" },
        { "brim_width",          5 },
    });
    const std::string gcode = slice({ cube(20) }, config);
    CHECK(role_passes(gcode, "brim") == 0);
}

TEST_CASE("Belt brim does not resurrect the skirt", "[SkirtBrim][belt]")
{
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",   "outer_only" },
        { "brim_width",  5 },
        { "skirt_loops", 2 },
    });
    const std::string gcode = slice({ cube(20) }, config);
    CHECK(role_passes(gcode, "skirt") == 0);
}

TEST_CASE("Belt brim lines all have the same width", "[SkirtBrim][belt]")
{
    // Each brim line's extrusion volume comes from its nozzle-to-belt clearance.  Anchoring
    // every line to a fixed fraction of its own band gives them all the same clearance, so
    // they all come out the same width.  The nominal-spacing lattice this replaced let each
    // line land wherever it fell inside its band, so the clearance - and the width with it -
    // varied by 2x, which showed up as visibly ragged brim.
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",       "outer_only" },
        { "brim_width",      5 },
        { "brim_object_gap", 0 },
    });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    const PrintObject *obj = print.objects().front();

    std::vector<float> widths;
    auto collect = [&widths](const ExtrusionEntityCollection &coll) {
        for (const ExtrusionEntity *ee : coll.entities)
            if (const auto *path = dynamic_cast<const ExtrusionPath *>(ee))
                widths.push_back(path->width);
    };
    for (const ExtrusionEntityCollection &band : obj->belt_brim_by_layer())
        collect(band);
    for (const BeltBrimBand &band : obj->belt_brim_prologue())
        collect(band.fills);

    REQUIRE(widths.size() > 10);
    const float lo = *std::min_element(widths.begin(), widths.end());
    const float hi = *std::max_element(widths.begin(), widths.end());
    CHECK_THAT(hi, Catch::Matchers::WithinRel(lo, 1e-4));
}

TEST_CASE("Belt apron survives another object printing at the same Z", "[SkirtBrim][belt]")
{
    // An apron band prints below its OWN object's first layer, but with two objects on the
    // belt the second one is already printing at that print_z.  The layer then has an
    // object layer and takes the ordinary process_layer() path rather than the brim-only
    // branch, so the band must be emitted from both or it is silently dropped.  A
    // single-object print cannot exercise this.
    auto brim_passes = [](int object_count) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "brim_type",           "outer_only" },
            { "brim_width",          3 },
            { "leading_brim_length", 8 },
            { "brim_object_gap",     0 },
        });
        std::vector<TriangleMesh> meshes;
        for (int i = 0; i < object_count; ++ i) {
            TriangleMesh m = cube(20);
            // Offset along the belt so the second object starts well after the first.
            m.translate(0.f, float(40 * i), 0.f);
            meshes.emplace_back(std::move(m));
        }
        Print print;
        Model model;
        init_print(std::move(meshes), print, model, config);
        print.process();
        return role_passes(gcode(print), "brim");
    };

    const int one = brim_passes(1);
    const int two = brim_passes(2);
    REQUIRE(one > 0);
    // Two identical objects should carry twice the brim.  Merely asserting `two > one`
    // would not be decisive: the FIRST object's apron survives the bug, because nothing
    // else is printing that early, so only the second object's apron goes missing.
    // Requiring close to 2x is what actually detects the dropped bands.
    CHECK(two >= 1.8 * one);
}

TEST_CASE("Belt brim allows instances placed across the belt", "[SkirtBrim][belt]")
{
    // Only movement ALONG the belt changes an instance's belt-floor Z, so copies placed
    // side by side ACROSS it share one set of bands and must still get a brim.  The first
    // version of this guard refused every multi-instance object outright, silently
    // dropping the brim.
    //
    // The global belt flags are off here so the instances stay in one PrintObject; with
    // them on, PrintApply splits each instance into its own object and the case cannot
    // arise at all.
    auto multi_instance_has_brim = [](double dx, double dy) {
        DynamicPrintConfig config = belt_brim_config();
        config.set_deserialize_strict({
            { "belt_slice_rotation_global", 0 },
            { "belt_preslice_global",       0 },
            { "preslice_remap_global",      0 },
            { "brim_type",                  "outer_only" },
            { "brim_width",                 4 },
            { "brim_object_gap",            0 },
        });
        Print  print;
        Model  model;
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(80., 80., 0.));
        object->add_instance()->set_offset(Vec3d(80. + dx, 80. + dy, 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
        REQUIRE(print.objects().size() == 1);
        REQUIRE(print.objects().front()->instances().size() == 2);
        return print.objects().front()->has_belt_brim();
    };

    // X is across the belt when the tilt is about X, since the shear then runs along Y.
    CHECK(multi_instance_has_brim(40., 0.));
    // Y is along the belt: the copies sit at different belt heights and would each need
    // their own bands, so the brim is refused (and validate() warns).
    CHECK_FALSE(multi_instance_has_brim(0., 40.));
}

TEST_CASE("Belt brim coexists with support material", "[SkirtBrim][belt]")
{
    // Supports put extra layers into the same z stream as the apron bands, which is what
    // the three-way merge in collect_layers_to_print() exists to handle: a band sharing a
    // print_z with a support layer of the SAME object used to overwrite it in the
    // print-wide merge.  A smoke test - it cannot prove the collision occurred - but it
    // does exercise the merge with all three streams populated.
    DynamicPrintConfig config = belt_brim_config();
    config.set_deserialize_strict({
        { "brim_type",           "outer_only" },
        { "brim_width",          4 },
        { "leading_brim_length", 6 },
        { "brim_object_gap",     0 },
        { "enable_support",      1 },
    });
    const std::string gc = slice({ TestMesh::overhang }, config);
    REQUIRE(! gc.empty());
    CHECK(role_passes(gc, "brim") > 0);
}
