#pragma once

#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "McpJournal.hpp"
#include "McpOptions.hpp"
#include "McpServicePtr.hpp"
#include "McpTypes.hpp"
#include "UiInvoker.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

class McpHttpServer;
class McpProtocol;
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
    uint16_t port() const;
    const std::string& token() const { return m_token; }

    // Main frame recreation (language change): refs become stale and UI calls fail with "retry"
    // until resume().
    void suspend();
    void resume();
    bool suspended() const { return m_suspended; }

    // The main frame finished starting up (end of GUI_App::post_init).
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

    // Auto-answer rules from dialog_auto_answer. Rules registered from inside a script (the nearest
    // running script in `ancestry`, the caller's job chain, nearest first) last until that script
    // ends; a client's rules replace the ones it registered before, and end when the endpoint
    // closes: with no endpoint open (the server turned off meanwhile), a client's rules are refused.
    // Returns "script" or "session". Any thread.
    std::string set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry);
    // run_script brackets its steps with these.
    void begin_script_scope(const std::string& job);
    void end_script_scope(const std::string& job);

    // Where screenshots and script outputs go when no path is given.
    boost::filesystem::path output_dir() const;

    // {serving, ready, version, port, url, token_file, token_source, discovery_file,
    // discovery_error} for Preferences and get_state.
    nlohmann::json describe() const;
    // Where the token in use comes from: "file", "ORCA_MCP_TOKEN" or "--mcp-token".
    const std::string& token_source() const { return m_token_source; }

    // Exit code of a --mcp-script run (0 passed, 1 failed, 2 invalid), -1 when none ran. Read by
    // GUI_Run after the event loop ends; wx ignores OnExit()'s return value.
    static int script_exit_code();

private:
    friend struct McpServiceDeleter;
    ~McpService();

    void register_tools();
    void run_launch_script();
    GuardState guards_on_ui() const;

    McpLaunchOptions m_options;
    std::string      m_token;
    std::string      m_token_source;
    std::string      m_discovery_error;

    std::unique_ptr<UiDispatcher>  m_dispatcher;
    ToolRegistry                   m_registry;
    JobRegistry                    m_jobs;
    Journal                        m_journal;
    std::unique_ptr<UiInvoker>     m_invoker;
    std::unique_ptr<McpCore>       m_core;
    std::unique_ptr<McpProtocol>   m_protocol;
    std::unique_ptr<McpHttpServer> m_http;
    std::shared_ptr<RequestGate>   m_gate;
    std::unique_ptr<ModalTracker>  m_modals;
    std::unique_ptr<UiInspector>   m_inspector;

    std::mutex                              m_rules_mutex;
    std::map<std::string, std::vector<int>> m_script_rule_layers; // running script job -> layers
    int                                     m_session_rule_layer = 0;
    bool                                    m_endpoint_open      = false;

    std::atomic<bool>     m_ready{false};
    std::atomic<bool>     m_suspended{false};
    std::atomic<bool>     m_stopping{false};
    std::atomic<uint64_t> m_generation{1};
    std::thread           m_script_thread;
};

}}} // namespace Slic3r::GUI::Mcp
