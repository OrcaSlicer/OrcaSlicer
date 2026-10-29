// run_script and export_script: deterministic replays of tool calls, and turning a session into one.

#include "JobRegistry.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "McpJournal.hpp"
#include "McpPaths.hpp"
#include "McpScript.hpp"
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

// An invalid script is a tool error flagged `invalid`, which --mcp-script turns into exit code 2.
ToolResult invalid(const std::string& message) { return ToolResult::error("Invalid script: " + message, {{"invalid", true}}); }

} // namespace

json run_script_job(McpService& service, ToolContext& ctx, const Script& script, ScriptContext& sctx, const ScriptRunOptions& options)
{
    boost::system::error_code ec;
    fs::create_directories(sctx.out_dir, ec);
    if (ec)
        throw ToolFailure("Cannot create " + path_to_utf8(sctx.out_dir) + ": " + ec.message());

    const std::string job = ctx.job_id();
    service.begin_script_scope(job);
    struct ScopeEnd
    {
        McpService& service;
        std::string job;
        ~ScopeEnd() { service.end_script_scope(job); }
    } scope_end{service, job};

    const auto stopped = [&ctx] { return ctx.cancelled(); };

    ScriptHost host;
    host.call     = [&ctx](const std::string& tool, const json& a) { return ctx.call_child(tool, a); };
    host.wait_job = [&service, &stopped](const std::string& id, int timeout_s) {
        return service.jobs().wait(id, std::chrono::seconds(timeout_s), stopped);
    };
    host.cancelled     = stopped;
    host.step_started  = options.step_started;
    host.step_finished = options.step_finished;

    json result       = run_script(script, sctx, host, options.only_step);
    result["out_dir"] = path_to_utf8(sctx.out_dir);
    if (options.write_result) {
        const fs::path file = sctx.out_dir / "result.json";
        write_file(file, result.dump(2));
        result["result_file"] = path_to_utf8(file);
    }
    return result;
}

void register_script_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    ToolDef run = mutate_tool(
        "run_script", L("Run a script"),
        "Run a script of tool calls in order and report each step: {orca_script:1, name, vars, steps:[{tool, args, expect, "
        "optional, await, save_as, timeout_s}]}, JSON with comments. Strings may use ${script_dir}, ${out_dir}, ${env:NAME}, "
        "${vars.NAME} and ${jobs.NAME}. A step that returns a job is awaited unless await:false, which stores the job under "
        "save_as so later steps can answer the dialog it opened. expect maps a JSON pointer into the step's result to "
        "matchers (eq ne gt ge lt le contains regex exists empty); the first failed step or expectation stops the run unless "
        "the step is optional. Writes result.json to out_dir (default: <script name>.out next to a script file). "
        "dialog_auto_answer rules a script registers end with it. export_script turns this session into a script.",
        object_schema({{"path", prop("string", "Absolute path of a script file.")},
                       {"script", {{"type", "object"}, {"description", "The script itself, instead of path."}}},
                       {"out_dir", prop("string", "Absolute directory for result.json and screenshots; created if missing.")},
                       {"vars", {{"type", "object"}, {"description", "Values that override the script's vars."}}}}),
        [&service](const json& args, ToolContext& ctx) {
            const bool has_path = args.contains("path"), has_script = args.contains("script");
            if (has_path == has_script)
                return invalid("give exactly one of path and script");

            std::optional<Script> script;
            ScriptContext         sctx;
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
                script          = parse_script(text, err);
                sctx.script_dir = path->parent_path();
                default_out     = path->parent_path() / path->stem();
                default_out += ".out";
            } else {
                script      = parse_script(args["script"].dump(), err);
                default_out = service.output_dir() / ("script-" + timestamp());
            }
            if (!script)
                return invalid(err);
            for (const ScriptStep& step : script->steps)
                if (service.registry().find(step.tool) == nullptr)
                    return invalid("unknown tool " + step.tool);
            if (args.contains("vars"))
                for (const auto& [key, value] : args["vars"].items())
                    script->vars[key] = value;

            sctx.out_dir = args.contains("out_dir") ? path_from_utf8(arg_string(args, "out_dir")) : default_out;
            if (!sctx.out_dir.is_absolute())
                return invalid("out_dir must be an absolute path");
            if (!has_path)
                sctx.script_dir = sctx.out_dir;
            json result = run_script_job(service, ctx, *script, sctx, ScriptRunOptions{});
            return ToolResult::ok(std::move(result));
        });
    reg.add(run);

    ToolDef exp = inspect_tool(
        "export_script", L("Export this session as a script"),
        "Turn the calls this session made into a script run_script can replay: mutating calls, wait and screenshot, in order, "
        "with refs replaced by locators, job replies replayed with await:false, and screenshot paths moved under ${out_dir}. "
        "Calls that failed, at once or as a job later, are left out; so is a call whose job is still running (unless it "
        "waits on a dialog), which left_out_running lists as {tool, job}. "
        "Returns the script; with path, also writes it. clear:true starts a new recording afterwards.",
        object_schema({{"name", prop("string", "Script name.")},
                       {"path", prop("string", "Absolute .json path to write.")},
                       {"overwrite", prop("boolean", "Replace an existing file.")},
                       {"clear", prop("boolean", "Forget the calls recorded so far once exported.")}}),
        [&service](const json& args, ToolContext&) {
            json running;
            json script = service.journal().to_script(arg_string(args, "name", "session " + timestamp()), &running);
            json out{{"steps", script["steps"].size()}};
            if (!running.empty())
                out["left_out_running"] = std::move(running);
            if (args.contains("path")) {
                std::string err;
                const auto  path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
                if (!path)
                    throw ToolFailure(err);
                write_file(*path, script.dump(2));
                out["path"] = path_to_utf8(*path);
            }
            if (arg_bool(args, "clear", false))
                service.journal().clear();
            out["script"] = std::move(script);
            return ToolResult::ok(std::move(out));
        });
    exp.read_only = false;
    reg.add(exp);
}

}}} // namespace Slic3r::GUI::Mcp
