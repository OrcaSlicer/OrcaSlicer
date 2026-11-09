#pragma once

#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "McpJournal.hpp"
#include "McpOptions.hpp"
#include "McpProtocol.hpp"
#include "McpReplay.hpp"
#include "McpPersonRun.hpp"
#include "McpServicePtr.hpp"
#include "McpTypes.hpp"
#include "UiInvoker.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class wxTimer;

namespace Slic3r {
class PythonSession;
}

namespace Slic3r { namespace GUI { namespace Mcp {

class McpHttpServer;
class McpProtocol;
class McpRecorder;
class ModalTracker;
class UiInspector;
class WebPresses;
struct AutoAnswerRule;
struct RequestGate;

// Whose an auto_answer_dialogs call's rules are ("run", "plugin" or "session"), the tracker layer they went into (0 for
// none), and whether they start paused: a Run Script session's rules registered between its works answer nothing, an open
// dialog included, until its next work.
struct AutoAnswerSet
{
    std::string scope;
    int         layer  = 0;
    bool        paused = false;
};

// The MCP server inside the running app: owns the tool catalog, the UI invoker, jobs, the session
// journal, the modal-dialog tracker and the HTTP endpoint, and runs a --mcp-run once the app is
// ready. Created by GUI_App; started, suspended and stopped on the UI thread.
//
// Off unless the preference or --mcp/--mcp-run asks for it; when off, none of this exists.
// Held in an McpServicePtr, whose deleter is the only way to destroy one.
class McpService
{
public:
    McpService();
    McpService(const McpService&)            = delete;
    McpService& operator=(const McpService&) = delete;

    // The tool catalog this build serves, held by a service that is never started. Building it needs
    // neither wx nor a window: registering a tool only describes it, and its handler runs only when
    // the tool is called. The stdio bridge lists the tools from it while no OrcaSlicer is running.
    // Built on first use and kept until the process exits.
    static const ToolRegistry& offline_catalog();
    // What initialize and server/discover report: the server's name, version and instructions.
    static ServerInfo server_info();

    // Builds the catalog and, when `serve` is true, opens the HTTP endpoint. A replay or script from
    // `options` runs after mark_ready(). Returns false with `err` set when the endpoint can't start;
    // the catalog stays usable for it either way.
    bool start(const McpLaunchOptions& options, bool serve, std::string& err);
    // Stops everything; idempotent, and final: start() refuses afterwards. Wakes every waiting call
    // and closes the endpoint, but frees nothing: a call may still be on the UI thread's stack inside
    // a nested event loop, and job and request threads may still be returning.
    void stop();
    // After stop(): waits until no request, call or job thread is inside the service. False when one
    // is still running after `timeout`; the service must then be kept alive (leaked) rather than
    // destroyed under it.
    bool drain(std::chrono::milliseconds timeout);
    // Closes the HTTP endpoint and removes the discovery file; the catalog, running jobs and a
    // launch run carry on, and start() with `serve` opens the endpoint again. This is what
    // turning the server off in Preferences does: a running call may have opened that very dialog,
    // so the service itself must outlive it.
    void stop_serving();
    bool started() const { return m_core != nullptr; }
    bool serving() const;
    // The last start() could not open the endpoint because another OrcaSlicer window that shares the
    // configuration folder serves on the port. Cleared when the endpoint opens or stop_serving() runs.
    bool port_held_by_other_window() const { return m_port_held_by_other; }
    uint16_t port() const;
    const std::string& token() const { return m_token; }

    // Main frame recreation (language change): refs become stale and UI calls fail with "retry"
    // until resume().
    void suspend();
    void resume();
    bool suspended() const { return m_suspended; }

    // The main frame finished starting up (end of GUI_App::post_init). Ready follows once the Prepare
    // canvas has loaded OpenGL, at once when it already has.
    void mark_ready();
    bool ready() const { return m_ready && !m_suspended; }

    // Monotonic counter bumped by suspend(); refs carry it to detect a rebuilt UI.
    uint64_t ui_generation() const { return m_generation; }

    ToolRegistry& registry() { return m_registry; }
    JobRegistry&  jobs() { return m_jobs; }
    Journal&      journal() { return m_journal; }
    UiInvoker&    ui() { return *m_invoker; }
    McpCore&      core() { return *m_core; }
    ModalTracker& modals() { return *m_modals; }
    // Refs from list_widgets live here, so they survive between calls. UI thread only.
    UiInspector&  inspector() { return *m_inspector; }
    // The presses mouse_gesture downs left on pages, the web counterpart of the inspector's held press.
    // Any thread.
    WebPresses&   web_presses() { return *m_web_presses; }
    // start_recording and its kin; also told about every call, for the captions.
    McpRecorder&  recorder() { return *m_recorder; }

    // Auto-answer rules from auto_answer_dialogs. Rules registered from inside a run (the nearest
    // running replay or script in `ancestry`, the caller's job chain, nearest first) last until that run
    // ends; a client's rules replace the ones it registered before, and end when the endpoint
    // closes: with no endpoint open (the server turned off meanwhile), a client's rules are refused.
    // A run scope that is paused (set_run_scope_paused()) gets them paused. Any thread.
    AutoAnswerSet set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry);
    // run_replay and run_script bracket their runs with these. A mouse press the run left held is released
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
    // Cancels the operations whose lineage holds `job`, as cancel_job does: a background job through the worker,
    // a slice or an export through the plater. UI thread.
    void cancel_operations_of(const std::string& job);
    // The job of the nearest running replay or script in `ancestry`, else the scope of the plugin whose call it is,
    // or "" for a client's own call. Any thread.
    std::string run_owner(const std::vector<std::string>& ancestry) const;
    // Keeps the service from being destroyed while a caller outside the core uses it: a plugin's call holds
    // a lease from the moment the UI thread hands it the service until the call returns, and drain() waits
    // for every lease. False once the service is stopping. Any thread.
    bool acquire_lease();
    void release_lease();
    // A plugin's calls own what they leave behind (auto-answer rules, a recording, a held press) as a
    // run's do, until the plugin unloads: begin is called before each of its calls (idempotent), end when
    // it unloads. Any thread.
    // begin returns false for a plugin whose scope ended (it unloaded) until plugin_loaded() is called for
    // it again; end also stops the plugin's running calls, and a call of its that would leave something
    // behind after that is refused (run_owner, set_auto_answer throw).
    bool begin_plugin_scope(const std::string& plugin_key);
    void end_plugin_scope(const std::string& plugin_key);
    void plugin_loaded(const std::string& plugin_key);
    // Whether clients can reach the server: false from stop_serving() until the endpoint opens again.
    // A client's call still running then must not leave state behind that the closing ended. Any thread.
    bool endpoint_open() const;

    // {serving, ready, version, port, url, token_file, token_source, discovery_file,
    // discovery_error} for Preferences and get_app_state.
    nlohmann::json describe() const;
    // Where the token in use comes from: "file", "ORCA_MCP_TOKEN" or "--mcp-token".
    const std::string& token_source() const { return m_token_source; }

    // Exit code of a --mcp-run run (0 passed, 1 failed, 2 invalid), -1 when none ran. Read by
    // GUI_Run after the event loop ends; wx ignores OnExit()'s return value.
    static int run_exit_code();

    // Runs a replay for the Action Replay window: an unjournaled mutate job, as run_replay runs one,
    // whose steps report through `events`. A thread the service owns follows the job to its end, past a
    // running or blocked_by_dialog reply, and posts `finished`; stop() joins it. `only_step` runs one
    // step (no result.json). `discard_authorized`: the user chose not to save before a run that
    // replaces the project, so the replacing tools may drop unsaved changes while it runs. nullptr when
    // the service is not started or is stopping. UI thread.
    std::shared_ptr<PersonRun> start_replay_run(Replay replay, ReplayContext sctx, std::optional<std::size_t> only_step,
                                                bool discard_authorized, PersonRun::Events events);
    // Hosts the Run Script panel's `session` as an unjournaled person's run_script job (run_python_session_job()) on a
    // thread the service owns, which stop() joins and stop_person_runs() stops. `file` is the session's file; empty
    // for an unsaved script. `events.finished` is posted only for a job refused before its handler ran or a handler
    // that refused (Python scripts off), as {"error": message}; the session's end is PythonSession::ended(), which a
    // refused job sets too. nullptr when the service is not started or is stopping. UI thread.
    std::shared_ptr<PersonRun> start_python_session(std::shared_ptr<PythonSession> session, const boost::filesystem::path& file,
                                                    PersonRun::Events events);
    // True when a run a person started (Action Replay, the Run Script panel, --mcp-run) is in `lineage`: a
    // run_script there runs as the person's script, not an agent's. Any thread.
    bool person_run(const std::set<std::string>& lineage) const;
    // Stops `job` and what it runs: every call it and its descendants make sees itself cancelled
    // (JobRegistry::request_stop). Any thread.
    void request_stop(const std::string& job);
    // True while a run in `lineage` that the user let drop unsaved changes is running. Any thread.
    bool discard_authorized(const std::set<std::string>& lineage) const;
    // Stops every run start_replay_run() started, while the main window is still the one they run in.
    // suspend() and stop() call it first. UI thread.
    void stop_person_runs();
    bool stopping() const { return m_stopping; }
    // Queues `fn` on the UI thread; nothing runs once the app is gone. Any thread.
    void post_ui(std::function<void()> fn);
    // Encodes a recording once its file is closed: encode_recording with `args` as an unjournaled job, which
    // a thread the service owns follows to its end before posting `done` its result on the UI thread. The job
    // is cancelled like any encode (cancel_job {job}, stop()), and `done` is not called once the service stops.
    // False when the service is not started or is stopping. Any thread.
    bool encode_when_closed(nlohmann::json args, std::function<void(const ToolResult&)> done);
    // Starts encode_recording with `args` as an unjournaled job of its own and returns at once: the job's
    // envelope, or the error that refused it. For a recording's encode that stop_recording starts, which a
    // replay of the recording asks for again. Any thread.
    ToolResult start_encode(const nlohmann::json& args);

private:
    friend struct McpServiceDeleter;
    ~McpService();

    void register_tools();
    // `def` with a handler that marks its job as a person's run while it runs (person_run()).
    ToolDef as_person(const ToolDef& def);
    // Calls `def` and follows the job it starts to its end; nullopt when the service stops first. A thread
    // the service owns calls it.
    std::optional<ToolResult> call_to_end(const ToolDef& def, const nlohmann::json& args);
    void set_ready();
    void run_launch_file();
    GuardState guards_on_ui() const;

    McpLaunchOptions m_options;
    std::string      m_token;
    std::string      m_token_source;
    std::string      m_discovery_error;
    bool             m_port_held_by_other = false;

    std::unique_ptr<UiDispatcher>  m_dispatcher;
    ToolRegistry                   m_registry;
    JobRegistry                    m_jobs;
    Journal                        m_journal;
    // Before the invoker, so it outlives the invoker's callbacks, which read it.
    std::unique_ptr<ModalTracker>  m_modals;
    // Before the core, which reports its calls to it.
    std::unique_ptr<McpRecorder>   m_recorder;
    std::unique_ptr<UiInvoker>     m_invoker;
    std::unique_ptr<McpCore>       m_core;
    std::unique_ptr<McpProtocol>   m_protocol;
    std::unique_ptr<McpHttpServer> m_http;
    std::shared_ptr<RequestGate>   m_gate;
    std::unique_ptr<UiInspector>   m_inspector;
    // Shared with the page calls that release its presses, whose replies may come after the service.
    std::shared_ptr<WebPresses>    m_web_presses;

    mutable std::mutex                      m_rules_mutex;
    std::map<std::string, std::vector<int>> m_run_rule_layers; // running replay's or script's job -> layers
    std::set<std::string>                   m_paused_scopes;      // jobs whose layers are paused
    int                                     m_session_rule_layer = 0;
    bool                                    m_endpoint_open      = false;

    std::atomic<bool>     m_ready{false};
    // Polls for the Prepare canvas's OpenGL between mark_ready() and ready. UI thread.
    std::unique_ptr<wxTimer> m_ready_timer;
    std::atomic<bool>     m_suspended{false};
    std::atomic<bool>     m_stopping{false};
    std::atomic<int>      m_leases{0};
    std::atomic<uint64_t> m_generation{1};
    std::thread           m_launch_thread;

    // Runs a person started (Action Replay, the Run Script panel), each with the thread that follows its job.
    struct RunThread
    {
        std::thread                thread;
        std::shared_ptr<PersonRun> run;
    };
    std::mutex             m_runs_mutex;
    std::vector<RunThread> m_run_threads;
    // Threads following encode_when_closed()'s jobs, each with the flag it sets on leaving.
    struct EncodeThread
    {
        std::thread                        thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<EncodeThread> m_encode_threads; // under m_runs_mutex
    mutable std::mutex     m_run_state_mutex;
    std::set<std::string>  m_discard_jobs;
    std::set<std::string>  m_person_jobs;
    std::set<std::string>  m_unloaded_plugins; // under m_rules_mutex
};

// Lets plugins call the tools through orca.host.tools: a call from a Script worker of a plugin holding the
// tools permission runs as McpCore::call_plugin(), starting the service in its catalog-only mode when MCP
// is off, and is logged. Called once by GUI_App as it sets up plugins.
void install_plugin_tool_caller();

}}} // namespace Slic3r::GUI::Mcp
