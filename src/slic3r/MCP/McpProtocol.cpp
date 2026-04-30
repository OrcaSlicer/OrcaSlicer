#include "MCP/McpProtocol.h"
#include "MCP/CommandDispatch.h"

#include <random>
#include <sstream>
#include <iomanip>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

// Helper: build an HTTP response with JSON body
static McpApiServer::Response json_http_response(const std::string& body, int status = 200,
                                                  const std::string& session_id = "") {
    McpApiServer::Response resp;
    resp.status_code = status;
    resp.status_text = (status == 200) ? "OK" : (status == 202) ? "Accepted" : "Bad Request";
    resp.content_type = "application/json";
    resp.body = body;
    // Session ID header is added by McpApiServer via a custom header mechanism
    // For now we embed it in the response body for the initialized response
    return resp;
}

McpProtocol::McpProtocol() = default;

void McpProtocol::init(const std::vector<McpToolDef>& tools) {
    m_tools = tools;
}

std::string McpProtocol::generate_session_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(16) << dist(gen)
       << std::setw(16) << dist(gen);
    return ss.str();
}

bool McpProtocol::is_valid_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.find(session_id) != m_sessions.end();
}

bool McpProtocol::is_initialized_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(session_id);
    return it != m_sessions.end() && it->second;
}

const McpToolDef* McpProtocol::find_tool(const std::string& name) const {
    for (const auto& tool : m_tools) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

json McpProtocol::make_result(int id, const json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

json McpProtocol::make_error(int id, int code, const std::string& message) {
    json err = {{"jsonrpc", "2.0"}, {"error", {{"code", code}, {"message", message}}}};
    if (id >= 0) err["id"] = id;
    else err["id"] = nullptr;
    return err;
}

// ---------------------------------------------------------------------------
// MCP method handlers
// ---------------------------------------------------------------------------

json McpProtocol::handle_initialize(const json& /*params*/) {
    json capabilities = {
        {"tools", json::object()}
    };

    return {
        {"protocolVersion", MCP_PROTOCOL_VERSION},
        {"capabilities", capabilities},
        {"serverInfo", {
            {"name", MCP_SERVER_NAME},
            {"version", MCP_SERVER_VERSION}
        }}
    };
}

json McpProtocol::handle_tools_list() {
    json tools_array = json::array();
    for (const auto& tool : m_tools) {
        tools_array.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"inputSchema", tool.input_schema}
        });
    }
    return {{"tools", tools_array}};
}

json McpProtocol::handle_tools_call(const json& params) {
    std::string tool_name = params.value("name", "");
    json arguments = params.value("arguments", json::object());

    const McpToolDef* tool = find_tool(tool_name);
    if (!tool) {
        return json{{"error_code", -32602}, {"error_message", "Unknown tool: " + tool_name}};
    }

    auto& cmd = CommandDispatch::instance();

    if (tool->is_screenshot) {
        // Screenshot: build query string from arguments, call handle_screenshot
        std::string url = "/api/screenshot?";
        if (arguments.contains("width"))
            url += "width=" + std::to_string(arguments["width"].get<int>()) + "&";
        if (arguments.contains("height"))
            url += "height=" + std::to_string(arguments["height"].get<int>()) + "&";
        if (arguments.contains("mode"))
            url += "mode=" + arguments["mode"].get<std::string>() + "&";

        auto http_resp = cmd.handle_screenshot(url);

        if (http_resp.status_code != 200) {
            return {
                {"content", json::array({{{"type", "text"}, {"text", http_resp.body}}})},
                {"isError", true}
            };
        }

        // Convert binary PNG to base64
        std::string base64;
        {
            static const char* b64chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const auto& data = http_resp.binary_body;
            size_t i = 0;
            base64.reserve((data.size() + 2) / 3 * 4);
            for (; i + 2 < data.size(); i += 3) {
                uint32_t n = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
                base64 += b64chars[(n >> 18) & 0x3F];
                base64 += b64chars[(n >> 12) & 0x3F];
                base64 += b64chars[(n >> 6) & 0x3F];
                base64 += b64chars[n & 0x3F];
            }
            if (i < data.size()) {
                uint32_t n = data[i] << 16;
                if (i + 1 < data.size()) n |= data[i+1] << 8;
                base64 += b64chars[(n >> 18) & 0x3F];
                base64 += b64chars[(n >> 12) & 0x3F];
                base64 += (i + 1 < data.size()) ? b64chars[(n >> 6) & 0x3F] : '=';
                base64 += '=';
            }
        }

        return {
            {"content", json::array({{{"type", "image"}, {"data", base64}, {"mimeType", "image/png"}}})},
            {"isError", false}
        };
    }

    // Normal tool: dispatch via CommandDispatch
    json result = cmd.dispatch(tool->dispatch_action, arguments);

    if (result.contains("error") && !result.contains("ok")) {
        return {
            {"content", json::array({{{"type", "text"}, {"text", result["error"].get<std::string>()}}})},
            {"isError", true}
        };
    }

    return {
        {"content", json::array({{{"type", "text"}, {"text", result.dump()}}})},
        {"isError", false}
    };
}

json McpProtocol::handle_ping() {
    return json::object();
}

// ---------------------------------------------------------------------------
// Main request handler
// ---------------------------------------------------------------------------

McpApiServer::Response McpProtocol::handle_mcp_request(
    const std::string& body,
    const std::map<std::string, std::string>& headers)
{
    // Parse JSON-RPC
    json req;
    try {
        req = json::parse(body);
    } catch (const json::exception&) {
        auto err = make_error(-1, -32700, "Parse error");
        return json_http_response(err.dump(), 400);
    }

    // Validate basic JSON-RPC structure
    if (!req.contains("method") || !req["method"].is_string()) {
        auto err = make_error(-1, -32600, "Invalid Request: missing method");
        return json_http_response(err.dump(), 400);
    }

    std::string method = req["method"].get<std::string>();
    json params = req.value("params", json::object());
    bool has_id = req.contains("id");
    int id = has_id ? req.value("id", 0) : -1;

    // Get session ID from headers
    std::string session_id;
    {
        auto it = headers.find("mcp-session-id");
        if (it != headers.end()) session_id = it->second;
    }

    // --- Handle initialize (no session required) ---
    if (method == "initialize") {
        if (!has_id) {
            auto err = make_error(-1, -32600, "Initialize must be a request (needs id)");
            return json_http_response(err.dump(), 400);
        }

        std::string new_session_id = generate_session_id();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sessions[new_session_id] = false; // not yet initialized
        }

        json result = handle_initialize(params);
        json response = make_result(id, result);

        McpApiServer::Response http_resp;
        http_resp.status_code = 200;
        http_resp.status_text = "OK";
        http_resp.content_type = "application/json";
        http_resp.body = response.dump();
        // Store session ID in a custom header field
        // We'll add Mcp-Session-Id as a response header
        http_resp.extra_headers["Mcp-Session-Id"] = new_session_id;
        return http_resp;
    }

    // --- Handle initialized notification ---
    if (method == "notifications/initialized") {
        if (!session_id.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_sessions.find(session_id);
            if (it != m_sessions.end()) {
                it->second = true; // mark as initialized
            }
        }

        McpApiServer::Response resp;
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.content_type = "application/json";
        resp.body = "";
        return resp;
    }

    // --- Handle ping (no session required) ---
    if (method == "ping") {
        if (!has_id) {
            McpApiServer::Response resp;
            resp.status_code = 202;
            resp.status_text = "Accepted";
            resp.body = "";
            return resp;
        }
        json result = handle_ping();
        return json_http_response(make_result(id, result).dump());
    }

    // --- Session validation ---
    // Local single-user server: skip strict session enforcement.
    // The initialize/initialized handshake is still supported for spec compliance,
    // but we don't reject requests with missing or stale session IDs.

    // --- tools/list ---
    if (method == "tools/list") {
        if (!has_id) {
            auto err = make_error(-1, -32600, "tools/list must be a request");
            return json_http_response(err.dump(), 400);
        }
        json result = handle_tools_list();
        return json_http_response(make_result(id, result).dump());
    }

    // --- tools/call ---
    if (method == "tools/call") {
        if (!has_id) {
            auto err = make_error(-1, -32600, "tools/call must be a request");
            return json_http_response(err.dump(), 400);
        }
        json result = handle_tools_call(params);

        // Check if this is an internal error (from find_tool failure)
        if (result.contains("error_code")) {
            auto err = make_error(id, result["error_code"].get<int>(),
                                  result["error_message"].get<std::string>());
            return json_http_response(err.dump(), 400);
        }

        return json_http_response(make_result(id, result).dump());
    }

    // --- Unknown method ---
    auto err = make_error(id, -32601, "Method not found: " + method);
    return json_http_response(err.dump(), 400);
}

}} // namespace Slic3r::GUI
