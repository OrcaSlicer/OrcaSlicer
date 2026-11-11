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
#include "WebPages.hpp"
#include "WindowControl.hpp"
#include "McpPaths.hpp"
#include "McpReplay.hpp"
#include "McpPersonRun.hpp"
#include "JobRegistry.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonScriptRunner.hpp"
#include "slic3r/plugin/PythonSession.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "UiInvoker.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/app.h>
#include <wx/event.h>
#include <wx/thread.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
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

std::atomic<int> g_run_exit_code{-1};

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
        if (wxTheApp == nullptr)
            return;
        wxTheApp->CallAfter(std::move(fn));
#ifdef __WXMSW__
        // CallAfter wakes only wx's own loop. In a native modal loop, a menu's or a system dialog's, wx runs
        // queued events from a message hook when the loop takes a message off the thread's queue, so give it one
        // rather than wait for an unrelated message.
        wxWakeUpMainThread();
#endif
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
// The scope key of a plugin's calls, which no job id ("j<n>") can equal.
std::string plugin_scope_key(const std::string& plugin_key) { return "plugin:" + plugin_key; }

const char* kInstructions =
    "OrcaSlicer, a 3D-printing slicer, driven through its GUI.\n"
    "Each tool's description starts with its lane: [core] runs the session and recovers from dialogs, [api] acts on "
    "project data by index and name with JSON in and out, [ui] drives the GUI itself. Every tool is callable; prefer "
    "[api] tools, and use [ui] tools for what no [api] tool reaches or to see what a person sees.\n"
    "Workflow: get_app_state (is it ready? which presets, plates, dialogs?) -> act -> check it with the reading "
    "tools (list_objects, list_plates, get_settings, slice's reply, get_slice_stats); screenshot only to see what they "
    "do not report.\n"
    "Core:\n"
    "- A call that takes long or opens a dialog returns {status:\"running\"|\"blocked_by_dialog\", job}. It keeps "
    "running: never retry it. Answer dialogs with answer_dialog (see get_app_state.dialogs), then wait {for:\"job\"}. "
    "A native message box (native: true) is answered with answer_dialog by one of the buttons listed for it; a file or "
    "folder picker, or a native dialog whose buttons list is empty (any on macOS), can't be: register auto_answer_dialogs "
    "before the step that opens one.\n"
    "- While an operation (slice, export, arrange or another background job) runs, other changes are refused; use wait "
    "or cancel_job.\n"
    "- export_replay saves this session's calls as a replay that run_replay runs.\n"
    "- run_script runs Python against orca.host, the plugin API (search_python_api searches it), computing over meshes and "
    "settings and calling tools through orca.host.tools; its own file, program, thread and network use is restricted "
    "unless get_app_state.python.scripts_unrestricted.\n"
    "- Not sure what does something? search_tools {query} searches every catalog at once: the tools, the operations "
    "plugins offer (plugin.<plugin>.<name>, which the tool list does not show; call_tool runs them), Speed Dial actions, "
    "settings, the Python API and the Design tab's CAD tools, each match with how to use it.\n"
    "API lane:\n"
    "- Typed tools: open_files, select_preset, set_settings, slice, export and the rest.\n"
    "- What a setting does, or which one controls an effect: get_settings with keys, or query with a few words, and no "
    "scope, which looks through the process, filament and printer settings; each item names its scope, the one "
    "set_settings needs (its default is the process), and its meta has its description and where (its settings page and "
    "group).\n"
    "- Objects, parts and plates are 0-based indices, as list_objects and list_plates give "
    "them; filament slots and Preview layers are 1-based.\n"
    "- Lists come in pages: {items, offset, total}, with next_offset while more remain; call again with that offset to "
    "read on. list_objects has plates and outside in place of items.\n"
    "- Paths are absolute. Outputs never overwrite a file unless overwrite is true.\n"
    "UI lane:\n"
    "- run_action runs every Speed Dial command (search_actions finds them): prefer it to GUI clicks.\n"
    "- list_widgets lists widgets, menus, 3D toolbar items, gizmos, ImGui items and web page elements with refs; click "
    "/ set_widget_value / press_keys / mouse_gesture act on refs from the latest list_widgets call, or on locators, which "
    "its description lists.\n"
    "- list_windows lists the open windows, modal or not, with refs: a window's ref as screenshot's target captures "
    "that window, as list_widgets's target lists it alone, and as set_window's target sizes, moves or raises it.\n"
    "- Gizmos (Cut, Text, SVG, the painting tools...): click {target: {gizmo: \"Cut\"}} opens one; its panel's "
    "fields and buttons are ImGui items for set_widget_value / click, and painting is a mouse_gesture drag starting on the "
    "object. Move the camera with set_view, never a drag: what a drag does follows the user's mouse settings. pick_gizmo_file only stands in for a file picker inside an open gizmo.\n"
    "- screenshot returns a downscaled image plus the full-size PNG path; target \"canvas\" is the 3D view.\n"
    "- start_recording / stop_recording record a video (MJPEG .mkv), optionally with the tool calls as captions and the "
    "pointer highlighted; with encode, stop_recording then encodes it to MP4 with the system ffmpeg as a job to wait "
    "for, and encode_recording encodes any recording later.";

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

const ToolRegistry& McpService::offline_catalog()
{
    // Kept, never destroyed: the catalog's handlers refer to this service, and the bridge answers from
    // the catalog until the process exits.
    static McpService* const service = [] {
        auto* s = new McpService();
        s->register_tools();
        return s;
    }();
    return service->m_registry;
}

ServerInfo McpService::server_info()
{
    ServerInfo info;
    info.version      = SLIC3R_VERSION;
    info.instructions = kInstructions;
    return info;
}

// Reached only through McpServiceDeleter, after stop() joined the launch thread and drain() saw
// every thread leave.
McpService::~McpService() = default;

int McpService::run_exit_code() { return g_run_exit_code.load(); }

bool McpService::serving() const { return m_http != nullptr && m_http->running(); }

uint16_t McpService::port() const { return m_http ? m_http->port() : 0; }

GuardState McpService::guards_on_ui() const
{
    GuardState g;
    g.closing   = m_stopping || wxTheApp == nullptr || wxGetApp().is_closing();
    g.suspended = m_suspended;
    if (m_modals) {
        g.dialogs    = m_modals->snapshot();
        g.modal      = !g.dialogs.empty();
        g.top_dialog = m_modals->serial(m_modals->top());
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
        // A plugin's calls are refused from the moment it unloads, before end_plugin_scope() comes.
        m_jobs.set_owner_liveness([](const std::string& plugin) { return PluginManager::instance().is_plugin_loaded(plugin); });
        m_core       = std::make_unique<McpCore>(m_registry, *m_invoker, m_jobs, &m_journal, CoreTiming{}, ui_job_worker_hooks(),
                                                 m_recorder.get());
        m_web_presses = std::make_shared<WebPresses>();
        m_modals->set_before_press([this] { release_web_presses(m_web_presses, nullptr); });
        register_tools();
        track_window_states();
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

    // Request threads of an endpoint closed earlier may still be using the protocol object.
    if (!m_protocol) {
        m_protocol = std::make_unique<McpProtocol>(m_registry, *m_core, server_info());
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
        // bridge and the harness, which need the file, see the reason in get_app_state.
        BOOST_LOG_TRIVIAL(warning) << "MCP server: " << m_discovery_error;
    BOOST_LOG_TRIVIAL(info) << "MCP server listening on http://127.0.0.1:" << m_http->port() << "/mcp";
    return true;
}

void McpService::stop()
{
    // Before the stop flag: a run's stop does nothing once the service is stopping, and it must cancel
    // the run's slice while the main window is still there.
    if (m_core != nullptr && !m_stopping)
        stop_person_runs();
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
    if (m_launch_thread.joinable())
        m_launch_thread.join();
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
        // Run rules too: no run survives the stop.
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        for (const auto& [job, layers] : m_run_rule_layers)
            for (int layer : layers)
                m_modals->pop_rules(layer);
        m_run_rule_layers.clear();
    }
    if (m_inspector) {
        m_inspector->release_held(nullptr);
        release_web_presses(m_web_presses, nullptr);
        m_inspector->capture_menus(false);
    }
    SyntheticInput::clear();
    if (wxTheApp != nullptr)
        if (ImGuiWrapper* imgui = wxGetApp().imgui())
            imgui->set_item_recording(false);
}

bool McpService::acquire_lease()
{
    if (m_stopping)
        return false;
    ++m_leases;
    // A stop that came meanwhile may already be draining: give the lease back rather than race it.
    if (m_stopping) {
        --m_leases;
        return false;
    }
    return true;
}

void McpService::release_lease() { --m_leases; }

bool McpService::drain(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // A lease's holder finds the core stopped and returns at once.
    while (m_leases.load() > 0) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m_core == nullptr)
        return true;
    if (m_gate) {
        std::unique_lock<std::mutex> lock(m_gate->mutex);
        m_gate->protocol = nullptr;
        if (!m_gate->cv.wait_until(lock, deadline, [this] { return m_gate->active == 0; }))
            return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return m_core->drain(std::max(left, std::chrono::milliseconds(0)));
}

AutoAnswerSet McpService::set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry)
{
    // A plugin's call is owned by its plugin's scope when no run in its chain is nearer.
    const std::string plugin = ancestry.empty() ? std::string() : m_jobs.owner(ancestry.front());
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    // The nearest running replay or script in the caller's job chain owns the rules.
    for (const std::string& job : ancestry)
        if (auto it = m_run_rule_layers.find(job); it != m_run_rule_layers.end()) {
            const bool paused = m_paused_scopes.count(job) != 0;
            if (rules.empty())
                return {"run", 0, paused};
            it->second.push_back(m_modals->push_rules(std::move(rules), paused));
            return {"run", it->second.back(), paused};
        }
    if (!plugin.empty()) {
        if (auto it = m_run_rule_layers.find(plugin_scope_key(plugin)); it != m_run_rule_layers.end()) {
            if (rules.empty())
                return {"plugin", 0, false};
            it->second.push_back(m_modals->push_rules(std::move(rules)));
            return {"plugin", it->second.back(), false};
        }
        if (m_unloaded_plugins.count(plugin) != 0)
            throw ToolFailure("The plugin that made this call has been unloaded; its rules were not applied");
    }
    // A call that was running when the endpoint closed must not bring its client's rules back.
    if (!m_endpoint_open)
        throw ToolFailure("The MCP server was turned off while the rules were being set; they were not applied");
    if (m_session_rule_layer != 0)
        m_modals->pop_rules(m_session_rule_layer);
    m_session_rule_layer = rules.empty() ? 0 : m_modals->push_rules(std::move(rules));
    return {"session", m_session_rule_layer, false};
}

void McpService::begin_run_scope(const std::string& job)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    m_run_rule_layers.emplace(job, std::vector<int>{});
}

void McpService::end_run_scope(const std::string& job, bool wait_recording)
{
    // Noted before the scope goes: from then on a call of this run finds no live scope, and must not be
    // taken for a client's.
    if (m_recorder)
        m_recorder->owner_ended(job);
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        auto it = m_run_rule_layers.find(job);
        if (it == m_run_rule_layers.end())
            return;
        if (m_modals)
            for (int layer : it->second)
                m_modals->pop_rules(layer);
        m_run_rule_layers.erase(it);
        m_paused_scopes.erase(job);
    }
    // A recording the run started ends with it, its file closed before the run's call returns.
    if (m_recorder)
        m_recorder->end_owned_by(job, "run_ended", wait_recording);
    release_run_presses(job);
}

void McpService::set_run_scope_paused(const std::string& job, bool paused)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    auto                        it = m_run_rule_layers.find(job);
    if (it == m_run_rule_layers.end())
        return;
    if (paused)
        m_paused_scopes.insert(job);
    else
        m_paused_scopes.erase(job);
    if (m_modals)
        for (int layer : it->second)
            m_modals->set_paused(layer, paused);
}

void McpService::release_run_presses(const std::string& job)
{
    // Found again on the UI thread: the service may be gone by the time the closure runs.
    if (m_dispatcher && !m_stopping)
        m_dispatcher->post([job] {
            if (McpService* service = wxGetApp().mcp_service(); service != nullptr && service->m_inspector) {
                service->m_inspector->release_held(&job);
                release_web_presses(service->m_web_presses, &job);
            }
        });
}

void McpService::cancel_operations_of(const std::string& job)
{
    for (const std::string& op : m_jobs.running_operations())
        if (m_jobs.lineage(op).count(job) != 0 && !m_core->cancel_worker(op))
            if (Plater* plater = wxGetApp().plater())
                plater->cancel_slicing();
}

std::string McpService::run_owner(const std::vector<std::string>& ancestry) const
{
    const std::string           plugin = ancestry.empty() ? std::string() : m_jobs.owner(ancestry.front());
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    for (const std::string& job : ancestry)
        if (m_run_rule_layers.count(job) != 0)
            return job;
    if (!plugin.empty() && m_run_rule_layers.count(plugin_scope_key(plugin)) != 0)
        return plugin_scope_key(plugin);
    // A call of an unloaded plugin must not leave anything nothing would end.
    if (!plugin.empty() && m_unloaded_plugins.count(plugin) != 0)
        throw ToolFailure("The plugin that made this call has been unloaded");
    return {};
}

bool McpService::begin_plugin_scope(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    if (m_unloaded_plugins.count(plugin_key) != 0)
        return false;
    m_run_rule_layers.emplace(plugin_scope_key(plugin_key), std::vector<int>{});
    return true;
}

void McpService::end_plugin_scope(const std::string& plugin_key)
{
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_unloaded_plugins.insert(plugin_key);
    }
    // No call of the plugin starts from now on, one it was admitted for included, and the ones running stop,
    // as a stopped replay's steps do.
    m_jobs.block_owner(plugin_key);
    for (const std::string& job : m_jobs.running_owned_by(plugin_key))
        m_jobs.request_stop(job);
    // Not waited for: an unloading plugin's recording is closed by the recorder's own threads.
    end_run_scope(plugin_scope_key(plugin_key), false);
}

void McpService::plugin_loaded(const std::string& plugin_key)
{
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_unloaded_plugins.erase(plugin_key);
    }
    m_jobs.unblock_owner(plugin_key);
}

bool McpService::endpoint_open() const
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    return m_endpoint_open;
}

void McpService::stop_serving()
{
    m_port_held_by_other = false;
    // A client's recording ends with its endpoint; a run's carries on until the run ends.
    if (m_recorder)
        m_recorder->end_owned_by({}, "endpoint_closed", false);
    // So does a press a client's mouse_gesture left held, on a widget or a page.
    if (m_inspector) {
        const std::string client;
        m_inspector->release_held(&client);
        release_web_presses(m_web_presses, &client);
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
    stop_person_runs();
    if (m_recorder)
        m_recorder->suspend();
    m_suspended = true;
    ++m_generation;
}

void McpService::resume()
{
    ++m_generation;
    m_suspended = false;
    if (m_recorder)
        m_recorder->resume();
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
    if (!m_options.run_file.empty())
        run_launch_file();
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
    register_replay_tools(*this);
    register_record_tools(*this);
#ifdef SLIC3R_CAD
    register_cad_tools(*this);
#endif
    register_python_tools(*this);
    register_api_tools(*this);
}

ToolDef McpService::as_person(const ToolDef& def)
{
    ToolDef person = def;
    person.handler = [this, handler = def.handler](const json& args, ToolContext& ctx) {
        const std::string job = ctx.job_id();
        {
            std::lock_guard<std::mutex> lock(m_run_state_mutex);
            m_person_jobs.insert(job);
        }
        struct Forget
        {
            McpService& service;
            std::string job;
            ~Forget()
            {
                std::lock_guard<std::mutex> lock(service.m_run_state_mutex);
                service.m_person_jobs.erase(job);
            }
        } forget{*this, job};
        return handler(args, ctx);
    };
    return person;
}

bool McpService::person_run(const std::set<std::string>& lineage) const
{
    std::lock_guard<std::mutex> lock(m_run_state_mutex);
    for (const std::string& job : lineage)
        if (m_person_jobs.count(job) != 0)
            return true;
    return false;
}

void McpService::run_launch_file()
{
    m_launch_thread = std::thread([this] {
        int code = 2;
        try {
            const fs::path file   = path_from_utf8(m_options.run_file);
            const bool     python = boost::algorithm::iequals(path_to_utf8(file.extension()), ".py");
            json           args;
            if (python) {
                boost::nowide::ifstream in(m_options.run_file.c_str(), std::ios::binary);
                if (!in)
                    throw std::runtime_error("Cannot read " + m_options.run_file);
                std::ostringstream raw;
                raw << in.rdbuf();
                std::string text, encoding, error;
                if (!decode_python_source(raw.str(), text, encoding, error))
                    throw std::runtime_error("Cannot read " + m_options.run_file + " as Python source: " + error);
                args = {{"code", text}, {"filename", m_options.run_file}};
            } else {
                args = {{"path", m_options.run_file}};
                if (!m_options.run_out.empty())
                    args["out_dir"] = m_options.run_out;
            }
            const ToolDef def = as_person(*m_registry.find(python ? "run_script" : "run_replay"));
            ToolResult    r   = m_core->call(def, args);
            // A replay or script that opened a dialog and answers it itself replies blocked_by_dialog first.
            const std::string status = r.is_error ? std::string() : r.data.value("status", "");
            if ((status == "running" || status == "blocked_by_dialog") && r.data.contains("job")) {
                std::optional<ToolResult> done;
                while (!done && !m_stopping)
                    done = m_jobs.wait(r.data["job"].get<std::string>(), std::chrono::seconds(5));
                if (done)
                    r = *done;
            }
            if (r.is_error)
                BOOST_LOG_TRIVIAL(error) << "--mcp-run: " << r.message;
            if (python) {
                if (!r.is_error && r.data.value("status", "") != "finished")
                    BOOST_LOG_TRIVIAL(error) << "--mcp-run: " << r.data.value("message", std::string()) << "\n"
                                             << r.data.value("traceback", std::string());
                code = !r.is_error && r.data.value("status", "") == "finished" ? 0 : 1;
            } else
                code = r.is_error ? (r.data.value("invalid", false) ? 2 : 1) : (r.data.value("passed", false) ? 0 : 1);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "--mcp-run: " << e.what();
        }
        g_run_exit_code = code;
        BOOST_LOG_TRIVIAL(info) << "--mcp-run finished with exit code " << code;
        if (m_stopping || wxTheApp == nullptr)
            return;
        // Close without the unsaved-project prompt: the run owns this session.
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
    // A person's encode, outside the session journal like Action Replay's runs.
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

ToolResult McpService::start_encode(const json& args)
{
    const ToolDef* encode = started() ? m_registry.find("encode_recording") : nullptr;
    if (encode == nullptr || m_stopping)
        return ToolResult::error("OrcaSlicer is shutting down");
    ToolDef def   = *encode;
    def.journal   = false;
    def.budget_ms = 0; // the envelope at once; the result comes from wait
    return m_core->call(def, args);
}

bool McpService::discard_authorized(const std::set<std::string>& lineage) const
{
    std::lock_guard<std::mutex> lock(m_run_state_mutex);
    for (const std::string& job : lineage)
        if (m_discard_jobs.count(job) != 0)
            return true;
    return false;
}

void McpService::stop_person_runs()
{
    std::vector<std::shared_ptr<PersonRun>> runs;
    {
        std::lock_guard<std::mutex> lock(m_runs_mutex);
        for (const RunThread& r : m_run_threads)
            if (r.run->running())
                runs.push_back(r.run);
    }
    for (const auto& run : runs)
        run->stop();
}

std::shared_ptr<PersonRun> McpService::start_replay_run(Replay replay, ReplayContext sctx, std::optional<std::size_t> only_step,
                                                        bool discard_authorized, PersonRun::Events events)
{
    const ToolDef* run_replay_def = started() ? m_registry.find("run_replay") : nullptr;
    if (run_replay_def == nullptr || m_stopping)
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

    auto run      = std::make_shared<PersonRun>(*this, std::move(events));
    auto shared   = std::make_shared<Replay>(std::move(replay));
    auto ctx_held = std::make_shared<ReplayContext>(std::move(sctx));
    // run_replay's properties, outside the session journal: a person's run is not an agent's call.
    ToolDef def = *run_replay_def;
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
        ReplayRunOptions options;
        options.only_step     = only_step;
        options.write_result  = !only_step;
        options.step_started  = [run](std::size_t index) { run->post_step_started(index); };
        options.step_finished = [run](const json& entry) { run->post_step_finished(entry); };
        return ToolResult::ok(run_replay_job(*this, ctx, *shared, *ctx_held, options));
    };
    def = as_person(def);

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

std::shared_ptr<PersonRun> McpService::start_python_session(std::shared_ptr<PythonSession> session, const fs::path& file,
                                                           PersonRun::Events events)
{
    const ToolDef* run_script = started() ? m_registry.find("run_script") : nullptr;
    if (run_script == nullptr || m_stopping || !session)
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
    auto run      = std::make_shared<PersonRun>(*this, std::move(events));
    auto hosted   = std::make_shared<std::atomic<bool>>(false);
    auto filename = file.empty() ? std::string("<run_script>") : path_to_utf8(file);
    // run_script's properties, outside the session journal, as a person's run.
    ToolDef def = as_person(*run_script);
    def.journal = false;
    def.handler = [this, run, session, hosted, filename](const json&, ToolContext& ctx) {
        run->set_job(ctx.job_id());
        hosted->store(true);
        // A handler that refuses never hosts the session: it ends here all the same.
        struct Ended
        {
            PythonSession& session;
            ~Ended()
            {
                if (!session.ended())
                    session.host_returned();
            }
        } ended{*session};
        return run_python_session_job(*this, ctx, *session, filename);
    };

    std::lock_guard<std::mutex> lock(m_runs_mutex);
    m_run_threads.push_back({std::thread([this, run, def, session, hosted] {
                                 const std::optional<ToolResult> r = call_to_end(def, json::object());
                                 run->mark_ended();
                                 // Refused before its handler ran: the session was never hosted.
                                 if (r && !hosted->load())
                                     session->host_returned();
                                 if (!r || m_stopping || !r->is_error)
                                     return;
                                 run->post_finished(json{{"error", r->message}}, json());
                             }),
                             run});
    return run;
}

}}} // namespace Slic3r::GUI::Mcp
