#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "Router.hpp"

namespace Slic3r {
namespace Server {

// Synchronous beast HTTP server using one thread per accepted connection.
//
// Design rationale — sync / thread-per-connection:
//   Beast supports both synchronous and async (asio-based) session styles.
//   The async approach is more efficient for large concurrent loads, but
//   adds significant complexity.  Because orca-server is constrained to
//   one worker by default (slicing is not proven thread-safe), concurrency
//   is already limited.  A simple synchronous acceptor loop with one
//   dedicated thread per connection is therefore chosen:
//     - Mirrors the WebSocketClient.hpp approach in this codebase
//       (synchronous ioc_, resolver_, ws_ calls).
//     - Easier to audit for correctness.
//     - Each connection thread blocks on its own socket, so it does not
//       compete with the job worker threads.
//
// stop() closes the acceptor socket, which unblocks the synchronous accept()
// call in run() with operation_aborted.  In-flight connection threads are
// left to finish their current request naturally.
class HttpServer {
public:
    // addr          — bind address (e.g. "0.0.0.0" or "127.0.0.1")
    // port          — TCP port to listen on
    // authenticator — injected auth strategy, forwarded to Router (non-null)
    // ctx           — server-wide config (datadir/resources), forwarded to Router
    // queue         — job queue reference; must outlive HttpServer
    HttpServer(std::string addr, unsigned short port,
               std::shared_ptr<IAuthenticator> authenticator,
               ServerContext ctx, JobQueue &queue);

    ~HttpServer();

    // Block until stop() is called.
    void run();

    // Signal the acceptor to stop accepting new connections.
    // Thread-safe; may be called from a signal handler thread.
    void stop();

private:
    std::string               m_addr;
    unsigned short            m_port;
    Router                    m_router;
    std::atomic<bool>         m_running;

    boost::asio::io_context           m_ioc;
    // Stored as a unique_ptr so it can be constructed inside run() (after
    // the io_context is live) and closed from stop() on a different thread.
    std::unique_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;

    // Handle a single HTTP connection from a freshly accepted socket.
    // Runs on a dedicated thread for the duration of the connection.
    void handle_connection(boost::asio::ip::tcp::socket sock);
};

} // namespace Server
} // namespace Slic3r
