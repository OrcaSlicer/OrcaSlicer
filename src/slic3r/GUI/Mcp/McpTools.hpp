#pragma once

#include "McpTypes.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

class Plater;

namespace Mcp {

class McpService;

// Each tool family registers itself with the service.
void register_app_tools(McpService& service);
void register_action_tools(McpService& service);
void register_project_tools(McpService& service);
void register_config_tools(McpService& service);
void register_scene_tools(McpService& service);
void register_view_tools(McpService& service);
void register_ui_tools(McpService& service);
void register_script_tools(McpService& service);
void register_cad_tools(McpService& service);

// ---- schema helpers --------------------------------------------------------------------------

using Property = std::pair<const char*, nlohmann::json>;

// {"type":"object","properties":{...},"required":[...],"additionalProperties":false}
nlohmann::json object_schema(std::initializer_list<Property> properties, std::vector<std::string> required = {});
nlohmann::json prop(const char* type, const std::string& description);
nlohmann::json enum_prop(std::vector<std::string> values, const std::string& description);
nlohmann::json array_prop(nlohmann::json items, const std::string& description);

// Pure observation: inline, read-only, inspect guard, not journaled.
ToolDef inspect_tool(std::string name, std::string title, std::string description, nlohmann::json schema, ToolHandler handler);
// Anything that changes the app: a job under the mutate guard, journaled.
ToolDef mutate_tool(std::string name, std::string title, std::string description, nlohmann::json schema, ToolHandler handler,
                    bool destructive = false);

// ---- handler helpers -------------------------------------------------------------------------

// One more UI round trip after a mutation, so events it queued are handled before the reply.
void settle(ToolContext& ctx);

// The plater, or ToolFailure while the main window is not up yet (startup, language change).
// UI thread only.
Plater& ui_plater();

// Typed argument access with defaults; schema validation has already checked the types.
std::string arg_string(const nlohmann::json& args, const char* key, const std::string& fallback = {});
int         arg_int(const nlohmann::json& args, const char* key, int fallback);
double      arg_double(const nlohmann::json& args, const char* key, double fallback);
bool        arg_bool(const nlohmann::json& args, const char* key, bool fallback);

// A setting value from JSON in the serialized form set_deserialize accepts: "3", "0.2", "1"/"0"
// for booleans, "a,b" for arrays. A list of strings (`key` names a coStrings option, such as
// post_process) is quoted and separated by semicolons, as ConfigOptionStrings writes it, since its
// entries may contain commas. Throws ToolFailure for values with no such form.
std::string to_config_string(const nlohmann::json& value, const std::string& key = {});

// Page a JSON array: {total, offset, items:[...]}.
nlohmann::json page(const nlohmann::json& items, const nlohmann::json& args, std::size_t default_limit = 100);

// Waits for a G-code export started between Plater export sequence numbers `before` and `after`
// (unchanged = it never started: `rejection` explains why) and reports how it ended; `revision` is
// the project revision when it started.
ToolResult await_gcode_export(ToolContext& ctx, uint64_t before, uint64_t after, uint64_t revision, const std::string& rejection);

// Polls `poll` on the UI thread every 200 ms until it returns a non-null value or `timeout`
// passes; throws ToolFailure on timeout or cancellation.
nlohmann::json poll_ui(ToolContext& ctx, const std::function<nlohmann::json()>& poll, std::chrono::milliseconds timeout,
                       const std::string& what);

}}} // namespace Slic3r::GUI::Mcp
