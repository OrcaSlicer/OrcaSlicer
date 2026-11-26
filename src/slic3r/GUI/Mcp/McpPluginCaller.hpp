#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

class CommandCatalog;
struct NativeCommand;

namespace Mcp {

// Lets plugins call the tools through orca.host.tools: a call from a plugin's Script worker runs as
// McpCore::call_plugin(), starting the automation runtime without the MCP endpoint when the server is off, and is
// logged. A plugin without the tools permission or full access is refused before anything starts. Called once by
// GUI_App as it sets up plugins.
void install_plugin_tool_caller();

// What a plugin's orca.host.tools.call(tool, args) runs: the command `tool` names in `catalog`, `args` normalized and
// checked against its schema, a tool_call resolved to its target and that target's arguments. ToolCallError
// (slic3r/plugin/host/PluginHostTools.hpp) for an unknown tool, with did_you_mean, or for arguments the schema refuses,
// with argument_refusal()'s message and details. Any thread.
std::shared_ptr<const NativeCommand> resolve_plugin_call(const CommandCatalog& catalog, const std::string& tool, nlohmann::json& args);

}}} // namespace Slic3r::GUI::Mcp
