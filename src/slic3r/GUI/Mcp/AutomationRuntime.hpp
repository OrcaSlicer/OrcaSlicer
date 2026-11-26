#pragma once

#include "AutomationRuntimePtr.hpp"
#include "FollowedThreads.hpp"
#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "McpJournal.hpp"
#include "UiInvoker.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class wxTimer;

namespace Slic3r { namespace GUI { namespace Mcp {

class ActivityLog;
class McpRecorder;
class ModalTracker;
class UiInspector;
class WebPresses;
struct AutoAnswerRule;

// Whose a dialog_auto_answer call's rules are ("run", "plugin" or "session"), the tracker layer they went into (0 for
// none), and whether they start paused: a Run Script session's rules registered between its works answer nothing, an open
// dialog included, until its next work.
struct AutoAnswerSet
{
    std::string scope;
    int         layer  = 0;
    bool        paused = false;
};

// A call the runtime follows to its end on a thread of its own (AutomationRuntime::follow()): a run a person started
// from a window or the command line, or the encode of a person's recording once it ends.
struct FollowedCall
{
    NativeCommand  command;
    nlohmann::json args = nlohmann::json::object();
    // Who started it, for the Agent Activity pane, as McpCore::call() takes it: {source: "action_replay"}, ...
    nlohmann::json origin;
    // The job is a person's run (JobRegistry::person_in).
    bool person = false;
    // Runs first on the following thread and may complete `args`; false gives the call up, and `ended` is not
    // called. One that throws gives the call up as false does. Optional.
    std::function<bool(nlohmann::json& args)> ready;
    // The call's result once its job has ended, past a running or blocked_by_dialog reply. While the runtime stops it
    // is an error ("OrcaSlicer is shutting down") when the call was refused or cut short within its reply budget, and
    // nullopt when the runtime stopped while the call's job was being waited for. When the call itself threw, it is
    // an "Internal error: ..." error. On the following thread.
    std::function<void(const std::optional<ToolResult>&)> ended;
    // Asks the call to end early. stop_followed() calls it while the call has not ended. Optional.
    std::function<void()> stop;
};

// What runs commands for every front end: the MCP endpoint, Action Replay, the Run Script panel, plugins, a person's
// commands from the Speed Dial and the command line's run. Owns the dispatch core, the UI invoker, the jobs, the
// session journal, the modal-dialog tracker, the recorder and the scopes, runs the process's command catalog
// (NativeCommands::instance()), and follows the calls its front ends start to their end (follow()). It knows none of
// its front ends: they hold it and call in. Created by GUI_App when a front end first asks for it; started,
// suspended and stopped on the UI thread.
//
// Until something asks for it, none of this exists. Held in an AutomationRuntimePtr, whose deleter is the only way
// to destroy one.
class AutomationRuntime
{
public:
    AutomationRuntime();
    AutomationRuntime(const AutomationRuntime&)            = delete;
    AutomationRuntime& operator=(const AutomationRuntime&) = delete;

    // Starts the jobs, the core, the UI invoker, the modal tracker and the recorder, once; a later call returns at
    // once. The GUI instruments are left to the front ends that drive the GUI (enable_gui_instruments()).
    bool start(std::string& err);   // false only once it has stopped for exit
    // Turns on what a front end that drives the GUI needs: the UI inspector, menu capture, ImGui item recording
    // and window tracking, and asks the shown 3D view for a frame so its ImGui items are recorded. Idempotent.
    // UI thread.
    //
    // The instruments cost every frame and every popup menu, so they stay on only while something drives the
    // GUI: the client scope is open (the endpoint serves), a replay, a script's run or session, or a plugin has
    // its scope open, or a window holds them (hold_gui_instruments()). They come off when the last of these
    // goes, and a scope that begins while they are off turns them on again. A front end turns them on before
    // what keeps them on begins, as the endpoint does once it listens.
    void enable_gui_instruments();
    // Keeps the GUI instruments on from hold to release, turning them on at the hold: for a window whose runs
    // drive the GUI (Action Replay, the Run Script panel), so that they are on between its runs too and its next
    // run finds the 3D view's ImGui items recorded. One release for each hold. UI thread.
    void hold_gui_instruments();
    void release_gui_instruments();
    // Stops everything; idempotent, and final: start() refuses afterwards. Wakes every waiting call, refuses new
    // ones, closes the client scope and joins the threads of the followed calls, but frees nothing: a call may
    // still be on the UI thread's stack inside a nested event loop, and job threads, and the request threads of a
    // front end, may still be returning.
    void stop();
    // After stop(): waits until no lease is held and no call or job thread is inside the core. False when one
    // is still running after `timeout`; the runtime must then be kept alive (leaked) rather than
    // destroyed under it.
    bool drain(std::chrono::milliseconds timeout);
    // The endpoint opened or closed: a client's rules, recording and held presses end with close, and the GUI
    // instruments come off when nothing else drives the GUI. UI thread.
    void open_client_scope();
    void close_client_scope();
    // Whether a client's call may leave rules, a recording or a held press behind: from open_client_scope()
    // until close_client_scope(). A client's call still running after the close must not leave behind what the
    // close ended. Any thread.
    bool client_scope_open() const;

    // Main frame recreation (language change): refs become stale and UI calls fail with "retry"
    // until resume().
    void suspend();
    void resume();
    bool suspended() const { return m_suspended; }

    // The main frame finished starting up (end of GUI_App::post_init). Ready follows once the Prepare
    // canvas has loaded OpenGL, at once when it already has.
    void mark_ready();
    bool ready() const { return m_ready && !m_suspended; }
    // Runs `fn` on the UI thread once the runtime is ready, at once when it already is. UI thread.
    void on_ready(std::function<void()> fn);

    // Monotonic counter bumped by suspend(); refs carry it to detect a rebuilt UI.
    uint64_t ui_generation() const { return m_generation; }

    JobRegistry&  jobs() { return m_jobs; }
    Journal&      journal() { return m_journal; }
    UiInvoker&    ui() { return *m_invoker; }
    McpCore&      core() { return *m_core; }
    ModalTracker& modals() { return *m_modals; }
    // Refs from widget_list live here. Turns the GUI instruments on if they are off. UI thread only.
    UiInspector&  inspector();
    // The presses mouse_gesture downs left on pages, the web counterpart of the inspector's held press.
    // Any thread.
    WebPresses&   web_presses() { return *m_web_presses; }
    // recording_start and its kin; also told about every call, for the captions.
    McpRecorder&  recorder() { return *m_recorder; }
    // What the Agent Activity pane lists: told about every call, and about the calls the endpoint refuses. Any thread.
    ActivityLog&  activity() { return *m_activity; }
    // Called on the UI thread when the activity log has changed since it was last read; empty for none. UI thread.
    void set_activity_listener(std::function<void()> listener) { m_activity_listener = std::move(listener); }

    // Auto-answer rules from dialog_auto_answer. Rules registered from inside a run (the nearest
    // running replay or script in `ancestry`, the caller's job chain, nearest first) last until that run
    // ends; a client's rules replace the ones it registered before, and end when the client scope
    // closes: with the scope closed (the server turned off meanwhile), a client's rules are refused.
    // A run scope that is paused (set_run_scope_paused()) gets them paused. Any thread.
    AutoAnswerSet set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry);
    // replay_run and script_run bracket their runs with these. A mouse press the run left held is released
    // when it ends, and so is a recording it started; `wait_recording` waits for that recording's file,
    // unless the run already did.
    void begin_run_scope(const std::string& job);
    void end_run_scope(const std::string& job, bool wait_recording = true);
    // Pauses or resumes every rule layer of `job`'s scope (ModalTracker::set_paused); a layer the scope adds while it
    // is paused starts paused. Any thread.
    void set_run_scope_paused(const std::string& job, bool paused);
    // Releases the mouse presses `job`'s calls left held, on widgets and pages, as the end of its scope does. Any
    // thread.
    void release_run_presses(const std::string& job);
    // Cancels the operations whose lineage holds `job`, as job_cancel does: a background job through the worker,
    // a slice or an export through the plater. UI thread.
    void cancel_operations_of(const std::string& job);
    // The job of the nearest running replay or script in `ancestry`, else the scope of the plugin whose call it is,
    // or "" for a client's own call. Throws ToolFailure for a plugin's call whose plugin lost the tools, unloaded, or
    // has no scope open (the call was made before it unloaded), so nothing it leaves behind falls to the client. Any
    // thread.
    std::string run_owner(const std::vector<std::string>& ancestry) const;
    // Keeps the runtime from being destroyed while a caller outside the core uses it: a plugin's call holds
    // a lease from the moment the UI thread hands it the runtime until the call returns, and drain() waits
    // for every lease. False once the runtime is stopping. Any thread.
    bool acquire_lease();
    void release_lease();
    // A plugin's calls own what they leave behind (auto-answer rules, a recording, a held press) as a
    // run's do, until the plugin unloads: begin is called before each of its calls (idempotent), end when
    // it unloads. Any thread.
    // begin returns false for a plugin whose scope ended (it unloaded) until plugin_loaded() is called for
    // it again; end also stops the plugin's running calls and ends the scopes of its running replays and scripts,
    // and a call of its that would leave something behind after that is refused (run_owner, set_auto_answer and
    // require_owner_scope throw).
    // A begin that opens the scope afresh also lets a recording start under it again (McpRecorder::owner_reopened()).
    bool begin_plugin_scope(const std::string& plugin_key);
    void end_plugin_scope(const std::string& plugin_key);
    void plugin_loaded(const std::string& plugin_key);
    // The plugin lost its permission to call the tools (the Plugins dialog's switch or full access turned off): its
    // scopes end as at unload, but its calls are not blocked; the permission refuses them, and a call admitted before
    // is refused where it would leave something behind, until begin_plugin_scope() opens the scope under a new grant.
    // UI thread.
    void plugin_tools_withdrawn(const std::string& plugin_key);
    // Throws ToolFailure unless what `owner` (a run_owner() value) leaves behind may still be published: its scope is
    // open and its plugin has neither lost the tools nor unloaded. A client's "" passes, as its own check covers it.
    // Asked where a press or a recording is published, after the owner was found on the caller's thread. Any thread.
    void require_owner_scope(const std::string& owner) const;

    // Calls a command on a thread the runtime owns, which stop() joins. False, with nothing started, when the
    // runtime is not started or is stopping, or when the system cannot start a thread. Any thread.
    bool follow(FollowedCall call);
    // Stops every followed call that has a stop, while the main window is still the one they run in. UI thread.
    void stop_followed();
    bool stopping() const { return m_stopping; }
    // Queues `fn` on the UI thread; nothing runs once the app is gone. Any thread.
    void post_ui(std::function<void()> fn);

private:
    friend struct AutomationRuntimeDeleter;
    ~AutomationRuntime();

    // Calls `command`, as a person's call with `person`, and follows the job it starts to its end; nullopt when
    // the runtime stops first. A thread follow() starts calls it.
    std::optional<ToolResult> call_to_end(const NativeCommand& command, const nlohmann::json& args, bool person,
                                          const nlohmann::json& origin);
    // Asks the plugin's running calls to stop and ends its scope with the scopes of its running replays and
    // scripts, popping their rules, ending their recording and releasing their presses. For an unload and a
    // withdrawal of the tools alike.
    void end_scopes_of(const std::string& plugin_key);
    // Under m_rules_mutex: throws for a call of `plugin` ("" for none) after it lost the tools, with kToolsNotAllowed,
    // or unloaded, with the message `unloaded`.
    void refuse_gone_plugin(const std::string& plugin, const char* unloaded) const;
    // An auto-answer rule of `layer` answered a dialog: the activity log lists it with the run, plugin or client whose
    // rule it was. UI thread.
    void rule_answered(int layer, const std::string& title, const std::string& message, const std::string& pattern,
                       const std::string& button);
    void set_ready();
    GuardState guards_on_ui() const;
    // Whether something drives the GUI (see enable_gui_instruments()). UI thread.
    bool gui_instruments_wanted() const;
    // Turns the GUI instruments on when something drives the GUI and they are off, and off when nothing does.
    // Does nothing once the runtime stops. UI thread.
    void sync_gui_instruments();
    // Queues sync_gui_instruments() on the UI thread. Any thread.
    void post_gui_instruments_sync();
    // Takes down what enable_gui_instruments() installed. The inspector stays, with its refs, for the calls
    // that hold it. UI thread.
    void disable_gui_instruments();

    std::unique_ptr<UiDispatcher>  m_dispatcher;
    JobRegistry                    m_jobs;
    Journal                        m_journal;
    // Before the invoker, so it outlives the invoker's callbacks, which read it.
    std::unique_ptr<ModalTracker>  m_modals;
    // Before the core, which reports its calls to them.
    std::unique_ptr<McpRecorder>   m_recorder;
    std::unique_ptr<ActivityLog>   m_activity;
    std::function<void()>          m_activity_listener; // UI thread
    std::unique_ptr<UiInvoker>     m_invoker;
    std::unique_ptr<McpCore>       m_core;
    // Null until enable_gui_instruments(). UI thread.
    std::unique_ptr<UiInspector>   m_inspector;
    // Whether the GUI instruments are installed, and how many holds keep them on. UI thread.
    bool                           m_instruments_on   = false;
    int                            m_instrument_holds = 0;
    // Made by start(). Shared with the page calls that release its presses, whose replies may come after the runtime.
    std::shared_ptr<WebPresses>    m_web_presses;

    mutable std::mutex                      m_rules_mutex;
    std::map<std::string, std::vector<int>> m_run_rule_layers; // running replay's or script's job -> layers
    std::set<std::string>                   m_paused_scopes;      // jobs whose layers are paused
    int                                     m_session_rule_layer = 0;
    bool                                    m_client_scope_open  = false;

    std::atomic<bool>     m_ready{false};
    // Polls for the Prepare canvas's OpenGL between mark_ready() and ready. UI thread.
    std::unique_ptr<wxTimer> m_ready_timer;
    std::atomic<bool>     m_suspended{false};
    std::atomic<bool>     m_stopping{false};
    std::atomic<int>      m_leases{0};
    std::atomic<uint64_t> m_generation{1};
    std::vector<std::function<void()>> m_on_ready; // UI thread

    std::set<std::string>  m_unloaded_plugins;  // under m_rules_mutex
    std::set<std::string>  m_withdrawn_plugins; // under m_rules_mutex: lost the tools since their scope last opened
    // Last, so its threads, which use everything above, are gone first.
    FollowedThreads        m_followed;
};

}}} // namespace Slic3r::GUI::Mcp
