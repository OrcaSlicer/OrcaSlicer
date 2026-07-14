#ifndef slic3r_PluginManager_hpp_
#define slic3r_PluginManager_hpp_

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <libslic3r/Config.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include <pybind11/embed.h>

#include "CloudPluginService.hpp"
#include "PluginFsUtils.hpp"
#include "PluginDescriptor.hpp"
#include "PluginLoader.hpp"

namespace Slic3r {

class OrcaCloudServiceAgent;

// Identity of a single capability, as published to lifecycle subscribers. Purely a message
// payload — capabilities are looked up by (plugin_key, name) linear scan, so unlike the registry
// key this replaces, it needs no hash and no equality.
struct PluginCapabilityId
{
    PluginCapabilityType type = PluginCapabilityType::Unknown;
    std::string          name;
    std::string          plugin_key; // owning package
};

// One discovered plugin package: one .py/.whl file -> one descriptor + one Python module +
// N materialized capabilities.
//
// PluginManager holds every discovered plugin in a single std::vector<Plugin>, loaded or not;
// module == nullptr means the package was discovered but is not loaded. "What exists" and
// "what is live" are therefore the same vector, distinguished by a null check.
//
// A capability carries its own name, type and enable flag (PluginCapabilityInterface), cached by
// the loader at materialization, and its owning package via audit_plugin_key(). Capability state
// therefore lives exactly as long as the capability: a package that is not loaded has none. The
// only durable record is the .install_state.json sidecar, which the loader seeds enable flags from.
// The descriptor holds package state only.
struct Plugin
{
    PluginDescriptor descriptor;       // All plugin state and metadata lives here.
    PyObject*        module = nullptr; // Python module object, shared by all capabilities. nullptr => not loaded.

    // Materialized capability instances. Empty unless loaded.
    std::vector<std::shared_ptr<PluginCapabilityInterface>> capabilities;

    Plugin()                         = default;
    Plugin(const Plugin&)            = delete;
    Plugin& operator=(const Plugin&) = delete;

    // Move transfers module ownership; the moved-from package must not Py_DECREF it. Move
    // assignment is required: the manager's vector is erased from and reordered.
    Plugin(Plugin&& other) noexcept
        : descriptor(std::move(other.descriptor)), module(other.module), capabilities(std::move(other.capabilities))
    {
        other.module = nullptr;
    }

    Plugin& operator=(Plugin&& other) noexcept
    {
        if (this != &other) {
            release_module();
            descriptor   = std::move(other.descriptor);
            module       = other.module;
            capabilities = std::move(other.capabilities);
            other.module = nullptr;
        }
        return *this;
    }

    ~Plugin() { release_module(); }

    bool is_loaded() const { return module != nullptr; }

    // Drops the module reference. If the interpreter is already finalized there is no safe way to
    // touch the object — acquiring the GIL after Py_Finalize is undefined — so the reference is
    // deliberately leaked instead. Defined in PluginManager.cpp.
    void release_module();
};

// The single owner of all plugin state.
//
// Every discovered plugin — valid or not, loaded or not — lives in one std::vector<Plugin>;
// module == nullptr simply means "not loaded". Discovery (PluginFsUtils) and loading
// (plugin_loader) are stateless services this class drives; all mutable state, all locking and all
// async orchestration are here.
//
// Two rules the API is built around:
//
//  1. Never hand out a reference into m_plugins. The vector reallocates on push_back, and a
//     detached load worker can push while the GUI reads. Descriptor queries therefore return
//     snapshots by value, capability queries return shared_ptrs, and Plugin& is internal only.
//
//  2. Never acquire the GIL under m_mutex. Every lookup and filter reads the values the loader
//     cached on the capability (name()/type()/is_enabled()) and never calls get_name()/get_type(),
//     which dispatch into Python — doing so under the registry lock would invert the lock order
//     against plugin dispatch on the slicing threads. No user callback runs under m_mutex either.
class PluginManager
{
public:
    using PluginLifecycleCompleteFn = std::function<void(const std::string& /*plugin_key*/)>;
    using CapabilityLifecycleFn     = std::function<void(const PluginCapabilityId&)>;

    static PluginManager& instance();

    ~PluginManager();

    // Initialize the plugin system, eagerly starting the embedded Python interpreter on the main thread.
    bool initialize();

    // Stop discovery and unload Python plugin objects before Python finalizes.
    void shutdown();

    // Reject new plugin loads. Called early in app teardown, before shutdown() drains.
    void set_shutting_down();

    // ── Discovery ───────────────────────────────────────────────────────────────────────────
    // Discover and scan plugins from the standard directories (manifest-only, no Python loading).
    // Runs on a worker thread when async = true.
    //
    // Discovery MERGES: a plugin that is currently loaded keeps its module and capabilities, and
    // only its descriptor is refreshed. `clear` drops entries that are no longer on disk (loaded
    // ones are always kept — removing them would strand a live Python module).
    void discover_plugins(bool async = false, bool clear = false);
    // Manually trigger a manifest-only rescan. Blocks until discovery is complete.
    void rescan_plugins();

    bool is_discovery_complete() const;
    bool is_discovery_in_progress() const;
    std::string get_discovery_error() const;
    bool wait_for_discovery(std::chrono::milliseconds timeout, std::string& error) const;

    // ── Descriptors (package metadata; snapshots, safe to hold) ──────────────────────────────
    // Valid packages only by default; include_invalid adds the packages whose manifest could not
    // be parsed (descriptor.is_invalid_package()).
    std::vector<PluginDescriptor> get_plugin_descriptors(bool include_invalid = false) const;
    // Packages that materialize at least one capability of this type. A package's capability types
    // are only known once it has been loaded, so a never-loaded package never matches.
    std::vector<PluginDescriptor> get_plugin_descriptors(PluginCapabilityType type) const;
    bool try_get_plugin_descriptor(const std::string& plugin_key, PluginDescriptor& out) const;
    // Same, but only for packages that are loadable (i.e. not an invalid package).
    bool try_get_valid_plugin_descriptor(const std::string& plugin_key, PluginDescriptor& out) const;
    // Packages whose .install_state.json marks them for auto-load.
    std::vector<std::string> get_enabled_plugin_keys() const;
    // The package owning a loaded capability, for the by-name dispatch path.
    bool try_get_plugin_descriptor_for_capability(const std::string& capability_name,
                                                  PluginCapabilityType type,
                                                  PluginDescriptor& out) const;

    // ── Capability instances (dispatch only; shared_ptr, safe to hold) ───────────────────────
    std::vector<std::shared_ptr<PluginCapabilityInterface>> get_plugin_capabilities(
        const std::string& plugin_key = "",                            // "" => all plugins
        PluginCapabilityType type     = PluginCapabilityType::Unknown, // Unknown => all types
        bool only_enabled             = true) const;
    std::shared_ptr<PluginCapabilityInterface> get_plugin_capability(const std::string& plugin_key,
                                                                     const std::string& capability_name,
                                                                     PluginCapabilityType type = PluginCapabilityType::Unknown,
                                                                     bool only_enabled         = true) const;
    std::shared_ptr<PluginCapabilityInterface> get_plugin_capability(const std::string& capability_name,
                                                                     PluginCapabilityType type = PluginCapabilityType::Unknown,
                                                                     bool only_enabled         = true) const;

    // ── Lifecycle ───────────────────────────────────────────────────────────────────────────
    // Asynchronous: spawns a detached worker. capabilities_to_enable selects which capabilities
    // come up enabled (empty => all), though a flag already persisted for a capability wins.
    void load_plugin(const std::string& plugin_key, bool skip_deps = false, std::vector<std::string> capabilities_to_enable = {});
    bool unload_plugin(const std::string& plugin_key);
    void unload_all_plugins();
    void unload_cloud_plugins();
    bool is_plugin_loaded(const std::string& plugin_key) const;
    bool is_plugin_load_in_progress(const std::string& plugin_key) const;
    void wait_for_all_plugin_loads() const;
    bool wait_for_all_plugin_loads(std::chrono::milliseconds timeout) const; // false on timeout
    bool wait_for_plugin_load(const std::string& plugin_key, std::chrono::milliseconds timeout, std::string& error) const;
    bool cancel_plugin_load(const std::string& plugin_key);
    std::string get_plugin_load_error(const std::string& plugin_key) const;

    // ── Enable state (writes through to the .install_state.json sidecar) ─────────────────────
    void set_capability_enabled(const std::string& plugin_key, const std::string& capability_name, bool enabled);

    // ── Misc ────────────────────────────────────────────────────────────────────────────────
    // The plugin's [tool.orcaslicer.plugin.settings] table (empty if the plugin is unknown). This will be replaced once the config is merged in.
    std::map<std::string, std::string> get_plugin_settings(const std::string& plugin_key) const;
    // Sets the cloud user whose _subscribed/{user_id} directory is scanned and installed into.
    void set_cloud_user(const std::string& user_id);

    // ── Callbacks ───────────────────────────────────────────────────────────────────────────
    void subscribe_on_load_callback(PluginLifecycleCompleteFn fn);
    void subscribe_on_unload_callback(PluginLifecycleCompleteFn fn);
    void subscribe_on_capability_load_callback(CapabilityLifecycleFn fn);
    void subscribe_on_capability_unload_callback(CapabilityLifecycleFn fn);

    // ── Install / delete / cloud ─────────────────────────────────────────────────────────────
    void set_cloud_agent(std::shared_ptr<OrcaCloudServiceAgent> agent) { m_cloud_service.set_cloud_agent(std::move(agent)); }

    bool install_plugin(const boost::filesystem::path& filepath, std::string& error);
    bool install_plugin(const boost::filesystem::path& filepath, PluginDescriptor& plugin_descriptor, std::string& error);
    bool inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                      PluginDescriptor& plugin_descriptor,
                                      bool& existing_installation,
                                      std::string& error) const;

    bool set_plugin_error(const std::string& plugin_key, std::string error);
    bool clear_plugin_error(const std::string& plugin_key);

    void fetch_plugins_from_cloud(std::vector<std::string>* out_not_found = nullptr, std::vector<std::string>* out_unauthorized = nullptr);
    void update_cloud_catalog(const std::vector<PluginDescriptor>& cloud_list);
    void clear_cloud_plugin_catalog();

    bool download_and_install_cloud_plugin(const std::string& plugin_key, const std::string& version, std::string& error);
    bool subscribe_and_install_cloud_plugin(const std::string& plugin_key, std::string& error);
    // If the version is empty, take the latest version.
    bool update_cloud_plugin(const std::string& plugin_key, std::string& error, std::string version = "");

    bool delete_plugin(const std::string& plugin_key, std::string& error);
    bool unsubscribe_cloud_plugin(const std::string& plugin_key, std::string& error);
    bool delete_and_unsubscribe_cloud_plugin(const std::string& plugin_key, std::string& error);
    bool delete_mine_plugin_from_cloud(const std::string& plugin_key, std::string& error);
    bool delete_mine_local_and_cloud_plugin(const std::string& plugin_key, std::string& error);

private:
    PluginManager()                                = default;
    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    enum class CallbackType { Load, Unload };

    // Caller holds m_mutex. The returned pointer is invalidated by any push_back into m_plugins,
    // so it must never escape the lock.
    Plugin* find_plugin_locked(const std::string& plugin_key);
    const Plugin* find_plugin_locked(const std::string& plugin_key) const;

    void load_plugin_impl(const std::string& plugin_key, bool skip_deps, const std::vector<std::string>& capabilities_to_enable);
    // Caller holds m_mutex. Non-empty return means the load must be abandoned.
    std::string check_registry_locked(const std::string& plugin_key, const Plugin& candidate) const;

    void run_discovery(bool async, bool clear);
    void run_discovery_task(bool clear);
    void merge_discovered_plugins(std::vector<PluginDescriptor> discovered, bool clear);

    bool cancel_plugin_load_locked(const std::string& plugin_key);
    bool is_plugin_load_cancelled_locked(const std::string& plugin_key) const;
    void notify_plugin_load_state_changed(bool changed);
    void release_load_slot(const std::string& plugin_key);

    // Snapshot subscribers under m_mutex so they can be invoked without holding it.
    std::vector<PluginLifecycleCompleteFn> copy_callbacks(CallbackType type) const;
    std::vector<CapabilityLifecycleFn> copy_capability_callbacks(CallbackType type) const;
    void run_on_load_callbacks(const std::string& plugin_key);
    void run_on_unload_callbacks(const std::string& plugin_key);
    void run_on_capability_load_callbacks(const PluginCapabilityId& id);
    void run_on_capability_unload_callbacks(const PluginCapabilityId& id);
    void clear_callbacks();

    // Writes the sidecar for a loaded plugin (enabled=true plus the current per-capability flags).
    void write_loaded_plugin_install_state(const std::string& plugin_key);

    bool finalize_cloud_plugin_removal(const PluginDescriptor& plugin, bool keep_local, std::string& error);
    bool delete_installed_plugin_package(const PluginDescriptor& plugin, std::string& error);
    bool keep_installed_plugin_as_local(const PluginDescriptor& plugin_descriptor, std::string& error);

    bool m_initialized = false;
    CloudPluginService m_cloud_service;

    mutable std::mutex m_mutex;

    // Every discovered plugin, loaded or not. module == nullptr => not loaded.
    std::vector<Plugin> m_plugins;

    std::unordered_set<std::string> m_load_in_progress;
    // Keys whose in-flight load has been cancelled. Cancellation does NOT remove the key from
    // m_load_in_progress: the detached worker is still inside the loader touching Python, and it
    // releases its own slot only once it has unwound. Dropping the key here would let
    // wait_for_all_plugin_loads() return early, after which shutdown() unloads everything and
    // GUI_App finalizes the interpreter — leaving the live worker to run Python against it.
    std::unordered_set<std::string> m_cancelled;
    std::map<std::string, std::string> m_load_errors;
    mutable std::condition_variable m_load_cv;

    std::map<CallbackType, std::vector<PluginLifecycleCompleteFn>> m_callbacks;
    std::map<CallbackType, std::vector<CapabilityLifecycleFn>> m_capability_callbacks;

    bool m_discovery_complete    = false;
    bool m_discovery_in_progress = false;
    std::string m_discovery_error;
    mutable std::condition_variable m_discovery_cv;

    std::string m_cloud_user_id;
    std::atomic<bool> m_shutting_down{false};
};

// Resolve each configured capability reference to a loaded capability of type T and run `execute`.
template<typename T>
void execute_capabilities_from_refs(const ConfigOptionStrings& capabilities,
                                    const ConfigOptionStrings* plugins,
                                    PluginCapabilityType type,
                                    std::function<void(std::shared_ptr<T>, const PluginCapabilityRef&)> execute)
{
    PluginManager& plugin_mgr = PluginManager::instance();

    // Log prefix derived from the capability type so each capability family (Printer connection,
    // Slicing Pipeline, ...) tags its dispatch diagnostics with its own display name.
    const std::string tag = plugin_capability_type_display_name(type);

    const bool has_any = std::any_of(capabilities.values.begin(), capabilities.values.end(),
                                     [](const std::string& s) { return !s.empty(); });
    if (has_any && !plugin_mgr.wait_for_all_plugin_loads(std::chrono::seconds(10))) {
        BOOST_LOG_TRIVIAL(warning) << tag << ": timed out waiting for plugin loads; unresolved capabilities will be skipped";
    }

    for (const std::string& capability : capabilities.values) {
        if (capability.empty())
            continue;

        std::optional<PluginCapabilityRef> ref;
        if (plugins != nullptr) {
            for (const std::string& plugin_ref : plugins->values) {
                auto parsed = Slic3r::parse_capability_ref(plugin_ref);
                if (parsed && parsed->capability_name == capability) {
                    ref = std::move(parsed);
                    break;
                }
            }
        }

        if (!ref) {
            BOOST_LOG_TRIVIAL(warning) << tag << ": no plugin reference found for capability '" << capability << "'; skipping";
            continue;
        }

        const std::string cap_name   = ref->capability_name;
        const std::string plugin_key = ref->uuid.empty() ? ref->name : ref->uuid;

        // only_enabled = false so that "not loaded" and "loaded but disabled" stay distinguishable
        // and each keeps its own diagnostic.
        auto cap = plugin_mgr.get_plugin_capability(plugin_key, cap_name, type, /*only_enabled=*/false);
        if (!cap) {
            BOOST_LOG_TRIVIAL(warning) << tag << ": no loaded capability '" << cap_name << "' for plugin '" << plugin_key << "'; skipping";
            continue;
        }

        if (!cap->is_enabled()) {
            BOOST_LOG_TRIVIAL(warning) << tag << ": capability '" << cap_name << "' for plugin '" << plugin_key
                                       << "' is disabled; skipping";
            continue;
        }

        auto plugin_capability = std::dynamic_pointer_cast<T>(cap);
        if (!plugin_capability) {
            BOOST_LOG_TRIVIAL(warning) << tag << ": capability '" << cap_name << "' (plugin_key=" << cap->audit_plugin_key()
                                       << ") is not a " << plugin_capability_type_to_string(type) << "; skipping";
            continue;
        }

        execute(plugin_capability, ref.value());
    }
}

} // namespace Slic3r

#endif /* slic3r_PluginManager_hpp_ */
