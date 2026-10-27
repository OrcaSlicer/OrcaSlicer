#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// `orca-slicer --mcp-stdio [--datadir DIR] [--mcp-port N]`: an MCP server on stdin/stdout for
// clients that only speak stdio (Claude Desktop, for one). Each newline-delimited JSON-RPC message
// is POSTed to the running OrcaSlicer's endpoint found through <datadir>/mcp/server.json; when none
// answers, OrcaSlicer is started with --mcp and given up to 60 s. Runs before any GUI setup.
int run_stdio_bridge(int argc, char** argv);

// The HTTP headers a Streamable HTTP client derives from a message: MCP-Protocol-Version (the
// message's _meta version, else the version `initialize` negotiated), Mcp-Method and, for
// tools/call, prompts/get and resources/read, Mcp-Name (base64-sentinel encoded when not plain
// ASCII).
std::map<std::string, std::string> bridge_headers(const nlohmann::json& message, const std::string& negotiated_version);

}}} // namespace Slic3r::GUI::Mcp
