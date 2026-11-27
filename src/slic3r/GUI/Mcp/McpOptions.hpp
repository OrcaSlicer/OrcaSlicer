#pragma once

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// Default port of the MCP endpoint; 13618 is the OAuth callback server.
inline constexpr int kDefaultPort = 13619;

// The app config key of the unrestricted Python scripts setting: one switch, off unless true. While it is on,
// or the command line's --unrestricted-scripts is, the scripts a person runs (the Run Script panel, --run) and
// agents send (script_run) skip the plugin permission checks. Only the window show_python_scripts_dialog() opens
// writes it; no tool and no command line option does.
inline constexpr char kPythonUnrestrictedKey[] = "python_scripts_unrestricted";

// The endpoint's and the run's settings from the command line. Paths are absolute: OrcaSlicer.cpp resolves them
// against the original working directory before the GUI changes it.
struct McpLaunchOptions
{
    bool        enable = false;   // --mcp, or --mcp-port given
    int         port   = 0;       // --mcp-port; 0 = the preference
    std::string run_file;         // --run: a replay (.json) or a Python script (.py)
    std::string run_out;          // --outputdir, when --run is given
};

}}} // namespace Slic3r::GUI::Mcp
