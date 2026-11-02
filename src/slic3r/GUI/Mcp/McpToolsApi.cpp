// api_describe and api_call: a generic way to every registered tool and to the operations plugins offer
// (plugin.<key>.<name>), which tools/list never shows.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginOperations.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
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
        "api_describe", L("Describe tools and plugin operations"),
        "Look up any registered tool, and the operations plugins offer (plugin.<plugin>.<name>, not in the tool list): "
        "with tool, its description and input schema; with query, the best matches by name and description, each with "
        "its schema; with neither, every name. Call them with api_call.",
        object_schema({{"tool", prop("string", "A tool or operation name: its schema.")},
                       {"query", prop("string", "A few words to search names and descriptions.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "At most this many (default 20)."}}}}),
        [&service](const json& args, ToolContext&) {
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
            const std::size_t limit = std::size_t(arg_int(args, "limit", 20));
            std::vector<std::string> words;
            {
                std::istringstream in(lower(arg_string(args, "query")));
                for (std::string w; in >> w;)
                    words.push_back(w);
            }
            json items = json::array();
            if (words.empty()) {
                for (const auto& def : all)
                    items.push_back(def->name);
                return ToolResult::ok({{"total", items.size()}, {"names", std::move(items)}});
            }
            std::vector<std::pair<int, std::shared_ptr<const ToolDef>>> scored;
            for (const auto& def : all) {
                const std::string name = lower(def->name), text = lower(def->description);
                int               score = 0;
                for (const std::string& w : words)
                    score += (name.find(w) != std::string::npos ? 3 : 0) + (text.find(w) != std::string::npos ? 1 : 0);
                if (score > 0)
                    scored.emplace_back(score, def);
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t i = 0; i < scored.size() && i < limit; ++i)
                items.push_back(describe_tool(*scored[i].second));
            return ToolResult::ok({{"total", scored.size()}, {"items", std::move(items)}});
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
