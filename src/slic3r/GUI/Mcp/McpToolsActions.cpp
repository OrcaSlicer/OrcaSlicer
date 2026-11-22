// action_list and action_run: every Speed Dial action - built-in commands, plates, recent projects,
// plugin scripts and pages, and (on request) settings - is listed and run through these two tools.

#include "slic3r/GUI/I18N.hpp"
#include "McpArguments.hpp"
#include "McpPersonContext.hpp"
#include "McpTools.hpp"
#include "slic3r/GUI/ActionIds.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/ExportOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

ActionRegistry& registry_or_fail()
{
    ActionRegistry* reg = wxGetApp().m_action_registry.get();
    if (reg == nullptr)
        throw ToolFailure("The action catalog is not available yet; app_wait {for:\"ready\"} first");
    return *reg;
}

// The name inside a "<source>:<name>:<plugin>" id (view_front in orca_command:view_front:orca), which
// is what agents and the tools' own messages call an action.
std::string short_id(const std::string& id)
{
    const std::size_t first = id.find(':'), last = id.rfind(':');
    return first == std::string::npos || first == last ? id : id.substr(first + 1, last - first - 1);
}

// The action `id` names: its full id, or a name only one action has, among the actions of the app's state now
// (find_live(): the call before may have opened a sketch, added a plate or switched the tab). Otherwise
// ToolFailure with the ids closest to it, scored against both forms, a name containing it (delete_all in
// obj_delete_all) counting as closest.
const AppAction& find_action(ActionRegistry& actions, std::string& id)
{
    if (const AppAction* action = actions.find_live(id))
        return *action;
    ActionRegistry::Query q;
    q.limit = 100000;
    std::vector<std::string> named;
    std::vector<std::pair<std::size_t, std::string>> scored;
    const json all = actions.describe(q);
    for (const json& a : all["items"]) {
        const std::string candidate = a["id"];
        const std::string name      = short_id(candidate);
        if (name == id)
            named.push_back(candidate);
        const bool contains = id.size() >= 3 && name.find(id) != std::string::npos;
        scored.emplace_back(contains ? 0 : std::min(edit_distance(id, candidate), edit_distance(id, name)), candidate);
    }
    if (named.size() == 1) {
        id = named.front();
        return *actions.by_id(id);
    }
    std::sort(scored.begin(), scored.end());
    json similar = json::array();
    for (std::size_t i = 0; i < scored.size() && similar.size() < 3 && scored[i].first <= id.size() / 3 + 2; ++i)
        similar.push_back(scored[i].second);
#ifdef SLIC3R_CAD
    const bool cad_on = wxGetApp().is_enable_cad_feature();
#else
    const bool cad_on = false;
#endif
    ToolFailure refused = unknown_action(id, NativeCommands::instance(), cad_on);
    refused.details["similar"] = std::move(similar);
    throw refused;
}

} // namespace

std::string action_args_error(const AppAction& action, const json& args)
{
    // An action that collects a value takes that value alone, as the Speed Dial's second phase gives it.
    if (!action.input.empty())
        return args.is_object() && args.size() == 1 && args.contains(action.input) ?
                   std::string() :
                   "The action " + action.id() + " takes exactly its " + action.input + ": run it with args {\"" + action.input +
                       "\": ...} (its input_schema)";
    if (person_form(args))
        return {};
    // A command's action runs its command with the action's arguments, which it takes as well as none; other arguments
    // are the command's to take.
    if (!action.preset.is_null()) {
        if (args == action.preset)
            return {};
        if (action.preset.empty())
            return "The action " + action.id() + " takes no arguments: run it without args, or call " + action.command +
                   " with them";
        return "The action " + action.id() + " runs " + action.command + " with " + action.preset.dump() +
               ": run it without args or with exactly those, or call " + action.command + " with other arguments";
    }
    // A Design command's own action takes the command's arguments, which the command checks.
    if (!action.command.empty())
        return {};
    return "The action " + action.id() + " takes no arguments: run it without args";
}

json action_journal_args(const AppAction& action, const json& args)
{
    if (action.preset.is_null())
        return nullptr;
    return action.input.empty() ? action.preset : args;
}

ToolFailure unknown_action(const std::string& id, const CommandCatalog& catalog, bool cad_on)
{
    // A Design tab action is in the registry only while it is live (ActionRegistry::materialize_design_actions()), which
    // none is while the CAD feature is off; its command is in the catalog, unless the catalog was built without the
    // Design commands. Its id is right then, and the code says what keeps it from running.
    const std::string action_id = id.find(':') != std::string::npos ? id : AppAction::compose_id(kCommandPrefix, id, kOrcaSourceKey);
    if (is_design_action_id(action_id)) {
        const std::string key = short_id(action_id);
        if (const std::string* why = catalog.left_out(key))
            return ToolFailure(id + " is a Design tab action, and " + *why, {{"code", catalog.unknown_command_code(key)}});
        if (!cad_on)
            return ToolFailure(id + " is a Design tab action, and the CAD feature is off: a person enables it in Preferences > "
                                    "Features and restarts OrcaSlicer",
                               {{"code", "design_unavailable"}});
        // A folded item's action is a call of the command it is folded into with the item's arguments, written as the
        // rest of the call is, a space after each colon and comma.
        const auto [command, folded] = find_command_action(catalog, short_id(action_id));
        std::string call = "{\"tool\": \"" + (command != nullptr ? command->key : short_id(action_id)) + "\"";
        if (folded != nullptr && !folded->at("args").empty()) {
            std::string args;
            for (const auto& [name, value] : folded->at("args").items())
                args += (args.empty() ? "" : ", ") + json(name).dump() + ": " + value.dump();
            call += ", \"args\": {" + args + "}";
        }
        return ToolFailure(id + " is a Design tab action, there only while the Design tab is shown in its mode (a sketch's or "
                                "the model's) with the CAD feature on; tool_call " + call + "} runs its command from any tab",
                           {{"code", "wrong_mode"}});
    }
    // The name may be a command's that has no action, which tool_call runs.
    return ToolFailure("Unknown action id " + id + "; find an action with action_list, or a command in tools/list or with tool_list",
                       {{"code", "invalid_argument"}});
}

ToolResult await_gcode_export(ToolContext& ctx, const Ops::ExportTicket& ticket)
{
    json done = poll_ui(
        ctx,
        [&ticket]() -> json {
            ui_plater();
            std::string             error;
            const Ops::ExportStatus status = Ops::gcode_export_state(ticket, &error);
            if (status == Ops::ExportStatus::Pending)
                return json();
            json out{{"export", Ops::to_string(status)}, {"path", ticket.path}};
            if (!error.empty())
                out["error"] = error;
            return out;
        },
        std::chrono::minutes(10), "the export to finish");
    const std::string status = done["export"];
    if (status == "ok")
        return ToolResult::ok(done);
    if (status == "superseded") {
        done["note"] = "The project changed while exporting; the file reflects the project as it was when the export started.";
        return ToolResult::ok(done);
    }
    return ToolResult::error("The export ended " + status, done);
}

namespace {

// What `actions` lists for `q`. A section no action is in now is a name to correct, refused with the sections there
// are, as tool_list refuses a group. Recent Projects and Design hold actions only while there are recent projects
// and while the Design tab is shown, so either one empty is an answer, not a wrong name.
json described(ActionRegistry& actions, const ActionRegistry::Query& q)
{
    json listed = actions.describe(q);
    if (q.section.empty() || listed.value("total", 0) != 0)
        return listed;
    for (const char* comes_and_goes : {"Recent Projects", kDesignGroup})
        if (boost::algorithm::iequals(q.section, comes_and_goes))
            return listed;
    ActionRegistry::Query every;
    every.include_settings = q.include_settings;
    every.limit            = 100000;
    const json  all      = actions.describe(every);
    json        sections = json::array();
    std::string names;
    for (const json& item : all["items"])
        if (std::find(sections.begin(), sections.end(), item["section"]) == sections.end()) {
            sections.push_back(item["section"]);
            names += (names.empty() ? "" : ", ") + item["section"].get<std::string>();
        }
    throw ToolFailure("Unknown section: " + q.section + "; the Speed Dial's sections now are " + names,
                      {{"code", "invalid_argument"}, {"sections", std::move(sections)}});
}

} // namespace

void register_action_tools(CommandCatalog& catalog)
{
    catalog.add(inspect_tool(
        "action_list", L("List Speed Dial actions"),
        "List everything the Speed Dial can run, by section and title: built-in commands (slice, export, views, object "
        "operations, plates, calibration wizards, help), recent projects, plugin scripts and pages. Each entry has an `id` for "
        "`action_run`, its `section` (where the Speed Dial lists it), a `description`, the `command` of a built-in "
        "command, and, when it takes arguments, their `input_schema`. `section` keeps them to one of the Speed Dial's sections, which are not "
        "`tool_list`'s groups; one no action is in is refused with the sections there are, but Recent Projects and Design "
        "list nothing while there are no recent projects or the Design tab is not shown. `include_settings` also lists "
        "settings (each jumps to that setting in the sidebar; use `settings_get` / `settings_set` to read or change values).",
        object_schema({{"section", prop("string", "Only this Speed Dial section, in any case, as an entry's `section` names it, "
                                                  "e.g. \"Object\", \"View\", \"Slice & Export\".")},
                       {"include_settings", prop("boolean", "Also list settings (thousands). Default false.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Default 50."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            ActionRegistry::Query q;
            q.section          = arg_string(args, "section");
            q.include_settings = arg_bool(args, "include_settings", false);
            q.limit            = std::size_t(arg_int(args, "limit", 50));
            q.offset           = std::size_t(arg_int(args, "offset", 0));
            return ToolResult::ok(ctx.run_ui([q] { return described(registry_or_fail(), q); }));
        }));

    NativeCommand run = mutate_tool(
        "action_run", L("Run a Speed Dial action"),
        "Run one action by the id `action_list` returned (`orca_command:view_front:orca`, or the name inside it, `view_front`), "
        "with `args`, the command's arguments, when it takes some (its `input_schema`). A command's action (`action_list` names "
        "the command) runs the command as a call with the action's arguments does: an action that asks in a dialog asks, "
        "and the call returns `blocked_by_dialog`; answer it with `dialog_answer`, or, for a native one it can't press (a file "
        "picker), register `dialog_auto_answer` before the call. Such an action takes no `args`, or exactly its own; to run a "
        "command without asking, call it with its arguments (an absolute path skips a file dialog). Go to layer and Go to "
        "tab take their value alone (`{\"percent\": 50}`, `{\"tab\": \"preview\"}`), and a Design command's own action takes "
        "the command's arguments. An action that starts a background job (`obj_arrange`, `obj_orient`) completes when the job "
        "ends. An action that does nothing (no object selected) is an error. Some commands add what they did to the reply "
        "(the order `object_order` leaves, the object `object_assemble` made). The other actions (plates, recent projects, "
        "plugin scripts and pages, settings) run only here.",
        object_schema({{"id", prop("string", "Action id from `action_list`, or the name inside it.")},
                       {"args", {{"type", "object"}, {"description", "The command's arguments, as its `input_schema` gives them."}}}},
                      {"id"}),
        [](const json& args, ToolContext& ctx) {
            std::string        id           = arg_string(args, "id");
            json               command_args = args.contains("args") ? args["args"] : json::object();
            std::string        key;         // the command of a command's action
            json               action_args; // what its journal step keeps for an action of fixed arguments
            AppActionRunResult result;      // of any other action
            ctx.run_ui([&]() -> json {
                ActionRegistry&  actions = registry_or_fail();
                const AppAction& action  = find_action(actions, id);
                if (const std::string err = action_args_error(action, command_args); !err.empty())
                    throw ToolFailure(err, {{"code", "invalid_argument"}});
                key         = action.command;
                action_args = action_journal_args(action, command_args);
                if (key.empty()) {
                    result = actions.run(id, command_args, /*record_stats=*/false);
                } else {
                    // What a person's pick of the action runs its command with, which the journal records.
                    const json* command_action = find_command_action(NativeCommands::instance(), short_id(id)).second;
                    command_args               = action_call_args(command_action, command_args);
                }
                return json();
            });
            if (key.empty()) {
                settle(ctx);
                return NativeCommands::from_run_result(id, result);
            }
            // A command's action runs the command's own function, as a call of the command does, inside action_run's
            // journal entry.
            const NativeCommand* command = NativeCommands::find(key);
            if (command == nullptr)
                throw ToolFailure("Unknown command " + key);
            if (const std::string err = argument_error(*command, command_args); !err.empty())
                throw argument_refusal(key, err);
            ActionContext action_ctx(ctx, id, action_args);
            ToolResult    reply = command->run(command_args, action_ctx);
            if (!reply.is_error && reply.data.is_object())
                reply.data["ran"] = id;
            return reply;
        });
    // Its actions send G-code to a printer or its print host (printer_send) and sync presets with the cloud account
    // (preset_sync).
    run.open_world = true;
    catalog.add(run);
}

}}} // namespace Slic3r::GUI::Mcp
