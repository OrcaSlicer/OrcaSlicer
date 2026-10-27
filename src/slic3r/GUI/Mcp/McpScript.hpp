#pragma once

#include "McpTypes.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

struct ScriptStep
{
    std::string    tool;
    nlohmann::json args   = nlohmann::json::object();
    nlohmann::json expect = nlohmann::json::object();
    bool           optional = false;
    // Wait for a step that returns a job before evaluating `expect`. False hands the job back at once
    // (stored under save_as) so later steps can deal with a dialog it opened.
    bool           await     = true;
    std::string    save_as;
    int            timeout_s = 120;
};

struct Script
{
    std::string             name;
    nlohmann::json          vars = nlohmann::json::object();
    std::vector<ScriptStep> steps;
};

struct ScriptContext
{
    boost::filesystem::path script_dir;
    boost::filesystem::path out_dir;
    nlohmann::json          jobs = nlohmann::json::object(); // save_as name -> job id
};

// Parses a script (JSON with // and /* */ comments). nullopt with `err` set when invalid.
std::optional<Script> parse_script(const std::string& text, std::string& err);

// Replaces ${script_dir}, ${out_dir}, ${env:NAME}, ${vars.NAME} and ${jobs.NAME} in every string of
// `value`. A string that is exactly one placeholder takes the referenced JSON value (so a var can
// hold a number or an array); otherwise the placeholder's text is spliced in. Unknown placeholders
// are an error.
nlohmann::json substitute(const nlohmann::json& value, const Script& script, const ScriptContext& ctx, std::string& err);

// Checks `expect` ({"/json/pointer": {"op": operand, ...}, ...}) against a step's result. nullopt
// when every matcher passes, else a message naming the pointer and matcher that failed.
// Matchers: eq ne gt ge lt le contains regex exists empty.
std::optional<std::string> check_expect(const nlohmann::json& result, const nlohmann::json& expect);

// What run_script needs from its host: calling a tool as a child of the script's job, waiting for
// a job's result, and dropping script-scoped dialog auto-answer rules when the script ends.
struct ScriptHost
{
    std::function<ToolResult(const std::string& tool, const nlohmann::json& args)>              call;
    std::function<std::optional<ToolResult>(const std::string& job, int timeout_s)>            wait_job;
    std::function<void()>                                                                       end_script;
    std::function<bool()>                                                                       cancelled;
};

// Runs the steps in order and returns {passed, failed_step?, steps:[{index, tool, ok, result|error,
// expect_failure?, skipped?}]}. Stops at the first failed step or expectation unless the step is
// optional.
nlohmann::json run_script(const Script& script, ScriptContext& ctx, const ScriptHost& host);

}}} // namespace Slic3r::GUI::Mcp
