#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Mcp/McpScript.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    bool                                                          ended = false;

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
        h.cancelled  = [] { return false; };
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
