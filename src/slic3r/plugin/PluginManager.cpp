#include "PluginManager.hpp"

#include <atomic>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <exception>
#include <functional>
#include "libslic3r/Config.hpp"
#include "libslic3r/LifecycleEvents.hpp"
#include "libslic3r/Utils.hpp"
#include <memory>
#include <optional>
#include <pybind11/embed.h>

#include "PluginFsUtils.hpp"
#include "PluginHooks.hpp"
#include "PluginAuditManager.hpp"
#include "PyPluginTrampoline.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginBridge.hpp"

#include "OrcaCloudServiceAgent.hpp"
#include "libslic3r/Semver.hpp"
#include "pybind11/pytypes.h"
#include "pybind11/gil.h"
#include "pybind11/pybind11.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include "slic3r/plugin/CloudPluginService.hpp"
#include <slic3r/plugin/PluginConfig.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/pluginTypes/script/ScriptPluginCapability.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wx/app.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r {
namespace {

// Sentinel error returned by the registry pre-check when the load was cancelled while it ran.
// A cancelled load records no error and fires no failure path — it just unwinds.
const char* const LOAD_CANCELLED = "__plugin_load_cancelled__";

std::vector<std::string>* permission_bucket(PluginPermissions& permissions, PluginPermissionCategory category)
{
    switch (category) {
    case PluginPermissionCategory::FsRead: return &permissions.fs_read;
    case PluginPermissionCategory::FsReadWrite: return &permissions.fs_readwrite;
    case PluginPermissionCategory::NetworkHttp: return &permissions.network_http;
    case PluginPermissionCategory::NetworkSocket: return &permissions.network_socket;
    case PluginPermissionCategory::Process: return &permissions.process;
    case PluginPermissionCategory::Events: return &permissions.events;
    }
    return nullptr;
}

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
    // release module, sys.modules, and sys.path ownership safely. Touch the async Script runtime
    // next, so ~PluginManager can still drain it during its own shutdown().
    PythonInterpreter::instance();
    ScriptAsyncRuntime::instance();
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

    // Re-arm the async Script runtime in case shutdown() ran earlier in this process.
    ScriptAsyncRuntime::instance().reset();

    // Bring every capability's stored config into memory. Deliberately unconditional and
    // independent of which plugins are installed: an entry outlives uninstall/unsubscribe, so a
    // plugin that comes back later finds its settings intact. A missing or malformed file just
    // leaves the store empty (see PluginConfig::load), never blocking startup.
    m_config.load();

    // Install the libslic3r hooks (capability resolver, slicing-pipeline dispatcher).
    // Uninstalled in shutdown() before the interpreter finalizes.
    plugin_hooks::install();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Plugin manager initialized";

    return true;
}

void PluginManager::set_shutting_down()
{
    m_shutting_down.store(true, std::memory_order_release);
    // The GUI may already be closing: drop audit prompts that have not been shown yet rather than
    // letting a queued dialog open after shutdown has begun.
    PluginAuditManager::discard_pending_permission_prompts();
}

void PluginManager::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool idle_and_empty = m_load_in_progress.empty() && std::none_of(m_plugins.begin(), m_plugins.end(),
                                                                               [](const Plugin& plugin) { return plugin.is_loaded(); });
        if (!m_initialized && !m_discovery_in_progress && idle_and_empty)
            return;
    }

    // Set the shutdown flag before releasing any blocked permission prompt: a worker that fails
    // because of that release must see the cancellation and not record a spurious load error or
    // disable the package.
    set_shutting_down();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": PluginManager shutdown enter";

    // Detach the libslic3r hooks first so nothing dispatches into Python while (or after) plugins
    // unload. The lifecycle-event hook also drains callbacks already in progress before returning;
    // the remaining hook seams retain their existing shutdown requirements.
    plugin_hooks::uninstall();

    std::string wait_error;
    if (!wait_for_discovery(std::chrono::milliseconds::max(), wait_error) && !wait_error.empty())
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Plugin discovery did not finish cleanly during shutdown: " << wait_error;

    // Wait for any in-progress plugin loads. Cancelled loads keep their slot until their worker
    // has actually unwound, so this cannot return while one is still executing Python.
    wait_for_all_plugin_loads();

    unload_all_plugins();
    PythonPluginBridge::instance().clear_pending_captures();

    // Every async Script run has been drained by unload_all_plugins above; wait for every
    // invocation worker before the interpreter is allowed to finalize.
    ScriptAsyncRuntime::instance().shutdown();

    // Every config write already goes to disk as it happens (store_capability_config), so this
    // only catches an in-memory-only mutation. Note we flush rather than clear: unloading the
    // plugins above must never discard their stored config.
    if (m_config.dirty())
        m_config.save();

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
        return GUI::format(_u8L("Plugin package is already loaded: %1%"), plugin_key);

    // A capability name must be unique per type across every loaded package.
    for (const auto& capability : candidate.capabilities) {
        if (!capability)
            continue;
        for (const Plugin& other : m_plugins) {
            if (!other.is_loaded() || other.descriptor.plugin_key == plugin_key)
                continue;
            for (const auto& existing : other.capabilities) {
                if (existing && existing->type() == capability->type() && existing->name() == capability->name())
                    // TRN %1% is the capability type (e.g. Script), %2% the capability name.
                    return GUI::format(_u8L("Capability collision for type %1% and name '%2%'"),
                                       PluginConfig::capability_type_label(capability->type()), capability->name());
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
                const std::string duplicate_error = GUI::format(_u8L("Duplicate plugin key discovered: %1%"), descriptor.plugin_key);
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
            m_missing_plugin_keys.erase(descriptor.plugin_key);

            Plugin* existing = find_plugin_locked(descriptor.plugin_key);
            if (existing == nullptr) {
                Plugin plugin;
                plugin.descriptor = std::move(descriptor);
                m_plugins.push_back(std::move(plugin));
                continue;
            }

            // A manifest-only rescan has nothing to say about capabilities, so the live module and
            // instances are left alone.
            if (!descriptor.install_state_valid)
                descriptor.enabled = existing->descriptor.enabled;
            existing->descriptor = std::move(descriptor);
        }

        if (!clear)
            return;
    }

    // A package can be temporarily absent while an external side-loader replaces it. Keep the
    // descriptor and its persisted enable state until the user explicitly removes the missing
    // entry, or a later scan rediscovers it. In particular, do not unload here: unloading would turn
    // a transient filesystem gap into enabled=false in the sidecar.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Plugin& plugin : m_plugins) {
            if (plugin.descriptor.has_local_package() &&
                std::find(seen.begin(), seen.end(), plugin.descriptor.plugin_key) == seen.end())
                m_missing_plugin_keys.insert(plugin.descriptor.plugin_key);
        }
    }
}

std::vector<PluginDescriptor> PluginManager::get_missing_plugin_descriptors() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<PluginDescriptor> result;
    result.reserve(m_missing_plugin_keys.size());
    for (const Plugin& plugin : m_plugins)
        if (m_missing_plugin_keys.count(plugin.descriptor.plugin_key) != 0)
            result.push_back(plugin.descriptor);
    return result;
}

void PluginManager::remove_missing_plugins(const std::vector<std::string>& plugin_keys)
{
    const std::unordered_set<std::string> requested(plugin_keys.begin(), plugin_keys.end());

    // The predicate is evaluated only while m_mutex is held by unload_and_erase_if(). Checking the
    // current missing set here prevents a package that reappeared between the dialog and removal
    // from being erased.
    unload_and_erase_if([this, &requested](const Plugin& plugin) {
        return requested.count(plugin.descriptor.plugin_key) != 0 &&
               m_missing_plugin_keys.count(plugin.descriptor.plugin_key) != 0;
    });

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::string& plugin_key : requested)
        m_missing_plugin_keys.erase(plugin_key);
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

            // A removed package can no longer retry the load a queued prompt was waiting for.
            // Loaded entries were unloaded above; not-loaded ones are erased here.
            for (const Plugin& plugin : m_plugins)
                if (should_remove(plugin))
                    m_failed_loads.erase(plugin.descriptor.plugin_key);

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

std::optional<PluginCapabilityRef> PluginManager::resolve_capability_ref(const std::string& capability_name,
                                                                         PluginCapabilityType type) const
{
    // only_enabled = false: a stored reference must stay resolvable whether or not the user
    // currently has the capability enabled; the dispatchers report "disabled" separately.
    if (!get_plugin_capability(capability_name, type, /*only_enabled=*/false))
        return std::nullopt;

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor_for_capability(capability_name, type, descriptor))
        return std::nullopt;

    // Cloud plugins are resolved at runtime via the UUID, so the name field keeps the friendly
    // display name. Local plugins are looked up by plugin_key with an empty UUID.
    const std::string identity = descriptor.is_cloud_plugin() ? descriptor.name : descriptor.plugin_key;
    return PluginCapabilityRef{identity, capability_name, descriptor.cloud_uuid()};
}

std::string PluginManager::get_storage_dir(const std::string& plugin_key) const
{
    namespace fs = boost::filesystem;

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor))
        throw std::runtime_error("The current plugin is not registered");

    const fs::path base_storage_dir = fs::path(get_orca_plugins_dir()) / PLUGIN_DATA_DIR;

    if (!descriptor.is_cloud_plugin()) {
        const fs::path local_storage_dir = base_storage_dir / plugin_key;
        fs::create_directories(local_storage_dir);
        return local_storage_dir.string();
    }

    auto agent = m_cloud_service.get_cloud_agent();
    if (!agent)
        throw std::runtime_error("Cloud plugin storage is unavailable before networking is initialized");

    const std::string user_id = agent->get_user_id();
    if (user_id.empty())
        throw std::runtime_error("Cloud plugin storage is unavailable without a logged-in user");

    if (!is_valid_plugin_id(plugin_key))
        throw std::runtime_error("The current cloud plugin key is not a valid folder name");

    const fs::path cloud_storage_dir = base_storage_dir / PLUGIN_SUBSCRIBED_DIR / user_id / plugin_key;
    fs::create_directories(cloud_storage_dir);
    return cloud_storage_dir.string();
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

std::shared_ptr<PluginCapabilityInterface> PluginManager::get_plugin_capability(const PluginCapabilityId& id, bool only_enabled) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const Plugin* plugin = find_plugin_locked(id.plugin_key);
    if (plugin == nullptr)
        return nullptr;

    for (const auto& capability : plugin->capabilities) {
        if (!capability || capability->name() != id.name)
            continue;
        if (id.type != PluginCapabilityType::Unknown && capability->type() != id.type)
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

bool PluginManager::get_install_state(const std::string& plugin_key, PluginInstallState& install_state)
{
    // Serialize the read against every in-process sidecar writer. Lock order stays
    // install-state -> m_mutex, matching the writers.
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty()) {
        return false;
    }

    if (!read_install_state(boost::filesystem::path(descriptor.plugin_root), install_state)) {
        return false;
    }

    return true;
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

bool PluginManager::is_plugin_load_cancelled(const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return is_plugin_load_cancelled_locked(plugin_key);
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
        error = _u8L("Plugin load is still in progress");
        return false;
    }

    // An update owns the key while it replaces the package and refuses to start a load, so there
    // is no slot to wait for. Report the reason here rather than recording it in m_load_errors,
    // where it would outlive the claim and surface on a later load.
    if (m_updates_in_progress.count(plugin_key) > 0) {
        error = _u8L("Plugin update is in progress.");
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

bool PluginManager::is_plugin_load_failed(const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return find_plugin_locked(plugin_key) != nullptr && m_failed_loads.count(plugin_key) > 0;
}

bool PluginManager::cancel_plugin_load(const std::string& plugin_key)
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cancelled = cancel_plugin_load_locked(plugin_key);
    }

    // A queued or active audit prompt for this load would keep its worker blocked on an answer
    // that can no longer authorize anything. Mark the load cancelled first, then discard outside
    // m_mutex, matching unload_plugin(): a prompt whose answer has not begun committing loses to
    // the cancellation, while an in-progress commit stays atomic.
    if (cancelled)
        PluginAuditManager::discard_pending_permission_prompts_for(plugin_key);

    notify_plugin_load_state_changed(cancelled);
    return cancelled;
}

bool PluginManager::cancel_plugin_load_locked(const std::string& plugin_key)
{
    if (m_load_in_progress.count(plugin_key) == 0)
        return false;

    m_cancelled.insert(plugin_key);
    m_load_errors.erase(plugin_key);
    m_failed_loads.erase(plugin_key);
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

        const bool cancelled = m_cancelled.erase(plugin_key) > 0;
        const Plugin* plugin = find_plugin_locked(plugin_key);
        const bool loaded    = plugin != nullptr && plugin->is_loaded();
        // Record an ordinary failure, but never a cancelled or successful load, and never one for
        // a package that was removed while the worker unwound. Only the ordinary failure leaves
        // its queued runtime prompts actionable for approval and retry.
        if (cancelled || loaded || plugin == nullptr || m_load_errors.count(plugin_key) == 0)
            m_failed_loads.erase(plugin_key);
        else
            m_failed_loads.insert(plugin_key);
    }
    notify_plugin_load_state_changed(changed);
}

void PluginManager::cancel_and_wait_for_capabilities(
    const std::vector<std::shared_ptr<PluginCapabilityInterface>>& capabilities)
{
    // Ask the plugin to cancel first, then schedule the async cancellation: on_cancelled() can
    // observe/record state while the run is still intact, and only afterwards is its asyncio task
    // cancelled. Draining of capability references still follows, so the module is not unloaded
    // until the run has settled.
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

    for (const auto& capability : capabilities) {
        if (capability)
            ScriptAsyncRuntime::instance().cancel_plugin(capability->audit_plugin_key());
    }

    // Script rows are bounded only by their own worker: a cancelled or stalled invocation of another
    // plugin can no longer couple to this one (each invocation owns a thread). A Script worker is
    // drained without a deadline — forcing module unload while one is outstanding would leave Python
    // executing against a freed module, or let a blocking preset selection mutate presets after the
    // plugin is gone. Non-Script synchronous calls keep the historical bounded grace. On the GUI
    // thread the event loop is pumped so a modal selection can settle instead of being starved by a
    // blocking wait.
    const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        const bool all_released = std::all_of(capabilities.begin(), capabilities.end(), [](const auto& capability) {
            return !capability || capability->ref_count() == 0;
        });
        // A cancellation that arrived before the invocation ran skips the ref counter, so an active
        // worker must also be gone: never unload the module while any Script thread is still live.
        const bool invocation_active = std::any_of(capabilities.begin(), capabilities.end(), [](const auto& capability) {
            return capability != nullptr && ScriptAsyncRuntime::instance().has_active_run(capability.get());
        });
        if (all_released && !invocation_active)
            return;

        if (!invocation_active && std::chrono::steady_clock::now() >= grace_deadline)
            break;

        if (wxTheApp != nullptr && wxIsMainThread())
            wxYield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

    bool exists             = false;
    bool invalid            = false;
    bool already_loaded     = false;
    bool load_in_progress   = false;
    bool load_slot_claimed  = false;
    bool shutting_down      = false;
    bool update_in_progress = false;
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

        load_in_progress   = m_load_in_progress.count(plugin_id) > 0;
        shutting_down      = m_shutting_down.load(std::memory_order_acquire);
        update_in_progress = m_updates_in_progress.count(plugin_id) > 0;
        if (exists && !invalid && !already_loaded && !load_in_progress && !update_in_progress && !shutting_down) {
            m_load_in_progress.insert(plugin_id);
            m_load_errors.erase(plugin_id);
            m_failed_loads.erase(plugin_id);
            load_slot_claimed = true;
        }
    }

    // An update owns the key until it has finished replacing the package on disk and reloads the
    // new one itself. Starting a load here would import the old package while it is being moved
    // aside, so the request is refused rather than left to race the replacement.
    if (update_in_progress) {
        BOOST_LOG_TRIVIAL(info) << "Plugin load skipped — update in progress: " << plugin_id;
        return;
    }

    if (already_loaded) {
        // An empty request means "restore the persisted capability state". Only an explicit
        // capability request may change an already-loaded capability, e.g. when resolving an
        // inactive-plugin reference. Runs outside m_mutex.
        for (const std::string& capability_name : capabilities_to_enable) {
            if (std::find(loaded_capability_names.begin(), loaded_capability_names.end(), capability_name) !=
                loaded_capability_names.end())
                set_capability_enabled({PluginCapabilityType::Unknown, capability_name, plugin_id}, true);
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
            message = GUI::format(_u8L("Plugin is invalid: %1%"), plugin_id);
            if (!invalid_error.empty())
                message += " - " + invalid_error;
        } else {
            message = GUI::format(_u8L("Plugin not found: %1%"), plugin_id);
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
            fail_unexpected(GUI::format(_u8L("Unexpected plugin load failure: %1%"), ex.what()));
        } catch (...) {
            fail_unexpected(_u8L("Unexpected plugin load failure"));
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
        fail(GUI::format(_u8L("Plugin manifest not found: %1%"), plugin_key));
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
    bool registration_failed = false;
    if (!plugin_loader::load(descriptor, skip_deps, capabilities_to_enable, registry_precheck, plugin, error, &registration_failed)) {
        // A permission prompt denied because the load was cancelled (unload or shutdown) surfaces
        // here as an ordinary registration/permission failure. Do not disable the package or record
        // an error for that: a genuine denial still follows the normal failure path below.
        bool load_cancelled = m_shutting_down.load(std::memory_order_acquire);
        if (!load_cancelled) {
            std::lock_guard<std::mutex> lock(m_mutex);
            load_cancelled = is_plugin_load_cancelled_locked(plugin_key);
        }
        if (error == LOAD_CANCELLED || load_cancelled) {
            BOOST_LOG_TRIVIAL(info) << "[load_plugin_impl] CANCELLED plugin=" << plugin_key;
            return; // cancelled: nothing materialized survives, and no error is recorded
        }
        if (registration_failed)
            mark_plugin_install_state_disabled(plugin_key);
        fail(std::move(error));
        return;
    }

    for (const auto& cap : plugin.capabilities) {
        auto config = cap->get_default_config();
        if (config.empty())
            continue;
        const PluginCapabilityId id = cap->identity();
        if (m_config.has_config(id))
            continue;

        m_config.save_config({id, plugin.descriptor.installed_version, config});
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
                    registry_error = GUI::format(_u8L("Plugin manifest not found: %1%"), plugin_key);
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

    // Persist only after the plugin is committed as loaded. Lifecycle callbacks are dispatched
    // asynchronously from worker threads and must not own sidecar persistence.
    if (!m_shutting_down.load(std::memory_order_acquire))
        write_loaded_plugin_install_state(plugin_key);

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
        // An explicit unload also revokes the "ordinary failure" state, so a prompt admitted
        // after the queue discard below can never be shown for the unloaded plugin.
        m_failed_loads.erase(plugin_key);

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

    // Queued audit prompts belong to a plugin that is going away: cancel any blocked load worker
    // and keep a late answer from persisting. Do this after marking the load cancelled so a worker
    // released by the cancellation cannot take it as approval.
    PluginAuditManager::discard_pending_permission_prompts_for(plugin_key);

    if (!found) {
        notify_plugin_load_state_changed(cancelled);
        return true;
    }

    // Flip the durable flag here rather than in a completion callback: the update path unloads on
    // a worker and immediately replaces the package, so a deferred write must not race the new
    // sidecar. Persisting before the unload completes also means callers that delete or replace
    // the plugin directory next already see the disabled state. Shutdown skips this to preserve
    // the auto-load list across app exit.
    if (!m_shutting_down.load(std::memory_order_acquire))
        mark_plugin_install_state_disabled(plugin_key);

    notify_unload_complete();

    cancel_and_wait_for_capabilities(removed.capabilities);

    notify_plugin_load_state_changed(cancelled);

    // Teardown runs outside m_mutex: it calls into Python and must not hold the registry lock.
    plugin_loader::unload(removed);

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
    for (const std::string& key : removed_keys) {
        PluginAuditManager::discard_pending_permission_prompts_for(key);
        run_on_unload_callbacks(key);
    }

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
void PluginManager::set_capability_enabled(const PluginCapabilityId& id, bool enabled)
{
    PluginCapabilityId changed;
    bool did_change = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        Plugin* plugin = find_plugin_locked(id.plugin_key);
        if (plugin == nullptr || !plugin->is_loaded())
            return;

        for (const auto& capability : plugin->capabilities) {
            if (!capability || capability->name() != id.name ||
                (id.type != PluginCapabilityType::Unknown && capability->type() != id.type) ||
                capability->is_enabled() == enabled)
                continue;

            capability->set_enabled(enabled);
            changed    = capability->identity();
            did_change = true;
            break;
        }
    }

    if (!did_change)
        return;

    write_loaded_plugin_install_state(id.plugin_key);

    if (enabled)
        run_on_capability_load_callbacks(changed);
    else
        run_on_capability_unload_callbacks(changed);
}

void PluginManager::write_loaded_plugin_install_state(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

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

void PluginManager::mark_plugin_install_state_disabled(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return;

    const boost::filesystem::path root(descriptor.plugin_root);
    PluginInstallState state;
    // Only a missing sidecar may be seeded. One that exists but cannot be read may still hold
    // permissions and capability choices that are recoverable, so it must not be replaced by a
    // fresh one. Report the skip and leave the descriptor alone, exactly as for a failed write.
    const InstallStateReadStatus status = read_install_state_status(root, state);
    if (status == InstallStateReadStatus::Unreadable) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Cannot read install state for " << plugin_key
                                   << "; leaving the sidecar untouched";
        return;
    }
    if (status == InstallStateReadStatus::Absent)
        seed_install_metadata(state, descriptor);
    state.enabled = false;
    if (!write_install_state(root, state))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (Plugin* plugin = find_plugin_locked(plugin_key))
        plugin->descriptor.enabled = false;
}

bool PluginManager::mutate_plugin_permissions(const std::string& plugin_key, const PluginPermissionMutation& mutation,
                                              PluginPermissions* applied)
{
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

    if (plugin_key.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": No plugin key supplied";
        return false;
    }

    // Enabling full access is the dangerous direction and must target a loadable (non-invalid)
    // package. Disabling and plain list edits accept any known installed package.
    PluginDescriptor descriptor;
    const bool       enabling_full_access = mutation.full_access && *mutation.full_access;
    const bool       known = enabling_full_access ? try_get_valid_plugin_descriptor(plugin_key, descriptor)
                                                  : try_get_plugin_descriptor(plugin_key, descriptor);
    if (!known || !descriptor.has_local_package() || descriptor.plugin_root.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": No installed plugin to update: " << plugin_key;
        return false;
    }

    const boost::filesystem::path root(descriptor.plugin_root);
    PluginInstallState state;
    if (!read_install_state(root, state)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to read install state for " << plugin_key;
        return false;
    }

    // Apply against the latest state read under the lock, so a grant another writer added since
    // the caller last looked is kept. Removals run first: a value named in both lists ends present.
    for (const PluginPermissionMutation::Entry& entry : mutation.remove) {
        if (entry.value.empty())
            continue;
        std::vector<std::string>* bucket = permission_bucket(state.permissions, entry.category);
        if (bucket == nullptr)
            continue;
        bucket->erase(std::remove(bucket->begin(), bucket->end(), entry.value), bucket->end());
    }
    for (const PluginPermissionMutation::Entry& entry : mutation.add) {
        if (entry.value.empty())
            continue;
        std::vector<std::string>* bucket = permission_bucket(state.permissions, entry.category);
        if (bucket == nullptr)
            continue;
        if (std::find(bucket->begin(), bucket->end(), entry.value) == bucket->end())
            bucket->push_back(entry.value);
    }

    if (mutation.threading)
        state.permissions.threading = *mutation.threading;
    if (mutation.full_access)
        state.permissions.full_access = *mutation.full_access;

    if (!write_install_state(root, state)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to write install state for " << plugin_key;
        return false;
    }

    if (applied != nullptr)
        *applied = state.permissions;

    BOOST_LOG_TRIVIAL(info) << "Applied permission changes for plugin " << plugin_key;
    return true;
}

void PluginManager::revoke_plugin_permissions(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return;

    const boost::filesystem::path root(descriptor.plugin_root);
    PluginInstallState state;
    if (!read_install_state(root, state)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to read install state for " << plugin_key;
        return;
    }

    state.permissions = {};
    if (!write_install_state(root, state))
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to revoke permissions for " << plugin_key;
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

// The run_on_*_callbacks below are called from detached load/unload workers. They snapshot the
// subscriber list under m_mutex (the copy protects the in-flight loop from a callback that
// subscribes mid-dispatch, and serialises against a concurrent subscribe), then hand the snapshot
// to the UI thread: subscribers touch wx and must not run on a worker, and none may run while the
// registry lock is held. The dispatch runs inline when already on the UI thread, else via
// CallAfter, since calling wx off the main thread is UB.
void PluginManager::run_on_load_callbacks(const std::string& plugin_key)
{
    auto work = [callbacks = copy_callbacks(CallbackType::Load), plugin_key] {
        for (auto& fn : callbacks) {
            try {
                fn(plugin_key);
            } catch (const std::exception& ex) {
                BOOST_LOG_TRIVIAL(error) << "Plugin load completion callback failed for " << plugin_key << ": " << ex.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Plugin load completion callback failed for " << plugin_key;
            }
        }
    };
    if (wxTheApp == nullptr || wxIsMainThread())
        work();
    else
        GUI::wxGetApp().CallAfter(std::move(work));
}

void PluginManager::run_on_unload_callbacks(const std::string& plugin_key)
{
    auto work = [callbacks = copy_callbacks(CallbackType::Unload), plugin_key] {
        for (auto& fn : callbacks) {
            try {
                fn(plugin_key);
            } catch (const std::exception& ex) {
                BOOST_LOG_TRIVIAL(error) << "Plugin unload completion callback failed for " << plugin_key << ": " << ex.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Plugin unload completion callback failed for " << plugin_key << ": unknown error";
            }
        }
    };
    if (wxTheApp == nullptr || wxIsMainThread())
        work();
    else
        GUI::wxGetApp().CallAfter(std::move(work));
}

void PluginManager::run_on_capability_load_callbacks(const PluginCapabilityId& id)
{
    auto work = [callbacks = copy_capability_callbacks(CallbackType::Load), id] {
        for (auto& fn : callbacks) {
            try {
                fn(id);
            } catch (const std::exception& ex) {
                BOOST_LOG_TRIVIAL(error) << "Plugin capability load callback failed for " << id.plugin_key << "/" << id.name << ": "
                                         << ex.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Plugin capability load callback failed for " << id.plugin_key << "/" << id.name;
            }
        }
    };
    if (wxTheApp == nullptr || wxIsMainThread())
        work();
    else
        GUI::wxGetApp().CallAfter(std::move(work));
}

void PluginManager::run_on_capability_unload_callbacks(const PluginCapabilityId& id)
{
    auto work = [callbacks = copy_capability_callbacks(CallbackType::Unload), id] {
        for (auto& fn : callbacks) {
            try {
                fn(id);
            } catch (const std::exception& ex) {
                BOOST_LOG_TRIVIAL(error) << "Plugin capability unload callback failed for " << id.plugin_key << "/" << id.name << ": "
                                         << ex.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Plugin capability unload callback failed for " << id.plugin_key << "/" << id.name;
            }
        }
    };
    if (wxTheApp == nullptr || wxIsMainThread())
        work();
    else
        GUI::wxGetApp().CallAfter(std::move(work));
}

// ── Cloud user ──────────────────────────────────────────────────────────────────────────────

void PluginManager::set_cloud_user(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cloud_user_id = user_id;
}

void PluginManager::sync_cloud_user_from_agent()
{
    // Ensure cloud plugins install to _subscribed/<user_id>/, matching the signed-in session.
    if (auto agent = m_cloud_service.get_cloud_agent()) {
        const std::string user_id = agent->get_user_id();
        if (!user_id.empty())
            set_cloud_user(user_id);
    }
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
    return install_plugin_impl(filepath, plugin_descriptor, nullptr, error, nullptr);
}

bool PluginManager::install_plugin_impl(const boost::filesystem::path& filepath, PluginDescriptor& plugin_descriptor,
                                        const std::string* cloud_user_id_override, std::string& error,
                                        bool* install_target_intact, const PluginInstallState* preserved_state,
                                        bool mark_error_on_failure)
{
    error.clear();

    std::string wait_error;
    if (!wait_for_discovery(std::chrono::milliseconds::max(), wait_error)) {
        error = wait_error.empty() ? "Plugin discovery is still running" : wait_error;
        if (mark_error_on_failure && !plugin_descriptor.plugin_key.empty())
            set_plugin_error(plugin_descriptor.plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Plugin installation failed while waiting for discovery: " << error;
        return false;
    }

    std::string cloud_user_id;
    if (cloud_user_id_override != nullptr) {
        cloud_user_id = *cloud_user_id_override;
    } else {
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

    if (!plugin_loader::install_plugin(filepath, cloud_user_id, plugin_descriptor, error, install_target_intact,
                                       preserved_state)) {
        if (mark_error_on_failure && !plugin_descriptor.plugin_key.empty())
            set_plugin_error(plugin_descriptor.plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Every successful install may have replaced executable plugin code. Revoke any permissions
    // associated with the previous package so the newly installed version must request them again.
    if (!plugin_descriptor.plugin_key.empty()) { {
        revoke_plugin_permissions(plugin_descriptor.plugin_key);

        // A prompt queued by the replaced code must not grant a permission the new code never
        // requested: an installed package starts its lifecycle from a clean failure state.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failed_loads.erase(plugin_descriptor.plugin_key);
    }

    if (!plugin_descriptor.plugin_key.empty())
        clear_plugin_error(plugin_descriptor.plugin_key);
    }

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

void PluginManager::update_cloud_metadata(const std::vector<PluginDescriptor>& cloud_list)
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

void PluginManager::clear_cloud_plugin_metadata()
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

    BOOST_LOG_TRIVIAL(info) << "Cleared cloud plugin metadata";
}

void PluginManager::fetch_plugins_from_cloud(std::vector<std::string>* out_not_found, std::vector<std::string>* out_unauthorized)
{
    if (!m_cloud_service.can_fetch_cloud_plugins())
        return;

    std::vector<PluginDescriptor> cloud_list{};
    std::vector<std::string> not_found{}, unauthorized{};
    const bool cloud_fetch_succeeded = m_cloud_service.fetch_manifests_into_descriptors(cloud_list, not_found, unauthorized);
    if (!cloud_fetch_succeeded) {
        if (wxTheApp != nullptr) {
            GUI::wxGetApp().CallAfter([] {
                if (GUI::wxGetApp().is_closing())
                    return;
                GUI::Plater* plater = GUI::wxGetApp().plater();
                if (plater == nullptr || GUI::wxGetApp().imgui() == nullptr || !GUI::wxGetApp().imgui()->display_initialized())
                    return;
                plater->get_notification_manager()->push_notification(GUI::NotificationType::CustomNotification,
                                                                      GUI::NotificationManager::NotificationLevel::WarningNotificationLevel,
                                                                      _u8L("Failed to fetch plugins from the cloud. See logs for details."));
            });
        }
    }

    if (cloud_fetch_succeeded)
        update_cloud_metadata(cloud_list);

    if (cloud_fetch_succeeded) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Clear the previous cloud verdicts before re-applying the fresh ones.
        for (Plugin& plugin : m_plugins) {
            PluginDescriptor& entry = plugin.descriptor;
            if (!entry.is_cloud_plugin())
                continue;
            entry.set_unauthorized(false);
            if (entry.cloud.has_value())
                entry.cloud->orphaned = false;
            if (entry.normalized_error() == CLOUD_PLUGIN_NOT_FOUND_ERROR)
                entry.clear_error();
        }

        // A successful subscriptions response may report missing UUIDs explicitly, or it may
        // simply omit an unsubscribed plugin from `data`. Both cases leave a locally retained
        // cloud package orphaned. Owned plugins are returned by the separate mine endpoint and
        // must not be orphaned merely because they are not subscribed.
        for (Plugin& plugin : m_plugins) {
            PluginDescriptor& entry = plugin.descriptor;
            if (!entry.is_cloud_plugin() || entry.cloud->is_mine)
                continue;

            const bool explicitly_not_found = std::find(not_found.begin(), not_found.end(), entry.cloud_uuid()) != not_found.end();
            const bool returned_by_cloud = std::any_of(cloud_list.begin(), cloud_list.end(), [&entry](const PluginDescriptor& cloud_entry) {
                return cloud_entry.cloud_uuid() == entry.cloud_uuid();
            });
            entry.cloud->orphaned = explicitly_not_found || !returned_by_cloud;
            if (entry.cloud->orphaned)
                entry.cloud->update_available = false;
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

    // A successful fetch is the only point where update availability is authoritative. Publish
    // one aggregate notice after the orphan/unauthorized verdicts above so a plugin the user can
    // no longer act on is not advertised. The check is repeated when the callback runs, keeping
    // the notice fresh if an update was installed while the fetch was in flight.
    if (cloud_fetch_succeeded) {
        std::string fetched_cloud_user;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            fetched_cloud_user = m_cloud_user_id;
        }

        auto push_update_notification = [this, fetched_cloud_user]() {
            if (wxTheApp == nullptr || m_shutting_down.load(std::memory_order_acquire))
                return;
            if (GUI::wxGetApp().is_closing())
                return;
            GUI::Plater* plater = GUI::wxGetApp().plater();
            // Before post_init() there is no plater yet; GUI_App's startup report delivers the
            // notice instead of dropping it.
            if (plater == nullptr)
                return;

            bool update_available = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_cloud_user_id != fetched_cloud_user)
                    return; // login/logout while the fetch was in flight
                update_available = std::any_of(m_plugins.begin(), m_plugins.end(), [](const Plugin& plugin) {
                    return plugin.descriptor.has_update_available();
                });
            }
            if (!update_available)
                return;

            GUI::NotificationManager* notifications = plater->get_notification_manager();
            if (notifications == nullptr ||
                notifications->has_notification_of_type(GUI::NotificationType::OrcaCloudPluginUpdateAvailable))
                return;
            notifications->push_notification(GUI::NotificationType::OrcaCloudPluginUpdateAvailable);
        };
        if (wxTheApp == nullptr || wxIsMainThread())
            push_update_notification();
        else
            GUI::wxGetApp().CallAfter(std::move(push_update_notification));
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
        error = _u8L("Cloud plugin key is missing UUID.");
        return false;
    }
    if (!m_cloud_service.can_fetch_cloud_plugins()) {
        error = _u8L("Sign in to OrcaCloud to install this plugin.");
        return false;
    }

    PluginDescriptor descriptor;
    bool found = try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_cloud_plugin();
    if (!found) {
        fetch_plugins_from_cloud();
        found = try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_cloud_plugin();
    }

    if (!found) {
        if (!m_cloud_service.request_cloud_subscribe(plugin_uuid, error))
            return false;

        fetch_plugins_from_cloud();
        found = try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_cloud_plugin();
        if (!found) {
            error = _u8L("Subscribed cloud plugin was not returned by OrcaCloud.");
            return false;
        }
    }

    if (descriptor.has_local_package())
        return true;

    return download_and_install_cloud_plugin(descriptor.plugin_key, descriptor.version, error);
}

bool PluginManager::install_cloud_plugin_package(const std::string& plugin_key,
                                                 const std::string& requested_version,
                                                 const boost::filesystem::path& package_path,
                                                 const PluginDescriptor& descriptor,
                                                 const std::string* cloud_user_id,
                                                 std::string& error,
                                                 CloudUpdateFailure* failure,
                                                 const PluginInstallState* preserved_state)
{
    error.clear();

    // When `failure` is given, the caller owns the failed update: it restores the previous package
    // when that is still usable, and only then decides whether the plugin is actually broken. Do
    // not record the install error here, where it would mark a working restored plugin as failed.
    const bool caller_owns_failure = failure != nullptr;

    // Record the version fetched from the cloud so install_plugin persists it to the install-state
    // sidecar (the source of truth for the installed cloud version) instead of the local
    // manifest/PEP723 header version.
    PluginDescriptor installed = descriptor;
    installed.installed_version = requested_version;
    // The executable identity belongs to the package being installed. A descriptor taken from the
    // previous installation can still carry its old wheel import name; clearing it lets
    // install_plugin() fill entry_package from the replacement's package metadata instead.
    installed.entry_package.clear();

    bool install_target_intact = true;
    if (!install_plugin_impl(package_path, installed, cloud_user_id, error, &install_target_intact, preserved_state,
                             /*mark_error_on_failure=*/!caller_owns_failure)) {
        if (failure != nullptr)
            *failure = install_target_intact ? CloudUpdateFailure::PreviousPackageUsable
                                             : CloudUpdateFailure::RollbackFailed;
        if (error.empty())
            error = _u8L("Failed to install cloud plugin.");
        if (!caller_owns_failure)
            set_plugin_error(plugin_key, error);
        boost::system::error_code ec;
        boost::filesystem::remove(package_path, ec);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    if (!requested_version.empty())
        installed.version = requested_version;
    // The version just downloaded and installed is now the locally installed version.
    installed.installed_version = requested_version.empty() ? installed.version : requested_version;
    // The installer kept the pre-update activation intent in the sidecar; mirror it in memory.
    installed.enabled = preserved_state != nullptr ? preserved_state->enabled : true;
    if (installed.cloud.has_value()) {
        installed.cloud->installed        = true;
        installed.cloud->update_available = false;
        installed.cloud->unauthorized     = false;
    }
    installed.clear_error();
    installed.set_unauthorized(false);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Plugin* entry = find_plugin_locked(plugin_key);
        if (entry == nullptr) {
            error = _u8L("Plugin not found.");
            if (failure != nullptr)
                *failure = CloudUpdateFailure::ReplacementInstalled;
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Cloud plugin " << plugin_key
                                       << " downloaded successfully but failed to update plugin metadata. Plugin object not found.";
        } else {
            entry->descriptor = std::move(installed);
        }
    }
    if (!error.empty()) {
        if (!caller_owns_failure)
            set_plugin_error(plugin_key, error);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Cloud plugin " << plugin_key << " installed successfully";
    return true;
}

bool PluginManager::download_and_install_cloud_plugin(const std::string& plugin_key, const std::string& version, std::string& error)
{
    error.clear();

    CloudPluginDownload download;
    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = GUI::format(_u8L("Plugin not found: %1%"), plugin_key);
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to find plugin key " << plugin_key;
        return false;
    }

    clear_plugin_error(plugin_key);

    const std::string requested_version = version.empty() ? descriptor.version : version;
    if (!m_cloud_service.download_cloud_plugin(descriptor, requested_version, download, error)) {
        if (error.empty())
            error = _u8L("Failed to download cloud plugin.");
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Ensure cloud plugins install to _subscribed/<user_id>/
    sync_cloud_user_from_agent();

    return install_cloud_plugin_package(plugin_key, requested_version, download.package_path, descriptor, nullptr, error);
}

void PluginManager::restore_plugin_state_after_failed_update(const PluginDescriptor& previous,
                                                             const PluginInstallState& previous_state,
                                                             bool had_previous_state,
                                                             bool was_enabled)
{
    namespace fs = boost::filesystem;

    if (previous.plugin_root.empty())
        return;

    const fs::path root(previous.plugin_root);
    boost::system::error_code ec;
    if (!fs::exists(root, ec)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Previous plugin folder is missing, cannot restore its state: "
                                   << previous.plugin_key;
        return;
    }

    {
        std::lock_guard<std::mutex> state_lock(install_state_mutex());

        PluginInstallState state;
        const InstallStateReadStatus status = read_install_state_status(root, state);
        if (status == InstallStateReadStatus::Unreadable) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Cannot restore install state for "
                                       << previous.plugin_key << "; leaving the sidecar untouched";
            return;
        }

        if (had_previous_state) {
            // The snapshot also restores permissions and capability choices, which belong to the
            // old package and were never replaced.
            state = previous_state;
        } else if (status == InstallStateReadStatus::Absent) {
            seed_install_metadata(state, previous);
        }
        // The unload before the failed replacement wrote enabled=false over the old intent.
        state.enabled = was_enabled;

        if (!write_install_state(root, state))
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to restore install state for " << previous.plugin_key;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Plugin* plugin = find_plugin_locked(previous.plugin_key))
            plugin->descriptor.enabled = was_enabled;
    }
}

bool PluginManager::update_cloud_plugin(const std::string& plugin_key, std::string& error, std::string version)
{
    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = GUI::format(_u8L("Plugin not found: %1%"), plugin_key);
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = _u8L("Only cloud plugins can be updated.");
        set_plugin_error(plugin_key, error);
        return false;
    }

    // Claim the key before any network or filesystem work. Both checks and the insert happen under
    // m_mutex, and load_plugin() checks the same set under it, so an in-flight load cannot be
    // cancelled halfway by the replacement and a new one cannot start while the package is being
    // moved aside. The account the update started under is captured with the claim.
    std::string update_cloud_user;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_load_in_progress.count(plugin_key) > 0)
            error = _u8L("Plugin load is still in progress");
        else if (!m_updates_in_progress.insert(plugin_key).second)
            error = _u8L("A plugin update is already in progress.");
        else
            update_cloud_user = m_cloud_user_id;
    }
    if (!error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Release the claim on every path. The reload of the replacement must run without it, so it
    // is released explicitly first and the scope guard then has nothing left to do.
    auto release_update = [this, &plugin_key]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_updates_in_progress.erase(plugin_key);
    };
    ScopeGuard update_scope(release_update);

    // Reloads whichever package is installed for the key. The claim is already released, so
    // load_plugin() can start the replacement load itself. Returns false with the reason.
    auto reload_plugin = [&](std::string& reason) {
        release_update();
        update_scope.reset();
        load_plugin(plugin_key);
        std::string load_error;
        if (wait_for_plugin_load(plugin_key, std::chrono::minutes(5), load_error) && is_plugin_loaded(plugin_key))
            return true;
        reason = load_error.empty() ? _u8L("Plugin failed to load.") : load_error;
        return false;
    };

    // An empty version means "update the current plugin". If there is no update available, the
    // intention is unclear.
    if (version.empty()) {
        if (descriptor.cloud->update_available) {
            version = descriptor.latest_available_version();
        } else {
            error = _u8L("No update is available for this plugin.");
            set_plugin_error(plugin_key, error);
            return false;
        }
    }

    // Keep the plugin's current error before the clear below. It is only meant to drop a stale
    // error from an earlier update attempt; if this update fails and the restored package is not
    // reloaded, a plugin that was already broken must not end up looking healthy.
    const std::string previous_error = descriptor.normalized_error();
    clear_plugin_error(plugin_key);

    // Pin the account target before any network work. If the account was not set when the update
    // started, the signed-in agent is authoritative (startup ordering); reject an update with no
    // known account, so a sign-in during the download cannot silently retarget the install.
    if (update_cloud_user.empty()) {
        sync_cloud_user_from_agent();
        std::lock_guard<std::mutex> lock(m_mutex);
        update_cloud_user = m_cloud_user_id;
    }
    if (update_cloud_user.empty()) {
        error = _u8L("Sign in to OrcaCloud to update this plugin.");
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Download before touching the installed package: a failed download must leave the old package
    // and its sidecar exactly as they were.
    CloudPluginDownload download;
    if (!m_cloud_service.download_cloud_plugin(descriptor, version, download, error)) {
        if (error.empty())
            error = _u8L("Failed to download cloud plugin.");
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Re-read after the download: a capability may have been toggled while it ran, and the plugin
    // may have been removed or replaced. This snapshot is what the update preserves.
    PluginDescriptor previous;
    if (!try_get_plugin_descriptor(plugin_key, previous)) {
        error = GUI::format(_u8L("Plugin not found: %1%"), plugin_key);
        boost::system::error_code ec;
        boost::filesystem::remove(download.package_path, ec);
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to find plugin key " << plugin_key;
        return false;
    }

    const bool had_local_package = previous.has_local_package();
    const bool was_loaded        = had_local_package && is_plugin_loaded(plugin_key);
    const bool was_enabled       = !had_local_package || was_loaded || previous.enabled;

    PluginInstallState previous_state;
    bool had_previous_state        = false;
    bool previous_state_unreadable = false;
    if (had_local_package && !previous.plugin_root.empty()) {
        std::lock_guard<std::mutex> state_lock(install_state_mutex());
        const InstallStateReadStatus status =
            read_install_state_status(boost::filesystem::path(previous.plugin_root), previous_state);
        had_previous_state        = status == InstallStateReadStatus::Readable;
        previous_state_unreadable = status == InstallStateReadStatus::Unreadable;
    }

    // A sidecar that exists but cannot be read may still hold the user's capability choices and
    // grants. Replacing the package would reset it with an empty capability list, so abort while
    // the installed package and its sidecar are still untouched. Absent is a package without
    // stored choices and keeps the normal replacement path.
    if (previous_state_unreadable) {
        const boost::filesystem::path previous_root(previous.plugin_root);
        error = GUI::format(_u8L("Failed to read plugin install state: %1%"),
                            (previous_root / INSTALL_STATE_FILE).string());
        boost::system::error_code ec;
        boost::filesystem::remove(download.package_path, ec);
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error;
        return false;
    }

    // Check the account before touching the installed package. It must still be the one captured
    // before the download: a sign-in/sign-out or account switch while it ran would otherwise
    // install the replacement into a different user's directory. The pinned id is passed to the
    // installer, so a change after this check cannot redirect the install either.
    bool account_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        account_changed = m_cloud_user_id != update_cloud_user;
    }
    if (account_changed) {
        error = _u8L("The cloud account changed while the plugin was updating. Please try again.");
        boost::system::error_code ec;
        boost::filesystem::remove(download.package_path, ec);
        set_plugin_error(plugin_key, error);
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error << " (" << plugin_key << ")";
        return false;
    }

    boost::filesystem::path resolved_root;
    if (had_local_package) {
        bool resolved_allowed_plugin_root = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            resolved_allowed_plugin_root = resolve_allowed_plugin_root(previous, get_plugin_directories(update_cloud_user),
                                                                       _u8L("Refusing to delete a plugin outside the known plugin directories."),
                                                                       resolved_root, error);
        }

        if (!resolved_allowed_plugin_root) {
            boost::system::error_code ec;
            boost::filesystem::remove(download.package_path, ec);
            set_plugin_error(plugin_key, error);
            return false;
        }

        unload_plugin(plugin_key);
    }

    std::string install_error;
    CloudUpdateFailure failure = CloudUpdateFailure::PreviousPackageUsable;

    // The installer writes the sidecar for the new package before it removes the backup, so it
    // needs the user's activation intent and capability choices up front. Permissions are not
    // carried over: the replacement is new executable code and must request them again.
    PluginInstallState preserved_state;
    if (had_local_package) {
        preserved_state.enabled = was_enabled;
        if (had_previous_state)
            preserved_state.capabilities = previous_state.capabilities;
    }

    if (!install_cloud_plugin_package(plugin_key, version, download.package_path, previous, &update_cloud_user, install_error,
                                      &failure, had_local_package ? &preserved_state : nullptr)) {
        // Restoring the old state and reloading the old package is only meaningful when the
        // installer left that package in place. A committed replacement must not inherit the old
        // package's grants.
        bool previous_package_usable = failure == CloudUpdateFailure::PreviousPackageUsable;

        // A failed rollback concerns only the directory the installer wrote to. When the package
        // this update preserves lives at a different root, that rollback did not touch it. Allow
        // the restore/reload only after proving the old package is still there; any path or
        // lookup error keeps the conservative "not usable" result, so old grants can never be
        // applied to a replacement package.
        if (failure == CloudUpdateFailure::RollbackFailed && had_local_package && !resolved_root.empty() &&
            !previous.cloud_uuid().empty()) {
            const boost::filesystem::path install_target =
                boost::filesystem::path(get_cloud_plugin_dir(update_cloud_user)) / previous.cloud_uuid();
            boost::system::error_code target_ec;
            const boost::filesystem::path target_root = boost::filesystem::weakly_canonical(install_target, target_ec);
            if (!target_ec && resolved_root != target_root) {
                boost::system::error_code exists_ec;
                const bool old_package_present =
                    !previous.entry_path.empty()
                        ? boost::filesystem::is_regular_file(boost::filesystem::path(previous.entry_path), exists_ec)
                        : boost::filesystem::is_directory(boost::filesystem::path(previous.plugin_root), exists_ec);
                previous_package_usable = !exists_ec && old_package_present;
            }
        }

        if (had_local_package && previous_package_usable)
            restore_plugin_state_after_failed_update(previous, previous_state, had_previous_state, was_enabled);

        bool previous_reloaded = true;
        if (was_loaded && previous_package_usable) {
            std::string reason;
            if (!reload_plugin(reason)) {
                previous_reloaded = false;
                install_error += "\n" + GUI::format(_u8L("The previous version also failed to reload: %1%"), reason);
            }
        }
        if (had_local_package && failure == CloudUpdateFailure::RollbackFailed && !previous_package_usable) {
            install_error += "\n" + _u8L("The previous plugin files could not be restored.");
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Previous package of " << plugin_key
                                       << " could not be restored; its permissions were not reapplied";
        }

        error = std::move(install_error);
        // The update failed, but a previous package that was restored and reloaded is still
        // working. Report the update error without marking the plugin broken. An unrestored
        // package or a failed reload leaves the plugin unusable, so it is marked.
        const bool previous_plugin_working = had_local_package && previous_package_usable && previous_reloaded;
        if (!previous_plugin_working) {
            set_plugin_error(plugin_key, error);
        } else if (!was_loaded && !previous_error.empty()) {
            // Nothing was reloaded, so a plugin that was already broken stays broken. The clear
            // above discarded its real error; put it back instead of leaving an unhealthy plugin
            // looking fine.
            set_plugin_error(plugin_key, previous_error);
        }
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error;
        return false;
    }

    // The installer committed the replacement with the preserved activation and capability state
    // and every permission cleared, so the new package is ready to load.
    PluginDescriptor installed;
    if (!try_get_plugin_descriptor(plugin_key, installed)) {
        error = _u8L("Plugin not found.");
        set_plugin_error(plugin_key, error);
        return false;
    }

    // The installed descriptor may point at another allowed directory (for example a package
    // retained from an earlier install location). The installer only replaces the directory it
    // installs into. Keep that package until the new one is fully committed (sidecar written and,
    // when applicable, reloaded) so any failure still leaves a fallback on disk.
    auto remove_stale_old_package = [&]() {
        if (!had_local_package || resolved_root.empty() || installed.plugin_root.empty())
            return;

        boost::system::error_code equivalent_ec;
        const bool same_root = boost::filesystem::equivalent(resolved_root, installed.plugin_root, equivalent_ec);
        if (!equivalent_ec && !same_root) {
            std::string cleanup_error;
            if (!delete_plugin_root(resolved_root, plugin_key, cleanup_error))
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << cleanup_error;
        }
    };

    if (was_loaded) {
        // The package is already installed and its state persisted, so a failure here is reported
        // as a reload failure rather than hiding the successful update.
        std::string reason;
        if (!reload_plugin(reason)) {
            error = GUI::format(_u8L("Updated the plugin, but failed to reload it: %1%"), reason);
            set_plugin_error(plugin_key, error);
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << error;
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Cloud plugin " << plugin_key
                                << " updated and reloaded";
    } else if (!was_enabled) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Cloud plugin " << plugin_key
                                << " updated and kept deactivated";
    }

    remove_stale_old_package();
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
                                                                   _u8L("Refusing to delete a plugin outside the known plugin directories."),
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
                                                                   _u8L("Refusing to update a plugin outside the known plugin directories."),
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
        // Sidecar read-modify-write first, then the registry lock: that is the lock order every
        // install-state writer uses (install_state_mutex -> m_mutex).
        std::lock_guard<std::mutex> state_lock(install_state_mutex());
        std::lock_guard<std::mutex> lock(m_mutex);

        Plugin* entry = find_plugin_locked(old_key);
        if (entry == nullptr) {
            error = GUI::format(_u8L("Plugin manifest not found: %1%"), old_key);
            return false;
        }

        if (find_plugin_locked(new_key) != nullptr && new_key != old_key) {
            error = GUI::format(_u8L("Cannot keep plugin local: local plugin key already exists: %1%"), new_key);
            BOOST_LOG_TRIVIAL(warning) << error;
            return false;
        }

        // Preserve the sidecar's package and capability choices while changing only its origin.
        // Re-keying an installed plugin must not silently re-enable capabilities.
        PluginInstallState           install_state;
        const InstallStateReadStatus install_state_status = read_install_state_status(resolved_root, install_state);
        if (install_state_status == InstallStateReadStatus::Unreadable) {
            // The sidecar exists but cannot be read, so its permissions and capability choices may
            // still be recoverable: fail without replacing it and without re-keying the registry
            // entry, leaving the plugin exactly as it was.
            error = GUI::format(_u8L("Failed to read plugin install state: %1%"),
                                (resolved_root / INSTALL_STATE_FILE).string());
            BOOST_LOG_TRIVIAL(warning) << error;
            return false;
        }
        if (install_state_status == InstallStateReadStatus::Absent) {
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
            error = GUI::format(_u8L("Failed to update plugin install state: %1%"), (resolved_root / INSTALL_STATE_FILE).string());
            return false;
        }

        entry->descriptor.plugin_key = new_key;
        entry->descriptor.cloud.reset();
        entry->descriptor.clear_error();
        // The package identity changed, so neither key may keep an old failure's prompt alive.
        m_failed_loads.erase(old_key);
        m_failed_loads.erase(new_key);

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
    if (keep_local && plugin.has_local_package()) {
        if (!keep_installed_plugin_as_local(plugin, error))
            return false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Removed cloud tracking, kept local copy: " << plugin.plugin_key;
        return true;
    }

    if (plugin.has_local_package()) {
        if (!delete_installed_plugin_package(plugin, error))
            return false;

        fetch_plugins_from_cloud();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Deleted local package after cloud removal: " << plugin.plugin_key;
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failed_loads.erase(plugin.plugin_key);
        m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(),
                                       [&plugin](const Plugin& entry) { return entry.descriptor.plugin_key == plugin.plugin_key; }),
                        m_plugins.end());
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Removed cloud-only plugin: " << plugin.plugin_key;
    return true;
}

bool PluginManager::delete_plugin(const std::string& plugin_key, std::string& error)
{
    if (!wait_for_discovery(std::chrono::milliseconds::max(), error))
        return false;

    error.clear();

    PluginDescriptor descriptor;
    if (!try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = GUI::format(_u8L("Plugin not found: %1%"), plugin_key);
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
        error = GUI::format(_u8L("Plugin not found: %1%"), plugin_key);
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = _u8L("Only cloud plugins can be unsubscribed.");
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (descriptor.cloud && descriptor.cloud->is_mine) {
        error = _u8L("Cannot unsubscribe your own plugins.");
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
        error = GUI::format(_u8L("Plugin not found: %1%"), plugin_key);
        return false;
    }

    if (!descriptor.is_cloud_plugin()) {
        error = _u8L("Only cloud plugins can be deleted and unsubscribed.");
        set_plugin_error(plugin_key, error);
        return false;
    }

    if (descriptor.cloud->is_mine) {
        error = _u8L("Cannot unsubscribe your own plugins. Use Delete to remove the local files.");
        set_plugin_error(plugin_key, error);
        return false;
    }

    unload_plugin(plugin_key);

    if (!m_cloud_service.request_cloud_unsubscribe(descriptor, error)) {
        set_plugin_error(plugin_key, error);
        return false;
    }

    return finalize_cloud_plugin_removal(descriptor, false, error);
}

// ── Async Script runtime ────────────────────────────────────────────────────────────────────

namespace {

// The asyncio loop of the Script invocation running on the calling thread. Owned +1 while set;
// always cleared under the GIL before the worker thread exits.
thread_local PyObject* tl_script_loop = nullptr;

} // namespace

struct ScriptAsyncRuntime::Impl
{
    struct Run
    {
        std::string plugin_key;
        std::string capability_name;
        std::shared_ptr<PluginCapabilityInterface> capability;
        std::function<void(const ScriptCompletion&)> on_done;
        std::atomic<bool> cancel_requested{false};
        // True while an asyncio loop/task is live for this run. Read without the GIL by
        // cancel_plugin() to avoid taking the GIL for a purely synchronous invocation.
        std::atomic<bool> has_loop{false};

        // Live asyncio loop/task for cancellation. Guarded by handle_mutex and only touched
        // with the GIL held.
        std::mutex       handle_mutex;
        pybind11::object loop;
        pybind11::object task;
    };

    std::mutex                        mutex;
    std::condition_variable           cv;
    std::vector<std::shared_ptr<Run>> active;
    bool                              stopping = false;

    // Runs one invocation to completion on the calling (worker) thread.
    void run_invocation(const std::shared_ptr<Run>& run);
};

void ScriptAsyncRuntime::Impl::run_invocation(const std::shared_ptr<Run>& run)
{
    ScriptCompletion completion;

    {
        PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
        if (!lease) {
            completion.error = _u8L("Python interpreter is shutting down.");
        } else {
            pybind11::gil_scoped_acquire gil;

            if (run->cancel_requested.load(std::memory_order_acquire)) {
                // Cancelled before it ran: expected termination, never an error.
                completion.cancelled = true;
            } else {
                // Hold the capability reference, the audit scope and the GIL for the whole
                // invocation, so unload/shutdown cannot tear the module out from under it.
                PluginCapabilityInterface::RefCounter ref_counter(*run->capability);

                std::optional<ScopedPluginAuditContext> audit_scope;
                if (!run->capability->audit_plugin_key().empty())
                    audit_scope.emplace(run->capability->audit_plugin_key(), run->capability->name());

                // Detection and execution both happen here, on the worker: a synchronous
                // `def execute` runs to completion off the UI thread, exactly like an `async def`.
                ScriptExecuteInvocation invocation = invoke_script_execute(run->capability);
                if (!invocation.error.empty()) {
                    completion.error = invocation.error;
                } else if (!invocation.is_async) {
                    completion.result = invocation.result;
                } else if (run->cancel_requested.load(std::memory_order_acquire)) {
                    completion.cancelled = true;
                } else {
                    try {
                        pybind11::module_ asyncio = pybind11::module_::import("asyncio");
                        pybind11::object  loop    = asyncio.attr("new_event_loop")();
                        asyncio.attr("set_event_loop")(loop);

                        tl_script_loop = loop.ptr();
                        Py_INCREF(tl_script_loop);

                        pybind11::object task = loop.attr("create_task")(std::move(invocation.awaitable));
                        {
                            std::lock_guard<std::mutex> lock(run->handle_mutex);
                            run->loop = loop;
                            run->task = task;
                        }
                        run->has_loop.store(true, std::memory_order_release);
                        // A cancellation that arrived before the loop came up must still land.
                        if (run->cancel_requested.load(std::memory_order_acquire))
                            loop.attr("call_soon_threadsafe")(task.attr("cancel"));

                        try {
                            pybind11::object returned = loop.attr("run_until_complete")(task);
                            completion.result = returned.cast<ExecutionResult>();
                        } catch (pybind11::error_already_set& err) {
                            // Only our own cancel_plugin() cancellation is expected termination; a
                            // CancelledError the plugin raised on its own stays a real error.
                            if (err.matches(asyncio.attr("CancelledError")) &&
                                run->cancel_requested.load(std::memory_order_acquire)) {
                                completion.cancelled = true;
                            } else {
                                log_python_exception_keep(err);
                                completion.error = err.what();
                            }
                        }

                        run->has_loop.store(false, std::memory_order_release);
                        {
                            std::lock_guard<std::mutex> lock(run->handle_mutex);
                            run->task = pybind11::object();
                            run->loop = pybind11::object();
                        }
                        Py_CLEAR(tl_script_loop);
                        loop.attr("close")();
                        asyncio.attr("set_event_loop")(pybind11::none());
                    } catch (pybind11::error_already_set& err) {
                        log_python_exception_keep(err);
                        completion.error = err.what();
                    } catch (const std::exception& ex) {
                        completion.error = ex.what();
                    }

                    // Covers an exception thrown after the loop came up but before the clear above.
                    if (run->has_loop.load(std::memory_order_acquire)) {
                        run->has_loop.store(false, std::memory_order_release);
                        std::lock_guard<std::mutex> lock(run->handle_mutex);
                        run->task = pybind11::object();
                        run->loop = pybind11::object();
                    }

                    if (tl_script_loop != nullptr)
                        Py_CLEAR(tl_script_loop);
                }
            }

            // Drop the capability reference while the GIL is held: releasing a pybind holder from
            // a non-Python thread would DECREF the capability without the GIL. Runs on every
            // lease-valid path, including an invocation cancelled before it ran.
            run->capability.reset();
        }
    }

    // A cancellation requested by cancel_plugin() (unload/shutdown) is expected termination and is
    // authoritative: whatever the body did — finished, swallowed CancelledError, or failed — nothing
    // is surfaced as an error or as a normal success.
    if (run->cancel_requested.load(std::memory_order_acquire)) {
        completion.cancelled = true;
        completion.error.clear();
    }

    auto on_done = std::move(run->on_done);
    auto deliver = [on_done, completion]() {
        if (on_done)
            on_done(completion);
    };

    if (wxTheApp == nullptr || wxIsMainThread())
        deliver();
    else
        GUI::wxGetApp().CallAfter(deliver);

    // Leave `active` last, so a drain cannot observe the run as finished before its completion was
    // queued/delivered and its capability reference released.
    {
        std::lock_guard<std::mutex> lock(mutex);
        active.erase(std::remove(active.begin(), active.end(), run), active.end());
    }
    cv.notify_all();
}

ScriptAsyncRuntime& ScriptAsyncRuntime::instance()
{
    static ScriptAsyncRuntime inst;
    return inst;
}

ScriptAsyncRuntime::ScriptAsyncRuntime() : m_impl(std::make_unique<Impl>()) {}

ScriptAsyncRuntime::~ScriptAsyncRuntime() { shutdown(); }

bool ScriptAsyncRuntime::start(const std::string& plugin_key,
                               const std::string& capability_name,
                               std::shared_ptr<PluginCapabilityInterface> capability,
                               std::function<void(const ScriptCompletion&)> on_done)
{
    if (!PythonInterpreter::instance().is_initialized())
        return false;

    auto run             = std::make_shared<Impl::Run>();
    run->plugin_key      = plugin_key;
    run->capability_name = capability_name;
    run->capability      = std::move(capability);
    run->on_done         = std::move(on_done);

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->stopping)
            return false;
        m_impl->active.push_back(run);
    }

    // One worker thread per invocation. A stalled invocation of one plugin can never delay another
    // plugin's unload or another invocation, and no invocation body ever runs on the caller (UI)
    // thread. The thread holds `run` (and so the capability) until it finishes; the runtime tracks
    // it in `active` and waits for it on shutdown.
    Impl* impl = m_impl.get();
    try {
        std::thread([impl, run] { impl->run_invocation(run); }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->active.erase(std::remove(m_impl->active.begin(), m_impl->active.end(), run), m_impl->active.end());
        return false;
    }
    return true;
}

void ScriptAsyncRuntime::cancel_plugin(const std::string& plugin_key)
{
    std::vector<std::shared_ptr<Impl::Run>> runs;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (const auto& run : m_impl->active)
            if (run->plugin_key == plugin_key)
                runs.push_back(run);
    }

    for (const auto& run : runs)
        run->cancel_requested.store(true, std::memory_order_release);

    for (const auto& run : runs) {
        // A synchronous invocation has no loop to cancel; skip it so the caller never blocks on
        // the GIL while that invocation is (uninterruptibly) running.
        if (!run->has_loop.load(std::memory_order_acquire))
            continue;

        PythonGILState gil;
        if (!gil)
            continue;

        pybind11::object loop;
        pybind11::object task;
        {
            std::lock_guard<std::mutex> lock(run->handle_mutex);
            loop = run->loop;
            task = run->task;
        }
        if (!loop || !task)
            continue;

        try {
            loop.attr("call_soon_threadsafe")(task.attr("cancel"));
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "Script async cancel failed for plugin '" << plugin_key << "': " << ex.what();
        }
    }
}

bool ScriptAsyncRuntime::has_active_run(const PluginCapabilityInterface* capability) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (const auto& run : m_impl->active)
        if (run->capability.get() == capability)
            return true;
    return false;
}

void ScriptAsyncRuntime::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->stopping = true;
    }

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->cv.wait(lock, [this] { return m_impl->active.empty(); });
}

void ScriptAsyncRuntime::reset()
{
    shutdown();

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->stopping = false;
}

pybind11::object ScriptAsyncRuntime::current_loop()
{
    if (tl_script_loop == nullptr)
        return pybind11::object();
    return pybind11::reinterpret_borrow<pybind11::object>(pybind11::handle(tl_script_loop));
}

ExecutionResult PluginManager::run_script_capability(const std::string& plugin_key, const std::string& capability_name, std::string& error)
{
    ExecutionResult result;
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;

    ScriptLaunchResult launch = launch_script_capability(
        plugin_key, capability_name, [&](const ScriptCompletion& completion) {
            std::lock_guard<std::mutex> lock(mutex);
            result = completion.result;
            // A cancel_plugin() cancellation is expected termination, not an error to report.
            error  = completion.cancelled ? std::string() : completion.error;
            done   = true;
            cv.notify_all();
        });

    if (launch.launched_async) {
        // Async run: the completion is delivered on the GUI thread. When this call is itself on
        // the GUI thread it must pump the event loop, or the completion can never be delivered.
        std::unique_lock<std::mutex> lock(mutex);
        if (wxTheApp != nullptr && wxIsMainThread()) {
            while (!done) {
                cv.wait_for(lock, std::chrono::milliseconds(10));
                lock.unlock();
                wxYield();
                lock.lock();
            }
        } else {
            cv.wait(lock, [&] { return done; });
        }
    } else {
        result = launch.outcome.result;
        error  = launch.outcome.error;
    }

    return result;
}

PluginManager::ScriptLaunchResult PluginManager::launch_script_capability(const std::string& plugin_key,
                                                                          const std::string& capability_name,
                                                                          ScriptCompletionFn  on_done)
{
    ScriptLaunchResult launch;

    // Mirror the historical synchronous behavior for a dangling reference: nothing to run and
    // no error (the caller's guarded UI normally prevents this).
    if (plugin_key.empty() || capability_name.empty())
        return launch;

    if (m_shutting_down.load(std::memory_order_acquire)) {
        launch.outcome.error = _u8L("Plugin system is shutting down.");
        return launch;
    }

    auto cap = get_plugin_capability({PluginCapabilityType::Script, capability_name, plugin_key});
    if (!cap)
        return launch;

    if (!std::dynamic_pointer_cast<ScriptPluginCapability>(cap))
        return launch;

    // Every Script body runs off the caller's thread now: the invocation — including detection and
    // a synchronous `def execute` — is handed to the runtime, which runs it on its own worker and
    // reports the outcome through the completion callback. Nothing executes before returning here,
    // so the UI thread is never blocked by a Script invocation.
    const bool started = ScriptAsyncRuntime::instance().start(
        plugin_key, capability_name, cap,
        [this, plugin_key, on_done](const ScriptCompletion& completion) {
            // A cancellation requested by cancel_plugin() (unload/shutdown) is expected termination:
            // record no plugin error and surface nothing. Genuine user/plugin errors still record.
            if (!completion.cancelled && !completion.error.empty())
                set_plugin_error(plugin_key, completion.error);
            if (on_done)
                on_done(completion);
        });

    if (!started) {
        launch.outcome.error = _u8L("Plugin system is shutting down.");
        set_plugin_error(plugin_key, launch.outcome.error);
        BOOST_LOG_TRIVIAL(error) << "Script plugin execution failed. plugin_key=" << plugin_key
                                 << " error=" << launch.outcome.error;
        return launch;
    }

    launch.launched_async = true;
    return launch;
}

void PluginManager::dispatch_lifecycle_event(LifecycleEvent evt, const LifecycleEventContext& ctx) {
    const auto is_canceled = [&ctx]() {
        return ctx.cancellation_check && ctx.cancellation_check();
    };

    if (is_canceled())
        return;

    for (const auto& cap : get_plugin_capabilities()) {
        if (!cap || !cap->is_enabled()) continue;
        if (is_canceled())
            break;
        try {
            cap->on_lifecycle_event(evt, ctx);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": plugin '" << cap->audit_plugin_key() << "/" << cap->name()
                                       << "' on_lifecycle_event(" << lifecycle_event_to_string(evt) << ") threw: " << ex.what()
                                       << " [ctx name='" << ctx.name << "', code=" << lifecycle_evt_code_to_string(ctx.code)
                                       << ", msg='" << ctx.msg << "']";
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": plugin '" << cap->audit_plugin_key() << "/" << cap->name()
                                       << "' on_lifecycle_event(" << lifecycle_event_to_string(evt)
                                       << ") threw a non-standard exception"
                                       << " [ctx name='" << ctx.name << "', code=" << lifecycle_evt_code_to_string(ctx.code)
                                       << ", msg='" << ctx.msg << "']";
        }
    }
}

} // namespace Slic3r
