#pragma once

class wxWindow;

namespace Slic3r {

class AppConfig;

namespace GUI { namespace Mcp {

// Whether the Python scripts setting is on in `config`.
bool python_scripts_allowed(const AppConfig& config);

// Opens the window that turns Python scripts on or off, modal on `parent`. The window is hidden from
// automation (kHiddenFromAutomation), so no MCP tool, script or plugin can find, operate or answer it:
// only a person changes the setting. Returns whether it changed. UI thread.
bool show_python_scripts_dialog(wxWindow* parent);

}}} // namespace Slic3r::GUI::Mcp
