#pragma once

#include "slic3r/GUI/CommandTypes.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

struct ReplayStep
{
    std::string    tool;
    nlohmann::json args   = nlohmann::json::object();
    nlohmann::json expect = nlohmann::json::object();
    bool           optional = false;
    // Wait for a step that returns a job before evaluating `expect`. False hands the job back at once
    // (stored under save_as) so later steps can deal with a dialog it opened; a call that finished within its
    // reply budget leaves no job, and an app_wait on it gives the step's result.
    bool           await     = true;
    std::string    save_as;
    int            timeout_s = 120;
};

struct Replay
{
    std::string             name;
    nlohmann::json          vars = nlohmann::json::object();
    std::vector<ReplayStep> steps;
};

struct ReplayContext
{
    boost::filesystem::path replay_dir;
    boost::filesystem::path out_dir;
    nlohmann::json          jobs = nlohmann::json::object(); // save_as name -> job id
    // save_as name -> the result of an "await": false step whose call finished within its reply budget, which left no
    // job: an app_wait on that job gives this result, as it gives a finished job's.
    nlohmann::json          finished = nlohmann::json::object();
};

// The version of the replay format this build writes, the semver string a replay gives as "orca_replay_version".
// A replay is read when it has this major version and a minor version no newer than this one; while the major
// version is 0, every minor version may break, so only this minor version is read. A replay of another format, whose
// steps may name commands this build does not have, is refused for its format before any step is read. The editor's
// draft and key suggestions write the same version (REPLAY_VERSION in resources/web/dialog/ActionReplay/replay_editor.js).
inline constexpr const char* REPLAY_VERSION = "0.2.0";

// Parses a replay (JSON with // and /* */ comments). nullopt with `err` set when invalid.
std::optional<Replay> parse_replay(const std::string& text, std::string& err);

// The text of a replay_run inline replay, which is given as the replay object or as its JSON text.
std::string inline_replay_text(const nlohmann::json& replay);

// Replaces ${replay_dir}, ${out_dir}, ${env:NAME}, ${vars.NAME} and ${jobs.NAME} in every string of
// `value`. A string that is exactly one placeholder takes the referenced JSON value (so a var can
// hold a number or an array); otherwise the placeholder's text is spliced in. Unknown placeholders
// are an error.
nlohmann::json substitute(const nlohmann::json& value, const Replay& replay, const ReplayContext& ctx, std::string& err);

// As substitute(), but a placeholder that can't be resolved stays in the text and is never an error:
// for describing steps before a run, when saved jobs and the like may not exist yet.
nlohmann::json substitute_lenient(const nlohmann::json& value, const Replay& replay, const ReplayContext& ctx);

// As substitute_lenient(), with what is put in bounded: each value put in takes its size from `budget` (an empty string
// or null nothing), and once it is spent the placeholders left stay as written. Variables that put each other in grow as the power of
// the nesting, which reading what a replay will put in (the activity log's masking) must not follow without end.
nlohmann::json substitute_lenient(const nlohmann::json& value, const Replay& replay, const ReplayContext& ctx, std::size_t& budget);

// A step's arguments as the bounded substitute_lenient() fills them in, except Python source, which a replay runs as
// written: the code of a script_run call, of one a tool_call makes, and of those an inline replay's steps make.
nlohmann::json substitute_args_lenient(const std::string& tool, const nlohmann::json& args, const Replay& replay,
                                       const ReplayContext& ctx, std::size_t& budget);

// What a replay puts in for one placeholder body ("vars.model", "env:HOME", "jobs.save", "replay_dir"); nullopt for
// one it can't resolve.
std::optional<nlohmann::json> resolve_placeholder(const std::string& key, const Replay& replay, const ReplayContext& ctx);

// Checks `expect` ({"/json/pointer": {"op": operand, ...}, ...}) against a step's result. nullopt
// when every matcher passes, else a message naming the pointer and matcher that failed.
// Matchers: eq ne gt ge lt le contains regex exists empty.
std::optional<std::string> check_expect(const nlohmann::json& result, const nlohmann::json& expect);

// The first matcher check_expect() finds failing, in parts a host can word for people. `op` is empty
// when the expectation itself is malformed (a bad pointer, a matcher that isn't an object).
struct ExpectFailure
{
    std::string    message; // what check_expect() returns
    std::string    pointer;
    std::string    op;
    nlohmann::json expected;
    nlohmann::json actual; // null when the pointer names nothing
    bool           exists = false;
};
std::optional<ExpectFailure> check_expect_detail(const nlohmann::json& result, const nlohmann::json& expect);

// What replay_run needs from its host: calling a tool as a child of the replay's job, waiting for
// a job's result, and dropping replay-scoped dialog auto-answer rules when the replay ends.
// step_started and step_finished, when set, report each step as it runs: step_started (1-based
// index) before its arguments are substituted, step_finished with the entry as it goes into the
// result, after the check for a stop during the step.
struct ReplayHost
{
    std::function<ToolResult(const std::string& tool, const nlohmann::json& args)>              call;
    std::function<std::optional<ToolResult>(const std::string& job, int timeout_s)>            wait_job;
    std::function<void()>                                                                       end_replay;
    std::function<bool()>                                                                       cancelled;
    std::function<void(std::size_t index)>                                                      step_started;
    std::function<void(const nlohmann::json& entry)>                                            step_finished;
    // The open modal dialogs, topmost last, as app_state lists them. Optional.
    std::function<nlohmann::json()>                                                             open_dialogs;
};

// Runs the steps in order and returns {passed, failed_step?, steps:[{index, tool, args, ok,
// duration_ms, result|error, details?, expect_failure?, cancelled?, reason?}]}. Stops at the first
// failed step or expectation unless the step is optional. A step during which `cancelled` became true
// is failed as cancelled whatever its outcome or `optional`, and ends the run. A failed step's
// `reason` says why: {kind: cancelled | timeout {seconds} | expect {pointer, op, expected, actual,
// exists} | expect_invalid | placeholder {text} | missing_job {name, saved_by} | tool}.
// `only_step` (1-based) runs just that step, with the jobs `ctx` holds from earlier runs; out of
// range throws std::out_of_range.
nlohmann::json run_replay(const Replay& replay, ReplayContext& ctx, const ReplayHost& host,
                          std::optional<std::size_t> only_step = std::nullopt);

}}} // namespace Slic3r::GUI::Mcp
