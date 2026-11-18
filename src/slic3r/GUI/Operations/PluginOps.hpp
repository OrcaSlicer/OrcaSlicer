#pragma once

#include "Outcome.hpp"

#include "slic3r/GUI/PluginSource.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The plugins, as the Plugins window lists them and as its switches and row menus change them: the list, a plugin's or a
// capability's switch, Delete and Unsubscribe, Reload and Reinstall, and Refresh Plugins.
//
// What the window does behind a progress dialog runs off the UI thread. Those steps are functions of their own here,
// which the window runs behind its progress dialog and a command on its own thread, with no dialog: plan on the UI
// thread, do the work, finish on the UI thread.
namespace Slic3r { namespace GUI { namespace Ops {

// ---- The list ----

struct PluginCapabilityRow
{
    std::string name;
    std::string type; // plugin_capability_type_to_string(): "script", "importer", ...
    bool        enabled = false;
};

// One row of the Plugins window, as data.
struct PluginRow
{
    std::string key;     // the plugin manager's key, which every plugin command takes
    std::string name;
    std::string version; // the version installed, else the latest one offered: what the row shows
    bool        installed = false; // a package is on disk
    bool        enabled   = false; // loaded: the row's switch is on
    std::string status;            // Activated, Error, RuntimeError, Inactive or Loading, as the row shows it
    std::string source;            // local, subscribed, mine or orphaned
    // A loaded plugin's capabilities, in the order it registered them; a plugin that is not loaded has none.
    std::vector<PluginCapabilityRow> capabilities;
    std::string                      error; // the blocking error the row shows, or ""
};

// What the plugin manager holds of a plugin besides its descriptor.
struct PluginRuntime
{
    bool                             loaded  = false;
    bool                             loading = false;
    std::vector<PluginCapabilityRow> capabilities;
};

// Where a row says a plugin comes from: the person's own upload, a subscription, one no longer available, or a local
// package.
PluginSource plugin_source(const PluginDescriptor& plugin);
// The version of the package on disk: `installed_version`, which a merge of the cloud's list leaves as it is, else
// `version`; "" where no package is installed.
std::string installed_plugin_version(const PluginDescriptor& plugin);

// What a row offers: its switch, and the items of its menu that act on the plugin.
struct PluginActions
{
    bool can_toggle                   = false; // the switch can be used
    bool toggle_installs_cloud_plugin = false; // switching it on downloads and installs the cloud plugin first
    bool unsubscribe                  = false; // Unsubscribe: a cloud plugin subscribed to, not the person's own
    bool remove                       = false; // Delete: where a package is installed and no Unsubscribe is offered
    bool open_folder                  = false; // Show in folder: where a package is installed
    bool reinstall                    = false; // Reinstall: a cloud plugin still available
    bool reload                       = false; // Reload and Delete cache and reload: a local plugin
};
// What the row of `plugin` offers, `loading` while its load runs.
PluginActions plugin_actions(const PluginDescriptor& plugin, bool loading);

// The rows of `plugins`, in their order, with what `runtime` holds for each key (nothing: not loaded).
std::vector<PluginRow> plugin_rows_from(const std::vector<PluginDescriptor>& plugins, const std::map<std::string, PluginRuntime>& runtime);
// The rows of every plugin the plugin manager knows, those with an invalid package among them, as the window lists them.
std::vector<PluginRow> plugin_rows();

// The descriptor of the plugin of key `plugin`, as the window reads a row's: a valid package, or an invalid one that is
// installed. False where there is none.
bool plugin_descriptor(const std::string& plugin, PluginDescriptor& descriptor);
// The plugin a call names: by its key, else by its name where one plugin has it.
// Refuses: no_such_plugin (with the closest keys).
Outcome<PluginDescriptor> find_plugin(const std::string& plugin);

// ---- Turning a plugin or a capability on or off: the row's switch and a capability's ----

enum class PluginChange
{
    None,        // it was so already
    Unloaded,
    LoadStarted, // the load goes on in the background (PluginManager::load_plugin())
    CapabilityEnabled,
    CapabilityDisabled,
};

// Runs `work` off the UI thread and returns what it returned, keeping the UI going meanwhile.
using RunOffUiThread = std::function<Outcome<Done>(const std::function<Outcome<Done>()>& work)>;

struct SetPluginEnabledArgs
{
    std::string          plugin;          // key
    std::string          capability;      // a capability's name; "": the plugin itself
    PluginCapabilityType capability_type = PluginCapabilityType::Unknown; // Unknown: the capability of that name, of any type
    bool                 enabled         = true;
    // How the download of a cloud plugin that is not installed, which switching it on starts, is waited for: the
    // window's behind its progress dialog. None: in a nested event loop, with no dialog (wait_off_ui_thread()).
    RunOffUiThread run_download;
};
struct PluginEnabled
{
    std::string  plugin;     // key
    std::string  capability; // the capability's name, or ""
    PluginChange change = PluginChange::None;
};
// Does what the row's switch, or a capability's switch, does (PluginsDialog::toggle_plugin(),
// toggle_plugin_capability()). Switching on a plugin starts its load, after downloading and installing a cloud plugin
// that is not installed. A plugin switched off loses its error, as one a person switched off has none. UI thread.
// Refuses: no_such_plugin; for the plugin: not_unloaded, plugin_loading (Busy), unauthorized_plugin (a cloud plugin
//          not authorized for install and not installed), not_installable (a cloud plugin no longer available, not
//          installed), download_failed (its error recorded on the plugin, as the window records it), plugin_error (the
//          package downloaded has an error), manifest_not_found; for a capability: plugin_not_enabled (a plugin not
//          loaded shows no capabilities), no_such_capability (with the plugin's), ambiguous_capability (a name two of
//          its capabilities have, with their types).
Outcome<PluginEnabled> set_plugin_enabled(const SetPluginEnabledArgs& args);

// Downloads and installs the cloud plugin's package of `version`; on failure records the error on the plugin, as the
// window does. Blocking: off the UI thread.
// Refuses: download_failed, with the plugin manager's error, which may be "".
Outcome<Done> download_cloud_plugin(const std::string& plugin, const std::string& version);
// Runs `work` on a thread of its own and waits in a nested event loop until it returns, with no dialog. UI thread.
Outcome<Done> wait_off_ui_thread(const std::function<Outcome<Done>()>& work);

// ---- Delete and Unsubscribe: the row menu's ----

struct PluginUninstall
{
    std::string      plugin; // key
    std::string      name;
    bool             unsubscribe = false; // Unsubscribe, which also ends the subscription in the cloud account; else Delete
    PluginDescriptor descriptor;          // as it was planned
};
// What the row's menu offers to remove the plugin: Unsubscribe for a cloud plugin the person subscribed to, else
// Delete where a package is installed (a local plugin, or the person's own cloud one, whose package alone goes). UI
// thread.
// Refuses: no_such_plugin, nothing_to_uninstall (no package installed and no subscription: the menu offers neither).
Outcome<PluginUninstall> plugin_uninstall_plan(const std::string& plugin);
// The work of the row's Delete or Unsubscribe: the plugin is unloaded where it is loaded, its package removed, and an
// Unsubscribe ends the subscription; a cloud plugin deleted stays listed, to be installed again. Blocking: off the UI
// thread.
// Refuses: not_uninstalled, with the plugin manager's error, which may be "".
Outcome<Done> uninstall_plugin(const PluginUninstall& plan);
// What follows an uninstall on the UI thread: the notices of plugins a project needs are checked again, now that the
// package is gone.
void plugin_uninstalled();

// ---- Reload, Delete cache and reload, and Reinstall: the row menu's ----

struct PluginReload
{
    std::string      plugin; // key
    std::string      name;
    bool             clear_cache = false;
    bool             was_loaded  = false; // a plugin that was not loaded is left so
    bool             cloud       = false; // Reinstall: the package is downloaded again
    PluginDescriptor descriptor;          // as it was planned
};
// What the row's menu offers to load the plugin again: Reload, or with `clear_cache` Delete cache and reload, for a
// local plugin; Reinstall for a cloud plugin still available. UI thread.
// Refuses: no_such_plugin, no_reload (a cloud plugin no longer available: its menu offers neither), no_cache_to_clear
//          (clear_cache for a cloud plugin, whose menu offers no Delete cache and reload).
Outcome<PluginReload> plugin_reload_plan(const std::string& plugin, bool clear_cache);
// Loads a local plugin again: unloaded, its extracted wheel cache removed with `clear_cache`, loaded, and unloaded
// again where it was not loaded. A cloud plugin is reinstalled: remove_plugin_package_for_reinstall(),
// download_cloud_plugin(), and load_plugin_and_wait() where it was loaded. Blocking: off the UI thread.
// Refuses: no_such_plugin, outside_plugins_folder, not_unloaded, cache_not_cleared, not_loaded (with the load's error),
//          not_restored (reloaded, and left loaded), and what the reinstall's steps refuse.
Outcome<Done> reload_plugin(const PluginReload& plan);
// Removes the installed package of a cloud plugin and keeps it listed, as Reinstall does first. Any thread.
// Refuses: not_uninstalled.
Outcome<Done> remove_plugin_package_for_reinstall(const PluginDescriptor& plugin);
// Loads the plugin and waits up to five minutes for its load to end. Blocking: off the UI thread.
// Refuses: not_loaded, with the load's error.
Outcome<Done> load_plugin_and_wait(const std::string& plugin);

// ---- Refresh Plugins ----

// The state of one run of RefreshRun, which its ticket and the queue share (PluginOps.cpp).
struct RefreshRunState;

// One run of RefreshRun, as start() gives it. It shares its run's state with the queue, so it reads how its own run
// ended for as long as it, or a copy, is kept. A default ticket is of no run.
class RefreshTicket
{
public:
    RefreshTicket() = default;
    friend bool operator==(const RefreshTicket& a, const RefreshTicket& b) { return a.m_run == b.m_run; }
    friend bool operator!=(const RefreshTicket& a, const RefreshTicket& b) { return a.m_run != b.m_run; }

private:
    friend class RefreshRun;
    explicit RefreshTicket(std::shared_ptr<RefreshRunState> run) : m_run(std::move(run)) {}
    std::shared_ptr<RefreshRunState> m_run;
};

// Refreshes that take turns: each is a run of its own, off the caller's thread, one at a time, in the order they were
// asked for. Free of wx, so it is unit-tested.
class RefreshRun
{
public:
    using Ticket = RefreshTicket;
    enum class State { Pending, Done, Failed };
    struct Status
    {
        State       state = State::Pending;
        std::string error; // what the work threw, for Failed
    };

    // Queues `work` and returns its ticket. A work asked for while another waits or runs begins once those before it
    // have returned; one that throws does not stop the ones after it. The works run on a detached thread that holds
    // what it needs, so they run, in turn, also after this object is gone.
    Ticket start(std::function<void()> work);
    // How the run of `ticket` stands: Pending while it waits or runs. A ticket start() did not give is Failed, with a
    // text that says so.
    Status status(Ticket ticket) const;

private:
    struct Runs
    {
        std::mutex mutex;
        bool       running = false; // a thread runs the queue
        std::deque<std::pair<std::shared_ptr<RefreshRunState>, std::function<void()>>> waiting;
    };
    // Runs the queue's works, one after another, until it is empty.
    static void run_queue(const std::shared_ptr<Runs>& runs);
    std::shared_ptr<Runs> m_runs = std::make_shared<Runs>();
};

// Starts a run of Refresh Plugins, the installed plugins rescanned and the cloud's list of plugins fetched again
// (refresh_plugin_metadata_blocking()), after the runs already asked for, a person's and a command's alike. A run that
// would begin while OrcaSlicer closes does not scan and fails. `finished` is called on the run's thread when the run
// ends: with "" when it succeeded, else with what failed, `unknown` where that has no text. UI thread.
RefreshRun::Ticket start_plugin_refresh(std::function<void(const std::string& error, bool unknown)> finished);
// The app's one queue of refreshes, which start_plugin_refresh() starts its runs on. Any thread.
RefreshRun& plugin_refresh_run();

// Waits for the run of `ticket` to end and returns how it ended, looking at its status every `poll`. Before each wait
// it asks `cancelled`, and returns nullopt once that says yes: the wait stops and the run goes on. Free of wx, so it is
// unit-tested. Blocking: off the UI thread.
std::optional<RefreshRun::Status> wait_for_refresh(const RefreshRun& run, RefreshRun::Ticket ticket, const std::function<bool()>& cancelled,
                                                   std::chrono::milliseconds poll);

}}} // namespace Slic3r::GUI::Ops
