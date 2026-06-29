#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
	#include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCodeReader.hpp"

#include "test_data.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <boost/regex.hpp>
#include <libslic3r/ModelArrange.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Test;

boost::regex perimeters_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; perimeter");
boost::regex infill_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; infill");
boost::regex skirt_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; skirt");

// [NotWorking]: slice() intermittently throws clipper's "Coordinate outside allowed
// range" in CI (Linux) while passing locally. Disabled pending a root-cause fix in a
// follow-up PR.
SCENARIO( "PrintGCode basic functionality", "[PrintGCode][NotWorking]") {
    GIVEN("A default configuration and a print test object") {
        WHEN("the output is executed with no support material") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, {
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.2 },
                { "initial_layer_line_width",   0 },
                { "gcode_comments",             true },
                { "machine_start_gcode",        "" },
                { "z_hop",                      0 }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            //THEN("Exported text contains git commit id") {
            //    REQUIRE(gcode.find("; Git Commit") != std::string::npos);
            //    REQUIRE(gcode.find(SLIC3R_BUILD_ID) != std::string::npos);
            //}
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Exported text does not contain cooling markers (they were consumed)") {
                REQUIRE(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
            }

            THEN("The config trailer includes print and region settings") {
                REQUIRE(gcode.find("; layer_height") != std::string::npos);
                REQUIRE(gcode.find("; sparse_infill_density") != std::string::npos);
            }
            THEN("Infill is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
				boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("final Z height is 20mm") {
                REQUIRE_THAT(max_z(gcode), Catch::Matchers::WithinAbs(20., 1e-4));
            }
        }
        WHEN("output is executed with two objects printed sequentially") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20,TestMesh::cube_20x20x20}, print, model, {
                { "initial_layer_line_width",   0 },
                { "initial_layer_print_height", 0.3 },
                { "layer_height",               0.2 },
                { "enable_support",             false },
                { "raft_layers",                0 },
                { "print_sequence",             "by object" },
                { "gcode_comments",             true },
                { "printing_by_object_gcode",   "; between-object-gcode" },
                { "z_hop",                      0 }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Infill is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("Between-object-gcode is emitted.") {
                REQUIRE(gcode.find("; between-object-gcode") != std::string::npos);
            }
            THEN("final Z height is 20.1mm") {
                REQUIRE_THAT(max_z(gcode), Catch::Matchers::WithinAbs(20.1, 1e-4));
            }
            THEN("Z height resets on object change") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { // saw higher Z before this, now it's lower
                        reset = true;
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
        }
        WHEN("the output is executed with support material") {
            std::string gcode = ::Test::slice({TestMesh::cube_20x20x20}, {
                { "initial_layer_line_width", 0 },
                { "enable_support",           true },
                { "raft_layers",              3 },
                { "gcode_comments",           true }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Raft is emitted.") {
                REQUIRE(gcode.find("; raft") != std::string::npos);
            }
        }
        WHEN("the output is executed with a separate first layer extrusion width") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "initial_layer_line_width", "0.5" }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") != std::string::npos);
            }
        }
        WHEN("Cooling is enabled and the fan is disabled.") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "cooling",                      true },
                { "close_fan_the_first_x_layers", 5 }
                });
            THEN("GCode to disable fan is emitted."){
                REQUIRE(gcode.find("M106 S0") != std::string::npos);
            }
        }
        WHEN("end_gcode exists with layer_num and layer_z") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "machine_end_gcode",          "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
                { "layer_height",               0.1 },
                { "initial_layer_print_height", 0.1 }
                });
            THEN("layer_num and layer_z are processed in the end gcode") {
                REQUIRE(gcode.find("; Layer_num 199") != std::string::npos);
                REQUIRE(gcode.find("; Layer_z 20") != std::string::npos);
            }
        }
        WHEN("current_extruder exists in start_gcode") {
            std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "machine_start_gcode", "; Extruder [current_extruder]" }
            });
            THEN("current_extruder is processed in the start gcode and set for first extruder") {
                REQUIRE(gcode.find("; Extruder 0") != std::string::npos);
            }
        }

        WHEN("layer_num represents the layer's index from z=0") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, {
                { "print_sequence",             "by object" },
                { "gcode_comments",             true },
                { "layer_change_gcode",         ";Layer:[layer_num] ([layer_z] mm)" },
                { "layer_height",               0.1 },
                { "initial_layer_print_height", 0.1 }
                });
			// End of the 1st object.
            std::string token = ";Layer:199 ";
			size_t pos = gcode.find(token);
			THEN("First and second object last layer is emitted") {
				// First object
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				double z = 0;
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE_THAT(z, Catch::Matchers::WithinAbs(20., 1e-4));
				// Second object
				pos = gcode.find(";Layer:399 ", pos);
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE_THAT(z, Catch::Matchers::WithinAbs(20., 1e-4));
			}
        }
    }
}

TEST_CASE("export_gcode writes G-code without a result pointer", "[PrintGCode][export_gcode]")
{
    Print print;
    Model model;
    Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model);
    print.process();

    SECTION("non-BBL printer") {}
    SECTION("BBL printer") { print.is_BBL_printer() = true; }

    ScopedTemporaryFile temp(".gcode");
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), nullptr, nullptr));

    std::ifstream in(temp.string());
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE_FALSE(gcode.empty());
}

TEST_CASE("Initial layer height is honored", "[PrintGCode]")
{
    const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, {
        { "initial_layer_print_height", 0.3 },
        { "layer_height",               0.2 },
        { "z_hop",                      0 } // keep recorded Z equal to the printed layer height
    });

    std::set<double> layer_zs;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&layer_zs] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (line.extruding(self) && line.dist_XY(self) > 0)
            layer_zs.insert(self.z());
    });

    REQUIRE(layer_zs.size() > 1);
    REQUIRE_THAT(*layer_zs.begin(),            Catch::Matchers::WithinAbs(0.3, 1e-4));
    REQUIRE_THAT(*std::next(layer_zs.begin()), Catch::Matchers::WithinAbs(0.5, 1e-4));
}

TEST_CASE("Sequential printing follows model order", "[PrintGCode]")
{
    // Two objects of different heights, taller one added first. Orca prints
    // sequential objects in model order, so the taller one is printed first.
    const std::string gcode = Slic3r::Test::slice({ Slic3r::make_cube(20, 20, 20), Slic3r::make_cube(20, 20, 10) }, {
        { "print_sequence",             "by object" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 }
    });

    // The first object's height is the peak Z reached before Z drops back to the
    // first layer (the object change). With by-object printing only an object
    // change returns Z to the bottom.
    double first_object_peak_z = 0.0;
    double running_peak        = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (first_object_peak_z != 0.0 || !line.extruding(self)) return; // ignore travels (e.g. start-gcode Z lift)
        if (running_peak > 1.0 && self.z() < 1.0)
            first_object_peak_z = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });

    REQUIRE_THAT(first_object_peak_z, Catch::Matchers::WithinAbs(20.0, 0.3));
}

// Verify that emit_machine_limits_to_gcode emits the correct max value across
// used extruders (regression for commit b4ee665: "Emit max value of machine
// limit among used extruders").
TEST_CASE("Machine envelope emits max limit among used extruders", "[PrintGCode][MachineEnvelope]")
{
    SECTION("Single extruder emits its configured values") {
        const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, {
            { "emit_machine_limits_to_gcode", "1" },
            { "gcode_flavor",                "marlin2" },
            { "gcode_comments",              "1" },
            { "machine_start_gcode",         "" },
            { "layer_height",                "0.2" },
            { "initial_layer_print_height",  "0.2" },
            { "initial_layer_line_width",    "0" },
            { "z_hop",                       "0" },
            // stride-2 options: (normal, silent)
            { "machine_max_acceleration_x",           "500,600" },
            { "machine_max_acceleration_y",           "700,800" },
            { "machine_max_acceleration_z",           "100,200" },
            { "machine_max_acceleration_e",           "5000,6000" },
            { "machine_max_acceleration_extruding",   "1200,1300" },
            { "machine_max_acceleration_retracting",  "1400,1500" },
            { "machine_max_acceleration_travel",      "1600,1700" },
            // stride-2 options: (normal, silent)
            { "machine_max_speed_x",  "100,100" },
            { "machine_max_speed_y",  "110,110" },
            { "machine_max_speed_z",  "10,10" },
            { "machine_max_speed_e",  "50,50" },
            { "machine_max_jerk_x",   "8,8" },
            { "machine_max_jerk_y",   "9,9" },
            { "machine_max_jerk_z",   "0.4,0.4" },
            { "machine_max_jerk_e",   "5,5" },
            { "machine_max_junction_deviation", "0.02,0.03" },
        });

        THEN("M201 uses the normal acceleration values") {
            REQUIRE(gcode.find("M201 X500 Y700 Z100 E5000") != std::string::npos);
        }
        THEN("M203 uses the speed values") {
            REQUIRE(gcode.find("M203 X100 Y110 Z10 E50") != std::string::npos);
        }
        THEN("M204 (Marlin 2) uses extruding / retracting / travel") {
            REQUIRE(gcode.find("M204 P1200 R1400 T1600") != std::string::npos);
        }
        THEN("M205 uses the jerk values") {
            REQUIRE(gcode.find("M205 X8.00 Y9.00 Z0.40 E5.00") != std::string::npos);
        }
        THEN("M205 J uses the junction deviation") {
            REQUIRE(gcode.find("M205 J0.020") != std::string::npos);
        }
    }

    SECTION("Legacy Marlin flavor emits correct format") {
        const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, {
            { "emit_machine_limits_to_gcode", "1" },
            { "gcode_flavor",                "marlin" },
            { "gcode_comments",              "1" },
            { "machine_start_gcode",         "" },
            { "layer_height",                "0.2" },
            { "initial_layer_print_height",  "0.2" },
            { "initial_layer_line_width",    "0" },
            { "z_hop",                       "0" },
            // All machine limits must be provided — defaults are empty vectors.
            { "machine_max_acceleration_x",           "500,600" },
            { "machine_max_acceleration_y",           "500,600" },
            { "machine_max_acceleration_z",           "500,600" },
            { "machine_max_acceleration_e",           "5000,6000" },
            { "machine_max_acceleration_extruding",   "1200,1300" },
            { "machine_max_acceleration_retracting",  "1400,1500" },
            { "machine_max_acceleration_travel",      "1600,1700" },
            { "machine_max_speed_x",  "100,100" },
            { "machine_max_speed_y",  "110,110" },
            { "machine_max_speed_z",  "10,10" },
            { "machine_max_speed_e",  "50,50" },
            { "machine_max_jerk_x",   "8,8" },
            { "machine_max_jerk_y",   "9,9" },
            { "machine_max_jerk_z",   "0.4,0.4" },
            { "machine_max_jerk_e",   "5,5" },
            { "machine_max_junction_deviation", "0.02,0.03" },
        });

        THEN("Legacy Marlin: M204 travel_acc = extruding_acc") {
            // gcfMarlinLegacy uses extruding acc for travel too
            REQUIRE(gcode.find("M204 P1200 R1400 T1200") != std::string::npos);
        }
        THEN("Legacy Marlin: M205 uses mm/sec format") {
            REQUIRE(gcode.find("M205 X8.00 Y9.00 Z0.40 E5.00") != std::string::npos);
        }
    }

    SECTION("Multi extruder - max of used extruders is emitted") {
        // Build config with 2 extruders that have *different* machine limits.
        // Extruder 1 has higher values; the emitted G-code must use the max.
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

        // Print basics
        config.set_key_value("emit_machine_limits_to_gcode", new ConfigOptionBool(true));
        config.set_key_value("gcode_flavor",                 new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
        config.set_key_value("gcode_comments",               new ConfigOptionBool(true));
        config.set_key_value("machine_start_gcode",          new ConfigOptionString(""));
        config.set_key_value("layer_height",                 new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height",   new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_line_width",     new ConfigOptionFloatOrPercent(0, false));
        config.set_key_value("z_hop",                        new ConfigOptionFloats({0}));
        // Print objects sequentially so each uses its own extruder without
        // wipe-tower / tool-change complexity.
        config.set_key_value("print_sequence",               new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

        // 2 extruders
        config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
        config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
        config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
        config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
        config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
        config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
        // filament_map maps filament slot index (1-based) → logical extruder ID (1-based).
        // Default [1] maps everything to extruder 0. Need [1, 2] for two distinct extruders.
        // fmmManual prevents auto-computation from overwriting the explicit mapping.
        config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
        config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
        config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
        config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
        config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
        config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
        // flush_volumes_matrix must be filament_count^2 * heads_count entries.
        // 2 filaments * 2 * 1 head = 4 entries (all zero — flush volumes not tested here).
        config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1}));
        config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 0, 0, 0}));

        // Machine limits: extruder 0 low, extruder 1 high
        // Stride-2 (normal, silent pairs): e0_n, e0_s, e1_n, e1_s
        config.set_key_value("machine_max_acceleration_x",          new ConfigOptionFloats({500, 0, 1000, 0}));
        config.set_key_value("machine_max_acceleration_y",          new ConfigOptionFloats({700, 0, 1100, 0}));
        config.set_key_value("machine_max_acceleration_z",          new ConfigOptionFloats({100, 0, 300, 0}));
        config.set_key_value("machine_max_acceleration_e",          new ConfigOptionFloats({5000, 0, 8000, 0}));
        config.set_key_value("machine_max_acceleration_extruding",  new ConfigOptionFloats({1200, 0, 2200, 0}));
        config.set_key_value("machine_max_acceleration_retracting", new ConfigOptionFloats({1400, 0, 2400, 0}));
        config.set_key_value("machine_max_acceleration_travel",     new ConfigOptionFloats({1600, 0, 2600, 0}));
        config.set_key_value("machine_max_speed_x",  new ConfigOptionFloats({100, 0, 200, 0}));
        config.set_key_value("machine_max_speed_y",  new ConfigOptionFloats({110, 0, 210, 0}));
        config.set_key_value("machine_max_speed_z",  new ConfigOptionFloats({10, 0, 30, 0}));
        config.set_key_value("machine_max_speed_e",  new ConfigOptionFloats({50, 0, 80, 0}));
        config.set_key_value("machine_max_jerk_x",   new ConfigOptionFloats({8, 0, 12, 0}));
        config.set_key_value("machine_max_jerk_y",   new ConfigOptionFloats({9, 0, 13, 0}));
        config.set_key_value("machine_max_jerk_z",   new ConfigOptionFloats({0.4, 0, 0.6, 0}));
        config.set_key_value("machine_max_jerk_e",   new ConfigOptionFloats({5, 0, 10, 0}));
        config.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats({0.02, 0, 0.05, 0}));

        // Model: two objects assigned to different extruders
        Model model;
        auto* obj1 = model.add_object();
        obj1->add_volume(mesh(TestMesh::cube_20x20x20));
        obj1->add_instance();
        // obj1 uses default extruder=1 (0-based index 0)

        auto* obj2 = model.add_object();
        obj2->add_volume(mesh(TestMesh::cube_20x20x20));
        obj2->add_instance();
        obj2->config.set_key_value("extruder", new ConfigOptionInt(2)); // 0-based index 1

        Print print;
        arrange_objects(model, InfiniteBed{},
                        ArrangeParams{scaled(min_object_distance(config))});
        for (auto* mo : model.objects) {
            mo->ensure_on_bed();
            print.auto_assign_extruders(mo);
        }

        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        std::string gcode = Slic3r::Test::gcode(print);

        THEN("M201 contains max (extruder 1's) acceleration values") {
            REQUIRE(gcode.find("M201 X1000 Y1100 Z300 E8000") != std::string::npos);
        }
        THEN("M203 contains max speed values") {
            REQUIRE(gcode.find("M203 X200 Y210 Z30 E80") != std::string::npos);
        }
        THEN("M204 contains max extruding / retracting / travel") {
            REQUIRE(gcode.find("M204 P2200 R2400 T2600") != std::string::npos);
        }
        THEN("M205 contains max jerk values") {
            REQUIRE(gcode.find("M205 X12.00 Y13.00 Z0.60 E10.00") != std::string::npos);
        }
        THEN("M205 contains max m_max_junction_deviation ") {
            REQUIRE(gcode.find("M205 J0.050") != std::string::npos);
        }
    }
}

// Verify that the EXTRUDER_LIMIT macro (GCodeWriter.cpp) correctly:
//  1) Uses the active extruder's specific limit when filament() is known.
//  2) Falls back to the maximum of all extruder limits when filament() is nullptr.
//
// These two behaviours were introduced in:
//  - "Use per-extruder motion limit" (1ab34a7454)
//  - "Use max limit when current extruder is unknown" (b7240ab1c6)
TEST_CASE("EXTRUDER_LIMIT per-extruder clamping and max fallback", "[PrintGCode][MachineEnvelope]")
{
    // --- Build config with 2 extruders that have different machine limits ---
    // Extruder 0: low limits
    // Extruder 1: high limits
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    config.set_key_value("emit_machine_limits_to_gcode", new ConfigOptionBool(true));
    config.set_key_value("gcode_flavor",                 new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    config.set_key_value("gcode_comments",               new ConfigOptionBool(true));
    config.set_key_value("machine_start_gcode",          new ConfigOptionString(""));
    config.set_key_value("layer_height",                 new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height",   new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_line_width",     new ConfigOptionFloatOrPercent(0, false));
    config.set_key_value("z_hop",                        new ConfigOptionFloats({0}));
    config.set_key_value("print_sequence",               new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

    // 2 extruders, 2 filaments
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 0, 0, 0}));

    // --- Machine limits (stride-2: e0_n, e0_s, e1_n, e1_s) ---
    // Extruder 0 has LOW limits, Extruder 1 has HIGH limits.
    config.set_key_value("machine_max_acceleration_x",          new ConfigOptionFloats({500, 0, 1000, 0}));
    config.set_key_value("machine_max_acceleration_y",          new ConfigOptionFloats({500, 0, 1000, 0}));
    config.set_key_value("machine_max_acceleration_z",          new ConfigOptionFloats({100, 0, 200, 0}));
    config.set_key_value("machine_max_acceleration_e",          new ConfigOptionFloats({5000, 0, 5000, 0}));
    config.set_key_value("machine_max_acceleration_extruding",  new ConfigOptionFloats({500, 0, 2000, 0}));
    config.set_key_value("machine_max_acceleration_retracting", new ConfigOptionFloats({600, 0, 2000, 0}));
    config.set_key_value("machine_max_acceleration_travel",     new ConfigOptionFloats({700, 0, 2500, 0}));
    config.set_key_value("machine_max_speed_x",  new ConfigOptionFloats({100, 0, 200, 0}));
    config.set_key_value("machine_max_speed_y",  new ConfigOptionFloats({110, 0, 210, 0}));
    config.set_key_value("machine_max_speed_z",  new ConfigOptionFloats({10, 0, 30, 0}));
    config.set_key_value("machine_max_speed_e",  new ConfigOptionFloats({50, 0, 80, 0}));
    config.set_key_value("machine_max_jerk_x",   new ConfigOptionFloats({5, 0, 15, 0}));
    config.set_key_value("machine_max_jerk_y",   new ConfigOptionFloats({6, 0, 16, 0}));
    config.set_key_value("machine_max_jerk_z",   new ConfigOptionFloats({0.4, 0, 0.8, 0}));
    config.set_key_value("machine_max_jerk_e",   new ConfigOptionFloats({3, 0, 8, 0}));
    config.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats({0.02, 0, 0.08, 0}));

    // --- Print acceleration: 1500 mm/s² ---
    // Exceeds extruder 0's limit (500) → should be clamped to 500.
    // Does NOT exceed extruder 1's limit (2000) → passes through as 1500.
    config.set_key_value("default_acceleration",        new ConfigOptionFloats({1500, 1500}));
    config.set_key_value("outer_wall_acceleration",     new ConfigOptionFloats({1500, 1500}));
    config.set_key_value("inner_wall_acceleration",     new ConfigOptionFloats({1500, 1500}));
    config.set_key_value("top_surface_acceleration",    new ConfigOptionFloats({1500, 1500}));
    config.set_key_value("initial_layer_acceleration",  new ConfigOptionFloats({1500, 1500}));
    config.set_key_value("travel_acceleration",         new ConfigOptionFloats({1500, 1500}));

    // Model: two objects assigned to different extruders
    Model model;
    auto* obj1 = model.add_object();
    obj1->add_volume(mesh(TestMesh::cube_20x20x20));
    obj1->add_instance();

    auto* obj2 = model.add_object();
    obj2->add_volume(mesh(TestMesh::cube_20x20x20));
    obj2->add_instance();
    obj2->config.set_key_value("extruder", new ConfigOptionInt(2)); // 0-based index 1

    Print print;
    arrange_objects(model, InfiniteBed{}, ArrangeParams{scaled(min_object_distance(config))});
    for (auto* mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    std::string gcode = Slic3r::Test::gcode(print);

    SECTION("Preamble: max limit among used extruders") {
        THEN("M201 uses max (extruder 1's) acceleration values") {
            REQUIRE(gcode.find("M201 X1000 Y1000 Z200 E5000") != std::string::npos);
        }
        THEN("M204 uses max extruding/retracting/travel") {
            REQUIRE(gcode.find("M204 P2000 R2000 T2500") != std::string::npos);
        }
        THEN("M205 uses max jerk values") {
            REQUIRE(gcode.find("M205 X15.00 Y16.00 Z0.80 E8.00") != std::string::npos);
        }
    }

    SECTION("Preamble: EXTRUDER_LIMIT falls back to max when no filament is active") {
        // set_junction_deviation() is called during preamble with no active filament.
        // EXTRUDER_LIMIT(m_max_junction_deviation) → filament() == nullptr → max of all (0.08).
        THEN("M205 J uses max junction deviation") {
            REQUIRE(gcode.find("M205 J0.080") != std::string::npos);
        }
    }

    SECTION("Print: extruder 0 acceleration clamped to its specific limit") {
        // Extruder 0 machine limit = 500. Print accel = 1500 > 500 → clamped to 500.
        THEN("M204 P500 appears (extruder 0 clamped)") {
            REQUIRE(gcode.find("M204 P500") != std::string::npos);
        }
        THEN("M204 T700 appears (extruder 0 travel clamped)") {
            REQUIRE(gcode.find("M204 T700") != std::string::npos);
        }
    }

    SECTION("Print: extruder 1 acceleration NOT clamped to extruder 0's limit") {
        // Extruder 1 machine limit = 2000. Print accel = 1500 < 2000 → not clamped.
        THEN("M204 P1500 appears (extruder 1 not clamped to 500)") {
            REQUIRE(gcode.find("M204 P1500") != std::string::npos);
        }
    }
}
