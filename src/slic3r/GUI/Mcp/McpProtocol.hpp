#pragma once

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
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

// What initialize and server/discover report: the server's name, version and instructions (McpServerInfo.cpp).
ServerInfo server_info();

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

// A tools/call the protocol refused before the caller saw it (an unknown tool, arguments its schema refuses, a
// tool_call it could not resolve): the tool the client named, the call's origin as the caller would have got it
// (McpCore::call()), and the error the client was sent.
using RefusedCall = std::function<void(const std::string& tool, const nlohmann::json& origin, const ToolResult& error)>;

// The MCP protocol layer: JSON-RPC 2.0 framing and the MCP methods of both eras, independent of
// the HTTP transport and of wx. It never throws; every failure becomes a JSON-RPC error or an
// isError tool result.
//
// Every tools/call reaches the caller with its origin: `{source: "agent", client, request}`, where `request` is the
// JSON-RPC message as received and `client` the name the client gave in `initialize`, while only one name has been
// given since the protocol was made; once a second name comes, or for a call of the 2026-07-28 era, which has no
// handshake, `client` is empty, since the call could be either client's.
class McpProtocol
{
public:
    // tools/list shows the commands of `catalog` that are listed; tools/call reaches every command it holds.
    McpProtocol(const CommandCatalog& catalog, ToolCaller& caller, ServerInfo info);

    PostReply handle_post(const PostRequest& request);
    // Told about each tools/call refused before the caller, on the request's thread; what it throws is dropped.
    // Set before the first request.
    void set_on_refused(RefusedCall on_refused) { m_on_refused = std::move(on_refused); }

    static nlohmann::json tool_to_json(const NativeCommand& command);
    // The MCP result of a tool's outcome, its numbers rounded to what they say (reply_number()) unless `exact`.
    static nlohmann::json result_to_json(const ToolResult& result, bool exact = false);

private:
    // `message` is a request whose id, method and params handle_post() checked.
    PostReply handle_legacy(const nlohmann::json& message, const PostRequest& request);
    PostReply handle_modern(const nlohmann::json& message, const std::string& version, const PostRequest& request);
    // `message` is the whole tools/call request, `client` the name its origin carries.
    nlohmann::json call_tool(const nlohmann::json& message, const std::string& client, bool& unknown_tool, std::string& unknown_name);
    nlohmann::json tools_list() const;
    // Remembers the client name an initialize gave; the name a legacy call is labelled with.
    void        client_seen(const nlohmann::json& params);
    std::string client_label() const;
    void        refused(const std::string& tool, const nlohmann::json& origin, const ToolResult& error) const;

    const CommandCatalog& m_catalog;
    ToolCaller&           m_caller;
    ServerInfo            m_info;
    RefusedCall           m_on_refused;
    mutable std::mutex    m_clients_mutex;
    std::set<std::string> m_clients;
};

// Decodes the `=?base64?...?=` sentinel form of an Mcp-Name / Mcp-Param header value; plain values
// pass through unchanged.
std::string decode_header_value(const std::string& value);

}}} // namespace Slic3r::GUI::Mcp
