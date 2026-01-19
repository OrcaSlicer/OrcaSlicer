#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"

#include <boost/algorithm/string.hpp>

#include "test_data.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

/// Helper method to find the tool used for the brim (always the first extrusion)
static int get_brim_tool(const std::string &gcode)
{
    int brim_tool	= -1;
    int tool		= -1;
	GCodeReader parser;
    parser.parse_buffer(gcode, [&tool, &brim_tool] (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line)
    {
        // if the command is a T command, set the the current tool
        if (boost::starts_with(line.cmd(), "T")) {
            tool = atoi(line.cmd().data() + 1);
        } else if (line.cmd() == "G1" && line.extruding(self) && line.dist_XY(self) > 0 && brim_tool < 0) {
            brim_tool = tool;
        }
    });
    return brim_tool;
}

TEST_CASE("Skirt height is honored", "[Skirt][.]") {
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
    	{ "skirts",					1 },
    	{ "skirt_height", 			5 },
    	{ "perimeters", 			0 },
    	{ "support_material_speed", 99 },
		// avoid altering speeds unexpectedly
    	{ "cooling", 				false },
    	{ "first_layer_speed", 		"100%" }
    });

	std::string gcode;
    SECTION("printing a single object") {
        gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    }
    SECTION("printing multiple objects") {
        gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, config);
    }

    std::map<double, bool> layers_with_skirt;
    double support_speed = config.opt<Slic3r::ConfigOptionFloat>("support_material_speed")->value * MM_PER_MIN;
	GCodeReader parser;
    parser.parse_buffer(gcode, [&layers_with_skirt, &support_speed] (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && self.f() == Catch::Approx(support_speed)) {
            layers_with_skirt[self.z()] = 1;
        }
    });
    REQUIRE(layers_with_skirt.size() == (size_t)config.opt_int("skirt_height"));
}

SCENARIO("Original Slic3r Skirt/Brim tests", "[SkirtBrim][.]") {
    GIVEN("A default configuration") {
	    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_num_extruders(4);
		config.set_deserialize_strict({
			{ "support_material_speed", 		99 },
			{ "first_layer_height", 			0.3 },
        	{ "gcode_comments", 				true },
        	// avoid altering speeds unexpectedly
        	{ "cooling", 						false },
        	{ "first_layer_speed", 				"100%" },
        	// remove noise from top/solid layers
        	{ "top_solid_layers", 				0 },
        	{ "bottom_solid_layers", 			1 },
			{ "start_gcode",					"T[initial_tool]\n" }
        });

        WHEN("Brim width is set to 5") {
        	config.set_deserialize_strict({
				{ "perimeters", 		0 },
				{ "skirts", 			0 },
				{ "brim_width", 		5 }
			});
			THEN("Brim is generated") {
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                bool brim_generated = false;
                double support_speed = config.opt<Slic3r::ConfigOptionFloat>("support_material_speed")->value * MM_PER_MIN;
			    Slic3r::GCodeReader parser;
                parser.parse_buffer(gcode, [&brim_generated, support_speed] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line) {
                    if (self.z() == Catch::Approx(0.3) || line.new_Z(self) == Catch::Approx(0.3)) {
                        if (line.extruding(self) && self.f() == Catch::Approx(support_speed)) {
                            brim_generated = true;
                        }
                    }
                });
                REQUIRE(brim_generated);
            }
        }

        WHEN("Skirt area is smaller than the brim") {
            config.set_deserialize_strict({
            	{ "skirts", 	1 },
            	{ "brim_width", 10}
            });
            THEN("Gcode generates") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }

        WHEN("Skirt height is 0 and skirts > 0") {
            config.set_deserialize_strict({
            	{ "skirts", 	  2 },
            	{ "skirt_height", 0 }
            });
            THEN("Gcode generates") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }

#if 0
		// This is a real error! One shall print the brim with the external perimeter extruder!
        WHEN("Perimeter extruder = 2 and support extruders = 3") {
            THEN("Brim is printed with the extruder used for the perimeters of first object") {
				config.set_deserialize_strict({
					{ "skirts", 					0 },
					{ "brim_width", 				5 },
					{ "perimeter_extruder", 		2 },
					{ "support_material_extruder", 	3 },
					{ "infill_extruder", 			4 }
				});
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                int tool = get_brim_tool(gcode);
                REQUIRE(tool == config.opt_int("perimeter_extruder") - 1);
            }
        }
        WHEN("Perimeter extruder = 2, support extruders = 3, raft is enabled") {
            THEN("brim is printed with same extruder as skirt") {
				config.set_deserialize_strict({
					{ "skirts",						0 },
					{ "brim_width", 				5 },
					{ "perimeter_extruder", 		2 },
					{ "support_material_extruder", 	3 },
					{ "infill_extruder", 			4 },
					{ "raft_layers", 				1 }
				});
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                int tool = get_brim_tool(gcode);
                REQUIRE(tool == config.opt_int("support_material_extruder") - 1);
            }
        }
#endif

        WHEN("brim width to 1 with layer_width of 0.5") {
        	config.set_deserialize_strict({
				{ "skirts", 						0 },
				{ "first_layer_extrusion_width", 	0.5 },
				{ "brim_width", 					1 }
        	});			
            THEN("2 brim lines") {
		        Slic3r::Print print;
		        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, config);
                size_t total_entities = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_entities += pair.second.entities.size();
                }
                REQUIRE(total_entities == 2);
            }
        }

#if 0
        WHEN("brim ears on a square") {
			config.set_deserialize_strict({
				{ "skirts",							0 },
				{ "first_layer_extrusion_width",	0.5 },
				{ "brim_width",						1 },
				{ "brim_ears",						1 },
				{ "brim_ears_max_angle",			91 }
			});
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, config);
            THEN("Four brim ears") {
                REQUIRE(print.brim().entities.size() == 4);
            }
        }

        WHEN("brim ears on a square but with a too small max angle") {
			config.set_deserialize_strict({
				{ "skirts",							0 },
				{ "first_layer_extrusion_width",	0.5 },
				{ "brim_width",						1 },
				{ "brim_ears",						1 },
				{ "brim_ears_max_angle",			89 }
				});
            THEN("no brim") {
		        Slic3r::Print print;
                Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, config);
                REQUIRE(print.brim().entities.size() == 0);
            }
        }
#endif

        WHEN("Object is plated with overhang support and a brim") {
        	config.set_deserialize_strict({
	            { "layer_height", 				0.4 },
	            { "first_layer_height", 		0.4 },
	            { "skirts", 					1 },
	            { "skirt_distance", 			0 },
	            { "support_material_speed", 	99 },
	            { "perimeter_extruder", 		1 },
	            { "support_material_extruder", 	2 },
	            { "infill_extruder", 			3 },			// ensure that a tool command gets emitted.
	            { "cooling", 					false },		// to prevent speeds to be altered
	            { "first_layer_speed", 			"100%" },		// to prevent speeds to be altered
				{ "start_gcode",				"T[initial_tool]\n" }
        	});

            THEN("overhang generates?") {
            	//FIXME does it make sense?
                REQUIRE(! Slic3r::Test::slice({TestMesh::overhang}, config).empty());
            }

            // config.set("support_material", true);      // to prevent speeds to be altered

#if 0
			// This test is not finished.
            THEN("skirt length is large enough to contain object with support") {
                CHECK(config.opt_bool("support_material")); // test is not valid if support material is off
				std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                double support_speed = config.opt<ConfigOptionFloat>("support_material_speed")->value * MM_PER_MIN;
				double skirt_length = 0.0;
				Points extrusion_points;
				int tool = -1;
				GCodeReader parser;
                parser.parse_buffer(gcode, [config, &extrusion_points, &tool, &skirt_length, support_speed] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line) {
                    // std::cerr << line.cmd() << "\n";
					if (boost::starts_with(line.cmd(), "T")) {
						tool = atoi(line.cmd().data() + 1);
					} else if (self.z() == Catch::Approx(config.opt<ConfigOptionFloat>("first_layer_height")->value)) {
                        // on first layer
						if (line.extruding(self) && line.dist_XY(self) > 0) {
                            float speed = ( self.f() > 0 ?  self.f() : line.new_F(self));
                            // std::cerr << "Tool " << tool << "\n";
                            if (speed == Catch::Approx(support_speed) && tool == config.opt_int("perimeter_extruder") - 1) {
                                // Skirt uses first material extruder, support material speed.
                                skirt_length += line.dist_XY(self);
                            } else
                                extrusion_points.push_back(Slic3r::Point::new_scale(line.new_X(self), line.new_Y(self)));
                        }
                    }
                    if (self.z() == Catch::Approx(0.3) || line.new_Z(self) == Catch::Approx(0.3)) {
                        if (line.extruding(self) && self.f() == Catch::Approx(support_speed)) {
                        }
                    }
                });
                Slic3r::Polygon convex_hull = Slic3r::Geometry::convex_hull(extrusion_points);
                double hull_perimeter = unscale<double>(convex_hull.split_at_first_point().length());
                REQUIRE(skirt_length > hull_perimeter);
            }
#endif

        }
        WHEN("Large minimum skirt length is used.") {
            config.set("min_skirt_length", 20);
            THEN("Gcode generation doesn't crash") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }
    }
}
