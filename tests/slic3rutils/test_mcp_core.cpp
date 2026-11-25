#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "automation/automation_test_utils.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/AutomationRuntime.hpp"
#include "slic3r/GUI/Mcp/AutomationRuntimePtr.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpPersonContext.hpp"
#include "slic3r/GUI/Mcp/ModalTracker.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

// The dispatch core's cases that name a type of the GUI library: the automation runtime and an action's
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

TEST_CASE("an action's journal update stays inside action_run's envelope", "[Mcp]")
{
    Harness h;
    h.registry.add(tool("action_run", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        // The command's function, as action_run runs an action with it.
        const auto command = [](const json&, ToolContext& own) {
            own.set_journal_args({{"a", 1}});
            return ToolResult::ok();
        };
        ActionContext action(ctx, "orca_command:k:orca");
        return command(json::object(), action);
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

TEST_CASE("an action of fixed arguments is journaled with the arguments action_run takes for it", "[Mcp]")
{
    // What action_run takes for the action: a folded item's own arguments, none for a command's own action outside the
    // Design tab, or the value an action with an input collects.
    const json action_args = GENERATE(json{{"type", "circular"}}, json::object(), json{{"percent", 50}});
    Harness    h;
    h.registry.add(tool("action_run", GuardClass::Mutate, [action_args](const json&, ToolContext& ctx) {
        // The command's function records what it ran with, as object_arrange records the settings in force and a
        // Design command its arguments with their refs as locators.
        const auto command = [](const json&, ToolContext& own) {
            own.set_journal_args({{"type", "circular"}, {"spacing", 6}});
            return ToolResult::ok();
        };
        ActionContext action(ctx, "orca_command:k:orca", action_args);
        return command(json::object(), action);
    }));
    h.start();
    REQUIRE_FALSE(h.call("action_run", {{"id", "k"}}).is_error);
    const json steps = h.journal.to_replay("s")["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "action_run");
    // action_run refuses any other arguments for such an action, so the step holds the action's, which its replay runs.
    CHECK(steps[0]["args"] == json{{"id", "orca_command:k:orca"}, {"args", action_args}});
}

namespace {

// Whether `fn` throws a ToolFailure whose details' code is `code`.
template<class Fn> bool fails_with_code(Fn&& fn, const std::string& code)
{
    try {
        fn();
    } catch (const ToolFailure& f) {
        return f.details.is_object() && f.details.value("code", "") == code;
    }
    return false;
}

} // namespace

TEST_CASE("a withdrawn plugin's late calls leave nothing behind until it may call the tools again", "[Mcp]")
{
    // Built, not started: the scope bookkeeping needs no GUI, and the scope ends skip what was never started.
    AutomationRuntimePtr runtime(new AutomationRuntime());
    const std::string    job   = runtime->jobs().create("x", {}, "p");
    const std::string    child = runtime->jobs().create("script_run", job);
    REQUIRE(runtime->begin_plugin_scope("p"));
    runtime->begin_run_scope(child);
    CHECK(runtime->run_owner({job}) == "plugin:p");
    CHECK(runtime->run_owner({child, job}) == child);
    CHECK_NOTHROW(runtime->require_owner_scope("plugin:p"));
    CHECK_NOTHROW(runtime->require_owner_scope(child));

    runtime->plugin_tools_withdrawn("p");
    // A call that passed its check before the withdrawal leaves no rule, press or recording, nor does its script's.
    CHECK(fails_with_code([&] { runtime->run_owner({job}); }, "tools_not_allowed"));
    CHECK(fails_with_code([&] { runtime->run_owner({child, job}); }, "tools_not_allowed"));
    CHECK(fails_with_code([&] { runtime->set_auto_answer({}, {job}); }, "tools_not_allowed"));
    CHECK(fails_with_code([&] { runtime->require_owner_scope("plugin:p"); }, "tools_not_allowed"));
    // The script's own scope ended with the withdrawal, so what it would publish is refused too, and for the reason.
    CHECK(fails_with_code([&] { runtime->require_owner_scope(child); }, "tools_not_allowed"));
    // Another plugin and a client are untouched.
    const std::string other = runtime->jobs().create("x", {}, "q");
    REQUIRE(runtime->begin_plugin_scope("q"));
    CHECK(runtime->run_owner({other}) == "plugin:q");
    CHECK(runtime->run_owner({}).empty());
    CHECK_NOTHROW(runtime->require_owner_scope(""));

    // A call admitted under a new grant opens the scope again.
    REQUIRE(runtime->begin_plugin_scope("p"));
    CHECK(runtime->run_owner({job}) == "plugin:p");
    CHECK_NOTHROW(runtime->require_owner_scope("plugin:p"));
    // The script's scope stays ended: the withdrawal closed it, not only the plugin's mark.
    CHECK_THROWS_WITH(runtime->require_owner_scope(child), "The replay or script that made this call has ended");
}

TEST_CASE("an unloaded plugin's late press, recording or rule is refused, its scripts' too", "[Mcp]")
{
    AutomationRuntimePtr runtime(new AutomationRuntime());
    const std::string    job   = runtime->jobs().create("x", {}, "q");
    const std::string    child = runtime->jobs().create("script_run", job);
    REQUIRE(runtime->begin_plugin_scope("q"));
    runtime->begin_run_scope(child);
    CHECK_NOTHROW(runtime->require_owner_scope("plugin:q"));
    CHECK_NOTHROW(runtime->require_owner_scope(child));

    runtime->end_plugin_scope("q");
    const char* unloaded = "The plugin that made this call has been unloaded";
    CHECK_THROWS_WITH(runtime->require_owner_scope("plugin:q"), unloaded);
    // Its script's scope ended with the unload: nothing the script would leave behind is published.
    CHECK_THROWS_WITH(runtime->require_owner_scope(child), unloaded);
    CHECK_THROWS_AS(runtime->run_owner({child, job}), ToolFailure);
    CHECK_THROWS_AS(runtime->set_auto_answer({}, {child, job}), ToolFailure);
    CHECK_FALSE(runtime->begin_plugin_scope("q"));

    // Loaded again, but before any call of the plugin is admitted: a call from before the unload still finds no scope
    // of the plugin's, and is refused rather than taken for the client's.
    runtime->plugin_loaded("q");
    CHECK_THROWS_WITH(runtime->run_owner({job}), unloaded);
    CHECK_THROWS_AS(runtime->set_auto_answer({}, {job}), ToolFailure);
    // Once a call is admitted the plugin's scope opens again, and the script's stays ended: the unload closed it, not
    // only the plugin's mark.
    REQUIRE(runtime->begin_plugin_scope("q"));
    CHECK(runtime->run_owner({job}) == "plugin:q");
    CHECK_THROWS_WITH(runtime->require_owner_scope(child), "The replay or script that made this call has ended");
}

TEST_CASE("a run of no plugin is refused where it publishes once it has ended", "[Mcp]")
{
    AutomationRuntimePtr runtime(new AutomationRuntime());
    const std::string    run = runtime->jobs().create("replay_run", {});
    runtime->begin_run_scope(run);
    CHECK_NOTHROW(runtime->require_owner_scope(run));
    runtime->end_run_scope(run, false);
    CHECK_THROWS_WITH(runtime->require_owner_scope(run), "The replay or script that made this call has ended");
}
