#pragma once

// The window names the MCP tools read. A header of their own, so that a window that sets one includes nothing more.
namespace Slic3r { namespace GUI { namespace Mcp {

// The name of a top-level window the MCP GUI tools never see: widget_list and locators skip it,
// webview_run_javascript never picks its web views, keys sent without a target never go to it, window screenshots leave it
// out, and as a modal dialog it is not tracked, so the dialog tools and auto-answer rules never reach
// it. Action Replay carries it, so a replay it runs can't act on it, and so does the window that
// changes the Python scripts setting, so no agent, replay or script can turn it on.
inline constexpr char kHiddenFromAutomation[] = "OrcaHiddenFromMcp";

// The name of a text field that holds a secret its dialog shows in plain text, such as a printer's
// access code: a modal dialog's description writes its value as [******] (describe_dialog()). A field
// with password style, or a setting field for a password or key, needs no name.
inline constexpr char kSecretField[] = "OrcaSecretForMcp";

}}} // namespace Slic3r::GUI::Mcp
