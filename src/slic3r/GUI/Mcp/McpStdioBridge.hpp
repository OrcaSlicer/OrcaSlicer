#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// `orca-slicer --mcp-stdio [--datadir DIR] [--mcp-port N]`: an MCP server on stdin/stdout for
// clients that only speak stdio (Claude Desktop, for one). Each newline-delimited JSON-RPC message
// is POSTed to the running OrcaSlicer's endpoint found through <datadir>/mcp/server.json. While none
// answers, every request but tools/call gets the reply this build's server would give, from its own
// catalog (McpService::offline_catalog()), and a tools/call starts OrcaSlicer with --mcp, giving it
// up to 60 s: an agent session that never calls a tool never opens the app. Runs before any GUI setup.
int run_stdio_bridge(int argc, char** argv);

// The HTTP headers a Streamable HTTP client derives from a message: MCP-Protocol-Version (the
// message's _meta version, else the version `initialize` negotiated), Mcp-Method and, for
// tools/call, prompts/get and resources/read, Mcp-Name (base64-sentinel encoded when not plain
// ASCII).
std::map<std::string, std::string> bridge_headers(const nlohmann::json& message, const std::string& negotiated_version);

}}} // namespace Slic3r::GUI::Mcp
