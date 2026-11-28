#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <nlohmann/json.hpp>
#include <wx/string.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/CrealityPrint.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using namespace std::chrono_literals;

enum class Behavior {
    HangsUp,               // Closes every connection without answering
    SilentBeforeHandshake, // Never answers the websocket upgrade
    Silent,                // Completes the websocket upgrade, then never reads or sends
    Answers,               // Sends a heartbeat, and the CFS slots once the heartbeat is acknowledged
    ReportsNoCfs,          // Sends a status reporting no CFS connected, then never sends the CFS slots
    ReportsNoCfsThenSlots, // Sends a status reporting no CFS connected, then the CFS slots, as a SPARKX i7 with a CFS Lite does
    ReportsCfs,            // Sends a status reporting a CFS connected, then the CFS slots
    ReportsCfsSlowly,      // Sends a status reporting a CFS connected, then the CFS slots 1.5 s later
    StreamsCfs,            // Sends a status reporting a CFS connected every 50 ms and never sends the CFS slots
    StreamsCfsThenSlots,   // Sends 25 statuses reporting a CFS connected 50 ms apart, then the CFS slots
};

struct Message
{
    bool text;
    std::string payload;
};

// Serves the printer's HTTP API and its websocket on one port. With file_lists, each G-code file list query gets the next one,
// and the last one once they run out.
class FakeCrealityPrinter
{
public:
    FakeCrealityPrinter(Behavior behavior, std::string model, std::vector<std::string> file_lists = {})
        : m_behavior(behavior), m_model(std::move(model)), m_file_lists(std::move(file_lists))
    {
        m_accept_thread = std::thread([this] { accept_loop(); });
    }

    ~FakeCrealityPrinter()
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

    std::string port() const { return std::to_string(m_acceptor.local_endpoint().port()); }
    std::string base_url() const { return "http://127.0.0.1:" + port(); }

    std::vector<Message> messages()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_messages;
    }

    int http_requests()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_http_requests;
    }

    int websocket_requests()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_websocket_requests;
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
            auto ioc = std::make_unique<net::io_context>();
            tcp::socket socket{*ioc};
            beast::error_code ec;
            m_acceptor.accept(socket, ec);
            if (ec || m_stopping)
                return;
            m_connections.emplace_back([this, ioc = std::move(ioc), socket = std::move(socket)]() mutable { serve(std::move(socket)); });
        }
    }

    void serve(tcp::socket socket)
    {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code ec;
        http::read(socket, buffer, request, ec);
        if (ec || m_behavior == Behavior::HangsUp)
            return;
        if (!websocket::is_upgrade(request)) {
            serve_http(socket, request);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_websocket_requests;
        }
        if (m_behavior == Behavior::SilentBeforeHandshake) {
            wait_for_release();
            return;
        }

        websocket::stream<tcp::socket> ws{std::move(socket)};
        ws.accept(request, ec);
        if (ec)
            return;
        if (m_behavior == Behavior::Silent) {
            wait_for_release();
            return;
        }

        ws.text(true);
        // Reading also answers the client's close frame.
        for (bool first = true; !ec; first = false) {
            beast::flat_buffer message;
            ws.read(message, ec);
            if (ec)
                break;
            const std::string payload = beast::buffers_to_string(message.data());
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_messages.push_back({ws.got_text(), payload});
            }
            const std::string slots = R"({"boxsInfo":{"materialBoxs":[]}})";
            if (!m_file_lists.empty() && payload.find("reqGcodeFile") != std::string::npos) {
                std::string file_list;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    file_list = m_file_lists[std::min(m_file_list_replies++, m_file_lists.size() - 1)];
                }
                ws.write(net::buffer(file_list), ec);
                continue;
            }
            if (first && m_behavior == Behavior::ReportsNoCfs) {
                ws.write(net::buffer(std::string(R"({"cfsConnect":0,"nozzleTemp":"25.000000"})")), ec);
                wait_for_release();
                return;
            }
            if (first && m_behavior == Behavior::ReportsNoCfsThenSlots) {
                ws.write(net::buffer(std::string(R"({"cfsConnect":0,"nozzleTemp":"25.000000"})")), ec);
                ws.write(net::buffer(std::string(R"({"connectionCount":1})")), ec);
                ws.write(net::buffer(slots), ec);
                continue;
            }
            if (first && (m_behavior == Behavior::StreamsCfs || m_behavior == Behavior::StreamsCfsThenSlots)) {
                const bool forever = m_behavior == Behavior::StreamsCfs;
                for (int count = 0; !ec && !released() && (forever || count < 25); ++count) {
                    ws.write(net::buffer(std::string(R"({"cfsConnect":1,"nozzleTemp":"25.000000"})")), ec);
                    std::this_thread::sleep_for(50ms);
                }
                if (forever) {
                    wait_for_release();
                    return;
                }
                ws.write(net::buffer(slots), ec);
                continue;
            }
            if (first && (m_behavior == Behavior::ReportsCfs || m_behavior == Behavior::ReportsCfsSlowly)) {
                ws.write(net::buffer(std::string(R"({"cfsConnect":1,"nozzleTemp":"25.000000"})")), ec);
                if (m_behavior == Behavior::ReportsCfsSlowly)
                    std::this_thread::sleep_for(1500ms);
                ws.write(net::buffer(slots), ec);
            } else if (first)
                ws.write(net::buffer(std::string(R"({"ModeCode":"heart_beat"})")), ec);
            else if (payload == "ok")
                ws.write(net::buffer(slots), ec);
        }
    }

    void serve_http(tcp::socket& socket, const http::request<http::string_body>& request)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_http_requests;
        }
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.keep_alive(false);
        // The upload only needs a success status.
        response.body() = request.target() == "/info" ? nlohmann::json{{"model", m_model}}.dump() : "{}";
        response.prepare_payload();
        beast::error_code ec;
        http::write(socket, response, ec);
    }

    bool released()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_released;
    }

    void wait_for_release()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_released; });
    }

    const Behavior m_behavior;
    const std::string m_model;
    const std::vector<std::string> m_file_lists;
    net::io_context m_accept_ioc;
    tcp::acceptor m_acceptor{m_accept_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool> m_stopping{false};
    std::thread m_accept_thread;
    std::vector<std::thread> m_connections;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<Message> m_messages;
    int m_http_requests = 0;
    int m_websocket_requests = 0;
    std::size_t m_file_list_replies = 0;
    bool m_released = false;
};

class TestHost : public CrealityPrint
{
public:
    using CrealityPrint::CrealityPrint;
    using CrealityPrint::ws_settings;
};

DynamicPrintConfig host_config(const std::string& url)
{
    DynamicPrintConfig config;
    config.set_key_value("print_host", new ConfigOptionString(url));
    config.set_key_value("print_host_webui", new ConfigOptionString(""));
    config.set_key_value("printhost_cafile", new ConfigOptionString(""));
    config.set_key_value("printhost_port", new ConfigOptionString(""));
    config.set_key_value("printhost_apikey", new ConfigOptionString(""));
    config.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    return config;
}

std::unique_ptr<TestHost> make_host(const FakeCrealityPrinter& printer, std::chrono::milliseconds deadline)
{
    DynamicPrintConfig config      = host_config(printer.base_url());
    auto host                      = std::make_unique<TestHost>(&config);
    host->ws_settings.port         = printer.port();
    host->ws_settings.connect      = deadline;
    host->ws_settings.reply        = deadline;
    host->ws_settings.slots_reply  = deadline;
    host->ws_settings.no_cfs_reply = deadline;
    host->ws_settings.files_reply  = deadline;
    host->ws_settings.close        = deadline;
    return host;
}

// Runs `call`, giving it `limit` to return before the printer hangs up on it.
template<class Call> bool finishes_within(FakeCrealityPrinter& printer, std::chrono::milliseconds limit, Call&& call)
{
    auto done          = std::async(std::launch::async, std::forward<Call>(call));
    const bool in_time = done.wait_for(limit) == std::future_status::ready;
    printer.release();
    // Exits instead of returning, since the future's destructor would wait forever for a call the hang-up cannot unblock.
    if (done.wait_for(10s) != std::future_status::ready) {
        std::cerr << "CrealityPrint call still running 10 s after the printer hung up" << std::endl;
        std::_Exit(EXIT_FAILURE);
    }
    return in_time;
}

bool upload_and_print(const CrealityPrint&                      host,
                      wxString&                                 error,
                      const std::map<std::string, std::string>& extended_info = {},
                      const std::string&                        upload_path   = "test.gcode")
{
    const ScopedTemporaryFile source(".gcode");
    std::ofstream(source.string()) << "G28\n";
    PrintHostUpload upload;
    upload.source_path   = source.path();
    upload.upload_path   = upload_path;
    upload.post_action   = PrintHostPostUploadAction::StartPrint;
    upload.extended_info = extended_info;
    return host.upload(upload, [](Http::Progress, bool&) {}, [&](wxString message) { error = message; }, [](wxString, wxString) {});
}

bool is_file_list_query(const Message& message) { return message.payload.find("reqGcodeFile") != std::string::npos; }

int file_list_queries(FakeCrealityPrinter& printer)
{
    const std::vector<Message> messages = printer.messages();
    return static_cast<int>(std::count_if(messages.begin(), messages.end(), is_file_list_query));
}

// The start commands, without the G-code file list query and the heartbeat acknowledgement sent on its connection.
std::vector<Message> start_messages(FakeCrealityPrinter& printer)
{
    std::vector<Message> messages = printer.messages();
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [](const Message& message) { return is_file_list_query(message) || message.payload == "ok"; }),
                   messages.end());
    return messages;
}

} // namespace

TEST_CASE("Querying the CFS slots gives a silent printer no more than its deadlines", "[CrealityPrint]")
{
    const Behavior behavior = GENERATE(Behavior::SilentBeforeHandshake, Behavior::Silent);
    CAPTURE(static_cast<int>(behavior));

    FakeCrealityPrinter printer(behavior, "F008");
    const auto host = make_host(printer, 200ms);
    std::string reply;

    CHECK(finishes_within(printer, 2s, [&] { reply = host->query_boxes_info(); }));
    CHECK(reply.empty());
}

TEST_CASE("The CFS slot query has a shorter default deadline than a command", "[CrealityPrint]")
{
    DynamicPrintConfig config = host_config("http://127.0.0.1");
    const TestHost     host(&config);

    CHECK(host.ws_settings.slots_reply < host.ws_settings.reply);
    CHECK(host.ws_settings.no_cfs_reply < host.ws_settings.slots_reply);
}

TEST_CASE("Querying the CFS slots waits on a silent printer for the slot deadline, not the command one", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Silent, "F008");
    const auto host         = make_host(printer, 200ms);
    host->ws_settings.reply = 30s;
    std::string reply;

    CHECK(finishes_within(printer, 2s, [&] { reply = host->query_boxes_info(); }));
    CHECK(reply.empty());
}

TEST_CASE("Querying the CFS slots waits only the no-CFS deadline after a status reporting no CFS connected", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::ReportsNoCfs, "K1C");
    const auto host               = make_host(printer, 200ms);
    host->ws_settings.slots_reply = 30s;
    std::string reply;

    CHECK(finishes_within(printer, 2s, [&] { reply = host->query_boxes_info(); }));
    CHECK(reply.empty());
}

TEST_CASE("Querying the CFS slots reads the slots sent after a status reporting no CFS connected", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::ReportsNoCfsThenSlots, "F022");
    const auto host = make_host(printer, 5s);
    bool no_cfs     = true;

    CHECK(host->query_boxes_info(&no_cfs) == R"({"boxsInfo":{"materialBoxs":[]}})");
    CHECK_FALSE(no_cfs);
}

TEST_CASE("Querying the CFS slots waits past a status reporting a CFS connected", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::ReportsCfs, "K1C");
    const auto host = make_host(printer, 5s);
    bool no_cfs     = true;

    CHECK(host->query_boxes_info(&no_cfs) == R"({"boxsInfo":{"materialBoxs":[]}})");
    CHECK_FALSE(no_cfs);
}

TEST_CASE("A status reporting a CFS connected never shortens the CFS slot deadline", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::ReportsCfsSlowly, "K1C");
    const auto host               = make_host(printer, 200ms);
    host->ws_settings.slots_reply = 5s;

    CHECK(host->query_boxes_info() == R"({"boxsInfo":{"materialBoxs":[]}})");
}

TEST_CASE("Querying the CFS slots keeps reading while the printer streams status updates", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::StreamsCfsThenSlots, "K1C");
    const auto host         = make_host(printer, 1s);
    host->ws_settings.reply = 5s;

    CHECK(host->query_boxes_info() == R"({"boxsInfo":{"materialBoxs":[]}})");
}

TEST_CASE("Querying the CFS slots gives a printer reporting a CFS connected no more than the command deadline", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::StreamsCfs, "K1C");
    const auto host         = make_host(printer, 200ms);
    host->ws_settings.reply = 400ms;
    std::string reply;

    CHECK(finishes_within(printer, 2s, [&] { reply = host->query_boxes_info(); }));
    CHECK(reply.empty());
}

TEST_CASE("Querying the CFS slots acknowledges heartbeats in text frames", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Answers, "F008");
    const auto host = make_host(printer, 5s);

    CHECK(host->query_boxes_info() == R"({"boxsInfo":{"materialBoxs":[]}})");
    const std::vector<Message> messages = printer.messages();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].text);
    CHECK(nlohmann::json::parse(messages[0].payload) == nlohmann::json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});
    CHECK(messages[1].text);
    CHECK(messages[1].payload == "ok");
}

TEST_CASE("Reading a CFS-capable printer gives its name and CFS slots", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Answers, "F008");
    const auto host = make_host(printer, 5s);

    const CrealityPrinterInfo info = host->read_printer_info();
    CHECK(info.reachable);
    CHECK(info.multi_color);
    CHECK_FALSE(info.no_cfs);
    CHECK(info.model_name == "K2 Plus");
    CHECK(info.boxes_json == R"({"boxsInfo":{"materialBoxs":[]}})");
}

TEST_CASE("Reading a Creality Hi gives its name and CFS slots", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Answers, "F018");
    const auto host = make_host(printer, 5s);

    const CrealityPrinterInfo info = host->read_printer_info();
    CHECK(info.reachable);
    CHECK(info.multi_color);
    CHECK(info.model_name == "Hi");
    CHECK(info.boxes_json == R"({"boxsInfo":{"materialBoxs":[]}})");
}

TEST_CASE("Reading a CFS-capable printer that reports no CFS connected flags it", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::ReportsNoCfs, "K1C");
    const auto host               = make_host(printer, 200ms);
    host->ws_settings.slots_reply = 30s;
    CrealityPrinterInfo info;

    CHECK(finishes_within(printer, 2s, [&] { info = host->read_printer_info(); }));
    CHECK(info.multi_color);
    CHECK(info.no_cfs);
    CHECK(info.boxes_json.empty());
}

TEST_CASE("Reading a printer without CFS support does not query the slots", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Answers, "F001");
    const auto host = make_host(printer, 5s);

    const CrealityPrinterInfo info = host->read_printer_info();
    CHECK(info.reachable);
    CHECK_FALSE(info.multi_color);
    CHECK(info.model_name == "unknown (F001)");
    CHECK(info.boxes_json.empty());
    CHECK(printer.messages().empty());
}

TEST_CASE("Reading a printer that names no model asks for its info once", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Answers, "");
    const auto host = make_host(printer, 5s);

    const CrealityPrinterInfo info = host->read_printer_info();
    CHECK(info.reachable);
    CHECK(info.model_name == "unknown");
    CHECK(printer.http_requests() == 1);
}

TEST_CASE("Reading a CFS-capable printer that never answers the slot query gives no slots within the deadlines", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::Silent, "K1C");
    const auto host = make_host(printer, 200ms);
    CrealityPrinterInfo info;

    CHECK(finishes_within(printer, 2s, [&] { info = host->read_printer_info(); }));
    CHECK(info.reachable);
    CHECK(info.multi_color);
    CHECK_FALSE(info.no_cfs);
    CHECK(info.model_name == "K1C");
    CHECK(info.boxes_json.empty());
}

TEST_CASE("Reading a printer that hangs up reports it unreachable", "[CrealityPrint]")
{
    FakeCrealityPrinter printer(Behavior::HangsUp, "K1C");
    const auto host = make_host(printer, 5s);

    const CrealityPrinterInfo info = host->read_printer_info();
    CHECK_FALSE(info.reachable);
    CHECK_FALSE(info.multi_color);
    CHECK(info.boxes_json.empty());
}

TEST_CASE("Starting a print gives a silent printer no more than its deadlines", "[CrealityPrint]")
{
    // F008 is a multi-color model and F001 a plain one. Without a handshake the start command is never sent, so the upload fails.
    const auto [behavior, model, started] = GENERATE(table<Behavior, std::string, bool>({
        {Behavior::SilentBeforeHandshake, "F008", false},
        {Behavior::Silent, "F008", true},
        {Behavior::SilentBeforeHandshake, "F001", false},
        {Behavior::Silent, "F001", true},
    }));
    CAPTURE(static_cast<int>(behavior), model);

    FakeCrealityPrinter printer(behavior, model);
    const auto host = make_host(printer, 200ms);
    bool result     = !started;
    wxString error;

    CHECK(finishes_within(printer, 3s, [&] { result = upload_and_print(*host, error); }));
    CHECK(result == started);
    CHECK(error.empty() == started);
    CHECK(error.StartsWith("Connection timed out") == !started);
    // A printer that never completes the handshake for the G-code file list query gets no second connection for the start.
    CHECK(printer.websocket_requests() == (started ? 2 : 1));
}

TEST_CASE("Starting a print sends each command with the G-code path in a text frame", "[CrealityPrint]")
{
    // The printer never answers the G-code file list query, so the model decides the folder. F008 is a multi-color model and F018
    // a plain one. K1C, F005 (Ender-3 V3 KE), F016 (K2 SE) and a Nebula Pad on an Ender-3 V2 Neo keep their G-code under /usr/data.
    // Only a mapping onto CFS slots starts a multi-color print; a printer that listed no slots starts plainly.
    const std::map<std::string, std::string> cfs_mapping = {{"enableSelfTest", "0"}, {"colorMatch_0", "T1A\tPLA\t#FF0000\t1\t0"}};
    const auto [model, extended_info, commands, gcode] = GENERATE_REF(table<std::string, std::map<std::string, std::string>, std::vector<std::string>, std::string>({
        {"F008", cfs_mapping, {"colorMatch", "multiColorPrint"}, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F008", {}, {"opGcodeFile"}, "printprt:/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F018", {}, {"opGcodeFile"}, "printprt:/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F005", {}, {"opGcodeFile"}, "printprt:/usr/data/printer_data/gcodes/test.gcode"},
        {"Ender-3 V2 Neo", {}, {"opGcodeFile"}, "printprt:/usr/data/printer_data/gcodes/test.gcode"},
        {"F016", cfs_mapping, {"colorMatch", "multiColorPrint"}, "/usr/data/printer_data/gcodes/test.gcode"},
        {"K1C", cfs_mapping, {"colorMatch", "multiColorPrint"}, "/usr/data/printer_data/gcodes/test.gcode"},
        {"K1C", {}, {"opGcodeFile"}, "printprt:/usr/data/printer_data/gcodes/test.gcode"},
    }));
    CAPTURE(model, extended_info.size());

    FakeCrealityPrinter printer(Behavior::Answers, model);
    const auto host = make_host(printer, 5s);
    host->ws_settings.files_reply = 200ms;
    wxString error;

    CHECK(upload_and_print(*host, error, extended_info));
    CHECK(error.empty());
    const std::vector<Message> messages = start_messages(printer);
    REQUIRE(messages.size() == commands.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        CAPTURE(i);
        CHECK(messages[i].text);
        const nlohmann::json params = nlohmann::json::parse(messages[i].payload).at("params");
        REQUIRE(params.contains(commands[i]));
        CHECK(params.at(commands[i]).dump().find(gcode) != std::string::npos);
    }
}

TEST_CASE("Starting a print uses the path the printer lists the upload at", "[CrealityPrint]")
{
    // K1-family firmware lists "folder:name:size:..." entries, escaping its slashes, and K2-family firmware files with their path.
    // The listed path decides over the model, which would give F018 and the unlisted F099 /mnt/UDISK and K1C /usr/data. Only the
    // upload counts: another file, a list without files, or a list in a format this host does not read leave the folder to the
    // model. A name holding ';' and a file in a subfolder are still found. A name that is not UTF-8 makes the printer's text frame
    // invalid, so the whole list is lost and the model decides, but the print still starts.
    const std::string k1_upload    = R"({"retGcodeFileInfo":{"totalNum":1,"fileInfo":"\/usr\/data\/printer_data\/gcodes:test.gcode:63301:0.240000:1794188820:109:\/tmp\/test.png;"}})";
    const std::string k1_other     = R"({"retGcodeFileInfo":{"totalNum":1,"fileInfo":"/usr/data/printer_data/gcodes:other.gcode:63301:0.240000:1794188820:109:/tmp/other.png;"}})";
    const std::string k1_semicolon = R"({"retGcodeFileInfo":{"totalNum":2,"fileInfo":"/usr/data/printer_data/gcodes:other.gcode:63301:0.240000:1794188820:109:/tmp/other.png;)"
                                     R"(/usr/data/printer_data/gcodes:a;b.gcode:63301:0.240000:1794188820:109:/tmp/a;b.png;"}})";
    const std::string k1_subfolder = R"({"retGcodeFileInfo":{"totalNum":2,"fileInfo":"/usr/data/printer_data/gcodes:test.gcode:63301:0.240000:1794188820:109:/tmp/test.png;)"
                                     R"(/usr/data/printer_data/gcodes/parts:test.gcode:63301:0.240000:1794188820:109:/tmp/test.png;"}})";
    const std::string k2_upload    = R"({"retGcodeFileInfo2":[{"name":"test.gcode","path":"/mnt/UDISK/printer_data/gcodes/test.gcode","type":8}]})";
    const std::string k2_both      = R"({"retGcodeFileInfo2":[{"name":"other.gcode","path":"/usr/data/printer_data/gcodes/other.gcode","type":8},)"
                                     R"({"name":"test.gcode","path":"/mnt/UDISK/printer_data/gcodes/test.gcode","type":8}]})";
    const std::string k2_not_utf8  = R"({"retGcodeFileInfo2":[{"name":"x.gcode","path":"/mnt/UDISK/printer_data/gcodes/)" "\xff" R"(.gcode","type":8},)"
                                     R"({"name":"test.gcode","path":"/mnt/UDISK/printer_data/gcodes/test.gcode","type":8}]})";
    const std::string no_files     = R"({"retGcodeFileInfo2":[]})";
    const std::string no_folders   = R"({"retGcodeFileInfo3":{"fileInfo":"test.gcode:63301:1794188820;"}})";
    const auto [model, upload_path, file_list, gcode] = GENERATE_REF(table<std::string, std::string, std::string, std::string>({
        {"F099", "test.gcode", k1_upload, "/usr/data/printer_data/gcodes/test.gcode"},
        {"F018", "test.gcode", k1_other, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F018", "a;b.gcode", k1_semicolon, "/usr/data/printer_data/gcodes/a;b.gcode"},
        {"F018", "parts/test.gcode", k1_subfolder, "/usr/data/printer_data/gcodes/parts/test.gcode"},
        {"K1C", "test.gcode", k2_upload, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"K1C", "test.gcode", k2_both, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"K1C", "test.gcode", k2_not_utf8, "/usr/data/printer_data/gcodes/test.gcode"},
        {"F005", "test.gcode", no_files, "/usr/data/printer_data/gcodes/test.gcode"},
        {"F018", "test.gcode", no_files, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F018", "test.gcode", no_folders, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
    }));
    CAPTURE(model, upload_path, file_list);

    FakeCrealityPrinter printer(Behavior::Answers, model, {file_list});
    const auto host               = make_host(printer, 5s);
    host->ws_settings.files_reply = 300ms;
    host->ws_settings.files_retry = 50ms;
    wxString error;

    CHECK(upload_and_print(*host, error, {}, upload_path));
    CHECK(error.empty());
    const std::vector<Message> messages = printer.messages();
    REQUIRE_FALSE(messages.empty());
    CHECK(is_file_list_query(messages[0]));
    CHECK(messages[0].text);
    const std::vector<Message> start = start_messages(printer);
    REQUIRE(start.size() == 1);
    const nlohmann::json params = nlohmann::json::parse(start[0].payload).at("params");
    CHECK(params.at("opGcodeFile") == "printprt:" + gcode);
}

TEST_CASE("Starting a print asks again for a file list that does not list the upload yet", "[CrealityPrint]")
{
    // The first list comes before the printer has indexed the upload. K1C's model alone would give /usr/data.
    const std::string no_files  = R"({"retGcodeFileInfo2":[]})";
    const std::string k2_upload = R"({"retGcodeFileInfo2":[{"name":"test.gcode","path":"/mnt/UDISK/printer_data/gcodes/test.gcode","type":8}]})";
    FakeCrealityPrinter printer(Behavior::Answers, "K1C", {no_files, k2_upload});
    const auto host               = make_host(printer, 5s);
    host->ws_settings.files_retry = 50ms;
    wxString error;

    CHECK(upload_and_print(*host, error));
    CHECK(error.empty());
    CHECK(file_list_queries(printer) == 2);
    const std::vector<Message> start = start_messages(printer);
    REQUIRE(start.size() == 1);
    CHECK(nlohmann::json::parse(start[0].payload).at("params").at("opGcodeFile") == "printprt:/mnt/UDISK/printer_data/gcodes/test.gcode");
}

TEST_CASE("Starting a print asks once for a file list in a format this host does not read", "[CrealityPrint]")
{
    const std::string no_folders = R"({"retGcodeFileInfo3":{"fileInfo":"test.gcode:63301:1794188820;"}})";
    FakeCrealityPrinter printer(Behavior::Answers, "F018", {no_folders});
    const auto host               = make_host(printer, 5s);
    host->ws_settings.files_retry = 50ms;
    wxString error;

    CHECK(upload_and_print(*host, error));
    CHECK(error.empty());
    CHECK(file_list_queries(printer) == 1);
}

namespace {

const std::string gcode_path = "/usr/data/printer_data/gcodes/part.gcode";

// A colorMatch_<n> value as the send dialog writes it: "toolId\ttype\tcolor\tboxId\tmaterialId"
std::string slot(const std::string& tool, int box_id, int material_id)
{
    return tool + "\tPLA\t#FF0000\t" + std::to_string(box_id) + "\t" + std::to_string(material_id);
}

std::vector<nlohmann::json> start_commands(bool multi_color, const std::map<std::string, std::string>& extended_info)
{
    std::vector<nlohmann::json> parsed;
    for (const std::string& cmd : CrealityPrint::start_print_commands(multi_color, gcode_path, extended_info))
        parsed.push_back(nlohmann::json::parse(cmd));
    return parsed;
}

} // namespace

TEST_CASE("A CFS-capable printer that listed no slots starts from the spool holder", "[CrealityPrint]")
{
    // The dialog maps filaments only onto slots the printer listed in boxsInfo;
    // a K1-family printer without a CFS lists none.
    auto cmds = start_commands(true, {{"enableSelfTest", "1"}});
    REQUIRE(cmds.size() == 1);
    const nlohmann::json& params = cmds[0]["params"];
    CHECK(params["opGcodeFile"] == "printprt:" + gcode_path);
    CHECK(params["enableSelfTest"] == 1);
    CHECK_FALSE(params.contains("colorMatch"));
}

TEST_CASE("A filament mapped onto the spool holder starts a plain print", "[CrealityPrint]")
{
    auto cmds = start_commands(true, {{"enableSelfTest", "0"}, {"colorMatch_0", slot("T1A", 0, 0)}});
    REQUIRE(cmds.size() == 1);
    const nlohmann::json& params = cmds[0]["params"];
    CHECK(params["opGcodeFile"] == "printprt:" + gcode_path);
    CHECK(params["enableSelfTest"] == 0);
}

TEST_CASE("A mapping that includes the spool holder starts a plain print even when other filaments map to CFS slots", "[CrealityPrint]")
{
    auto cmds = start_commands(true, {{"enableSelfTest", "0"},
                                      {"colorMatch_0", slot("T1A", 1, 2)},
                                      {"colorMatch_1", slot("T1B", 0, 0)}});
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0]["params"]["opGcodeFile"] == "printprt:" + gcode_path);
    CHECK_FALSE(cmds[0]["params"].contains("colorMatch"));
}

TEST_CASE("Filaments mapped onto CFS slots send the colour match before the multi-colour start", "[CrealityPrint]")
{
    auto cmds = start_commands(true, {{"enableSelfTest", "1"},
                                      {"colorMatch_0", slot("T1A", 1, 0)},
                                      {"colorMatch_1", slot("T1B", 1, 3)}});
    REQUIRE(cmds.size() == 2);

    const nlohmann::json& match = cmds[0]["params"]["colorMatch"];
    CHECK(match["path"] == gcode_path);
    REQUIRE(match["list"].size() == 2);
    CHECK(match["list"][0]["id"] == "T1A");
    CHECK(match["list"][0]["boxId"] == 1);
    CHECK(match["list"][0]["materialId"] == 0);
    CHECK(match["list"][1]["id"] == "T1B");
    CHECK(match["list"][1]["materialId"] == 3);
    CHECK(match["list"][1]["type"] == "PLA");
    CHECK(match["list"][1]["color"] == "#FF0000");

    const nlohmann::json& start = cmds[1]["params"]["multiColorPrint"];
    CHECK(start["gcode"] == gcode_path);
    CHECK(start["enableSelfTest"] == 1);
}

TEST_CASE("A printer without CFS support gets the plain start command", "[CrealityPrint]")
{
    auto cmds = start_commands(false, {{"enableSelfTest", "1"}});
    REQUIRE(cmds.size() == 1);
    const nlohmann::json& params = cmds[0]["params"];
    CHECK(params["opGcodeFile"] == "printprt:" + gcode_path);
    CHECK_FALSE(params.contains("enableSelfTest"));
}
