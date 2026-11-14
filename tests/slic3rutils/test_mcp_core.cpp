#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "automation/automation_test_utils.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/AutomationRuntime.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpPersonContext.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

// The dispatch core's cases that name a type of the GUI library: the automation runtime and a tile's
// context. The others are in tests/automation/test_mcp_core.cpp.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Slic3r::GUI::CommandFn;
using Slic3r::GUI::NativeCommand;
using Slic3r::GUI::Mcp::Test::Harness;

namespace {

NativeCommand tool(const std::string& name, GuardClass guard, CommandFn handler)
{
    NativeCommand d;
    d.key     = name;
    d.guard   = guard;
    d.run     = std::move(handler);
    return d;
}

} // namespace

TEST_CASE("a context with no runtime refuses", "[Mcp]")
{
    Harness         h;
    const CommandFn needs_runtime = [](const json&, ToolContext& ctx) {
        [[maybe_unused]] AutomationRuntime& runtime = ctx.runtime();
        return ToolResult::ok();
    };
    // Inline, so the handler runs on this thread, where it may assert.
    NativeCommand look = tool("look", GuardClass::Inspect, [needs_runtime](const json& args, ToolContext& ctx) {
        REQUIRE_THROWS_WITH(ctx.runtime(), "This call runs without the automation runtime");
        return needs_runtime(args, ctx);
    });
    look.inline_call = true;
    h.registry.add(look);
    h.registry.add(tool("act", GuardClass::Mutate, needs_runtime));
    h.start();

    // A core no AutomationRuntime owns, as a test builds one: the call fails, inline or as a job, and nothing crashes.
    CHECK(h.call("look").is_error);
    const ToolResult acted = h.call("act");
    CHECK(acted.is_error);
    CHECK_FALSE(acted.data.contains("status"));
}

TEST_CASE("a tile's journal update stays inside run_action's envelope", "[Mcp]")
{
    Harness h;
    h.registry.add(tool("run_action", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        // The command's function, as run_action runs a tile with it.
        const auto command = [](const json&, ToolContext& own) {
            own.set_journal_args({{"a", 1}});
            return ToolResult::ok();
        };
        TileContext tile(ctx, "orca_command:k:orca");
        return command(json::object(), tile);
    }));
    h.start();
    const ToolResult ran = h.call("run_action", {{"id", "k"}});
    REQUIRE_FALSE(ran.is_error);
    REQUIRE_FALSE(ran.data.contains("status"));
    const json steps = h.journal.to_replay("s")["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "run_action");
    CHECK(steps[0]["args"] == json{{"id", "orca_command:k:orca"}, {"args", {{"a", 1}}}});
}
