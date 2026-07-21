#ifndef slic3r_PluginResolver_hpp_
#define slic3r_PluginResolver_hpp_

#include <libslic3r/Config.hpp>   // PluginCapabilityRef, parse_capability_ref
#include <libslic3r/Preset.hpp>   // Preset::Type
#include <libslic3r/PresetBundle.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp> // PluginCapabilityId, PluginCapabilityType
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

// A plugin capability referenced by an active preset but not currently available (installed and
// loadable) on this machine.
struct MissingPlugin
{
    PluginCapabilityRef ref;  // parsed "name;uuid;capability"
    std::string recovery_url; // OrcaCloud search URL (local plugins only)
    std::string opt;
    Preset::Type opt_type;
    PluginCapabilityType type{PluginCapabilityType::Unknown};
};

// Rebuild one preset type's missing-plugin set from its "plugins" manifest, comparing each ref
// against the live catalog and loaded/enabled capabilities. A null/empty manifest clears the set.
// Only TYPE_PRINT, TYPE_PRINTER and TYPE_FILAMENT are tracked.
void refresh_missing_plugins(Preset::Type type, const ConfigOptionStrings* manifest, const Preset* preset = nullptr);
void refresh_missing_plugins(const PresetBundle& preset_bundle);

// Aggregate queries across all tracked preset types. Each entry is a full "name;uuid;capability"
// reference. Cloud refs carry a non-empty UUID; local refs do not.
std::vector<MissingPlugin> get_missing_cloud_plugins();
std::vector<MissingPlugin> get_missing_local_plugins();
bool                     has_missing_plugins();

// Installed-but-inactive: the plugin has a local package but is not loaded, or is loaded with the
// capability disabled. Resolved locally by loading and/or enabling — no download.
std::vector<MissingPlugin> get_inactive_plugins();
bool                     has_inactive_plugins();

// Broken references: the plugin is installed AND loaded but does not provide the referenced
// capability at all (renamed/removed/outdated plugin). Activation cannot fix these.
std::vector<MissingPlugin> get_broken_plugins();
bool                     has_broken_plugins();

// Optional progress hook for the cloud install worker. All three callbacks fire on the worker
// thread; implementations must only touch thread-safe state or marshal to the UI thread.
struct PluginInstallProgress
{
    // Fired before each plugin's install begins. `index` is 0-based; `total` is the plugin count.
    std::function<void(const std::string& name, std::size_t index, std::size_t total)> on_plugin_begin;
    // Polled between plugins; returning true stops the loop before the next plugin starts.
    std::function<bool()> is_cancelled;
    // Fired exactly once when the loop ends (all installed, failed, or cancelled).
    std::function<void()> on_finished;
};

// Subscribe, install and load the cloud refs on a detached worker; failures are reported through a
// non-blocking notification. Local refs are ignored — they go through the browser flow below.
void resolve_missing_plugins(const std::vector<std::string>& refs,
                             PluginInstallProgress progress = {});

// Load each referenced plugin and/or enable its disabled capabilities. Local only — no network. The
// loads run on a background worker that waits for them and then re-validates the plate, clearing the
// notification (or reclassifying the ref as broken if the plugin does not provide the capability).
void resolve_inactive_plugins(const std::vector<std::string>& refs);

// Opens the OrcaCloud plugin hub (a search when there is exactly one ref, the hub otherwise).
void open_missing_plugins_on_cloud(const std::vector<std::string>& local_refs);

std::string create_full_ref(const PluginCapabilityRef& ref);
std::string resolve_recovery_url(const PluginCapabilityRef& ref);

// The capabilities `preset`'s "plugins" manifest declares AND that one of its plugin-backed options
// (ConfigOptionDef::is_plugin_backed) currently references: a manifest entry nobody points at is not
// in use. Pure preset logic — the catalog and loader are not consulted. Empty for untracked types.
std::vector<PluginCapabilityRef> referenced_capabilities(Preset::Type type, const Preset& preset);
std::vector<PluginCapabilityId> capabilities_in_use(Preset::Type type, const Preset& preset);

// The referenced capabilities of the active preset(s) of `type` that are loaded right now: the set
// that can actually be configured. Missing and broken refs are absent, having no instance to ask for
// a config UI or defaults. A loaded-but-disabled capability IS listed — it still has stored config
// worth editing.
std::vector<PluginCapabilityId> capabilities_in_use(const PresetBundle& preset_bundle, Preset::Type type);

bool check_capability_in_use(const std::string& capability_refs);

} // namespace Slic3r

#endif
