#include "libslic3r/ProcessSettingsMerger.hpp"

#include <algorithm>

#include <catch2/catch_all.hpp>

namespace Slic3r {

TEST_CASE("Process settings merger keeps only selected categories", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig old_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig new_defaults = DynamicPrintConfig::full_print_config();

    old_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    new_defaults.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    old_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;
    new_defaults.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;

    old_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 0.96;
    new_defaults.option<ConfigOptionFloat>("print_flow_ratio")->value = 1.0;

    old_settings.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 230;
    new_defaults.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 205;

    old_settings.option<ConfigOptionFloats>("pressure_advance")->values[0] = 0.04;
    new_defaults.option<ConfigOptionFloats>("pressure_advance")->values[0] = 0.02;

    SECTION("safe transfer keeps safe settings and resets unsafe ones") {
        DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(
            old_settings,
            new_defaults,
            { ProcessSettingsCategory::Safe });

        REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.28));
        REQUIRE(merged.option<ConfigOptionFloats>("retraction_length")->values[0] == Catch::Approx(0.7));
        REQUIRE(merged.option<ConfigOptionFloat>("print_flow_ratio")->value == Catch::Approx(1.0));
        REQUIRE(merged.option<ConfigOptionInts>("nozzle_temperature")->values[0] == 205);
        REQUIRE(merged.option<ConfigOptionFloats>("pressure_advance")->values[0] == Catch::Approx(0.02));
    }

    SECTION("conditional and hardware transfer keep only selected groups") {
        DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(
            old_settings,
            new_defaults,
            { ProcessSettingsCategory::Conditional, ProcessSettingsCategory::HardwareSpecific });

        REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.16));
        REQUIRE(merged.option<ConfigOptionFloats>("retraction_length")->values[0] == Catch::Approx(1.3));
        REQUIRE(merged.option<ConfigOptionFloat>("print_flow_ratio")->value == Catch::Approx(0.96));
        REQUIRE(merged.option<ConfigOptionInts>("nozzle_temperature")->values[0] == 230);
        REQUIRE(merged.option<ConfigOptionFloats>("pressure_advance")->values[0] == Catch::Approx(0.04));
    }
}

TEST_CASE("Process settings merger keeps only explicitly selected values", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig old_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig new_defaults = DynamicPrintConfig::full_print_config();

    old_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    new_defaults.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    old_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;
    new_defaults.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;

    old_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 0.96;
    new_defaults.option<ConfigOptionFloat>("print_flow_ratio")->value = 1.0;

    const ProcessSettingsMerger::OptionKeys selected_keys {
        "layer_height",
        "print_flow_ratio"
    };

    DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(old_settings, new_defaults, selected_keys);
    const auto changed_keys = ProcessSettingsMerger::diff_keys(old_settings, new_defaults, selected_keys);

    REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.28));
    REQUIRE(merged.option<ConfigOptionFloat>("print_flow_ratio")->value == Catch::Approx(0.96));
    REQUIRE(merged.option<ConfigOptionFloats>("retraction_length")->values[0] == Catch::Approx(0.7));
    REQUIRE(changed_keys.size() == 2);
    REQUIRE(std::find(changed_keys.begin(), changed_keys.end(), "layer_height") != changed_keys.end());
    REQUIRE(std::find(changed_keys.begin(), changed_keys.end(), "print_flow_ratio") != changed_keys.end());
}

TEST_CASE("Process settings merger ignores empty explicit selection", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig old_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig new_defaults = DynamicPrintConfig::full_print_config();

    old_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    new_defaults.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    const ProcessSettingsMerger::OptionKeys selected_keys;

    DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(old_settings, new_defaults, selected_keys);
    const auto changed_keys = ProcessSettingsMerger::diff_keys(old_settings, new_defaults, selected_keys);

    REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.16));
    REQUIRE(changed_keys.empty());
}

TEST_CASE("Process settings merger deduplicates explicit option keys", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig old_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig new_defaults = DynamicPrintConfig::full_print_config();

    old_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    new_defaults.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    const ProcessSettingsMerger::OptionKeys selected_keys {
        "layer_height",
        "layer_height",
        "layer_height"
    };

    DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(old_settings, new_defaults, selected_keys);
    const auto changed_keys = ProcessSettingsMerger::diff_keys(old_settings, new_defaults, selected_keys);

    REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.28));
    REQUIRE(changed_keys.size() == 1);
    REQUIRE(changed_keys.front() == "layer_height");
}

TEST_CASE("Process settings merger reports transferable settings only for selected categories", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig old_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig new_defaults = DynamicPrintConfig::full_print_config();

    old_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;
    new_defaults.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;

    old_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 0.96;
    new_defaults.option<ConfigOptionFloat>("print_flow_ratio")->value = 1.0;

    REQUIRE(ProcessSettingsMerger::has_transferable_settings(
        old_settings,
        new_defaults,
        { ProcessSettingsCategory::Conditional }));

    REQUIRE_FALSE(ProcessSettingsMerger::has_transferable_settings(
        old_settings,
        new_defaults,
        { ProcessSettingsCategory::Safe }));

    REQUIRE(ProcessSettingsMerger::has_transferable_settings(
        old_settings,
        new_defaults,
        { ProcessSettingsCategory::HardwareSpecific }));
}

TEST_CASE("Process settings merger category keys are unique across combined categories", "[ProcessSettingsMerger]")
{
    const auto keys = ProcessSettingsMerger::option_keys_for_categories({
        ProcessSettingsCategory::Safe,
        ProcessSettingsCategory::Conditional,
        ProcessSettingsCategory::HardwareSpecific,
        ProcessSettingsCategory::Conditional
    });

    REQUIRE(std::count(keys.begin(), keys.end(), "layer_height") == 1);
    REQUIRE(std::count(keys.begin(), keys.end(), "retraction_length") == 1);
    REQUIRE(std::count(keys.begin(), keys.end(), "print_flow_ratio") == 1);
}

TEST_CASE("Process settings merger diff keys respect explicit option selection", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig old_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig new_defaults = DynamicPrintConfig::full_print_config();

    old_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    new_defaults.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    old_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;
    new_defaults.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;

    const auto changed_keys = ProcessSettingsMerger::diff_keys(old_settings, new_defaults, { "retraction_length" });

    REQUIRE(changed_keys.size() == 1);
    REQUIRE(changed_keys.front() == "retraction_length");
}

} // namespace Slic3r