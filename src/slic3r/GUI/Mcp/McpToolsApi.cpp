// search_tools and call_tool: a generic way to every registered tool and to the operations plugins offer
// (plugin.<key>.<name>), which tools/list never shows. search_tools's search also reaches the other
// catalogs an agent calls into: Speed Dial actions, settings, the Python API and the Design tab's CAD tools.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#ifdef SLIC3R_CAD
#include "slic3r/GUI/Mcp/McpToolsCad.hpp"
#endif
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

json editor_tool_catalog(const ToolRegistry& registry)
{
    json tools = json::array();
    for (const ToolDef& def : registry.tools()) {
        json       args       = json::object();
        const auto properties = def.input_schema.find("properties");
        if (properties != def.input_schema.end())
            for (const auto& [name, schema] : properties->items()) {
                json arg = json::object();
                for (const char* key : {"type", "enum"})
                    if (schema.contains(key))
                        arg[key] = schema[key];
                args[name] = std::move(arg);
            }
        tools.push_back({{"name", def.name}, {"title", _u8L(def.title.c_str())}, {"args", std::move(args)}});
    }
    return tools;
}

namespace {

// How many of each other catalog's matches search_tools shows beside the tools; its own search lists
// the rest.
constexpr std::size_t kCatalogHits = 5;

using Scored = std::vector<std::pair<std::pair<int, int>, json>>;

// A catalog's best matches for search_tools: {total, items, use}, or null when nothing matched.
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

// The Speed Dial actions, settings and CAD tools matching `words` (of `query`), for search_tools. A
// catalog that can't be read yet (startup, before the main window) is left out. UI thread.
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
        out["actions"] = catalog_hits(std::move(scored), "run_action {id, param}; search_actions {query} finds more");
    }
    try {
        json settings = search_settings(query, kCatalogHits);
        if (settings["total"] != 0) {
            settings["use"]  = "get_settings {scope, keys} reads one with its description; set_settings {scope, values} changes it";
            out["settings"] = std::move(settings);
        }
    } catch (const ToolFailure&) {
    }
#ifdef SLIC3R_CAD
    // Scored on the details too, so a word that is not in the one-line summary still finds the tool.
    Scored scored;
    for (const json& row : cad_tool_table()) {
        const std::pair<int, int> score = match_score(words, row.at("name").get<std::string>(),
                                                      row.at("summary").get<std::string>() + ' ' +
                                                          row.at("details").get<std::string>());
        if (score.first > 0)
            scored.emplace_back(score, json{{"name", row.at("name")}, {"summary", row.at("summary")}});
    }
    out["cad"] = catalog_hits(std::move(scored),
                              wxGetApp().is_enable_cad_feature() ?
                                  "call_cad_tool {method, params}; call_cad_tool {method: \"describe_tools\", params: {method}} gives its params" :
                                  "call_cad_tool {method, params}, once the Design tab is on: a person enables it in Preferences and restarts "
                                  "OrcaSlicer");
#endif
    return out;
}

// The Python API entries matching `query`, for search_tools, or null when Python is not running or none
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
        return nullptr; // search_python_api reports why
    }
    if (ranked.empty())
        return nullptr;
    json items = json::array();
    for (std::size_t i = 0; i < ranked.size() && i < kCatalogHits; ++i)
        items.push_back({{"name", ranked[i]["name"]}, {"kind", ranked[i]["kind"]}, {"signature", ranked[i]["signature"]},
                         {"summary", first_sentence(ranked[i]["doc"].get<std::string>())}});
    return {{"total", ranked.size()},
            {"items", std::move(items)},
            {"use", "run_script {code}; search_python_api {query} gives each entry's whole documentation"}};
}

json describe_tool(const ToolDef& def)
{
    json out{{"name", def.name},
             {"lane", lane_name(def.lane)},
             {"read_only", def.read_only},
             {"description", def.description},
             {"input_schema", def.input_schema}};
    // The Design tab's methods live behind one tool, which lists them itself.
    if (def.name == "call_cad_tool")
        out["methods"] = "call_cad_tool {method: \"describe_tools\"} lists the Design tab's methods with their parameters";
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
        const std::string caller = ctx.caller_plugin().empty() ? "client, replay or script" : "plugin " + ctx.caller_plugin();
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
        "search_tools", L("Find tools, actions, settings and APIs"),
        "Find what does something, in one search, or look up one tool. With query: the tools and the operations plugins "
        "offer (plugin.<plugin>.<name>, not in the tool list) that match, best first, a tool with its first sentence (its "
        "schema is in the tool list, or here with tool) and an operation with its schema; and beside "
        "them the best few matches of each other catalog, with its total and how to use it: actions (Speed Dial commands "
        "for run_action), settings (get_settings / set_settings), python (the orca API for run_script) and cad (Design tab "
        "methods for call_cad_tool). With tool: that tool's or operation's description and input schema. With neither: every "
        "tool and operation name. Call tools and operations with call_tool.",
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
    ToolDef call = mutate_tool(
        "call_tool", L("Call a tool or plugin operation"),
        "Call any registered tool, or an operation a plugin offers (see search_tools), by name with its arguments. It "
        "runs and is recorded as that tool's own call, with its guards and reply budget.",
        object_schema({{"tool", prop("string", "The tool or operation to call.")},
                       {"args", {{"type", "object"}, {"description", "Its arguments."}}}},
                      {"tool"}),
        [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("call_tool is dispatched to its tool before it runs"); });
    // It calls any tool, run_script included, and plugin code.
    call.open_world = true;
    reg.add(call);
}

}}} // namespace Slic3r::GUI::Mcp
