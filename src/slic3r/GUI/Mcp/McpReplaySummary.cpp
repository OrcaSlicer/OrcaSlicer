#include "McpReplaySummary.hpp"
#include "McpArguments.hpp"
#include "McpPaths.hpp"
#include "McpReplay.hpp"

#include "slic3r/GUI/ActionIds.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <ios>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// Nested replays are read this deep; a deeper one counts as unreadable.
constexpr int kMaxDepth = 4;

constexpr std::size_t kFlagCount = 5;

// The description being built: flags as a set, so nested steps can be joined in.
struct Builder
{
    std::array<bool, kFlagCount> flags{};
    StepDescription              d;

    void flag(StepFlag f) { flags[std::size_t(f)] = true; }
    void replace(const char* kind)
    {
        // "checked" wins: it is the kind the runner's answer reaches, so it decides whether to ask.
        if (d.replace != "checked")
            d.replace = kind;
    }
    void every_flag(const char* replace_kind)
    {
        flags.fill(true);
        replace(replace_kind);
        d.answers_dialogs = true;
    }
    void join(const StepDescription& nested)
    {
        for (StepFlag f : nested.flags)
            flag(f);
        if (!nested.replace.empty())
            replace(nested.replace.c_str());
        d.answers_dialogs = d.answers_dialogs || nested.answers_dialogs;
    }
    StepDescription done()
    {
        for (std::size_t i = 0; i < kFlagCount; ++i)
            if (flags[i])
                d.flags.push_back(StepFlag(i));
        return std::move(d);
    }
};

std::string file_name(const std::string& path) { return path_to_utf8(path_from_utf8(path).filename()); }

bool has_extension(const std::string& path, std::initializer_list<const char*> extensions)
{
    for (const char* e : extensions)
        if (boost::algorithm::iends_with(path, e))
            return true;
    return false;
}

bool same_component(const fs::path& a, const fs::path& b)
{
#ifdef _WIN32
    return boost::algorithm::iequals(a.generic_wstring(), b.generic_wstring());
#else
    return a == b;
#endif
}

// `path` names a file inside `dir`, compared component by component after lexical normalization.
bool inside(const std::string& path, const fs::path& dir)
{
    if (dir.empty())
        return false;
    const fs::path p = path_from_utf8(path).lexically_normal();
    const fs::path d = dir.lexically_normal();
    auto           pi = p.begin();
    for (auto di = d.begin(); di != d.end(); ++di) {
        if (*di == "." || di->empty())
            continue;
        if (pi == p.end() || !same_component(*di, *pi))
            return false;
        ++pi;
    }
    return pi != p.end();
}

// A Speed Dial id "<prefix>:<middle>:<source>"; the middle may itself hold colons (a Windows path).
std::pair<std::string, std::string> split_action_id(const std::string& id)
{
    const auto first = id.find(':');
    const auto last  = id.rfind(':');
    if (first == std::string::npos || last == first)
        return {id, {}};
    return {id.substr(0, first), id.substr(first + 1, last - first - 1)};
}

bool one_of(const std::string& s, std::initializer_list<const char*> names)
{
    for (const char* n : names)
        if (s == n)
            return true;
    return false;
}

// A post_process value that names a script: a text that is not empty, or a list holding one.
bool names_script(const json& value)
{
    if (value.is_string())
        return !value.get_ref<const std::string&>().empty();
    return value.is_array() && std::any_of(value.begin(), value.end(), names_script);
}

// The effects of command `key` called with `args`, whichever way it is called: the files it writes, the presets it
// changes, what it sends to a printer, the programs it can start.
void flag_effects(Builder& b, const std::string& key, const json& args)
{
    // A script is a program of its own, and a post-processing script is one the next G-code export starts.
    const bool sets_script = key == "settings_set" && args.is_object() && args.contains("values") && args["values"].is_object() &&
                             args["values"].contains("post_process") && names_script(args["values"]["post_process"]);
    if (key == "script_run" || sets_script)
        b.flag(StepFlag::RunsProgram);
    if (one_of(key, {"project_save", "project_export", "preset_export"}))
        b.flag(StepFlag::WritesFile);
    // The preset bundle manager imports and deletes bundles of presets.
    const bool preset_bundle = key == "window_open" && args.is_object() && args.value("window", std::string()) == "preset_bundle";
    if (one_of(key, {"preset_import", "preset_sync"}) || preset_bundle)
        b.flag(StepFlag::ChangesPresets);
    // Their dialogs print on a printer or send it a file, and the job commands pause, resume or stop its print;
    // printer_test_host only asks the host whether it answers, and printer_status reads what the app holds.
    if (one_of(key, {"print_start", "printer_send", "print_pause", "print_resume", "print_stop"}))
        b.flag(StepFlag::SendsToPrinter);
}

// Each argument of `args` as "name: value", the file of a path by its name.
json argument_texts(const json& args)
{
    json texts = json::array();
    for (const auto& [name, value] : args.items()) {
        const bool file = name == "path" && value.is_string();
        texts.push_back(name + ": " + (file ? file_name(value.get<std::string>()) : value.is_string() ? value.get<std::string>() : value.dump()));
    }
    return texts;
}

// A call of command `key` with `args`, or of its action `action` (one of its actions, which runs it with `args`): a
// call whose arguments are an action's is that action, `action` or else the first with them (action_with_args()). An
// action that collects a value is described with the call's arguments, which hold that value.
void describe_command(Builder& b, const CommandCatalog& catalog, const std::string& key, const json& args,
                      const json* action = nullptr)
{
    json what{{"kind", "command"}, {"key", key}};
    if (action == nullptr)
        if (const NativeCommand* command = catalog.find(key))
            action = action_with_args(*command, args);
    if (action != nullptr)
        what["action"] = action->at("key");
    if ((action == nullptr || !action->value("input", std::string()).empty()) && args.is_object() && !args.empty())
        what["args"] = argument_texts(args);
    b.d.what = std::move(what);

    // What the command's action does, as a person's pick of it: a calibration test's dialog, which then replaces the
    // project and changes the presets, and Load Project and New Project, which ask about unsaved changes.
    if (key == "calibration_generate_test") {
        b.flag(StepFlag::ReplacesProject);
        b.flag(StepFlag::ChangesPresets);
        b.replace("app_asked");
    }
    if (one_of(key, {"project_open", "project_new"})) {
        b.flag(StepFlag::ReplacesProject);
        b.replace("app_asked");
    }
    flag_effects(b, key, args);
}

void describe_action(Builder& b, const CommandCatalog& catalog, const json& args)
{
    const std::string id    = args.contains("id") && args["id"].is_string() ? args["id"].get<std::string>() : std::string();
    const json        given = args.contains("args") ? args["args"] : json();
    // A command's action, by its full id or by the key inside it, as action_run takes either: a folded item's action runs
    // its command with the item's arguments, and the value it collects when it has one, as a person's pick of it does.
    if (const auto [command, action] = find_command_action(catalog, command_action_key(id)); command != nullptr) {
        describe_command(b, catalog, command->key, action_call_args(action, given), action);
        return;
    }
    const auto [prefix, middle] = split_action_id(id);
    if (prefix == kCommandPrefix) {
        // An action `catalog` does not hold (a Design tab action of a run without the Design tab), by the key in its id.
        describe_command(b, catalog, middle, given);
    } else if (prefix == "orca_recent_project") {
        b.d.what = {{"kind", "recent_project"}, {"name", file_name(middle)}};
        b.flag(StepFlag::ReplacesProject);
        b.replace("app_asked");
    } else if (prefix == "orca_plate_goto" || prefix == "orca_setting") {
        // A plate to show, or a setting to show in the sidebar: they change nothing.
        b.d.what = {{"kind", "value"}, {"text", middle}};
    } else {
        // Plugin replays and pages, or an id this list doesn't know: code whose effect can't be read.
        // Plugin code doesn't read the runner's permission to drop unsaved changes.
        b.d.what = {{"kind", "value"}, {"text", id}};
        b.every_flag("app_asked");
    }
}

// A command of `catalog` called as one of its actions: with the arguments of an action that runs it with them (a menu
// item folded into it), or with none, as its action of its own (plate_delete). A command with an action of its own that
// the tool list does not show (a Design command) is its action's call whatever its arguments. Called so, it does what
// action_run of that action does.
bool action_command(const CommandCatalog& catalog, const std::string& tool, const json& args)
{
    const NativeCommand* command = catalog.find(tool);
    return command != nullptr && (action_with_args(*command, args) != nullptr ||
                                  (command->has_own_action && (!command->listed || person_form(args))));
}

std::optional<std::string> read_text(const fs::path& path)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A replay_run step: the nested replay, read in the context its run will have.
void describe_nested(Builder& b, const CommandCatalog& catalog, const json& args, const ReplayContext& outer, int depth)
{
    std::optional<Replay> nested;
    ReplayContext         ctx;
    std::string           err;
    if (args.contains("replay") && (args["replay"].is_object() || args["replay"].is_string())) {
        nested = parse_replay(inline_replay_text(args["replay"]), err);
        // An inline replay is part of the step's args, which the outer replay substitutes before the
        // nested run starts: its placeholders are the outer replay's, already applied to `args`, and
        // its files land where the outer ${out_dir} points. A placeholder left is one the run fails on,
        // so the nested vars must not resolve it here.
        ctx = outer;
        if (nested)
            nested->vars = json::object();
        else
            b.d.what = {{"kind", "value"}, {"text", err}};
    } else if (args.contains("path") && args["path"].is_string()) {
        const fs::path path = path_from_utf8(args["path"].get<std::string>());
        if (const auto text = read_text(path))
            nested = parse_replay(*text, err);
        if (!nested)
            b.d.what = {{"kind", "files"}, {"names", {file_name(path_to_utf8(path))}}};
        ctx.replay_dir = path.parent_path();
        // Without out_dir the run names its folder when it starts; one named now stands for it, since what the nested
        // steps write under ${out_dir} is the run's output whatever the folder is called.
        ctx.out_dir = args.contains("out_dir") && args["out_dir"].is_string() ? path_from_utf8(args["out_dir"].get<std::string>()) :
                                                                                replay_output_dir();
        if (nested && args.contains("vars") && args["vars"].is_object())
            for (const auto& [key, value] : args["vars"].items())
                nested->vars[key] = value;
    }
    if (nested)
        b.d.what = {{"kind", "replay"}, {"steps", nested->steps.size()}};
    if (!nested || depth >= kMaxDepth) {
        b.every_flag("checked");
        return;
    }
    for (const ReplayStep& step : nested->steps)
        b.join(describe_step(catalog, step, *nested, ctx, depth + 1));
}

// The first scalar argument as "key: value", for tools without a more telling description.
json first_scalar(const json& args)
{
    for (const auto& [key, value] : args.items())
        if (value.is_primitive() && !value.is_null())
            return {{"kind", "value"}, {"text", key + ": " + (value.is_string() ? value.get<std::string>() : value.dump())}};
    return nullptr;
}

json describe_what(const std::string& tool, const json& args)
{
    auto text = [&](const char* key) -> json {
        if (args.contains(key) && args[key].is_primitive() && !args[key].is_null())
            return {{"kind", "value"}, {"text", args[key].is_string() ? args[key].get<std::string>() : args[key].dump()}};
        return nullptr;
    };
    if (one_of(tool, {"preset_select", "preset_save", "preset_delete"}) && args.contains("name") && args["name"].is_string())
        return {{"kind", "preset"}, {"name", args["name"]}};
    if (one_of(tool, {"plate_slice", "object_arrange", "print_start", "printer_send"}))
        return {{"kind", "plates"}, {"plates", args.contains("plates") ? args["plates"] : json(tool == "object_arrange" ? "all" : "current")}};
    if (tool == "app_wait")
        return text("for");
    if (tool == "dialog_answer")
        return text("button");
    if (args.contains("paths") && args["paths"].is_array()) {
        json names = json::array();
        for (const json& p : args["paths"])
            if (p.is_string())
                names.push_back(file_name(p.get<std::string>()));
        return {{"kind", "files"}, {"names", std::move(names)}};
    }
    for (const char* key : {"save_path", "path"})
        if (args.contains(key) && args[key].is_string())
            return {{"kind", "files"}, {"names", {file_name(args[key].get<std::string>())}}};
    if (args.contains("values") && args["values"].is_object()) {
        json pairs = json::array();
        for (const auto& [key, value] : args["values"].items())
            pairs.push_back(key + " = " + (value.is_string() ? value.get<std::string>() : value.dump()));
        return {{"kind", "settings"}, {"pairs", std::move(pairs)}};
    }
    return first_scalar(args);
}

} // namespace

json describe_call(const CommandCatalog& catalog, const std::string& tool, const json& args)
{
    if (tool == "action_run") {
        Builder b;
        describe_action(b, catalog, args);
        return b.d.what;
    }
    if (action_command(catalog, tool, args)) {
        Builder b;
        describe_command(b, catalog, tool, args);
        return b.d.what;
    }
    if (tool == "replay_run")
        return args.contains("path") && args["path"].is_string() ? json{{"kind", "files"}, {"names", {file_name(args["path"].get<std::string>())}}} :
                                                                    json();
    return describe_what(tool, args);
}

const char* flag_id(StepFlag flag)
{
    switch (flag) {
    case StepFlag::WritesFile: return "writes_file";
    case StepFlag::ChangesPresets: return "changes_presets";
    case StepFlag::ReplacesProject: return "replaces_project";
    case StepFlag::SendsToPrinter: return "sends_to_printer";
    case StepFlag::RunsProgram: return "runs_program";
    }
    return "";
}

StepDescription describe_step(const CommandCatalog& catalog, const ReplayStep& step, const Replay& replay, const ReplayContext& ctx,
                              int depth)
{
    json        args = substitute_lenient(step.args, replay, ctx);
    std::string tool = step.tool;
    Builder     b;
    b.d.checks_result = !step.expect.empty();

    // A tool_call runs as the call it makes, which is what the step is described as.
    if (tool == "tool_call") {
        const json call = std::move(args);
        tool = call.is_object() && call.contains("tool") && call["tool"].is_string() ? call["tool"].get<std::string>() : std::string();
        args = call.is_object() && call.contains("args") && call["args"].is_object() ? call["args"] : json::object();
    }

    if (tool == "action_run") {
        describe_action(b, catalog, args);
        return b.done();
    }
    if (tool == "replay_run") {
        describe_nested(b, catalog, args, ctx, depth);
        return b.done();
    }
    if (action_command(catalog, tool, args)) {
        describe_command(b, catalog, tool, args);
        return b.done();
    }
    b.d.what = describe_what(tool, args);

    // A file written into the run's own output folder is the run's output, not an effect to flag.
    auto writes_outside = [&](const char* key) {
        if (args.contains(key) && args[key].is_string() && !inside(args[key].get<std::string>(), ctx.out_dir))
            b.flag(StepFlag::WritesFile);
    };
    flag_effects(b, tool, args);
    if (one_of(tool, {"window_screenshot", "recording_start"}))
        writes_outside("save_path");
    if (tool == "recording_encode") {
        // Without save_path the video goes beside its source, which it deletes unless keep_raw_video_file.
        writes_outside(args.contains("save_path") ? "save_path" : "path");
        if (!(args.contains("keep_raw_video_file") && args["keep_raw_video_file"].is_boolean() && args["keep_raw_video_file"].get<bool>()))
            writes_outside("path");
    }
    if (tool == "replay_export")
        writes_outside("path");
    if (tool == "gizmo_pick_file" && args.value("action", std::string()) == "svg_save_as")
        writes_outside("path");
    // A rule's path lets a save picker write the file, or with overwrite replace it. Which picker a rule will
    // answer isn't known here, so every path counts.
    if (tool == "dialog_auto_answer" && args.contains("rules") && args["rules"].is_array())
        for (const json& rule : args["rules"])
            if (rule.is_object() && rule.contains("path") && rule["path"].is_string() && !rule["path"].get<std::string>().empty() &&
                !inside(rule["path"].get<std::string>(), ctx.out_dir))
                b.flag(StepFlag::WritesFile);
    if (one_of(tool, {"preset_save", "preset_delete", "calibration_generate_test"}))
        b.flag(StepFlag::ChangesPresets);
    if (one_of(tool, {"project_new", "calibration_generate_test"})) {
        b.flag(StepFlag::ReplacesProject);
        // project_new with no arguments is New Project's: the app asks about the unsaved changes.
        b.replace(tool == "project_new" && person_form(args) ? "app_asked" : "checked");
    }
    // A project replaces the current one as discard_changes says; a G-code to look at, after the app asks about its
    // unsaved changes. project_open refuses any other file, which replaces nothing.
    if (tool == "project_open" && args.contains("path") && args["path"].is_string()) {
        const std::string path = args["path"].get<std::string>();
        if (has_extension(path, {".gcode", ".bgcode"})) {
            b.flag(StepFlag::ReplacesProject);
            b.replace("app_asked");
        } else if (has_extension(path, {".3mf"})) {
            b.flag(StepFlag::ReplacesProject);
            b.replace("checked");
        }
    }
    b.d.answers_dialogs = one_of(tool, {"dialog_auto_answer", "dialog_answer", "widget_click", "keyboard_press"});
    return b.done();
}

json describe_result(const std::string& tool, const json& result)
{
    if (!result.is_object())
        return nullptr;
    auto file_of = [&](const char* key) -> json {
        if (result.contains(key) && result[key].is_string() && !result[key].get<std::string>().empty())
            return {{"kind", "file"}, {"name", file_name(result[key].get<std::string>())}};
        return nullptr;
    };
    if (tool == "project_open" && result.value("opened", std::string()) == "gcode")
        return file_of("path");
    if (one_of(tool, {"project_open", "project_import", "project_new", "calibration_generate_test"}) && result.contains("objects") &&
        result["objects"].is_number_integer())
        return {{"kind", "objects"}, {"count", result["objects"]}};
    if (tool == "plate_slice" && result.contains("plates") && result["plates"].is_array()) {
        int ok = 0, other = 0;
        for (const json& p : result["plates"])
            (p.is_object() && p.value("outcome", std::string()) == "ok" ? ok : other)++;
        return {{"kind", "plates"}, {"ok", ok}, {"other", other}};
    }
    if (one_of(tool, {"project_export", "project_save", "window_screenshot"}))
        return file_of("path");
    if (tool == "gizmo_pick_file")
        return file_of("saved");
    if (tool == "replay_run" && result.contains("passed") && result["passed"].is_boolean()) {
        json out{{"kind", "replay"}, {"passed", result["passed"]}};
        if (result.contains("failed_step"))
            out["failed_step"] = result["failed_step"];
        return out;
    }
    return nullptr;
}

std::vector<std::string> result_files(const json& result)
{
    std::vector<std::string> out;
    if (!result.is_object())
        return out;
    for (const char* key : {"path", "saved"})
        if (result.contains(key) && result[key].is_string() && !result[key].get<std::string>().empty())
            out.push_back(result[key].get<std::string>());
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
