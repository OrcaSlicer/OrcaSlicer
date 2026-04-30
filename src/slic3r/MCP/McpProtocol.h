#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include "MCP/McpApiServer.h"
#include "MCP/ToolSchemas.h"

#define MCP_PROTOCOL_VERSION "2025-03-26"
#define MCP_SERVER_NAME      "orca-mcp-server"
#define MCP_SERVER_VERSION   "0.2.0"

namespace Slic3r { namespace GUI {

class CommandDispatch;

class McpProtocol {
public:
    using json = nlohmann::json;

    McpProtocol();

    // Initialize with tool definitions
    void init(const std::vector<McpToolDef>& tools);

    // Main entry: handles POST /mcp requests
    // Returns an HTTP response (JSON-RPC over HTTP)
    McpApiServer::Response handle_mcp_request(
        const std::string& body,
        const std::map<std::string, std::string>& headers);

private:
    // JSON-RPC method handlers
    json handle_initialize(const json& params);
    json handle_tools_list();
    json handle_tools_call(const json& params);
    json handle_ping();

    // JSON-RPC helpers
    json make_result(int id, const json& result);
    json make_error(int id, int code, const std::string& message);

    // Session management
    std::string generate_session_id();
    bool is_valid_session(const std::string& session_id);
    bool is_initialized_session(const std::string& session_id);

    // Tool lookup
    const McpToolDef* find_tool(const std::string& name) const;

    // State
    std::vector<McpToolDef> m_tools;
    std::map<std::string, bool> m_sessions; // session_id → initialized
    std::mutex m_mutex;
};

}} // namespace Slic3r::GUI
