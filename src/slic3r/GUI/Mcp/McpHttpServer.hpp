#pragma once

#include "McpProtocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// The parts of an HTTP request the endpoint rules look at. Header values are empty when absent.
struct HttpRequestView
{
    std::string method;
    std::string target;
    std::string host;
    std::string origin;
    std::string content_type;
    std::string authorization;
    std::size_t body_size = 0;
};

// Status 0 accepts the request; anything else is the HTTP status to reply with.
struct HttpCheck
{
    int         status = 0;
    std::string message;
};

// Endpoint rules applied before any JSON is parsed: only POST /mcp, Host must name this loopback
// port (DNS rebinding), a browser Origin must be loopback, the body must be JSON, and the bearer
// token must match.
HttpCheck validate_request(const HttpRequestView& request, uint16_t port, const std::string& token, std::size_t max_body);

struct HttpLimits
{
    std::chrono::milliseconds read_timeout{30000};  // to receive a complete request
    std::chrono::milliseconds idle_timeout{120000}; // between keep-alive requests
    std::chrono::milliseconds write_timeout{30000};
    std::chrono::milliseconds stop_timeout{5000};   // for request threads to return
    std::size_t               max_body        = 8u << 20;
    int                       max_connections  = 64;
    int                       overflow_reserve = 8;  // used only when no connection can be evicted
};

// Loopback HTTP/1.1 server for the MCP endpoint. One I/O thread runs every socket asynchronously
// with deadlines. Each complete request is handled on a thread of its own, because a handler may
// block for a tool's whole reply budget; a connection carries one request at a time, so handler
// threads never outnumber connections and a blocked handler cannot delay another connection.
class McpHttpServer
{
public:
    using Handler = std::function<PostReply(const PostRequest&)>;

    McpHttpServer(std::string token, Handler handler, HttpLimits limits = {});
    ~McpHttpServer();
    McpHttpServer(const McpHttpServer&)            = delete;
    McpHttpServer& operator=(const McpHttpServer&) = delete;

    // Binds 127.0.0.1:port (0 picks a free port) and starts serving.
    bool start(uint16_t port, std::string& error);
    // Stops accepting, waits (up to stop_timeout) for running handlers, then closes every connection.
    // The owner makes handlers return first (see UiInvoker::shutdown); a handler that still has not
    // returned keeps the server's internals alive until it does.
    void stop();
    bool running() const;
    uint16_t port() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

}}} // namespace Slic3r::GUI::Mcp
