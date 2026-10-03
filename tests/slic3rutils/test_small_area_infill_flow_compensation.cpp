#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/GUI.hpp"

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
