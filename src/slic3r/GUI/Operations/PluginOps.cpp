#include "PluginOps.hpp"

#include "Outcome.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PluginSource.hpp"
#include "slic3r/GUI/PluginStatus.hpp"
#include "slic3r/GUI/PluginsDialog.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <cassert>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

Refusal no_such_plugin(const std::string& plugin, std::vector<std::string> keys)
{
    return Refusal{RefusalKind::Precondition, "no_such_plugin",
                   // TRN %1% is what a call named a plugin by.
                   L("No plugin is named \"%1%\"."), {plugin}, closest_names(std::move(keys), plugin)};
}

Refusal plugin_loading(const std::string& name)
{
    // TRN %1% is a plugin's name.
    return Refusal{RefusalKind::Busy, "plugin_loading", L("\"%1%\" is loading; try again when its load has ended."), {name}};
}

// A cloud plugin that is no longer available, with its package removed so that it is listed to be installed again.
PluginDescriptor as_cloud_only_descriptor(PluginDescriptor descriptor)
{
    descriptor.plugin_root.clear();
    descriptor.entry_path.clear();
    descriptor.installed_version.clear();
    // No local package is left behind, so the package verdict and any load error it produced go with it.
    descriptor.metadata_valid = false;
    descriptor.clear_error();
    if (descriptor.cloud.has_value()) {
        descriptor.cloud->installed        = false;
        descriptor.cloud->update_available = false;
    }
    return descriptor;
}

} // namespace

// ---- The list ----

PluginSource plugin_source(const PluginDescriptor& plugin)
{
    const bool has_cloud_meta = plugin.cloud.has_value();
    const bool is_cloud       = plugin.is_cloud_plugin();
    const bool is_mine        = is_cloud && has_cloud_meta && plugin.cloud->is_mine;

    if (is_cloud && has_cloud_meta && plugin.cloud->orphaned)
        return PluginSource::Orphaned;
    if (is_mine)
        return PluginSource::Mine;
    if (is_cloud)
        return PluginSource::Subscribed;
    return PluginSource::Local;
}

std::string installed_plugin_version(const PluginDescriptor& plugin)
{
    // The cloud merge overwrites `version` with the latest cloud version, so the preserved local one comes first,
    // `version` serving local-only and pre-merge descriptors.
    if (!plugin.has_local_package())
        return {};
    return plugin.installed_version.empty() ? plugin.version : plugin.installed_version;
}

PluginActions plugin_actions(const PluginDescriptor& plugin, bool loading)
{
    PluginActions actions;
    const bool is_cloud    = plugin.is_cloud_plugin();
    const bool is_orphaned = plugin.cloud.has_value() && plugin.cloud->orphaned;
    const bool is_mine     = plugin_source(plugin) == PluginSource::Mine;
    const bool has_local   = plugin.has_local_package();

    actions.toggle_installs_cloud_plugin = is_cloud && !is_orphaned && !has_local && !plugin.is_unauthorized();
    actions.can_toggle                   = !loading && (has_local || actions.toggle_installs_cloud_plugin);
    // The person's own cloud plugin falls through to the local delete: it removes the installed package only. Deleting
    // a plugin from the cloud is a plugin hub operation and is never offered.
    actions.unsubscribe = is_cloud && !is_orphaned && !is_mine;
    actions.remove      = !actions.unsubscribe && has_local;
    actions.open_folder = has_local;
    actions.reinstall   = !is_orphaned && is_cloud;
    actions.reload      = !is_orphaned && !is_cloud;
    return actions;
}

std::vector<PluginRow> plugin_rows_from(const std::vector<PluginDescriptor>& plugins, const std::map<std::string, PluginRuntime>& runtime)
{
    std::vector<PluginRow> rows;
    rows.reserve(plugins.size());
    for (const PluginDescriptor& plugin : plugins) {
        const auto          found = runtime.find(plugin.plugin_key);
        const PluginRuntime state = found != runtime.end() ? found->second : PluginRuntime{};
        const std::string   installed = installed_plugin_version(plugin);
        PluginRow           row;
        row.key          = plugin.plugin_key;
        row.name         = plugin.name;
        row.version      = installed.empty() ? plugin.latest_available_version() : installed;
        row.installed    = plugin.has_local_package();
        row.enabled      = state.loaded;
        row.status       = to_string(resolve_plugin_status(state.loading, plugin.has_error(), state.loaded));
        row.source       = to_string(plugin_source(plugin));
        row.capabilities = state.capabilities;
        row.error        = plugin.normalized_error();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<PluginRow> plugin_rows()
{
    PluginManager&                       manager = PluginManager::instance();
    const std::vector<PluginDescriptor>  plugins = manager.get_plugin_descriptors(/*include_invalid=*/true);
    std::map<std::string, PluginRuntime> runtime;
    for (const PluginDescriptor& plugin : plugins) {
        PluginRuntime& state = runtime[plugin.plugin_key];
        state.loaded         = manager.is_plugin_loaded(plugin.plugin_key);
        state.loading        = manager.is_plugin_load_in_progress(plugin.plugin_key);
        // A package that is not loaded has no capabilities.
        for (const auto& capability : manager.get_plugin_capabilities(plugin.plugin_key, PluginCapabilityType::Unknown,
                                                                      /*only_enabled=*/false))
            state.capabilities.push_back(
                {capability->identity().name, plugin_capability_type_to_string(capability->type()), capability->is_enabled()});
    }
    return plugin_rows_from(plugins, runtime);
}

bool plugin_descriptor(const std::string& plugin, PluginDescriptor& descriptor)
{
    PluginManager& manager = PluginManager::instance();
    if (manager.try_get_valid_plugin_descriptor(plugin, descriptor))
        return true;
    return manager.try_get_plugin_descriptor(plugin, descriptor) && descriptor.is_invalid_package();
}

Outcome<PluginDescriptor> find_plugin(const std::string& plugin)
{
    PluginDescriptor descriptor;
    if (plugin_descriptor(plugin, descriptor))
        return descriptor;
    std::vector<std::string>      keys;
    std::vector<PluginDescriptor> named;
    for (const PluginDescriptor& listed : PluginManager::instance().get_plugin_descriptors(/*include_invalid=*/true)) {
        keys.push_back(listed.plugin_key);
        if (listed.name == plugin && plugin_descriptor(listed.plugin_key, descriptor))
            named.push_back(descriptor);
    }
    if (named.size() == 1)
        return named.front();
    if (named.size() > 1) {
        std::vector<std::string> named_keys;
        for (const PluginDescriptor& one : named)
            named_keys.push_back(one.plugin_key);
        return Refusal{RefusalKind::Precondition, "ambiguous_plugin",
                       // TRN %1% is a plugin's name.
                       L("Several plugins are named \"%1%\": name one by its key."), {plugin}, named_keys};
    }
    return no_such_plugin(plugin, std::move(keys));
}

// ---- Turning a plugin or a capability on or off ----

Outcome<PluginEnabled> set_plugin_enabled(const SetPluginEnabledArgs& args)
{
    assert(wxThread::IsMain());
    const std::string& key = args.plugin;
    PluginDescriptor   row;
    if (!plugin_descriptor(key, row))
        return no_such_plugin(key, {});
    PluginManager& manager = PluginManager::instance();
    PluginEnabled  done{key, args.capability, PluginChange::None};

    if (!args.capability.empty()) {
        const auto capabilities = manager.get_plugin_capabilities(key, PluginCapabilityType::Unknown, /*only_enabled=*/false);
        if (capabilities.empty() && !manager.is_plugin_loaded(key))
            return Refusal{RefusalKind::Precondition, "plugin_not_enabled",
                           // TRN %1% is a plugin's name.
                           L("\"%1%\" is not enabled, so its capabilities are not loaded: enable it first."), {row.name}};
        std::vector<std::string>                                names;
        std::vector<std::shared_ptr<PluginCapabilityInterface>> matching;
        for (const auto& capability : capabilities) {
            const PluginCapabilityId id = capability->identity();
            names.push_back(id.name);
            if (id.name == args.capability && (args.capability_type == PluginCapabilityType::Unknown || id.type == args.capability_type))
                matching.push_back(capability);
        }
        if (matching.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot toggle missing plugin capability: " << key << " | " << args.capability;
            return Refusal{RefusalKind::Precondition, "no_such_capability",
                           // TRN %1% is a capability's name, %2% a plugin's.
                           L("\"%2%\" has no capability named \"%1%\"."), {args.capability, row.name}, names};
        }
        if (matching.size() > 1) {
            std::vector<std::string> types;
            for (const auto& capability : matching)
                types.push_back(plugin_capability_type_to_string(capability->type()));
            return Refusal{RefusalKind::Precondition, "ambiguous_capability",
                           // TRN %1% is a capability's name.
                           L("Several capabilities are named \"%1%\"."), {args.capability}, types};
        }
        const bool was_enabled = matching.front()->is_enabled();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": " << (args.enabled ? "Enabling" : "Disabling") << " plugin capability: " << key
                                << " | " << args.capability;
        manager.set_capability_enabled(matching.front()->identity(), args.enabled);
        if (was_enabled != args.enabled)
            done.change = args.enabled ? PluginChange::CapabilityEnabled : PluginChange::CapabilityDisabled;
        return done;
    }

    BOOST_LOG_TRIVIAL(info) << "Toggle plugin request. plugin_key=" << key << " enabled=" << args.enabled;
    if (!args.enabled) {
        const bool was_loaded = manager.is_plugin_loaded(key);
        if (!manager.unload_plugin(key)) {
            BOOST_LOG_TRIVIAL(error) << "Failed to unload plugin: " << key;
            return Refusal{RefusalKind::Failed, "not_unloaded", L("Failed to unload plugin.")};
        }
        BOOST_LOG_TRIVIAL(info) << "Plugin unloaded: " << key;
        // A plugin switched off has no meaningful error state.
        if (!manager.clear_plugin_error(key))
            BOOST_LOG_TRIVIAL(warning) << "Failed to clear plugin error for " << key << " (failed to find)";
        if (was_loaded)
            done.change = PluginChange::Unloaded;
        return done;
    }

    const bool          loading = manager.is_plugin_load_in_progress(key);
    const PluginActions actions = plugin_actions(row, loading);
    if (!actions.can_toggle) {
        if (row.is_unauthorized() && !actions.toggle_installs_cloud_plugin && !row.has_local_package())
            return Refusal{RefusalKind::Precondition, "unauthorized_plugin", L("Unauthorized cloud plugins cannot be installed.")};
        if (loading)
            return plugin_loading(row.name);
        return Refusal{RefusalKind::Precondition, "not_installable",
                       // TRN %1% is a plugin's name.
                       L("\"%1%\" is no longer available from the cloud, and it is not installed."), {row.name}};
    }

    if (actions.toggle_installs_cloud_plugin) {
        const std::function<Outcome<Done>()> work = [key, version = row.version] { return download_cloud_plugin(key, version); };
        const Outcome<Done> downloaded = args.run_download ? args.run_download(work) : wait_off_ui_thread(work);
        if (!downloaded)
            return downloaded.refusal();
        BOOST_LOG_TRIVIAL(info) << "Cloud plugin installed locally: " << key;
        // The download updated the descriptor already, so no rescan is needed here.
        if (!plugin_descriptor(key, row))
            return no_such_plugin(key, {});
        if (row.has_error())
            return Refusal{RefusalKind::Failed, "plugin_error", row.normalized_error()};
    }

    if (manager.is_plugin_loaded(key))
        return done;
    PluginDescriptor valid_descriptor;
    if (!manager.try_get_valid_plugin_descriptor(key, valid_descriptor))
        return Refusal{RefusalKind::Precondition, "manifest_not_found", L("Plugin manifest was not found.")};
    if (manager.is_plugin_load_in_progress(key))
        return plugin_loading(row.name);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Starting plugin load: " << key;
    manager.load_plugin(key);
    done.change = PluginChange::LoadStarted;
    return done;
}

Outcome<Done> download_cloud_plugin(const std::string& plugin, const std::string& version)
{
    BOOST_LOG_TRIVIAL(info) << "Downloading cloud plugin. plugin_key=" << plugin;
    std::string error;
    bool        downloaded = false;
    try {
        downloaded = PluginManager::instance().download_and_install_cloud_plugin(plugin, version, error);
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = L("Unknown error");
    }
    if (downloaded) {
        BOOST_LOG_TRIVIAL(info) << "Cloud plugin downloaded successfully. plugin_key=" << plugin;
        return Done{};
    }
    BOOST_LOG_TRIVIAL(error) << "Cloud plugin download failed. plugin_key=" << plugin << " error=" << error;
    PluginManager::instance().set_plugin_error(plugin, error);
    return Refusal{RefusalKind::Failed, "download_failed", error.empty() ? std::string(L("Plugin download failed.")) : error};
}

Outcome<Done> wait_off_ui_thread(const std::function<Outcome<Done>()>& work)
{
    assert(wxThread::IsMain());
    struct State
    {
        std::mutex                   mutex;
        std::optional<Outcome<Done>> result;
    };
    auto       state    = std::make_shared<State>();
    bool       finished = false;
    wxEventLoop loop;
    // The completion runs on the UI thread, which is in the loop below until it has run, so the loop outlives it.
    std::function<void()> on_finish = [&finished, &loop] {
        finished = true;
        if (loop.IsRunning())
            loop.Exit();
    };
    std::thread([state, work, on_finish] {
        Outcome<Done> result = Refusal{RefusalKind::Failed, "failed", L("Unknown error")};
        try {
            result = work();
        } catch (const std::exception& ex) {
            result = Refusal{RefusalKind::Failed, "failed", ex.what()};
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result.emplace(std::move(result));
        }
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter(on_finish);
    }).detach();
    if (!finished)
        loop.Run();
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->result ? *state->result : Outcome<Done>(Refusal{RefusalKind::Failed, "failed", L("Unknown error")});
}

// ---- Delete and Unsubscribe ----

Outcome<PluginUninstall> plugin_uninstall_plan(const std::string& plugin)
{
    const Outcome<PluginDescriptor> found = find_plugin(plugin);
    if (!found)
        return found.refusal();
    const PluginDescriptor& descriptor = found.value();
    const PluginActions     actions =
        plugin_actions(descriptor, PluginManager::instance().is_plugin_load_in_progress(descriptor.plugin_key));
    if (!actions.unsubscribe && !actions.remove)
        return Refusal{RefusalKind::Precondition, "nothing_to_uninstall",
                       // TRN %1% is a plugin's name.
                       L("\"%1%\" is not installed, and no subscription keeps it listed."), {descriptor.name}};
    return PluginUninstall{descriptor.plugin_key, descriptor.name, actions.unsubscribe, descriptor};
}

Outcome<Done> uninstall_plugin(const PluginUninstall& plan)
{
    PluginManager& manager = PluginManager::instance();
    std::string    error;
    bool           succeeded = false;
    if (plan.unsubscribe) {
        succeeded = manager.delete_and_unsubscribe_cloud_plugin(plan.plugin, error);
    } else {
        succeeded = manager.delete_plugin(plan.plugin, error);
        // Keep the cloud row listed so the plugin stays installable without waiting on a cloud fetch.
        if (succeeded && plan.descriptor.is_cloud_plugin())
            manager.update_cloud_metadata(std::vector<PluginDescriptor>{as_cloud_only_descriptor(plan.descriptor)});
    }
    if (succeeded)
        return Done{};
    if (!error.empty())
        return Refusal{RefusalKind::Failed, "not_uninstalled", error};
    return Refusal{RefusalKind::Failed, "not_uninstalled",
                   plan.unsubscribe ? L("Failed to unsubscribe plugin.") : L("Failed to delete plugin.")};
}

void plugin_uninstalled()
{
    // The package is gone only now; the unload callback rechecked the notices too early.
    if (Plater* plater = wxGetApp().plater())
        plater->refresh_missing_plugin_notifications();
}

// ---- Reload, Delete cache and reload, and Reinstall ----

Outcome<PluginReload> plugin_reload_plan(const std::string& plugin, bool clear_cache)
{
    const Outcome<PluginDescriptor> found = find_plugin(plugin);
    if (!found)
        return found.refusal();
    const PluginDescriptor& descriptor = found.value();
    PluginManager&          manager    = PluginManager::instance();
    const PluginActions     actions    = plugin_actions(descriptor, manager.is_plugin_load_in_progress(descriptor.plugin_key));
    PluginReload            plan{descriptor.plugin_key, descriptor.name, clear_cache, manager.is_plugin_loaded(descriptor.plugin_key),
                      actions.reinstall, descriptor};
    if (actions.reinstall) {
        if (clear_cache)
            return Refusal{RefusalKind::Precondition, "no_cache_to_clear",
                           // TRN %1% is a plugin's name.
                           L("\"%1%\" is a cloud plugin, which is installed again with its cache; leave clear_cache out."),
                           {descriptor.name}};
        return plan;
    }
    if (!actions.reload)
        return Refusal{RefusalKind::Precondition, "no_reload",
                       // TRN %1% is a plugin's name.
                       L("\"%1%\" is no longer available from the cloud, so it cannot be installed or loaded again."), {descriptor.name}};
    return plan;
}

Outcome<Done> remove_plugin_package_for_reinstall(const PluginDescriptor& plugin)
{
    PluginManager& manager = PluginManager::instance();
    std::string    error;
    if (!manager.delete_plugin(plugin.plugin_key, error))
        return Refusal{RefusalKind::Failed, "not_uninstalled", error.empty() ? std::string(L("Failed to delete plugin.")) : error};
    manager.update_cloud_metadata(std::vector<PluginDescriptor>{as_cloud_only_descriptor(plugin)});
    return Done{};
}

Outcome<Done> load_plugin_and_wait(const std::string& plugin)
{
    PluginManager& manager = PluginManager::instance();
    manager.load_plugin(plugin);
    std::string error;
    if (!manager.wait_for_plugin_load(plugin, std::chrono::minutes(5), error) || !manager.is_plugin_loaded(plugin))
        return Refusal{RefusalKind::Failed, "not_loaded", error.empty() ? std::string(L("Plugin failed to load.")) : error};
    return Done{};
}

Outcome<Done> reload_plugin(const PluginReload& plan)
{
    const std::string& key = plan.plugin;
    if (plan.cloud) {
        if (plan.descriptor.has_local_package())
            if (const Outcome<Done> removed = remove_plugin_package_for_reinstall(plan.descriptor); !removed)
                return removed;
        if (const Outcome<Done> downloaded = download_cloud_plugin(key, plan.descriptor.version); !downloaded)
            return downloaded;
        return plan.was_loaded ? load_plugin_and_wait(key) : Outcome<Done>(Done{});
    }

    PluginManager&          manager = PluginManager::instance();
    boost::filesystem::path plugin_root;
    if (plan.clear_cache) {
        PluginDescriptor descriptor;
        if (!manager.try_get_plugin_descriptor(key, descriptor))
            return Refusal{RefusalKind::Precondition, "no_such_plugin", L("Plugin not found.")};
        std::string resolve_error;
        if (!resolve_allowed_plugin_root(descriptor, {get_orca_plugins_dir()},
                                         L("Refusing to clear a plugin cache outside the local plugin directory."), plugin_root,
                                         resolve_error))
            return Refusal{RefusalKind::Precondition, "outside_plugins_folder", resolve_error};
    }
    if (!manager.unload_plugin(key))
        return Refusal{RefusalKind::Failed, "not_unloaded", L("Failed to unload plugin.")};
    if (plan.clear_cache) {
        // __whl_extracted__ holds both dependency extractions and the uv-installed entry wheel. The stored wheel and
        // plugin data are left untouched.
        std::string cache_error;
        if (!plugin_wheel_install::delete_install_cache(plugin_root, cache_error))
            return Refusal{RefusalKind::Failed, "cache_not_cleared", cache_error};
    }
    if (const Outcome<Done> loaded = load_plugin_and_wait(key); !loaded)
        return loaded;
    if (!plan.was_loaded && !manager.unload_plugin(key))
        return Refusal{RefusalKind::Failed, "not_restored", L("Plugin reloaded, but failed to restore the inactive state.")};
    return Done{};
}

// ---- Refresh Plugins ----

// A run's own lock: the queue's thread writes how the run ended, and the threads that hold its ticket read it, with no
// need of the queue.
struct RefreshRunState
{
    std::mutex         mutex;
    RefreshRun::Status status;
};

namespace {

void set_status(RefreshRunState& run, RefreshRun::Status status)
{
    std::lock_guard<std::mutex> lock(run.mutex);
    run.status = std::move(status);
}

} // namespace

RefreshRun::Ticket RefreshRun::start(std::function<void()> work)
{
    const std::shared_ptr<Runs> runs = m_runs;
    const auto                  run  = std::make_shared<RefreshRunState>();
    std::thread                 queue;
    {
        std::lock_guard<std::mutex> lock(runs->mutex);
        runs->waiting.emplace_back(run, std::move(work));
        if (runs->running)
            return Ticket(run);
        // The thread starts under the lock, which it takes first in run_queue(), so no other call queues a work before it
        // is known whether a thread runs the queue.
        try {
            queue         = std::thread([runs] { run_queue(runs); });
            runs->running = true;
        } catch (const std::exception& ex) {
            // No thread runs the queue, so it holds only this work.
            runs->waiting.clear();
            set_status(*run, Status{State::Failed, ex.what()});
            return Ticket(run);
        }
    }
    try {
        queue.detach();
    } catch (const std::system_error&) {
        // The thread goes on running the queue. A joinable thread can be neither destroyed nor, without holding the caller
        // until every queued refresh has ended, waited for, so its handle is kept for the process's life.
        new std::thread(std::move(queue));
    }
    return Ticket(run);
}

void RefreshRun::run_queue(const std::shared_ptr<Runs>& runs)
{
    for (;;) {
        std::pair<std::shared_ptr<RefreshRunState>, std::function<void()>> next;
        {
            std::lock_guard<std::mutex> lock(runs->mutex);
            if (runs->waiting.empty()) {
                runs->running = false;
                return;
            }
            next = std::move(runs->waiting.front());
            runs->waiting.pop_front();
        }
        Status status{State::Done, {}};
        try {
            next.second();
        } catch (const std::exception& ex) {
            status = Status{State::Failed, ex.what()};
        } catch (...) {
            status = Status{State::Failed, "Unknown error"};
        }
        set_status(*next.first, std::move(status));
    }
}

RefreshRun::Status RefreshRun::status(Ticket ticket) const
{
    if (ticket.m_run == nullptr)
        return Status{State::Failed, "This ticket is of no refresh."};
    std::lock_guard<std::mutex> lock(ticket.m_run->mutex);
    return ticket.m_run->status;
}

std::optional<RefreshRun::Status> wait_for_refresh(const RefreshRun& run, RefreshRun::Ticket ticket, const std::function<bool()>& cancelled,
                                                   std::chrono::milliseconds poll)
{
    for (RefreshRun::Status status = run.status(ticket);; status = run.status(ticket)) {
        if (status.state != RefreshRun::State::Pending)
            return status;
        if (cancelled && cancelled())
            return std::nullopt;
        std::this_thread::sleep_for(poll);
    }
}

RefreshRun& plugin_refresh_run()
{
    static RefreshRun run;
    return run;
}

RefreshRun::Ticket start_plugin_refresh(std::function<void(const std::string& error, bool unknown)> finished)
{
    assert(wxThread::IsMain());
    return plugin_refresh_run().start([finished = std::move(finished)] {
        try {
            // A run that waited for others must not begin on an app that is closing, whose plugins are being unloaded.
            if (wxTheApp == nullptr || wxGetApp().is_closing())
                throw std::runtime_error("OrcaSlicer is closing.");
            refresh_plugin_metadata_blocking(/*fetch_cloud=*/true);
        } catch (const std::exception& ex) {
            if (finished)
                finished(ex.what(), false);
            throw;
        } catch (...) {
            if (finished)
                finished({}, true);
            throw;
        }
        if (finished)
            finished({}, false);
    });
}

}}} // namespace Slic3r::GUI::Ops
