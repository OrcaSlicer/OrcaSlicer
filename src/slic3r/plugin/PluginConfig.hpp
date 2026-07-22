#pragma once

#include <libslic3r/Utils.hpp>
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

class Preset;
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
    // plugin-backed options no longer name. Returns true if anything was removed.
    bool                                 prune_unreferenced(const std::set<std::pair<PluginCapabilityType, std::string>>& referenced);
    bool                                 empty() const;
    nlohmann::json                       serialize_entries() const;
    nlohmann::json                       root_json() const;

private:
    std::map<PluginCapabilityId, nlohmann::json> m_entries;
    std::vector<nlohmann::json>                  m_opaque_entries;
};

inline constexpr const char* PLUGIN_OVERRIDES_OPTION_KEY = "plugin_config_overrides";

std::string plugin_overrides_of(const Preset& preset);
bool parse_plugin_overrides(const std::string& raw, CapabilityConfigDocument& document, std::string& error);
std::string serialize_plugin_overrides(const CapabilityConfigDocument& document);

// Drops plugin_config_overrides entries for capabilities no longer named by any plugin-backed
// option's current value in `config` (e.g. slicing_pipeline_plugin cleared or switched to a
// different capability), and writes the result back if anything changed. Called wherever a
// plugin-backed option's value changes, so a saved preset never carries configuration for a
// capability it no longer references. Returns true if `config` was modified, so a caller holding a
// GUI field over PLUGIN_OVERRIDES_OPTION_KEY knows it must refresh that field's displayed value.
bool prune_stale_plugin_overrides(DynamicConfig& config);

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
