#include "McpHttpServer.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = net::ip::tcp;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

// "http://localhost:3000" or "https://127.0.0.1" — any port, loopback host names only.
bool is_loopback_origin(const std::string& origin)
{
    const std::string o = lower(origin);
    for (const char* scheme : {"http://", "https://"}) {
        const std::string s(scheme);
        if (o.compare(0, s.size(), s) != 0)
            continue;
        std::string rest = o.substr(s.size());
        const auto  colon = rest.find(':');
        const std::string host = rest.substr(0, colon);
        if (host != "localhost" && host != "127.0.0.1")
            return false;
        if (colon == std::string::npos)
            return true;
        const std::string port = rest.substr(colon + 1);
        return !port.empty() && port.size() <= 5 && std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
    }
    return false;
}

bool constant_time_equal(const std::string& a, const std::string& b)
{
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        diff |= (unsigned char) ((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}

std::string error_json(const std::string& message)
{
    return nlohmann::json{{"jsonrpc", "2.0"}, {"error", {{"code", -32600}, {"message", message}}}}.dump();
}

} // namespace

HttpCheck validate_request(const HttpRequestView& r, uint16_t port, const std::string& token, std::size_t max_body)
{
    const std::string path = r.target.substr(0, r.target.find('?'));
    if (path != "/mcp")
        return {404, "Not found"};
    if (r.method != "POST")
        return {405, "Only POST is supported on this endpoint"};

    const std::string host = lower(trim(r.host));
    const std::string p    = std::to_string(port);
    if (host != "127.0.0.1:" + p && host != "localhost:" + p)
        return {403, "Host header must name this loopback endpoint"};
    if (!r.origin.empty() && !is_loopback_origin(trim(r.origin)))
        return {403, "Origin not allowed"};

    const std::string auth = trim(r.authorization);
    if (auth.size() < 7 || lower(auth.substr(0, 7)) != "bearer " || !constant_time_equal(trim(auth.substr(7)), token))
        return {401, "Missing or invalid bearer token"};

    const std::string type = lower(trim(r.content_type.substr(0, r.content_type.find(';'))));
    if (type != "application/json")
        return {415, "Content-Type must be application/json"};
    if (r.body_size > max_body)
        return {413, "Request body too large"};
    return {};
}

// ---- implementation ---------------------------------------------------------------------------

struct McpHttpServer::Impl : std::enable_shared_from_this<McpHttpServer::Impl>
{
    struct Session;

    std::string token;
    Handler     handler;
    HttpLimits  limits;

    net::io_context ioc{1};
    tcp::acceptor   acceptor{ioc};
    std::thread     io_thread;
    uint16_t        bound_port = 0;
    std::atomic<bool> running{false};

    // Live sessions, touched only on the I/O thread.
    std::set<std::shared_ptr<Session>> sessions;

    // Handler threads, one per in-flight request.
    std::mutex              requests_mutex;
    std::condition_variable requests_cv;
    int                     active_requests    = 0;
    bool                    accepting_requests = true;

    Impl(std::string t, Handler h, HttpLimits l) : token(std::move(t)), handler(std::move(h)), limits(l) {}

    // Runs `job` on a new detached thread that keeps this object alive until it returns.
    void run_request(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(requests_mutex);
            if (!accepting_requests)
                return;
            ++active_requests;
        }
        std::thread([self = shared_from_this(), job = std::move(job)] {
            job();
            {
                std::lock_guard<std::mutex> lock(self->requests_mutex);
                --self->active_requests;
            }
            self->requests_cv.notify_all();
        }).detach();
    }

    void do_accept();
};

struct McpHttpServer::Impl::Session : std::enable_shared_from_this<McpHttpServer::Impl::Session>
{
    Impl*                                                    server; // outlives every session (see stop())
    beast::tcp_stream                                        stream;
    beast::flat_buffer                                       buffer;
    std::unique_ptr<http::request_parser<http::string_body>> parser;
    http::response<http::string_body>                        response;
    bool                                                     first     = true;
    bool                                                     in_flight = false; // a request is being handled or answered
    std::chrono::steady_clock::time_point                    idle_since = std::chrono::steady_clock::now();

    Session(Impl* s, tcp::socket&& socket) : server(s), stream(std::move(socket)) {}

    void start() { do_read(); }

    void do_read()
    {
        in_flight  = false;
        idle_since = std::chrono::steady_clock::now();
        parser = std::make_unique<http::request_parser<http::string_body>>();
        parser->body_limit(server->limits.max_body);
        parser->header_limit(64 * 1024);
        stream.expires_after(first ? server->limits.read_timeout : server->limits.idle_timeout);
        first = false;
        http::async_read(stream, buffer, *parser,
                         [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    void on_read(beast::error_code ec)
    {
        in_flight = true;
        if (ec == http::error::body_limit) {
            reply(413, error_json("Request body too large"), false);
            return;
        }
        if (ec) {
            close();
            return;
        }
        const auto& req = parser->get();
        HttpRequestView view;
        view.method        = std::string(req.method_string());
        view.target        = std::string(req.target());
        view.host          = std::string(req[http::field::host]);
        view.origin        = std::string(req[http::field::origin]);
        view.content_type  = std::string(req[http::field::content_type]);
        view.authorization = std::string(req[http::field::authorization]);
        view.body_size     = req.body().size();

        const HttpCheck check = validate_request(view, server->bound_port, server->token, server->limits.max_body);
        if (check.status != 0) {
            reply(check.status, error_json(check.message), req.keep_alive());
            return;
        }

        PostRequest post;
        post.body             = req.body();
        post.protocol_version = std::string(req["MCP-Protocol-Version"]);
        post.method_header    = std::string(req["Mcp-Method"]);
        post.name_header      = std::string(req["Mcp-Name"]);
        const bool keep_alive = req.keep_alive();

        // A handler can block for a tool's whole reply budget; nothing may time the socket out
        // meanwhile.
        stream.expires_never();
        server->run_request([self = shared_from_this(), post = std::move(post), keep_alive]() {
            PostReply out;
            try {
                out = self->server->handler(post);
            } catch (const std::exception& e) {
                out = {500, error_json(e.what())};
            }
            net::post(self->stream.get_executor(), [self, out = std::move(out), keep_alive]() mutable {
                self->reply(out.status, std::move(out.body), keep_alive);
            });
        });
    }

    void reply(int status, std::string body, bool keep_alive)
    {
        response = {};
        response.version(11);
        response.result(http::status(status));
        response.set(http::field::server, "OrcaSlicer-MCP");
        response.set(http::field::cache_control, "no-store");
        if (status == 405)
            response.set(http::field::allow, "POST");
        if (status == 401)
            response.set(http::field::www_authenticate, "Bearer");
        if (!body.empty())
            response.set(http::field::content_type, "application/json");
        response.body() = std::move(body);
        response.keep_alive(keep_alive && status < 500 && status != 413);
        response.prepare_payload();
        stream.expires_after(server->limits.write_timeout);
        http::async_write(stream, response, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec || !self->response.keep_alive()) {
                self->close();
                return;
            }
            self->do_read();
        });
    }

    void close()
    {
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream.close();
        server->sessions.erase(shared_from_this());
    }
};

void McpHttpServer::Impl::do_accept()
{
    acceptor.async_accept(net::make_strand(ioc), [self = this](beast::error_code ec, tcp::socket socket) {
        if (!self->acceptor.is_open())
            return;
        if (!ec) {
            // At the cap, make room by closing the connection that has waited longest on its client
            // (idle between requests, or still sending one); a dispatched request is never cut off.
            if (int(self->sessions.size()) >= self->limits.max_connections) {
                std::shared_ptr<Session> oldest;
                for (const auto& s : self->sessions)
                    if (!s->in_flight && (!oldest || s->idle_since < oldest->idle_since))
                        oldest = s;
                if (oldest)
                    oldest->close();
            }
            // With nothing evictable (every connection has a dispatched, authenticated request) the
            // overflow reserve absorbs the short moments until one of them returns.
            if (int(self->sessions.size()) >= self->limits.max_connections + self->limits.overflow_reserve) {
                beast::error_code ignored;
                socket.shutdown(tcp::socket::shutdown_both, ignored);
                socket.close(ignored);
            } else {
                // Only the I/O thread runs this io_context, so the session set needs no lock.
                auto session = std::make_shared<Session>(self, std::move(socket));
                self->sessions.insert(session);
                session->start();
            }
        }
        self->do_accept();
    });
}

McpHttpServer::McpHttpServer(std::string token, Handler handler, HttpLimits limits)
    : m_impl(std::make_shared<Impl>(std::move(token), std::move(handler), limits))
{}

McpHttpServer::~McpHttpServer() { stop(); }

bool McpHttpServer::start(uint16_t port, std::string& error)
{
    if (m_impl->running)
        return true;
    try {
        const tcp::endpoint endpoint(net::ip::address_v4::loopback(), port);
        m_impl->acceptor.open(endpoint.protocol());
#ifndef _WIN32
        // Lets a restarted server bind while its old connections sit in TIME_WAIT. POSIX still
        // refuses a second listener on the port; Windows' SO_REUSEADDR would not, so it stays off.
        m_impl->acceptor.set_option(tcp::acceptor::reuse_address(true));
#endif
        m_impl->acceptor.bind(endpoint);
        m_impl->acceptor.listen(net::socket_base::max_listen_connections);
        m_impl->bound_port = m_impl->acceptor.local_endpoint().port();
    } catch (const std::exception& e) {
        error = e.what();
        beast::error_code ignored;
        m_impl->acceptor.close(ignored);
        return false;
    }
    m_impl->running = true;
    m_impl->do_accept();
    m_impl->io_thread = std::thread([impl = m_impl.get()] { impl->ioc.run(); });
    return true;
}

void McpHttpServer::stop()
{
    if (!m_impl || !m_impl->running.exchange(false))
        return;
    // Handlers return once the owner has woken them; give them a bounded time to do so. A handler
    // that has not returned holds a reference to the internals, so nothing it touches is freed.
    {
        std::unique_lock<std::mutex> lock(m_impl->requests_mutex);
        m_impl->accepting_requests = false;
        m_impl->requests_cv.wait_for(lock, m_impl->limits.stop_timeout, [this] { return m_impl->active_requests == 0; });
    }

    m_impl->ioc.stop();
    if (m_impl->io_thread.joinable())
        m_impl->io_thread.join();
    // No thread runs the context any more, so the sockets can be closed from here. Pending
    // handlers that still hold sessions are destroyed with the io_context.
    beast::error_code ignored;
    m_impl->acceptor.close(ignored);
    for (const auto& s : m_impl->sessions) {
        s->stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        s->stream.socket().close(ignored);
    }
    m_impl->sessions.clear();
}

bool McpHttpServer::running() const { return m_impl && m_impl->running; }

uint16_t McpHttpServer::port() const { return m_impl ? m_impl->bound_port : 0; }

}}} // namespace Slic3r::GUI::Mcp
