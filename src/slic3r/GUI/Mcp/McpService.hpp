#pragma once

#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "McpJournal.hpp"
#include "McpOptions.hpp"
#include "McpScript.hpp"
#include "McpScriptRun.hpp"
#include "McpServicePtr.hpp"
#include "McpTypes.hpp"
#include "UiInvoker.hpp"

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

namespace Slic3r { namespace GUI { namespace Mcp {

class McpHttpServer;
class McpProtocol;
class McpRecorder;
class ModalTracker;
class UiInspector;
struct AutoAnswerRule;
struct RequestGate;

// The MCP server inside the running app: owns the tool catalog, the UI invoker, jobs, the session
// journal, the modal-dialog tracker and the HTTP endpoint, and runs a --mcp-script once the app is
// ready. Created by GUI_App; started, suspended and stopped on the UI thread.
//
// Off unless the preference or --mcp/--mcp-script asks for it; when off, none of this exists.
// Held in an McpServicePtr, whose deleter is the only way to destroy one.
class McpService
{
public:
    McpService();
    McpService(const McpService&)            = delete;
    McpService& operator=(const McpService&) = delete;

    // Builds the catalog and, when `serve` is true, opens the HTTP endpoint. A script from
    // `options` runs after mark_ready(). Returns false with `err` set when the endpoint can't start;
    // the catalog stays usable for the script either way.
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
    // launch script carry on, and start() with `serve` opens the endpoint again. This is what
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
    // Refs from ui_snapshot live here, so they survive between calls. UI thread only.
    UiInspector&  inspector() { return *m_inspector; }
    // start_recording and its kin; also told about every call, for the captions.
    McpRecorder&  recorder() { return *m_recorder; }

    // Auto-answer rules from dialog_auto_answer. Rules registered from inside a script (the nearest
    // running script in `ancestry`, the caller's job chain, nearest first) last until that script
    // ends; a client's rules replace the ones it registered before, and end when the endpoint
    // closes: with no endpoint open (the server turned off meanwhile), a client's rules are refused.
    // Returns "script" or "session". Any thread.
    std::string set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry);
    // run_script brackets its steps with these. A mouse press the script left held is released
    // when it ends, and so is a recording it started; `wait_recording` waits for that recording's file,
    // unless the run already did.
    void begin_script_scope(const std::string& job);
    void end_script_scope(const std::string& job, bool wait_recording = true);
    // The job of the nearest running script in `ancestry`, or "" for a client's own call. Any thread.
    std::string script_owner(const std::vector<std::string>& ancestry) const;
    // Whether clients can reach the server: false from stop_serving() until the endpoint opens again.
    // A client's call still running then must not leave state behind that the closing ended. Any thread.
    bool endpoint_open() const;

    // {serving, ready, version, port, url, token_file, token_source, discovery_file,
    // discovery_error} for Preferences and get_state.
    nlohmann::json describe() const;
    // Where the token in use comes from: "file", "ORCA_MCP_TOKEN" or "--mcp-token".
    const std::string& token_source() const { return m_token_source; }

    // Exit code of a --mcp-script run (0 passed, 1 failed, 2 invalid), -1 when none ran. Read by
    // GUI_Run after the event loop ends; wx ignores OnExit()'s return value.
    static int script_exit_code();

    // Runs a script for the script runner dialog: an unjournaled mutate job, as run_script runs one,
    // whose steps report through `events`. A thread the service owns follows the job to its end, past a
    // running or blocked_by_dialog reply, and posts `finished`; stop() joins it. `only_step` runs one
    // step (no result.json). `discard_authorized`: the user chose not to save before a run that
    // replaces the project, so the replacing tools may drop unsaved changes while it runs. nullptr when
    // the service is not started or is stopping. UI thread.
    std::shared_ptr<ScriptRun> start_script_run(Script script, ScriptContext sctx, std::optional<std::size_t> only_step,
                                                bool discard_authorized, ScriptRun::Events events);
    // Stops `job` and what it runs: every call it and its descendants make sees itself cancelled
    // (JobRegistry::request_stop). Any thread.
    void request_stop(const std::string& job);
    // True while a run in `lineage` that the user let drop unsaved changes is running. Any thread.
    bool discard_authorized(const std::set<std::string>& lineage) const;
    // Stops every run start_script_run() started, while the main window is still the one they run in.
    // suspend() and stop() call it first. UI thread.
    void stop_script_runs();
    bool stopping() const { return m_stopping; }
    // Queues `fn` on the UI thread; nothing runs once the app is gone. Any thread.
    void post_ui(std::function<void()> fn);
    // Encodes a recording once its file is closed: encode_recording with `args` as an unjournaled job, which
    // a thread the service owns follows to its end before posting `done` its result on the UI thread. The job
    // is cancelled like any encode (cancel {job}, stop()), and `done` is not called once the service stops.
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
    // Calls `def` and follows the job it starts to its end; nullopt when the service stops first. A thread
    // the service owns calls it.
    std::optional<ToolResult> call_to_end(const ToolDef& def, const nlohmann::json& args);
    void set_ready();
    void run_launch_script();
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

    mutable std::mutex                      m_rules_mutex;
    std::map<std::string, std::vector<int>> m_script_rule_layers; // running script job -> layers
    int                                     m_session_rule_layer = 0;
    bool                                    m_endpoint_open      = false;

    std::atomic<bool>     m_ready{false};
    // Polls for the Prepare canvas's OpenGL between mark_ready() and ready. UI thread.
    std::unique_ptr<wxTimer> m_ready_timer;
    std::atomic<bool>     m_suspended{false};
    std::atomic<bool>     m_stopping{false};
    std::atomic<uint64_t> m_generation{1};
    std::thread           m_script_thread;

    // Runs of the script runner dialog, each with the thread that follows its job.
    struct RunThread
    {
        std::thread                thread;
        std::shared_ptr<ScriptRun> run;
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
};

}}} // namespace Slic3r::GUI::Mcp
