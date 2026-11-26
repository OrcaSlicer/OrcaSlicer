#pragma once

class wxWindow;

namespace Slic3r {

class AppConfig;

namespace GUI { namespace Mcp {

// The saved setting.
bool python_scripts_setting(const AppConfig& config);
// The value in force: the setting, or the command line's --unrestricted-scripts for this run.
bool python_scripts_unrestricted(const AppConfig& config);
// Called once at startup with the command line's flag.
void set_python_scripts_for_run(bool unrestricted);
bool python_scripts_set_for_run();

// Passes the value in force on to the plugin audit, which judges a script's accesses by it. Called once the app
// config is loaded, and by the setting's window on a change.
void apply_python_scripts_setting(const AppConfig& config);

// Opens the window that lets Python scripts run unrestricted or not, modal on `parent`. It shows and writes the
// saved setting. The window is hidden from automation (kHiddenFromAutomation), so no MCP tool, script or plugin
// can find, operate or answer it: only a person changes the setting. Returns whether it changed. UI thread.
bool show_python_scripts_dialog(wxWindow* parent);

}}} // namespace Slic3r::GUI::Mcp
