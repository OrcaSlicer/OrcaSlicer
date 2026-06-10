#include "HttpServer.hpp"

#include <iostream>
#include <thread>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace Slic3r {
namespace Server {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

HttpServer::HttpServer(std::string addr, unsigned short port,
                       std::shared_ptr<IAuthenticator> authenticator,
                       ServerContext ctx, JobQueue &queue)
    : m_addr(std::move(addr))
    , m_port(port)
    , m_router(std::move(authenticator), std::move(ctx), queue)
    , m_running(false)
{}

HttpServer::~HttpServer()
{
    stop();
}

// ---------------------------------------------------------------------------
// run() — synchronous acceptor loop
// ---------------------------------------------------------------------------

void HttpServer::run()
{
    m_running.store(true, std::memory_order_relaxed);

    // Construct the acceptor here (inside run) so that stop() can close it.
    m_acceptor = std::make_unique<tcp::acceptor>(m_ioc);

    try {
        tcp::endpoint endpoint(net::ip::make_address(m_addr), m_port);
        m_acceptor->open(endpoint.protocol());
        m_acceptor->set_option(tcp::acceptor::reuse_address(true));
        m_acceptor->bind(endpoint);
        m_acceptor->listen(net::socket_base::max_listen_connections);
    } catch (const std::exception &ex) {
        std::cerr << "[orca-server] Failed to bind " << m_addr << ":" << m_port
                  << " — " << ex.what() << "\n";
        m_acceptor.reset();
        return;
    }

    std::cerr << "[orca-server] Listening on " << m_addr << ":" << m_port << "\n";

    while (m_running.load(std::memory_order_relaxed)) {
        boost::system::error_code ec;
        tcp::socket sock(m_ioc);
        m_acceptor->accept(sock, ec);

        if (ec) {
            // accept() returns operation_aborted when the acceptor is closed
            // from stop(); any other error is logged and we continue.
            if (ec == net::error::operation_aborted)
                break;
            if (!m_running.load(std::memory_order_relaxed))
                break; // stop() was called concurrently
            std::cerr << "[orca-server] accept error: " << ec.message() << "\n";
            continue;
        }

        // Detach a new thread to handle this connection.  The lambda captures
        // the socket by move.  Thread detach is acceptable here because:
        //   (a) connections are short-lived (single request/response),
        //   (b) each thread ends naturally after the response is sent.
        // For a production server, a bounded thread pool would be preferable.
        std::thread([this, s = std::move(sock)]() mutable {
            handle_connection(std::move(s));
        }).detach();
    }

    std::cerr << "[orca-server] Acceptor stopped.\n";
}

// ---------------------------------------------------------------------------
// stop()
//
// Closes the acceptor from the calling thread.  Closing the acceptor socket
// causes the synchronous accept() call in run() to return immediately with
// boost::asio::error::operation_aborted, which breaks the accept loop.
//
// Thread-safe: boost::asio acceptor::close() is safe to call from a thread
// other than the one running accept() when there is no async operation in
// flight (we use synchronous accept here).
// ---------------------------------------------------------------------------

void HttpServer::stop()
{
    if (!m_running.exchange(false, std::memory_order_relaxed))
        return; // Already stopped or never started.

    // Close the acceptor to unblock the synchronous accept() call.
    if (m_acceptor) {
        boost::system::error_code ec;
        m_acceptor->close(ec);
        // Ignore errors from close — the accept loop will see operation_aborted.
    }
}

// ---------------------------------------------------------------------------
// handle_connection() — read one request, write one response, honour keep-alive
// ---------------------------------------------------------------------------

void HttpServer::handle_connection(tcp::socket sock)
{
    beast::flat_buffer buffer;

    // Keep reading requests until the connection is closed or keep-alive ends.
    while (m_running.load(std::memory_order_relaxed)) {
        beast::error_code ec;

        // Read the request.
        http::request<http::string_body> req;
        http::read(sock, buffer, req, ec);

        if (ec == http::error::end_of_stream)
            break; // Client closed connection.
        if (ec) {
            // Log unexpected read errors at debug level; they are normal for
            // abruptly closed connections (e.g. client timeout).
            std::cerr << "[orca-server] read error: " << ec.message() << "\n";
            break;
        }

        // Dispatch through the router.
        auto res = m_router.dispatch(req);

        // Write the response.
        http::write(sock, res, ec);
        if (ec) {
            std::cerr << "[orca-server] write error: " << ec.message() << "\n";
            break;
        }

        // Check keep-alive.  If the response indicates close, stop.
        if (!res.keep_alive())
            break;
    }

    // Graceful TCP shutdown.
    boost::system::error_code ec;
    sock.shutdown(tcp::socket::shutdown_send, ec);
    // Ignore shutdown errors (client may have already closed).
}

} // namespace Server
} // namespace Slic3r
