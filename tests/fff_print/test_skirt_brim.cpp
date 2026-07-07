#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"

#include <boost/algorithm/string.hpp>

#include <cmath>

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
