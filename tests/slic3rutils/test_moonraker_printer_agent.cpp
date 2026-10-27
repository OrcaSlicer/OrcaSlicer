#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

using namespace Slic3r;

namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;
using namespace std::chrono_literals;

// A self-signed certificate for 127.0.0.1, valid until 2126; the agent does not verify it.
const char* const TEST_CERTIFICATE = R"(-----BEGIN CERTIFICATE-----
MIIBfjCCASWgAwIBAgIUQ6GXkNGttqdwgzP8jnIbjZmbmsUwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MTAyNTIyMzQzNloYDzIxMjYxMDAx
MjIzNDM2WjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAAQzyTk4fzS67d2QTOgff5VZdiFu6zfRKL1j1XrmZRdSNSQ+A/ecb+RA
wYLxO+ImIhC9nfFDz83gN6HMdIDhKZqvo1MwUTAdBgNVHQ4EFgQUqLCEdBHexenh
R0myp07v6RmToacwHwYDVR0jBBgwFoAUqLCEdBHexenhR0myp07v6RmToacwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiBtuziYmXx6OICr8OZ/ljgW
206TTprzJEsote+V0YwmawIgCTzg5n5sn/JH8DSUh7udfljYTtgIwlfqiqLZ9Y79
Xz8=
-----END CERTIFICATE-----
)";
const char* const TEST_PRIVATE_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgvrE3Z+AMBnG/JEQl
PP9feoILjFkSRm5XAYES7mSg6JWhRANCAAQzyTk4fzS67d2QTOgff5VZdiFu6zfR
KL1j1XrmZRdSNSQ+A/ecb+RAwYLxO+ImIhC9nfFDz83gN6HMdIDhKZqv
-----END PRIVATE KEY-----
)";

class DiscoveryAgent : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::MoonrakerPrinterAgent;
    using MoonrakerPrinterAgent::device_info;
    mutable unsigned fetch_count = 0;

protected:
    bool fetch_device_info(
        const std::string&, const std::string&, MoonrakerDeviceInfo& info, std::string&, const std::function<bool()>&) const override
    {
        ++fetch_count;
        info.dev_name = "Test printer";
        return true;
    }
};

// Connects without the app's printer preset, and counts the device info queries that have returned.
class ConnectAgent : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::MoonrakerPrinterAgent;
    std::shared_ptr<std::atomic<int>> fetches_returned;

protected:
    bool init_device_info(
        const std::string& dev_id, const std::string& dev_ip, const std::string&, const std::string&, bool, const std::string& port) override
    {
        device_info          = MoonrakerDeviceInfo{};
        device_info.dev_id   = dev_id;
        device_info.base_url = "http://" + dev_ip + ":" + port;
        return true;
    }

    bool fetch_device_info(const std::string&           base_url,
                           const std::string&           api_key,
                           MoonrakerDeviceInfo&         info,
                           std::string&                 error,
                           const std::function<bool()>& cancelled) const override
    {
        // Copied first, since the base destructor that waits for this query runs after this class's members are gone
        const std::shared_ptr<std::atomic<int>> counter = fetches_returned;
        const bool fetched = MoonrakerPrinterAgent::fetch_device_info(base_url, api_key, info, error, cancelled);
        if (counter)
            ++*counter;
        return fetched;
    }
};

class StreamAgent : public ConnectAgent
{
public:
    using ConnectAgent::ConnectAgent;
    using MoonrakerPrinterAgent::start_status_stream;
    using MoonrakerPrinterAgent::stop_status_stream;
    using MoonrakerPrinterAgent::ws_timings;

protected:
    bool fetch_device_info(
        const std::string&, const std::string&, MoonrakerDeviceInfo& info, std::string&, const std::function<bool()>&) const override
    {
        info.dev_name = "Test printer";
        return true;
    }
};

enum class Behavior {
    SilentBeforeHandshake,     // Accepts the connection and never answers
    Silent,                    // Completes the upgrade, then never reads or sends
    AnswersPings,              // Reports a status, then answers pings and sends nothing else
    AnswersPingsWithoutStatus, // Answers pings but never reports a status
    Chatters,                  // Reports a status, then answers pings and sends notifications that carry no status
    HangsUp,                   // Reports a status, then closes the websocket
    HangsUpWithoutAnswer,      // Closes the websocket without sending anything
    IgnoresClose,              // Reports Klippy disconnected, then never answers the close
    StallsObjectList,          // Never answers the stream's object list request
    StallsHttp,                // Never answers HTTP requests until released, then hangs up on them
};

class FakePrinter
{
public:
    explicit FakePrinter(Behavior behavior, bool tls = false) : m_behavior(behavior), m_tls(tls)
    {
        m_tls_context.use_certificate_chain(net::buffer(std::string_view(TEST_CERTIFICATE)));
        m_tls_context.use_private_key(net::buffer(std::string_view(TEST_PRIVATE_KEY)), ssl::context::pem);
        m_accept_thread = std::thread([this] { accept_loop(); });
    }

    ~FakePrinter()
    {
        release();
        m_stopping = true;
        // Wakes the blocking accept.
        tcp::socket wake{m_accept_ioc};
        beast::error_code ec;
        wake.connect(m_acceptor.local_endpoint(), ec);
        m_accept_thread.join();
        for (std::thread& connection : m_connections)
            connection.join();
    }

    std::string host() const { return "127.0.0.1"; }
    std::string port() const { return std::to_string(m_acceptor.local_endpoint().port()); }
    std::string base_url() const { return (m_tls ? "https://" : "http://") + host() + ":" + port(); }

    // A connection is ready once it reaches the state its behavior waits in.
    bool wait_for_ready_connections(int count, std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&] { return m_ready >= count; });
    }

    bool wait_for_http_requests(int count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_http_requests >= count; });
    }

    // Waits until the printer, once released, has hung up on `count` stalled HTTP requests.
    bool wait_for_http_hang_ups(int count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_http_hung_up >= count; });
    }

    int close_frames()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_close_frames;
    }

    // The printer answers a close frame before its own read completes, so the agent's stop can return first.
    bool wait_for_close_frames(int count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_close_frames >= count; });
    }

    // Hangs up every connection that is waiting.
    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    void accept_loop()
    {
        for (;;) {
            // Each connection gets its own io_context, so a connection that runs one never runs another's handlers.
            auto ioc = std::make_unique<net::io_context>();
            tcp::socket socket{*ioc};
            beast::error_code ec;
            m_acceptor.accept(socket, ec);
            if (ec || m_stopping)
                return;
            m_connections.emplace_back(
                [this, ioc = std::move(ioc), socket = std::move(socket)]() mutable { serve(*ioc, std::move(socket)); });
        }
    }

    void serve(net::io_context& ioc, tcp::socket socket)
    {
        if (m_behavior == Behavior::SilentBeforeHandshake) {
            mark_ready();
            wait_for_release();
            return;
        }
        if (!m_tls) {
            serve_stream(ioc, std::move(socket));
            return;
        }
        ssl::stream<tcp::socket> stream{std::move(socket), m_tls_context};
        beast::error_code ec;
        stream.handshake(ssl::stream_base::server, ec);
        if (!ec)
            serve_stream(ioc, std::move(stream));
    }

    template<class Stream> void serve_stream(net::io_context& ioc, Stream stream)
    {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code ec;
        http::read(stream, buffer, request, ec);
        if (ec)
            return;
        // The agent also queries the printer's status and object list over HTTP.
        if (!websocket::is_upgrade(request)) {
            serve_http(stream, request);
            return;
        }

        websocket::stream<Stream> ws{std::move(stream)};
        ws.accept(request, ec);
        // Without its object list the stream never sends the subscribe request.
        const int requests = m_behavior == Behavior::StallsObjectList ? 1 : 2;
        for (int i = 0; i < requests && !ec; ++i) {
            beast::flat_buffer message;
            ws.read(message, ec);
        }
        if (!ec && (m_behavior == Behavior::AnswersPings || m_behavior == Behavior::Chatters || m_behavior == Behavior::HangsUp)) {
            const std::string status =
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"status", {{"print_stats", {{"state", "standby"}}}}}}}}.dump();
            ws.write(net::buffer(status), ec);
        }
        if (ec)
            return;

        switch (m_behavior) {
        case Behavior::AnswersPings:
        case Behavior::AnswersPingsWithoutStatus:
        case Behavior::Chatters:
            mark_ready();
            answer_pings(ioc, ws, m_behavior == Behavior::Chatters);
            break;
        case Behavior::HangsUp:
        case Behavior::HangsUpWithoutAnswer:
            ws.close(websocket::close_code::normal, ec);
            mark_ready();
            break;
        case Behavior::IgnoresClose: {
            const std::string notification = nlohmann::json{{"jsonrpc", "2.0"}, {"method", "notify_klippy_disconnected"}}.dump();
            ws.write(net::buffer(notification), ec);
            mark_ready();
            wait_for_release();
            break;
        }
        case Behavior::StallsObjectList: answer_pings(ioc, ws, false); break;
        default: mark_ready(); wait_for_release();
        }
    }

    template<class Stream> void serve_http(Stream& stream, const http::request<http::string_body>& request)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_http_requests;
            m_changed.notify_all();
        }
        if (m_behavior == Behavior::StallsObjectList) {
            mark_ready();
            wait_for_release();
            return;
        }
        if (m_behavior == Behavior::StallsHttp) {
            wait_for_release();
            beast::error_code ec;
            beast::get_lowest_layer(stream).close(ec);
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_http_hung_up;
            m_changed.notify_all();
            return;
        }
        http::response<http::string_body> response{http::status::not_found, request.version()};
        response.prepare_payload();
        beast::error_code ec;
        http::write(stream, response, ec);
    }

    // Keeps a read pending, which answers pings, until released or closed, and with `chatter` also sends a notification every 50 ms.
    template<class WebSocket> void answer_pings(net::io_context& ioc, WebSocket& ws, bool chatter)
    {
        const std::string notification = nlohmann::json{{"jsonrpc", "2.0"},
                                                        {"method", "notify_proc_stat_update"},
                                                        {"params", nlohmann::json::array({nlohmann::json::object()})}}
                                             .dump();
        beast::flat_buffer message;
        bool reading                    = true;
        bool writing                    = false;
        std::function<void()> read_next = [&] {
            ws.async_read(message, [&](beast::error_code ec, std::size_t) {
                message.clear();
                if (ec == websocket::error::closed) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_close_frames;
                    m_changed.notify_all();
                }
                if (ec)
                    reading = false;
                else
                    read_next();
            });
        };
        read_next();

        auto next_notification = std::chrono::steady_clock::now();
        while (reading && !wait_for_release(10ms)) {
            if (chatter && !writing && std::chrono::steady_clock::now() >= next_notification) {
                writing = true;
                next_notification += 50ms;
                ws.async_write(net::buffer(notification), [&](beast::error_code, std::size_t) { writing = false; });
            }
            ioc.restart();
            ioc.run_for(20ms);
        }
        beast::error_code ignored;
        beast::get_lowest_layer(ws).close(ignored);
        while (reading || writing) {
            ioc.restart();
            ioc.run_for(20ms);
        }
    }

    void mark_ready()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_ready;
        m_changed.notify_all();
    }

    void wait_for_release()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_released; });
    }

    bool wait_for_release(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [this] { return m_released; });
    }

    const Behavior m_behavior;
    const bool m_tls;
    ssl::context m_tls_context{ssl::context::tls_server};
    net::io_context m_accept_ioc;
    tcp::acceptor m_acceptor{m_accept_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool> m_stopping{false};
    std::thread m_accept_thread;
    std::vector<std::thread> m_connections;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_ready         = 0;
    int m_http_requests = 0;
    int m_http_hung_up  = 0;
    int m_close_frames  = 0;
    bool m_released     = false;
};

// Records when the agent dispatches a status report.
class Reports
{
public:
    void attach(StreamAgent& agent)
    {
        agent.set_on_local_message_fn([this](std::string, std::string) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_times.push_back(std::chrono::steady_clock::now());
            m_changed.notify_all();
        });
    }

    bool wait_for(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_times.size() >= count; });
    }

    std::vector<std::chrono::steady_clock::time_point> times()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_times;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::chrono::steady_clock::time_point> m_times;
};

// A printer and a stream agent connected to it.
struct StreamFixture
{
    explicit StreamFixture(Behavior behavior, bool tls = false) : printer(behavior, tls) {}

    // Runs before the members are destroyed, so the agent's destructor never joins a stream the printer still holds open.
    ~StreamFixture() { printer.release(); }

    void start() { agent.start_status_stream("printer", printer.base_url(), ""); }

    // Stops the stream, giving it `limit` to return before the printer hangs up on it.
    bool stops_within(std::chrono::milliseconds limit)
    {
        auto stopped       = std::async(std::launch::async, [this] { agent.stop_status_stream(); });
        const bool in_time = stopped.wait_for(limit) == std::future_status::ready;
        printer.release();
        stopped.wait();
        return in_time;
    }

    FakePrinter printer;
    StreamAgent agent{""};
};

PrinterConnectionParams connection_to(const FakePrinter& printer)
{
    PrinterConnectionParams params;
    params.dev_id = "printer";
    params.host   = printer.host();
    params.port   = printer.port();
    return params;
}

// Destroys the agent, giving it `limit` to return.
template<class Agent> bool destroys_within(std::unique_ptr<Agent>& agent, std::chrono::milliseconds limit)
{
    auto destroyed = std::async(std::launch::async, [&] { agent.reset(); });
    return destroyed.wait_for(limit) == std::future_status::ready;
}

} // namespace

TEST_CASE("Moonraker discovery waits for a complete device identity", "[MoonrakerPrinterAgent][Discovery]")
{
    DiscoveryAgent agent("");
    unsigned announcements = 0;
    nlohmann::json announcement;
    agent.set_on_ssdp_msg_fn([&](std::string payload) {
        ++announcements;
        announcement = nlohmann::json::parse(payload);
    });

    agent.start_discovery(true, false);
    REQUIRE(announcements == 0);
    REQUIRE(agent.fetch_count == 0);

    agent.device_info.dev_id = "127.0.0.1:7125";
    agent.device_info.dev_ip = agent.device_info.dev_id;
    agent.device_info.base_url = "http://127.0.0.1:7125";
    agent.device_info.model_name = "Test model";
    agent.start_discovery(true, false);
    REQUIRE(announcements == 1);
    REQUIRE(agent.fetch_count == 0);
    REQUIRE(announcement.at("dev_id") == agent.device_info.dev_id);
    REQUIRE(announcement.at("dev_ip") == agent.device_info.dev_ip);
    REQUIRE(announcement.at("dev_name") == "Test model");
}

TEST_CASE("Stopping the status stream gives a silent printer no more than its timeouts", "[MoonrakerPrinterAgent][StatusStream]")
{
    // An open websocket gets the 200 ms close timeout, and a stalled object list request up to 1 s of curl progress to see the stop.
    const auto [behavior, limit] = GENERATE(table<Behavior, std::chrono::milliseconds>({
        {Behavior::SilentBeforeHandshake, 500ms},
        {Behavior::Silent, 1000ms},
        {Behavior::StallsObjectList, 2000ms},
    }));
    CAPTURE(static_cast<int>(behavior));

    StreamFixture f(behavior);
    f.agent.ws_timings.close = 200ms;
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));

    CHECK(f.stops_within(limit));
}

TEST_CASE("Stopping the status stream sends the printer a close frame", "[MoonrakerPrinterAgent][StatusStream]")
{
    // A stop while the stream reads, and one while it waits on the object list request.
    const Behavior behavior = GENERATE(Behavior::AnswersPings, Behavior::StallsObjectList);
    CAPTURE(static_cast<int>(behavior));

    StreamFixture f(behavior);
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));

    CHECK(f.stops_within(3s));
    CHECK(f.printer.wait_for_close_frames(1));
    CHECK(f.printer.close_frames() == 1);
}

TEST_CASE("Stopping the status stream does not wait out the reconnect delay", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::HangsUp);
    f.agent.ws_timings.reconnect_delay = 10s;
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));
    // Lets the stream reach its reconnect wait.
    std::this_thread::sleep_for(200ms);

    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream reconnects when the printer stops answering", "[MoonrakerPrinterAgent][StatusStream]")
{
    // An unanswered close ends at whichever of the handshake and close timeouts is shorter.
    const auto [behavior, handshake, close] = GENERATE(table<Behavior, std::chrono::milliseconds, std::chrono::milliseconds>({
        {Behavior::Silent, 300ms, 1000ms},
        {Behavior::IgnoresClose, 300ms, 1000ms},
        {Behavior::IgnoresClose, 1000ms, 200ms},
    }));
    CAPTURE(static_cast<int>(behavior), handshake.count(), close.count());

    StreamFixture f(behavior);
    f.agent.ws_timings.handshake       = handshake;
    f.agent.ws_timings.close           = close;
    f.agent.ws_timings.idle            = 400ms;
    f.agent.ws_timings.reconnect_delay = 20ms;
    f.start();

    CHECK(f.printer.wait_for_ready_connections(2));
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream reconnects at the shortest delay while the printer answers", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::HangsUp);
    f.agent.ws_timings.reconnect_delay = 50ms;
    f.start();

    // Twelve 50 ms waits; a backoff that kept doubling would need over 10 s.
    CHECK(f.printer.wait_for_ready_connections(12, 3s));
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream keeps retrying a printer that never answers", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::HangsUpWithoutAnswer);
    f.agent.ws_timings.reconnect_delay = 1ms;
    f.start();

    // The backoff stops doubling at 32 ms, so 20 attempts take well under the wait.
    CHECK(f.printer.wait_for_ready_connections(20));
    CHECK(f.stops_within(3s));
}

TEST_CASE("A callback on the stream thread can stop the status stream", "[MoonrakerPrinterAgent][StatusStream]")
{
    std::promise<void> stopped;
    std::once_flag stopped_once;
    StreamFixture f(Behavior::AnswersPings);
    f.agent.set_on_local_message_fn([&](std::string, std::string) {
        std::call_once(stopped_once, [&] {
            f.agent.stop_status_stream();
            stopped.set_value();
        });
    });
    f.start();

    REQUIRE(stopped.get_future().wait_for(5s) == std::future_status::ready);
    CHECK(f.stops_within(3s));
}

TEST_CASE("A quiet printer that is still up keeps reporting its status", "[MoonrakerPrinterAgent][StatusStream]")
{
    Reports reports;
    StreamFixture f(Behavior::AnswersPings);
    f.agent.ws_timings.idle      = 400ms;
    f.agent.ws_timings.heartbeat = 300ms;
    reports.attach(f.agent);
    f.start();

    // The status the printer sent, then two heartbeats.
    REQUIRE(reports.wait_for(3));
    const auto times = reports.times();
    // The steady clock is compared in whole milliseconds, so allow 1 ms of rounding.
    CHECK(times[1] - times[0] >= 299ms);
    CHECK(times[2] - times[1] >= 299ms);
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream reports a printer's status over TLS", "[MoonrakerPrinterAgent][StatusStream]")
{
    Reports reports;
    StreamFixture f(Behavior::AnswersPings, true);
    reports.attach(f.agent);
    f.start();

    CHECK(reports.wait_for(1));
    CHECK(f.stops_within(3s));
    CHECK(f.printer.wait_for_close_frames(1));
}

TEST_CASE("The status stream sends no heartbeat while the printer has nothing new to report", "[MoonrakerPrinterAgent][StatusStream]")
{
    const Behavior behavior = GENERATE(Behavior::Chatters, Behavior::AnswersPingsWithoutStatus);
    CAPTURE(static_cast<int>(behavior));

    Reports reports;
    StreamFixture f(behavior);
    f.agent.ws_timings.idle      = 400ms;
    f.agent.ws_timings.heartbeat = 800ms;
    reports.attach(f.agent);
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));

    // The chattering printer's status, and nothing from the one that never sent a status.
    const std::size_t expected = behavior == Behavior::Chatters ? 1 : 0;
    std::this_thread::sleep_for(1500ms);
    CHECK(reports.times().size() == expected);
    CHECK(f.stops_within(3s));
}

TEST_CASE("A disconnect during the connection queries keeps the status stream from starting", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::StallsHttp);
    f.agent.connect_printer(connection_to(f.printer));
    REQUIRE(f.printer.wait_for_http_requests(1));

    f.agent.disconnect_printer();
    f.printer.release();
    // The query has returned once the printer hangs up on it, and a stream would connect right after.
    REQUIRE(f.printer.wait_for_http_hang_ups(1));
    CHECK_FALSE(f.printer.wait_for_ready_connections(1, 500ms));
}

TEST_CASE("Destroying the agent waits for the connection threads it superseded", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::StallsHttp);
    const auto fetches_returned = std::make_shared<std::atomic<int>>(0);
    auto agent                  = std::make_unique<ConnectAgent>("");
    agent->fetches_returned     = fetches_returned;

    agent->connect_printer(connection_to(printer));
    REQUIRE(printer.wait_for_http_requests(1));
    agent->connect_printer(connection_to(printer));
    REQUIRE(printer.wait_for_http_requests(2));
    agent->disconnect_printer();

    // The stalled queries see the change within 1 s of curl progress.
    CHECK(destroys_within(agent, 2s));
    CHECK(*fetches_returned == 2);
}

TEST_CASE("Destroying the agent stops a stalled status query", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::StallsHttp);
    auto agent = std::make_unique<StreamAgent>("");
    agent->connect_printer(connection_to(printer));
    REQUIRE(printer.wait_for_http_requests(1));

    // The stalled query sees the change within 1 s of curl progress.
    CHECK(destroys_within(agent, 2s));
}
