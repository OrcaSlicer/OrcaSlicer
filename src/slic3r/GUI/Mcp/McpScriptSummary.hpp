#pragma once

#include "McpScript.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What a script step will do, read from its arguments before it runs, for the script runner dialog:
// which effects beyond the open project it has, what it acts on, and how it treats unsaved changes.
// Everything here is data for a host to word and translate; nothing is user-visible text.

// Effects beyond the open project. Where a step's effect can't be read before the run (a plugin
// action, a nested script too deep or unreadable), it gets every flag.
enum class StepFlag { WritesFile, ChangesPresets, ReplacesProject, SendsToPrinter };

// "writes_file", "changes_presets", "replaces_project", "sends_to_printer".
const char* flag_id(StepFlag flag);

struct StepDescription
{
    std::vector<StepFlag> flags; // no duplicates, in enum order
    // What the step acts on: {"kind":"files","names":[...]} | {"kind":"settings","pairs":[...]}
    // | {"kind":"command","key":k,"param"?:p} (a file param as its file name) | {"kind":"recent_project","name":n}
    // | {"kind":"preset","name":n} | {"kind":"plates","plates":"current"|"all"|index}
    // | {"kind":"script","steps":n} | {"kind":"value","text":t} | null.
    nlohmann::json what;
    bool           checks_result = false; // the step has an expect
    // How a replacing step treats the project's unsaved changes: "" (it doesn't replace the project),
    // "checked" (the tool checks them itself and fails unless allowed to drop them) or "app_asked"
    // (it goes through the app's own save question). A nested script's "checked" wins.
    std::string replace;
    // The step can answer a dialog, the app's save question included (here or in a nested script).
    bool answers_dialogs = false;
};

// Describes `step` of `script`, substituting its arguments leniently over `ctx`; a file counts as
// written inside the output folder when it is inside ctx.out_dir. A run_script step is described by
// the steps of its inline script, or of its file when `path` reads and parses, each in the nested
// run's own context (its vars merged with the step's `vars`, its own script and output folders);
// past `depth` 4, or when the file doesn't read or parse, it gets every flag, "checked" and
// answers_dialogs.
StepDescription describe_step(const ScriptStep& step, const Script& script, const ScriptContext& ctx, int depth = 0);

// What a call of `tool` with `args` acts on, as describe_step()'s `what` says it, Speed Dial ids
// included, but from the arguments as they are and without reading files: a run_script call is named
// by its script file. For recording captions.
nlohmann::json describe_call(const std::string& tool, const nlohmann::json& args);

// What a finished step returned, from the tools' real replies: {"kind":"objects","count":n}
// | {"kind":"plates","ok":n,"other":m} | {"kind":"file","name":n}
// | {"kind":"script","passed":b,"failed_step"?:n} | null.
nlohmann::json describe_result(const std::string& tool, const nlohmann::json& result);

// The files a step's result names: its "path" and "saved" fields when they are non-empty strings.
std::vector<std::string> result_files(const nlohmann::json& result);

}}} // namespace Slic3r::GUI::Mcp
