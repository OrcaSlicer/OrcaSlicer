// api_describe and api_call: a generic way to every registered tool and to the operations plugins offer
// (plugin.<key>.<name>), which tools/list never shows. api_describe's search also reaches the other
// catalogs an agent calls into: Speed Dial actions, settings, the Python API and the Design tab's methods.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CAD/McpControl.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginOperations.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// How many of each other catalog's matches api_describe shows beside the tools; its own search lists
// the rest.
constexpr std::size_t kCatalogHits = 5;

using Scored = std::vector<std::pair<std::pair<int, int>, json>>;

// A catalog's best matches for api_describe: {total, items, use}, or null when nothing matched.
json catalog_hits(Scored scored, const std::string& use)
{
    if (scored.empty())
        return nullptr;
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    json items = json::array();
    for (std::size_t i = 0; i < scored.size() && i < kCatalogHits; ++i)
        items.push_back(std::move(scored[i].second));
    return {{"total", scored.size()}, {"items", std::move(items)}, {"use", use}};
}

// The Speed Dial actions, settings and Design tab methods matching `words` (of `query`), for
// api_describe. A catalog that can't be read yet (startup, before the main window) is left out. UI thread.
json ui_catalog_hits(const std::vector<std::string>& words, const std::string& query)
{
    json out = json::object();
    if (ActionRegistry* actions = wxGetApp().m_action_registry.get()) {
        ActionRegistry::Query all;
        all.limit = 100000;
        const json listed = actions->describe(all);
        Scored     scored;
        for (const json& a : listed["actions"]) {
            const std::pair<int, int> score = match_score(words, a.value("id", "") + ' ' + a.value("title", ""),
                                                          a.value("group", "") + ' ' + a.value("description", ""));
            if (score.first == 0)
                continue;
            json hit{{"id", a["id"]}, {"title", a["title"]}, {"description", a.value("description", "")}};
            if (a.contains("param"))
                hit["param"] = a["param"];
            scored.emplace_back(score, std::move(hit));
        }
        out["actions"] = catalog_hits(std::move(scored), "run_action {id, param}; list_actions {query} lists more");
    }
    try {
        json settings = search_settings(query, kCatalogHits);
        if (settings["total"] != 0) {
            settings["use"]  = "get_settings {scope, keys} reads one with its description; set_settings {scope, values} changes it";
            out["settings"] = std::move(settings);
        }
    } catch (const ToolFailure&) {
    }
    const json methods = cad_describe_tools(json::object());
    Scored     scored;
    for (const json& row : methods["tools"]) {
        const std::pair<int, int> score = match_score(words, row.value("name", ""), row.value("summary", ""));
        if (score.first > 0)
            scored.emplace_back(score, json{{"name", row["name"]}, {"summary", row.value("summary", "")}});
    }
    out["cad"] = catalog_hits(std::move(scored),
                              wxGetApp().is_enable_cad_feature() ?
                                  "cad_call {method, params}; cad_call {method: \"describe_tools\", params: {method}} gives its params" :
                                  "cad_call {method, params}, once the Design tab is on: a person enables it in Preferences and restarts "
                                  "OrcaSlicer");
    return out;
}

// The Python API entries matching `query`, for api_describe, or null when Python is not running or none
// matches.
json python_hits(const std::string& query)
{
    if (!PythonInterpreter::instance().is_initialized())
        return nullptr;
    PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
    if (!lease)
        return nullptr;
    json ranked;
    try {
        ranked = rank_python_api(python_api_reference(), query);
    } catch (const ToolFailure&) {
        return nullptr; // python_api reports why
    }
    if (ranked.empty())
        return nullptr;
    json items = json::array();
    for (std::size_t i = 0; i < ranked.size() && i < kCatalogHits; ++i)
        items.push_back({{"name", ranked[i]["name"]}, {"kind", ranked[i]["kind"]}, {"signature", ranked[i]["signature"]},
                         {"summary", first_sentence(ranked[i]["doc"].get<std::string>())}});
    return {{"total", ranked.size()},
            {"items", std::move(items)},
            {"use", "run_python {code}; python_api {query} gives each entry's whole documentation"}};
}

json describe_tool(const ToolDef& def)
{
    json out{{"name", def.name},
             {"lane", lane_name(def.lane)},
             {"read_only", def.read_only},
             {"description", def.description},
             {"input_schema", def.input_schema}};
    // The Design tab's methods live behind one tool, which lists them itself.
    if (def.name == "cad_call")
        out["methods"] = "cad_call {method: \"describe_tools\"} lists the Design tab's methods with their parameters";
    return out;
}

// A plugin's operation as a tool: a job (plugin code may take long), under the mutate guard and journaled
// when it changes something, under the inspect guard and unjournaled when it only reads.
std::shared_ptr<const ToolDef> operation_tool(const PluginOperation& op)
{
    auto def          = std::make_shared<ToolDef>();
    def->name         = op.full_name();
    def->title        = op.name;
    def->description  = "[api] Offered by a plugin. " + op.description;
    def->input_schema = op.schema;
    def->lane         = ToolLane::Api;
    def->read_only    = !op.changes;
    def->guard        = op.changes ? GuardClass::Mutate : GuardClass::Inspect;
    def->journal      = op.changes;
    def->handler      = [op](const json& args, ToolContext& ctx) {
        // The operation runs third-party code: the log names who asked and whose code ran.
        const std::string caller = ctx.caller_plugin().empty() ? "client or script" : "plugin " + ctx.caller_plugin();
        BOOST_LOG_TRIVIAL(info) << "[MCP operation] plugin=" << op.plugin_key << " operation=" << op.name << " caller=" << caller;
        // Refused from the moment its plugin unloads, before the unload withdraws the operation.
        if (!PluginManager::instance().is_plugin_loaded(op.plugin_key))
            throw ToolFailure("The plugin that offers this operation has unloaded");
        try {
            return ToolResult::ok(PluginOperations::instance().invoke(op, args));
        } catch (const std::exception& e) {
            throw ToolFailure(std::string("The plugin's operation failed: ") + e.what());
        }
    };
    return def;
}

} // namespace

void register_api_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();
    // Plugin operations resolve by name, beside the registered tools, without being listed.
    reg.set_dynamic([](const std::string& name) -> std::shared_ptr<const ToolDef> {
        if (name.rfind("plugin.", 0) != 0)
            return nullptr;
        if (auto op = PluginOperations::instance().find(name))
            return operation_tool(*op);
        return nullptr;
    });

    reg.add(inspect_tool(
        "api_describe", L("Find tools, actions, settings and APIs"),
        "Find what does something, in one search, or look up one tool. With query: the tools and the operations plugins "
        "offer (plugin.<plugin>.<name>, not in the tool list) that match, best first, a tool with its first sentence (its "
        "schema is in the tool list, or here with tool) and an operation with its schema; and beside "
        "them the best few matches of each other catalog, with its total and how to use it: actions (Speed Dial commands "
        "for run_action), settings (get_settings / set_settings), python (the orca API for run_python) and cad (Design tab "
        "methods for cad_call). With tool: that tool's or operation's description and input schema. With neither: every "
        "tool and operation name. Call tools and operations with api_call.",
        object_schema({{"tool", prop("string", "A tool or operation name: its schema.")},
                       {"query", prop("string", "A few words saying what you want to do or find, e.g. \"brim\" or \"mesh normals\".")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "At most this many tools (default 5)."}}}}),
        [&service](const json& args, ToolContext& ctx) {
            std::vector<std::shared_ptr<const ToolDef>> all;
            for (const ToolDef& def : service.registry().tools())
                all.emplace_back(std::shared_ptr<const ToolDef>(), &def);
            for (const PluginOperation& op : PluginOperations::instance().all())
                all.push_back(operation_tool(op));

            if (args.contains("tool")) {
                const std::string name = arg_string(args, "tool");
                for (const auto& def : all)
                    if (def->name == name)
                        return ToolResult::ok(describe_tool(*def));
                throw ToolFailure("Unknown tool: " + name, {{"did_you_mean", service.registry().similar(name)}});
            }
            const std::size_t              limit = std::size_t(arg_int(args, "limit", int(kCatalogHits)));
            const std::string              query = arg_string(args, "query");
            const std::vector<std::string> words = query_words(query);
            json                           items = json::array();
            if (query.empty()) {
                for (const auto& def : all)
                    items.push_back(def->name);
                return ToolResult::ok({{"total", items.size()}, {"names", std::move(items)}});
            }
            std::vector<std::pair<std::pair<int, int>, std::shared_ptr<const ToolDef>>> scored;
            for (const auto& def : all) {
                const std::pair<int, int> score = match_score(words, def->name, def->description);
                if (score.first > 0)
                    scored.emplace_back(score, def);
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            // A registered tool's schema is in tools/list already, so a match names it with its first sentence; an
            // operation's is nowhere else, so it comes with the match.
            for (std::size_t i = 0; i < scored.size() && i < limit; ++i) {
                const ToolDef& def = *scored[i].second;
                if (def.name.rfind("plugin.", 0) == 0)
                    items.push_back(describe_tool(def));
                else
                    items.push_back({{"name", def.name}, {"lane", lane_name(def.lane)}, {"summary", first_sentence(def.description)}});
            }
            json out{{"total", scored.size()}, {"items", std::move(items)}};
            // The tools and operations answer without the UI thread: one held by a dialog or still starting
            // leaves its catalogs out rather than failing the search.
            try {
                out.update(ctx.run_ui([&] { return ui_catalog_hits(words, query); }));
            } catch (const ToolFailure&) {
            }
            out["python"] = python_hits(query);
            for (const char* catalog : {"actions", "settings", "python", "cad"})
                if (out.contains(catalog) && out[catalog].is_null())
                    out.erase(catalog);
            return ToolResult::ok(std::move(out));
        }));

    // Dispatched before it runs (resolve_api_call): the call runs as the target's own, with its guard,
    // mode, budget and journal flag, and is journaled as the target's call. This handler is never reached.
    reg.add(mutate_tool(
        "api_call", L("Call a tool or plugin operation"),
        "Call any registered tool, or an operation a plugin offers (see api_describe), by name with its arguments. It "
        "runs and is recorded as that tool's own call, with its guards and reply budget.",
        object_schema({{"tool", prop("string", "The tool or operation to call.")},
                       {"args", {{"type", "object"}, {"description", "Its arguments."}}}},
                      {"tool"}),
        [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("api_call is dispatched to its tool before it runs"); }));
}

}}} // namespace Slic3r::GUI::Mcp
