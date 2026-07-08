#include <catch2/catch_all.hpp>

#include <memory>

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ModelArrange.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("set_speed emits values with fixed-point output.", "[GCodeWriter]") {

    GIVEN("GCodeWriter instance") {
        GCodeWriter writer;
        WHEN("set_speed is called to set speed to 99999.123") {
            THEN("Output string is G1 F99999.123") {
                REQUIRE_THAT(writer.set_speed(99999.123), Catch::Matchers::Equals("G1 F99999.123\n"));
            }
        }
        WHEN("set_speed is called to set speed to 1") {
            THEN("Output string is G1 F1") {
                REQUIRE_THAT(writer.set_speed(1.0), Catch::Matchers::Equals("G1 F1\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200022") {
            THEN("Output string is G1 F203.2") {
                REQUIRE_THAT(writer.set_speed(203.200022), Catch::Matchers::Equals("G1 F203.2\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200522") {
            THEN("Output string is G1 F203.201") {
                REQUIRE_THAT(writer.set_speed(203.200522), Catch::Matchers::Equals("G1 F203.201\n"));
            }
        }
    }
}

SCENARIO("z_hop lifts the nozzle when a lift is requested", "[GCodeWriter]") {
    GIVEN("A writer with the nozzle parked at Z = 10") {
        GCodeWriter writer;
        std::vector<unsigned int> extruder_ids { 0 };
        writer.set_extruders(extruder_ids);
        writer.set_extruder(0);
        writer.travel_to_z(10.0);

        WHEN("z_hop is 1 and an eager lift is requested") {
            writer.config.z_hop.values = { 1.0 };
            std::string gcode = writer.eager_lift(LiftType::NormalLift);
            THEN("a Z move up by z_hop is emitted") {
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("Z11"));
            }
        }
        WHEN("z_hop is 0") {
            writer.config.z_hop.values = { 0.0 };
            std::string gcode = writer.eager_lift(LiftType::NormalLift);
            THEN("no lift is emitted") {
                REQUIRE(gcode.empty());
            }
        }
    }
}

SCENARIO("Origin manipulation", "[GCodeWriter]") {
	Slic3r::GCode gcodegen;
	WHEN("set_origin to (10,0)") {
    	gcodegen.set_origin(Vec2d(10,0));
    	REQUIRE(gcodegen.origin() == Vec2d(10, 0));
    }
	WHEN("set_origin to (10,0) and translate by (5, 5)") {
		gcodegen.set_origin(Vec2d(10,0));
		gcodegen.set_origin(gcodegen.origin() + Vec2d(5, 5));
		THEN("origin returns reference to point") {
    		REQUIRE(gcodegen.origin() == Vec2d(15,5));
    	}
    }
}

// Verify that emit_machine_limits_to_gcode emits the correct max value across
// used extruders (regression for commit b4ee665: "Emit max value of machine
// limit among used extruders").
TEST_CASE("Machine envelope emits max limit among used extruders", "[GCodeWriter]")
{
    SECTION("Single extruder emits its configured values") {
        const std::string gcode = Slic3r::Test::slice({ cube(20) }, {
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
        const std::string gcode = Slic3r::Test::slice({ cube(20) }, {
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
        obj1->add_volume(cube(20));
        obj1->add_instance();
        // obj1 uses default extruder=1 (0-based index 0)

        auto* obj2 = model.add_object();
        obj2->add_volume(cube(20));
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
TEST_CASE("EXTRUDER_LIMIT per-extruder clamping and max fallback", "[GCodeWriter]")
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
    obj1->add_volume(cube(20));
    obj1->add_instance();

    auto* obj2 = model.add_object();
    obj2->add_volume(cube(20));
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
