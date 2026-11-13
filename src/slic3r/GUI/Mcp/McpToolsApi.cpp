// search_tools and call_tool: a generic way to every command of the catalog, those tools/list never shows
// among them: the Speed Dial's commands, the Design tab's and the operations plugins offer (plugin.<key>.<name>).
// search_tools's search also reaches the other catalogs an agent calls into: the Speed Dial's other actions,
// settings and the Python API.

#include "McpTools.hpp"
#include "McpTypes.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginOperations.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"

#include <boost/algorithm/string/predicate.hpp>
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

json editor_tool_catalog(const CommandCatalog& catalog)
{
    json tools = json::array();
    for (const NativeCommand& def : catalog.commands()) {
        if (!def.listed)
            continue;
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
        tools.push_back(
            {{"name", def.key}, {"title", into_u8(NativeCommands::tile_title(def))}, {"args", std::move(args)}});
    }
    return tools;
}

namespace {

// How many of each other catalog's matches search_tools shows beside the commands, and of the commands
// without a limit; each catalog's own search lists the rest.
constexpr std::size_t kCatalogHits = 5;

// What search_tools looks through: every command of `catalog`, in the order they were added, then what its
// sources hold now; with `group`, those of that group, compared in any case. A group none of them has is
// refused with the groups there are, in the order they first appear.
std::vector<std::shared_ptr<const NativeCommand>> search_inventory(const CommandCatalog& catalog, const std::string& group)
{
    std::vector<std::shared_ptr<const NativeCommand>> all;
    for (const NativeCommand& def : catalog.commands())
        all.emplace_back(std::shared_ptr<const NativeCommand>(), &def); // the catalog owns it
    for (std::shared_ptr<const NativeCommand>& def : catalog.source_commands())
        all.push_back(std::move(def));
    if (group.empty())
        return all;
    std::vector<std::shared_ptr<const NativeCommand>> out;
    std::vector<std::string>                          groups;
    for (std::shared_ptr<const NativeCommand>& def : all) {
        if (std::find(groups.begin(), groups.end(), def->group) == groups.end())
            groups.push_back(def->group);
        if (boost::algorithm::iequals(def->group, group))
            out.push_back(std::move(def));
    }
    if (out.empty()) {
        std::string names;
        for (const std::string& name : groups)
            names += (names.empty() ? "" : ", ") + name;
        throw ToolFailure("Unknown group: " + group + "; the groups are " + names, {{"groups", groups}});
    }
    return out;
}

// A catalog's best matches for search_tools, from `ranked`, its matches best first: {total, items, use}
// with the first few, or null when nothing matched.
json catalog_hits(const json& ranked, const std::string& use)
{
    if (ranked.empty())
        return nullptr;
    json items = json::array();
    for (std::size_t i = 0; i < ranked.size() && i < kCatalogHits; ++i)
        items.push_back(ranked[i]);
    return {{"total", ranked.size()}, {"items", std::move(items)}, {"use", use}};
}

// The Speed Dial's actions that are no command (plates, recent projects, plugin scripts and pages) and settings
// matching `words` (of `query`), for search_tools. A command's tile is left out: the command is among the
// commands already. A catalog that can't be read yet (startup, before the main window) is left out. UI thread.
json ui_catalog_hits(const std::vector<std::string>& words, const std::string& query)
{
    json out = json::object();
    if (ActionRegistry* actions = wxGetApp().m_action_registry.get()) {
        ActionRegistry::Query all;
        all.limit = 100000;
        json listed = actions->describe(all);
        json others = json::array();
        for (json& action : listed["items"])
            if (!action.contains("command"))
                others.push_back(std::move(action));
        // Ranked as search_actions ranks them, so the same query reads on there from these.
        out["actions"] = catalog_hits(rank_actions(others, words), "run_action {id, args}; search_actions {query} lists the rest");
    }
    try {
        json settings = search_settings(query, kCatalogHits);
        if (settings["total"] != 0) {
            settings["use"]  = "get_settings {scope, keys} reads one with its description; set_settings {scope, values} changes it";
            out["settings"] = std::move(settings);
        }
    } catch (const ToolFailure&) {
    }
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

json describe_tool(const NativeCommand& def)
{
    json out{{"name", def.key},
             {"group", def.group},
             {"listed", def.listed},
             {"read_only", def.read_only},
             {"description", def.description},
             {"input_schema", def.input_schema}};
    if (!def.examples.is_null())
        out["examples"] = def.examples;
    return out;
}

// A plugin's operation as a command: a job (plugin code may take long), under the mutate guard and journaled
// when it changes something, under the inspect guard and unjournaled when it only reads. Not listed; in the api
// group, as it is called by name with JSON in and out.
std::shared_ptr<const NativeCommand> operation_command(const PluginOperation& op)
{
    auto def          = std::make_shared<NativeCommand>();
    def->key          = op.full_name();
    def->title        = op.name;
    def->group        = "api";
    def->description  = "Offered by a plugin. " + op.description;
    def->input_schema = op.schema;
    def->read_only    = !op.changes;
    def->guard        = op.changes ? GuardClass::Mutate : GuardClass::Inspect;
    def->journal      = op.changes;
    def->run          = [op](const json& args, ToolContext& ctx) {
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

// The operations the loaded plugins offer (plugin.<key>.<name>), which come and go with their plugins: a
// call finds one by name, and search_tools lists them; tools/list does not.
class PluginOperationSource final : public CommandSource
{
public:
    std::shared_ptr<const NativeCommand> find(const std::string& key) const override
    {
        if (key.rfind("plugin.", 0) != 0)
            return nullptr;
        if (auto op = PluginOperations::instance().find(key))
            return operation_command(*op);
        return nullptr;
    }
    std::vector<std::shared_ptr<const NativeCommand>> all() const override
    {
        std::vector<std::shared_ptr<const NativeCommand>> out;
        for (const PluginOperation& op : PluginOperations::instance().all())
            out.push_back(operation_command(op));
        return out;
    }
};

} // namespace

json search_commands(const CommandCatalog& catalog, const json& args)
{
    if (args.contains("tool")) {
        const std::string name = arg_string(args, "tool");
        if (const std::shared_ptr<const NativeCommand> def = catalog.lookup(name))
            return describe_tool(*def);
        throw ToolFailure(catalog.unknown_command(name), {{"did_you_mean", catalog.similar(name)}});
    }
    const std::vector<std::shared_ptr<const NativeCommand>> all   = search_inventory(catalog, arg_string(args, "group"));
    const std::string                                       query = arg_string(args, "query");
    json                                                    items = json::array();
    if (query.empty()) {
        for (const auto& def : all)
            items.push_back(def->key);
        return {{"total", items.size()}, {"names", std::move(items)}};
    }
    // A command is called by its key; its title, a person's name for it, counts as its text. Among equal
    // matches a listed command comes first: the typed tools are what an agent calls, and many tiles run one.
    const std::vector<std::string>                                    words = query_words(query);
    std::vector<std::pair<std::pair<int, int>, const NativeCommand*>> scored;
    for (const auto& def : all) {
        const std::pair<int, int> score = match_score(words, def->key, def->title + ' ' + def->description);
        if (score.first > 0)
            scored.emplace_back(score, def.get());
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second->listed && !b.second->listed;
    });
    const std::size_t limit = std::size_t(arg_int(args, "limit", int(kCatalogHits)));
    for (std::size_t i = 0; i < scored.size() && i < limit; ++i) {
        const NativeCommand& def = *scored[i].second;
        const std::string summary = first_sentence(def.description);
        json              item{{"name", def.key}, {"group", def.group}, {"summary", summary}, {"listed", def.listed}};
        // A listed command's schema is in tools/list; nothing else shows the others' description and schema.
        if (!def.listed) {
            if (def.description != summary)
                item["description"] = def.description;
            item["input_schema"] = def.input_schema;
        }
        items.push_back(std::move(item));
    }
    return {{"total", scored.size()},
            {"items", std::move(items)},
            {"use", "call a listed command by its name; call_tool {tool, args} runs one that is not listed"}};
}

void register_api_tools(CommandCatalog& catalog)
{
    // Plugin operations resolve by name, beside the catalog's commands, without being listed.
    catalog.add_source(std::make_unique<PluginOperationSource>());

    catalog.add(inspect_tool(
        "api", "search_tools", L("Find tools, actions, settings and APIs"),
        "Find what does something, in one search, or look up one command. With query: the commands that match, best "
        "first, each with its group, the first sentence of its description and listed, whether the tool list shows it; "
        "one that is not listed (a Speed Dial command, a Design tab command, or an operation a plugin offers, "
        "plugin.<plugin>.<name>) comes with its input schema, and its description when that says more, which nothing "
        "else shows. Beside them, the best few matches of each other catalog, with its total and how to use it: actions "
        "(the Speed Dial's plates, recent projects and plugin scripts and pages, for run_action), settings (get_settings "
        "/ set_settings) and python (the orca API for run_script). With tool: that command's description and input schema, "
        "and its examples when it has some (the Design commands do). With neither: every command's name. group keeps the "
        "names and the command matches to one group; the other catalogs' matches stay as they are. call_tool runs any "
        "command, listed or not.",
        object_schema({{"tool", prop("string", "A command's name: its description and schema.")},
                       {"query", prop("string", "A few words saying what you want to do or find, e.g. \"brim\" or \"mesh normals\".")},
                       {"group", prop("string", "Only the commands of this group, in any case: core (the session, dialogs and "
                                                "Python), api (project data by index and name; plugin operations too), ui (the "
                                                "GUI itself), Design (the Design tab's, with the CAD feature on), or a Speed Dial "
                                                "section such as Commands, Object or View. A group no command has is refused with "
                                                "the groups there are.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "At most this many commands (default 5)."}}}}),
        [](const json& args, ToolContext& ctx) {
            json out = search_commands(NativeCommands::instance(), args);
            if (args.contains("tool") || arg_string(args, "query").empty())
                return ToolResult::ok(std::move(out));
            const std::string              query = arg_string(args, "query");
            const std::vector<std::string> words = query_words(query);
            // The commands answer without the UI thread: one held by a dialog or still starting leaves its
            // catalogs out rather than failing the search.
            try {
                out.update(ctx.run_ui([&] { return ui_catalog_hits(words, query); }));
            } catch (const ToolFailure&) {
            }
            out["python"] = python_hits(query);
            for (const char* other : {"actions", "settings", "python"})
                if (out.contains(other) && out[other].is_null())
                    out.erase(other);
            return ToolResult::ok(std::move(out));
        }));

    // Dispatched before it runs (resolve_call): the call runs as the target's own, with its guard,
    // mode, budget and journal flag, and is journaled as the target's call. This handler is never reached.
    NativeCommand call = mutate_tool(
        "api", "call_tool", L("Call a tool or plugin operation"),
        "Call any command by name with its arguments: a tool, or one the tool list does not show, a Speed Dial command, a "
        "Design tab command or an operation a plugin offers (see search_tools). It runs and is recorded as that command's "
        "own call, with its guards and reply budget.",
        object_schema({{"tool", prop("string", "The command to call.")},
                       {"args", {{"type", "object"}, {"description", "Its arguments."}}}},
                      {"tool"}),
        [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("call_tool is dispatched to its tool before it runs"); });
    // It calls any tool, run_script included, and plugin code.
    call.open_world = true;
    catalog.add(call);
}

}}} // namespace Slic3r::GUI::Mcp
