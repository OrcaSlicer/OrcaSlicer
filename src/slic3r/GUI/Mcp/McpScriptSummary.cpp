#include "McpScriptSummary.hpp"
#include "McpPaths.hpp"
#include "McpScript.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <array>
#include <cstddef>
#include <exception>
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

// Nested scripts are read this deep; a deeper one counts as unreadable.
constexpr int kMaxDepth = 4;

constexpr std::size_t kFlagCount = 4;

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

// A Speed Dial param that names a file, in either of the forms path params take.
std::optional<std::string> param_file(const std::string& param)
{
    std::string err;
    try {
        if (const auto p = parse_path_param(param, err); p && path_from_utf8(p->path).is_absolute())
            return p->path;
    } catch (const std::exception&) {
        // A malformed {"overwrite": ...}; the param is then not read as a file.
    }
    return std::nullopt;
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool one_of(const std::string& s, std::initializer_list<const char*> names)
{
    for (const char* n : names)
        if (s == n)
            return true;
    return false;
}

void describe_command(Builder& b, const std::string& key, const std::string& param)
{
    const std::optional<std::string> file = param.empty() ? std::nullopt : param_file(param);
    json                             what{{"kind", "command"}, {"key", key}};
    if (!param.empty())
        what["param"] = file ? file_name(*file) : param;
    b.d.what = std::move(what);

    if (starts_with(key, "calib_")) {
        b.flag(StepFlag::ReplacesProject);
        b.flag(StepFlag::ChangesPresets);
        b.replace("app_asked");
    }
    if (one_of(key, {"load_project", "new_project"}) ||
        (key == "import_file" && file && has_extension(*file, {".3mf", ".gcode", ".bgcode"}))) {
        b.flag(StepFlag::ReplacesProject);
        b.replace("app_asked");
    }
    if (one_of(key, {"save_project", "save_project_as", "export_config"}) || starts_with(key, "export_"))
        b.flag(StepFlag::WritesFile);
    if (one_of(key, {"import_configs", "preset_bundle", "sync_presets"}))
        b.flag(StepFlag::ChangesPresets);
    if (key == "send_gcode")
        b.flag(StepFlag::SendsToPrinter);
}

void describe_action(Builder& b, const json& args)
{
    const std::string id                = args.contains("id") && args["id"].is_string() ? args["id"].get<std::string>() : std::string();
    const std::string param             = args.contains("param") && args["param"].is_string() ? args["param"].get<std::string>() : std::string();
    const auto [prefix, middle]         = split_action_id(id);
    if (prefix == "orca_command") {
        describe_command(b, middle, param);
    } else if (prefix == "orca_recent_project") {
        b.d.what = {{"kind", "recent_project"}, {"name", file_name(middle)}};
        b.flag(StepFlag::ReplacesProject);
        b.replace("app_asked");
    } else if (prefix == "orca_plate_goto" || prefix == "orca_setting") {
        // A plate to show, or a setting to show in the sidebar: they change nothing.
        b.d.what = {{"kind", "value"}, {"text", middle}};
    } else {
        // Plugin scripts and pages, or an id this list doesn't know: code whose effect can't be read.
        // Plugin code doesn't read the runner's permission to drop unsaved changes.
        b.d.what = {{"kind", "value"}, {"text", id}};
        b.every_flag("app_asked");
    }
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

// A run_script step: the nested script, read in the context its run will have.
void describe_nested(Builder& b, const json& args, const ScriptContext& outer, int depth)
{
    std::optional<Script> nested;
    ScriptContext         ctx;
    std::string           err;
    if (args.contains("script") && args["script"].is_object()) {
        nested = parse_script(args["script"].dump(), err);
        // An inline script is part of the step's args, which the outer script substitutes before the
        // nested run starts: its placeholders are the outer script's, already applied to `args`, and
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
            nested = parse_script(*text, err);
        if (!nested)
            b.d.what = {{"kind", "files"}, {"names", {file_name(path_to_utf8(path))}}};
        ctx.script_dir = path.parent_path();
        if (args.contains("out_dir") && args["out_dir"].is_string())
            ctx.out_dir = path_from_utf8(args["out_dir"].get<std::string>());
        else {
            ctx.out_dir = path.parent_path() / path.stem();
            ctx.out_dir += ".out";
        }
        if (nested && args.contains("vars") && args["vars"].is_object())
            for (const auto& [key, value] : args["vars"].items())
                nested->vars[key] = value;
    }
    if (nested)
        b.d.what = {{"kind", "script"}, {"steps", nested->steps.size()}};
    if (!nested || depth >= kMaxDepth) {
        b.every_flag("checked");
        return;
    }
    for (const ScriptStep& step : nested->steps)
        b.join(describe_step(step, *nested, ctx, depth + 1));
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
    if (one_of(tool, {"select_preset", "save_preset", "delete_preset"}) && args.contains("name") && args["name"].is_string())
        return {{"kind", "preset"}, {"name", args["name"]}};
    if (one_of(tool, {"slice", "arrange"}))
        return {{"kind", "plates"}, {"plates", args.contains("plates") ? args["plates"] : json(tool == "slice" ? "current" : "all")}};
    if (tool == "wait")
        return text("for");
    if (tool == "dialog_respond")
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

const char* flag_id(StepFlag flag)
{
    switch (flag) {
    case StepFlag::WritesFile: return "writes_file";
    case StepFlag::ChangesPresets: return "changes_presets";
    case StepFlag::ReplacesProject: return "replaces_project";
    case StepFlag::SendsToPrinter: return "sends_to_printer";
    }
    return "";
}

StepDescription describe_step(const ScriptStep& step, const Script& script, const ScriptContext& ctx, int depth)
{
    const json        args = substitute_lenient(step.args, script, ctx);
    const std::string tool = step.tool;
    Builder           b;
    b.d.checks_result = !step.expect.empty();

    if (tool == "run_action") {
        describe_action(b, args);
        return b.done();
    }
    if (tool == "run_script") {
        describe_nested(b, args, ctx, depth);
        return b.done();
    }
    b.d.what = describe_what(tool, args);

    // A file written into the run's own output folder is the run's output, not an effect to flag.
    auto writes_outside = [&](const char* key) {
        if (args.contains(key) && args[key].is_string() && !inside(args[key].get<std::string>(), ctx.out_dir))
            b.flag(StepFlag::WritesFile);
    };
    if (one_of(tool, {"export", "save_project"}))
        b.flag(StepFlag::WritesFile);
    if (tool == "screenshot")
        writes_outside("save_path");
    if (tool == "export_script")
        writes_outside("path");
    if (tool == "gizmo_file" && args.value("action", std::string()) == "svg_save_as")
        writes_outside("path");
    if (one_of(tool, {"save_preset", "delete_preset", "calibrate"}))
        b.flag(StepFlag::ChangesPresets);
    if (one_of(tool, {"new_project", "calibrate"})) {
        b.flag(StepFlag::ReplacesProject);
        b.replace("checked");
    }
    if (tool == "open_files" && args.contains("paths") && args["paths"].is_array() && args["paths"].size() == 1 &&
        args["paths"][0].is_string()) {
        const std::string path = args["paths"][0].get<std::string>();
        if (has_extension(path, {".gcode", ".bgcode"})) {
            b.flag(StepFlag::ReplacesProject);
            b.replace("app_asked");
        } else if (has_extension(path, {".3mf"}) && args.value("as", std::string("auto")) != "geometry") {
            b.flag(StepFlag::ReplacesProject);
            b.replace("checked");
        }
    }
    b.d.answers_dialogs = one_of(tool, {"dialog_auto_answer", "dialog_respond", "ui_click", "ui_key"});
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
    if (tool == "open_files" && result.value("opened", std::string()) == "gcode")
        return file_of("path");
    if (one_of(tool, {"open_files", "new_project", "calibrate"}) && result.contains("objects") && result["objects"].is_number_integer())
        return {{"kind", "objects"}, {"count", result["objects"]}};
    if (tool == "slice" && result.contains("plates") && result["plates"].is_array()) {
        int ok = 0, other = 0;
        for (const json& p : result["plates"])
            (p.is_object() && p.value("outcome", std::string()) == "ok" ? ok : other)++;
        return {{"kind", "plates"}, {"ok", ok}, {"other", other}};
    }
    if (one_of(tool, {"export", "save_project", "screenshot"}))
        return file_of("path");
    if (tool == "gizmo_file")
        return file_of("saved");
    if (tool == "run_script" && result.contains("passed") && result["passed"].is_boolean()) {
        json out{{"kind", "script"}, {"passed", result["passed"]}};
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
