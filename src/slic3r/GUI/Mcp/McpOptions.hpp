#pragma once

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// Default port of the MCP endpoint; 13618 is the OAuth callback server.
inline constexpr int kDefaultPort = 13619;

// The app config key of the unrestricted Python scripts setting: one switch, off unless true. While it is on,
// the scripts a person runs (the Run Script panel, --mcp-run) and agents send (run_script) skip the plugin
// permission checks. Only the window show_python_scripts_dialog() opens writes it; no tool does.
inline constexpr char kPythonUnrestrictedKey[] = "mcp_python_unrestricted";

// MCP settings from the command line. Paths are absolute: OrcaSlicer.cpp resolves them
// against the original working directory before the GUI changes it.
struct McpLaunchOptions
{
    bool        enable = false;   // --mcp
    int         port   = 0;       // --mcp-port; 0 = the preference
    std::string token;            // --mcp-token; ORCA_MCP_TOKEN takes precedence
    std::string run_file;         // --mcp-run: a replay (.json) or a Python script (.py)
    std::string run_out;          // --mcp-run-out
};

}}} // namespace Slic3r::GUI::Mcp
