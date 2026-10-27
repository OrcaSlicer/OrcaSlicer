// list_actions and run_action: every Speed Dial action - built-in commands, plates, recent projects,
// plugin scripts and pages, and (on request) settings - is reachable through these two tools.

#include "McpOperations.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <chrono>
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

ToolResult await_gcode_export(ToolContext& ctx, uint64_t before, uint64_t after, uint64_t revision, const std::string& rejection)
{
    if (after == before)
        return ToolResult::error(rejection.empty() ? "The export did not start" : rejection);
    json done = poll_ui(
        ctx,
        [before, after, revision]() -> json {
            Plater& plater = ui_plater();
            auto    run    = plater.export_run(after);
            std::optional<ExportRecord> record;
            if (run) {
                record = ExportRecord{run->seq, run->path, run->finished, run->ok, run->cancelled, run->error, run->revision_at_finish};
            }
            const ExportStatus status = export_status(before, after, record, revision);
            if (status == ExportStatus::Pending)
                return json();
            json out{{"export", to_string(status)}, {"path", run ? run->path : std::string()}};
            if (run && !run->error.empty())
                out["error"] = run->error;
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
        "list_actions", "Search Speed Dial actions",
        "Search everything the Speed Dial can run: built-in commands (slice, export, views, object operations, plates, "
        "calibration wizards, help), recent projects, plugin scripts and pages. Each entry has an id for run_action, a "
        "description and, when it takes one, a param (type and help). include_settings also lists settings (each jumps "
        "to that setting in the sidebar; use get_settings / set_settings to read or change values).",
        object_schema({{"query", prop("string", "Case-insensitive text to find in id, title, group or description.")},
                       {"group", prop("string", "Only this group, e.g. \"Object\", \"View\", \"Slice & Export\".")},
                       {"include_settings", prop("boolean", "Also list settings (thousands). Default false.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Default 50."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            ActionRegistry::Query q;
            q.text             = arg_string(args, "query");
            q.group            = arg_string(args, "group");
            q.include_settings = arg_bool(args, "include_settings", false);
            q.limit            = std::size_t(arg_int(args, "limit", 50));
            q.offset           = std::size_t(arg_int(args, "offset", 0));
            return ToolResult::ok(ctx.run_ui([q] { return registry_or_fail().describe(q); }));
        }));

    reg.add(mutate_tool(
        "run_action", "Run a Speed Dial action",
        "Run one action by the id list_actions returned, with its param when it takes one (a percent, tab id, plate index, "
        "or an absolute path - commands with a path parameter skip their file dialog; outputs need {\"path\": ..., "
        "\"overwrite\": true} to replace a file). Actions that open a dialog return blocked_by_dialog; answer it with "
        "dialog_respond. An export_gcode with a path completes when the file is written.",
        object_schema({{"id", prop("string", "Action id from list_actions.")},
                       {"param", prop("string", "Parameter, when the action takes one.")}},
                      {"id"}),
        [](const json& args, ToolContext& ctx) {
            const std::string id    = arg_string(args, "id");
            const std::string param = arg_string(args, "param");
            json started = ctx.run_ui([&]() -> json {
                ActionRegistry& actions = registry_or_fail();
                const AppAction* action = actions.by_id(id);
                if (action == nullptr) {
                    // Suggest the ids closest to the requested one.
                    ActionRegistry::Query q;
                    q.limit = 100000;
                    std::vector<std::pair<std::size_t, std::string>> scored;
                    const json all = actions.describe(q);
                    for (const json& a : all["actions"]) {
                        const std::string candidate = a["id"];
                        scored.emplace_back(edit_distance(id, candidate), candidate);
                    }
                    std::sort(scored.begin(), scored.end());
                    json similar = json::array();
                    for (std::size_t i = 0; i < scored.size() && i < 3 && scored[i].first <= id.size() / 3 + 2; ++i)
                        similar.push_back(scored[i].second);
                    throw ToolFailure("Unknown action id " + id + "; search with list_actions", {{"similar", similar}});
                }
                const bool background = action->completes_in_background && !param.empty();
                Plater*    plater     = wxGetApp().plater();
                uint64_t   before = 0, revision = 0;
                if (background && plater != nullptr) {
                    before   = plater->export_seq();
                    revision = plater->project_revision();
                }
                const AppActionRunResult r = actions.run(id, param, /*record_stats=*/false);
                json out{{"level", level_name(r.level)}, {"message", into_u8(r.message)}, {"background", background}};
                if (background && plater != nullptr)
                    out.update({{"before", before}, {"after", plater->export_seq()}, {"revision", revision}});
                return out;
            });
            if (started["background"].get<bool>()) {
                ctx.begin_operation();
                if (started["level"] == "error")
                    return ToolResult::error(started["message"].get<std::string>());
                return await_gcode_export(ctx, started["before"], started["after"], started["revision"], started["message"]);
            }
            settle(ctx);
            const std::string level = started["level"];
            if (level == "error" || level == "busy")
                return ToolResult::error(started["message"].get<std::string>().empty() ? "The action failed" : started["message"].get<std::string>());
            json out{{"ran", id}, {"level", level}};
            if (!started["message"].get<std::string>().empty())
                out["message"] = started["message"];
            return ToolResult::ok(out);
        }));
}

}}} // namespace Slic3r::GUI::Mcp
