#pragma once

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// Default port of the MCP endpoint; 13618 is the OAuth callback server.
inline constexpr int kDefaultPort = 13619;

// MCP settings from the command line. Script paths are absolute: OrcaSlicer.cpp resolves them
// against the original working directory before the GUI changes it.
struct McpLaunchOptions
{
    bool        enable = false;   // --mcp
    int         port   = 0;       // --mcp-port; 0 = the preference
    std::string token;            // --mcp-token; ORCA_MCP_TOKEN takes precedence
    std::string script;           // --mcp-script
    std::string script_out;       // --mcp-script-out
};

}}} // namespace Slic3r::GUI::Mcp
