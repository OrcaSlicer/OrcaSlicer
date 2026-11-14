// search_actions and run_action: every Speed Dial action - built-in commands, plates, recent projects,
// plugin scripts and pages, and (on request) settings - is reachable through these two tools.

#include "slic3r/GUI/I18N.hpp"
#include "McpPersonContext.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/ExportOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"

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
        throw ToolFailure("The action catalog is not available yet; wait {for:\"ready\"} first");
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
    throw ToolFailure(unknown_action(id, NativeCommands::instance()), {{"similar", similar}});
}

} // namespace

std::string action_args_error(const AppAction& action, const json& args)
{
    if (!action.command.empty() || person_form(args))
        return {};
    return "The action " + action.id() + " takes no arguments: run it without args";
}

std::string unknown_action(const std::string& id, const CommandCatalog& catalog)
{
    // A Design tile is no action while it is not live (ActionRegistry::materialize_design_actions()); its
    // command is in the catalog, unless the catalog was built with the CAD feature off.
    const std::string tile = id.find(':') != std::string::npos ? id : AppAction::compose_id(kCommandPrefix, id, kOrcaSourceKey);
    if (is_design_tile_id(tile)) {
        if (const std::string* why = catalog.left_out(short_id(tile)))
            return id + " is a Design tab tile, and " + *why;
        return id + " is a Design tab tile, there only while the Design tab is shown in its mode (a sketch's or the model's) "
                    "with the CAD feature on; call_tool {\"tool\": \"" + short_id(tile) + "\"} runs its command from any tab";
    }
    // The name may be a command's that has no tile, which call_tool runs.
    return "Unknown action id " + id + "; find an action with search_actions, or a command with search_tools";
}

json rank_actions(const json& listed, const std::vector<std::string>& words)
{
    struct Hit
    {
        std::pair<int, int> score;
        std::size_t         name_size;
        const json*         action;
    };
    std::vector<Hit> hits;
    for (const json& action : listed) {
        // The name inside the id, without the source and plugin every built-in id shares: "orca" and
        // "command" would match them all. The group is part of what the action is called, as the
        // palette shows one by group and title: Delete Selected is the Object group's delete, though
        // its own title names no object.
        const std::string         name  = short_id(action.value("id", std::string()));
        const std::pair<int, int> score = match_score(words,
                                                      name + ' ' + action.value("title", std::string()) + ' ' +
                                                          action.value("group", std::string()),
                                                      action.value("description", std::string()));
        if (score.first > 0)
            hits.push_back({score, name.size(), &action});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& a, const Hit& b) { return a.score != b.score ? a.score > b.score : a.name_size < b.name_size; });
    json ranked = json::array();
    for (const Hit& hit : hits)
        ranked.push_back(*hit.action);
    return ranked;
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

void register_action_tools(CommandCatalog& catalog)
{
    catalog.add(inspect_tool(
        "ui", "search_actions", L("Search Speed Dial actions"),
        "Search everything the Speed Dial can run: built-in commands (slice, export, views, object operations, plates, "
        "calibration wizards, help), recent projects, plugin scripts and pages. Each entry has an id for run_action, a "
        "description, the command's key for a built-in command, and, when it takes arguments, their input_schema. A query "
        "ranks them, best match first, as "
        "search_tools does; without one they are listed by group and title. include_settings also lists settings (each "
        "jumps to that setting in the sidebar; use get_settings / set_settings to read or change values).",
        object_schema({{"query", prop("string", "A few words, e.g. \"delete object\": each is matched at the start of the words of "
                                                "an action's id, title, group and description.")},
                       {"group", prop("string", "Only this group, e.g. \"Object\", \"View\", \"Slice & Export\".")},
                       {"include_settings", prop("boolean", "Also list settings (thousands). Default false.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Default 50."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            ActionRegistry::Query q;
            q.group            = arg_string(args, "group");
            q.include_settings = arg_bool(args, "include_settings", false);
            const std::string query = arg_string(args, "query");
            if (query.empty()) {
                q.limit  = std::size_t(arg_int(args, "limit", 50));
                q.offset = std::size_t(arg_int(args, "offset", 0));
                return ToolResult::ok(ctx.run_ui([q] { return registry_or_fail().describe(q); }));
            }
            // The registry's own search wants the query as one text; an agent's is a few words.
            q.limit           = 100000;
            const json listed = ctx.run_ui([q] { return registry_or_fail().describe(q); });
            return ToolResult::ok(page(rank_actions(listed["items"], query_words(query)), args, 50));
        }));

    NativeCommand run = mutate_tool(
        "ui", "run_action", L("Run a Speed Dial action"),
        "Run one action by the id search_actions returned (orca_command:view_front:orca, or the name inside it, view_front), "
        "with args, the command's arguments, when it takes some (its input_schema). A command that asks in a dialog asks "
        "when it is given no args, as a person's pick does, and runs without asking when given them: an absolute path "
        "skips a file dialog, and an output needs {\"path\": ..., \"overwrite\": true} to replace a file. Go to layer and "
        "Go to tab need their value ({\"percent\": 50}, {\"tab\": \"preview\"}). Actions that open a dialog return "
        "blocked_by_dialog; answer it with answer_dialog, or, for a native one it can't press (a file picker), register "
        "auto_answer_dialogs before the call. An export_gcode with a path completes when the file is written; an action "
        "that starts a background job (obj_arrange, obj_orient) when the job ends. An action that does nothing (no object "
        "selected) is an error. Some commands add what they did to the reply (the obj_order_by_* order, the object "
        "obj_assemble made).",
        object_schema({{"id", prop("string", "Action id from search_actions, or the name inside it.")},
                       {"args", {{"type", "object"}, {"description", "The command's arguments, as its input_schema gives them."}}}},
                      {"id"}),
        [](const json& args, ToolContext& ctx) {
            std::string        id           = arg_string(args, "id");
            json               command_args = args.contains("args") ? args["args"] : json::object();
            std::string        key;    // the command of a command's tile
            AppActionRunResult result; // of any other action
            ctx.run_ui([&]() -> json {
                ActionRegistry&  actions = registry_or_fail();
                const AppAction& action  = find_action(actions, id);
                if (const std::string err = action_args_error(action, command_args); !err.empty())
                    throw ToolFailure(err);
                key = action.command;
                if (key.empty())
                    result = actions.run(id, command_args, /*record_stats=*/false);
                return json();
            });
            if (key.empty()) {
                settle(ctx);
                return NativeCommands::from_run_result(id, result);
            }
            // A command's tile runs the command's own function, as a call of the command does, inside run_action's
            // journal entry.
            const NativeCommand* command = NativeCommands::find(key);
            if (command == nullptr)
                throw ToolFailure("Unknown command " + key);
            if (const std::string err = argument_error(*command, command_args); !err.empty())
                throw ToolFailure("Invalid arguments for " + key + ": " + err);
            TileContext tile(ctx, id);
            ToolResult  reply = command->run(command_args, tile);
            if (!reply.is_error && reply.data.is_object())
                reply.data["ran"] = id;
            return reply;
        });
    // Its commands upload G-code to a print host (send_gcode) and sync presets with the cloud account (sync_presets).
    run.open_world = true;
    catalog.add(run);
}

}}} // namespace Slic3r::GUI::Mcp
