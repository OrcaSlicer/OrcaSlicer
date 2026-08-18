#include "libslic3r/ProcessSettingsMerger.hpp"

#include <algorithm>

#include <catch2/catch_all.hpp>

namespace Slic3r {

namespace {

bool contains_key(const ProcessSettingsMerger::OptionKeys &keys, const std::string &key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

} // namespace

TEST_CASE("Process settings transfer defaults select unsaved changes only", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig parent_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig saved_settings  = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig edited_settings = DynamicPrintConfig::full_print_config();

    parent_settings.option<ConfigOptionFloat>("layer_height")->value = 0.20;
    saved_settings.option<ConfigOptionFloat>("layer_height")->value  = 0.20;
    edited_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;

    parent_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;
    saved_settings.option<ConfigOptionFloats>("retraction_length")->values[0]  = 1.3;
    edited_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;

    const auto transferable = ProcessSettingsMerger::transferable_settings(saved_settings, edited_settings, &parent_settings);
    const auto selection    = ProcessSettingsMerger::default_selection(transferable);

    REQUIRE(contains_key(transferable.unsaved_options, "layer_height"));
    REQUIRE(contains_key(transferable.system_default_options, "retraction_length"));
    REQUIRE(contains_key(selection.unsaved_options, "layer_height"));
    REQUIRE_FALSE(contains_key(selection.system_default_options, "retraction_length"));
}

TEST_CASE("Process settings transfer keeps overlapping keys in unsaved changes", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig parent_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig saved_settings  = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig edited_settings = DynamicPrintConfig::full_print_config();

    parent_settings.option<ConfigOptionFloat>("layer_height")->value = 0.16;
    saved_settings.option<ConfigOptionFloat>("layer_height")->value  = 0.20;
    edited_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;

    const auto transferable = ProcessSettingsMerger::transferable_settings(saved_settings, edited_settings, &parent_settings);

    REQUIRE(contains_key(transferable.unsaved_options, "layer_height"));
    REQUIRE_FALSE(contains_key(transferable.system_default_options, "layer_height"));
}

TEST_CASE("Process settings transfer can be filtered against a target profile", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig parent_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig saved_settings  = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig edited_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig target_settings = DynamicPrintConfig::full_print_config();

    parent_settings.option<ConfigOptionFloat>("layer_height")->value = 0.20;
    saved_settings.option<ConfigOptionFloat>("layer_height")->value  = 0.20;
    edited_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    target_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;

    parent_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;
    saved_settings.option<ConfigOptionFloats>("retraction_length")->values[0]  = 1.3;
    edited_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;
    target_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;

    const auto transferable = ProcessSettingsMerger::transferable_settings(saved_settings, edited_settings, &parent_settings, &target_settings);

    REQUIRE_FALSE(contains_key(transferable.unsaved_options, "layer_height"));
    REQUIRE(contains_key(transferable.system_default_options, "retraction_length"));
}

TEST_CASE("Process settings transfer compares saved source settings against target profile", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig parent_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig source_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig target_settings = DynamicPrintConfig::full_print_config();

    parent_settings.option<ConfigOptionFloat>("layer_height")->value = 0.20;
    source_settings.option<ConfigOptionFloat>("layer_height")->value = 0.20;
    target_settings.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    const auto transferable = ProcessSettingsMerger::transferable_settings(source_settings, source_settings, &parent_settings, &target_settings);
    const auto selection    = ProcessSettingsMerger::default_selection(transferable);

    REQUIRE(transferable.unsaved_options.empty());
    REQUIRE(contains_key(transferable.system_default_options, "layer_height"));
    REQUIRE(selection.unsaved_options.empty());
    REQUIRE_FALSE(contains_key(selection.system_default_options, "layer_height"));
}

TEST_CASE("Process settings transfer reports model profile differences against target without parent differences", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig parent_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig model_settings  = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig target_settings = DynamicPrintConfig::full_print_config();

    parent_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 0.96;
    model_settings.option<ConfigOptionFloat>("print_flow_ratio")->value  = 0.96;
    target_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 1.00;

    const auto source = ProcessSettingsMerger::transfer_source_settings(model_settings, model_settings, &parent_settings);
    const auto transferable = ProcessSettingsMerger::transferable_settings(
        source.saved_settings,
        source.edited_settings,
        source.parent_settings(),
        &target_settings);

    REQUIRE(transferable.unsaved_options.empty());
    REQUIRE(contains_key(transferable.system_default_options, "print_flow_ratio"));
}

TEST_CASE("Process settings transfer source can represent an imported model profile", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig parent_settings   = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig selected_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig imported_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig edited_settings   = DynamicPrintConfig::full_print_config();

    parent_settings.option<ConfigOptionFloat>("layer_height")->value   = 0.20;
    selected_settings.option<ConfigOptionFloat>("layer_height")->value = 0.20;
    imported_settings.option<ConfigOptionFloat>("layer_height")->value = 0.24;
    edited_settings.option<ConfigOptionFloat>("layer_height")->value   = 0.28;

    parent_settings.option<ConfigOptionFloats>("retraction_length")->values[0]   = 0.7;
    selected_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;
    imported_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.3;
    edited_settings.option<ConfigOptionFloats>("retraction_length")->values[0]   = 1.3;

    const auto source = ProcessSettingsMerger::transfer_source_settings(imported_settings, edited_settings, &parent_settings);
    const auto transferable = ProcessSettingsMerger::transferable_settings(
        source.saved_settings,
        source.edited_settings,
        source.parent_settings(),
        nullptr);
    const auto selection = ProcessSettingsMerger::default_selection(transferable);

    REQUIRE(contains_key(transferable.unsaved_options, "layer_height"));
    REQUIRE(contains_key(transferable.system_default_options, "retraction_length"));
    REQUIRE_FALSE(contains_key(transferable.system_default_options, "layer_height"));
    REQUIRE(contains_key(selection.unsaved_options, "layer_height"));
    REQUIRE_FALSE(contains_key(selection.system_default_options, "retraction_length"));

    const DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(
        source.saved_settings,
        source.edited_settings,
        selected_settings,
        selection);
    REQUIRE_THAT(merged.option<ConfigOptionFloat>("layer_height")->value, Catch::Matchers::WithinAbs(0.28, 1e-9));
    REQUIRE_THAT(merged.option<ConfigOptionFloats>("retraction_length")->values[0], Catch::Matchers::WithinAbs(0.7, 1e-9));
}

TEST_CASE("Process settings transfer merges selected keys from their source configs", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig saved_settings  = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig edited_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig target_settings = DynamicPrintConfig::full_print_config();

    saved_settings.option<ConfigOptionFloat>("layer_height")->value  = 0.20;
    edited_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    target_settings.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    saved_settings.option<ConfigOptionFloats>("retraction_length")->values[0]  = 1.3;
    edited_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 2.0;
    target_settings.option<ConfigOptionFloats>("retraction_length")->values[0] = 0.7;

    saved_settings.option<ConfigOptionFloat>("print_flow_ratio")->value  = 0.96;
    edited_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 0.97;
    target_settings.option<ConfigOptionFloat>("print_flow_ratio")->value = 1.0;

    ProcessSettingsMerger::TransferSelection selection;
    selection.system_default_options = { "retraction_length", "print_flow_ratio" };
    selection.unsaved_options        = { "layer_height", "print_flow_ratio" };

    const DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(saved_settings, edited_settings, target_settings, selection);
    const auto selected_keys        = selection.all_options();

    REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.28));
    REQUIRE(merged.option<ConfigOptionFloats>("retraction_length")->values[0] == Catch::Approx(1.3));
    REQUIRE(merged.option<ConfigOptionFloat>("print_flow_ratio")->value == Catch::Approx(0.97));
    REQUIRE(contains_key(selected_keys, "layer_height"));
    REQUIRE(contains_key(selected_keys, "retraction_length"));
    REQUIRE(contains_key(selected_keys, "print_flow_ratio"));
}

TEST_CASE("Process settings transfer ignores empty source selections", "[ProcessSettingsMerger]")
{
    DynamicPrintConfig saved_settings  = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig edited_settings = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig target_settings = DynamicPrintConfig::full_print_config();

    saved_settings.option<ConfigOptionFloat>("layer_height")->value  = 0.20;
    edited_settings.option<ConfigOptionFloat>("layer_height")->value = 0.28;
    target_settings.option<ConfigOptionFloat>("layer_height")->value = 0.16;

    const ProcessSettingsMerger::TransferSelection selection;
    const DynamicPrintConfig merged = ProcessSettingsMerger::merge_settings(saved_settings, edited_settings, target_settings, selection);

    REQUIRE(merged.option<ConfigOptionFloat>("layer_height")->value == Catch::Approx(0.16));
}

} // namespace Slic3r
