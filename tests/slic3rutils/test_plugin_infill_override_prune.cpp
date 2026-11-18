// Pruning plugin capability overrides when a preset changes its plugin-backed options. Infill
// pattern settings are also valid in model object, volume, layer-range and plate overrides, so
// pruning needs those configs to decide whether an infill override is stale. Without them it
// preserves infill overrides; with the complete context it resolves pattern IDs through
// InfillFactory and prunes normally.
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include <libslic3r/Config.hpp>
#include <libslic3r/Fill/FillBase.hpp>
#include <libslic3r/Fill/InfillFactory.hpp>
#include <libslic3r/Fill/InfillPurpose.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <nlohmann/json.hpp>
#include <slic3r/plugin/PluginConfig.hpp>

#include <memory>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

const char* overrides_key()
{
    return Preset::plugin_overrides_key(Preset::TYPE_PRINT);
}

// Unregisters the fixture's pattern on every exit path, including a failed REQUIRE.
struct ScopedPluginInfill
{
    std::string plugin_key;
    std::string capability;

    ~ScopedPluginInfill() { InfillFactory::unregister_plugin_infills(plugin_key, capability); }
};

json override_entry(const char*        type,
                    const std::string& capability,
                    const std::string& plugin_key,
                    const json&        config = json::object())
{
    return json{
        {"plugin_key", plugin_key},
        {"capability", capability},
        {"capability_type", type},
        {"plugin_version", "1.0.0"},
        {"cap_config", config},
    };
}

// A legacy entry has no capability_type, so from_entries() keys it as Unknown and find() resolves
// it by name through its legacy fallback.
json legacy_override_entry(const std::string& capability, const std::string& plugin_key, const json& config)
{
    return json{
        {"plugin_key", plugin_key},
        {"capability", capability},
        {"plugin_version", "1.0.0"},
        {"cap_config", config},
    };
}

void set_overrides(DynamicPrintConfig& config, const json& entries)
{
    const CapabilityConfigDocument document = CapabilityConfigDocument::from_entries(entries);
    config.set_key_value(overrides_key(), new ConfigOptionString(serialize_plugin_overrides(document)));
}

CapabilityConfigDocument stored_overrides(const DynamicPrintConfig& config)
{
    const auto* option = config.option<ConfigOptionString>(overrides_key());
    REQUIRE(option != nullptr);

    CapabilityConfigDocument document;
    std::string              error;
    REQUIRE(parse_plugin_overrides(option->value, document, error));
    CHECK(error.empty());
    return document;
}

} // namespace

TEST_CASE("Pruning keeps infill overrides when the context is incomplete", "[PluginConfig][InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));
    set_overrides(config, json::array({override_entry("infill", "Multi Infill", "infill-plugin", {{"sparse", 7}})}));

    // The default context supplies no object, volume, layer-range or plate configs, so the preset
    // alone cannot prove the override stale. It must survive unchanged.
    CHECK_FALSE(prune_stale_plugin_overrides(config, overrides_key()));

    const CapabilityConfigDocument overrides = stored_overrides(config);
    const auto owned = overrides.find({PluginCapabilityType::Infill, "Multi Infill", "infill-plugin"});
    REQUIRE(owned.has_value());
    CHECK(owned->config == json{{"sparse", 7}});
}

TEST_CASE("Pruning keeps infill overrides when only some extra configs are supplied", "[PluginConfig][InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));

    DynamicPrintConfig object;
    object.set_key_value("sparse_infill_pattern", new ConfigOptionString("rectilinear"));

    set_overrides(config, json::array({override_entry("infill", "Multi Infill", "infill-plugin")}));

    // Supplying some configs is not enough: without the completeness guarantee another object,
    // volume, layer range or plate may still select a plugin pattern.
    PluginOverridePruneContext context;
    context.configs = {&object};

    CHECK_FALSE(prune_stale_plugin_overrides(config, overrides_key(), context));
    CHECK(stored_overrides(config).contains({PluginCapabilityType::Infill, "Multi Infill", "infill-plugin"}));
}

TEST_CASE("Pruning resolves an object-selected infill pattern through the factory", "[PluginConfig][InfillPlugin]")
{
    const std::string plugin_key = "prune-context-plugin";
    const std::string capability = "Multi Infill";
    const std::string pattern_id = "prune-context-grid";
    ScopedPluginInfill cleanup{plugin_key, capability};

    REQUIRE(InfillFactory::register_plugin_infill(
        pattern_id, "Context Grid", plugin_key, capability, {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));

    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));

    // A model object override is the only place this plugin pattern is selected.
    DynamicPrintConfig object;
    object.set_key_value("sparse_infill_pattern", new ConfigOptionString(pattern_id));

    set_overrides(preset,
                  json::array({
                      override_entry("infill", capability, plugin_key, {{"sparse", 11}}),
                      override_entry("infill", "Stale Infill", "stale-infill-plugin"),
                  }));

    PluginOverridePruneContext context;
    context.configs  = {&object};
    context.complete = true;

    REQUIRE(prune_stale_plugin_overrides(preset, overrides_key(), context));

    const CapabilityConfigDocument overrides = stored_overrides(preset);
    const auto owned = overrides.find({PluginCapabilityType::Infill, capability, plugin_key});
    REQUIRE(owned.has_value());
    CHECK(owned->config == json{{"sparse", 11}});
    CHECK_FALSE(overrides.contains({PluginCapabilityType::Infill, "Stale Infill", "stale-infill-plugin"}));
}

TEST_CASE("Pruning drops stale infill overrides when the complete context uses only built-ins", "[PluginConfig][InfillPlugin]")
{
    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));

    DynamicPrintConfig object;
    object.set_key_value("sparse_infill_pattern", new ConfigOptionString("rectilinear"));

    set_overrides(preset, json::array({override_entry("infill", "Unused Infill", "infill-plugin")}));

    PluginOverridePruneContext context;
    context.configs  = {&object};
    context.complete = true;

    REQUIRE(prune_stale_plugin_overrides(preset, overrides_key(), context));

    CHECK(stored_overrides(preset).empty());
}

TEST_CASE("Pruning keeps infill overrides when a pattern in the complete context cannot be resolved", "[PluginConfig][InfillPlugin]")
{
    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));

    DynamicPrintConfig object;
    object.set_key_value("sparse_infill_pattern", new ConfigOptionString("prune-context-missing"));

    set_overrides(preset, json::array({override_entry("infill", "Unavailable Infill", "infill-plugin")}));

    PluginOverridePruneContext context;
    context.configs  = {&object};
    context.complete = true;

    CHECK_FALSE(prune_stale_plugin_overrides(preset, overrides_key(), context));
    CHECK(stored_overrides(preset).contains({PluginCapabilityType::Infill, "Unavailable Infill", "infill-plugin"}));
}

TEST_CASE("Pruning still drops stale overrides of capabilities not shared with object configs", "[PluginConfig][InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Pipeline Cap"}));
    set_overrides(config,
                  json::array({
                      override_entry("infill", "Multi Infill", "infill-plugin"),
                      override_entry("slicing-pipeline", "Pipeline Cap", "pipeline-plugin"),
                      override_entry("slicing-pipeline", "Stale Pipeline", "pipeline-plugin"),
                  }));

    REQUIRE(prune_stale_plugin_overrides(config, overrides_key()));

    const CapabilityConfigDocument overrides = stored_overrides(config);
    CHECK(overrides.contains({PluginCapabilityType::Infill, "Multi Infill", "infill-plugin"}));
    CHECK(overrides.contains({PluginCapabilityType::SlicingPipeline, "Pipeline Cap", "pipeline-plugin"}));
    CHECK_FALSE(overrides.contains({PluginCapabilityType::SlicingPipeline, "Stale Pipeline", "pipeline-plugin"}));
}

TEST_CASE("Pruning keeps a legacy override without a capability type in a complete context", "[PluginConfig][InfillPlugin]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("sparse_infill_pattern", new ConfigOptionString("gyroid"));
    set_overrides(config,
                  json::array({
                      legacy_override_entry("Legacy Infill", "legacy-plugin", {{"sparse", 3}}),
                      override_entry("infill", "Stale Infill", "stale-infill-plugin"),
                  }));

    PluginOverridePruneContext context;
    context.complete = true;

    // The typed infill entry is provably stale and removed; the typeless legacy entry cannot be
    // attributed to a capability and is kept.
    REQUIRE(prune_stale_plugin_overrides(config, overrides_key(), context));

    const CapabilityConfigDocument overrides = stored_overrides(config);
    CHECK(overrides.contains({PluginCapabilityType::Infill, "Legacy Infill", "legacy-plugin"}));
    CHECK_FALSE(overrides.contains({PluginCapabilityType::Infill, "Stale Infill", "stale-infill-plugin"}));
}
