#include "PluginManager.hpp"

#include <memory>
#include <pybind11/embed.h>

#include "PluginFsUtils.hpp"
#include "PluginHooks.hpp"
#include "PythonFileUtils.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginBridge.hpp"

#include "OrcaCloudServiceAgent.hpp"
#include "libslic3r/Semver.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/pluginTypes/script/ScriptPluginCapability.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

// Sentinel error returned by the registry pre-check when the load was cancelled while it ran.
// A cancelled load records no error and fires no failure path — it just unwinds.
const char* const LOAD_CANCELLED = "__plugin_load_cancelled__";

} // namespace

void Plugin::release_module()
{
    if (module == nullptr && plugin_sys_paths.empty() && plugin_modules.empty())
        return;

    PythonInterpreter::instance().unload_module(module, module_name, plugin_sys_paths, plugin_modules);
    module = nullptr;
    module_name.clear();
    plugin_sys_paths.clear();
    plugin_modules.clear();
}

PluginManager& PluginManager::instance()
{
    // Touch the interpreter first so its static outlives this one: ~Plugin needs it alive to
    // release module, sys.modules, and sys.path ownership safely.
    PythonInterpreter::instance();
    static PluginManager inst;
    return inst;
}

PluginManager::~PluginManager() { shutdown(); }

bool PluginManager::initialize()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;
        m_shutting_down.store(false, std::memory_order_release);
    }

    // Initialize the Python interpreter eagerly on the main thread. CPython must be initialized
    // from the main thread; calling Py_InitializeFromConfig from a background thread (e.g. the
    // load_plugin worker) causes heap corruption in CPython internals.
    PythonInterpreter& interpreter = PythonInterpreter::instance();
    if (!interpreter.is_initialized() && !interpreter.initialize()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Failed to initialize Python interpreter: " << interpreter.last_error();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;
        m_initialized = true;
    }

    // Install the libslic3r hooks (capability resolver, slicing-pipeline dispatcher).
    // Uninstalled in shutdown() before the interpreter finalizes.
    plugin_hooks::install();

    // Persist auto-load / capability state to each plugin's .install_state.json sidecar.
    // On load: write enabled=true plus the current capability flags. On unload: flip enabled=false.
    // The on-unload callback is skipped during shutdown, so app exit does not wipe the auto-load list.
    subscribe_on_load_callback([this](const std::string& key) { write_loaded_plugin_install_state(key); });
    subscribe_on_unload_callback([this](const std::string& key) {
        if (m_shutting_down.load(std::memory_order_acquire))
            return;
        PluginDescriptor descriptor;
        if (!try_get_plugin_descriptor(key, descriptor) || descriptor.plugin_root.empty())
            return;
        const boost::filesystem::path root(descriptor.plugin_root);
        PluginInstallState state;
        if (read_install_state(root, state)) {
            state.enabled = false;
            write_install_state(root, state);
        }
    });

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Plugin manager initialized";

    return true;
}

void PluginManager::set_shutting_down() { m_shutting_down.store(true, std::memory_order_release); }

void PluginManager::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool idle_and_empty = m_load_in_progress.empty() && std::none_of(m_plugins.begin(), m_plugins.end(),
                                                                               [](const Plugin& plugin) { return plugin.is_loaded(); });
        if (!m_initialized && !m_discovery_in_progress && idle_and_empty)
            return;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": PluginManager shutdown enter";

    // Detach the libslic3r hooks first so nothing dispatches into Python while (or after) plugins
    // unload. Callers stop background slicing before this.
    plugin_hooks::uninstall();

    // Reject new plugin loads before we drain.
    set_shutting_down();

    std::string wait_error;
    if (!wait_for_discovery(std::chrono::milliseconds::max(), wait_error) && !wait_error.empty())
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Plugin discovery did not finish cleanly during shutdown: " << wait_error;

    // Wait for any in-progress plugin loads. Cancelled loads keep their slot until their worker
    // has actually unwound, so this cannot return while one is still executing Python.
    wait_for_all_plugin_loads();

    unload_all_plugins();
    PythonPluginBridge::instance().clear_pending_captures();

    // Drop the lifecycle subscriptions taken out during initialize(). Without this a second
    // initialize() in the same process re-subscribes the same callbacks on top of the old ones,
    // and every load would then write the install-state sidecar once per duplicate.
    clear_callbacks();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_initialized = false;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": PluginManager shutdown exit";
}

// ── Registry helpers ────────────────────────────────────────────────────────────────────────

Plugin* PluginManager::find_plugin_locked(const std::string& plugin_key)
{
    const auto it = std::find_if(m_plugins.begin(), m_plugins.end(),
                                 [&plugin_key](const Plugin& plugin) { return plugin.descriptor.plugin_key == plugin_key; });
    return it == m_plugins.end() ? nullptr : &*it;
}

const Plugin* PluginManager::find_plugin_locked(const std::string& plugin_key) const
{
    const auto it = std::find_if(m_plugins.begin(), m_plugins.end(),
                                 [&plugin_key](const Plugin& plugin) { return plugin.descriptor.plugin_key == plugin_key; });
    return it == m_plugins.end() ? nullptr : &*it;
}

std::string PluginManager::check_registry_locked(const std::string& plugin_key, const Plugin& candidate) const
{
    const Plugin* entry = find_plugin_locked(plugin_key);
    if (entry != nullptr && entry->is_loaded())
        return "Plugin package is already loaded: " + plugin_key;

    // A capability name must be unique per type across every loaded package.
    for (const auto& capability : candidate.capabilities) {
        if (!capability)
            continue;
        for (const Plugin& other : m_plugins) {
            if (!other.is_loaded() || other.descriptor.plugin_key == plugin_key)
                continue;
            for (const auto& existing : other.capabilities) {
                if (existing && existing->type() == capability->type() && existing->name() == capability->name())
                    return "Capability collision for type " + plugin_capability_type_to_string(capability->type()) + " and name '" +
                           capability->name() + "'";
            }
        }
    }

    return {};
}

// ── Discovery ───────────────────────────────────────────────────────────────────────────────

void PluginManager::discover_plugins(bool async, bool clear)
{
    if (!initialize())
        return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_discovery_in_progress) {
            BOOST_LOG_TRIVIAL(debug) << "Plugin discovery already running";
            return;
        }
        if (clear) {
            for (Plugin& plugin : m_plugins)
                plugin.descriptor.clear_error();
        }
        m_discovery_in_progress = true;
        m_discovery_complete    = false;
        m_discovery_error.clear();
    }

    run_discovery(async, clear);
}

void PluginManager::rescan_plugins()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Rescanning plugins...";

    std::string wait_error;
    wait_for_discovery(std::chrono::milliseconds::max(), wait_error);

    discover_plugins(/*async=*/false, /*clear=*/true);
}

void PluginManager::run_discovery(bool async, bool clear)
{
    auto task = [this, clear]() { run_discovery_task(clear); };

    if (async)
        std::thread(std::move(task)).detach();
    else
        task();
}

void PluginManager::run_discovery_task(bool clear)
{
    std::string error;

    try {
        std::string cloud_user_id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cloud_user_id = m_cloud_user_id;
        }

        const std::vector<std::string> dirs      = get_plugin_directories(cloud_user_id);
        std::vector<PluginDescriptor> discovered = discover_plugin_packages(dirs, error);
        merge_discovered_plugins(std::move(discovered), clear);
    } catch (const std::exception& ex) {
        error = std::string("Plugin discovery failed: ") + ex.what();
        BOOST_LOG_TRIVIAL(error) << error;
    } catch (...) {
        error = "Plugin discovery failed: unknown error";
        BOOST_LOG_TRIVIAL(error) << error;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_discovery_error       = std::move(error);
        m_discovery_complete    = true;
        m_discovery_in_progress = false;
    }
    m_discovery_cv.notify_all();
}

void PluginManager::merge_discovered_plugins(std::vector<PluginDescriptor> discovered, bool clear)
{
    std::vector<std::string> seen;
    seen.reserve(discovered.size());

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (PluginDescriptor& descriptor : discovered) {
            if (descriptor.plugin_key.empty())
                continue;

            if (std::find(seen.begin(), seen.end(), descriptor.plugin_key) != seen.end()) {
                const std::string duplicate_error = "Duplicate plugin key discovered: " + descriptor.plugin_key;
                if (Plugin* existing = find_plugin_locked(descriptor.plugin_key)) {
                    existing->descriptor.set_error(duplicate_error);
                    BOOST_LOG_TRIVIAL(error) << duplicate_error << " (" << existing->descriptor.entry_path << ", "
                                             << descriptor.entry_path << ")";
                } else {
                    BOOST_LOG_TRIVIAL(error) << duplicate_error;
                }
                continue;
            }

            seen.push_back(descriptor.plugin_key);

            Plugin* existing = find_plugin_locked(descriptor.plugin_key);
            if (existing == nullptr) {
                Plugin plugin;
                plugin.descriptor = std::move(descriptor);
                m_plugins.push_back(std::move(plugin));
                continue;
            }

            // A manifest-only rescan has nothing to say about capabilities, so the live module and
            // instances are left alone.
            existing->descriptor = std::move(descriptor);
        }

        if (!clear)
            return;
    }

    // Unloading may call Python and lifecycle subscribers may re-enter the manager, so never do it
    // while holding m_mutex. unload_and_erase_if() retries until no matching entry is loaded at the
    // moment of erase, in case another caller starts a load between the initial snapshot and the
    // teardown.
    unload_and_erase_if(
        [&seen](const Plugin& plugin) { return std::find(seen.begin(), seen.end(), plugin.descriptor.plugin_key) == seen.end(); });
}

void PluginManager::unload_and_erase_if(const std::function<bool(const Plugin&)>& should_remove,
                                        const std::function<void()>& after_erase_locked)
{
    for (;;) {
        std::vector<std::string> unload_keys;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const Plugin& plugin : m_plugins) {
                if (should_remove(plugin) && plugin.is_loaded())
                    unload_keys.push_back(plugin.descriptor.plugin_key);
            }
        }

        if (unload_keys.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Keep the check and erase in one critical section so a concurrent load cannot make a
            // live entry reach vector compaction and move-assignment.
            bool loaded_entry_found = false;
            for (const Plugin& plugin : m_plugins) {
                if (should_remove(plugin) && plugin.is_loaded()) {
                    loaded_entry_found = true;
                    break;
                }
            }
            if (loaded_entry_found)
                continue;

            m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(), should_remove), m_plugins.end());
            if (after_erase_locked)
                after_erase_locked();
            break;
        }
        for (const std::string& key : unload_keys)
            unload_plugin(key);
    }
}

bool PluginManager::is_discovery_complete() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_discovery_complete;
}

bool PluginManager::is_discovery_in_progress() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_discovery_in_progress;
}

std::string PluginManager::get_discovery_error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_discovery_error;
}

bool PluginManager::wait_for_discovery(std::chrono::milliseconds timeout, std::string& error) const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_discovery_in_progress)
        return true;

    const auto done = [this]() { return !m_discovery_in_progress; };

    if (timeout == std::chrono::milliseconds::max()) {
        m_discovery_cv.wait(lock, done);
        return true;
    }

    if (!m_discovery_cv.wait_for(lock, timeout, done)) {
        error = "Plugin discovery is still running";
        return false;
    }

    return true;
}

// ── Descriptors ─────────────────────────────────────────────────────────────────────────────

std::vector<PluginDescriptor> PluginManager::get_plugin_descriptors(bool include_invalid) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<PluginDescriptor> result;
    result.reserve(m_plugins.size());
    for (const Plugin& plugin : m_plugins) {
        if (!include_invalid && plugin.descriptor.is_invalid_package())
            continue;
        result.push_back(plugin.descriptor);
    }
    return result;
}

bool PluginManager::try_get_plugin_descriptor(const std::string& plugin_key, PluginDescriptor& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const Plugin* plugin = find_plugin_locked(plugin_key);
    if (plugin == nullptr)
        return false;

    out = plugin->descriptor;
    return true;
}

bool PluginManager::try_get_valid_plugin_descriptor(const std::string& plugin_key, PluginDescriptor& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const Plugin* plugin = find_plugin_locked(plugin_key);
    if (plugin == nullptr || plugin->descriptor.is_invalid_package())
        return false;

    out = plugin->descriptor;
    return true;
}

std::vector<std::string> PluginManager::get_enabled_plugin_keys() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::string> keys;
    for (const Plugin& plugin : m_plugins) {
        if (plugin.descriptor.enabled && plugin.descriptor.has_local_package())
            keys.push_back(plugin.descriptor.plugin_key);
    }
    return keys;
}

bool PluginManager::try_get_plugin_descriptor_for_capability(const std::string& capability_name,
                                                             PluginCapabilityType type,
                                                             PluginDescriptor& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const Plugin& plugin : m_plugins) {
        for (const auto& capability : plugin.capabilities) {
            if (!capability || capability->name() != capability_name)
                continue;
            if (type != PluginCapabilityType::Unknown && capability->type() != type)
                continue;
            out = plugin.descriptor;
            return true;
        }
    }

    return false;
}

// ── Capability instances ────────────────────────────────────────────────────────────────────

std::vector<std::shared_ptr<PluginCapabilityInterface>> PluginManager::get_plugin_capabilities(const std::string& plugin_key,
                                                                                               PluginCapabilityType type,
                                                                                               bool only_enabled) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::shared_ptr<PluginCapabilityInterface>> result;
    for (const Plugin& plugin : m_plugins) {
        if (!plugin_key.empty() && plugin.descriptor.plugin_key != plugin_key)
            continue;

        for (const auto& capability : plugin.capabilities) {
            if (!capability)
                continue;
            if (type != PluginCapabilityType::Unknown && capability->type() != type)
                continue;
            if (only_enabled && !capability->is_enabled())
                continue;
            result.push_back(capability);
        }
    }
    return result;
}

std::shared_ptr<PluginCapabilityInterface> PluginManager::get_plugin_capability(const std::string& plugin_key,
                                                                                const std::string& capability_name,
                                                                                PluginCapabilityType type,
                                                                                bool only_enabled) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const Plugin* plugin = find_plugin_locked(plugin_key);
    if (plugin == nullptr)
        return nullptr;

    for (const auto& capability : plugin->capabilities) {
        if (!capability || capability->name() != capability_name)
            continue;
        if (type != PluginCapabilityType::Unknown && capability->type() != type)
            continue;
        if (only_enabled && !capability->is_enabled())
            continue;
        return capability;
    }

    return nullptr;
}

std::shared_ptr<PluginCapabilityInterface> PluginManager::get_plugin_capability(const std::string& capability_name,
                                                                                PluginCapabilityType type,
                                                                                bool only_enabled) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const Plugin& plugin : m_plugins) {
        for (const auto& capability : plugin.capabilities) {
            if (!capability || capability->name() != capability_name)
                continue;
            if (type != PluginCapabilityType::Unknown && capability->type() != type)
                continue;
            if (only_enabled && !capability->is_enabled())
                continue;
            return capability;
        }
    }

    return nullptr;
}

std::map<std::string, std::string> PluginManager::get_plugin_settings(const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const Plugin* plugin = find_plugin_locked(plugin_key);
    if (plugin == nullptr || !plugin->is_loaded())
        return {};

    return plugin->descriptor.settings;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────────────────────

bool PluginManager::is_plugin_loaded(const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Plugin* plugin = find_plugin_locked(plugin_key);
    return plugin != nullptr && plugin->is_loaded();
}

bool PluginManager::is_plugin_load_in_progress(const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_load_in_progress.count(plugin_key) > 0;
}

void PluginManager::wait_for_all_plugin_loads() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_load_cv.wait(lock, [this]() { return m_load_in_progress.empty(); });
}

bool PluginManager::wait_for_all_plugin_loads(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_load_cv.wait_for(lock, timeout, [this]() { return m_load_in_progress.empty(); });
}

bool PluginManager::wait_for_plugin_load(const std::string& plugin_key, std::chrono::milliseconds timeout, std::string& error) const
{
    std::unique_lock<std::mutex> lock(m_mutex);

    const auto done = [this, &plugin_key]() { return m_load_in_progress.count(plugin_key) == 0; };

    if (timeout == std::chrono::milliseconds::max()) {
        m_load_cv.wait(lock, done);
    } else if (!m_load_cv.wait_for(lock, timeout, done)) {
        error = "Plugin load is still in progress";
        return false;
    }

    const auto it = m_load_errors.find(plugin_key);
    if (it != m_load_errors.end()) {
        error = it->second;
        return false;
    }

    return true;
}

std::string PluginManager::get_plugin_load_error(const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_load_errors.find(plugin_key);
    return it == m_load_errors.end() ? std::string{} : it->second;
}

bool PluginManager::cancel_plugin_load(const std::string& plugin_key)
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cancelled = cancel_plugin_load_locked(plugin_key);
    }

    notify_plugin_load_state_changed(cancelled);
    return cancelled;
}

bool PluginManager::cancel_plugin_load_locked(const std::string& plugin_key)
{
    if (m_load_in_progress.count(plugin_key) == 0)
        return false;

    m_cancelled.insert(plugin_key);
    m_load_errors.erase(plugin_key);
    return true;
}

bool PluginManager::is_plugin_load_cancelled_locked(const std::string& plugin_key) const { return m_cancelled.count(plugin_key) > 0; }

void PluginManager::notify_plugin_load_state_changed(bool changed)
{
    if (changed)
        m_load_cv.notify_all();
}

void PluginManager::release_load_slot(const std::string& plugin_key)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        changed = m_load_in_progress.erase(plugin_key) > 0;
        m_cancelled.erase(plugin_key);
    }
    notify_plugin_load_state_changed(changed);
}

void PluginManager::cancel_and_wait_for_capabilities(
    const std::vector<std::shared_ptr<PluginCapabilityInterface>>& capabilities)
{
    for (const auto& capability : capabilities) {
        if (!capability || !capability->is_enabled())
            continue;

        try {
            capability->on_cancelled();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "Plugin on_cancelled failed for '" << capability->audit_plugin_key() << "/"
                                       << capability->name() << "': " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "Plugin on_cancelled failed for '" << capability->audit_plugin_key() << "/"
                                       << capability->name() << "'";
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const bool all_released = std::all_of(capabilities.begin(), capabilities.end(), [](const auto& capability) {
            return !capability || capability->ref_count() == 0;
        });
        if (all_released)
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (const auto& capability : capabilities) {
        if (!capability)
            continue;

        const int refs = capability->ref_count();
        if (refs > 0) {
            BOOST_LOG_TRIVIAL(warning) << "Forcing unload of plugin capability with " << refs
                                       << " active call(s): " << capability->audit_plugin_key() << "/" << capability->name();
        }
    }
}

void PluginManager::load_plugin(const std::string& plugin_key, bool skip_deps, std::vector<std::string> capabilities_to_enable)
{
    std::string plugin_id = plugin_key;

    bool exists           = false;
    bool invalid          = false;
    bool already_loaded   = false;
    bool load_in_progress = false;
    bool load_slot_claimed = false;
    bool shutting_down     = false;
    std::string invalid_error;
    std::vector<std::string> loaded_capability_names;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const Plugin* plugin = find_plugin_locked(plugin_key);
        if (plugin != nullptr) {
            exists         = true;
            plugin_id      = plugin->descriptor.plugin_key;
            invalid        = plugin->descriptor.is_invalid_package();
            already_loaded = plugin->is_loaded();
            invalid_error  = plugin->descriptor.normalized_error();
            if (already_loaded) {
                for (const auto& capability : plugin->capabilities)
                    if (capability)
                        loaded_capability_names.push_back(capability->name());
            }
        }

        load_in_progress = m_load_in_progress.count(plugin_id) > 0;
        shutting_down   = m_shutting_down.load(std::memory_order_acquire);
        if (exists && !invalid && !already_loaded && !load_in_progress && !shutting_down) {
            m_load_in_progress.insert(plugin_id);
            m_load_errors.erase(plugin_id);
            load_slot_claimed = true;
        }
    }

    if (already_loaded) {
        // An empty request means "restore the persisted capability state". Only an explicit
        // capability request may change an already-loaded capability, e.g. when resolving an
        // inactive-plugin reference. Runs outside m_mutex.
        for (const std::string& capability_name : capabilities_to_enable) {
            if (std::find(loaded_capability_names.begin(), loaded_capability_names.end(), capability_name) !=
                loaded_capability_names.end())
                set_capability_enabled(plugin_id, capability_name, true);
        }

        run_on_load_callbacks(plugin_id);
        return;
    }

    if (load_in_progress)
        return;

    if (shutting_down) {
        BOOST_LOG_TRIVIAL(info) << "Plugin load rejected — shutting down: " << plugin_id;
        run_on_load_callbacks(plugin_id);
        return;
    }

    if (!exists || invalid) {
        std::string message;
        if (invalid) {
            message = "Plugin is invalid: " + plugin_id;
            if (!invalid_error.empty())
                message += " - " + invalid_error;
        } else {
            message = "Plugin not found: " + plugin_id;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_load_errors[plugin_id] = message;
            BOOST_LOG_TRIVIAL(error) << message;
        }

        if (invalid)
            set_plugin_error(plugin_id, message);

        run_on_load_callbacks(plugin_id);
        return;
    }

    if (!load_slot_claimed)
        return;

    clear_plugin_error(plugin_id);

    std::thread([this, plugin_id, skip_deps, capabilities_to_enable]() {
        auto fail_unexpected = [this, &plugin_id](std::string message) {
            BOOST_LOG_TRIVIAL(error) << "[load_plugin] Unexpected worker failure plugin=" << plugin_id << " error=" << message;
            set_plugin_error(plugin_id, message);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_load_errors[plugin_id] = std::move(message);
            }
        };

        try {
            load_plugin_impl(plugin_id, skip_deps, capabilities_to_enable);
        } catch (const std::exception& ex) {
            fail_unexpected(std::string("Unexpected plugin load failure: ") + ex.what());
        } catch (...) {
            fail_unexpected("Unexpected plugin load failure");
        }

        if (!m_shutting_down.load(std::memory_order_acquire))
            run_on_load_callbacks(plugin_id);

        // The worker owns its slot and releases it here, once it is done with Python for good —
        // including a load that was cancelled mid-flight, which is what keeps
        // wait_for_all_plugin_loads() blocking until the worker is really gone.
        release_load_slot(plugin_id);
    }).detach();
}

void PluginManager::load_plugin_impl(const std::string& plugin_key, bool skip_deps, const std::vector<std::string>& capabilities_to_enable)
{
    auto fail = [this, &plugin_key](std::string message) {
        BOOST_LOG_TRIVIAL(error) << "[load_plugin_impl] FAIL plugin=" << plugin_key << " error=" << message;
        set_plugin_error(plugin_key, message);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_load_errors[plugin_key] = std::move(message);
        }
    };

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        fail("Plugin manifest not found: " + plugin_key);
        return;
    }

    // Reject a duplicate package or a capability-name collision BEFORE on_load() runs, so a
    // rejected load costs no on_load/on_unload cycle. Also the point at which a cancellation that
    // arrived while the module was importing is noticed.
    auto registry_precheck = [this, &plugin_key](const Plugin& candidate) -> std::string {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (is_plugin_load_cancelled_locked(plugin_key))
            return LOAD_CANCELLED;
        return check_registry_locked(plugin_key, candidate);
    };

    Plugin plugin;
    std::string error;
    if (!plugin_loader::load(descriptor, skip_deps, capabilities_to_enable, registry_precheck, plugin, error)) {
        if (error == LOAD_CANCELLED)
            return; // cancelled: nothing materialized survives, and no error is recorded
        fail(std::move(error));
        return;
    }

    bool committed = false;
    bool cancelled = false;
    std::string registry_error;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        cancelled = is_plugin_load_cancelled_locked(plugin_key);
        if (!cancelled) {
            registry_error = check_registry_locked(plugin_key, plugin);
            if (registry_error.empty()) {
                Plugin* entry = find_plugin_locked(plugin_key);
                if (entry == nullptr) {
                    registry_error = "Plugin manifest not found: " + plugin_key;
                } else {
                    // Move everything via Plugin's own move assignment, but the package's
                    // descriptor (identity/metadata) belongs to the registry entry, not to the
                    // freshly-loaded `plugin` -- preserve it across the move.
                    PluginDescriptor entry_descriptor = std::move(entry->descriptor);
                    *entry                            = std::move(plugin);
                    entry->descriptor                 = std::move(entry_descriptor);
                    committed                         = true;
                }
            }
        }
    }

    if (!committed) {
        // Outside the lock: this runs on_unload() and DECREFs the module.
        plugin_loader::unload(plugin);
        if (!cancelled)
            fail(std::move(registry_error));
        return;
    }

    clear_plugin_error(plugin_key);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_load_errors.erase(plugin_key);
    }

    BOOST_LOG_TRIVIAL(info) << "[load_plugin_impl] SUCCESS plugin=" << plugin_key;
}

bool PluginManager::unload_plugin(const std::string& plugin_key)
{
    auto notify_unload_complete = [this, &plugin_key]() {
        if (!m_shutting_down.load(std::memory_order_acquire))
            run_on_unload_callbacks(plugin_key);
    };

    Plugin removed;
    bool cancelled = false;
    bool found     = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Cancel any in-progress load for this plugin so its worker discards the result.
        cancelled = cancel_plugin_load_locked(plugin_key);

        Plugin* plugin = find_plugin_locked(plugin_key);
        if (plugin != nullptr && plugin->is_loaded()) {
            found = true;

            // Take the live parts out of the entry via Plugin's own move assignment; the
            // descriptor stays behind, so the package remains discovered. Everything
            // capability-shaped is discarded with the capabilities — the user's enable choices
            // live on in the .install_state.json sidecar.
            PluginDescriptor kept_descriptor = std::move(plugin->descriptor);
            removed                          = std::move(*plugin);
            plugin->descriptor               = std::move(kept_descriptor);
        }
    }

    if (!found) {
        notify_plugin_load_state_changed(cancelled);
        return true;
    }

    notify_unload_complete();

    cancel_and_wait_for_capabilities(removed.capabilities);

    notify_plugin_load_state_changed(cancelled);

    // Teardown runs outside m_mutex: it calls into Python and must not hold the registry lock.
    plugin_loader::unload(removed);

    // The .install_state.json sidecar is flipped to enabled=false by the on-unload callback
    // (skipped during shutdown), so the auto-load list survives app exit.
    BOOST_LOG_TRIVIAL(info) << "Unloaded plugin: " << plugin_key;

    return true;
}

void PluginManager::unload_all_plugins()
{
    std::vector<Plugin> removed;
    std::vector<std::string> removed_keys;
    std::vector<std::shared_ptr<PluginCapabilityInterface>> capabilities;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Plugin& plugin : m_plugins) {
            if (!plugin.is_loaded())
                continue;

            // Take the live parts out via Plugin's own move; the descriptor stays behind, as in
            // unload_plugin() above.
            PluginDescriptor kept_descriptor = std::move(plugin.descriptor);
            Plugin           taken           = std::move(plugin);
            plugin.descriptor                = std::move(kept_descriptor);
            removed.push_back(std::move(taken));
            removed_keys.push_back(plugin.descriptor.plugin_key);
            capabilities.insert(capabilities.end(), removed.back().capabilities.begin(), removed.back().capabilities.end());
        }
    }

    // Bulk shutdown intentionally skips the normal unload notification path, but resources owned
    // by GUI subscribers still need to be torn down before the interpreter is finalized.
    for (const std::string& key : removed_keys)
        run_on_unload_callbacks(key);

    // Broadcast cancellation to every plugin before waiting. This keeps bulk unload from
    // starving a later plugin while an earlier plugin uses the full cancellation grace period.
    cancel_and_wait_for_capabilities(capabilities);

    for (Plugin& plugin : removed)
        plugin_loader::unload(plugin);
}

void PluginManager::unload_cloud_plugins()
{
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Plugin& plugin : m_plugins) {
            if (plugin.is_loaded() && plugin.descriptor.is_cloud_plugin())
                keys.push_back(plugin.descriptor.plugin_key);
        }
    }

    // Release m_mutex before unloading: unload_plugin() re-acquires it and runs plugin teardown
    // plus unload callbacks, which can re-enter the manager.
    for (const std::string& key : keys)
        unload_plugin(key);
}

// ── Enable state ────────────────────────────────────────────────────────────────────────────

void PluginManager::set_capability_enabled(const std::string& plugin_key, const std::string& capability_name, bool enabled)
{
    PluginCapabilityId changed;
    bool did_change = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        Plugin* plugin = find_plugin_locked(plugin_key);
        if (plugin == nullptr || !plugin->is_loaded())
            return;

        for (const auto& capability : plugin->capabilities) {
            if (!capability || capability->name() != capability_name || capability->is_enabled() == enabled)
                continue;
            capability->set_enabled(enabled);
            changed    = PluginCapabilityId{capability->type(), capability->name(), plugin_key};
            did_change = true;
            break;
        }
    }

    if (!did_change)
        return;

    write_loaded_plugin_install_state(plugin_key);

    if (enabled)
        run_on_capability_load_callbacks(changed);
    else
        run_on_capability_unload_callbacks(changed);
}

void PluginManager::write_loaded_plugin_install_state(const std::string& plugin_key)
{
    PluginDescriptor descriptor;
    std::vector<std::pair<std::string, bool>> capabilities;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const Plugin* plugin = find_plugin_locked(plugin_key);
        if (plugin == nullptr || !plugin->is_loaded())
            return;

        descriptor = plugin->descriptor;
        for (const auto& capability : plugin->capabilities)
            if (capability)
                capabilities.emplace_back(capability->name(), capability->is_enabled());
    }

    if (descriptor.plugin_root.empty())
        return;

    write_install_state(boost::filesystem::path(descriptor.plugin_root), descriptor, /*enabled=*/true, capabilities);
}

// ── Callbacks ───────────────────────────────────────────────────────────────────────────────

void PluginManager::subscribe_on_load_callback(PluginLifecycleCompleteFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks[CallbackType::Load].push_back(std::move(fn));
}

void PluginManager::subscribe_on_unload_callback(PluginLifecycleCompleteFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks[CallbackType::Unload].push_back(std::move(fn));
}

void PluginManager::subscribe_on_capability_load_callback(CapabilityLifecycleFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_capability_callbacks[CallbackType::Load].push_back(std::move(fn));
}

void PluginManager::subscribe_on_capability_unload_callback(CapabilityLifecycleFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_capability_callbacks[CallbackType::Unload].push_back(std::move(fn));
}

void PluginManager::clear_callbacks()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.clear();
    m_capability_callbacks.clear();
}

std::vector<PluginManager::PluginLifecycleCompleteFn> PluginManager::copy_callbacks(CallbackType type) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_callbacks.find(type);
    return it == m_callbacks.end() ? std::vector<PluginLifecycleCompleteFn>{} : it->second;
}

std::vector<PluginManager::CapabilityLifecycleFn> PluginManager::copy_capability_callbacks(CallbackType type) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_capability_callbacks.find(type);
    return it == m_capability_callbacks.end() ? std::vector<CapabilityLifecycleFn>{} : it->second;
}

// The run_on_*_callbacks below are called from detached load/unload workers. They copy the
// subscriber list under m_mutex and invoke it OUTSIDE the lock: subscribers re-enter the manager,
// and no callback may run while the registry lock is held.
void PluginManager::run_on_load_callbacks(const std::string& plugin_key)
{
    for (auto& fn : copy_callbacks(CallbackType::Load)) {
        try {
            fn(plugin_key);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin load completion callback failed for " << plugin_key << ": " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Plugin load completion callback failed for " << plugin_key;
        }
    }
}

void PluginManager::run_on_unload_callbacks(const std::string& plugin_key)
{
    for (auto& fn : copy_callbacks(CallbackType::Unload)) {
        try {
            fn(plugin_key);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin unload completion callback failed for " << plugin_key << ": " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Plugin unload completion callback failed for " << plugin_key << ": unknown error";
        }
    }
}

void PluginManager::run_on_capability_load_callbacks(const PluginCapabilityId& id)
{
    for (auto& fn : copy_capability_callbacks(CallbackType::Load)) {
        try {
            fn(id);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin capability load callback failed for " << id.plugin_key << "/" << id.name << ": "
                                     << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Plugin capability load callback failed for " << id.plugin_key << "/" << id.name;
        }
    }
}

void PluginManager::run_on_capability_unload_callbacks(const PluginCapabilityId& id)
{
    for (auto& fn : copy_capability_callbacks(CallbackType::Unload)) {
        try {
            fn(id);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin capability unload callback failed for " << id.plugin_key << "/" << id.name << ": "
                                     << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Plugin capability unload callback failed for " << id.plugin_key << "/" << id.name;
        }
    }
}

// ── Cloud user ──────────────────────────────────────────────────────────────────────────────

void PluginManager::set_cloud_user(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cloud_user_id = user_id;
}

// ── Errors ──────────────────────────────────────────────────────────────────────────────────

bool PluginManager::set_plugin_error(const std::string& plugin_key, std::string error)
{
    std::string wait_error;
    if (!wait_for_discovery(std::chrono::milliseconds::max(), wait_error))
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    Plugin* plugin = find_plugin_locked(plugin_key);
    if (plugin == nullptr)
        return false;

    plugin->descriptor.set_error(std::move(error));
    return true;
}

bool PluginManager::clear_plugin_error(const std::string& plugin_key)
{
    std::string wait_error;
    if (!wait_for_discovery(std::chrono::milliseconds::max(), wait_error))
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    Plugin* plugin = find_plugin_locked(plugin_key);
    if (plugin == nullptr || !plugin->descriptor.is_metadata_valid())
        return false;

    plugin->descriptor.clear_error();
    return true;
}

// ── Install / inspect ───────────────────────────────────────────────────────────────────────

bool PluginManager::inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                                 PluginDescriptor& plugin_descriptor,
                                                 bool& existing_installation,
                                                 std::string& error) const
{ return plugin_loader::inspect_local_plugin_package(filepath, plugin_descriptor, existing_installation, error); }

bool PluginManager::install_plugin(const boost::filesystem::path& filepath, std::string& error)
{
    PluginDescriptor descriptor{};
    return install_plugin(filepath, descriptor, error);
}

bool PluginManager::install_plugin(const boost::filesystem::path& filepath, PluginDescriptor& plugin_descriptor, std::string& error)
{
    error.clear();

    std::string wait_error;
    if (!wait_for_discovery(std::chrono::milliseconds::max(), wait_error)) {
        error = wait_error.empty() ? "Plugin discovery is still running" : wait_error;
        if (!plugin_descriptor.plugin_key.empty())
            set_plugin_error(plugin_descriptor.plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Plugin installation failed while waiting for discovery: " << error;
        return false;
    }

    std::string cloud_user_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cloud_user_id = m_cloud_user_id;
    }

    // A local overwrite must not leave the old module holding the package file or its Python
    // namespace while the installer replaces the package on disk.
    if (!plugin_descriptor.is_cloud_plugin()) {
        const std::string local_plugin_key = make_local_plugin_key(filepath.stem().string());
        if (is_plugin_loaded(local_plugin_key))
            unload_plugin(local_plugin_key);
    }

    if (!plugin_loader::install_plugin(filepath, cloud_user_id, plugin_descriptor, error)) {
        if (!plugin_descriptor.plugin_key.empty())
            set_plugin_error(plugin_descriptor.plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    if (!plugin_descriptor.plugin_key.empty())
        clear_plugin_error(plugin_descriptor.plugin_key);

    return true;
}

// ── Cloud overlay ───────────────────────────────────────────────────────────────────────────

namespace {

bool is_cloud_version_newer(const std::string& cloud_version, const std::string& local_version)
{
    auto cloud_parsed = Semver::parse(cloud_version);
    auto local_parsed = Semver::parse(local_version);
    if (cloud_parsed && local_parsed)
        return *cloud_parsed > *local_parsed;
    // Fall back to string comparison if semver parsing fails for either version.
    return cloud_version != local_version;
}

const char* const CLOUD_PLUGIN_NOT_FOUND_ERROR = "Plugin was not found in the cloud.";

} // namespace

void PluginManager::update_cloud_catalog(const std::vector<PluginDescriptor>& cloud_list)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Collected and appended after the scan: push_back into m_plugins would invalidate the
    // pointers the loop is holding.
    std::vector<PluginDescriptor> new_entries;

    for (const PluginDescriptor& cloud_entry : cloud_list) {
        const std::string cloud_uuid = cloud_entry.cloud_uuid();
        const std::string cloud_key  = cloud_entry.plugin_key;
        if (cloud_uuid.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Skipping cloud plugin record without UUID";
            continue;
        }

        const auto matches_cloud_descriptor = [&cloud_key, &cloud_uuid](const PluginDescriptor& entry) {
            if (!cloud_key.empty() && entry.plugin_key == cloud_key)
                return true;
            return entry.is_cloud_plugin() && entry.cloud_uuid() == cloud_uuid;
        };

        const auto existing = std::find_if(m_plugins.begin(), m_plugins.end(), [&matches_cloud_descriptor](const Plugin& plugin) {
            return matches_cloud_descriptor(plugin.descriptor);
        });

        if (existing == m_plugins.end()) {
            PluginDescriptor normalized_entry = cloud_entry;
            if (normalized_entry.plugin_key.empty())
                normalized_entry.plugin_key = cloud_uuid;
            if (!normalized_entry.cloud.has_value())
                normalized_entry.cloud = CloudPluginState{cloud_uuid, false, false, false};
            else if (normalized_entry.cloud->uuid.empty())
                normalized_entry.cloud->uuid = cloud_uuid;
            new_entries.push_back(std::move(normalized_entry));
            continue;
        }

        PluginDescriptor& entry = existing->descriptor;

        const PluginDescriptor local_entry  = entry;
        const std::string installed_version = entry.installed_version;
        const std::string latest_version    = cloud_entry.latest_available_version();
        const std::string local_plugin_root = entry.plugin_root;
        const std::string local_entry_path  = entry.entry_path;
        const bool local_metadata_ok        = entry.is_metadata_valid();
        const bool has_local_package        = entry.has_local_package();
        const std::string previous_error    = entry.error;
        // The cloud does not know about the local auto-load flag, which comes from the sidecar.
        const bool enabled = entry.enabled;

        entry             = cloud_entry;
        entry.plugin_root = local_plugin_root;
        entry.entry_path  = local_entry_path;
        if (has_local_package)
            apply_plugin_metadata_fallbacks(entry, local_entry);
        if (entry.plugin_key.empty())
            entry.plugin_key = cloud_uuid;
        entry.metadata_valid = has_local_package ? local_metadata_ok : cloud_entry.metadata_valid;
        entry.error          = previous_error;
        entry.enabled        = enabled;
        if (!entry.cloud.has_value())
            entry.cloud = CloudPluginState{cloud_uuid, has_local_package, false, false};
        else if (entry.cloud->uuid.empty())
            entry.cloud->uuid = cloud_uuid;

        entry.cloud->installed = has_local_package;
        // The installed version is the source of truth read back from the install-state sidecar
        // (the version fetched from the cloud at install time), not the local manifest/PEP723
        // header. The header may be stale — the cloud can bump the version without the header
        // changing — which would otherwise make an already-updated plugin appear perpetually out
        // of date.
        entry.installed_version       = has_local_package ? installed_version : std::string{};
        entry.cloud->update_available = has_local_package && local_metadata_ok && !installed_version.empty() && !latest_version.empty() &&
                                        is_cloud_version_newer(latest_version, installed_version);
        if (entry.normalized_error() == CLOUD_PLUGIN_NOT_FOUND_ERROR)
            entry.clear_error();
    }

    for (PluginDescriptor& descriptor : new_entries) {
        Plugin plugin;
        plugin.descriptor = std::move(descriptor);
        m_plugins.push_back(std::move(plugin));
    }
}

void PluginManager::clear_cloud_plugin_catalog()
{
    // Cloud entries may own live Python modules. Unload them before erasing their vector entries,
    // and do so outside m_mutex because both Python teardown and lifecycle callbacks can re-enter
    // the manager.
    unload_and_erase_if(
        [](const Plugin& plugin) { return plugin.descriptor.is_cloud_plugin(); },
        [this]() {
            // A local plugin can still be carrying a stale "not found in the cloud" error from
            // when it was cloud-tracked.
            for (Plugin& plugin : m_plugins) {
                if (plugin.descriptor.normalized_error() == CLOUD_PLUGIN_NOT_FOUND_ERROR)
                    plugin.descriptor.clear_error();
            }
        });

    BOOST_LOG_TRIVIAL(info) << "Cleared cloud plugin catalog entries";
}

void PluginManager::fetch_plugins_from_cloud(std::vector<std::string>* out_not_found, std::vector<std::string>* out_unauthorized)
{
    if (!m_cloud_service.can_fetch_cloud_plugins())
        return;

    std::vector<PluginDescriptor> cloud_list{};
    std::vector<std::string> not_found{}, unauthorized{};
    if (!m_cloud_service.fetch_manifests_into_descriptors(cloud_list, not_found, unauthorized)) {
        if (wxTheApp != nullptr) {
            GUI::wxGetApp().CallAfter([] {
                if (GUI::wxGetApp().is_closing())
                    return;
                GUI::Plater* plater = GUI::wxGetApp().plater();
                if (plater == nullptr || GUI::wxGetApp().imgui() == nullptr || !GUI::wxGetApp().imgui()->display_initialized())
                    return;
                plater->get_notification_manager()->push_notification(GUI::NotificationType::CustomNotification,
                                                                      GUI::NotificationManager::NotificationLevel::WarningNotificationLevel,
                                                                      "Failed to fetch plugins from the cloud. See logs for details.");
            });
        }
    }

    update_cloud_catalog(cloud_list);

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Clear the previous cloud verdicts before re-applying the fresh ones.
        for (Plugin& plugin : m_plugins) {
            PluginDescriptor& entry = plugin.descriptor;
            if (!entry.is_cloud_plugin())
                continue;
            entry.set_unauthorized(false);
            if (entry.normalized_error() == CLOUD_PLUGIN_NOT_FOUND_ERROR)
                entry.clear_error();
        }

        for (const std::string& uuid : not_found) {
            for (Plugin& plugin : m_plugins) {
                PluginDescriptor& entry = plugin.descriptor;
                if (!entry.is_cloud_plugin() || entry.cloud_uuid() != uuid)
                    continue;
                if (!entry.has_local_package())
                    entry.set_error(CLOUD_PLUGIN_NOT_FOUND_ERROR);
                break;
            }
        }

        for (const std::string& uuid : unauthorized) {
            for (Plugin& plugin : m_plugins) {
                PluginDescriptor& entry = plugin.descriptor;
                if (!entry.is_cloud_plugin() || entry.cloud_uuid() != uuid)
                    continue;
                entry.set_unauthorized(true);
                if (entry.cloud.has_value())
                    entry.cloud->update_available = false;
                break;
            }
        }
    }

    if (out_not_found)
        *out_not_found = std::move(not_found);
    if (out_unauthorized)
        *out_unauthorized = std::move(unauthorized);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Cloud plugin fetch: " << cloud_list.size() << " plugins returned";
}

bool PluginManager::subscribe_and_install_cloud_plugin(const std::string& plugin_key, std::string& error)
{
    error.clear();

    const std::string plugin_uuid = is_uuid(plugin_key) ? plugin_key : std::string{};
    if (plugin_uuid.empty()) {
        error = "Cloud plugin key is missing UUID.";
        return false;
    }
    if (!m_cloud_service.can_fetch_cloud_plugins()) {
        error = "Sign in to OrcaCloud to install this plugin.";
        return false;
    }

    PluginDescriptor descriptor;
    bool found = try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_cloud_plugin();
    if (!found) {
        // The plugin may already be subscribed or owned while the local catalog is stale.
        fetch_plugins_from_cloud();
        found = try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_cloud_plugin();
    }

    if (!found) {
        if (!m_cloud_service.request_cloud_subscribe(plugin_uuid, error))
            return false;

        fetch_plugins_from_cloud();
        found = try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_cloud_plugin();
        if (!found) {
            error = "Subscribed cloud plugin was not returned by OrcaCloud.";
            return false;
        }
    }

    if (descriptor.has_local_package())
        return true;

    return download_and_install_cloud_plugin(descriptor.plugin_key, descriptor.version, error);
}

bool PluginManager::download_and_install_cloud_plugin(const std::string& plugin_key, const std::string& version, std::string& error)
{
    error.clear();

    CloudPluginDownload download;
    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to find plugin key " << plugin_key;
        return false;
    }

    clear_plugin_error(plugin_key);

    const std::string requested_version = version.empty() ? descriptor.version : version;
    if (!m_cloud_service.download_cloud_plugin(descriptor, requested_version, download, error)) {
        if (error.empty())
            error = "Failed to download cloud plugin.";
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Ensure cloud plugins install to _subscribed/<user_id>/
    if (auto agent = m_cloud_service.get_cloud_agent()) {
        const std::string user_id = agent->get_user_id();
        if (!user_id.empty())
            set_cloud_user(user_id);
    }

    // Record the version we just fetched from the cloud so install_plugin persists it to the
    // install-state sidecar (the source of truth for the installed cloud version) instead of the
    // local manifest/PEP723 header version.
    descriptor.installed_version = requested_version;

    if (!install_plugin(download.package_path, descriptor, error)) {
        if (error.empty())
            error = "Failed to install cloud plugin.";
        set_plugin_error(plugin_key, error);
        boost::system::error_code ec;
        boost::filesystem::remove(download.package_path, ec);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    PluginDescriptor updated_descriptor = descriptor;
    if (!requested_version.empty())
        updated_descriptor.version = requested_version;
    // The version just downloaded and installed is now the locally installed version.
    updated_descriptor.installed_version = requested_version.empty() ? descriptor.version : requested_version;
    updated_descriptor.enabled = true;
    if (updated_descriptor.cloud.has_value()) {
        updated_descriptor.cloud->installed        = true;
        updated_descriptor.cloud->update_available = false;
        updated_descriptor.cloud->unauthorized     = false;
    }
    updated_descriptor.clear_error();
    updated_descriptor.set_unauthorized(false);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Plugin* entry = find_plugin_locked(plugin_key);
        if (entry == nullptr) {
            error = "Plugin Manifest not found.";
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Cloud plugin " << plugin_key
                                       << " downloaded successfully but failed to update plugin manifest. Manifest not found in catalog.";
        } else {
            entry->descriptor = std::move(updated_descriptor);
        }
    }
    if (!error.empty()) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Cloud plugin " << plugin_key << " installed successfully";
    return true;
}

bool PluginManager::update_cloud_plugin(const std::string& plugin_key, std::string& error, std::string version)
{
    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = "Only cloud plugins can be updated.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    // An empty version means "update the current plugin". If there is no update available, the
    // intention is unclear.
    if (version.empty()) {
        if (descriptor.cloud->update_available) {
            version = descriptor.latest_available_version();
        } else {
            error = "Trying to update plugin with no available update. Version is empty.";
            set_plugin_error(plugin_key, error);
            return false;
        }
    }

    clear_plugin_error(plugin_key);

    if (descriptor.has_local_package()) {
        boost::filesystem::path resolved_root;
        bool resolved_allowed_plugin_root = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            resolved_allowed_plugin_root = resolve_allowed_plugin_root(descriptor, get_plugin_directories(m_cloud_user_id),
                                                                       "Refusing to delete a plugin outside the known plugin directories.",
                                                                       resolved_root, error);
        }

        if (!resolved_allowed_plugin_root) {
            set_plugin_error(plugin_key, error);
            return false;
        }

        unload_plugin(plugin_key);

        if (!delete_plugin_root(resolved_root, plugin_key, error)) {
            set_plugin_error(plugin_key, error);
            return false;
        }
    }

    if (!download_and_install_cloud_plugin(plugin_key, version, error)) {
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error;
        return false;
    }

    clear_plugin_error(plugin_key);
    return true;
}

// ── Delete / unsubscribe ────────────────────────────────────────────────────────────────────

bool PluginManager::delete_installed_plugin_package(const PluginDescriptor& plugin, std::string& error)
{
    boost::filesystem::path resolved_root;
    bool resolved_allowed_plugin_root = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        resolved_allowed_plugin_root = resolve_allowed_plugin_root(plugin, get_plugin_directories(m_cloud_user_id),
                                                                   "Refusing to delete a plugin outside the known plugin directories.",
                                                                   resolved_root, error);
    }
    if (!resolved_allowed_plugin_root)
        return false;

    unload_plugin(plugin.plugin_key);

    if (!delete_plugin_root(resolved_root, plugin.plugin_key, error))
        return false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(),
                                       [&plugin](const Plugin& entry) { return entry.descriptor.plugin_key == plugin.plugin_key; }),
                        m_plugins.end());
    }

    return true;
}

bool PluginManager::keep_installed_plugin_as_local(const PluginDescriptor& plugin_descriptor, std::string& error)
{
    namespace fs = boost::filesystem;

    boost::filesystem::path resolved_root;

    bool resolved_allowed_plugin_root = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        resolved_allowed_plugin_root = resolve_allowed_plugin_root(plugin_descriptor, get_plugin_directories(m_cloud_user_id),
                                                                   "Refusing to update a plugin outside the known plugin directories.",
                                                                   resolved_root, error);
    }

    if (!resolved_allowed_plugin_root)
        return false;

    const std::string old_key = plugin_descriptor.plugin_key;

    // Generate a new local key from the entry file stem (cloud -> local conversion).
    const std::string entry_stem = fs::path(plugin_descriptor.entry_path).stem().string();
    const std::string new_key    = make_local_plugin_key(!entry_stem.empty() ? entry_stem : resolved_root.stem().string());

    PluginDescriptor local_descriptor = plugin_descriptor;
    local_descriptor.plugin_key       = new_key;
    local_descriptor.cloud            = std::nullopt;
    local_descriptor.clear_error();

    // Re-key the entry in place, carrying the live module/capabilities (if any) with it. The
    // capabilities' audit key must follow, since it is what identifies their owning package.
    std::vector<PluginCapabilityId> rekeyed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        Plugin* entry = find_plugin_locked(old_key);
        if (entry == nullptr) {
            error = "Plugin manifest not found: " + old_key;
            return false;
        }

        if (find_plugin_locked(new_key) != nullptr && new_key != old_key) {
            error = "Cannot keep plugin local: local plugin key already exists: " + new_key;
            BOOST_LOG_TRIVIAL(warning) << error;
            return false;
        }

        // Preserve the sidecar's package and capability choices while changing only its origin.
        // Re-keying an installed plugin must not silently re-enable capabilities.
        PluginInstallState install_state;
        if (!read_install_state(resolved_root, install_state)) {
            install_state.installed_version = !local_descriptor.installed_version.empty()
                                                  ? local_descriptor.installed_version
                                                  : local_descriptor.version;
            install_state.enabled = local_descriptor.enabled;
        }
        install_state.installed_from = "local";
        install_state.plugin_name    = local_descriptor.name;
        install_state.cloud_uuid.clear();
        if (install_state.installed_version.empty())
            install_state.installed_version = local_descriptor.version;
        if (!write_install_state(resolved_root, install_state)) {
            error = "Failed to update plugin install state: " + (resolved_root / INSTALL_STATE_FILE).string();
            return false;
        }

        entry->descriptor.plugin_key = new_key;
        entry->descriptor.cloud.reset();
        entry->descriptor.clear_error();

        for (const auto& capability : entry->capabilities) {
            if (capability)
                capability->set_audit_plugin_key(new_key);
        }

        for (const auto& capability : entry->capabilities) {
            if (capability && capability->is_enabled())
                rekeyed.push_back(PluginCapabilityId{capability->type(), capability->name(), new_key});
        }
    }

    // Subscribers key their own state by plugin_key. Publish the identity transition after the
    // registry update and outside m_mutex so they can safely query the manager.
    for (const PluginCapabilityId& id : rekeyed) {
        run_on_capability_unload_callbacks(PluginCapabilityId{id.type, id.name, old_key});
        run_on_capability_load_callbacks(id);
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Transitioned plugin from " << old_key << " to " << new_key;

    return true;
}

bool PluginManager::finalize_cloud_plugin_removal(const PluginDescriptor& plugin, bool keep_local, std::string& error)
{
    // Shared by all four cloud-removal entrypoints after the cloud-side request succeeds. Handles
    // the common local follow-up of keeping a detached local copy, deleting local files, or
    // dropping a cloud-only row from the catalog.
    if (keep_local && plugin.has_local_package()) {
        if (!keep_installed_plugin_as_local(plugin, error))
            return false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Removed cloud tracking, kept local copy: " << plugin.plugin_key;
        return true;
    }

    if (plugin.has_local_package()) {
        if (!delete_installed_plugin_package(plugin, error))
            return false;
        // Re-sync the cloud catalog so observers/UI see the updated cloud list after the local
        // package has been removed.
        fetch_plugins_from_cloud();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Deleted local package after cloud removal: " << plugin.plugin_key;
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(),
                                       [&plugin](const Plugin& entry) { return entry.descriptor.plugin_key == plugin.plugin_key; }),
                        m_plugins.end());
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Removed cloud-only plugin from catalog: " << plugin.plugin_key;
    return true;
}

bool PluginManager::delete_plugin(const std::string& plugin_key, std::string& error)
{
    if (!wait_for_discovery(std::chrono::milliseconds::max(), error))
        return false;

    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        return false;
    }

    if (!delete_installed_plugin_package(descriptor, error)) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Deleted plugin: " << plugin_key;
    return true;
}

bool PluginManager::unsubscribe_cloud_plugin(const std::string& plugin_key, std::string& error)
{
    if (!wait_for_discovery(std::chrono::milliseconds::max(), error))
        return false;

    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = "Only cloud plugins can be unsubscribed.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (descriptor.cloud && descriptor.cloud->is_mine) {
        error = "Cannot unsubscribe your own plugins. Use Delete from Cloud instead.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (!m_cloud_service.request_cloud_unsubscribe(descriptor, error)) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    return finalize_cloud_plugin_removal(descriptor, true, error);
}

bool PluginManager::delete_and_unsubscribe_cloud_plugin(const std::string& plugin_key, std::string& error)
{
    if (!wait_for_discovery(std::chrono::milliseconds::max(), error))
        return false;

    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = "Only cloud plugins can be deleted and unsubscribed.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (descriptor.cloud->is_mine) {
        error = "Use Delete local and cloud for owned plugins.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (!m_cloud_service.request_cloud_unsubscribe(descriptor, error)) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    return finalize_cloud_plugin_removal(descriptor, false, error);
}

bool PluginManager::delete_mine_plugin_from_cloud(const std::string& plugin_key, std::string& error)
{
    if (!wait_for_discovery(std::chrono::milliseconds::max(), error))
        return false;

    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = "Only owned cloud plugins can be deleted from the cloud.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (!descriptor.cloud->is_mine) {
        error = "Only your own plugins can be deleted from the cloud.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (!m_cloud_service.request_cloud_delete(descriptor, error)) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    return finalize_cloud_plugin_removal(descriptor, true, error);
}

bool PluginManager::delete_mine_local_and_cloud_plugin(const std::string& plugin_key, std::string& error)
{
    if (!wait_for_discovery(std::chrono::milliseconds::max(), error))
        return false;

    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "Plugin not found: " + plugin_key;
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = "Only owned cloud plugins can be deleted from local and cloud.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (!descriptor.cloud->is_mine) {
        error = "Only your own plugins can be deleted from local and cloud.";
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (!m_cloud_service.request_cloud_delete(descriptor, error)) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    return finalize_cloud_plugin_removal(descriptor, false, error);
}

ExecutionResult PluginManager::run_script_capability(const std::string& plugin_key, const std::string& capability_name, std::string& error)
{
    if (plugin_key.empty() || capability_name.empty()) {
        return {};
    }

    auto cap = get_plugin_capability(plugin_key, capability_name, PluginCapabilityType::Script);
    if (!cap)
        return {};

    auto cap_interface = std::dynamic_pointer_cast<ScriptPluginCapability>(cap);
    if (!cap_interface)
        return {};

    ExecutionResult result;
    try {
        PythonGILState gil;
        if (!gil) {
            error = "Python interpreter is shutting down";
            return {};
        }
        result = cap_interface->execute();
    } catch (const std::exception& ex) {
        error = ex.what();
        BOOST_LOG_TRIVIAL(error) << "Script plugin execution threw exception. plugin_key=" << plugin_key << " error=" << error;
    } catch (...) {
        error = "Unknown error";
        BOOST_LOG_TRIVIAL(error) << "Script plugin execution threw unknown exception. plugin_key=" << plugin_key;
    }

    if (!error.empty()) {
        set_plugin_error(plugin_key, error);
    }

    return result;
}

} // namespace Slic3r
