#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include "MCP/McpProtocol.h"
#include "MCP/ToolSchemas.h"

using json = nlohmann::json;
using namespace Slic3r::GUI;

namespace {

// Helper: create an initialized McpProtocol with all tools
std::unique_ptr<McpProtocol> make_protocol() {
    auto p = std::make_unique<McpProtocol>();
    p->init(get_all_tool_definitions());
    return p;
}

// Helper: send a JSON-RPC request and parse the response
json send_request(McpProtocol& p, const json& req,
                  const std::map<std::string, std::string>& headers = {}) {
    auto resp = p.handle_mcp_request(req.dump(), headers);
    if (resp.body.empty()) return json::object();
    return json::parse(resp.body);
}

// Helper: perform initialize handshake, return session_id
std::string do_initialize(McpProtocol& p) {
    json init_req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}
        }}
    };
    auto resp = p.handle_mcp_request(init_req.dump(), {});
    std::string session_id;
    auto it = resp.extra_headers.find("Mcp-Session-Id");
    if (it != resp.extra_headers.end()) session_id = it->second;

    // Send initialized notification
    json notif = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    p.handle_mcp_request(notif.dump(), {{"mcp-session-id", session_id}});

    return session_id;
}

} // anonymous namespace

TEST_CASE("JSON-RPC parsing and responses", "[MCP][JSONRPC]") {
    auto proto_ptr = make_protocol();
    auto& proto = *proto_ptr;

    SECTION("Valid JSON-RPC initialize request returns jsonrpc 2.0 and matching id") {
        json req = {
            {"jsonrpc", "2.0"}, {"id", 42}, {"method", "initialize"},
            {"params", {
                {"protocolVersion", "2025-03-26"},
                {"capabilities", json::object()},
                {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}
            }}
        };
        auto resp = proto.handle_mcp_request(req.dump(), {});
        REQUIRE(resp.status_code == 200);

        auto body = json::parse(resp.body);
        REQUIRE(body["jsonrpc"] == "2.0");
        REQUIRE(body["id"] == 42);
        REQUIRE(body.contains("result"));
    }

    SECTION("Invalid JSON returns parse error -32700") {
        auto resp = proto.handle_mcp_request("{broken json!!!", {});
        REQUIRE(resp.status_code == 400);

        auto body = json::parse(resp.body);
        REQUIRE(body.contains("error"));
        REQUIRE(body["error"]["code"] == -32700);
    }

    SECTION("Missing method returns invalid request -32600") {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}};
        auto resp = send_request(proto, req);
        REQUIRE(resp.contains("error"));
        REQUIRE(resp["error"]["code"] == -32600);
    }

    SECTION("Notification (no id) for initialized returns 202 status") {
        // First initialize to get a session
        json init_req = {
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
            {"params", {
                {"protocolVersion", "2025-03-26"},
                {"capabilities", json::object()},
                {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}
            }}
        };
        auto init_resp = proto.handle_mcp_request(init_req.dump(), {});
        std::string session_id = init_resp.extra_headers.at("Mcp-Session-Id");

        // Send notification (no id)
        json notif = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
        auto resp = proto.handle_mcp_request(notif.dump(), {{"mcp-session-id", session_id}});
        REQUIRE(resp.status_code == 202);
        REQUIRE(resp.body.empty());
    }

    SECTION("Success response contains result field") {
        std::string session_id = do_initialize(proto);

        json req = {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "tools/list"}};
        auto resp = send_request(proto, req, {{"mcp-session-id", session_id}});
        REQUIRE(resp.contains("result"));
        REQUIRE_FALSE(resp.contains("error"));
    }

    SECTION("Error response contains error.code and error.message") {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "nonexistent/method"}};
        std::string session_id = do_initialize(proto);
        auto resp = send_request(proto, req, {{"mcp-session-id", session_id}});
        REQUIRE(resp.contains("error"));
        REQUIRE(resp["error"].contains("code"));
        REQUIRE(resp["error"].contains("message"));
        REQUIRE(resp["error"]["code"].is_number());
        REQUIRE(resp["error"]["message"].is_string());
    }
}
