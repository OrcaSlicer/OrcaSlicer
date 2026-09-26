// A 2-extruder object sliced through the real Print::apply -> process path. Print::apply runs
// enforce_cardinality, which sizes every per-N array (per-extruder printable height, machine limits,
// filament_map, ...) to its declared cardinality, so the multi-extruder slice completes.
#include <catch2/catch_all.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ModelArrange.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("enforce_cardinality sizes per-N arrays so a multi-extruder object slices", "[Cardinality]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    config.set_key_value("gcode_flavor",               new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_line_width",   new ConfigOptionFloatOrPercent(0, false));
    config.set_key_value("z_hop",                       new ConfigOptionFloats({0}));
    // Sequential print so each object uses its own extruder without a wipe tower.
    config.set_key_value("print_sequence",             new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

    // Clear every custom G-code block so the end-of-export placeholder-def check
    // (ORCA_CHECK_GCODE_PLACEHOLDERS) has nothing to complain about. This isolates
    // the per-extruder sizing behaviour from unrelated placeholder validation.
    config.set_key_value("machine_start_gcode",        new ConfigOptionString(""));
    config.set_key_value("machine_end_gcode",          new ConfigOptionString(""));
    config.set_key_value("before_layer_change_gcode",  new ConfigOptionString(""));
    config.set_key_value("layer_change_gcode",         new ConfigOptionString(""));
    config.set_key_value("change_filament_gcode",      new ConfigOptionString(""));
    config.set_key_value("time_lapse_gcode",           new ConfigOptionString(""));
    config.set_key_value("printing_by_object_gcode",   new ConfigOptionString(""));
    config.set_key_value("filament_start_gcode",       new ConfigOptionStrings({"", ""}));
    config.set_key_value("filament_end_gcode",         new ConfigOptionStrings({"", ""}));

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

    // Stride-2 (normal, silent) machine limits per extruder.
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

    Model model;
    auto *obj1 = model.add_object();
    obj1->add_volume(cube(20));
    obj1->add_instance();

    auto *obj2 = model.add_object();
    obj2->add_volume(cube(20));
    obj2->add_instance();
    obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

    Print print;
    arrange_objects(model, InfiniteBed{}, ArrangeParams{scaled(min_object_distance(config))});
    for (auto *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    // apply() runs enforce_cardinality, sizing every per-N array to its cardinality; the slice then completes.
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 2);
    REQUIRE(print.objects().front()->layer_count() > 0);
}
