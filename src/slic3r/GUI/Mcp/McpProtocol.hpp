#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// Revision served statelessly to requests that carry the per-request `_meta` version.
inline constexpr const char* kModernVersion = "2026-07-28";
// Revisions negotiated through the `initialize` handshake, newest first.
inline const std::vector<std::string>& legacy_versions()
{
    static const std::vector<std::string> versions{"2025-11-25", "2025-06-18"};
    return versions;
}

struct ServerInfo
{
    std::string name = "orcaslicer";
    std::string title = "OrcaSlicer";
    std::string version;
    std::string instructions;
};

// One POSTed message and the transport headers the protocol checks. Header values are empty when
// the header was absent.
struct PostRequest
{
    std::string body;
    std::string protocol_version; // MCP-Protocol-Version
    std::string method_header;    // Mcp-Method
    std::string name_header;      // Mcp-Name
};

// Body is empty for 202 Accepted.
struct PostReply
{
    int         status = 200;
    std::string body;
};

// The MCP protocol layer: JSON-RPC 2.0 framing and the MCP methods of both eras, independent of
// the HTTP transport and of wx. It never throws; every failure becomes a JSON-RPC error or an
// isError tool result.
class McpProtocol
{
public:
    McpProtocol(const ToolRegistry& registry, ToolCaller& caller, ServerInfo info);

    PostReply handle_post(const PostRequest& request);

    static nlohmann::json tool_to_json(const ToolDef& def);
    static nlohmann::json result_to_json(const ToolResult& result);

private:
    PostReply handle_legacy(const nlohmann::json& id, const std::string& method, const nlohmann::json& params,
                            const PostRequest& request);
    PostReply handle_modern(const nlohmann::json& id, const std::string& method, const nlohmann::json& params,
                            const std::string& version, const PostRequest& request);
    nlohmann::json call_tool(const nlohmann::json& params, bool& unknown_tool, std::string& unknown_name);
    nlohmann::json tools_list() const;

    const ToolRegistry& m_registry;
    ToolCaller&         m_caller;
    ServerInfo          m_info;
};

// Decodes the `=?base64?...?=` sentinel form of an Mcp-Name / Mcp-Param header value; plain values
// pass through unchanged.
std::string decode_header_value(const std::string& value);

}}} // namespace Slic3r::GUI::Mcp
