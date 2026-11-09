// run_replay and export_replay: deterministic replays of tool calls, and turning a session into one.

#include "JobRegistry.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "McpJournal.hpp"
#include "McpPaths.hpp"
#include "McpRecorder.hpp"
#include "McpReplay.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

std::string read_file(const fs::path& path)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    if (!in)
        throw ToolFailure("Cannot read " + path_to_utf8(path));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& text)
{
    boost::nowide::ofstream out(path_to_utf8(path).c_str(), std::ios::binary | std::ios::trunc);
    out << text;
    if (!out)
        throw ToolFailure("Cannot write " + path_to_utf8(path));
}

std::string timestamp()
{
    const std::time_t t = std::time(nullptr);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

// An invalid replay is a tool error flagged `invalid`, which --mcp-run turns into exit code 2.
ToolResult invalid(const std::string& message) { return ToolResult::error("Invalid replay: " + message, {{"invalid", true}}); }

} // namespace

json run_replay_job(McpService& service, ToolContext& ctx, const Replay& replay, ReplayContext& sctx, const ReplayRunOptions& options)
{
    boost::system::error_code ec;
    fs::create_directories(sctx.out_dir, ec);
    if (ec)
        throw ToolFailure("Cannot create " + path_to_utf8(sctx.out_dir) + ": " + ec.message());

    const std::string job = ctx.job_id();
    service.begin_run_scope(job);
    struct ScopeEnd
    {
        McpService& service;
        std::string job;
        bool        recording_waited = false; // the run below waited for the replay's recording already
        ~ScopeEnd() { service.end_run_scope(job, !recording_waited); }
    } scope_end{service, job};

    const auto stopped = [&ctx] { return ctx.cancelled(); };

    ReplayHost host;
    host.call     = [&ctx](const std::string& tool, const json& a) { return ctx.call_child(tool, a); };
    host.wait_job = [&service, &stopped](const std::string& id, int timeout_s) {
        return service.jobs().wait(id, std::chrono::seconds(timeout_s), stopped);
    };
    host.cancelled     = stopped;
    host.step_started  = options.step_started;
    host.step_finished = options.step_finished;
    host.open_dialogs  = [&service] { return service.ui().open_dialogs(); };

    json result = run_replay(replay, sctx, host, options.only_step);
    // A recording the replay started ends with it, its file closed before the result is written; one still
    // being written after the wait is named, since an encode of it would fail.
    std::optional<std::string> unfinished = service.recorder().end_owned_by(job, "run_ended", true);
    scope_end.recording_waited            = true;
    if (unfinished)
        result["unfinished_recording"] = *unfinished;
    result["out_dir"] = path_to_utf8(sctx.out_dir);
    if (options.write_result) {
        const fs::path file = sctx.out_dir / "result.json";
        write_file(file, result.dump(2));
        result["result_file"] = path_to_utf8(file);
    }
    return result;
}

void register_replay_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    ToolDef run = mutate_tool(
        "run_replay", L("Run a replay"),
        "Replay a list of tool calls in order and report each step: {orca_replay:1, name, vars, steps:[{tool, args, expect, "
        "optional, await, save_as, timeout_s}]}, JSON with comments. Strings may use ${replay_dir}, ${out_dir}, ${env:NAME}, "
        "${vars.NAME} and ${jobs.NAME}. A step that returns a job is awaited unless await:false, which stores the job under "
        "save_as so later steps can answer the dialog it opened. expect maps a JSON pointer into the step's result to "
        "matchers (eq ne gt ge lt le contains regex exists empty); the first failed step or expectation stops the run unless "
        "the step is optional. Writes result.json to out_dir (default: <replay name>.out next to a replay file). "
        "auto_answer_dialogs rules a replay registers end with it. export_replay turns this session into one.",
        object_schema({{"path", prop("string", "Absolute path of a replay file.")},
                       {"replay", {{"type", json::array({"object", "string"})},
                                   {"description", "The replay itself, as an object or as its JSON text, instead of path."}}},
                       {"out_dir", prop("string", "Absolute directory for result.json and screenshots; created if missing.")},
                       {"vars", {{"type", "object"}, {"description", "Values that override the replay's vars."}}}}),
        [&service](const json& args, ToolContext& ctx) {
            const bool has_path = args.contains("path"), has_replay = args.contains("replay");
            if (has_path == has_replay)
                return invalid("give exactly one of path and replay");

            std::optional<Replay> replay;
            ReplayContext         sctx;
            fs::path              default_out;
            std::string           err;
            if (has_path) {
                const auto path = resolve_input_path(arg_string(args, "path"), err);
                if (!path)
                    return invalid(err);
                std::string text;
                try {
                    text = read_file(*path);
                } catch (const ToolFailure& f) {
                    return invalid(f.what());
                }
                replay          = parse_replay(text, err);
                sctx.replay_dir = path->parent_path();
                default_out     = path->parent_path() / path->stem();
                default_out += ".out";
            } else {
                replay      = parse_replay(inline_replay_text(args["replay"]), err);
                default_out = output_dir() / ("replay-" + timestamp());
            }
            if (!replay)
                return invalid(err);
            for (const ReplayStep& step : replay->steps)
                if (service.registry().lookup(step.tool) == nullptr)
                    return invalid("unknown tool " + step.tool);
            if (args.contains("vars"))
                for (const auto& [key, value] : args["vars"].items())
                    replay->vars[key] = value;

            sctx.out_dir = args.contains("out_dir") ? path_from_utf8(arg_string(args, "out_dir")) : default_out;
            if (!sctx.out_dir.is_absolute())
                return invalid("out_dir must be an absolute path");
            if (!has_path)
                sctx.replay_dir = sctx.out_dir;
            json result = run_replay_job(service, ctx, *replay, sctx, ReplayRunOptions{});
            return ToolResult::ok(std::move(result));
        });
    // Its steps call any tool, run_script included.
    run.open_world = true;
    reg.add(run);

    ToolDef exp = inspect_tool(
        "export_replay", L("Export this session as a replay"),
        "Turn the calls this session made into a replay that run_replay runs: mutating calls, wait and screenshot, in order, "
        "with refs replaced by locators, job replies replayed with await:false, and screenshot paths moved under ${out_dir}. "
        "Calls that failed, at once or as a job later, are left out; so is a call whose job is still running (unless it "
        "waits on a dialog), which left_out_running lists as {tool, job}. "
        "Reply: the replay, which with path is also written there. clear:true starts a new recording afterwards.",
        object_schema({{"name", prop("string", "Replay name.")},
                       {"path", prop("string", "Absolute .json path to write.")},
                       {"overwrite", prop("boolean", "Replace an existing file.")},
                       {"clear", prop("boolean", "Forget the calls recorded so far once exported.")}}),
        [&service](const json& args, ToolContext& ctx) {
            if (!ctx.caller_plugin().empty())
                throw ToolFailure("export_replay exports the agent's session, which a plugin's calls are not part of");
            json running;
            json replay = service.journal().to_replay(arg_string(args, "name", "session " + timestamp()), &running);
            json out{{"steps", replay["steps"].size()}};
            if (!running.empty())
                out["left_out_running"] = std::move(running);
            if (args.contains("path")) {
                std::string err;
                const auto  path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
                if (!path)
                    throw ToolFailure(err);
                write_file(*path, replay.dump(2));
                out["path"] = path_to_utf8(*path);
            }
            if (arg_bool(args, "clear", false))
                service.journal().clear();
            out["replay"] = std::move(replay);
            return ToolResult::ok(std::move(out));
        });
    exp.read_only = false;
    // clear empties the recording, so a second call exports less.
    exp.idempotent = false;
    reg.add(exp);
}

}}} // namespace Slic3r::GUI::Mcp
