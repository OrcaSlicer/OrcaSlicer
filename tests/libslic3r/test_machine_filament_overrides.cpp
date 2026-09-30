#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

TEST_CASE("Machine tool-change values replace every filament only when enabled", "[MachineFilamentOverrides]")
{
    const bool enabled = GENERATE(false, true);
    const int count = GENERATE(1, 5, 8);
    DynamicPrintConfig config;
    config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>(count, 1.75)));
    config.set_key_value("filament_loading_speed", new ConfigOptionFloats{11., 22.});
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings{"M117 filament A", "M117 filament B"});
    config.set_key_value("filament_ramming_parameters", new ConfigOptionStrings{"filament curve A", "filament curve B"});
    config.set_key_value("machine_filament_overrides", new ConfigOptionBool(enabled));
    config.set_key_value("machine_filament_loading_speed", new ConfigOptionFloats{37.});
    config.set_key_value("filament_minimal_purge_on_wipe_tower", new ConfigOptionFloats{15., 35.});
    config.set_key_value("machine_filament_minimal_purge_on_wipe_tower", new ConfigOptionFloats{10.});
    config.set_key_value("machine_filament_start_gcode", new ConfigOptionStrings{"M117 machine"});
    config.set_key_value("machine_filament_ramming_parameters", new ConfigOptionStrings{"machine curve"});
    config.apply_machine_filament_overrides();

    const auto &speeds = config.option<ConfigOptionFloats>("filament_loading_speed")->values;
    const auto &scripts = config.option<ConfigOptionStrings>("filament_start_gcode")->values;
    const auto expected_speeds = enabled ? std::vector<double>(std::max(count, 2), 37.) : std::vector<double>{11., 22.};
    REQUIRE(speeds.size() == expected_speeds.size());
    for (size_t i = 0; i < speeds.size(); ++i)
        REQUIRE_THAT(speeds[i], Catch::Matchers::WithinAbs(expected_speeds[i], 1e-6));
    const auto &purges = config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values;
    const auto expected_purges = enabled ? std::vector<double>(std::max(count, 2), 10.) : std::vector<double>{15., 35.};
    REQUIRE(purges.size() == expected_purges.size());
    for (size_t i = 0; i < purges.size(); ++i)
        REQUIRE_THAT(purges[i], Catch::Matchers::WithinAbs(expected_purges[i], 1e-6));
    if (enabled) {
        REQUIRE(scripts == std::vector<std::string>(std::max(count, 2), "M117 machine"));
        REQUIRE(config.option<ConfigOptionStrings>("filament_ramming_parameters")->values ==
                std::vector<std::string>(std::max(count, 2), "machine curve"));
    } else {
        REQUIRE(scripts == std::vector<std::string>{"M117 filament A", "M117 filament B"});
        REQUIRE(config.option<ConfigOptionStrings>("filament_ramming_parameters")->values ==
                std::vector<std::string>{"filament curve A", "filament curve B"});
    }
}

TEST_CASE("Empty machine G-code clears filament G-code while omitted overrides preserve it", "[MachineFilamentOverrides]")
{
    DynamicPrintConfig config;
    config.set_key_value("machine_filament_overrides", new ConfigOptionBool(true));
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings{"keep"});
    config.set_key_value("filament_end_gcode", new ConfigOptionStrings{"remove"});
    config.set_key_value("machine_filament_start_gcode", new ConfigOptionStrings{});
    config.set_key_value("machine_filament_end_gcode", new ConfigOptionStrings{""});
    config.apply_machine_filament_overrides();
    REQUIRE(config.opt_string("filament_start_gcode", 0u) == "keep");
    REQUIRE(config.opt_string("filament_end_gcode", 0u).empty());
}

TEST_CASE("Machine swap scripts can disable automatic filament pressure advance", "[MachineFilamentOverrides]")
{
    DynamicPrintConfig config;
    config.set_key_value("machine_filament_overrides", new ConfigOptionBool(true));
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools{true, true});
    config.set_key_value("machine_enable_pressure_advance", new ConfigOptionBools{false});
    config.set_key_value("filament_cooling_moves", new ConfigOptionInts{3, 4});
    config.set_key_value("machine_filament_cooling_moves", new ConfigOptionInts{2});
    config.apply_machine_filament_overrides();
    REQUIRE_FALSE(config.option<ConfigOptionBools>("enable_pressure_advance")->get_at(0));
    REQUIRE_FALSE(config.option<ConfigOptionBools>("enable_pressure_advance")->get_at(1));
    REQUIRE(config.option<ConfigOptionInts>("filament_cooling_moves")->values == std::vector<int>{2, 2});
}

TEST_CASE("Machine tool-change settings survive serialization and both normalization paths", "[MachineFilamentOverrides]")
{
    const bool split_normalization = GENERATE(false, true);
    DynamicPrintConfig saved;
    saved.set_key_value("machine_filament_overrides", new ConfigOptionBool(true));
    saved.set_key_value("machine_filament_start_gcode", new ConfigOptionStrings{"M117 machine\nG92 E0"});
    DynamicPrintConfig loaded;
    for (const std::string &key : saved.keys())
        loaded.set_deserialize_strict(key, saved.option(key)->serialize());
    loaded.set_key_value("filament_start_gcode", new ConfigOptionStrings{"M117 filament"});
    if (split_normalization)
        loaded.normalize_fdm_1();
    else
        loaded.normalize_fdm();
    REQUIRE(loaded.opt_string("filament_start_gcode", 0u) == "M117 machine\nG92 E0");
}

TEST_CASE("Machine overrides reject per-filament values", "[MachineFilamentOverrides]")
{
    DynamicPrintConfig config;
    config.set_key_value("machine_filament_overrides", new ConfigOptionBool(true));
    config.set_key_value("machine_filament_loading_speed", new ConfigOptionFloats{10., 20.});
    REQUIRE_THROWS_AS(config.apply_machine_filament_overrides(), ConfigurationError);
}

TEST_CASE("Printer composition applies overrides without modifying filament presets", "[MachineFilamentOverrides]")
{
    PresetBundle bundle;
    auto &printer = bundle.printers.get_edited_preset();
    auto &filament = bundle.filaments.get_edited_preset();
    REQUIRE_FALSE(printer.config.opt_bool("machine_filament_overrides"));
    REQUIRE_FALSE(filament.config.has("machine_filament_overrides"));
    printer.config.set_key_value("machine_filament_overrides", new ConfigOptionBool(true));
    printer.config.set_key_value("machine_filament_start_gcode", new ConfigOptionStrings{"M117 machine"});
    filament.config.set_key_value("filament_start_gcode", new ConfigOptionStrings{"M117 filament"});
    bundle.filament_presets = {filament.name};
    REQUIRE(bundle.full_config(false).opt_string("filament_start_gcode", 0u) == "M117 machine");
    REQUIRE(filament.config.opt_string("filament_start_gcode", 0u) == "M117 filament");

    std::vector<Preset> filaments{filament};
    auto composed = PresetBundle::construct_full_config(printer, bundle.prints.get_edited_preset(),
                                                       bundle.project_config, filaments, false, {}, {});
    REQUIRE(composed.opt_string("filament_start_gcode", 0u) == "M117 machine");
    printer.config.set_key_value("machine_filament_overrides", new ConfigOptionBool(false));
    REQUIRE(bundle.full_config(false).opt_string("filament_start_gcode", 0u) == "M117 filament");
}
