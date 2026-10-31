#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Mcp/McpScript.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

#include <nlohmann/json.hpp>

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

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

Script parse_ok(const std::string& text)
{
    std::string err;
    auto        s = parse_script(text, err);
    INFO(err);
    REQUIRE(s.has_value());
    return *s;
}

// A script host whose tools are lambdas; records calls and rule cleanup.
struct FakeHost
{
    std::map<std::string, std::function<ToolResult(const json&)>> tools;
    std::map<std::string, ToolResult>                             job_results;
    std::vector<std::pair<std::string, json>>                     calls;
    bool                                                          ended  = false;
    bool                                                          cancel = false;

    ScriptHost host()
    {
        ScriptHost h;
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
        h.end_script = [this] { ended = true; };
        h.cancelled  = [this] { return cancel; };
        return h;
    }
};

} // namespace

TEST_CASE("scripts allow comments and read their fields", "[Mcp]")
{
    const Script s = parse_ok(R"({
        // a comment
        "orca_script": 1, "name": "repro", /* block */ "vars": {"n": 3},
        "steps": [{"tool": "slice", "expect": {"/ok": {"eq": true}}, "optional": true, "timeout_s": 5},
                  {"tool": "run_action", "await": false, "save_as": "prefs"}]
    })");
    CHECK(s.name == "repro");
    REQUIRE(s.steps.size() == 2);
    CHECK(s.steps[0].optional);
    CHECK(s.steps[0].timeout_s == 5);
    CHECK_FALSE(s.steps[1].await);
    CHECK(s.steps[1].save_as == "prefs");
}

TEST_CASE("invalid scripts are rejected with a reason", "[Mcp]")
{
    const auto [text, reason] = GENERATE(table<std::string, std::string>({
        {"{", "not valid JSON"},
        {R"({"orca_script": 2, "steps": []})", "orca_script"},
        {R"({"orca_script": 1})", "steps"},
        {R"({"orca_script": 1, "steps": [{"args": {}}]})", "step 1"},
        {R"({"orca_script": 1, "steps": [{"tool": "x", "await": false}]})", "save_as"},
        {R"({"orca_script": 1, "steps": [{"tool": "x", "args": []}]})", "args"},
    }));
    std::string err;
    CHECK_FALSE(parse_script(text, err).has_value());
    CHECK(err.find(reason) != std::string::npos);
}

TEST_CASE("vars, env and jobs substitute recursively", "[Mcp]")
{
    const Script s = parse_ok(R"({"orca_script": 1, "vars": {"model": "${script_dir}/part.stl", "count": 3}, "steps": []})");
    ScriptContext ctx;
    ctx.script_dir = "/scripts";
    ctx.out_dir    = "/out";
    ctx.jobs       = {{"prefs", "j9"}};
#ifdef _WIN32
    _putenv_s("MCP_SCRIPT_TEST", "hello");
#else
    setenv("MCP_SCRIPT_TEST", "hello", 1);
#endif
    std::string err;
    const json  out = substitute({{"paths", {"${vars.model}"}},
                                  {"n", "${vars.count}"},
                                  {"shot", "${out_dir}/a.png"},
                                  {"job", "${jobs.prefs}"},
                                  {"greeting", "say ${env:MCP_SCRIPT_TEST}"}},
                                 s, ctx, err);
    CHECK(err.empty());
    CHECK(out["paths"][0] == "/scripts/part.stl");
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
    f.tools["slice"]    = [](const json&) { return ToolResult::ok({{"status", "running"}, {"job", "j3"}}); };
    f.job_results["j3"] = ToolResult::ok({{"plates", {{{"outcome", "ok"}}}}});
    const Script  s     = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}}]})");
    ScriptContext ctx;
    const json    out = run_script(s, ctx, f.host());
    CHECK(out["passed"] == true);
    CHECK(f.ended);
}

TEST_CASE("await false stores the job under save_as for later steps", "[Mcp]")
{
    FakeHost f;
    f.tools["run_action"]     = [](const json&) { return ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j5"}}); };
    f.tools["dialog_respond"] = [](const json&) { return ToolResult::ok(); };
    f.tools["wait"]           = [](const json& a) { return ToolResult::ok({{"job", a["job"]}}); };
    const Script s = parse_ok(R"({"orca_script": 1, "steps": [
        {"tool": "run_action", "await": false, "save_as": "prefs"},
        {"tool": "dialog_respond", "args": {"button": "close"}},
        {"tool": "wait", "args": {"for": "job", "job": "${jobs.prefs}"}, "expect": {"/job": {"eq": "j5"}}}]})");
    ScriptContext ctx;
    const json    out = run_script(s, ctx, f.host());
    CHECK(out["passed"] == true);
    REQUIRE(f.calls.size() == 3);
    CHECK(f.calls[2].second["job"] == "j5");
}

TEST_CASE("a run stops at the first failed step unless it is optional", "[Mcp]")
{
    FakeHost f;
    f.tools["ok"]   = [](const json&) { return ToolResult::ok({{"v", 1}}); };
    f.tools["fail"] = [](const json&) { return ToolResult::error("nope"); };
    const Script s = parse_ok(R"({"orca_script": 1, "steps": [
        {"tool": "fail", "optional": true}, {"tool": "ok", "expect": {"/v": {"eq": 2}}}, {"tool": "ok"}]})");
    ScriptContext ctx;
    const json    out = run_script(s, ctx, f.host());
    CHECK(out["passed"] == false);
    CHECK(out["failed_step"] == 2);
    CHECK(out["steps"].size() == 2);
    CHECK(out["steps"][1]["expect_failure"].get<std::string>().find("/v") == 0);
    CHECK(f.ended); // script-scoped rules are dropped even when it fails
}

TEST_CASE("a run reports each step as it starts and ends", "[Mcp]")
{
    FakeHost f;
    f.tools["a"] = [](const json&) { return ToolResult::ok(); };
    f.tools["b"] = [](const json&) { return ToolResult::ok(); };
    const Script             s = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "a"}, {"tool": "b"}]})");
    std::vector<std::string> events;
    ScriptHost               h = f.host();
    h.step_started             = [&](std::size_t index) { events.push_back("started " + std::to_string(index)); };
    h.step_finished            = [&](const json& entry) {
        events.push_back("finished " + std::to_string(entry["index"].get<int>()));
        CHECK(entry["ok"] == true);
        CHECK(entry["duration_ms"].get<long long>() >= 0);
    };
    ScriptContext ctx;
    run_script(s, ctx, h);
    CHECK(events == std::vector<std::string>{"started 1", "finished 1", "started 2", "finished 2"});
}

TEST_CASE("only_step runs one step and reports its own index", "[Mcp]")
{
    FakeHost f;
    for (const char* name : {"a", "b", "c"})
        f.tools[name] = [](const json&) { return ToolResult::ok(); };
    const Script  s = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "a"}, {"tool": "b"}, {"tool": "c"}]})");
    ScriptContext ctx;
    const json    out = run_script(s, ctx, f.host(), 2);
    REQUIRE(f.calls.size() == 1);
    CHECK(f.calls[0].first == "b");
    REQUIRE(out["steps"].size() == 1);
    CHECK(out["steps"][0]["index"] == 2);
    CHECK(out["passed"] == true);
    CHECK_THROWS_AS(run_script(s, ctx, f.host(), 4), std::out_of_range);
}

TEST_CASE("jobs saved by an earlier run resolve in a later single step", "[Mcp]")
{
    FakeHost f;
    f.tools["x"]    = [](const json&) { return ToolResult::ok({{"status", "running"}, {"job", "j7"}}); };
    f.tools["wait"] = [](const json& a) { return ToolResult::ok({{"job", a["job"]}}); };
    const Script s  = parse_ok(R"({"orca_script": 1, "steps": [
        {"tool": "x", "await": false, "save_as": "prefs"},
        {"tool": "wait", "args": {"job": "${jobs.prefs}"}},
        {"tool": "wait", "args": {"job": "${jobs.none}"}}]})");

    ScriptContext fresh;
    const json    alone = run_script(s, fresh, f.host(), 2);
    CHECK(alone["steps"][0]["ok"] == false);
    CHECK(alone["steps"][0]["error"].get<std::string>().find("no job is saved as prefs yet; step 1 saves it") != std::string::npos);
    CHECK(alone["steps"][0]["reason"] == json{{"kind", "missing_job"}, {"name", "prefs"}, {"saved_by", 1}});

    ScriptContext shared;
    run_script(s, shared, f.host(), 1);
    run_script(s, shared, f.host(), 2);
    CHECK(f.calls.back().second["job"] == "j7");

    const json unsaved = run_script(s, shared, f.host(), 3);
    CHECK(unsaved["steps"][0]["reason"]["kind"] == "missing_job");
    CHECK(unsaved["steps"][0]["reason"]["saved_by"].is_null());
}

TEST_CASE("a stop during a step marks that step, even the last one", "[Mcp]")
{
    const std::string text = GENERATE(as<std::string>{}, R"({"orca_script": 1, "steps": [{"tool": "stopper"}]})",
                                      R"({"orca_script": 1, "steps": [{"tool": "stopper", "optional": true}]})");
    const bool        single = GENERATE(false, true);
    FakeHost          f;
    f.tools["stopper"] = [&f](const json&) {
        f.cancel = true;
        return ToolResult::ok({{"plates", json::array()}});
    };
    json       reported;
    ScriptHost h       = f.host();
    h.step_finished    = [&](const json& entry) { reported = entry; };
    const Script  s    = parse_ok(text);
    ScriptContext ctx;
    const json    out = single ? run_script(s, ctx, h, 1) : run_script(s, ctx, h);
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
    const Script s  = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "slow"}]})");

    ScriptHost stopping = f.host();
    stopping.wait_job   = [&f](const std::string&, int) -> std::optional<ToolResult> {
        f.cancel = true;
        return std::nullopt;
    };
    ScriptContext ctx;
    const json    stopped = run_script(s, ctx, stopping);
    CHECK(stopped["steps"][0]["cancelled"] == true);
    CHECK(stopped["steps"][0]["reason"]["kind"] == "cancelled");

    FakeHost   g;
    g.tools        = f.tools;
    const json out = run_script(s, ctx, g.host());
    CHECK(out["steps"][0]["reason"] == json{{"kind", "timeout"}, {"seconds", 120}});
}

TEST_CASE("a job wait that times out names the dialog left open", "[Mcp]")
{
    FakeHost f;
    f.tools["opens"] = [](const json&) { return ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j1"}}); };
    const Script s   = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "opens"}]})");

    const json dialogs = json::array({{{"title", "Uniform scale"}, {"native", false}}, {{"title", "Preferences"}, {"native", false}}});
    ScriptHost host    = f.host();
    host.open_dialogs  = [&dialogs] { return dialogs; };
    ScriptContext ctx;
    const json out          = run_script(s, ctx, host);
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
    f.tools["slice"] = [](const json&) { return ToolResult::ok({{"plates", {{{"outcome", "error"}}}}}); };
    const Script  s  = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}}]})");
    ScriptContext ctx;
    const json    entry = run_script(s, ctx, f.host())["steps"][0];
    CHECK(entry["reason"] == json{{"kind", "expect"}, {"pointer", "/plates/0/outcome"}, {"op", "eq"},
                                  {"expected", "ok"},  {"actual", "error"},              {"exists", true}});
    CHECK(entry["expect_failure"].get<std::string>().find("/plates/0/outcome: expected eq \"ok\"") == 0);
}

TEST_CASE("a matcher that is not an object is a malformed expectation", "[Mcp]")
{
    FakeHost f;
    f.tools["slice"] = [](const json&) { return ToolResult::ok({{"outcome", "ok"}}); };
    const Script  s  = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "slice", "expect": {"/outcome": "ok"}}]})");
    ScriptContext ctx;
    const json    entry = run_script(s, ctx, f.host())["steps"][0];
    CHECK(entry["ok"] == false);
    CHECK(entry["reason"] == json{{"kind", "expect_invalid"}});
    CHECK(entry["expect_failure"].get<std::string>().find("must be an object") != std::string::npos);
}

// The script runner's step descriptions mirror this: an inline nested script reads its placeholders
// from the outer script, whose substitution runs before the nested run starts.
TEST_CASE("an inline nested script's placeholders are the outer script's", "[Mcp]")
{
    FakeHost f;
    f.tools["run_script"] = [](const json&) { return ToolResult::ok({{"passed", true}}); };
    const Script with_outer = parse_ok(R"({"orca_script": 1, "vars": {"p": "outer.stl"}, "steps": [{"tool": "run_script",
        "args": {"vars": {"p": "step.stl"}, "script": {"orca_script": 1, "steps": [{"tool": "open_files", "args": {"paths": ["${vars.p}"]}}]}}}]})");
    ScriptContext ctx;
    CHECK(run_script(with_outer, ctx, f.host())["passed"] == true);
    REQUIRE(f.calls.size() == 1);
    CHECK(f.calls[0].second["script"]["steps"][0]["args"]["paths"][0] == "outer.stl");

    FakeHost     g;
    const Script step_only = parse_ok(R"({"orca_script": 1, "steps": [{"tool": "run_script",
        "args": {"vars": {"p": "step.stl"}, "script": {"orca_script": 1, "steps": [{"tool": "open_files", "args": {"paths": ["${vars.p}"]}}]}}}]})");
    const json   out = run_script(step_only, ctx, g.host());
    CHECK(out["passed"] == false);
    CHECK(out["steps"][0]["reason"] == json{{"kind", "placeholder"}, {"text", "${vars.p}"}});
    CHECK(g.calls.empty());
}

TEST_CASE("lenient substitution keeps what it cannot resolve", "[Mcp]")
{
    const Script  s = parse_ok(R"({"orca_script": 1, "vars": {"model": "C:/m/part.stl"}, "steps": []})");
    ScriptContext ctx;
    CHECK(substitute_lenient("${vars.model}", s, ctx) == "C:/m/part.stl");
    CHECK(substitute_lenient("${jobs.none}/x", s, ctx) == "${jobs.none}/x");
    CHECK(substitute_lenient(json{{"p", "${vars.nope}"}}, s, ctx) == json{{"p", "${vars.nope}"}});
}

TEST_CASE("malformed scripts are errors, not exceptions", "[Mcp]")
{
    const auto [text, field] = GENERATE(table<std::string, std::string>({
        {R"({"orca_script": "1", "steps": []})", "orca_script"},
        {R"({"orca_script": 1, "name": 5, "steps": []})", "name"},
        {R"({"orca_script": 1, "steps": [{"tool": 3}]})", "step 1: \"tool\""},
        {R"({"orca_script": 1, "steps": [{"tool": "a", "timeout_s": "9"}]})", "step 1: \"timeout_s\""},
        {R"({"orca_script": 1, "steps": [{"tool": "a", "optional": "yes"}]})", "step 1: \"optional\""},
        {R"([])", "orca_script"},
        {R"({})", "orca_script"},
    }));
    std::string          err;
    std::optional<Script> s;
    CHECK_NOTHROW(s = parse_script(text, err));
    CHECK_FALSE(s.has_value());
    CHECK(err.find(field) != std::string::npos);
}

TEST_CASE("broken JSON says where", "[Mcp]")
{
    std::string err;
    CHECK_FALSE(parse_script("{\n  \"orca_script\": 1,\n  \"steps\": [ {\"tool\": \"a\",, } ]\n}", err).has_value());
    CHECK(err.find("line 3, column") != std::string::npos);
}
