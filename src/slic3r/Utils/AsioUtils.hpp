#pragma once

#include <atomic>
#include <chrono>
#include <utility>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/tcp_stream.hpp>

namespace Slic3r {

// Runs one asynchronous operation to completion, because synchronous Beast calls ignore timeouts and cannot be aborted
template<class Initiate>
boost::beast::error_code run_until_done(boost::asio::io_context& ioc, const std::atomic<bool>& stop, Initiate&& initiate)
{
    // Returns early, since an abort posted before the operation starts has nothing to cancel
    if (stop.load()) {
        return boost::asio::error::operation_aborted;
    }
    boost::beast::error_code result;
    bool                     done = false;
    initiate([&](boost::beast::error_code ec, auto&&...) {
        result = ec;
        done   = true;
    });
    // A timed-out operation can stop the context by running it out of work, and a stopped context runs no handlers
    ioc.restart();
    while (!done && ioc.run_one() != 0) {} // run() would also wait for unrelated work, such as an armed websocket idle timer
    return done ? result : boost::beast::error_code{boost::asio::error::operation_aborted};
}

template<class Initiate>
boost::beast::error_code run_until_done(boost::asio::io_context& ioc, Initiate&& initiate)
{
    const std::atomic<bool> never_stop{false};
    return run_until_done(ioc, never_stop, std::forward<Initiate>(initiate));
}

// Connects to the first resolved address that accepts, giving each address its own deadline so a dead one cannot use up the next one's
template<class Endpoints>
boost::beast::error_code connect_until_done(boost::asio::io_context&  ioc,
                                            boost::beast::tcp_stream& stream,
                                            const Endpoints&          endpoints,
                                            std::chrono::milliseconds timeout,
                                            const std::atomic<bool>&  stop)
{
    boost::beast::error_code ec = boost::asio::error::host_not_found;
    for (const auto& entry : endpoints) {
        boost::beast::error_code ignored;
        stream.socket().close(ignored); // The socket stays open after a failed attempt
        stream.expires_after(timeout);
        ec = run_until_done(ioc, stop, [&](auto&& handler) { stream.async_connect(entry.endpoint(), handler); });
        if (!ec || stop.load()) {
            break;
        }
    }
    return ec;
}

template<class Endpoints>
boost::beast::error_code connect_until_done(boost::asio::io_context&  ioc,
                                            boost::beast::tcp_stream& stream,
                                            const Endpoints&          endpoints,
                                            std::chrono::milliseconds timeout)
{
    const std::atomic<bool> never_stop{false};
    return connect_until_done(ioc, stream, endpoints, timeout, never_stop);
}

} // namespace Slic3r
