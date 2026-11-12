// search_actions and run_action: every Speed Dial action - built-in commands, plates, recent projects,
// plugin scripts and pages, and (on request) settings - is reachable through these two tools.

#include "slic3r/GUI/I18N.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#ifdef SLIC3R_CAD
#include "slic3r/GUI/Mcp/McpToolsCad.hpp"
#endif
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/ExportOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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

std::size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diag = row[0];
        row[0]           = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t up = row[j];
            row[j]               = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag                 = up;
        }
    }
    return row[b.size()];
}

// The name inside a "<source>:<name>:<plugin>" id (view_front in orca_command:view_front:orca), which
// is what agents and the tools' own messages call an action.
std::string short_id(const std::string& id)
{
    const std::size_t first = id.find(':'), last = id.rfind(':');
    return first == std::string::npos || first == last ? id : id.substr(first + 1, last - first - 1);
}

// The action `id` names: its full id, or a name only one action has. Otherwise ToolFailure: for a name
// that is a tool of `tools` or a CAD tool, where it runs; else the ids closest to it, scored against
// both forms, a name containing it (delete_all in obj_delete_all) counting as closest.
const AppAction& find_action(ActionRegistry& actions, std::string& id, const ToolRegistry& tools)
{
    if (const AppAction* action = actions.by_id(id))
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
    if (const std::string elsewhere = other_catalog_hint(tools, id, Catalog::Actions); !elsewhere.empty())
        throw ToolFailure("Unknown action id " + id + "; " + elsewhere);
    std::sort(scored.begin(), scored.end());
    json similar = json::array();
    for (std::size_t i = 0; i < scored.size() && similar.size() < 3 && scored[i].first <= id.size() / 3 + 2; ++i)
        similar.push_back(scored[i].second);
    throw ToolFailure("Unknown action id " + id + "; find it with search_actions", {{"similar", similar}});
}

const char* level_name(AppActionRunResult::Level level)
{
    switch (level) {
    case AppActionRunResult::Level::Success: return "success";
    case AppActionRunResult::Level::Error: return "error";
    case AppActionRunResult::Level::Busy: return "busy";
    default: return "info";
    }
}

} // namespace

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

std::string other_catalog_hint(const ToolRegistry& registry, const std::string& name, Catalog missed)
{
    if (name.empty())
        return {};
    if (missed != Catalog::Tools && registry.lookup(name) != nullptr)
        return "it is a tool of its own: call " + name + " directly";
#ifdef SLIC3R_CAD
    if (missed != Catalog::Cad) {
        const std::vector<std::string> cad = cad_tool_names();
        if (std::find(cad.begin(), cad.end(), name) != cad.end())
            return "it is a CAD tool: call_cad_tool {tool: \"" + name + "\", args} runs it, and describe_cad_tool {tool} gives its arguments";
    }
#endif
    // The built-in commands only: the registry's other actions (plates, recent projects, plugin
    // scripts) are read on the UI thread, and their ids are not names an agent mistakes for a tool's.
    if (missed != Catalog::Actions) {
        const std::string key = short_id(name);
        if (NativeCommands::find(key) != nullptr && (name == key || name == "orca_command:" + key + ":orca"))
            return "it is a Speed Dial action: run_action {id: \"" + name + "\"} runs it";
    }
    return {};
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

void register_action_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(inspect_tool(
        "search_actions", L("Search Speed Dial actions"),
        "Search everything the Speed Dial can run: built-in commands (slice, export, views, object operations, plates, "
        "calibration wizards, help), recent projects, plugin scripts and pages. Each entry has an id for run_action, a "
        "description and, when it takes one, a param (type and help). A query ranks them, best match first, as "
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

    ToolDef run = mutate_tool(
        "run_action", L("Run a Speed Dial action"),
        "Run one action by the id search_actions returned (orca_command:view_front:orca, or the name inside it, view_front), "
        "with its param when it takes one (a percent, tab id, plate index, "
        "or an absolute path - commands with a path parameter skip their file dialog; outputs need {\"path\": ..., "
        "\"overwrite\": true} to replace a file). Actions that open a dialog return blocked_by_dialog; answer it with "
        "answer_dialog, or, for a native one it can't press (a file picker), register auto_answer_dialogs before the call. "
        "An export_gcode with a path "
        "completes when the file is written; an action that starts a background job (obj_arrange, obj_orient) when the "
        "job ends. An action that does nothing (no object selected) is an error. Some commands add what they did to the "
        "reply (the obj_order_by_* order, the object obj_assemble made).",
        object_schema({{"id", prop("string", "Action id from search_actions, or the name inside it.")},
                       {"param", prop("string", "Parameter, when the action takes one.")}},
                      {"id"}),
        [&service](const json& args, ToolContext& ctx) {
            std::string       id    = arg_string(args, "id");
            const std::string param = arg_string(args, "param");
            Ops::ExportTicket ticket; // of an action that completes in the background
            json started = ctx.run_ui([&]() -> json {
                ActionRegistry&  actions = registry_or_fail();
                const AppAction& action  = find_action(actions, id, service.registry());
                // Without a path the command would ask for one in a native file dialog, which MCP
                // can't fill in, and export behind it untracked.
                if (action.completes_in_background && param.empty())
                    throw ToolFailure("This action needs a path param here: " + action.param_help +
                                      " (or use the export tool)");
                const bool background = action.completes_in_background;
                if (background)
                    ctx.begin_operation(); // held from before the export starts
                Plater*    plater     = wxGetApp().plater();
                uint64_t   before = 0, revision = 0;
                if (background && plater != nullptr) {
                    before   = plater->export_seq();
                    revision = plater->project_revision();
                }
                const AppActionRunResult r = actions.run(id, param, /*record_stats=*/false);
                json out{{"level", level_name(r.level)}, {"message", into_u8(r.message)}, {"done", r.done}, {"background", background}};
                // An operation's refusal is worded once, in English here as in a tool's reply.
                if (r.refusal) {
                    out["message"] = r.refusal->english();
                    out["refusal"] = {{"code", r.refusal->code}};
                    if (!r.refusal->names.empty())
                        out["refusal"]["names"] = r.refusal->names;
                }
                if (!r.reply.empty())
                    out["reply"] = json::parse(r.reply, nullptr, /*allow_exceptions=*/false);
                if (background && plater != nullptr) {
                    ticket.seq_before = before;
                    ticket.seq_after  = plater->export_seq();
                    ticket.revision   = revision;
                    if (const auto run = plater->export_run(ticket.seq_after))
                        ticket.path = run->path;
                }
                return out;
            });
            // A command that did nothing (no selection, no objects) fails here, though the palette
            // shows it as information.
            const bool done    = started["done"].get<bool>();
            const json details = started.value("refusal", json());
            if (started["background"].get<bool>()) {
                if (started["level"] == "error" || !done)
                    return ToolResult::error(started["message"].get<std::string>(), details);
                if (ticket.seq_after == ticket.seq_before)
                    return ToolResult::error(started["message"].get<std::string>().empty() ? "The export did not start." :
                                                                                             started["message"].get<std::string>());
                return await_gcode_export(ctx, ticket);
            }
            settle(ctx);
            const std::string level = started["level"];
            if (level == "error" || level == "busy" || !done)
                return ToolResult::error(started["message"].get<std::string>().empty() ? "The action failed" : started["message"].get<std::string>(),
                                         details);
            json out{{"ran", id}, {"level", level}};
            if (!started["message"].get<std::string>().empty())
                out["message"] = started["message"];
            if (started.contains("reply") && started["reply"].is_object())
                out.update(started["reply"]);
            return ToolResult::ok(out);
        });
    // Its commands upload G-code to a print host (send_gcode) and sync presets with the cloud account (sync_presets).
    run.open_world = true;
    reg.add(run);
}

}}} // namespace Slic3r::GUI::Mcp
