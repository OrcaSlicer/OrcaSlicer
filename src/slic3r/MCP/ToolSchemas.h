#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class McpProtocol; // forward declaration

struct McpToolDef {
    std::string      name;            // MCP tool name (e.g. "screenshot")
    std::string      description;     // Human-readable description
    nlohmann::json   input_schema;    // JSON Schema for inputSchema
    std::string      dispatch_action; // CommandDispatch action name (empty for special tools)
    bool             is_screenshot = false;
};

// Returns all tool definitions
std::vector<McpToolDef> get_all_tool_definitions();

}} // namespace Slic3r::GUI
