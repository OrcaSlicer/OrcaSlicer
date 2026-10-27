#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

// Runs handlers directly on the calling thread, as if every call were inline.
class DirectCaller final : public ToolCaller
{
public:
    ToolResult call(const ToolDef& def, const json& args) override
    {
        struct Context final : ToolContext
        {
            std::string id;
            json run_ui(const std::function<json()>& fn) override { return fn(); }
            const std::string& job_id() const override { return id; }
            bool cancelled() const override { return false; }
            ToolResult call_child(const std::string&, const json&) override { return ToolResult::error("no children"); }
            std::set<std::string> lineage() const override { return {}; }
            void set_journal_args(json) override {}
        } ctx;
        return def.handler(args, ctx);
    }
};

struct Fixture
{
    ToolRegistry registry;
    DirectCaller caller;
    McpProtocol  protocol{registry, caller, ServerInfo{"orcaslicer", "OrcaSlicer", "9.9.9", "Use get_state first."}};

    Fixture()
    {
        ToolDef echo;
        echo.name         = "echo";
        echo.description  = "Echo text back.";
        echo.read_only    = true;
        echo.idempotent   = true;
        echo.input_schema = {{"type", "object"},
                             {"properties", {{"text", {{"type", "string"}}}}},
                             {"required", {"text"}},
                             {"additionalProperties", false}};
        echo.handler      = [](const json& args, ToolContext&) { return ToolResult::ok({{"echo", args["text"]}}); };
        registry.add(echo);

        ToolDef fail;
        fail.name    = "fail";
        fail.handler = [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("it broke", {{"why", "test"}}); };
        registry.add(fail);
    }

    PostReply post(const json& msg, const std::string& version = {}, const std::string& method = {}, const std::string& name = {})
    {
        return protocol.handle_post(PostRequest{msg.dump(), version, method, name});
    }
};

json body(const PostReply& r) { return json::parse(r.body); }

json modern_params(json params = json::object())
{
    params["_meta"] = {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}};
    return params;
}

} // namespace

TEST_CASE("initialize echoes a supported client version", "[Mcp]")
{
    Fixture f;
    const PostReply r = f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-06-18"}}}});
    REQUIRE(r.status == 200);
    const json result = body(r)["result"];
    CHECK(result["protocolVersion"] == "2025-06-18");
    CHECK(result["serverInfo"]["name"] == "orcaslicer");
    CHECK(result["capabilities"].contains("tools"));
    CHECK(result["instructions"] == "Use get_state first.");
}

TEST_CASE("initialize answers an unsupported version with the latest supported one", "[Mcp]")
{
    Fixture f;
    for (const char* requested : {"2025-03-26", "1999-01-01"}) {
        const json result = body(f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", requested}}}}))["result"];
        CHECK(result["protocolVersion"] == "2025-11-25");
    }
}

TEST_CASE("notifications are accepted with 202 and no body", "[Mcp]")
{
    Fixture f;
    const PostReply r = f.post({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    CHECK(r.status == 202);
    CHECK(r.body.empty());
}

TEST_CASE("tools/list keeps registration order and maps annotations", "[Mcp]")
{
    Fixture f;
    const json tools = body(f.post({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}))["result"]["tools"];
    REQUIRE(tools.size() == 2);
    CHECK(tools[0]["name"] == "echo");
    CHECK(tools[1]["name"] == "fail");
    CHECK(tools[0]["annotations"]["readOnlyHint"] == true);
    CHECK(tools[0]["annotations"]["idempotentHint"] == true);
    CHECK(tools[1]["annotations"]["destructiveHint"] == false);
    CHECK(tools[0]["inputSchema"]["required"][0] == "text");
}

TEST_CASE("tools/call returns the result as a JSON text block", "[Mcp]")
{
    Fixture f;
    const json result = body(f.post({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", {{"name", "echo"}, {"arguments", {{"text", "hi"}}}}}}))["result"];
    CHECK_FALSE(result.contains("isError"));
    CHECK_FALSE(result.contains("structuredContent"));
    REQUIRE(result["content"].size() == 1);
    CHECK(json::parse(result["content"][0]["text"].get<std::string>()) == json{{"echo", "hi"}});
}

TEST_CASE("tool failures and invalid arguments are isError results", "[Mcp]")
{
    Fixture f;
    const json failed = body(f.post({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "fail"}}}}))["result"];
    CHECK(failed["isError"] == true);
    CHECK(failed["content"][0]["text"].get<std::string>().find("it broke") != std::string::npos);

    const json bad = body(f.post({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"}, {"params", {{"name", "echo"}, {"arguments", {{"txt", "hi"}}}}}}))["result"];
    CHECK(bad["isError"] == true);
    const std::string text = bad["content"][0]["text"];
    CHECK(text.find("text is required") != std::string::npos);
}

TEST_CASE("an unknown tool is a JSON-RPC error with suggestions", "[Mcp]")
{
    Fixture f;
    const json reply = body(f.post({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/call"}, {"params", {{"name", "ecko"}}}}));
    CHECK(reply["error"]["code"] == -32602);
    CHECK(reply["error"]["data"]["did_you_mean"][0] == "echo");
}

TEST_CASE("malformed messages are rejected", "[Mcp]")
{
    Fixture f;
    PostReply r = f.protocol.handle_post(PostRequest{"{not json", {}, {}, {}});
    CHECK(r.status == 400);
    CHECK(body(r)["error"]["code"] == -32700);

    r = f.post(json::array({{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}}));
    CHECK(r.status == 400);
    CHECK(body(r)["error"]["code"] == -32600);

    r = f.post({{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}});
    CHECK(body(r)["error"]["code"] == -32600);

    r = f.post({{"jsonrpc", "1.0"}, {"id", 1}, {"method", "ping"}});
    CHECK(body(r)["error"]["code"] == -32600);
}

TEST_CASE("ping returns an empty result", "[Mcp]")
{
    Fixture f;
    CHECK(body(f.post({{"jsonrpc", "2.0"}, {"id", "p"}, {"method", "ping"}}))["result"] == json::object());
}

TEST_CASE("a legacy request with an unsupported MCP-Protocol-Version header is 400", "[Mcp]")
{
    Fixture f;
    const PostReply r = f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, "2024-01-01");
    CHECK(r.status == 400);
    CHECK(f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, "2025-11-25").status == 200);
}

TEST_CASE("2026-07-28 server/discover lists versions and capabilities", "[Mcp]")
{
    Fixture f;
    const PostReply r = f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "server/discover"}, {"params", modern_params()}}, "2026-07-28", "server/discover");
    REQUIRE(r.status == 200);
    const json result = body(r)["result"];
    CHECK(result["resultType"] == "complete");
    CHECK(result["supportedVersions"][0] == "2026-07-28");
    CHECK(result["capabilities"].contains("tools"));
    CHECK(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "orcaslicer");
    CHECK(result.contains("ttlMs"));
    CHECK(result.contains("cacheScope"));
}

TEST_CASE("2026-07-28 results carry resultType complete", "[Mcp]")
{
    Fixture f;
    const json list = body(f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", modern_params()}}, "2026-07-28", "tools/list"))["result"];
    CHECK(list["resultType"] == "complete");
    CHECK(list.contains("ttlMs"));
    CHECK(list.contains("cacheScope"));

    const json call = body(f.post({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                                    {"params", modern_params({{"name", "echo"}, {"arguments", {{"text", "x"}}}})}},
                                   "2026-07-28", "tools/call", "echo"))["result"];
    CHECK(call["resultType"] == "complete");
}

TEST_CASE("2026-07-28 header mismatches are 400 -32020", "[Mcp]")
{
    Fixture f;
    const json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", modern_params({{"name", "echo"}, {"arguments", {{"text", "x"}}}})}};
    PostReply r = f.post(req, "2026-07-28", "tools/list", "echo");
    CHECK(r.status == 400);
    CHECK(body(r)["error"]["code"] == -32020);

    r = f.post(req, "2026-07-28", "tools/call", "");
    CHECK(body(r)["error"]["code"] == -32020);

    r = f.post(req, "", "tools/call", "echo");
    CHECK(body(r)["error"]["code"] == -32020);

    // Mcp-Name may arrive base64-encoded.
    r = f.post(req, "2026-07-28", "tools/call", "=?base64?ZWNobw==?=");
    CHECK(r.status == 200);
}

TEST_CASE("2026-07-28 unsupported _meta version is 400 -32022 with the supported list", "[Mcp]")
{
    Fixture f;
    json params = modern_params();
    params["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2030-01-01";
    const PostReply r = f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", params}}, "2030-01-01", "tools/list");
    CHECK(r.status == 400);
    const json err = body(r)["error"];
    CHECK(err["code"] == -32022);
    CHECK(err["data"]["requested"] == "2030-01-01");
    CHECK(err["data"]["supported"][0] == "2026-07-28");
}

TEST_CASE("2026-07-28 unknown method is 404 -32601", "[Mcp]")
{
    Fixture f;
    const PostReply r = f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "resources/list"}, {"params", modern_params()}}, "2026-07-28", "resources/list");
    CHECK(r.status == 404);
    CHECK(body(r)["error"]["code"] == -32601);
}

TEST_CASE("argument validation covers types, enums, ranges and arrays", "[Mcp]")
{
    const json schema = {{"type", "object"},
                         {"properties",
                          {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}},
                           {"mode", {{"type", "string"}, {"enum", {"a", "b"}}}},
                           {"paths", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                         {"additionalProperties", false}};
    CHECK(validate_arguments(schema, {{"n", 3}, {"mode", "a"}, {"paths", {"x"}}}).empty());
    CHECK(validate_arguments(schema, {{"n", 0}}).find("n must be >= 1") != std::string::npos);
    CHECK(validate_arguments(schema, {{"n", 2.5}}).find("integer") != std::string::npos);
    CHECK(validate_arguments(schema, {{"mode", "c"}}).find("one of") != std::string::npos);
    CHECK(validate_arguments(schema, {{"paths", {1}}}).find("paths[0]") != std::string::npos);
    CHECK(validate_arguments(schema, {{"extra", 1}}).find("unknown argument extra") != std::string::npos);
}
