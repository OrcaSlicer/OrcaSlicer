#pragma once

namespace Slic3r { namespace GUI { namespace Mcp {

// Lets plugins call the tools through orca.host.tools: a call from a plugin's Script worker runs as
// McpCore::call_plugin(), starting the automation runtime without the MCP endpoint when the server is off, and is
// logged. Called once by GUI_App as it sets up plugins.
void install_plugin_tool_caller();

}}} // namespace Slic3r::GUI::Mcp
