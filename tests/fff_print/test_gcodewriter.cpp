#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ModelArrange.hpp"

#include <boost/filesystem.hpp>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Arrange on a finite bed, not an unbounded InfiniteBed: the latter places items
// near INT64_MIN/4 (~2.3e18), which reaches ClipperLib's coordinate limit and throws
// "Coordinate outside allowed range" on Windows/arm64. A 500x500 bed keeps coordinates
// small while still covering large printers.
static void arrange_objects_on_test_bed(Model &model, const DynamicPrintConfig &config)
{
    const BoundingBox bed{Point::new_scale(0., 0.), Point::new_scale(500., 500.)};
    arrange_objects(model, bed, ArrangeParams{scaled(min_object_distance(config))});
}

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
        arrange_objects_on_test_bed(model, config);
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
    arrange_objects_on_test_bed(model, config);
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

SCENARIO("Extruder reads the injected config column", "[GCodeWriter][H2C]") {
    GIVEN("A writer whose per-variant arrays hold three columns for two filaments") {
        GCodeWriter writer;
        // Column layout after a migrating regroup: filament 0 -> column 0, filament 1 ->
        // columns 1 (its first variant) and 2 (its second variant).
        writer.config.retraction_length.values   = {0.8, 0.5, 1.2};
        writer.config.z_hop.values               = {0.4, 0.6, 0.9};
        writer.config.retraction_speed.values    = {30., 40., 50.};
        writer.config.filament_flow_ratio.values = {0.98, 1.0, 1.02};
        // Filament-indexed arrays keep one entry per filament.
        writer.config.filament_diameter.values   = {1.75, 1.75};
        writer.set_extruders({0, 1});
        writer.toolchange(1, 1);
        Extruder *fil = writer.filament();
        REQUIRE(fil != nullptr);
        REQUIRE(fil->id() == 1);
        const double crossection = 1.75 * 1.75 * 0.25 * PI;

        WHEN("no column has been injected") {
            THEN("the getters read the filament id's column") {
                REQUIRE(fil->config_index() == 1);
                REQUIRE_THAT(fil->retraction_length(), Catch::Matchers::WithinAbs(0.5, 1e-9));
                REQUIRE_THAT(fil->retract_lift(), Catch::Matchers::WithinAbs(0.6, 1e-9));
                REQUIRE(fil->retract_speed() == 40);
                REQUIRE_THAT(fil->e_per_mm3(), Catch::Matchers::WithinRel(1.0 / crossection, 1e-9));
            }
        }
        WHEN("the second variant column is injected") {
            fil->set_config_index(2);
            THEN("the getters follow the column and the flow cache is rescaled") {
                REQUIRE(fil->config_index() == 2);
                REQUIRE_THAT(fil->retraction_length(), Catch::Matchers::WithinAbs(1.2, 1e-9));
                REQUIRE_THAT(fil->retract_lift(), Catch::Matchers::WithinAbs(0.9, 1e-9));
                REQUIRE(fil->retract_speed() == 50);
                REQUIRE_THAT(fil->e_per_mm3(), Catch::Matchers::WithinRel(1.02 / crossection, 1e-9));
            }
            THEN("filament-indexed reads keep using the filament id") {
                REQUIRE_THAT(fil->filament_diameter(), Catch::Matchers::WithinAbs(1.75, 1e-9));
            }
        }
        WHEN("a negative index is injected") {
            fil->set_config_index(2);
            fil->set_config_index(-1);
            THEN("resolution resets to the filament id") {
                REQUIRE(fil->config_index() == 1);
                REQUIRE_THAT(fil->retraction_length(), Catch::Matchers::WithinAbs(0.5, 1e-9));
                REQUIRE_THAT(fil->e_per_mm3(), Catch::Matchers::WithinRel(1.0 / crossection, 1e-9));
            }
        }
    }
}

// Numeric argument of every line starting with `prefix`, in file order.
static std::vector<int> collect_line_args(const std::string &gcode, const std::string &prefix)
{
    std::vector<int> values;
    std::istringstream stream(gcode);
    std::string line;
    while (std::getline(stream, line))
        if (line.compare(0, prefix.size(), prefix) == 0)
            values.push_back(std::atoi(line.c_str() + int(prefix.size())));
    return values;
}

static int count_lines_with_prefix(const std::string &gcode, const std::string &prefix)
{
    return (int) collect_line_args(gcode, prefix).size();
}

// A toolchange ordinal sequence is healthy when it advances by exactly one per
// change block; a change-less prime-tower visit must not consume an ordinal.
static bool ordinals_consecutive(const std::vector<int> &values)
{
    for (size_t i = 1; i < values.size(); ++i)
        if (values[i] != values[i - 1] + 1)
            return false;
    return true;
}

SCENARIO("Toolchange emission and prefix per printer kind", "[GCodeWriter][H2C]") {
    GIVEN("A dual-extruder writer with two filaments") {
        GCodeWriter writer;
        writer.config.filament_diameter.values = {1.75, 1.75};
        writer.set_extruders({0, 1});

        WHEN("the printer is a BBL machine") {
            writer.set_is_bbl_machine(true);
            THEN("the toolchange prefix is the plain T command") {
                REQUIRE_THAT(writer.toolchange_prefix(), Catch::Matchers::Equals("T"));
            }
            THEN("toolchange emits a single M1020 with the nozzle id") {
                const std::string gcode = writer.toolchange(1, 0);
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("M1020 S1 H0"));
                REQUIRE_THAT(gcode, !Catch::Matchers::StartsWith("T1"));
            }
            THEN("the other filament and nozzle emit their own ids") {
                REQUIRE_THAT(writer.toolchange(0, 1), Catch::Matchers::ContainsSubstring("M1020 S0 H1"));
            }
            THEN("an unresolved nozzle keeps the literal -1 convention") {
                REQUIRE_THAT(writer.toolchange(1, -1), Catch::Matchers::ContainsSubstring("M1020 S1 H-1"));
            }
        }
        WHEN("the printer is a BBL machine with manual filament change") {
            writer.set_is_bbl_machine(true);
            writer.config.manual_filament_change.value = true;
            THEN("the manual tag wins over the M1020 form") {
                REQUIRE_THAT(writer.toolchange_prefix(), Catch::Matchers::StartsWith(";"));
                const std::string gcode = writer.toolchange(1, 0);
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring(writer.toolchange_prefix() + "1"));
                REQUIRE_THAT(gcode, !Catch::Matchers::ContainsSubstring("M1020"));
            }
        }
        WHEN("the printer is not a BBL machine") {
            THEN("toolchange keeps the plain T command") {
                REQUIRE_THAT(writer.toolchange_prefix(), Catch::Matchers::Equals("T"));
                const std::string gcode = writer.toolchange(1, 0);
                REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("T1"));
                REQUIRE_THAT(gcode, !Catch::Matchers::ContainsSubstring("M1020"));
            }
        }
    }
}

// Shared dual-extruder printer config for the toolchange-count scenarios below.
static DynamicPrintConfig dual_extruder_toolchange_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("gcode_flavor",               new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    config.set_key_value("emit_machine_limits_to_gcode", new ConfigOptionBool(false));
    config.set_key_value("machine_start_gcode",        new ConfigOptionString(""));
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_line_width",   new ConfigOptionFloatOrPercent(0, false));
    config.set_key_value("z_hop",                      new ConfigOptionFloats({0., 0.}));
    // The change block carries both a real toolchange command and the ordinal
    // placeholder the stock profiles feed to the firmware.
    config.set_key_value("change_filament_gcode",
                         new ConfigOptionString("T[next_filament_id]\nM620 O{toolchange_count + 1}\n"));

    // 2 extruders, one filament each (manual map so nothing regroups them).
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 140, 140, 0}));
    return config;
}

SCENARIO("Change blocks carry consecutive toolchange ordinals without a duplicate command", "[GCodeWriter][H2C]") {
    GIVEN("Two sequentially printed objects on different extruders of a BBL machine") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

        Model model;
        auto *obj1 = model.add_object();
        obj1->add_volume(cube(20));
        obj1->add_instance();
        auto *obj2 = model.add_object();
        obj2->add_volume(cube(20));
        obj2->add_instance();
        obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

        auto slice_to_gcode = [&]() {
            Print print;
            print.is_BBL_printer() = true;
            arrange_objects_on_test_bed(model, config);
            for (auto *mo : model.objects) {
                mo->ensure_on_bed();
                print.auto_assign_extruders(mo);
            }
            print.apply(model, config);
            print.validate();
            print.set_status_silent();
            print.process();
            return Slic3r::Test::gcode(print);
        };

        WHEN("the change block already changes the tool") {
            const std::string gcode = slice_to_gcode();
            const std::vector<int> ordinals = collect_line_args(gcode, "M620 O");
            THEN("each change block advances the ordinal by exactly one, without inflation") {
                REQUIRE(!ordinals.empty());
                REQUIRE(ordinals_consecutive(ordinals));
                REQUIRE(ordinals.front() <= 3);
            }
            THEN("the writer's own command is suppressed as a duplicate") {
                REQUIRE(count_lines_with_prefix(gcode, "M1020") == 0);
                REQUIRE(count_lines_with_prefix(gcode, "T1") >= 1);
            }
        }
        WHEN("the change block does not change the tool itself") {
            config.set_key_value("change_filament_gcode",
                                 new ConfigOptionString("M620 O{toolchange_count + 1}\n"));
            const std::string gcode = slice_to_gcode();
            const std::vector<int> ordinals = collect_line_args(gcode, "M620 O");
            THEN("the writer's toolchange survives and carries a nozzle id") {
                REQUIRE(count_lines_with_prefix(gcode, "M1020 S1 H") >= 1);
            }
            THEN("the ordinal sequence stays consecutive") {
                REQUIRE(!ordinals.empty());
                REQUIRE(ordinals_consecutive(ordinals));
                REQUIRE(ordinals.front() <= 3);
            }
        }
    }
}

SCENARIO("Prime-tower visits without a filament change do not advance the toolchange ordinal", "[GCodeWriter][H2C]") {
    GIVEN("A print whose only filament change happens far above the bed") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));

        // Filament 2 is used only above z=6, so every tower layer below it is a
        // change-less visit — the exact geometry that used to inflate the ordinal.
        Model model;
        auto *obj = model.add_object();
        obj->add_volume(cube(10));
        obj->add_instance();
        DynamicPrintConfig range_config;
        range_config.set_key_value("extruder", new ConfigOptionInt(2));
        // Every layer range must carry a layer_height (see layer_height_profile_from_ranges).
        range_config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        obj->layer_config_ranges[{6.0, 10.0}].assign_config(std::move(range_config));

        Print print;
        print.is_BBL_printer() = true;
        arrange_objects_on_test_bed(model, config);
        for (auto *mo : model.objects) {
            mo->ensure_on_bed();
            print.auto_assign_extruders(mo);
        }
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
        const std::string gcode = Slic3r::Test::gcode(print);

        WHEN("the print is exported") {
            const std::vector<int> ordinals = collect_line_args(gcode, "M620 O");
            THEN("the prime-tower toolchange path was exercised") {
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("CP TOOLCHANGE START"));
            }
            THEN("dozens of change-less tower layers consume no ordinal") {
                REQUIRE(!ordinals.empty());
                REQUIRE(ordinals_consecutive(ordinals));
                REQUIRE(ordinals.front() <= 3);
            }
            THEN("no duplicate toolchange command follows the change block") {
                REQUIRE(count_lines_with_prefix(gcode, "M1020") == 0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Real-profile toolchange coverage, targeted. The all-vendors sweep in test_profile_slicing.cpp now
// slices a two-colour cube per printer, so it already expands every shipped change_filament_gcode with
// each printer's DEFAULT extruder variants (both the single-nozzle append_tcr and dual-nozzle set_extruder
// paths). What that sweep can't reach is a variant-conditional branch the defaults never select — H2D's
// change gcode has an `== "Direct Drive TPU High Flow"` block. This scenario forces that branch by handing
// the extruders distinct kits, so an unregistered placeholder inside it still throws "Variable does not
// exist" here instead of only in the field.
// ---------------------------------------------------------------------------

// Two 20mm cubes on separate extruders of a BBL machine, printed by object so exactly
// one real toolchange fires and drives the change_filament_gcode. Returns the g-code.
static std::string slice_two_object_bbl(DynamicPrintConfig &config)
{
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

    Model model;
    auto *obj1 = model.add_object();
    obj1->add_volume(cube(20));
    obj1->add_instance();
    auto *obj2 = model.add_object();
    obj2->add_volume(cube(20));
    obj2->add_instance();
    obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

    Print print;
    print.is_BBL_printer() = true;
    arrange_objects_on_test_bed(model, config);
    for (auto *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    return Slic3r::Test::gcode(print);
}

// The real change_filament_gcode of a shipped "<printer> 0.4 nozzle" machine profile.
static std::string shipped_change_filament_gcode(const std::string &printer)
{
    const std::string path = std::string(PROFILES_DIR) + "/BBL/machine/Bambu Lab " + printer + " 0.4 nozzle.json";
    // PROFILES_DIR is an absolute path baked in at build time; a sparse test checkout
    // without resources/ leaves it missing. Skip rather than dereference a config that
    // never loaded - this is the only fff_print test that reads a shipped profile.
    if (!boost::filesystem::exists(path))
        SKIP("shipped profile not present in this checkout: " << path);
    DynamicPrintConfig                 config;
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    config.load_from_json(path, ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
    // Fail loudly on a malformed/renamed profile instead of null-dereferencing in opt_string.
    INFO("profile: " << path << (reason.empty() ? "" : ("  load reason: " + reason)));
    REQUIRE(config.has("change_filament_gcode"));
    return config.opt_string("change_filament_gcode");
}

SCENARIO("Toolchange gcode resolves old/new_extruder_variant from printer_extruder_variant", "[GCodeWriter][H2C]")
{
    GIVEN("a BBL dual-extruder print whose change gcode reads the extruder-variant placeholders") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        // A distinctive variant that can only reach the g-code through printer_extruder_variant.
        // Both entries carry it so the assertion is independent of which physical extruder the
        // emitted change routes through.
        config.set_key_value("printer_extruder_variant",
                             new ConfigOptionStrings({"Direct Drive TPU High Flow", "Direct Drive TPU High Flow"}));
        config.set_key_value("change_filament_gcode", new ConfigOptionString(
            "; VARIANT old={old_extruder_variant} new={new_extruder_variant}\nT[next_filament_id]\n"));

        WHEN("the print is sliced") {
            const std::string gcode = slice_two_object_bbl(config);
            THEN("both placeholders resolve to the printer_extruder_variant value") {
                // The resolved line is the proof: an unresolved token or a parser throw would
                // prevent this exact line from being emitted. (A negative "{token}" check is
                // unreliable — the g-code's trailing config dump echoes the raw template.)
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring(
                    "; VARIANT old=Direct Drive TPU High Flow new=Direct Drive TPU High Flow"));
            }
        }
    }
}

SCENARIO("Global current-tool placeholders resolve in a context with no local injection", "[GCodeWriter][H2C]")
{
    GIVEN("a BBL dual-extruder print whose before_layer_change_gcode reads the current-tool placeholders") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        // before_layer_change is one of the contexts that inject NO current_* into their local config
        // (unlike change_filament / machine_end / layer_change), so these placeholders can only resolve
        // through the GLOBAL parser vars published at each toolchange (and at the initial set_extruder).
        // Pre-fix current_filament_id / current_extruder_id / current_nozzle_id were undefined here and the
        // whole slice threw a PlaceholderParserError — the same failure mode X2D's layer_change hit.
        config.set_key_value("before_layer_change_gcode", new ConfigOptionString(
            "; GVAR fid={current_filament_id} eid={current_extruder_id} nid={current_nozzle_id}\n"));

        WHEN("the print is sliced (obj1 on filament 0, obj2 on filament 1)") {
            std::string gcode;
            REQUIRE_NOTHROW(gcode = slice_two_object_bbl(config));
            THEN("all three globals resolve to the CORRECT active-tool values on both sides of the change") {
                // Assert the FULL resolved marker, not just no-throw: obj1 prints on filament 0 (extruder 0,
                // nozzle 0) and obj2 on filament 1 (extruder 1, nozzle 1). Locking every field means a
                // stale or wrong global (e.g. obj2 still reading fid=0, or a mismatched extruder/nozzle id)
                // fails here — this is the value guard that replaces the old "throws on undefined" canary.
                // Only the emitted before_layer_change lines carry resolved values; the trailing config dump
                // keeps the raw "{current_filament_id}" template, so these are unambiguous.
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("; GVAR fid=0 eid=0 nid=0"));
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("; GVAR fid=1 eid=1 nid=1"));
            }
        }
    }
}

SCENARIO("Shipped dual-nozzle change_filament_gcode resolves during a real slice", "[GCodeWriter][H2C][Profiles]")
{
    const std::string printer = GENERATE(std::string("H2C"), std::string("H2D"), std::string("H2D Pro"), std::string("X2D"));

    GIVEN("the real " + printer + " change_filament_gcode driving a BBL dual-extruder slice") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        config.set_key_value("change_filament_gcode", new ConfigOptionString(shipped_change_filament_gcode(printer)));
        // H2D's gcode branches on the extruder variant; give the extruders distinct kits so the
        // "Direct Drive TPU High Flow" branch is reachable.
        config.set_key_value("printer_extruder_variant",
                             new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive TPU High Flow"}));
        // Extruder-indexed machine rates the stock gcode divides by (default size 1); size to 2 extruders.
        config.set_key_value("hotend_cooling_rate", new ConfigOptionFloatsNullable({2.0, 2.0}));
        config.set_key_value("hotend_heating_rate", new ConfigOptionFloatsNullable({2.0, 2.0}));

        THEN("every placeholder resolves (no undefined-variable throw) and the change block runs") {
            std::string gcode;
            REQUIRE_NOTHROW(gcode = slice_two_object_bbl(config));
            // A resolved marker only the emitted change block produces (the trailing config
            // dump keeps the raw "{filament_type[...]}" template), so this confirms the real
            // change_filament_gcode was expanded, not merely echoed.
            REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("set_filament_type:PLA"));
        }
    }
}

TEST_CASE("Custom G-code motion limits are restored before generated moves", "[GCodeWriter]")
{
    const std::string gcode = Slic3r::Test::slice({ cube(20) }, {
        { "gcode_flavor",                "marlin" },
        { "gcode_comments",              "1" },
        { "machine_start_gcode",         "" },
        { "layer_change_gcode",          "M204 S5000\nm205 x5 y5\n" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "initial_layer_line_width",    "0" },
        { "z_hop",                       "0" },
        { "default_acceleration",        "6000" },
        { "initial_layer_acceleration",  "6000" },
        { "outer_wall_acceleration",     "6000" },
        { "inner_wall_acceleration",     "0" },
        { "default_jerk",                "8" },
        { "initial_layer_jerk",          "8" },
        { "outer_wall_jerk",             "8" },
        { "inner_wall_jerk",             "0" },
    });

    const size_t custom_gcode_pos = gcode.find("m205 x5 y5");
    REQUIRE(custom_gcode_pos != std::string::npos);
    REQUIRE(gcode.find("M204 S6000 ; adjust acceleration", custom_gcode_pos) != std::string::npos);
    REQUIRE(gcode.find("M205 X8 Y8 ; adjust jerk", custom_gcode_pos) != std::string::npos);
}
