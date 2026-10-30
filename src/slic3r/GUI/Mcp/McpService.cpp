#include "McpCore.hpp"
#include "McpOptions.hpp"
#include "McpService.hpp"

#include "McpHttpServer.hpp"
#include "McpProtocol.hpp"
#include "McpRecorder.hpp"
#include "McpServicePtr.hpp"
#include "McpToken.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "UiInspector.hpp"
#include "McpPaths.hpp"
#include "McpScript.hpp"
#include "McpScriptRun.hpp"
#include "JobRegistry.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "UiInvoker.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/app.h>
#include <wx/event.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

// HTTP request threads can outlive the endpoint they came from; they reach the protocol through
// this gate, which drain() closes before the service goes away.
struct RequestGate
{
    std::mutex              mutex;
    std::condition_variable cv;
    McpProtocol*            protocol = nullptr;
    int                     active   = 0;
};

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

std::atomic<int> g_script_exit_code{-1};

std::string shutting_down_reply()
{
    return json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32603}, {"message", "OrcaSlicer is shutting down"}}}}.dump();
}

class WxDispatcher final : public UiDispatcher
{
public:
    void post(std::function<void()> fn) override
    {
        // CallAfter queues a thread-safe event; without an app the closure never runs and its
        // caller gives up after the queue timeout.
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter(std::move(fn));
    }
};

// Whether the Prepare canvas has loaded OpenGL: its functions, shaders and the ImGui font.
bool canvas_gl_ready()
{
    Plater*     plater = wxGetApp().plater();
    GLCanvas3D* canvas = plater != nullptr ? plater->canvas3D() : nullptr;
    return canvas == nullptr || canvas->is_initialized();
}

// Whether the main window has tab pages still to build at idle.
bool pages_pending()
{
    const MainFrame* frame = wxGetApp().mainframe;
    return frame != nullptr && frame->prebuild_pending();
}

// The Plater's UI job worker, as McpCore watches it for jobs that calls start.
WorkerHooks ui_job_worker_hooks()
{
    WorkerHooks hooks;
    hooks.busy = [] {
        Plater* plater = wxGetApp().plater();
        return plater != nullptr && !plater->get_ui_job_worker().is_idle();
    };
    hooks.cancel = [] {
        if (Plater* plater = wxGetApp().plater())
            plater->get_ui_job_worker().cancel();
    };
    return hooks;
}

// Server instructions shown to the model; clients truncate them at 4096 characters.
const char* kInstructions =
    "OrcaSlicer, a 3D-printing slicer, driven through its GUI.\n"
    "Workflow: get_state (is it ready? which presets, plates, dialogs?) -> act -> wait -> screenshot.\n"
    "- Prefer typed tools (open_files, select_preset, set_settings, slice, export) and run_action (every Speed Dial "
    "command: list_actions to search) over GUI clicks.\n"
    "- What a setting does, or which one controls an effect: explain_settings; with no arguments it maps every "
    "setting by page and group.\n"
    "- For anything else use the GUI tools: ui_snapshot lists widgets, menus, 3D toolbar items, gizmos and ImGui "
    "items with refs; ui_click / ui_input / ui_key / ui_mouse act on refs from the latest snapshot.\n"
    "- A call that takes long or opens a dialog returns {status:\"running\"|\"blocked_by_dialog\", job}. It keeps "
    "running: never retry it. Answer dialogs with dialog_respond (see get_state.dialogs), then wait {for:\"job\"}. "
    "A dialog marked native: true (a file or folder picker, a message box) can't be answered; register "
    "dialog_auto_answer before the step that opens one.\n"
    "- While an operation (slice, export, arrange or another background job) runs, other changes are refused; use wait "
    "or cancel.\n"
    "- Paths are absolute. Outputs never overwrite an existing file unless overwrite is true.\n"
    "- screenshot returns a downscaled image plus the full-size PNG path; target \"canvas\" is the 3D view.\n"
    "- start_recording / stop_recording record a video (MJPEG .mkv), with the tool calls as captions and the pointer "
    "highlighted when captions and pointer ask for them; "
    "encode_recording turns it into an H.265 MP4 with the system ffmpeg.\n"
    "- export_script saves this session's calls as a replayable script; run_script replays one.";

} // namespace

void McpServiceDeleter::operator()(McpService* service) const
{
    service->stop();
    if (service->drain(std::chrono::seconds(10)))
        delete service;
    else
        BOOST_LOG_TRIVIAL(error) << "MCP server: a call did not return; leaving the service in place";
}

McpService::McpService() = default;

// Reached only through McpServiceDeleter, after stop() joined the script thread and drain() saw
// every thread leave.
McpService::~McpService() = default;

int McpService::script_exit_code() { return g_script_exit_code.load(); }

bool McpService::serving() const { return m_http != nullptr && m_http->running(); }

uint16_t McpService::port() const { return m_http ? m_http->port() : 0; }

GuardState McpService::guards_on_ui() const
{
    GuardState g;
    g.closing   = m_stopping || wxTheApp == nullptr || wxGetApp().is_closing();
    g.suspended = m_suspended;
    if (m_modals) {
        g.dialogs = m_modals->snapshot();
        g.modal   = !g.dialogs.empty();
    }
    return g;
}

bool McpService::start(const McpLaunchOptions& options, bool serve, std::string& err)
{
    if (m_stopping) {
        err = "MCP server: stopped for exit";
        return false;
    }
    if (m_core == nullptr) {
        m_options    = options;
        m_dispatcher = std::make_unique<WxDispatcher>();
        m_modals     = std::make_unique<ModalTracker>();
        m_inspector  = std::make_unique<UiInspector>(*this);
        m_recorder   = std::make_unique<McpRecorder>(*this);
        m_invoker    = std::make_unique<UiInvoker>(*m_dispatcher, [this] { return guards_on_ui(); }, [this] { return m_generation.load(); },
                                                [this] { return m_modals->snapshot(); },
                                                [this](const std::string& tool, bool running) { m_inspector->ui_activity(tool, running); });
        m_core       = std::make_unique<McpCore>(m_registry, *m_invoker, m_jobs, &m_journal, CoreTiming{}, ui_job_worker_hooks(),
                                                 m_recorder.get());
        register_tools();
        m_inspector->capture_menus(true);
        if (ImGuiWrapper* imgui = wxGetApp().imgui())
            imgui->set_item_recording(true);
    }
    if (!serve || serving())
        return true;

    AppConfig* config = wxGetApp().app_config;
    int        port   = options.port;
    if (port <= 0 && config != nullptr) {
        try {
            port = std::stoi(config->get("mcp_port"));
        } catch (const std::exception&) {
            port = 0;
        }
    }
    if (port <= 0 || port > 65535)
        port = kDefaultPort;

    std::string token;
    if (const char* env = boost::nowide::getenv("ORCA_MCP_TOKEN"); env != nullptr && *env != '\0') {
        token          = env;
        m_token_source = "ORCA_MCP_TOKEN";
    } else if (!options.token.empty()) {
        token          = options.token;
        m_token_source = "--mcp-token";
    } else {
        token          = load_or_create_token(path_from_utf8(data_dir()), err);
        m_token_source = "file";
    }
    if (token.empty()) {
        err = "MCP server: no token (" + err + ")";
        return false;
    }
    m_token = token;

    ServerInfo info;
    info.version      = SLIC3R_VERSION;
    info.instructions = kInstructions;
    // Request threads of an endpoint closed earlier may still be using the protocol object.
    if (!m_protocol) {
        m_protocol = std::make_unique<McpProtocol>(m_registry, *m_core, info);
        m_gate     = std::make_shared<RequestGate>();
        m_gate->protocol = m_protocol.get();
    }
    m_http = std::make_unique<McpHttpServer>(token, [gate = m_gate](const PostRequest& r) -> PostReply {
        McpProtocol* protocol = nullptr;
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            protocol = gate->protocol;
            if (protocol == nullptr)
                return {503, shutting_down_reply()};
            ++gate->active;
        }
        struct Leave
        {
            RequestGate& gate;
            ~Leave()
            {
                {
                    std::lock_guard<std::mutex> lock(gate.mutex);
                    --gate.active;
                }
                gate.cv.notify_all();
            }
        } leave{*gate};
        return protocol->handle_post(r);
    });
    if (!m_http->start(uint16_t(port), err)) {
        err = "MCP server: cannot listen on 127.0.0.1:" + std::to_string(port) + " (" + err + ")";
        m_http.reset();
        // Expected with several windows on one configuration folder, and retried until that window
        // closes, so logged once rather than as an error.
        const bool held_before = m_port_held_by_other;
        m_port_held_by_other   = discovery_held_by_other(path_from_utf8(data_dir()), port);
        if (!m_port_held_by_other)
            BOOST_LOG_TRIVIAL(error) << err;
        else if (!held_before)
            BOOST_LOG_TRIVIAL(info) << "MCP server: another OrcaSlicer window serves on port " << port;
        return false;
    }
    m_port_held_by_other = false;
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_endpoint_open = true;
    }

    m_discovery_error.clear();
    if (!write_discovery(path_from_utf8(data_dir()),
                         {{"url", "http://127.0.0.1:" + std::to_string(m_http->port()) + "/mcp"},
                          {"port", m_http->port()},
                          {"pid", int(getpid())},
                          {"version", SLIC3R_VERSION},
                          {"token", token}},
                         m_discovery_error))
        // Serving anyway: clients configured with the URL and token still connect; the stdio
        // bridge and the harness, which need the file, see the reason in get_state.
        BOOST_LOG_TRIVIAL(warning) << "MCP server: " << m_discovery_error;
    BOOST_LOG_TRIVIAL(info) << "MCP server listening on http://127.0.0.1:" << m_http->port() << "/mcp";
    return true;
}

void McpService::stop()
{
    // Before the stop flag: a run's stop does nothing once the service is stopping, and it must cancel
    // the run's slice while the main window is still there.
    if (m_core != nullptr && !m_stopping)
        stop_script_runs();
    if (m_core == nullptr || m_stopping.exchange(true))
        return;
    BOOST_LOG_TRIVIAL(info) << "MCP server stopping";
    // Its file is finished on the recorder's own threads; this waits a little for them, not for the UI.
    m_recorder->shutdown(std::chrono::seconds(5));
    if (m_ready_timer)
        m_ready_timer->Stop();
    // Wake everything that waits, so running calls return.
    m_invoker->shutdown();
    m_jobs.wake_all();
    m_core->shutdown();
    stop_serving();
    if (m_script_thread.joinable())
        m_script_thread.join();
    {
        std::lock_guard<std::mutex> lock(m_runs_mutex);
        for (RunThread& r : m_run_threads)
            if (r.thread.joinable())
                r.thread.join();
        m_run_threads.clear();
        for (EncodeThread& e : m_encode_threads)
            if (e.thread.joinable())
                e.thread.join();
        m_encode_threads.clear();
    }
    {
        // Script rules too: no script survives the stop.
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        for (const auto& [job, layers] : m_script_rule_layers)
            for (int layer : layers)
                m_modals->pop_rules(layer);
        m_script_rule_layers.clear();
    }
    if (m_inspector) {
        m_inspector->release_held(nullptr);
        m_inspector->capture_menus(false);
    }
    SyntheticInput::clear();
    if (wxTheApp != nullptr)
        if (ImGuiWrapper* imgui = wxGetApp().imgui())
            imgui->set_item_recording(false);
}

bool McpService::drain(std::chrono::milliseconds timeout)
{
    if (m_core == nullptr)
        return true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (m_gate) {
        std::unique_lock<std::mutex> lock(m_gate->mutex);
        m_gate->protocol = nullptr;
        if (!m_gate->cv.wait_until(lock, deadline, [this] { return m_gate->active == 0; }))
            return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return m_core->drain(std::max(left, std::chrono::milliseconds(0)));
}

std::string McpService::set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    // The nearest running script in the caller's job chain owns the rules.
    for (const std::string& job : ancestry)
        if (auto it = m_script_rule_layers.find(job); it != m_script_rule_layers.end()) {
            if (!rules.empty())
                it->second.push_back(m_modals->push_rules(std::move(rules)));
            return "script";
        }
    // A call that was running when the endpoint closed must not bring its client's rules back.
    if (!m_endpoint_open)
        throw ToolFailure("The MCP server was turned off while the rules were being set; they were not applied");
    if (m_session_rule_layer != 0)
        m_modals->pop_rules(m_session_rule_layer);
    m_session_rule_layer = rules.empty() ? 0 : m_modals->push_rules(std::move(rules));
    return "session";
}

void McpService::begin_script_scope(const std::string& job)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    m_script_rule_layers.emplace(job, std::vector<int>{});
}

void McpService::end_script_scope(const std::string& job, bool wait_recording)
{
    // Noted before the scope goes: from then on a call of this script finds no live scope, and must not be
    // taken for a client's.
    if (m_recorder)
        m_recorder->owner_ended(job);
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        auto it = m_script_rule_layers.find(job);
        if (it == m_script_rule_layers.end())
            return;
        if (m_modals)
            for (int layer : it->second)
                m_modals->pop_rules(layer);
        m_script_rule_layers.erase(it);
    }
    // A recording the script started ends with it, its file closed before the script's call returns.
    if (m_recorder)
        m_recorder->end_owned_by(job, "script_ended", wait_recording);
    // Found again on the UI thread: the service may be gone by the time the closure runs.
    if (m_dispatcher && !m_stopping)
        m_dispatcher->post([job] {
            if (McpService* service = wxGetApp().mcp_service(); service != nullptr && service->m_inspector)
                service->m_inspector->release_held(&job);
        });
}

std::string McpService::script_owner(const std::vector<std::string>& ancestry) const
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    for (const std::string& job : ancestry)
        if (m_script_rule_layers.count(job) != 0)
            return job;
    return {};
}

bool McpService::endpoint_open() const
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    return m_endpoint_open;
}

void McpService::stop_serving()
{
    m_port_held_by_other = false;
    // A client's recording ends with its endpoint; a script's carries on until its script ends.
    if (m_recorder)
        m_recorder->end_owned_by({}, "endpoint_closed", false);
    // So does a press a client's ui_mouse left held.
    if (m_inspector) {
        const std::string client;
        m_inspector->release_held(&client);
    }
    {
        // A client's rules end with its endpoint; a person using the app afterwards must not have
        // dialogs answered for them.
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_endpoint_open = false;
        if (m_session_rule_layer != 0)
            m_modals->pop_rules(m_session_rule_layer);
        m_session_rule_layer = 0;
    }
    if (!m_http)
        return;
    // Requests in flight may be waiting on the UI thread, which is running this; drain() waits for
    // them before the service goes away.
    m_http->stop(false);
    m_http.reset();
    remove_discovery(path_from_utf8(data_dir()));
    BOOST_LOG_TRIVIAL(info) << "MCP server endpoint closed";
}

void McpService::suspend()
{
    // The main window is about to be rebuilt; runs stop while its plater is still the current one.
    stop_script_runs();
    if (m_recorder)
        m_recorder->suspend();
    m_suspended = true;
    ++m_generation;
}

void McpService::resume()
{
    ++m_generation;
    m_suspended = false;
}

void McpService::mark_ready()
{
    if (m_ready || m_ready_timer)
        return;
    // Starting on Home, the Prepare canvas loads OpenGL at idle after post_init(), and until then a
    // 3MF save draws its thumbnail with no GL functions loaded. The main window builds its tab pages
    // at idle then too, and a page's web view registers its "wx" handler with a script it waits for in
    // a nested loop; on GTK, calls that came in meanwhile have left that wait unfinished for good,
    // and with it every later web view's window.wx, a dialog page's own buttons included. So ready
    // waits for the canvas and the pages. When the hidden canvas can't get a GL context, the first
    // draw of Prepare loads it instead, and the pages build only while no input comes, so ready stops
    // waiting after 10 s.
    const auto settled = [] { return canvas_gl_ready() && !pages_pending(); };
    if (settled()) {
        set_ready();
        return;
    }
    const auto since = std::chrono::steady_clock::now();
    m_ready_timer    = std::make_unique<wxTimer>();
    m_ready_timer->Bind(wxEVT_TIMER, [this, since, settled](wxTimerEvent&) {
        if (m_stopping || (!settled() && std::chrono::steady_clock::now() - since < std::chrono::seconds(10)))
            return;
        m_ready_timer->Stop();
        if (!canvas_gl_ready())
            BOOST_LOG_TRIVIAL(warning) << "MCP ready before the 3D view loaded OpenGL";
        if (pages_pending())
            BOOST_LOG_TRIVIAL(warning) << "MCP ready before the main window built its tab pages";
        set_ready();
    });
    m_ready_timer->Start(100);
}

void McpService::set_ready()
{
    m_ready = true;
    if (!m_options.script.empty())
        run_launch_script();
}

json McpService::describe() const
{
    json out{{"serving", serving()}, {"ready", ready()}, {"version", SLIC3R_VERSION}};
    if (serving()) {
        out["port"]           = port();
        out["url"]            = "http://127.0.0.1:" + std::to_string(port()) + "/mcp";
        out["token_source"]   = m_token_source;
        out["token_file"]     = path_to_utf8(mcp_dir(path_from_utf8(data_dir())) / "token");
        out["discovery_file"] = path_to_utf8(discovery_path(path_from_utf8(data_dir())));
        if (!m_discovery_error.empty())
            out["discovery_error"] = m_discovery_error;
    }
    return out;
}

void McpService::register_tools()
{
    register_app_tools(*this);
    register_action_tools(*this);
    register_project_tools(*this);
    register_config_tools(*this);
    register_scene_tools(*this);
    register_layer_tools(*this);
    register_view_tools(*this);
    register_ui_tools(*this);
    register_script_tools(*this);
    register_record_tools(*this);
#ifdef SLIC3R_CAD
    register_cad_tools(*this);
#endif
}

void McpService::run_launch_script()
{
    m_script_thread = std::thread([this] {
        int code = 2;
        try {
            json args{{"path", m_options.script}};
            if (!m_options.script_out.empty())
                args["out_dir"] = m_options.script_out;
            const ToolDef* def = m_registry.find("run_script");
            ToolResult     r   = m_core->call(*def, args);
            // A script that opened a dialog and answers it itself replies blocked_by_dialog first.
            const std::string status = r.is_error ? std::string() : r.data.value("status", "");
            if ((status == "running" || status == "blocked_by_dialog") && r.data.contains("job")) {
                std::optional<ToolResult> done;
                while (!done && !m_stopping)
                    done = m_jobs.wait(r.data["job"].get<std::string>(), std::chrono::seconds(5));
                if (done)
                    r = *done;
            }
            if (r.is_error)
                BOOST_LOG_TRIVIAL(error) << "MCP script: " << r.message;
            code = r.is_error ? (r.data.value("invalid", false) ? 2 : 1) : (r.data.value("passed", false) ? 0 : 1);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "MCP script: " << e.what();
        }
        g_script_exit_code = code;
        BOOST_LOG_TRIVIAL(info) << "MCP script finished with exit code " << code;
        if (m_stopping || wxTheApp == nullptr)
            return;
        // Close without the unsaved-project prompt: the script owns this session.
        m_dispatcher->post([] {
            if (wxTheApp != nullptr && wxGetApp().mainframe != nullptr)
                wxGetApp().mainframe->Close(true);
        });
    });
}

void McpService::post_ui(std::function<void()> fn)
{
    if (m_dispatcher)
        m_dispatcher->post(std::move(fn));
}

void McpService::request_stop(const std::string& job) { m_jobs.request_stop(job); }

std::optional<ToolResult> McpService::call_to_end(const ToolDef& def, const json& args)
{
    ToolResult        r      = m_core->call(def, args);
    const std::string status = r.is_error ? std::string() : r.data.value("status", "");
    if ((status != "running" && status != "blocked_by_dialog") || !r.data.contains("job"))
        return r;
    const std::string         job = r.data["job"].get<std::string>();
    std::optional<ToolResult> done;
    while (!done && !m_stopping) {
        try {
            done = m_jobs.wait(job, std::chrono::seconds(1));
        } catch (const ToolFailure& f) {
            done = ToolResult::error(f.what()); // the job's result was lost
        }
    }
    return done;
}

bool McpService::encode_when_closed(json args, std::function<void(const ToolResult&)> done)
{
    const ToolDef* encode = started() ? m_registry.find("encode_recording") : nullptr;
    if (encode == nullptr || m_stopping)
        return false;
    // A person's encode, outside the session journal like the script runner's runs.
    ToolDef def = *encode;
    def.journal = false;
    std::lock_guard<std::mutex> lock(m_runs_mutex);
    // Threads that have set their flag are done or about to be; joining them keeps the list short.
    for (auto it = m_encode_threads.begin(); it != m_encode_threads.end();) {
        if (!*it->finished) {
            ++it;
            continue;
        }
        if (it->thread.joinable())
            it->thread.join();
        it = m_encode_threads.erase(it);
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    m_encode_threads.push_back({std::thread([this, def, args = std::move(args), done = std::move(done), finished] {
                                    struct Finished
                                    {
                                        std::atomic<bool>& flag;
                                        ~Finished() { flag = true; }
                                    } mark{*finished};
                                    // The sink's threads close the file soon after the recording ends.
                                    const fs::path source = path_from_utf8(args.value("path", std::string()));
                                    while (!m_stopping && !m_recorder->wait_closed(source, std::chrono::seconds(1))) {
                                    }
                                    if (m_stopping)
                                        return;
                                    std::optional<ToolResult> r = call_to_end(def, args);
                                    if (!r || m_stopping)
                                        return;
                                    post_ui([done, result = std::move(*r)] { done(result); });
                                }),
                                finished});
    return true;
}

bool McpService::discard_authorized(const std::set<std::string>& lineage) const
{
    std::lock_guard<std::mutex> lock(m_run_state_mutex);
    for (const std::string& job : lineage)
        if (m_discard_jobs.count(job) != 0)
            return true;
    return false;
}

void McpService::stop_script_runs()
{
    std::vector<std::shared_ptr<ScriptRun>> runs;
    {
        std::lock_guard<std::mutex> lock(m_runs_mutex);
        for (const RunThread& r : m_run_threads)
            if (r.run->running())
                runs.push_back(r.run);
    }
    for (const auto& run : runs)
        run->stop();
}

std::shared_ptr<ScriptRun> McpService::start_script_run(Script script, ScriptContext sctx, std::optional<std::size_t> only_step,
                                                        bool discard_authorized, ScriptRun::Events events)
{
    const ToolDef* run_script_def = started() ? m_registry.find("run_script") : nullptr;
    if (run_script_def == nullptr || m_stopping)
        return nullptr;
    {
        // Threads whose run has ended are done or about to be; joining them keeps the list short.
        std::lock_guard<std::mutex> lock(m_runs_mutex);
        for (auto it = m_run_threads.begin(); it != m_run_threads.end();) {
            if (it->run->running()) {
                ++it;
                continue;
            }
            if (it->thread.joinable())
                it->thread.join();
            it = m_run_threads.erase(it);
        }
    }

    auto run      = std::make_shared<ScriptRun>(*this, std::move(events));
    auto shared   = std::make_shared<Script>(std::move(script));
    auto ctx_held = std::make_shared<ScriptContext>(std::move(sctx));
    // run_script's properties, outside the session journal: a person's run is not an agent's call.
    ToolDef def = *run_script_def;
    def.journal = false;
    def.handler = [this, run, shared, ctx_held, only_step, discard_authorized](const json&, ToolContext& ctx) {
        const std::string job = ctx.job_id();
        run->set_job(job);
        struct Authorization
        {
            McpService& service;
            std::string job;
            bool        held;
            ~Authorization()
            {
                if (held) {
                    std::lock_guard<std::mutex> lock(service.m_run_state_mutex);
                    service.m_discard_jobs.erase(job);
                }
            }
        } authorization{*this, job, discard_authorized};
        if (discard_authorized) {
            std::lock_guard<std::mutex> lock(m_run_state_mutex);
            m_discard_jobs.insert(job);
        }
        ScriptRunOptions options;
        options.only_step     = only_step;
        options.write_result  = !only_step;
        options.step_started  = [run](std::size_t index) { run->post_step_started(index); };
        options.step_finished = [run](const json& entry) { run->post_step_finished(entry); };
        return ToolResult::ok(run_script_job(*this, ctx, *shared, *ctx_held, options));
    };

    std::lock_guard<std::mutex> lock(m_runs_mutex);
    m_run_threads.push_back({std::thread([this, run, def, ctx_held] {
                                 const std::optional<ToolResult> r = call_to_end(def, json::object());
                                 run->mark_ended();
                                 if (!r || m_stopping)
                                     return;
                                 // The handler has returned, so the jobs it saved are settled. A run refused
                                 // before its handler started (another operation holds the project) saved none
                                 // and replaces none.
                                 run->post_finished(r->is_error ? json{{"error", r->message}} : r->data,
                                                    run->job().empty() ? json() : ctx_held->jobs);
                             }),
                             run});
    return run;
}

}}} // namespace Slic3r::GUI::Mcp
