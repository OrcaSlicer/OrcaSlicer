#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Mcp/McpHttpServer.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::GUI::Mcp;
namespace net  = boost::asio;
namespace http = boost::beast::http;
using tcp      = net::ip::tcp;

namespace {

const std::string kToken = "test-token";

struct Response
{
    int         status = 0;
    std::string body;
    std::string allow;
    std::string www_authenticate;
};

struct Request
{
    http::verb  method = http::verb::post;
    std::string target = "/mcp";
    std::string host;   // default: 127.0.0.1:<port>
    std::string origin;
    std::string content_type = "application/json";
    std::string token        = kToken;
    std::string body         = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
};

Response send(uint16_t port, const Request& r)
{
    net::io_context ioc;
    tcp::socket     socket(ioc);
    socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    http::request<http::string_body> req{r.method, r.target, 11};
    req.set(http::field::host, r.host.empty() ? "127.0.0.1:" + std::to_string(port) : r.host);
    if (!r.origin.empty())
        req.set(http::field::origin, r.origin);
    if (!r.content_type.empty())
        req.set(http::field::content_type, r.content_type);
    if (!r.token.empty())
        req.set(http::field::authorization, "Bearer " + r.token);
    req.body() = r.body;
    req.prepare_payload();
    http::write(socket, req);
    boost::beast::flat_buffer          buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);
    return {int(res.result_int()), res.body(), std::string(res[http::field::allow]), std::string(res[http::field::www_authenticate])};
}

// A server whose handler replies with the request body, after an optional delay.
struct EchoServer
{
    std::atomic<int> delay_ms{0};
    McpHttpServer    server;

    explicit EchoServer(HttpLimits limits = {})
        : server(kToken,
                 [this](const PostRequest& r) {
                     if (int d = delay_ms.load())
                         std::this_thread::sleep_for(std::chrono::milliseconds(d));
                     return PostReply{200, r.body};
                 },
                 limits)
    {
        std::string err;
        REQUIRE(server.start(0, err));
    }
};

} // namespace

TEST_CASE("validate_request applies the endpoint rules", "[Mcp]")
{
    struct Case
    {
        HttpRequestView view;
        int             status;
    };
    HttpRequestView ok{"POST", "/mcp", "127.0.0.1:4000", "", "application/json", "Bearer secret", 10};
    auto with = [&](auto change) {
        HttpRequestView v = ok;
        change(v);
        return v;
    };
    const Case c = GENERATE_COPY(values<Case>({
        {ok, 0},
        {with([](HttpRequestView& v) { v.host = "localhost:4000"; }), 0},
        {with([](HttpRequestView& v) { v.content_type = "application/json; charset=utf-8"; }), 0},
        {with([](HttpRequestView& v) { v.origin = "http://localhost:3000"; }), 0},
        {with([](HttpRequestView& v) { v.target = "/mcp?x=1"; }), 0},
        {with([](HttpRequestView& v) { v.target = "/other"; }), 404},
        {with([](HttpRequestView& v) { v.method = "GET"; }), 405},
        {with([](HttpRequestView& v) { v.method = "DELETE"; }), 405},
        {with([](HttpRequestView& v) { v.host = "evil.com:4000"; }), 403},
        {with([](HttpRequestView& v) { v.host = "127.0.0.1:4001"; }), 403},
        {with([](HttpRequestView& v) { v.origin = "http://evil.com"; }), 403},
        {with([](HttpRequestView& v) { v.origin = "null"; }), 403},
        {with([](HttpRequestView& v) { v.authorization = ""; }), 401},
        {with([](HttpRequestView& v) { v.authorization = "Bearer wrong"; }), 401},
        {with([](HttpRequestView& v) { v.content_type = "text/plain"; }), 415},
        {with([](HttpRequestView& v) { v.body_size = 100; }), 413},
    }));
    CHECK(validate_request(c.view, 4000, "secret", 50).status == c.status);
}

TEST_CASE("an authorized POST reaches the handler and gets 200 JSON", "[Mcp]")
{
    EchoServer s;
    const Response r = send(s.server.port(), {});
    CHECK(r.status == 200);
    CHECK(r.body.find("ping") != std::string::npos);
}

TEST_CASE("the server rejects requests that break the endpoint rules", "[Mcp]")
{
    EchoServer s;
    const uint16_t port = s.server.port();
    Request no_token;
    no_token.token = "";
    const Response unauth = send(port, no_token);
    CHECK(unauth.status == 401);
    CHECK(unauth.www_authenticate == "Bearer");

    Request get;
    get.method = http::verb::get;
    const Response get_r = send(port, get);
    CHECK(get_r.status == 405);
    CHECK(get_r.allow == "POST");

    Request origin;
    origin.origin = "http://evil.example";
    CHECK(send(port, origin).status == 403);

    Request plain;
    plain.content_type = "text/plain";
    CHECK(send(port, plain).status == 415);
}

TEST_CASE("an incomplete request is closed after the read timeout", "[Mcp]")
{
    HttpLimits limits;
    limits.read_timeout = std::chrono::milliseconds(300);
    EchoServer s(limits);

    net::io_context ioc;
    tcp::socket     socket(ioc);
    socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), s.server.port()));
    const std::string partial = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    net::write(socket, net::buffer(partial));
    const auto start = std::chrono::steady_clock::now();
    char       byte;
    boost::system::error_code ec;
    socket.read_some(net::buffer(&byte, 1), ec);
    const auto waited = std::chrono::steady_clock::now() - start;
    CHECK(ec); // closed without a reply
    CHECK(waited < std::chrono::seconds(3));
}

TEST_CASE("stop() returns promptly with idle keep-alive connections and releases the port", "[Mcp]")
{
    auto           s    = std::make_unique<EchoServer>();
    const uint16_t port = s->server.port();
    net::io_context ioc;
    std::vector<std::unique_ptr<tcp::socket>> idle;
    for (int i = 0; i < 3; ++i) {
        idle.push_back(std::make_unique<tcp::socket>(ioc));
        idle.back()->connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    }
    const auto start = std::chrono::steady_clock::now();
    s->server.stop();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    s.reset();

    McpHttpServer again(kToken, [](const PostRequest&) { return PostReply{200, "{}"}; });
    std::string   err;
    CHECK(again.start(port, err));
}

TEST_CASE("a handler blocked on one connection does not delay a request on another", "[Mcp]")
{
    EchoServer     s;
    const uint16_t port = s.server.port();
    s.delay_ms          = 1500;
    auto slow           = std::async(std::launch::async, [port] { return send(port, {}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s.delay_ms       = 0;
    const auto start = std::chrono::steady_clock::now();
    CHECK(send(port, {}).status == 200);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
    CHECK(slow.get().status == 200);
}

TEST_CASE("at the connection cap a new connection evicts one waiting on its client", "[Mcp]")
{
    HttpLimits limits;
    limits.max_connections  = 4;
    limits.overflow_reserve = 1;
    EchoServer     s(limits);
    const uint16_t port = s.server.port();

    net::io_context ioc;
    std::vector<std::unique_ptr<tcp::socket>> waiting;
    for (int i = 0; i < 5; ++i) { // fills the cap and the reserve with incomplete requests
        waiting.push_back(std::make_unique<tcp::socket>(ioc));
        waiting.back()->connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
        net::write(*waiting.back(), net::buffer(std::string("POST /mcp HTTP/1.1\r\n")));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(send(port, {}).status == 200);
}

TEST_CASE("with every connection dispatched a new connection takes an overflow place, and beyond it is closed", "[Mcp]")
{
    HttpLimits limits;
    limits.max_connections  = 2;
    limits.overflow_reserve = 1;
    EchoServer     s(limits);
    const uint16_t port = s.server.port();
    s.delay_ms          = 1500;
    std::vector<std::future<Response>> busy;
    for (int i = 0; i < 2; ++i)
        busy.push_back(std::async(std::launch::async, [port] { return send(port, {}); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Third connection: nothing is evictable, so it takes the reserve place and is served.
    auto reserve = std::async(std::launch::async, [port] { return send(port, {}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Fourth: cap and reserve are all dispatched, so it is closed.
    bool closed = false;
    try {
        send(port, {});
    } catch (const std::exception&) {
        closed = true;
    }
    CHECK(closed);
    CHECK(reserve.get().status == 200);
    for (auto& b : busy)
        CHECK(b.get().status == 200);
}
