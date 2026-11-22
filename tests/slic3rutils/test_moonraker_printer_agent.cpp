#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/string_type.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "test_utils.hpp"

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

// Fails its first `refusals` device info queries, as when macOS refuses the app's first local network connections.
class RefusedAgent : public StreamAgent
{
public:
    using StreamAgent::StreamAgent;
    int refusals = 0;
    mutable std::atomic<int> fetches{0};

protected:
    bool fetch_device_info(const std::string&           base_url,
                           const std::string&           api_key,
                           MoonrakerDeviceInfo&         info,
                           std::string&                 error,
                           const std::function<bool()>& cancelled) const override
    {
        if (++fetches <= refusals) {
            error = "Couldn't connect to server";
            return false;
        }
        return StreamAgent::fetch_device_info(base_url, api_key, info, error, cancelled);
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

    // Waits for a message the agent sent after subscribing that contains `text`.
    bool wait_for_ws_message(const std::string& text)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] {
            return std::any_of(m_ws_messages.begin(), m_ws_messages.end(),
                               [&](const std::string& message) { return message.find(text) != std::string::npos; });
        });
    }

    int close_frames()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_close_frames;
    }

    // The answer to the gcodes directory listing, which reports the free space in disk_usage. Null leaves disk_usage out.
    void set_gcodes_listing(nlohmann::json disk_usage)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_disk_usage = std::move(disk_usage);
    }

    // The multipart body of the last file upload.
    std::string upload()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_upload;
    }

    // The last G-code script sent to the printer.
    std::string script()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_script;
    }

    // Waits for the G-code script sent to the printer to be `script`.
    bool wait_for_script(const std::string& script)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_script == script; });
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
        if (request.target() == "/server/files/directory?path=gcodes") {
            nlohmann::json result = {{"dirs", nlohmann::json::array()}, {"files", nlohmann::json::array()}};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_disk_usage.is_null())
                    result["disk_usage"] = m_disk_usage;
            }
            response.result(http::status::ok);
            response.body() = nlohmann::json{{"result", result}}.dump();
        } else if (request.target() == "/server/files/upload") {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_upload = request.body();
            response.result(http::status::created);
            response.body() = "{}";
        } else if (request.target() == "/printer/gcode/script") {
            std::lock_guard<std::mutex> lock(m_mutex);
            const nlohmann::json body = nlohmann::json::parse(request.body(), nullptr, false);
            m_script                  = body.is_object() ? body.value("script", "") : "";
            m_changed.notify_all();
            response.result(http::status::ok);
            response.body() = R"({"result": "ok"})";
        }
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
        // Counted on arrival: the read completes only after the reply and the teardown, by which time the
        // agent's stop may have returned and the test's release closed this socket, failing the read instead.
        ws.control_callback([&](websocket::frame_type kind, beast::string_view) {
            if (kind == websocket::frame_type::close) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_close_frames;
            }
        });
        std::function<void()> read_next = [&] {
            ws.async_read(message, [&](beast::error_code ec, std::size_t) {
                if (!ec) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_ws_messages.push_back(beast::buffers_to_string(message.data()));
                    m_changed.notify_all();
                }
                message.clear();
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
    nlohmann::json m_disk_usage = {{"total", 8e9}, {"used", 2e9}, {"free", 6e9}};
    std::string m_upload;
    std::string m_script;
    std::vector<std::string> m_ws_messages;
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

// Uploads to a printer without connecting to it, and records the file it is told to print.
class UploadAgent : public MoonrakerPrinterAgent
{
public:
    explicit UploadAgent(const FakePrinter& printer) : MoonrakerPrinterAgent("") { device_info.base_url = printer.base_url(); }
    std::string printed;
    bool        takes_3mf = false;

protected:
    bool accepts_3mf() const override { return takes_3mf; }

    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override
    {
        printed = filename;
        return MoonrakerPrinterAgent::print_uploaded_file(params, filename);
    }
};

// Writes a .gcode.3mf holding one line of G-code for each of the given plate numbers.
void write_bundle(const std::string& path, const std::vector<std::string>& plates)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, path));
    for (const std::string& plate : plates) {
        const std::string gcode = "; plate " + plate + "\n";
        REQUIRE(mz_zip_writer_add_mem(&zip, ("Metadata/plate_" + plate + ".gcode").c_str(), gcode.data(), gcode.size(), MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    REQUIRE(close_zip_writer(&zip));
}

// Makes a fresh directory the system temp folder for its lifetime, so a test sees only what the agent leaves there.
class ScopedSystemTemp : public ScopedTemporaryDir
{
public:
    ScopedSystemTemp() : ScopedTemporaryDir("moonraker-temp")
    {
#ifdef _WIN32
        const std::string dir = boost::nowide::narrow(path().wstring());
#else
        const std::string dir = string();
#endif
        for (const char* name : {"TMP", "TEMP", "TMPDIR"}) {
            const char* value = boost::nowide::getenv(name);
            m_saved.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
            boost::nowide::setenv(name, dir.c_str(), 1);
        }
    }
    ~ScopedSystemTemp()
    {
        for (const auto& [name, value] : m_saved) {
            if (value)
                boost::nowide::setenv(name.c_str(), value->c_str(), 1);
            else
                boost::nowide::unsetenv(name.c_str());
        }
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> m_saved;
};

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
    agent.device_info.dev_ip = "127.0.0.1";
    agent.device_info.base_url = "http://127.0.0.1:7125";
    agent.device_info.model_name = "Test model";
    agent.start_discovery(true, false);
    REQUIRE(announcements == 1);
    REQUIRE(agent.fetch_count == 0);
    REQUIRE(announcement.at("dev_id") == agent.device_info.dev_id);
    REQUIRE(announcement.at("dev_ip") == "127.0.0.1:7125");
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

TEST_CASE("Status reports tell the send dialog whether the gcodes storage has free space", "[MoonrakerPrinterAgent][StatusStream]")
{
    auto [disk_usage, has_storage] = GENERATE(table<nlohmann::json, bool>({
        {{{"total", 8e9}, {"used", 2e9}, {"free", 6e9}}, true},
        {{{"total", 8e9}, {"used", 8e9}, {"free", 0}}, false},
        {nullptr, false},
    }));
    CAPTURE(disk_usage.dump());

    std::promise<std::string> first_report;
    std::once_flag reported;
    StreamFixture f(Behavior::AnswersPings);
    f.printer.set_gcodes_listing(disk_usage);
    f.agent.set_on_local_message_fn([&](std::string, std::string payload) {
        std::call_once(reported, [&] { first_report.set_value(payload); });
    });
    f.start();

    auto report = first_report.get_future();
    REQUIRE(report.wait_for(5s) == std::future_status::ready);
    const nlohmann::json print = nlohmann::json::parse(report.get()).at("print");
    // MachineObject reads the storage state from "sdcard" and then again from home_flag bits 8-9.
    CHECK(print.at("sdcard") == has_storage);
    CHECK(((print.at("home_flag").get<int>() >> 8) & 0x3) == (has_storage ? DevStorage::HAS_SDCARD_NORMAL : DevStorage::NO_SDCARD));
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream keeps re-reading the printer's free space", "[MoonrakerPrinterAgent][StatusStream]")
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<bool> storage_reports;
    // Chatters sends a message every 50 ms, which the stream re-reads the free space after
    StreamFixture f(Behavior::Chatters);
    f.agent.ws_timings.storage_check = 100ms;
    f.printer.set_gcodes_listing({{"total", 8e9}, {"used", 8e9}, {"free", 0}});
    f.agent.set_on_local_message_fn([&](std::string, std::string payload) {
        std::lock_guard<std::mutex> lock(mutex);
        storage_reports.push_back(nlohmann::json::parse(payload).at("print").at("sdcard").get<bool>());
        changed.notify_all();
    });
    const auto wait_for_storage = [&](bool has_storage) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return !storage_reports.empty() && storage_reports.back() == has_storage; });
    };
    f.start();
    REQUIRE(wait_for_storage(false));

    f.printer.set_gcodes_listing({{"total", 8e9}, {"used", 2e9}, {"free", 6e9}});
    REQUIRE(wait_for_storage(true));

    // A listing without disk usage cannot be checked, which counts as no storage
    f.printer.set_gcodes_listing(nullptr);
    CHECK(wait_for_storage(false));
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
    CHECK(f.printer.close_frames() == 1);
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

TEST_CASE("A connection the printer refuses at first is retried", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::AnswersPings);
    RefusedAgent agent("");
    agent.refusals                   = 2;
    agent.ws_timings.reconnect_delay = 10ms;
    agent.connect_printer(connection_to(printer));

    // The status stream starts only once a device info query succeeds.
    CHECK(printer.wait_for_ready_connections(1));
    CHECK(agent.fetches == 3);
    printer.release();
}

TEST_CASE("Destroying the agent does not wait out a connection retry's delay", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::AnswersPings);
    auto agent                        = std::make_unique<RefusedAgent>("");
    agent->refusals                   = 1;
    agent->ws_timings.reconnect_delay = 10s;
    agent->connect_printer(connection_to(printer));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (agent->fetches == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(agent->fetches == 1);

    CHECK(destroys_within(agent, 1s));
}

TEST_CASE("A .gcode.3mf goes to the printer as the printed plate's G-code unless the printer takes 3MF", "[MoonrakerPrinterAgent][Upload]")
{
    const bool takes_3mf = GENERATE(false, true);
    CAPTURE(takes_3mf);
    const std::string name = takes_3mf ? "Cube.gcode.3mf" : "Cube.gcode";

    ScopedTemporaryDir dir("moonraker-upload");
    const std::string bundle = (dir.path() / ".1234.0.3mf").string();
    write_bundle(bundle, {"1", "2"});
    ScopedSystemTemp temp;
    REQUIRE(boost::filesystem::equivalent(boost::filesystem::temp_directory_path(), temp.path()));

    FakePrinter printer(Behavior::AnswersPings);
    UploadAgent agent(printer);
    agent.takes_3mf = takes_3mf;
    PrintParams params{};
    params.filename    = bundle;
    params.plate_index = 2;

    SECTION("Print plate")
    {
        params.project_name = "Cube";
        REQUIRE(agent.start_local_print(params, nullptr, nullptr) == BAMBU_NETWORK_SUCCESS);
        CHECK(agent.printed == name);
    }
    SECTION("Send to printer")
    {
        params.project_name = "Cube.gcode.3mf";
        REQUIRE(agent.start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr) == BAMBU_NETWORK_SUCCESS);
        CHECK(agent.printed.empty());
    }

    const std::string upload = printer.upload();
    CHECK(upload.find("filename=\"" + name + "\"") != std::string::npos);
    if (takes_3mf) {
        CHECK(upload.find("PK\x03\x04") != std::string::npos);
    } else {
        CHECK(upload.find("; plate 2") != std::string::npos);
        CHECK(upload.find("; plate 1") == std::string::npos);
    }
    // Neither the source folder nor the temp folder keeps an extracted copy
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path()), boost::filesystem::directory_iterator()) == 1);
    CHECK(boost::filesystem::is_empty(temp.path()));
}

TEST_CASE("A bundle without the printed plate's G-code is uploaded only when it holds a single plate", "[MoonrakerPrinterAgent][Upload]")
{
    auto [plates, uploaded] = GENERATE(table<std::vector<std::string>, bool>({
        {{"1"}, true},
        {{"1", "3"}, false},
    }));
    CAPTURE(plates.size());

    ScopedTemporaryDir dir("moonraker-upload");
    const std::string bundle = (dir.path() / ".1234.0.3mf").string();
    write_bundle(bundle, plates);

    FakePrinter printer(Behavior::AnswersPings);
    UploadAgent agent(printer);
    PrintParams params{};
    params.filename     = bundle;
    params.plate_index  = 2;
    params.project_name = "Cube";

    const int result = agent.start_local_print(params, nullptr, nullptr);
    CHECK((result == BAMBU_NETWORK_SUCCESS) == uploaded);
    CHECK((printer.upload().find("; plate 1") != std::string::npos) == uploaded);
}

TEST_CASE("The job name reaches the print start as a file name the printer can store and list", "[MoonrakerPrinterAgent][Upload]")
{
    // A long name is cut to fit 255 bytes with room for .gcode.3mf, at a whole 2-byte character
    std::string long_name;
    for (int i = 0; i < 150; ++i)
        long_name += "\xC3\xA9";
    const std::size_t max_stem      = 255 - std::strlen(".gcode.3mf");
    const std::string cut_long_name = long_name.substr(0, max_stem - max_stem % 2);

    auto [job_name, file_name] = GENERATE_COPY(table<std::string, std::string>({
        {"Cable clip #2; v\"3\"*", "Cable clip _2_ v_3__.gcode"},
        {"Bracket 1/2", "Bracket 1_2.gcode"},
        {"A: frame", "A: frame.gcode"},
        {"Bob's", "Bob's.gcode"},
        {"Bob's \"best\" frame (v2) [final] {\xC3\xA9} <x> |y| ?!&%$@=+,~`", "Bob's _best_ frame (v2) [final] {\xC3\xA9} <x> |y| ?!&%$@=+,~`.gcode"},
        {"x\nM104 S300", "x_M104 S300.gcode"},
        {" .Cube", "Cube.gcode"},
        {"", "1234.0.gcode"},
        {long_name, cut_long_name + ".gcode"},
    }));
    CAPTURE(job_name);

    ScopedTemporaryDir dir("moonraker-upload");
    const std::string bundle = (dir.path() / ".1234.0.3mf").string();
    write_bundle(bundle, {"1"});

    FakePrinter printer(Behavior::AnswersPings);
    UploadAgent agent(printer);
    PrintParams params{};
    params.filename     = bundle;
    params.plate_index  = 1;
    params.project_name = job_name;

    REQUIRE(agent.start_local_print(params, nullptr, nullptr) == BAMBU_NETWORK_SUCCESS);
    CHECK(printer.upload().find("filename=\"" + file_name + "\"") != std::string::npos);
    CHECK(printer.script() == "SDCARD_PRINT_FILE FILENAME=\"" + file_name + "\"");
}

TEST_CASE("A file that is not a 3MF is uploaded as it is, named after the job when there is one", "[MoonrakerPrinterAgent][Upload]")
{
    auto [job_name, file_name] = GENERATE(table<std::string, std::string>({
        {"Cube", "Cube.gcode"},
        {"", "part.gcode"},
    }));
    CAPTURE(job_name);

    ScopedTemporaryDir dir("moonraker-upload");
    const boost::filesystem::path gcode = dir.path() / "part.gcode";
    boost::filesystem::ofstream(gcode, std::ios::binary) << "; plain G-code\n";

    FakePrinter printer(Behavior::AnswersPings);
    UploadAgent agent(printer);
    PrintParams params{};
    params.filename     = gcode.string();
    params.project_name = job_name;

    SECTION("Print plate")
    {
        REQUIRE(agent.start_local_print(params, nullptr, nullptr) == BAMBU_NETWORK_SUCCESS);
        CHECK(agent.printed == file_name);
    }
    SECTION("Send to printer")
    {
        REQUIRE(agent.start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr) == BAMBU_NETWORK_SUCCESS);
    }

    const std::string upload = printer.upload();
    CHECK(upload.find("filename=\"" + file_name + "\"") != std::string::npos);
    CHECK(upload.find("; plain G-code") != std::string::npos);
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path()), boost::filesystem::directory_iterator()) == 1);
}

TEST_CASE("The camera is the first enabled webcam the player can open", "[MoonrakerPrinterAgent][Camera]")
{
    const std::string base = "http://192.0.2.10:7125";
    const auto pick = [&](nlohmann::json webcams) { return MoonrakerPrinterAgent::pick_webcam({{"webcams", webcams}}, base); };

    SECTION("An MJPEG stream plays as a stream, resolved against the web frontend on the printer's address")
    {
        const auto camera = pick({{{"name", "cam"}, {"service", "mjpegstreamer-adaptive"}, {"stream_url", "/webcam/?action=stream"},
                                   {"snapshot_url", "/webcam/?action=snapshot"}}});
        CHECK(camera.mode == CameraStreamMode::http);
        CHECK(camera.url == "http://192.0.2.10/webcam/?action=stream");
    }
    SECTION("A frontend port other than Moonraker's is kept")
    {
        const auto camera = MoonrakerPrinterAgent::pick_webcam(
            {{"webcams", {{{"service", "mjpegstreamer"}, {"stream_url", "/webcam/?action=stream"}}}}}, "http://192.0.2.10:4408");
        CHECK(camera.url == "http://192.0.2.10:4408/webcam/?action=stream");
    }
    SECTION("An absolute RTSP stream is kept as it is")
    {
        const auto camera = pick({{{"service", "ipstream"}, {"stream_url", "rtsp://192.0.2.11/live"}}});
        CHECK(camera.mode == CameraStreamMode::rtsp);
        CHECK(camera.url == "rtsp://192.0.2.11/live");
    }
    SECTION("A WebRTC camera falls back to its snapshot")
    {
        const auto camera = pick({{{"service", "webrtc-camerastreamer"}, {"stream_url", "/webcam/webrtc"},
                                   {"snapshot_url", "/webcam/snapshot.jpg"}}});
        CHECK(camera.mode == CameraStreamMode::http_snapshot);
        CHECK(camera.url == "http://192.0.2.10/webcam/snapshot.jpg");
    }
    SECTION("A disabled webcam is skipped")
    {
        const auto camera = pick({{{"enabled", false}, {"service", "mjpegstreamer"}, {"stream_url", "/one"}},
                                  {{"service", "mjpegstreamer"}, {"stream_url", "https://cam.example/two"}}});
        CHECK(camera.mode == CameraStreamMode::https);
        CHECK(camera.url == "https://cam.example/two");
    }
    SECTION("A printer behind HTTPS keeps the scheme")
    {
        const auto camera = MoonrakerPrinterAgent::pick_webcam(
            {{"webcams", {{{"service", "mjpegstreamer"}, {"stream_url", "/webcam/?action=stream"}}}}}, "https://printer.example");
        CHECK(camera.mode == CameraStreamMode::https);
        CHECK(camera.url == "https://printer.example/webcam/?action=stream");
    }
    SECTION("A relative URL that carries a URL in its query stays relative")
    {
        const auto camera = pick({{{"service", "mjpegstreamer"}, {"stream_url", "/webcam/?src=http://192.0.2.99/x"}}});
        CHECK(camera.url == "http://192.0.2.10/webcam/?src=http://192.0.2.99/x");
    }
    SECTION("A protocol-relative URL takes the printer's scheme")
    {
        const auto camera = pick({{{"service", "mjpegstreamer"}, {"stream_url", "//192.0.2.12:8080/stream"}}});
        CHECK(camera.url == "http://192.0.2.12:8080/stream");
    }
    SECTION("An RTSPS stream plays as RTSP")
    {
        const auto camera = pick({{{"service", "ipstream"}, {"stream_url", "RTSPS://192.0.2.11/live"}}});
        CHECK(camera.mode == CameraStreamMode::rtsp);
    }
    SECTION("A scheme the player cannot open is never taken")
    {
        CHECK(pick({{{"service", "mjpegstreamer"}, {"stream_url", "file:///etc/passwd"}}}).mode == CameraStreamMode::none);
        CHECK(pick({{{"service", "webrtc-camerastreamer"}, {"stream_url", "/webrtc"}, {"snapshot_url", "smb://192.0.2.13/s.jpg"}}}).mode ==
              CameraStreamMode::none);
        // Nor an RTSP URL as a snapshot
        CHECK(pick({{{"service", "webrtc-camerastreamer"}, {"stream_url", "/webrtc"}, {"snapshot_url", "rtsp://192.0.2.13/s"}}}).mode ==
              CameraStreamMode::none);
        // The next webcam is taken instead
        const auto camera = pick({{{"service", "mjpegstreamer"}, {"stream_url", "javascript:alert(1)"}},
                                  {{"service", "mjpegstreamer"}, {"stream_url", "/two"}}});
        CHECK(camera.url == "http://192.0.2.10/two");
    }
    SECTION("No webcam, no camera")
    {
        CHECK(pick(nlohmann::json::array()).mode == CameraStreamMode::none);
        CHECK(MoonrakerPrinterAgent::pick_webcam(nlohmann::json::object(), base).mode == CameraStreamMode::none);
    }
}

TEST_CASE("Status reports carry the layers and the time left", "[MoonrakerPrinterAgent][Status]")
{
    const nlohmann::json status = {
        {"print_stats", {{"state", "printing"}, {"print_duration", 600.0}, {"total_duration", 900.0},
                         {"info", {{"current_layer", 12}, {"total_layer", 48}}}}},
        {"virtual_sdcard", {{"progress", 0.25}}},
    };
    const nlohmann::json print = MoonrakerPrinterAgent::build_print_payload(status, {}, true).at("print");

    CHECK(print.at("layer_num") == 12);
    CHECK(print.at("total_layer_num") == 48);
    CHECK(print.at("mc_percent") == 25);
    // 10 minutes for the first quarter: 30 minutes for the rest
    CHECK(print.at("mc_remaining_time") == 30);
    CHECK(print.at("gcode_state") == "RUNNING");

    const nlohmann::json no_layers = MoonrakerPrinterAgent::build_print_payload({{"print_stats", {{"state", "standby"}}}}, {}, true);
    CHECK_FALSE(no_layers.at("print").contains("total_layer_num"));
}

TEST_CASE("Status reports show the active extruder of a printer with several", "[MoonrakerPrinterAgent][Status]")
{
    const nlohmann::json status = {
        {"toolhead", {{"extruder", "extruder2"}}},
        {"extruder", {{"temperature", 25.0}, {"target", 0.0}}},
        {"extruder2", {{"temperature", 215.0}, {"target", 220.0}}},
    };
    const nlohmann::json print = MoonrakerPrinterAgent::build_print_payload(status, {}, true).at("print");
    CHECK(print.at("nozzle_temper") == 215.0);
    CHECK(print.at("nozzle_target_temper") == 220.0);
}

TEST_CASE("The Device tab's speed levels map to the printer's speed factor and back", "[MoonrakerPrinterAgent][Status]")
{
    CHECK(MoonrakerPrinterAgent::speed_percent_for_level(1) == 50);
    CHECK(MoonrakerPrinterAgent::speed_percent_for_level(4) == 166);
    CHECK(MoonrakerPrinterAgent::speed_percent_for_level(7) == 0);
    CHECK(MoonrakerPrinterAgent::speed_level_for_factor(1.0) == 2);
    CHECK(MoonrakerPrinterAgent::speed_level_for_factor(1.3) == 3);
    CHECK(MoonrakerPrinterAgent::speed_level_for_factor(0.4) == 1);
    // Halfway between two levels: the one nearer standard
    CHECK(MoonrakerPrinterAgent::speed_level_for_factor(0.75) == 2);
    CHECK(MoonrakerPrinterAgent::speed_level_for_factor(1.12) == 2);
    CHECK(MoonrakerPrinterAgent::speed_level_for_factor(1.45) == 3);

    const nlohmann::json status = {{"gcode_move", {{"speed_factor", 1.66}}}};
    CHECK(MoonrakerPrinterAgent::build_print_payload(status, {}, true).at("print").at("spd_lvl") == 4);
}

TEST_CASE("Status reports show the chamber LED as the chamber light and the chamber sensor's temperature", "[MoonrakerPrinterAgent][Status]")
{
    const auto light = [](const nlohmann::json& color_data) {
        const nlohmann::json status = {{"led cavity_led", {{"color_data", color_data}}}};
        return MoonrakerPrinterAgent::build_print_payload(status, {}, true, "cavity_led").at("print").at("lights_report").at(0);
    };
    CHECK(light({{0, 0, 0, 1}}).at("mode") == "on");
    CHECK(light({{0, 0, 0, 0}}).at("mode") == "off");
    CHECK(light({{0, 0, 0, 0}}).at("node") == "chamber_light");
    // The chamber sensor's temperature, shown because it is reported
    const nlohmann::json chamber = MoonrakerPrinterAgent::build_print_payload(
        {{"temperature_sensor cavity", {{"temperature", 31.5}}}}, {}, true, {}, "temperature_sensor cavity").at("print");
    CHECK(chamber.at("chamber_temper") == 31.5);
    CHECK(chamber.at("support_chamber_temp_display") == true);
    // An agent without a chamber LED reports no light
    CHECK_FALSE(MoonrakerPrinterAgent::build_print_payload({{"led cavity_led", {{"color_data", {{1, 1, 1, 1}}}}}}, {}, true)
                    .at("print")
                    .contains("lights_report"));
}

TEST_CASE("The printed file's thumbnail is its largest one, in the file's folder", "[MoonrakerPrinterAgent][Status]")
{
    const nlohmann::json metadata = {{"result",
                                      {{"filename", "Toys/dragon 4.gcode"},
                                       {"thumbnails",
                                        {{{"width", 32}, {"height", 32}, {"relative_path", ".thumbs/dragon 4-32x32.png"}},
                                         {{"width", 300}, {"height", 300}, {"relative_path", ".thumbs/dragon 4-300x300.png"}}}}}}};
    CHECK(MoonrakerPrinterAgent::thumbnail_url(metadata, "http://192.0.2.10:7125", "Toys/dragon 4.gcode") ==
          "http://192.0.2.10:7125/server/files/gcodes/Toys/.thumbs/dragon%204-300x300.png");
    CHECK(MoonrakerPrinterAgent::thumbnail_url({{"result", {{"thumbnails", nlohmann::json::array()}}}}, "http://h", "a.gcode").empty());
    CHECK(MoonrakerPrinterAgent::thumbnail_url(nlohmann::json::object(), "http://h", "a.gcode").empty());
    // A size of the wrong type counts as none
    CHECK(MoonrakerPrinterAgent::thumbnail_url({{"result", {{"thumbnails", {{{"width", "big"}, {"relative_path", "a.png"}}}}}}}, "http://h",
                                               "a.gcode") == "http://h/server/files/gcodes/a.png");
}

// Sends G-code to a printer without connecting to it, with the given chamber LED.
class CommandAgent : public MoonrakerPrinterAgent
{
public:
    CommandAgent(const FakePrinter& printer, std::string led) : MoonrakerPrinterAgent(""), m_led(std::move(led))
    {
        device_info.base_url = printer.base_url();
    }

protected:
    std::string chamber_led() const override { return m_led; }

private:
    std::string m_led;
};

TEST_CASE("The Device tab's speed level reaches the printer as M220", "[MoonrakerPrinterAgent][Command]")
{
    FakePrinter  printer(Behavior::AnswersPings);
    CommandAgent agent(printer, "");
    const auto   speed = [&](const nlohmann::json& param) {
        return agent.send_message("printer", nlohmann::json{{"print", {{"command", "print_speed"}, {"param", param}}}}.dump(), 0, 0);
    };

    CHECK(speed("3") == BAMBU_NETWORK_SUCCESS);
    CHECK(printer.wait_for_script("M220 S124"));
    for (const char* invalid : {"0", "5", "-1", "", "abc", "2x", " 2"}) {
        CAPTURE(invalid);
        CHECK(speed(invalid) == BAMBU_NETWORK_ERR_INVALID_RESULT);
    }
}

TEST_CASE("The Device tab switches the chamber LED with every channel", "[MoonrakerPrinterAgent][Command]")
{
    FakePrinter printer(Behavior::AnswersPings);
    const auto  light = [&](CommandAgent& agent, const nlohmann::json& system) {
        nlohmann::json request = {{"system", system}};
        request["system"]["command"] = "ledctrl";
        return agent.send_message("printer", request.dump(), 0, 0);
    };

    CommandAgent agent(printer, "chamber");
    CHECK(light(agent, {{"led_node", "chamber_light"}, {"led_mode", "on"}}) == BAMBU_NETWORK_SUCCESS);
    CHECK(printer.wait_for_script("SET_LED LED=chamber RED=1 GREEN=1 BLUE=1 WHITE=1"));
    CHECK(light(agent, {{"led_node", "chamber_light"}, {"led_mode", "off"}}) == BAMBU_NETWORK_SUCCESS);
    CHECK(printer.wait_for_script("SET_LED LED=chamber RED=0 GREEN=0 BLUE=0 WHITE=0"));

    // Modes other than on and off, and values of the wrong type
    CHECK(light(agent, {{"led_node", "chamber_light"}, {"led_mode", "flashing"}}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(light(agent, {{"led_node", "chamber_light"}, {"led_mode", 1}}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(light(agent, {{"led_node", "chamber_light"}}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    // Another LED, or a node of the wrong type, is not the chamber light
    CHECK(light(agent, {{"led_node", "work_light"}, {"led_mode", "on"}}) == BAMBU_NETWORK_SUCCESS);
    CHECK(light(agent, {{"led_node", 7}, {"led_mode", "on"}}) == BAMBU_NETWORK_SUCCESS);
    // An agent without a chamber LED
    CommandAgent no_led(printer, "");
    CHECK(light(no_led, {{"led_node", "chamber_light"}, {"led_mode", "on"}}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    // None of the refused requests reached the printer
    CHECK(printer.script() == "SET_LED LED=chamber RED=0 GREEN=0 BLUE=0 WHITE=0");
}

// Sends one JSON-RPC request over the status stream, the first time it is asked.
class RequestAgent : public StreamAgent
{
public:
    using StreamAgent::StreamAgent;

protected:
    std::vector<nlohmann::json> take_ws_requests() override
    {
        if (m_sent.exchange(true))
            return {};
        return {{{"method", "camera.start_monitor"}, {"params", {{"domain", "lan"}, {"interval", 0}}}}};
    }

private:
    std::atomic<bool> m_sent{false};
};

TEST_CASE("The status stream sends an agent's requests to the printer", "[MoonrakerPrinterAgent][StatusStream]")
{
    FakePrinter printer(Behavior::Chatters);
    RequestAgent agent("");
    agent.start_status_stream("printer", printer.base_url(), "");

    CHECK(printer.wait_for_ws_message("\"method\":\"camera.start_monitor\""));
    CHECK(printer.wait_for_ws_message("\"jsonrpc\":\"2.0\""));
    printer.release();
    agent.stop_status_stream();
}

TEST_CASE("The filament objects are read from the status stream once it reported them", "[MoonrakerPrinterAgent][Status]")
{
    const std::set<std::string> names = {"print_task_config", "filament_detect"};
    const nlohmann::json        ptc   = {{"filament_type", {"PLA"}}};
    // A printer without filament_detect needs only print_task_config
    CHECK(MoonrakerPrinterAgent::pick_objects({{"print_task_config", ptc}}, {"print_task_config"}, names) ==
          nlohmann::json{{"print_task_config", ptc}});
    // Not reported yet: the caller asks the printer
    CHECK(MoonrakerPrinterAgent::pick_objects({{"print_task_config", ptc}}, {"print_task_config", "filament_detect"}, names).is_null());
    CHECK(MoonrakerPrinterAgent::pick_objects({{"print_task_config", ptc}}, {}, names).is_null());
}

TEST_CASE("The Snapmaker agent keeps the stock camera picture current only while it is shown", "[SnapmakerPrinterAgent][Camera]")
{
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    const clock::time_point never{};

    // Shown just now and never asked: ask
    CHECK(SnapmakerPrinterAgent::camera_request_due(now, now - 1s, never));
    // Asked 20 s ago: not yet
    CHECK_FALSE(SnapmakerPrinterAgent::camera_request_due(now, now - 1s, now - 20s));
    // Asked 50 s ago: again, before the firmware stops renewing the picture
    CHECK(SnapmakerPrinterAgent::camera_request_due(now, now - 1s, now - 50s));
    // Not shown for a while: let it stop
    CHECK_FALSE(SnapmakerPrinterAgent::camera_request_due(now, now - 30s, never));
}
