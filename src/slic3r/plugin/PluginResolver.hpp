#ifndef slic3r_PluginResolver_hpp_
#define slic3r_PluginResolver_hpp_

#include <libslic3r/Config.hpp>   // PluginCapabilityRef, parse_capability_ref
#include <libslic3r/Preset.hpp>   // Preset::Type
#include <libslic3r/PresetBundle.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp> // PluginCapabilityType
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

// Rebuild the missing-plugin set owned by a single preset type from that preset's "plugins"
// manifest, comparing each ref against the live plugin catalog and loaded/enabled capabilities. A
// null/empty manifest clears the set for that type. Only TYPE_PRINT (process), TYPE_PRINTER
// (machine) and TYPE_FILAMENT are tracked; other types are ignored.
void refresh_missing_plugins(Preset::Type type, const ConfigOptionStrings* manifest, const Preset* preset = nullptr);
void refresh_missing_plugins(const PresetBundle& preset_bundle);

// Aggregate queries across all tracked preset types. Each entry is a full "name;uuid;capability"
// reference. Cloud refs carry a non-empty UUID; local refs do not.
std::vector<MissingPlugin> get_missing_cloud_plugins();
std::vector<MissingPlugin> get_missing_local_plugins();
bool                     has_missing_plugins();

// Installed-but-inactive capabilities: the plugin has a local package but the referenced capability
// is not active because the plugin is not loaded, or it is loaded but the capability is disabled.
// Resolved locally by loading the plugin and/or enabling the capability — no download.
std::vector<MissingPlugin> get_inactive_plugins();
bool                     has_inactive_plugins();

// Broken references: the plugin is installed AND loaded but does not provide the referenced
// capability at all (renamed/removed/outdated plugin). Activation cannot fix these; surfaced as an
// informational notification pointing the user at OrcaCloud to update the plugin.
std::vector<MissingPlugin> get_broken_plugins();
bool                     has_broken_plugins();

// Resolution actions invoked from the missing-plugin notifications:
// - cloud refs are subscribed/installed and loaded on a detached worker thread; failures are
//   reported through a non-blocking notification. Non-cloud refs are ignored.

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

// Cloud refs only; local refs are handled via the browser flow. `progress` is optional — a
// default-constructed value preserves the previous silent behavior.
void resolve_missing_plugins(const std::vector<std::string>& refs,
                             PluginInstallProgress progress = {});

// Activate inactive plugins: load each referenced plugin (passing the capabilities to enable) and/or
// enable already-loaded-but-disabled capabilities. Local only — no network. The loads run on a
// background worker that waits for them and then re-validates the plate, clearing the notification
// (or reclassifying the ref as broken if the loaded plugin turns out not to provide the capability).
void resolve_inactive_plugins(const std::vector<std::string>& refs);

// - local refs are opened on the OrcaCloud plugin hub (search when exactly one ref, hub otherwise).
void open_missing_plugins_on_cloud(const std::vector<std::string>& local_refs);

std::string create_full_ref(const PluginCapabilityRef& ref);
std::string resolve_recovery_url(const PluginCapabilityRef& ref);

} // namespace Slic3r

#endif
