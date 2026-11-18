#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

TEST_CASE("a tile's journal update stays inside action_run's envelope", "[Mcp]")
{
    Harness h;
    h.registry.add(tool("action_run", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        // The command's function, as action_run runs a tile with it.
        const auto command = [](const json&, ToolContext& own) {
            own.set_journal_args({{"a", 1}});
            return ToolResult::ok();
        };
        TileContext tile(ctx, "orca_command:k:orca");
        return command(json::object(), tile);
    }));
    h.start();
    const ToolResult ran = h.call("action_run", {{"id", "k"}});
    REQUIRE_FALSE(ran.is_error);
    REQUIRE_FALSE(ran.data.contains("status"));
    const json steps = h.journal.to_replay("s")["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "action_run");
    CHECK(steps[0]["args"] == json{{"id", "orca_command:k:orca"}, {"args", {{"a", 1}}}});
}

TEST_CASE("a tile of fixed arguments is journaled with the arguments action_run takes for it", "[Mcp]")
{
    // What action_run takes for the tile: a folded row's own arguments, none for a command's own tile outside the
    // Design tab, or the value a tile with an input collects.
    const json tile_args = GENERATE(json{{"type", "circular"}}, json::object(), json{{"percent", 50}});
    Harness    h;
    h.registry.add(tool("action_run", GuardClass::Mutate, [tile_args](const json&, ToolContext& ctx) {
        // The command's function records what it ran with, as object_arrange records the settings in force and a
        // Design command its arguments with their refs as locators.
        const auto command = [](const json&, ToolContext& own) {
            own.set_journal_args({{"type", "circular"}, {"spacing", 6}});
            return ToolResult::ok();
        };
        TileContext tile(ctx, "orca_command:k:orca", tile_args);
        return command(json::object(), tile);
    }));
    h.start();
    REQUIRE_FALSE(h.call("action_run", {{"id", "k"}}).is_error);
    const json steps = h.journal.to_replay("s")["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "action_run");
    // action_run refuses any other arguments for such a tile, so the step holds the tile's, which its replay runs.
    CHECK(steps[0]["args"] == json{{"id", "orca_command:k:orca"}, {"args", tile_args}});
}
