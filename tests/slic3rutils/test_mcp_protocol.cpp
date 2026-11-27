#include <catch2/catch_test_macros.hpp>

#include "automation/automation_test_utils.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "libslic3r_version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

// The protocol's cases that read the catalog NativeCommands builds. The others are in
// tests/automation/test_mcp_protocol.cpp.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::NativeCommand;
using Slic3r::GUI::Mcp::Test::DirectCaller;
namespace NativeCommands = Slic3r::GUI::NativeCommands;

namespace {

json body(const PostReply& r) { return json::parse(r.body); }

} // namespace

TEST_CASE("tools/list lists the listed commands in the order they were added", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    DirectCaller          caller;
    McpProtocol           protocol{catalog, caller, ServerInfo{}};
    const json tools = body(protocol.handle_post(PostRequest{json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}.dump(), {}, {}, {}}))
                           ["result"]["tools"];
    std::vector<std::string> names;
    for (const json& tool : tools)
        names.push_back(tool["name"].get<std::string>());

    std::vector<std::string> added;
    for (const NativeCommand& def : catalog.commands())
        if (def.listed)
            added.push_back(def.key);
    CHECK(names == added);

    // The listing of the other commands first, then the manual, the generic call and the app's state, then the commands
    // by place, the GUI-driving ones last.
    REQUIRE_FALSE(names.empty());
    CHECK(names.front() == "tool_list");
    const auto position = [&names](const std::string& name) {
        const auto it = std::find(names.begin(), names.end(), name);
        REQUIRE(it != names.end());
        return it - names.begin();
    };
    CHECK(position("tool_describe") < position("project_open"));
    CHECK(position("plate_slice") < position("widget_click"));
}

TEST_CASE("the catalog the stdio bridge lists without the app holds every tool family", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    std::set<std::string> names;
    for (const NativeCommand& def : catalog.commands()) {
        if (!def.listed)
            continue;
        CHECK(names.insert(def.key).second);
    }
    for (const char* name : {"app_state", "action_run", "project_open", "settings_set", "plate_slice", "object_set_height_ranges", "view_set",
                             "widget_list", "replay_run", "recording_start", "script_run", "tool_call", "object_set_variable_layer_height",
                             "plate_set_custom_gcode"})
        CHECK(names.count(name) == 1);
    // Built once, for the life of the process.
    CHECK(&NativeCommands::instance() == &catalog);
    CHECK(server_info().version == SLIC3R_VERSION);
    CHECK_FALSE(server_info().instructions.empty());
    // Clients cut the instructions off after 4096 characters.
    CHECK(server_info().instructions.size() <= 4096);
}
