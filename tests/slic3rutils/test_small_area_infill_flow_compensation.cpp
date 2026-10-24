#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/any.hpp>
#include <algorithm>
#include <string>
#include <vector>
#include <wx/sizer.h>

#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/ConfigValueFormatter.hpp"
#include "slic3r/GUI/OptionsGroup.hpp"
#include "slic3r/GUI/SettingsIndex.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

using namespace Slic3r;
using FlowModel = Slic3r::SmallAreaInfillFlowCompensationModel;

TEST_CASE("The curve editor GUI value updates every model point and reports unsaved changes", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::string key = "small_area_infill_flow_compensation_model";
    const std::vector<std::string> parameters = {"0,0", "\n10,1"};
    Preset saved(Preset::TYPE_PRINT, "saved");
    saved.config.set_key_value(key, new ConfigOptionStrings{"0,0", "\n10,1"});
    saved.config.set_key_value("small_area_infill_flow_compensation", new ConfigOptionBool(false));
    Preset edited = saved;
    const FlowModel::Rows rows = {{"0", "0"}, {"2,5", "0,75"}, {"10", "1"}};
    FlowModel model(parameters);
    model.accept_rows(rows);
    const auto& points = model.parameters();

    GUI::change_opt_value(edited.config, key, model.serialized_parameters());
    CHECK(model.is_modified());
    CHECK(edited.config.option<ConfigOptionStrings>(key)->values == points);
    CHECK_FALSE(edited.config.opt_bool("small_area_infill_flow_compensation"));
    CHECK(PresetCollection::dirty_options(&edited, &saved, true) == std::vector<std::string>{key});

    // The widget's revert path uses the same serialized value, with no Field to read back.
    GUI::change_opt_value(edited.config, key, FlowModel::serialize_parameters(parameters));
    CHECK(edited.config.option<ConfigOptionStrings>(key)->values == parameters);
    CHECK(PresetCollection::dirty_options(&edited, &saved, true).empty());
}

TEST_CASE("Unsaved curve values include all points while indexed string options remain indexed", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::string key = "small_area_infill_flow_compensation_model";
    DynamicPrintConfig saved, edited;
    saved.set_key_value(key, new ConfigOptionStrings{"0,0", "\n10,1"});
    edited.set_key_value(key, new ConfigOptionStrings{"0,0", "\n5,0.5", "\n10,1"});
    CHECK(GUI::get_string_value(key, saved) == "0,0;\n10,1;");
    CHECK(GUI::get_string_value(key, edited) == "0,0;\n5,0.5;\n10,1;");
    CHECK(GUI::get_string_value(key + "#0", edited) == "0,0");
    CHECK(print_config_def.get(key)->is_curve());
    CHECK_FALSE(print_config_def.get("compatible_printers")->is_curve());
}

TEST_CASE("Unsaved curve changes resolve to one indexed setting regardless of point count", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::string key = "small_area_infill_flow_compensation_model";
    Preset saved(Preset::TYPE_PRINT, "saved");
    saved.config.set_key_value(key, new ConfigOptionStrings{"0,0", "\n10,1"});
    Preset edited = saved;
    const int change = GENERATE(0, 1, 2);
    const FlowModel::Rows rows = change == 0 ? FlowModel::Rows{{"0", "0"}, {"11", "1"}} :
                                change == 1 ? FlowModel::Rows{{"0", "0"}, {"5", "0.5"}, {"10", "1"}} : FlowModel::Rows{};
    FlowModel model(saved.config.opt<ConfigOptionStrings>(key)->values);
    model.accept_rows(rows);
    GUI::change_opt_value(edited.config, key, model.serialized_parameters());

    Search::SettingsIndex index;
    index.add_key(key, Preset::TYPE_PRINT, "Walls and surfaces", "Quality");
    index.init({{&saved.config, Preset::TYPE_PRINT, comAdvanced}});
    REQUIRE(index.all_options().size() == 1);
    CHECK(index.all_options().front().opt_key() == key);
    REQUIRE(index.apply(&edited.config, Preset::TYPE_PRINT, comAdvanced));
    index.sort_options_by_key();

    const auto dirty = PresetCollection::dirty_options(&edited, &saved, true);
    REQUIRE(dirty == std::vector<std::string>{key});
    int variant_index;
    const auto& option = index.get_option(dirty.front(), Preset::TYPE_PRINT, variant_index);
    REQUIRE(variant_index == -1);
    CHECK(option.opt_key() == key);
    CHECK_FALSE(option.label_local.empty());
    CHECK_FALSE(option.group_local.empty());
    CHECK_FALSE(option.category_local.empty());
    CHECK(GUI::get_string_value(dirty.front(), edited.config) != GUI::get_string_value(dirty.front(), saved.config));
    CHECK(index.all_options().size() == 1);
}

TEST_CASE("Settings indexing retains individual printer vector entries alongside complete curves", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::string key = "small_area_infill_flow_compensation_model";
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionStrings{"0,0", "\n10,1"});
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.6});
    Search::SettingsIndex index;
    index.add_key(key, Preset::TYPE_PRINTER, "Curve", "General");
    index.add_key("nozzle_diameter#0", Preset::TYPE_PRINTER, "Extruder 1", "General");
    index.add_key("nozzle_diameter#1", Preset::TYPE_PRINTER, "Extruder 2", "General");
    index.init({{&config, Preset::TYPE_PRINTER, comAdvanced}});
    index.sort_options_by_key();
    REQUIRE(index.all_options().size() == 3);
    int variant_index;
    CHECK(index.get_option(key, Preset::TYPE_PRINTER, variant_index).opt_key() == key);
    CHECK(variant_index == -1);
    std::vector<std::string> keys;
    for (const auto& option : index.all_options()) keys.push_back(option.opt_key());
    std::sort(keys.begin(), keys.end());
    CHECK(keys == std::vector<std::string>{"nozzle_diameter#0", "nozzle_diameter#1", key});
}

TEST_CASE("Generic widget rows only expose curve undo controls", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::string key = GENERATE(as<std::string>{}, "small_area_infill_flow_compensation_model", "printable_area", "compatible_printers", "compatible_prints", "filament_ramming_parameters");
    GUI::Line line("", "");
    line.append_option(GUI::Option(*print_config_def.get(key), key));
    line.widget = [](wxWindow*) -> wxSizer* { return nullptr; };
    const ScalableBitmap icon;
    line.set_undo_bitmap(&icon);
    CHECK(line.has_undo_ui() == print_config_def.get(key)->is_curve());
}

TEST_CASE("The curve editor GUI value clears and restores an empty model", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::string key = "small_area_infill_flow_compensation_model";
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionStrings{"0,0", "\n10,1"});
    GUI::change_opt_value(config, key, FlowModel::serialize_parameters({}));
    CHECK(config.option<ConfigOptionStrings>(key)->values.empty());
    GUI::change_opt_value(config, key, FlowModel::serialize_parameters({"0,0", "\n10,1"}));
    CHECK(config.option<ConfigOptionStrings>(key)->values == std::vector<std::string>{"0,0", "\n10,1"});
}
