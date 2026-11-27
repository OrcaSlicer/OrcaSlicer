#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/McpReplay.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <stdlib.h> // setenv
#endif

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {

Replay parse_ok(const std::string& text)
{
    std::string err;
    auto        s = parse_replay(text, err);
    INFO(err);
    REQUIRE(s.has_value());
    return *s;
}

// A replay host whose tools are lambdas; records calls and rule cleanup.
struct FakeHost
{
    std::map<std::string, std::function<ToolResult(const json&)>> tools;
    std::map<std::string, ToolResult>                             job_results;
    std::vector<std::pair<std::string, json>>                     calls;
    bool                                                          ended  = false;
    bool                                                          cancel = false;

    ReplayHost host()
    {
        ReplayHost h;
        h.call = [this](const std::string& tool, const json& args) {
            calls.emplace_back(tool, args);
            auto it = tools.find(tool);
            return it == tools.end() ? ToolResult::error("unknown tool " + tool) : it->second(args);
        };
        h.wait_job = [this](const std::string& job, int) -> std::optional<ToolResult> {
            auto it = job_results.find(job);
            if (it == job_results.end())
                return std::nullopt;
            return it->second;
        };
        h.end_replay = [this] { ended = true; };
        h.cancelled  = [this] { return cancel; };
        return h;
    }
};

} // namespace

TEST_CASE("replays allow comments and read their fields", "[Mcp]")
{
    const Replay s = parse_ok(R"({
        // a comment
        "orca_replay_version": "0.2.0", "name": "repro", /* block */ "vars": {"n": 3},
        "steps": [{"tool": "plate_slice", "expect": {"/ok": {"eq": true}}, "optional": true, "timeout_s": 5},
                  {"tool": "action_run", "await": false, "save_as": "prefs"}]
    })");
    CHECK(s.name == "repro");
    REQUIRE(s.steps.size() == 2);
    CHECK(s.steps[0].optional);
    CHECK(s.steps[0].timeout_s == 5);
    CHECK_FALSE(s.steps[1].await);
    CHECK(s.steps[1].save_as == "prefs");
}

TEST_CASE("invalid replays are rejected with a reason", "[Mcp]")
{
    const auto [text, reason] = GENERATE(table<std::string, std::string>({
        {"{", "not valid JSON"},
        {R"({"steps": []})", "orca_replay_version"},
        {R"({"orca_replay_version": "0.2.0"})", "steps"},
        {R"({"orca_replay_version": "0.2.0", "steps": [{"args": {}}]})", "step 1"},
        {R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "x", "await": false}]})", "save_as"},
        {R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "x", "args": []}]})", "args"},
    }));
    std::string err;
    CHECK_FALSE(parse_replay(text, err).has_value());
    CHECK(err.find(reason) != std::string::npos);
}

TEST_CASE("a replay is read only in a format version this build reads", "[Mcp]")
{
    // This build reads format 0.2.0 (REPLAY_VERSION): while the major version is 0, only its own minor version.
    const auto [version, reason] = GENERATE(table<std::string, std::string>({
        {"0.2.0", ""},
        {"0.2.7", ""},
        {"0.2.0-beta", ""},
        {"0.3.0", "written for a newer OrcaSlicer"},
        {"1.0.0", "written for a newer OrcaSlicer"},
        {"0.1.0", "no longer reads"},
        {"0.0.9", "no longer reads"},
        {"v0.2.0", "must be a version"},
        {"0.x.0", "must be a version"},
    }));
    CAPTURE(version);
    std::string err;
    const auto  replay = parse_replay(R"({"orca_replay_version": ")" + version + R"(", "steps": []})", err);
    CHECK(replay.has_value() == reason.empty());
    CHECK(err.find(reason) != std::string::npos);
}

TEST_CASE("a replay of an older format is refused for its format", "[Mcp]")
{
    // A replay written before the commands were named by place calls tools this build does not have: it is refused for
    // its format before any step is read, not for its first step's tool.
    std::string err;
    CHECK_FALSE(parse_replay(R"({"orca_replay_version": "0.1.0", "name": "old", "steps": [{"tool": "list_objects", "args": {}}]})", err)
                    .has_value());
    CHECK_THAT(err, ContainsSubstring("format 0.1.0"));
    CHECK_THAT(err, ContainsSubstring("this one reads format 0.2.0"));
    CHECK_THAT(err, !ContainsSubstring("Unknown tool"));
    // The same replay in this format, on the command's name, is read.
    const Replay now = parse_ok(R"({"orca_replay_version": "0.2.0", "name": "old", "steps": [{"tool": "object_list", "args": {}}]})");
    CHECK(now.name == "old");
    REQUIRE(now.steps.size() == 1);
    CHECK(now.steps[0].tool == "object_list");
}

TEST_CASE("vars, env and jobs substitute recursively", "[Mcp]")
{
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "vars": {"model": "${replay_dir}/part.stl", "count": 3}, "steps": []})");
    ReplayContext ctx;
    ctx.replay_dir = "/replays";
    ctx.out_dir    = "/out";
    ctx.jobs       = {{"prefs", "j9"}};
#ifdef _WIN32
    _putenv_s("MCP_REPLAY_TEST", "hello");
#else
    setenv("MCP_REPLAY_TEST", "hello", 1);
#endif
    std::string err;
    const json  out = substitute({{"paths", {"${vars.model}"}},
                                  {"n", "${vars.count}"},
                                  {"shot", "${out_dir}/a.png"},
                                  {"job", "${jobs.prefs}"},
                                  {"greeting", "say ${env:MCP_REPLAY_TEST}"}},
                                 s, ctx, err);
    CHECK(err.empty());
    CHECK(out["paths"][0] == "/replays/part.stl");
    CHECK(out["n"] == 3);
    CHECK(out["shot"] == "/out/a.png");
    CHECK(out["job"] == "j9");
    CHECK(out["greeting"] == "say hello");

    substitute("${vars.missing}", s, ctx, err);
    CHECK(err.find("unknown placeholder") != std::string::npos);
}

TEST_CASE("each matcher passes and fails with a message naming the pointer", "[Mcp]")
{
    const json result = {{"n", 5}, {"s", "Slicing done"}, {"a", {1, 2}}, {"e", json::array()}, {"o", {{"k", 1}}}};
    struct Case
    {
        json expect;
        bool pass;
    };
    const Case c = GENERATE_COPY(values<Case>({
        {{{"/n", {{"eq", 5}}}}, true},           {{{"/n", {{"eq", 6}}}}, false},
        {{{"/n", {{"ne", 6}}}}, true},           {{{"/n", {{"gt", 4}}}}, true},
        {{{"/n", {{"gt", 5}}}}, false},          {{{"/n", {{"ge", 5}}}}, true},
        {{{"/n", {{"lt", 6}}}}, true},           {{{"/n", {{"le", 4}}}}, false},
        {{{"/s", {{"contains", "done"}}}}, true}, {{{"/a", {{"contains", 2}}}}, true},
        {{{"/o", {{"contains", "k"}}}}, true},   {{{"/a", {{"contains", 3}}}}, false},
        {{{"/s", {{"regex", "^Slic"}}}}, true},  {{{"/s", {{"regex", "^x"}}}}, false},
        {{{"/n", {{"exists", true}}}}, true},    {{{"/zz", {{"exists", false}}}}, true},
        {{{"/zz", {{"eq", 1}}}}, false},         {{{"/e", {{"empty", true}}}}, true},
        {{{"/a", {{"empty", true}}}}, false},
    }));
    const auto failure = check_expect(result, c.expect);
    CHECK(failure.has_value() != c.pass);
    if (failure)
        CHECK(failure->find(c.expect.begin().key()) == 0);
}

TEST_CASE("a job-returning step is awaited before its expect is checked", "[Mcp]")
{
    FakeHost f;
    f.tools["plate_slice"]    = [](const json&) { return ToolResult::ok({{"status", "running"}, {"job", "j3"}}); };
    f.job_results["j3"] = ToolResult::ok({{"plates", {{{"outcome", "ok"}}}}});
    const Replay  s     = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "plate_slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}}]})");
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, f.host());
    CHECK(out["passed"] == true);
    CHECK(f.ended);
}

TEST_CASE("await false stores the job under save_as for later steps", "[Mcp]")
{
    FakeHost f;
    f.tools["action_run"]     = [](const json&) { return ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j5"}}); };
    f.tools["dialog_answer"] = [](const json&) { return ToolResult::ok(); };
    f.tools["app_wait"]           = [](const json& a) { return ToolResult::ok({{"job", a["job"]}}); };
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "action_run", "await": false, "save_as": "prefs"},
        {"tool": "dialog_answer", "args": {"button": "close"}},
        {"tool": "app_wait", "args": {"for": "job", "job": "${jobs.prefs}"}, "expect": {"/job": {"eq": "j5"}}}]})");
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, f.host());
    CHECK(out["passed"] == true);
    REQUIRE(f.calls.size() == 3);
    CHECK(f.calls[2].second["job"] == "j5");
}

TEST_CASE("a bounded substitution charges what it puts in, and nothing for an empty string or null", "[Mcp]")
{
    Replay r;
    r.vars             = {{"a", "xyz"}, {"e", ""}, {"n", nullptr}};
    std::size_t budget = 3;
    // Free: the empty variable, the replay's folder (empty here) and null, which is still put in.
    CHECK(substitute_lenient(json("${vars.e}${out_dir}${vars.n}"), r, ReplayContext{}, budget) == "null");
    CHECK(substitute_lenient(json("${vars.n}"), r, ReplayContext{}, budget).is_null());
    CHECK(budget == 3);
    // A value costs its length; past the budget, a placeholder stays as written.
    CHECK(substitute_lenient(json("${vars.a}${vars.a}"), r, ReplayContext{}, budget) == "xyz${vars.a}");
    CHECK(budget == 0);
}

TEST_CASE("an inline replay's step whose tool isn't a string is left to the nested run to refuse", "[Mcp]")
{
    FakeHost f;
    f.tools["replay_run"] = [](const json&) { return ToolResult::error("step 1: \"tool\" must be a string"); };
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "replay_run", "args": {"replay": {"steps": [{"tool": 17, "args": {}}]}}}]})");
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, f.host());
    CHECK(out["passed"] == false);
    CHECK(out["failed_step"] == 1);
    REQUIRE(f.calls.size() == 1);
    CHECK(f.calls[0].second["replay"]["steps"][0]["tool"] == 17);
}

TEST_CASE("a run stops at the first failed step unless it is optional", "[Mcp]")
{
    FakeHost f;
    f.tools["ok"]   = [](const json&) { return ToolResult::ok({{"v", 1}}); };
    f.tools["fail"] = [](const json&) { return ToolResult::error("nope"); };
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "fail", "optional": true}, {"tool": "ok", "expect": {"/v": {"eq": 2}}}, {"tool": "ok"}]})");
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, f.host());
    CHECK(out["passed"] == false);
    CHECK(out["failed_step"] == 2);
    CHECK(out["steps"].size() == 2);
    CHECK(out["steps"][1]["expect_failure"].get<std::string>().find("/v") == 0);
    CHECK(f.ended); // replay-scoped rules are dropped even when it fails
}

TEST_CASE("a run reports each step as it starts and ends", "[Mcp]")
{
    FakeHost f;
    f.tools["a"] = [](const json&) { return ToolResult::ok(); };
    f.tools["b"] = [](const json&) { return ToolResult::ok(); };
    const Replay             s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "a"}, {"tool": "b"}]})");
    std::vector<std::string> events;
    ReplayHost               h = f.host();
    h.step_started             = [&](std::size_t index) { events.push_back("started " + std::to_string(index)); };
    h.step_finished            = [&](const json& entry) {
        events.push_back("finished " + std::to_string(entry["index"].get<int>()));
        CHECK(entry["ok"] == true);
        CHECK(entry["duration_ms"].get<long long>() >= 0);
    };
    ReplayContext ctx;
    run_replay(s, ctx, h);
    CHECK(events == std::vector<std::string>{"started 1", "finished 1", "started 2", "finished 2"});
}

TEST_CASE("only_step runs one step and reports its own index", "[Mcp]")
{
    FakeHost f;
    for (const char* name : {"a", "b", "c"})
        f.tools[name] = [](const json&) { return ToolResult::ok(); };
    const Replay  s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "a"}, {"tool": "b"}, {"tool": "c"}]})");
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, f.host(), 2);
    REQUIRE(f.calls.size() == 1);
    CHECK(f.calls[0].first == "b");
    REQUIRE(out["steps"].size() == 1);
    CHECK(out["steps"][0]["index"] == 2);
    CHECK(out["passed"] == true);
    CHECK_THROWS_AS(run_replay(s, ctx, f.host(), 4), std::out_of_range);
}

TEST_CASE("a wait on an await false step that finished in its budget gives that step's result", "[Mcp]")
{
    FakeHost f;
    f.tools["action_run"] = [](const json&) { return ToolResult::ok({{"opened", "Preferences"}}); };
    f.tools["app_wait"]   = [](const json&) { return ToolResult::error("app_wait is not called"); };
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "action_run", "await": false, "save_as": "prefs"},
        {"tool": "app_wait", "args": {"for": "job", "job": "${jobs.prefs}"}, "expect": {"/opened": {"eq": "Preferences"}}},
        {"tool": "app_wait", "args": {"job": "${jobs.prefs}"}}]})");
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, f.host());
    CHECK(out["passed"] == true);
    REQUIRE(f.calls.size() == 1);
    CHECK(f.calls[0].first == "action_run");
}

TEST_CASE("jobs saved by an earlier run resolve in a later single step", "[Mcp]")
{
    FakeHost f;
    f.tools["x"]    = [](const json&) { return ToolResult::ok({{"status", "running"}, {"job", "j7"}}); };
    f.tools["app_wait"] = [](const json& a) { return ToolResult::ok({{"job", a["job"]}}); };
    const Replay s  = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "x", "await": false, "save_as": "prefs"},
        {"tool": "app_wait", "args": {"job": "${jobs.prefs}"}},
        {"tool": "app_wait", "args": {"job": "${jobs.none}"}}]})");

    ReplayContext fresh;
    const json    alone = run_replay(s, fresh, f.host(), 2);
    CHECK(alone["steps"][0]["ok"] == false);
    CHECK(alone["steps"][0]["error"].get<std::string>().find("no job is saved as prefs yet; step 1 saves it") != std::string::npos);
    CHECK(alone["steps"][0]["reason"] == json{{"kind", "missing_job"}, {"name", "prefs"}, {"saved_by", 1}});

    ReplayContext shared;
    run_replay(s, shared, f.host(), 1);
    run_replay(s, shared, f.host(), 2);
    CHECK(f.calls.back().second["job"] == "j7");

    const json unsaved = run_replay(s, shared, f.host(), 3);
    CHECK(unsaved["steps"][0]["reason"]["kind"] == "missing_job");
    CHECK(unsaved["steps"][0]["reason"]["saved_by"].is_null());
}

TEST_CASE("a stop during a step marks that step, even the last one", "[Mcp]")
{
    const std::string text = GENERATE(as<std::string>{}, R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "stopper"}]})",
                                      R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "stopper", "optional": true}]})");
    const bool        single = GENERATE(false, true);
    FakeHost          f;
    f.tools["stopper"] = [&f](const json&) {
        f.cancel = true;
        return ToolResult::ok({{"plates", json::array()}});
    };
    json       reported;
    ReplayHost h       = f.host();
    h.step_finished    = [&](const json& entry) { reported = entry; };
    const Replay  s    = parse_ok(text);
    ReplayContext ctx;
    const json    out = single ? run_replay(s, ctx, h, 1) : run_replay(s, ctx, h);
    for (const json& entry : {reported, out["steps"][0]}) {
        CHECK(entry["cancelled"] == true);
        CHECK(entry["ok"] == false);
        CHECK(entry["reason"]["kind"] == "cancelled");
    }
    CHECK(out["passed"] == false);
    CHECK(out["failed_step"] == 1);
}

TEST_CASE("a stop during a job wait is a cancel, not a timeout", "[Mcp]")
{
    FakeHost f;
    f.tools["slow"] = [](const json&) { return ToolResult::ok({{"status", "running"}, {"job", "j1"}}); };
    const Replay s  = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "slow"}]})");

    ReplayHost stopping = f.host();
    stopping.wait_job   = [&f](const std::string&, int) -> std::optional<ToolResult> {
        f.cancel = true;
        return std::nullopt;
    };
    ReplayContext ctx;
    const json    stopped = run_replay(s, ctx, stopping);
    CHECK(stopped["steps"][0]["cancelled"] == true);
    CHECK(stopped["steps"][0]["reason"]["kind"] == "cancelled");

    FakeHost   g;
    g.tools        = f.tools;
    const json out = run_replay(s, ctx, g.host());
    CHECK(out["steps"][0]["reason"] == json{{"kind", "timeout"}, {"seconds", 120}});
}

TEST_CASE("a job wait that times out names the dialog left open", "[Mcp]")
{
    FakeHost f;
    f.tools["opens"] = [](const json&) { return ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j1"}}); };
    const Replay s   = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "opens"}]})");

    const json dialogs = json::array({{{"title", "Uniform scale"}, {"native", false}}, {{"title", "Preferences"}, {"native", false}}});
    ReplayHost host    = f.host();
    host.open_dialogs  = [&dialogs] { return dialogs; };
    ReplayContext ctx;
    const json out          = run_replay(s, ctx, host);
    const json& step        = out["steps"][0];
    const std::string error = step["error"].get<std::string>();
    CHECK(step["reason"]["kind"] == "timeout");
    // The topmost dialog, the last one listed, is the one named.
    CHECK(error.find("\"Preferences\"") != std::string::npos);
    CHECK(error.find("dialog_auto_answer") != std::string::npos);
    CHECK(step["details"]["dialogs"] == dialogs);
}

TEST_CASE("a failed expectation carries its parts", "[Mcp]")
{
    FakeHost f;
    f.tools["plate_slice"] = [](const json&) { return ToolResult::ok({{"plates", {{{"outcome", "error"}}}}}); };
    const Replay  s  = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "plate_slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}}]})");
    ReplayContext ctx;
    const json    entry = run_replay(s, ctx, f.host())["steps"][0];
    CHECK(entry["reason"] == json{{"kind", "expect"}, {"pointer", "/plates/0/outcome"}, {"op", "eq"},
                                  {"expected", "ok"},  {"actual", "error"},              {"exists", true}});
    CHECK(entry["expect_failure"].get<std::string>().find("/plates/0/outcome: expected eq \"ok\"") == 0);
}

TEST_CASE("a matcher that is not an object is a malformed expectation", "[Mcp]")
{
    FakeHost f;
    f.tools["plate_slice"] = [](const json&) { return ToolResult::ok({{"outcome", "ok"}}); };
    const Replay  s  = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "plate_slice", "expect": {"/outcome": "ok"}}]})");
    ReplayContext ctx;
    const json    entry = run_replay(s, ctx, f.host())["steps"][0];
    CHECK(entry["ok"] == false);
    CHECK(entry["reason"] == json{{"kind", "expect_invalid"}});
    CHECK(entry["expect_failure"].get<std::string>().find("must be an object") != std::string::npos);
}

// Action Replay's step descriptions mirror this: an inline nested replay reads its placeholders
// from the outer replay, whose substitution runs before the nested run starts.
TEST_CASE("an inline nested replay's placeholders are the outer replay's", "[Mcp]")
{
    FakeHost f;
    f.tools["replay_run"] = [](const json&) { return ToolResult::ok({{"passed", true}}); };
    const Replay with_outer = parse_ok(R"({"orca_replay_version": "0.2.0", "vars": {"p": "outer.stl"}, "steps": [{"tool": "replay_run",
        "args": {"vars": {"p": "step.stl"}, "replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "project_import", "args": {"paths": ["${vars.p}"]}}]}}}]})");
    ReplayContext ctx;
    CHECK(run_replay(with_outer, ctx, f.host())["passed"] == true);
    REQUIRE(f.calls.size() == 1);
    CHECK(f.calls[0].second["replay"]["steps"][0]["args"]["paths"][0] == "outer.stl");

    FakeHost     g;
    const Replay step_only = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "replay_run",
        "args": {"vars": {"p": "step.stl"}, "replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "project_import", "args": {"paths": ["${vars.p}"]}}]}}}]})");
    const json   out = run_replay(step_only, ctx, g.host());
    CHECK(out["passed"] == false);
    CHECK(out["steps"][0]["reason"] == json{{"kind", "placeholder"}, {"text", "${vars.p}"}});
    CHECK(g.calls.empty());
}

TEST_CASE("lenient substitution keeps what it cannot resolve", "[Mcp]")
{
    const Replay  s = parse_ok(R"({"orca_replay_version": "0.2.0", "vars": {"model": "C:/m/part.stl"}, "steps": []})");
    ReplayContext ctx;
    CHECK(substitute_lenient("${vars.model}", s, ctx) == "C:/m/part.stl");
    CHECK(substitute_lenient("${jobs.none}/x", s, ctx) == "${jobs.none}/x");
    CHECK(substitute_lenient(json{{"p", "${vars.nope}"}}, s, ctx) == json{{"p", "${vars.nope}"}});
}

TEST_CASE("malformed replays are errors, not exceptions", "[Mcp]")
{
    const auto [text, field] = GENERATE(table<std::string, std::string>({
        {R"({"orca_replay_version": 1, "steps": []})", "orca_replay_version"},
        {R"({"orca_replay_version": "0.2.0", "name": 5, "steps": []})", "name"},
        {R"({"orca_replay_version": "0.2.0", "steps": [{"tool": 3}]})", "step 1: \"tool\""},
        {R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "a", "timeout_s": "9"}]})", "step 1: \"timeout_s\""},
        {R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "a", "optional": "yes"}]})", "step 1: \"optional\""},
        {R"([])", "orca_replay_version"},
        {R"({})", "orca_replay_version"},
    }));
    std::string          err;
    std::optional<Replay> s;
    CHECK_NOTHROW(s = parse_replay(text, err));
    CHECK_FALSE(s.has_value());
    CHECK(err.find(field) != std::string::npos);
}

TEST_CASE("broken JSON says where", "[Mcp]")
{
    std::string err;
    CHECK_FALSE(parse_replay("{\n  \"orca_replay_version\": \"0.2.0\",\n  \"steps\": [ {\"tool\": \"a\",, } ]\n}", err).has_value());
    CHECK(err.find("line 3, column") != std::string::npos);
}

TEST_CASE("a Python step's code runs as written while its other arguments substitute", "[Mcp]")
{
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "vars": {"a": "seven"}, "steps": [
        {"tool": "script_run", "args": {"code": "label = '${vars.a}'", "args": {"label": "${vars.a}"}}}]})");
    FakeHost fake;
    fake.tools["script_run"] = [](const json&) { return ToolResult::ok({{"status", "finished"}, {"result", nullptr}, {"output", ""}}); };
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, fake.host(), std::nullopt);
    CHECK(out["passed"] == true);
    REQUIRE(fake.calls.size() == 1);
    CHECK(fake.calls[0].second["code"] == "label = '${vars.a}'");
    CHECK(fake.calls[0].second["args"]["label"] == "seven");
}

TEST_CASE("a Python step that did not finish fails its replay", "[Mcp]")
{
    const Replay s = parse_ok(R"S({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "script_run", "args": {"code": "raise ValueError('x')"}},
        {"tool": "app_state"}]})S");
    FakeHost fake;
    fake.tools["script_run"] = [](const json&) {
        return ToolResult::ok({{"status", "raised"}, {"message", "ValueError: x"}, {"traceback", "..."}, {"output", ""}});
    };
    fake.tools["app_state"] = [](const json&) { return ToolResult::ok(); };
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, fake.host(), std::nullopt);
    CHECK(out["passed"] == false);
    CHECK(out["failed_step"] == 1);
    CHECK(out["steps"][0]["error"] == "ValueError: x");
    CHECK(out["steps"][0]["reason"]["kind"] == "python");
    CHECK(out["steps"][0]["reason"]["status"] == "raised");
    CHECK(fake.calls.size() == 1);
}

TEST_CASE("a Python step left running with await false keeps its job for later steps", "[Mcp]")
{
    const Replay s = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "script_run", "args": {"code": "x = 1"}, "await": false, "save_as": "py"},
        {"tool": "app_state"}]})");
    FakeHost fake;
    fake.tools["script_run"] = [](const json&) { return ToolResult::ok({{"status", "running"}, {"job", "j5"}}); };
    fake.tools["app_state"]  = [](const json&) { return ToolResult::ok(); };
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, fake.host(), std::nullopt);
    CHECK(out["passed"] == true);
    CHECK(ctx.jobs["py"] == "j5");
}

TEST_CASE("Python reached through tool_call or a nested replay follows the Python step rules", "[Mcp]")
{
    // Its code runs as written however the step reaches it.
    const Replay s = parse_ok(R"S({"orca_replay_version": "0.2.0", "vars": {"a": "seven"}, "steps": [
        {"tool": "tool_call", "args": {"tool": "script_run", "args": {"code": "x = '${vars.a}'", "args": {"v": "${vars.a}"}}}},
        {"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [
            {"tool": "script_run", "args": {"code": "y = '${vars.a}'"}},
            {"tool": "settings_set", "args": {"note": "${vars.a}"}}]}}}]})S");
    FakeHost fake;
    fake.tools["tool_call"]   = [](const json&) { return ToolResult::ok({{"status", "finished"}, {"result", nullptr}, {"output", ""}}); };
    fake.tools["replay_run"] = [](const json&) { return ToolResult::ok({{"passed", true}}); };
    ReplayContext ctx;
    const json    out = run_replay(s, ctx, fake.host(), std::nullopt);
    CHECK(out["passed"] == true);
    REQUIRE(fake.calls.size() == 2);
    CHECK(fake.calls[0].second["args"]["code"] == "x = '${vars.a}'");
    CHECK(fake.calls[0].second["args"]["args"]["v"] == "seven");
    const json nested = fake.calls[1].second["replay"]["steps"];
    CHECK(nested[0]["args"]["code"] == "y = '${vars.a}'");
    CHECK(nested[1]["args"]["note"] == "seven");

    // A run that raised through tool_call fails its step.
    const Replay failing = parse_ok(R"({"orca_replay_version": "0.2.0", "steps": [
        {"tool": "tool_call", "args": {"tool": "script_run", "args": {"code": "x"}}}]})");
    FakeHost raising;
    raising.tools["tool_call"] = [](const json&) { return ToolResult::ok({{"status", "raised"}, {"message", "NameError: x"}}); };
    ReplayContext ctx2;
    const json    failed = run_replay(failing, ctx2, raising.host(), std::nullopt);
    CHECK(failed["passed"] == false);
    CHECK(failed["steps"][0]["reason"]["kind"] == "python");
}
