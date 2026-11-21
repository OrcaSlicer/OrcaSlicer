#pragma once

#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define PLUGIN_CONFIG_DIR "config.json"

namespace Slic3r {

class ConfigBase;
class DynamicConfig;
struct CapabilityConfigEntry
{
    PluginCapabilityId id;
    std::string        plugin_version;
    nlohmann::json     config = nlohmann::json::object();
};

class CapabilityConfigDocument
{
public:
    static constexpr const char* KeyEntries = "config";

    static CapabilityConfigDocument from_root_json(const nlohmann::json& root);
    static CapabilityConfigDocument from_entries(const nlohmann::json& entries);

    std::optional<CapabilityConfigEntry> find(const PluginCapabilityId& id) const;
    bool                                 contains(const PluginCapabilityId& id) const;
    bool                                 upsert(CapabilityConfigEntry entry);
    bool                                 erase(const PluginCapabilityId& id);
    // Drops every entry whose (type, name) is not in `referenced`, e.g. capabilities a preset's
    // plugin-backed options no longer name. An entry whose type is in `preserved_types` is kept
    // regardless, for callers that cannot prove it stale: an unresolved value, an incomplete
    // pruning context, or a legacy entry without a capability type. Returns true if anything was
    // removed.
    bool                                 prune_unreferenced(const std::set<std::pair<PluginCapabilityType, std::string>>& referenced,
                                                            const std::set<PluginCapabilityType>&                         preserved_types = {});
    bool                                 empty() const;
    nlohmann::json                       serialize_entries() const;
    nlohmann::json                       root_json() const;

private:
    std::map<PluginCapabilityId, nlohmann::json> m_entries;
    std::vector<nlohmann::json>                  m_opaque_entries;
};

bool parse_plugin_overrides(const std::string& raw, CapabilityConfigDocument& document, std::string& error);
std::string serialize_plugin_overrides(const CapabilityConfigDocument& document);

// Additional configs that can select plugin-backed settings outside the edited preset: model
// object, volume and layer-range configs, and the plate config. They contribute to the references
// pruning checks, so an override is not deleted while one of them still selects its capability.
// Each config must expose the print config definitions (ModelConfig::get() does), so its
// plugin-backed options can be recognized. The pointers must stay valid for the call.
struct PluginOverridePruneContext
{
    std::vector<const ConfigBase*> configs;

    // True only when `configs` contains every scope that can select a plugin-backed setting. Infill
    // overrides are preserved when this is false: the edited preset alone cannot prove them stale,
    // because a model object, volume or layer range may still select a plugin pattern. A caller that
    // can enumerate all those scopes may set this and enable normal infill pruning.
    bool complete = false;
};

// Drops override entries for capabilities no longer named by any plugin-backed option in `config`
// or in `context.configs` (e.g. slicing_pipeline_plugin cleared or pointed at another capability)
// and writes the result back to `overrides_key`. Infill pattern IDs are resolved to their owning
// capability through InfillFactory; an unresolved ID conservatively preserves infill overrides.
// Legacy entries without a capability type are always kept because they cannot be attributed to a
// capability. Call it wherever such an option changes, so a saved preset never carries
// configuration for a capability it no longer references. Returns true if `config` was modified, so
// a caller holding a GUI field over `overrides_key` knows to refresh it.
bool prune_stale_plugin_overrides(DynamicConfig&                     config,
                                  const std::string&                 overrides_key,
                                  const PluginOverridePruneContext& context = {});

struct EffectiveCapabilityConfig
{
    PluginCapabilityId id;
    nlohmann::json     config = nlohmann::json::object();

    bool        has_preset_override = false;
    bool        has_base_config     = false;
    std::string stored_plugin_version;
    std::string running_plugin_version;
};

struct MutationResult
{
    bool                      ok      = false;
    bool                      changed = false;
    std::string               error;
    EffectiveCapabilityConfig effective;
};

class PresetPluginConfigService
{
public:
    EffectiveCapabilityConfig get_effective_config(const CapabilityConfigDocument& overrides,
                                                   const PluginCapabilityId& id) const;
    MutationResult            set_preset_override(CapabilityConfigDocument& overrides,
                                                  const PluginCapabilityId& id,
                                                  const nlohmann::json& value) const;
    MutationResult            remove_preset_override(CapabilityConfigDocument& overrides,
                                                     const PluginCapabilityId& id) const;
};

EffectiveCapabilityConfig active_capability_config(const PluginCapabilityId& id);

class PluginConfig
{
public:
    static const std::string plugin_config_file() { return (boost::filesystem::path(get_orca_plugins_dir()) / PLUGIN_CONFIG_DIR).string(); }
    void load();
    bool save();

    void save_config(const CapabilityConfigEntry& config);

    bool store_capability_config(const PluginCapabilityId& id, const nlohmann::json& config);
    bool erase_capability_config(const PluginCapabilityId& id);

    std::optional<CapabilityConfigEntry> get_config(const PluginCapabilityId& id) const;
    bool has_config(const PluginCapabilityId& id) const;

    bool dirty() const;

    // The capability type as the plugin dialogs show it, translated; plugin_capability_type_display_name()
    // is the English name the logs use.
    static std::string capability_type_label(PluginCapabilityType type);
    static nlohmann::json capabilities_payload(const std::vector<PluginCapabilityId>& caps);
    static nlohmann::json get_config_response(const PluginCapabilityId& id);
    static nlohmann::json save_config_response(const PluginCapabilityId& id, const nlohmann::json& config);
    static nlohmann::json restore_config_response(const PluginCapabilityId& id);

private:
    mutable std::mutex m_mutex;
    CapabilityConfigDocument m_document;
    bool m_dirty = false;
};

nlohmann::json capability_get_config(const PluginCapabilityInterface& capability);
std::string capability_get_config_version(const PluginCapabilityInterface& capability);
bool capability_save_config(const PluginCapabilityInterface& capability, const nlohmann::json& config);

} // namespace Slic3r
