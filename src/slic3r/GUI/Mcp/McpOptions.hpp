#pragma once

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// Default port of the MCP endpoint; 13618 is the OAuth callback server.
inline constexpr int kDefaultPort = 13619;

// The app config key of the Python scripts setting: one switch, off unless true, that lets a person run
// .py scripts (the Run Script dialog, --mcp-run) and agents send run_script. Only the window
// show_python_scripts_dialog() opens writes it; no tool does.
inline constexpr char kPythonScriptsKey[] = "mcp_python_scripts";

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
