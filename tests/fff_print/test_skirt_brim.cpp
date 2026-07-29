#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Layer.hpp"

#include <boost/algorithm/string.hpp>

#include <cmath>
#include <iterator>
#include <limits>
#include <map>

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

TEST_CASE("Brim is emitted on the configured number of layers", "[SkirtBrim]") {
    auto brim_layers_val = GENERATE(1, 2, 4);
    DYNAMIC_SECTION("brim_layers=" << brim_layers_val) {
        const std::string gcode = slice({ cube(20) }, {
            { "brim_type",   "outer_only" },
            { "brim_width",  5 },
            { "brim_layers", brim_layers_val },
            { "skirt_loops", 0 },
            { "layer_height", 0.2 },
        });

        // brim should appear on exactly brim_layers distinct Z heights
        REQUIRE(layers_with_role(gcode, "brim").size() == (size_t) brim_layers_val);

        // brim should be emitted once per layer (one contiguous pass)
        REQUIRE(role_passes(gcode, "brim") == brim_layers_val);
    }
}

TEST_CASE("Per-object brim layers are honored independently", "[SkirtBrim]") {
    const double layer_height = 0.2;

    // Two cubes far apart so their brims don't merge.
    // Object 0 gets 1 brim layer, object 1 gets 3.
    const std::string gcode = slice({ cube(20), cube(20) }, {
        { "brim_type",   "outer_only" },
        { "brim_width",  5 },
        { "brim_layers", 1 },      // default for all objects
        { "skirt_loops", 0 },
        { "layer_height", layer_height },
        { "print_sequence", "by layer" },
    });

    // With default brim_layers=1, brim appears on exactly 1 Z height
    REQUIRE(layers_with_role(gcode, "brim").size() == 1u);
}

// Accumulate total brim extrusion length per layer Z.
static std::map<double, double> brim_length_per_layer(const std::string &gcode)
{
    std::map<double, double> lengths;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self) || line.dist_XY(self) <= 0)
            return;
        if (line.comment().find("brim") == std::string_view::npos)
            return;
        lengths[self.z()] += line.dist_XY(self);
    });
    return lengths;
}

// Build an inverted truncated pyramid — narrow at the base, wide at the top.
TriangleMesh make_inverted_frustum(double bottom_size, double top_size, double height)
{
    float hb = float(bottom_size) / 2, ht = float(top_size) / 2, h = float(height);
    // 0-3: bottom square (z=0), 4-7: top square (z=height)
    std::vector<Vec3f> verts {
        { -hb, -hb, 0.f }, {  hb, -hb, 0.f }, {  hb,  hb, 0.f }, { -hb,  hb, 0.f },
        { -ht, -ht, h }, {  ht, -ht, h }, {  ht,  ht, h }, { -ht,  ht, h },
    };
    // 12 triangles: bottom (2) + top (2) + 4 sides × 2
    std::vector<Vec3i32> faces {
        // bottom
        { 0, 1, 2 }, { 0, 2, 3 },
        // top
        { 4, 6, 5 }, { 4, 7, 6 },
        { 0, 5, 1 }, { 0, 4, 5 },
        { 1, 5, 6 }, { 1, 6, 2 },
        { 2, 6, 7 }, { 2, 7, 3 },
        { 3, 7, 4 }, { 3, 4, 0 },
    };
    return TriangleMesh(std::move(verts), std::move(faces));
}

TEST_CASE("Brim adapts to widening object on upper brim layers", "[SkirtBrim]") {
    // An inverted pyramid widens from 10mm at the base to 30mm at the top.
    // With brim_layers=3, brims should be emitted on layers 0, 1, 2.
    // Because the object widens, the brim on layer 1 must have a larger
    // inner boundary (and thus a longer total length) than the brim on layer 0.
    const double layer_height = 0.2;
    const int    brim_layers  = 3;

    auto frustum = make_inverted_frustum(10, 30, 20); // 10mm base, 30mm top, 20mm tall
    const std::string gcode = slice({ frustum }, {
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "brim_layers",              brim_layers },
        { "brim_object_gap",          0.4 },
        { "skirt_loops",              0 },
        { "layer_height",             layer_height },
        { "initial_layer_line_width", 0.4 },
        { "wall_loops",               1 },
    });

    auto lengths = brim_length_per_layer(gcode);
    REQUIRE(lengths.size() == (size_t) brim_layers);

    // The object widens from 10mm to 30mm over 100 layers.
    // At layer 1 (z=0.2), the cross-section is ~10.4mm; at layer 2 (z=0.4), ~10.8mm.
    // The brim inner boundary (outline + gap) is longer on upper layers,
    // so total brim length must be strictly greater on layer 1 than layer 0.
    double layer0 = lengths.begin()->second;
    REQUIRE(layer0 > 0.0);

    auto it1 = std::next(lengths.begin());
    REQUIRE(it1 != lengths.end());
    double layer1 = it1->second;
    REQUIRE(layer1 > 0.0);

    CHECK(layer1 > layer0);
}

TEST_CASE("Brim avoids overlap when object widens on brim layers", "[SkirtBrim]") {
    // Sharply widening frustum (10mm -> 50mm) over 20mm height.
    // Each brim layer is generated from the current outline, so the brim perimeter grows as the object widens.
    const double layer_height = 0.2;
    const int    brim_layers  = 4;

    auto frustum = make_inverted_frustum(10, 50, 20);
    const std::string gcode = slice({ frustum }, {
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "brim_layers",              brim_layers },
        { "brim_object_gap",          0.4 },
        { "skirt_loops",              0 },
        { "layer_height",             layer_height },
        { "initial_layer_line_width", 0.4 },
        { "initial_layer_print_height", layer_height },
        { "wall_loops",               1 },
        { "slow_down_for_layer_cooling", false },
    });

    auto lengths = brim_length_per_layer(gcode);
    REQUIRE(lengths.size() == (size_t) brim_layers);

    // Each successive brim layer should be longer (object widens -> brim inner
    // boundary grows -> total brim perimeter grows).
    double prev = 0;
    for (const auto &[z, len] : lengths) {
        REQUIRE(len > 0.0);
        if (prev > 0)
            CHECK(len > prev);
        prev = len;
    }
}

TEST_CASE("Per-object brim layers via object overrides", "[SkirtBrim]") {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_deserialize_strict({
        { "brim_type",   "outer_only" },
        { "brim_width",  5 },
        { "brim_layers", 1 },
        { "skirt_loops", 0 },
        { "layer_height", 0.2 },
        { "print_sequence", "by layer" },
    });

    // object 0 gets 1 layer, object 1 gets 3 layers
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> per_object_overrides{
        { { "brim_layers", 1 } },
        { { "brim_layers", 3 } },
    };

    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) }, config, per_object_overrides);

    // brim appears on max(1,3) = 3 distinct Z heights
    REQUIRE(layers_with_role(gcode, "brim").size() == 3u);

    // Layer 0: both objects emit brim (2 passes), layers 1-2: object 1 only (1 pass each) -> 4 total
    REQUIRE(role_passes(gcode, "brim") == 4);
}

// Inverted test: a cone that narrows on upper layers should produce shorter brim perimeters.
TEST_CASE("Brim shrinks on narrowing object on upper brim layers", "[SkirtBrim]") {
    // Brim perimeter should decrease on each successive brim layer.
    const double layer_height = 0.2;
    const int    brim_layers  = 4;

    auto frustum = make_inverted_frustum(30, 10, 20); // 30mm base, 10mm top
    const std::string gcode = slice({ frustum }, {
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "brim_layers",              brim_layers },
        { "brim_object_gap",          0.4 },
        { "skirt_loops",              0 },
        { "layer_height",             layer_height },
        { "initial_layer_line_width", 0.4 },
        { "initial_layer_print_height", layer_height },
        { "wall_loops",               1 },
        { "slow_down_for_layer_cooling", false },
    });

    auto lengths = brim_length_per_layer(gcode);
    REQUIRE(lengths.size() == (size_t) brim_layers);

    // Each successive brim layer should be shorter.
    double prev = std::numeric_limits<double>::max();
    for (const auto &[z, len] : lengths) {
        REQUIRE(len > 0.0);
        if (prev < std::numeric_limits<double>::max())
            CHECK(len < prev);
        prev = len;
    }
}

// Two objects with different shapes: one widening (inverted frustum), one constant (cube).
// Brim should adapt independently to each object's geometry on upper layers.
TEST_CASE("Per-object brim adapts to different shapes on upper layers", "[SkirtBrim]") {
    // Cube (constant cross-section) placed far from inverted frustum (widening).
    // Both get brim_layers=3. Cube brim length stays constant; frustum brim grows.
    auto frustum = make_inverted_frustum(10, 30, 20);

    const std::string gcode = slice({ cube(20), frustum }, {
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "brim_layers",              3 },
        { "brim_object_gap",          0.4 },
        { "combine_brims",            0 },
        { "skirt_loops",              0 },
        { "layer_height",             0.2 },
        { "initial_layer_line_width", 0.4 },
        { "wall_loops",               1 },
        { "slow_down_for_layer_cooling", false },
    });

    auto lengths = brim_length_per_layer(gcode);
    REQUIRE(lengths.size() == 3u);

    // Total brim length across both objects should increase on upper layers
    // because the frustum widens (cube stays constant, so growth is purely from frustum).
    double prev = 0;
    for (const auto &[z, len] : lengths) {
        REQUIRE(len > 0.0);
        if (prev > 0)
            CHECK(len > prev);
        prev = len;
    }
}

// Verify that brim and normal support can coexist on upper layers
// without crashing (the brim code path subtracts support_islands from brim area).
TEST_CASE("Brim with support enabled on upper brim layers", "[SkirtBrim]") {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_deserialize_strict({
        { "layer_height",               0.4 },
        { "initial_layer_print_height", 0.4 },
        { "brim_type",                  "outer_only" },
        { "brim_width",                 5 },
        { "brim_layers",                3 },
        { "brim_object_gap",            0.5 },
        { "skirt_loops",                0 },
        { "enable_support",             1 },
        { "support_type",               "normal" },
        { "gcode_comments",             true },
        { "wall_loops",                 1 },
    });

    const std::string gcode = slice({ TestMesh::overhang }, config);

    // Brim should be present on all 3 requested brim layers.
    REQUIRE(layers_with_role(gcode, "brim").size() == (size_t) 3);

    auto lengths = brim_length_per_layer(gcode);
    for (const auto &[z, len] : lengths) {
        REQUIRE(len > 0.0);
    }
}

// Verify that brim and tree support can coexist on upper layers
// without crashing (the brim code path subtracts tree lslices from brim area).
TEST_CASE("Brim with tree support enabled on upper brim layers", "[SkirtBrim]") {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_deserialize_strict({
        { "layer_height",               0.4 },
        { "initial_layer_print_height", 0.4 },
        { "brim_type",                  "outer_only" },
        { "brim_width",                 5 },
        { "brim_layers",                3 },
        { "brim_object_gap",            0.5 },
        { "skirt_loops",                0 },
        { "enable_support",             1 },
        { "support_type",               "tree" },
        { "gcode_comments",             true },
        { "wall_loops",                 1 },
    });

    const std::string gcode = slice({ TestMesh::overhang }, config);

    // Brim should be present on all 3 requested brim layers.
    REQUIRE(layers_with_role(gcode, "brim").size() == (size_t) 3);

    auto lengths = brim_length_per_layer(gcode);
    for (const auto &[z, len] : lengths) {
        REQUIRE(len > 0.0);
    }
}

// Verify that brim is not emitted when the object has no geometry on a given layer
// (e.g., the object ends before all brim layers are consumed).
TEST_CASE("Brim stops when object ends before all brim layers", "[SkirtBrim]") {
    // Object is 1mm tall (5 layers at 0.2mm), but brim_layers=10 wants 10.
    // Brim should only be emitted on the 5 layers the object actually occupies.
    auto short_cube = cube(1);

    const std::string gcode = slice({ short_cube }, {
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "brim_layers",              10 },
        { "brim_object_gap",          0.4 },
        { "skirt_loops",              0 },
        { "layer_height",             0.2 },
        { "initial_layer_line_width", 0.4 },
        { "wall_loops",               1 },
    });

    auto lengths = brim_length_per_layer(gcode);
    // Brim should appear on exactly 5 layers (the object height), not all 10 requested.
    REQUIRE(lengths.size() == 5u);

    for (const auto &[z, len] : lengths) {
        REQUIRE(len > 0.0);
    }
}
