#include "McpCore.hpp"
#include "McpOptions.hpp"
#include "McpService.hpp"

#include "McpHttpServer.hpp"
#include "McpProtocol.hpp"
#include "McpToken.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"

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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

std::atomic<int> g_script_exit_code{-1};

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

// Server instructions shown to the model; clients truncate them at 4096 characters.
const char* kInstructions =
    "OrcaSlicer, a 3D-printing slicer, driven through its GUI.\n"
    "Workflow: get_state (is it ready? which presets, plates, dialogs?) -> act -> wait -> screenshot.\n"
    "- Prefer typed tools (open_files, select_preset, set_settings, slice, export) and run_action (every Speed Dial "
    "command: list_actions to search) over GUI clicks.\n"
    "- For anything else use the GUI tools: ui_snapshot lists widgets, menus, 3D toolbar items, gizmos and ImGui "
    "items with refs; ui_click / ui_input / ui_key / ui_mouse act on refs from the latest snapshot.\n"
    "- A call that takes long or opens a dialog returns {status:\"running\"|\"blocked_by_dialog\", job}. It keeps "
    "running: never retry it. Answer dialogs with dialog_respond (see get_state.dialogs), then wait {for:\"job\"}.\n"
    "- While a slice or export runs, other changes are refused; use wait or cancel.\n"
    "- Paths are absolute. Outputs never overwrite an existing file unless overwrite is true.\n"
    "- screenshot returns a downscaled image plus the full-size PNG path; target \"canvas\" is the 3D view.\n"
    "- export_script saves this session's calls as a replayable script; run_script replays one.";

} // namespace

McpService::McpService() = default;

McpService::~McpService() { stop(); }

int McpService::script_exit_code() { return g_script_exit_code.load(); }

bool McpService::serving() const { return m_http != nullptr && m_http->running(); }

uint16_t McpService::port() const { return m_http ? m_http->port() : 0; }

fs::path McpService::output_dir() const
{
    const fs::path dir = mcp_dir(fs::path(data_dir())) / "out";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

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
    if (m_core == nullptr) {
        m_options    = options;
        m_dispatcher = std::make_unique<WxDispatcher>();
        m_modals     = std::make_unique<ModalTracker>();
        m_inspector  = std::make_unique<UiInspector>(*this);
        m_invoker    = std::make_unique<UiInvoker>(*m_dispatcher, [this] { return guards_on_ui(); });
        m_core       = std::make_unique<McpCore>(m_registry, *m_invoker, m_jobs, &m_journal);
        register_tools();
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
    if (const char* env = boost::nowide::getenv("ORCA_MCP_TOKEN"); env != nullptr && *env != '\0')
        token = env;
    else if (!options.token.empty())
        token = options.token;
    else
        token = load_or_create_token(fs::path(data_dir()), err);
    if (token.empty()) {
        err = "MCP server: no token (" + err + ")";
        return false;
    }
    m_token = token;

    ServerInfo info;
    info.version      = SLIC3R_VERSION;
    info.instructions = kInstructions;
    // Request threads of an endpoint closed earlier may still be using the protocol object.
    if (!m_protocol)
        m_protocol = std::make_unique<McpProtocol>(m_registry, *m_core, info);
    m_http            = std::make_unique<McpHttpServer>(token, [this](const PostRequest& r) { return m_protocol->handle_post(r); });
    if (!m_http->start(uint16_t(port), err)) {
        err = "MCP server: cannot listen on 127.0.0.1:" + std::to_string(port) + " (" + err + ")";
        BOOST_LOG_TRIVIAL(error) << err;
        m_http.reset();
        return false;
    }

    std::string discovery_err;
    if (!write_discovery(fs::path(data_dir()),
                         {{"url", "http://127.0.0.1:" + std::to_string(m_http->port()) + "/mcp"},
                          {"port", m_http->port()},
                          {"pid", int(getpid())},
                          {"version", SLIC3R_VERSION},
                          {"token", token}},
                         discovery_err))
        BOOST_LOG_TRIVIAL(warning) << "MCP server: " << discovery_err;
    BOOST_LOG_TRIVIAL(info) << "MCP server listening on http://127.0.0.1:" << m_http->port() << "/mcp";
    return true;
}

void McpService::stop()
{
    if (m_core == nullptr || m_stopping.exchange(true))
        return;
    BOOST_LOG_TRIVIAL(info) << "MCP server stopping";
    // Wake everything that waits before joining anything.
    m_invoker->shutdown();
    m_jobs.wake_all();
    m_core->shutdown();
    stop_serving();
    if (m_script_thread.joinable())
        m_script_thread.join();
    if (wxTheApp != nullptr)
        if (ImGuiWrapper* imgui = wxGetApp().imgui())
            imgui->set_item_recording(false);
    m_inspector.reset();
    m_modals.reset();
}

std::string McpService::set_auto_answer(std::vector<AutoAnswerRule> rules, const std::set<std::string>& lineage)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    // A running script in the caller's lineage owns the rules.
    for (const std::string& job : lineage)
        if (auto it = m_script_rule_layers.find(job); it != m_script_rule_layers.end()) {
            if (!rules.empty())
                it->second.push_back(m_modals->push_rules(std::move(rules)));
            return "script";
        }
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

void McpService::end_script_scope(const std::string& job)
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

void McpService::stop_serving()
{
    if (!m_http)
        return;
    // Requests in flight may be waiting on the UI thread, which is running this.
    m_http->stop(m_stopping);
    m_http.reset();
    remove_discovery(fs::path(data_dir()));
    BOOST_LOG_TRIVIAL(info) << "MCP server endpoint closed";
}

void McpService::suspend()
{
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
    if (m_ready.exchange(true))
        return;
    if (!m_options.script.empty())
        run_launch_script();
}

json McpService::describe() const
{
    json out{{"serving", serving()}, {"ready", ready()}, {"version", SLIC3R_VERSION}};
    if (serving()) {
        out["port"] = port();
        out["url"]  = "http://127.0.0.1:" + std::to_string(port()) + "/mcp";
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
    register_view_tools(*this);
    register_ui_tools(*this);
    register_script_tools(*this);
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
            if (!r.is_error && r.data.value("status", "") == "running" && r.data.contains("job")) {
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

}}} // namespace Slic3r::GUI::Mcp
