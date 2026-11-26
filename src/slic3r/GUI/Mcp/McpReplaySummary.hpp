#pragma once

#include "McpReplay.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What a replay step will do, read from its arguments before it runs, for the Action Replay window:
// which effects beyond the open project it has, what it acts on, and how it treats unsaved changes.
// Everything here is data for a host to word and translate; nothing is user-visible text.

// Effects beyond the open project. Where a step's effect can't be read before the run (a plugin
// action, a nested replay too deep or unreadable), it gets every flag. RunsProgram is a step that can
// start a program on the computer: a script_run, whose Python code has the app's access to it, and a
// settings_set that sets a post-processing script, which the next G-code export runs.
enum class StepFlag { WritesFile, ChangesPresets, ReplacesProject, SendsToPrinter, RunsProgram };

// "writes_file", "changes_presets", "replaces_project", "sends_to_printer", "runs_program".
const char* flag_id(StepFlag flag);

struct StepDescription
{
    std::vector<StepFlag> flags; // no duplicates, in enum order
    // What the step acts on: {"kind":"files","names":[...]} | {"kind":"settings","pairs":[...]}
    // | {"kind":"command","key":k,"args"?:["name: value", ...]} (a file by its name), or with "action":a in place of
    // "args" for a call with the arguments of k's action a (NativeCommand::actions) | {"kind":"recent_project","name":n}
    // | {"kind":"preset","name":n} | {"kind":"plates","plates":"current"|"all"|index}
    // | {"kind":"replay","steps":n} | {"kind":"value","text":t} | null.
    nlohmann::json what;
    bool           checks_result = false; // the step has an expect
    // How a replacing step treats the project's unsaved changes: "" (it doesn't replace the project),
    // "checked" (the tool checks them itself and fails unless allowed to drop them) or "app_asked"
    // (it goes through the app's own save question). A nested replay's "checked" wins.
    std::string replace;
    // The step can answer a dialog, the app's save question included (here or in a nested replay).
    bool answers_dialogs = false;
};

// Describes `step` of `replay`, substituting its arguments leniently over `ctx`; a file counts as
// written inside the output folder when it is inside ctx.out_dir. A step that calls a command of `catalog` as one
// of its actions (with exactly an action's arguments, or with none where it has an action of its own, or, for one the
// tool list does not show, a Design command, with any) is described as action_run of that action is. A step's flags
// come from the command it runs and its arguments, whether it calls the command or action_run of its action (an export
// flags writes_file either way). A tool_call step is described as the call it makes. A replay_run step is described by
// the steps of its inline replay, or of its file when `path` reads and parses, each in the nested
// run's own context (its vars merged with the step's `vars`, its own replay and output folders);
// past `depth` 4, or when the file doesn't read or parse, it gets every flag, "checked" and
// answers_dialogs.
StepDescription describe_step(const CommandCatalog& catalog, const ReplayStep& step, const Replay& replay,
                              const ReplayContext& ctx, int depth = 0);

// What a call of `tool` with `args` acts on, as describe_step()'s `what` says it over `catalog`, Speed Dial
// ids included, but from the arguments as they are and without reading files: a replay_run call is named
// by its replay file. For recording captions.
nlohmann::json describe_call(const CommandCatalog& catalog, const std::string& tool, const nlohmann::json& args);

// What a finished step returned, from the tools' real replies: {"kind":"objects","count":n}
// | {"kind":"plates","ok":n,"other":m} | {"kind":"file","name":n}
// | {"kind":"replay","passed":b,"failed_step"?:n} | null.
nlohmann::json describe_result(const std::string& tool, const nlohmann::json& result);

// The files a step's result names: its "path" and "saved" fields when they are non-empty strings.
std::vector<std::string> result_files(const nlohmann::json& result);

}}} // namespace Slic3r::GUI::Mcp
