#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
    SilentBeforeHandshake, // Never answers the websocket upgrade
    Silent,                // Completes the websocket upgrade, then never reads or sends
    Answers,               // Sends a heartbeat, and the CFS slots once the heartbeat is acknowledged
};

struct Message
{
    bool text;
    std::string payload;
};

// Serves the printer's HTTP API and its websocket on one port.
class FakeCrealityPrinter
{
public:
    FakeCrealityPrinter(Behavior behavior, std::string model) : m_behavior(behavior), m_model(std::move(model))
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
        if (ec)
            return;
        if (!websocket::is_upgrade(request)) {
            serve_http(socket, request);
            return;
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
            if (first)
                ws.write(net::buffer(std::string(R"({"ModeCode":"heart_beat"})")), ec);
            else if (payload == "ok")
                ws.write(net::buffer(std::string(R"({"boxsInfo":{"materialBoxs":[]}})")), ec);
        }
    }

    void serve_http(tcp::socket& socket, const http::request<http::string_body>& request)
    {
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.keep_alive(false);
        // The upload only needs a success status.
        response.body() = request.target() == "/info" ? nlohmann::json{{"model", m_model}}.dump() : "{}";
        response.prepare_payload();
        beast::error_code ec;
        http::write(socket, response, ec);
    }

    void wait_for_release()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_released; });
    }

    const Behavior m_behavior;
    const std::string m_model;
    net::io_context m_accept_ioc;
    tcp::acceptor m_acceptor{m_accept_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool> m_stopping{false};
    std::thread m_accept_thread;
    std::vector<std::thread> m_connections;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<Message> m_messages;
    bool m_released = false;
};

class TestHost : public CrealityPrint
{
public:
    using CrealityPrint::CrealityPrint;
    using CrealityPrint::ws_settings;
};

std::unique_ptr<TestHost> make_host(const FakeCrealityPrinter& printer, std::chrono::milliseconds deadline)
{
    DynamicPrintConfig config;
    config.set_key_value("print_host", new ConfigOptionString(printer.base_url()));
    config.set_key_value("print_host_webui", new ConfigOptionString(""));
    config.set_key_value("printhost_cafile", new ConfigOptionString(""));
    config.set_key_value("printhost_port", new ConfigOptionString(""));
    config.set_key_value("printhost_apikey", new ConfigOptionString(""));
    config.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    auto host                 = std::make_unique<TestHost>(&config);
    host->ws_settings.port    = printer.port();
    host->ws_settings.connect = deadline;
    host->ws_settings.reply   = deadline;
    host->ws_settings.close   = deadline;
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

bool upload_and_print(const CrealityPrint& host, wxString& error, const std::map<std::string, std::string>& extended_info = {})
{
    const ScopedTemporaryFile source(".gcode");
    std::ofstream(source.string()) << "G28\n";
    PrintHostUpload upload;
    upload.source_path   = source.path();
    upload.upload_path   = "test.gcode";
    upload.post_action   = PrintHostPostUploadAction::StartPrint;
    upload.extended_info = extended_info;
    return host.upload(upload, [](Http::Progress, bool&) {}, [&](wxString message) { error = message; }, [](wxString, wxString) {});
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
}

TEST_CASE("Starting a print sends each command with the G-code path in a text frame", "[CrealityPrint]")
{
    // F008 is a multi-color model and F001 a plain one. K1-family models keep their G-code under /usr/data.
    // Only a mapping onto CFS slots starts a multi-color print; a printer that listed no slots starts plainly.
    const std::map<std::string, std::string> cfs_mapping = {{"enableSelfTest", "0"}, {"colorMatch_0", "T1A\tPLA\t#FF0000\t1\t0"}};
    const auto [model, extended_info, commands, gcode] = GENERATE_REF(table<std::string, std::map<std::string, std::string>, std::vector<std::string>, std::string>({
        {"F008", cfs_mapping, {"colorMatch", "multiColorPrint"}, "/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F008", {}, {"opGcodeFile"}, "printprt:/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"F001", {}, {"opGcodeFile"}, "printprt:/mnt/UDISK/printer_data/gcodes/test.gcode"},
        {"K1C", cfs_mapping, {"colorMatch", "multiColorPrint"}, "/usr/data/printer_data/gcodes/test.gcode"},
        {"K1C", {}, {"opGcodeFile"}, "printprt:/usr/data/printer_data/gcodes/test.gcode"},
    }));
    CAPTURE(model, extended_info.size());

    FakeCrealityPrinter printer(Behavior::Answers, model);
    const auto host = make_host(printer, 5s);
    wxString error;

    CHECK(upload_and_print(*host, error, extended_info));
    CHECK(error.empty());
    const std::vector<Message> messages = printer.messages();
    REQUIRE(messages.size() == commands.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        CAPTURE(i);
        CHECK(messages[i].text);
        const nlohmann::json params = nlohmann::json::parse(messages[i].payload).at("params");
        REQUIRE(params.contains(commands[i]));
        CHECK(params.at(commands[i]).dump().find(gcode) != std::string::npos);
    }
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
