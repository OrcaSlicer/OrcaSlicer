#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/Mcp/McpService.hpp"
#include "slic3r/GUI/Mcp/McpStdioBridge.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "libslic3r_version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::CommandSource;
using Slic3r::GUI::NativeCommand;
namespace NativeCommands = Slic3r::GUI::NativeCommands;

namespace {

// Runs handlers directly on the calling thread, as if every call were inline.
class DirectCaller final : public ToolCaller
{
public:
    std::vector<std::string> called; // the tools it ran, in order

    ToolResult call(const NativeCommand& def, const json& args) override
    {
        called.push_back(def.key);
        struct Context final : ToolContext
        {
            std::string id;
            json run_ui(const std::function<json()>& fn) override { return fn(); }
            json observe_ui(const std::function<json()>& fn) override { return fn(); }
            const std::string& job_id() const override { return id; }
            bool cancelled() const override { return false; }
            ToolResult call_child(const std::string&, const json&) override { return ToolResult::error("no children"); }
            std::set<std::string> lineage() const override { return {}; }
            void set_journal_args(json) override {}
            void begin_operation() override {}
        } ctx;
        return def.run(args, ctx);
    }
};

struct Fixture
{
    CommandCatalog registry;
    DirectCaller   caller;
    McpProtocol    protocol{registry, caller, ServerInfo{"orcaslicer", "OrcaSlicer", "9.9.9", "Use get_app_state first."}};

    Fixture()
    {
        NativeCommand echo;
        echo.key          = "echo";
        echo.listed       = true;
        echo.description  = "Echo text back.";
        echo.read_only    = true;
        echo.idempotent   = true;
        echo.input_schema = {{"type", "object"},
                             {"properties", {{"text", {{"type", "string"}}}}},
                             {"required", {"text"}},
                             {"additionalProperties", false}};
        echo.run          = [](const json& args, ToolContext&) { return ToolResult::ok({{"echo", args["text"]}}); };
        registry.add(echo);

        NativeCommand fail;
        fail.key         = "fail";
        fail.listed      = true;
        fail.destructive = true;
        fail.open_world  = true;
        fail.run         = [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("it broke", {{"why", "test"}}); };
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
    CHECK(result["instructions"] == "Use get_app_state first.");
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
    CHECK(tools[0]["annotations"]["destructiveHint"] == false);
    CHECK(tools[0]["annotations"]["openWorldHint"] == false);
    CHECK(tools[1]["annotations"]["readOnlyHint"] == false);
    CHECK(tools[1]["annotations"]["destructiveHint"] == true);
    CHECK(tools[1]["annotations"]["openWorldHint"] == true);
    CHECK(tools[0]["inputSchema"]["required"][0] == "text");
}

TEST_CASE("tools/list lists the listed commands in the order they were added", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    DirectCaller          caller;
    McpProtocol           protocol{catalog, caller, ServerInfo{}};
    const json tools = body(protocol.handle_post(PostRequest{json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}.dump(), {}, {}, {}}))
                           ["result"]["tools"];
    std::vector<std::string> names;
    for (const json& tool : tools)
        names.push_back(tool["name"].get<std::string>());

    std::vector<std::string> added;
    for (const NativeCommand& def : catalog.commands())
        if (def.listed)
            added.push_back(def.key);
    CHECK(names == added);

    // The families in the order register_all_commands() adds them: the Speed Dial's commands before them are
    // not listed.
    REQUIRE_FALSE(names.empty());
    CHECK(names.front() == "get_app_state");
    const auto position = [&names](const std::string& name) {
        const auto it = std::find(names.begin(), names.end(), name);
        REQUIRE(it != names.end());
        return it - names.begin();
    };
    CHECK(position("search_tools") < position("open_files"));
    CHECK(position("slice") < position("click"));
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

TEST_CASE("one edit distance serves every did-you-mean", "[Mcp]")
{
    CHECK(edit_distance("fillet", "filet") == 1);
    CHECK(edit_distance("", "abc") == 3);
    CHECK(edit_distance("a", "a") == 0);
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

    // A jsonrpc that is not a string is the same error, never an exception: the stdio bridge calls
    // handle_post directly.
    for (const json& version : {json(2), json(nullptr), json::object()}) {
        r = f.post({{"jsonrpc", version}, {"id", 1}, {"method", "ping"}});
        CHECK(r.status == 400);
        CHECK(body(r)["error"]["code"] == -32600);
    }
}

TEST_CASE("params that are not an object are invalid params", "[Mcp]")
{
    Fixture   f;
    PostReply r = f.post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::array()}});
    CHECK(body(r)["error"]["code"] == -32602);
    r = f.post({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}, {"params", 3}});
    CHECK(body(r)["error"]["code"] == -32602);
}

TEST_CASE("the stdio bridge derives no headers from a line that is not a message object", "[Mcp]")
{
    CHECK(bridge_headers(json::array({{{"method", "ping"}}}), "2025-06-18").empty());
    CHECK(bridge_headers(json(42), {}).empty());
    CHECK(bridge_headers({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "get_app_state"}}}}, "2025-06-18")
              .count("MCP-Protocol-Version") == 1);
    // Members of the wrong type are left out rather than thrown on.
    CHECK(bridge_headers({{"jsonrpc", "2.0"}, {"id", 1}, {"method", 5}}, "").empty());
    CHECK(bridge_headers({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", 3}}}}, "").count("Mcp-Name") == 0);
}

TEST_CASE("the catalog the stdio bridge lists without the app holds every tool family", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    std::set<std::string> names;
    for (const NativeCommand& def : catalog.commands()) {
        if (!def.listed)
            continue;
        CHECK(names.insert(def.key).second);
    }
    for (const char* name : {"get_app_state", "run_action", "open_files", "set_settings", "slice", "set_height_ranges", "set_view",
                             "list_widgets", "run_replay", "start_recording", "run_script", "call_tool", "set_variable_layer_height",
                             "set_custom_gcode_at_layer"})
        CHECK(names.count(name) == 1);
    // Built once, for the life of the process.
    CHECK(&NativeCommands::instance() == &catalog);
    CHECK(McpService::server_info().version == SLIC3R_VERSION);
    CHECK_FALSE(McpService::server_info().instructions.empty());
    // Clients cut the instructions off after 4096 characters.
    CHECK(McpService::server_info().instructions.size() <= 4096);
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
                           {"zoom", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                           {"mode", {{"type", "string"}, {"enum", {"a", "b"}}}},
                           {"paths", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                         {"additionalProperties", false}};
    CHECK(validate_arguments(schema, {{"n", 3}, {"mode", "a"}, {"paths", {"x"}}}).empty());
    CHECK(validate_arguments(schema, {{"n", 0}}).find("n must be >= 1") != std::string::npos);
    CHECK(validate_arguments(schema, {{"zoom", 0}}).find("zoom must be > 0") != std::string::npos);
    CHECK(validate_arguments(schema, {{"zoom", 0.5}}).empty());
    CHECK(validate_arguments(schema, {{"n", 2.5}}).find("integer") != std::string::npos);
    CHECK(validate_arguments(schema, {{"mode", "c"}}).find("one of") != std::string::npos);
    CHECK(validate_arguments(schema, {{"paths", {1}}}).find("paths[0]") != std::string::npos);
    CHECK(validate_arguments(schema, {{"extra", 1}}).find("unknown argument extra") != std::string::npos);

    // A plugin operation's draft-04 schema gives exclusiveMinimum as a boolean that qualifies minimum.
    const json draft4 = {{"type", "object"}, {"properties", {{"zoom", {{"type", "number"}, {"minimum", 0}, {"exclusiveMinimum", true}}}}}};
    std::string draft4_error;
    CHECK_NOTHROW(draft4_error = validate_arguments(draft4, {{"zoom", 0.5}}));
    CHECK(draft4_error.empty());
}

TEST_CASE("argument normalization reads the names agents reach for as the ones a tool takes", "[Mcp]")
{
    const json schema = {{"type", "object"},
                         {"properties",
                          {{"target", {{"type", {"string", "object"}}}},
                           {"button", {{"type", "string"}}},
                           {"objects", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
                         {"additionalProperties", false}};
    const auto normalized = [&](json args) {
        normalize_arguments(schema, args);
        return args;
    };
    CHECK(normalized({{"ref", "w12"}}) == json{{"target", "w12"}});
    // action stands for id or button, whichever the tool takes.
    CHECK(normalized({{"action", "close"}}) == json{{"button", "close"}});
    CHECK(normalized({{"object", 2}}) == json{{"objects", {2}}});
    CHECK(normalized({{"object", "0"}}) == json{{"objects", {0}}});
    // Given both, the call keeps them and validation names the stray one.
    const json both = normalized({{"ref", "w12"}, {"target", "w13"}});
    CHECK(both == json{{"ref", "w12"}, {"target", "w13"}});
    CHECK(validate_arguments(schema, both).find("unknown argument ref") != std::string::npos);
    // A tool that takes the name itself keeps it.
    const json own = {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}, {"value", {{"type", "string"}}}}}};
    json       text{{"text", "abc"}};
    normalize_arguments(own, text);
    CHECK(text == json{{"text", "abc"}});
    // Only a value that fits: a click is no button, so the call keeps action for the validator to name.
    const json clicks = {{"type", "object"},
                         {"properties", {{"button", {{"type", "string"}, {"enum", {"left", "right", "middle"}}}}}},
                         {"additionalProperties", false}};
    json       click{{"action", "click"}};
    normalize_arguments(clicks, click);
    CHECK(click == json{{"action", "click"}});
}

TEST_CASE("argument normalization reads a capture's output path as its save_path", "[Mcp]")
{
    const auto normalized = [](const json& properties, json args) {
        normalize_arguments({{"type", "object"}, {"properties", properties}, {"additionalProperties", false}}, args);
        return args;
    };
    // A capture's output file is its save_path; a tool with a path of its own keeps that.
    const json capture = {{"save_path", {{"type", "string"}}}};
    CHECK(normalized(capture, {{"path", "/a.png"}}) == json{{"save_path", "/a.png"}});
    const json encode = {{"path", {{"type", "string"}}}, {"save_path", {{"type", "string"}}}};
    CHECK(normalized(encode, {{"path", "/a.mkv"}}) == json{{"path", "/a.mkv"}});
}

TEST_CASE("argument normalization leaves a value its enum lacks, and a 3D view under another name, for the validator", "[Mcp]")
{
    const json schema = {{"type", "object"},
                         {"properties",
                          {{"scope", {{"type", "string"}, {"enum", {"process", "filament", "part"}}}},
                           {"target", {{"type", "string"}, {"enum", {"main", "canvas"}}}},
                           {"canvas", {{"type", "string"}, {"enum", {"3d", "preview"}}}}}},
                         {"additionalProperties", false}};
    const auto normalized = [&](json args) {
        normalize_arguments(schema, args);
        return args;
    };
    // No value stands for another: the process scope and the main window go by those names alone.
    CHECK(normalized({{"scope", "print"}}) == json{{"scope", "print"}});
    CHECK(normalized({{"target", "window"}}) == json{{"target", "window"}});
    CHECK_FALSE(validate_arguments(schema, normalized({{"scope", "print"}})).empty());
    // set_view's 3D view is its canvas.
    const json view = normalized({{"view", "preview"}});
    CHECK(view == json{{"view", "preview"}});
    CHECK(validate_arguments(schema, view).find("unknown argument view") != std::string::npos);
}

TEST_CASE("argument normalization reads values written in another JSON form", "[Mcp]")
{
    const json schema = {{"type", "object"},
                         {"properties",
                          {{"n", {{"type", "integer"}}},
                           {"flag", {{"type", "boolean"}}},
                           {"plate", {{"type", {"string", "integer"}}}},
                           {"point", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                           {"paths", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                           {"name", {{"type", "string"}}}}},
                         {"additionalProperties", false}};
    json args{{"n", "3"}, {"flag", "true"}, {"plate", "1"}, {"point", "[1, 2.5, 3]"}, {"paths", "/a.stl"}, {"name", "7"}};
    normalize_arguments(schema, args);
    CHECK(args["n"] == 3);
    CHECK(args["flag"] == true);
    CHECK(args["plate"] == 1);
    CHECK(args["point"] == json{1, 2.5, 3});
    CHECK(args["paths"] == json{"/a.stl"});
    // A string where a string goes stays one.
    CHECK(args["name"] == "7");
    CHECK(validate_arguments(schema, args).empty());

    json kept{{"plate", "all"}, {"n", "2.5"}};
    normalize_arguments(schema, kept);
    CHECK(kept["plate"] == "all");
    CHECK(validate_arguments(schema, kept).find("n must be of type integer") != std::string::npos);
}

TEST_CASE("argument normalization moves a value outside its enum to the argument whose enum takes it", "[Mcp]")
{
    const json schema = {{"type", "object"},
                         {"properties",
                          {{"view", {{"type", "string"}, {"enum", {"3d", "preview"}}}},
                           {"camera", {{"type", "string"}, {"enum", {"front", "top", "fit"}}}},
                           {"tab", {{"type", "string"}, {"enum", {"prepare", "preview"}}}}}},
                         {"additionalProperties", false}};
    json moved{{"view", "front"}};
    normalize_arguments(schema, moved);
    CHECK(moved == json{{"camera", "front"}});

    // Both given: the call names its camera, so view's value is an error, not a second camera.
    json both{{"view", "front"}, {"camera", "fit"}};
    normalize_arguments(schema, both);
    CHECK(both["view"] == "front");
    CHECK(validate_arguments(schema, both).find("view must be one of") != std::string::npos);

    // A value of its own enum stays, even where another enum also takes it.
    json own{{"view", "preview"}};
    normalize_arguments(schema, own);
    CHECK(own == json{{"view", "preview"}});

    // Taken by no other enum: left for the validator.
    json unknown{{"view", "sideways"}};
    normalize_arguments(schema, unknown);
    CHECK(unknown == json{{"view", "sideways"}});
}

TEST_CASE("pairs list_windows reports are read as position and size", "[Mcp]")
{
    const json schema = {{"type", "object"},
                         {"properties", {{"position", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                                         {"size", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
                         {"additionalProperties", false}};
    json flat{{"x", 10}, {"y", 20}, {"width", 800}, {"height", 600}};
    normalize_arguments(schema, flat);
    CHECK(flat == json{{"position", {10, 20}}, {"size", {800, 600}}});
    json half{{"x", 10}};
    normalize_arguments(schema, half);
    CHECK(half == json{{"x", 10}});                        // left for the validator to name
    json given{{"x", 1}, {"y", 2}, {"position", {3, 4}}};
    normalize_arguments(schema, given);
    CHECK(given["position"] == json{3, 4});                // never replaces a given argument
    const json own = {{"type", "object"}, {"properties", {{"x", {{"type", "integer"}}}, {"y", {{"type", "integer"}}}}}};
    json kept{{"x", 1}, {"y", 2}};
    normalize_arguments(own, kept);
    CHECK(kept == json{{"x", 1}, {"y", 2}});               // a tool that takes x and y keeps them
}

TEST_CASE("a position or size in mm, an array of numbers, takes no pair list_windows reports", "[Mcp]")
{
    // add_part's position and size, and transform's position.
    const json schema = {{"type", "object"},
                         {"properties", {{"position", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                                         {"size", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
                         {"additionalProperties", false}};
    const json flat{{"x", 10}, {"y", 20}, {"width", 30}, {"height", 5}};
    json       args = flat;
    normalize_arguments(schema, args);
    CHECK(args == flat);
    CHECK(validate_arguments(schema, args).find("unknown argument") != std::string::npos);
    // transform's size, whose null keeps an axis.
    const json nullable = {{"type", "object"},
                           {"properties", {{"size", {{"type", "array"}, {"items", {{"type", {"number", "null"}}}}}}}},
                           {"additionalProperties", false}};
    json sized{{"width", 30}, {"height", 5}};
    normalize_arguments(nullable, sized);
    CHECK(sized == json{{"width", 30}, {"height", 5}});
}

TEST_CASE("an unknown tool is answered with the closest names", "[Mcp]")
{
    Fixture f;
    const json direct = body(f.post({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"}, {"params", {{"name", "no_such_tool"}}}}));
    CHECK(direct["error"]["message"] == "Unknown tool: no_such_tool");

    NativeCommand api;
    api.key          = "call_tool";
    api.input_schema = {{"type", "object"},
                        {"properties", {{"tool", {{"type", "string"}}}, {"args", {{"type", "object"}}}}},
                        {"required", {"tool"}}};
    api.run          = [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("not dispatched"); };
    f.registry.add(api);
    // call_tool's target names the closest command, and nothing else.
    const json typo = body(f.post({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"},
                                   {"params", {{"name", "call_tool"}, {"arguments", {{"tool", "ech"}}}}}}))["result"];
    CHECK(typo["isError"] == true);
    CHECK(typo["content"][0]["text"] == "Unknown tool: ech (did you mean echo?)");
}

TEST_CASE("a call's arguments are normalized, then validated", "[Mcp]")
{
    NativeCommand capture;
    capture.key          = "screenshot";
    capture.input_schema = {{"type", "object"},
                            {"properties", {{"target", {{"type", "string"}}}, {"save_path", {{"type", "string"}}}}},
                            {"additionalProperties", false}};
    // A name agents reach for is read as the one the tool takes.
    json aliased{{"path", "/a.png"}};
    CHECK(argument_error(capture, aliased).empty());
    CHECK(aliased == json{{"save_path", "/a.png"}});
    // An argument the tool does not take is refused with the ones it does.
    json unknown{{"colour", "red"}};
    const std::string refused = argument_error(capture, unknown);
    CHECK(refused.find("unknown argument colour") != std::string::npos);
    CHECK(refused.find("save_path") != std::string::npos);
}

TEST_CASE("call_tool runs and is journaled as the tool it names", "[Mcp]")
{
    Fixture f;
    NativeCommand api;
    api.key          = "call_tool";
    api.input_schema = {{"type", "object"},
                        {"properties", {{"tool", {{"type", "string"}}}, {"args", {{"type", "object"}}}}},
                        {"required", {"tool"}}};
    api.run          = [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("not dispatched"); };
    f.registry.add(api);

    const json result = body(f.post({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
                                     {"params", {{"name", "call_tool"}, {"arguments", {{"tool", "echo"}, {"args", {{"text", "hi"}}}}}}}}))["result"];
    CHECK(json::parse(result["content"][0]["text"].get<std::string>()) == json{{"echo", "hi"}});
    REQUIRE(f.caller.called.size() == 1);
    CHECK(f.caller.called.back() == "echo");

    // The target's arguments are checked against its schema, and an unknown tool is named.
    const json invalid = body(f.post({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                                      {"params", {{"name", "call_tool"}, {"arguments", {{"tool", "echo"}, {"args", json::object()}}}}}}))["result"];
    CHECK(invalid["isError"] == true);
    const json unknown = body(f.post({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/call"},
                                      {"params", {{"name", "call_tool"}, {"arguments", {{"tool", "ech"}}}}}}))["result"];
    CHECK(unknown["isError"] == true);
    CHECK(unknown["content"][0]["text"].get<std::string>().find("did you mean echo") != std::string::npos);
}

// Holds the one operation a plugin offers while it is loaded.
class MeasureSource final : public CommandSource
{
public:
    MeasureSource()
    {
        auto area = std::make_shared<NativeCommand>();
        area->key = "plugin.measure.area";
        area->run = [](const json&, ToolContext&) { return ToolResult::ok({{"area", 12.5}}); };
        m_area    = std::move(area);
    }
    std::shared_ptr<const NativeCommand> find(const std::string& key) const override { return key == m_area->key ? m_area : nullptr; }
    std::vector<std::shared_ptr<const NativeCommand>> all() const override { return {m_area}; }

private:
    std::shared_ptr<const NativeCommand> m_area;
};

TEST_CASE("a name the table lacks resolves through a source, without being listed", "[Mcp]")
{
    Fixture f;
    f.registry.add_source(std::make_unique<MeasureSource>());
    REQUIRE(f.registry.lookup("plugin.measure.area") != nullptr);
    CHECK(f.registry.lookup("plugin.measure.volume") == nullptr);
    CHECK(f.registry.lookup("echo")->key == "echo");
    const json tools = body(f.post({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}))["result"]["tools"];
    for (const json& t : tools)
        CHECK(t["name"] != "plugin.measure.area");
    const json result = body(f.post({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                                     {"params", {{"name", "plugin.measure.area"}, {"arguments", json::object()}}}}))["result"];
    CHECK(json::parse(result["content"][0]["text"].get<std::string>()) == json{{"area", 12.5}});
}
