#include "McpEndpoint.hpp"

#include "ActivityLog.hpp"
#include "AutomationRuntime.hpp"
#include "McpHttpServer.hpp"
#include "McpOptions.hpp"
#include "McpPaths.hpp"
#include "McpProtocol.hpp"
#include "McpToken.hpp"

#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NativeCommands.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <nlohmann/json.hpp>

#include <wx/event.h>
#include <wx/timer.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

// HTTP request threads can outlive the server they came from; they reach the protocol through this gate, which
// drain() closes before the endpoint goes away.
struct RequestGate
{
    std::mutex              mutex;
    std::condition_variable cv;
    McpProtocol*            protocol = nullptr;
    int                     active   = 0;
};

using nlohmann::json;

namespace {

std::string shutting_down_reply()
{
    return json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32603}, {"message", "OrcaSlicer is shutting down"}}}}.dump();
}

} // namespace

int endpoint_port(int option_port, const std::string& setting)
{
    int port = option_port;
    if (port <= 0) {
        try {
            port = std::stoi(setting);
        } catch (const std::exception&) {
            port = 0;
        }
    }
    return port > 0 && port <= 65535 ? port : kDefaultPort;
}

McpEndpoint::McpEndpoint(AutomationRuntime& runtime, std::function<void(const std::string&)> retry_failed)
    : m_runtime(runtime), m_retry_failed(std::move(retry_failed))
{}

McpEndpoint::~McpEndpoint() { close(); }

bool McpEndpoint::serving() const { return m_http != nullptr && m_http->running(); }

uint16_t McpEndpoint::port() const { return m_http ? m_http->port() : 0; }

bool McpEndpoint::open(int port, std::string& err)
{
    m_wanted      = true;
    m_wanted_port = port;
    const bool ok = listen(port, err);
    if (!ok && m_port_held_by_other) {
        // Another window that shares the configuration folder serves agents. Not an error: try the port every few
        // seconds, so agents keep a server once that window closes.
        if (!m_retry_timer) {
            m_retry_timer = std::make_unique<wxTimer>();
            m_retry_timer->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { retry(); });
        }
        if (!m_retry_timer->IsRunning())
            m_retry_timer->Start(3000);
    } else if (m_retry_timer)
        m_retry_timer->Stop();
    return ok;
}

void McpEndpoint::retry()
{
    if (!m_wanted || serving() || m_runtime.stopping() || wxGetApp().is_closing()) {
        m_retry_timer->Stop();
        return;
    }
    std::string err;
    // A failure other than the port being held has stopped the retries; the owner shows it.
    if (!open(m_wanted_port, err) && !m_port_held_by_other && m_retry_failed)
        m_retry_failed(err);
}

bool McpEndpoint::listen(int port, std::string& err)
{
    if (serving())
        return true;

    std::string token;
    if (const char* env = boost::nowide::getenv("ORCA_MCP_TOKEN"); env != nullptr && *env != '\0') {
        token          = env;
        m_token_source = "ORCA_MCP_TOKEN";
    } else {
        token          = load_or_create_token(path_from_utf8(data_dir()), err);
        m_token_source = "file";
    }
    if (token.empty()) {
        err = "MCP server: no token (" + err + ")";
        // Not the port: the retries of a takeover stop on this, and the owner shows it.
        m_port_held_by_other = false;
        return false;
    }
    m_token = token;

    // Request threads of an endpoint closed earlier may still be using the protocol object.
    if (!m_protocol) {
        m_protocol       = std::make_unique<McpProtocol>(NativeCommands::instance(), m_runtime.core(), server_info());
        // A call refused before it ran is shown too: it is the agent's.
        m_protocol->set_on_refused([&runtime = m_runtime](const std::string& tool, const nlohmann::json& origin, const ToolResult& error) {
            runtime.activity().refused(tool, origin, error);
        });
        m_gate           = std::make_shared<RequestGate>();
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
    // The client scope opens before the port does: a request that arrives at once must find it open.
    m_runtime.open_client_scope();
    if (!m_http->start(uint16_t(port), err)) {
        err = "MCP server: cannot listen on 127.0.0.1:" + std::to_string(port) + " (" + err + ")";
        m_http.reset();
        // No client was served, so nothing of a client's is there to end; this takes the scope back.
        m_runtime.close_client_scope();
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
    // Agents drive the GUI through the endpoint.
    m_runtime.enable_gui_instruments();

    m_discovery_error.clear();
    if (!write_discovery(path_from_utf8(data_dir()),
                         {{"url", "http://127.0.0.1:" + std::to_string(m_http->port()) + "/mcp"},
                          {"port", m_http->port()},
                          {"pid", int(getpid())},
                          {"version", SLIC3R_VERSION},
                          {"token", token}},
                         m_discovery_error))
        // Serving anyway: clients configured with the URL and token still connect; the stdio
        // bridge and the harness, which need the file, see the reason in app_state.
        BOOST_LOG_TRIVIAL(warning) << "MCP server: " << m_discovery_error;
    BOOST_LOG_TRIVIAL(info) << "MCP server listening on http://127.0.0.1:" << m_http->port() << "/mcp";
    return true;
}

void McpEndpoint::close()
{
    m_wanted = false;
    m_retry_timer.reset();
    m_port_held_by_other = false;
    // A client's recording, held presses and rules end with its endpoint; a run's carry on until the run ends.
    m_runtime.close_client_scope();
    if (!m_http)
        return;
    // Requests in flight may be waiting on the UI thread, which is running this; drain() waits for
    // them before the endpoint goes away.
    m_http->stop(false);
    m_http.reset();
    remove_discovery(path_from_utf8(data_dir()));
    BOOST_LOG_TRIVIAL(info) << "MCP server endpoint closed";
}

bool McpEndpoint::drain(std::chrono::milliseconds timeout)
{
    if (!m_gate)
        return true;
    std::unique_lock<std::mutex> lock(m_gate->mutex);
    // A request that comes after this gets 503 rather than the protocol.
    m_gate->protocol = nullptr;
    return m_gate->cv.wait_for(lock, timeout, [this] { return m_gate->active == 0; });
}

json McpEndpoint::describe() const
{
    json out{{"serving", serving()}};
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

}}} // namespace Slic3r::GUI::Mcp
