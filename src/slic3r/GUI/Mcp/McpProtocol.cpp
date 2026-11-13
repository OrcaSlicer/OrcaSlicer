#include "McpProtocol.hpp"
#include "McpTypes.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

constexpr int kParseError       = -32700;
constexpr int kInvalidRequest   = -32600;
constexpr int kMethodNotFound   = -32601;
constexpr int kInvalidParams    = -32602;
constexpr int kInternalError    = -32603;
constexpr int kHeaderMismatch   = -32020;
constexpr int kUnsupportedProto = -32022;

constexpr const char* kMetaVersion = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kMetaServer  = "io.modelcontextprotocol/serverInfo";
// How long a modern client may cache the tool list and discovery result. The catalog is fixed for
// the life of the process, so the value only bounds how long a stale client survives a restart into
// a different build.
constexpr int kListTtlMs = 300000;

json error_body(const json& id, int code, const std::string& message, json data = json())
{
    json err{{"code", code}, {"message", message}};
    if (!data.is_null())
        err["data"] = std::move(data);
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(err)}};
}

PostReply reply(int status, const json& body) { return {status, body.dump()}; }

PostReply result_reply(const json& id, json result) { return reply(200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}); }

json supported_versions()
{
    json v = json::array({kModernVersion});
    for (const std::string& legacy : legacy_versions())
        v.push_back(legacy);
    return v;
}

int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

// ---- call_tool ----------------------------------------------------------------

std::string resolve_call(const CommandCatalog& catalog, std::shared_ptr<const NativeCommand>& command, json& args)
{
    if (!command || command->key != "call_tool")
        return {};
    const std::string                    target = args.value("tool", std::string());
    std::shared_ptr<const NativeCommand> inner  = catalog.lookup(target);
    if (!inner)
        return catalog.unknown_command(target, /*suggest=*/true);
    if (inner->key == "call_tool")
        return "call_tool calls another tool, not itself";
    json inner_args = args.contains("args") && args["args"].is_object() ? args["args"] : json::object();
    if (std::string err = argument_error(*inner, inner_args); !err.empty())
        return "Invalid arguments for " + target + ": " + err;
    command = std::move(inner);
    args    = std::move(inner_args);
    return {};
}

std::string decode_header_value(const std::string& value)
{
    static const std::string prefix = "=?base64?", suffix = "?=";
    if (value.size() < prefix.size() + suffix.size() || value.compare(0, prefix.size(), prefix) != 0 ||
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0)
        return value;
    std::string out;
    uint32_t    acc  = 0;
    int         bits = 0;
    for (std::size_t i = prefix.size(); i < value.size() - suffix.size(); ++i) {
        const int v = base64_value(value[i]);
        if (v < 0)
            continue; // padding
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// ---- McpProtocol --------------------------------------------------------------

McpProtocol::McpProtocol(const CommandCatalog& catalog, ToolCaller& caller, ServerInfo info)
    : m_catalog(catalog), m_caller(caller), m_info(std::move(info))
{}

json McpProtocol::tool_to_json(const NativeCommand& def)
{
    json t{{"name", def.key}, {"description", def.description}, {"inputSchema", def.input_schema}};
    if (!def.title.empty())
        t["title"] = def.title;
    t["annotations"] = {{"readOnlyHint", def.read_only},
                        {"destructiveHint", def.destructive},
                        {"idempotentHint", def.idempotent},
                        {"openWorldHint", def.open_world}};
    if (!def.title.empty())
        t["annotations"]["title"] = def.title;
    return t;
}

json McpProtocol::result_to_json(const ToolResult& result)
{
    json content = json::array();
    if (result.is_error) {
        std::string text = result.message;
        if (!result.data.is_null() && !(result.data.is_object() && result.data.empty()))
            text += "\n" + result.data.dump();
        content.push_back({{"type", "text"}, {"text", text}});
    } else {
        content.push_back({{"type", "text"}, {"text", result.data.dump()}});
    }
    for (const json& block : result.content)
        content.push_back(block);
    json out{{"content", std::move(content)}};
    if (result.is_error)
        out["isError"] = true;
    return out;
}

json McpProtocol::tools_list() const
{
    // The listed commands, in the order they were added.
    json tools = json::array();
    for (const NativeCommand& def : m_catalog.commands())
        if (def.listed)
            tools.push_back(tool_to_json(def));
    return tools;
}

json McpProtocol::call_tool(const json& params, bool& unknown_tool, std::string& unknown_name)
{
    unknown_tool          = false;
    const std::string                    name = params.value("name", "");
    std::shared_ptr<const NativeCommand> def  = m_catalog.lookup(name);
    if (def == nullptr) {
        unknown_tool = true;
        unknown_name = name;
        return json();
    }
    json args = params.contains("arguments") && !params["arguments"].is_null() ? params["arguments"] : json::object();
    if (!args.is_object())
        return result_to_json(ToolResult::error("arguments must be an object"));
    if (std::string err = argument_error(*def, args); !err.empty())
        return result_to_json(ToolResult::error("Invalid arguments: " + err));
    if (std::string err = resolve_call(m_catalog, def, args); !err.empty())
        return result_to_json(ToolResult::error(err));
    try {
        return result_to_json(m_caller.call(*def, args));
    } catch (const ToolFailure& f) {
        return result_to_json(ToolResult::error(f.what(), f.details));
    } catch (const std::exception& e) {
        return result_to_json(ToolResult::error(std::string("Internal error: ") + e.what()));
    }
}

PostReply McpProtocol::handle_post(const PostRequest& request)
{
    json msg = json::parse(request.body, nullptr, false);
    if (msg.is_discarded())
        return reply(400, error_body(nullptr, kParseError, "Parse error"));
    if (msg.is_array())
        return reply(400, error_body(nullptr, kInvalidRequest, "Batches are not supported"));
    // Compared as JSON, not read with value(), which throws when the member is not a string.
    if (!msg.is_object() || !msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0")
        return reply(400, error_body(nullptr, kInvalidRequest, "Not a JSON-RPC 2.0 message"));

    // Responses (to requests this server never sends) and notifications are accepted silently.
    if (!msg.contains("method") || !msg["method"].is_string())
        return msg.contains("result") || msg.contains("error") ? PostReply{202, {}}
                                                               : reply(400, error_body(nullptr, kInvalidRequest, "Missing method"));
    if (!msg.contains("id"))
        return {202, {}};

    const json& id = msg["id"];
    if (!id.is_string() && !id.is_number_integer() && !id.is_number_unsigned())
        return reply(400, error_body(nullptr, kInvalidRequest, "id must be a string or an integer"));

    const std::string method = msg["method"].get<std::string>();
    if (msg.contains("params") && !msg["params"].is_object())
        return reply(200, error_body(id, kInvalidParams, "params must be an object"));
    const json params = msg.contains("params") ? msg["params"] : json::object();

    try {
        if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains(kMetaVersion)) {
            const json& v = params["_meta"][kMetaVersion];
            return handle_modern(id, method, params, v.is_string() ? v.get<std::string>() : v.dump(), request);
        }
        return handle_legacy(id, method, params, request);
    } catch (const std::exception& e) {
        return reply(500, error_body(id, kInternalError, e.what()));
    }
}

PostReply McpProtocol::handle_legacy(const json& id, const std::string& method, const json& params, const PostRequest& request)
{
    const auto& versions = legacy_versions();
    if (method != "initialize" && !request.protocol_version.empty() &&
        std::find(versions.begin(), versions.end(), request.protocol_version) == versions.end())
        return reply(400, error_body(id, kInvalidRequest, "Unsupported MCP-Protocol-Version: " + request.protocol_version,
                                     {{"supported", versions}}));

    if (method == "initialize") {
        const std::string requested  = params.value("protocolVersion", "");
        const bool        supported  = std::find(versions.begin(), versions.end(), requested) != versions.end();
        json              server_info{{"name", m_info.name}, {"title", m_info.title}, {"version", m_info.version}};
        return result_reply(id, {{"protocolVersion", supported ? requested : versions.front()},
                                 {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                 {"serverInfo", std::move(server_info)},
                                 {"instructions", m_info.instructions}});
    }
    if (method == "ping")
        return result_reply(id, json::object());
    if (method == "tools/list")
        return result_reply(id, {{"tools", tools_list()}});
    if (method == "tools/call") {
        bool        unknown = false;
        std::string name;
        json        result = call_tool(params, unknown, name);
        if (unknown) {
            json data{{"name", name}};
            if (auto close = m_catalog.similar(name); !close.empty())
                data["did_you_mean"] = close;
            return reply(200, error_body(id, kInvalidParams, m_catalog.unknown_command(name), std::move(data)));
        }
        return result_reply(id, std::move(result));
    }
    return reply(200, error_body(id, kMethodNotFound, "Method not found: " + method));
}

PostReply McpProtocol::handle_modern(const json& id, const std::string& method, const json& params, const std::string& version,
                                     const PostRequest& request)
{
    if (version != kModernVersion)
        return reply(400, error_body(id, kUnsupportedProto, "Unsupported protocol version",
                                     {{"supported", supported_versions()}, {"requested", version}}));
    if (request.protocol_version.empty())
        return reply(400, error_body(id, kHeaderMismatch, "Header mismatch: MCP-Protocol-Version header is required"));
    if (request.protocol_version != version)
        return reply(400, error_body(id, kHeaderMismatch,
                                     "Header mismatch: MCP-Protocol-Version header value '" + request.protocol_version +
                                         "' does not match body value '" + version + "'"));
    if (request.method_header.empty())
        return reply(400, error_body(id, kHeaderMismatch, "Header mismatch: Mcp-Method header is required"));
    if (request.method_header != method)
        return reply(400, error_body(id, kHeaderMismatch,
                                     "Header mismatch: Mcp-Method header value '" + request.method_header +
                                         "' does not match body value '" + method + "'"));
    if (method == "tools/call" || method == "prompts/get" || method == "resources/read") {
        const std::string body_name = method == "resources/read" ? params.value("uri", "") : params.value("name", "");
        if (request.name_header.empty())
            return reply(400, error_body(id, kHeaderMismatch, "Header mismatch: Mcp-Name header is required"));
        const std::string header_name = decode_header_value(request.name_header);
        if (header_name != body_name)
            return reply(400, error_body(id, kHeaderMismatch,
                                         "Header mismatch: Mcp-Name header value '" + header_name + "' does not match body value '" +
                                             body_name + "'"));
    }

    auto complete = [](json result) {
        result["resultType"] = "complete";
        return result;
    };

    if (method == "server/discover") {
        json result{{"supportedVersions", supported_versions()},
                    {"capabilities", {{"tools", json::object()}}},
                    {"_meta", {{kMetaServer, {{"name", m_info.name}, {"title", m_info.title}, {"version", m_info.version}}}}},
                    {"instructions", m_info.instructions},
                    {"ttlMs", kListTtlMs},
                    {"cacheScope", "public"}};
        return result_reply(id, complete(std::move(result)));
    }
    if (method == "ping")
        return result_reply(id, complete(json::object()));
    if (method == "tools/list")
        return result_reply(id, complete({{"tools", tools_list()}, {"ttlMs", kListTtlMs}, {"cacheScope", "public"}}));
    if (method == "tools/call") {
        bool        unknown = false;
        std::string name;
        json        result = call_tool(params, unknown, name);
        if (unknown) {
            json data{{"name", name}};
            if (auto close = m_catalog.similar(name); !close.empty())
                data["did_you_mean"] = close;
            return reply(200, error_body(id, kInvalidParams, m_catalog.unknown_command(name), std::move(data)));
        }
        return result_reply(id, complete(std::move(result)));
    }
    return reply(404, error_body(id, kMethodNotFound, "Method not found: " + method));
}

}}} // namespace Slic3r::GUI::Mcp
