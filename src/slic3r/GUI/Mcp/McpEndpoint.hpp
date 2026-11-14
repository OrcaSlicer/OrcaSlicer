#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class wxTimer;

namespace Slic3r { namespace GUI { namespace Mcp {

class AutomationRuntime;
class McpHttpServer;
class McpProtocol;
struct RequestGate;

// The port the endpoint listens on: `option_port` when it is above 0, else `setting` when it parses; the result when
// it is 1 to 65535, else kDefaultPort.
int endpoint_port(int option_port, const std::string& setting);

// The MCP server, one of the automation runtime's front ends: HTTP on the loopback, the protocol, the request gate,
// the token, the discovery file, and the takeover of a port that another window holds. It reaches the runtime's core
// through the protocol and holds the runtime by reference; the runtime does not know it. GUI_App owns it, opens it
// once the runtime has started, closes it before the runtime stops and destroys it first.
class McpEndpoint
{
public:
    // `retry_failed` is called on the UI thread with the error when a retry of open() fails for another reason
    // than the port being held by another window; the retries then stop. It must not close the endpoint: it runs
    // inside the retry timer's handler, and close() deletes that timer.
    explicit McpEndpoint(AutomationRuntime& runtime, std::function<void(const std::string&)> retry_failed = {});
    McpEndpoint(const McpEndpoint&)            = delete;
    McpEndpoint& operator=(const McpEndpoint&) = delete;
    // Closes it. The owner drains first.
    ~McpEndpoint();

    // Opens HTTP on 127.0.0.1:`port`, turns the runtime's GUI instruments on and opens its client scope.
    // False with `err` set when it cannot listen; while another window that shares the configuration folder
    // holds the port it retries every 3 s until it opens, a retry fails otherwise, or close(). UI thread.
    bool open(int port, std::string& err);
    // Closes the socket, stops the retries, removes the discovery file and closes the runtime's client
    // scope. The request gate stays for requests in flight. UI thread.
    void close();
    // After close(): waits until no request thread is inside the protocol; false when one still is after
    // `timeout`, and the endpoint and the runtime must then be leaked. An endpoint that has been drained does not
    // serve again: a later request gets 503.
    bool drain(std::chrono::milliseconds timeout);

    bool serving() const;
    uint16_t port() const;
    const std::string& token() const { return m_token; }
    // Where the token in use comes from: "file" or "ORCA_MCP_TOKEN".
    const std::string& token_source() const { return m_token_source; }
    // The last open() could not listen because another OrcaSlicer window that shares the configuration folder
    // serves on the port. Cleared when the endpoint opens or close() runs.
    bool port_held_by_other_window() const { return m_port_held_by_other; }
    // {serving} and, while serving, {port, url, token_source, token_file, discovery_file, discovery_error?}.
    nlohmann::json describe() const;

private:
    // open() without the retries: listens on `port` with the token in force. UI thread.
    bool listen(int port, std::string& err);
    // The retry timer's tick: open() again on what the last open() asked for, while that is still wanted. UI thread.
    void retry();

    AutomationRuntime&                      m_runtime;
    std::function<void(const std::string&)> m_retry_failed;

    std::string m_token;
    std::string m_token_source;
    std::string m_discovery_error;
    bool        m_port_held_by_other = false;
    // What the last open() asked for, which the retries ask for again: from open() until close().
    bool        m_wanted      = false;
    int         m_wanted_port = 0;

    // Made by the first open() and kept across close(): request threads of an endpoint closed earlier may still be
    // inside the protocol, which they reach through the gate.
    std::unique_ptr<McpProtocol>   m_protocol;
    std::shared_ptr<RequestGate>   m_gate;
    std::unique_ptr<McpHttpServer> m_http;
    // Tries the port again every 3 s while another window holds it. Made on first need, and gone with close(), so an
    // endpoint left in place at exit holds no timer past the toolkit.
    std::unique_ptr<wxTimer> m_retry_timer;
};

}}} // namespace Slic3r::GUI::Mcp
