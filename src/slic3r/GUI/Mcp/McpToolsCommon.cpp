#include "McpTools.hpp"

#include "McpTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/app.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

json object_schema(std::initializer_list<Property> properties, std::vector<std::string> required)
{
    json props = json::object();
    for (const auto& [name, schema] : properties)
        props[name] = schema;
    json out{{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty())
        out["required"] = std::move(required);
    return out;
}

json prop(const char* type, const std::string& description) { return {{"type", type}, {"description", description}}; }

json enum_prop(std::vector<std::string> values, const std::string& description)
{
    return {{"type", "string"}, {"enum", std::move(values)}, {"description", description}};
}

json array_prop(json items, const std::string& description) { return {{"type", "array"}, {"items", std::move(items)}, {"description", description}}; }

ToolDef inspect_tool(std::string name, std::string title, std::string description, json schema, ToolHandler handler)
{
    ToolDef d;
    d.name         = std::move(name);
    d.title        = std::move(title);
    d.description  = std::move(description);
    d.input_schema = std::move(schema);
    d.read_only    = true;
    d.idempotent   = true;
    d.guard        = GuardClass::Inspect;
    d.inline_call  = true;
    d.journal      = false;
    d.handler      = std::move(handler);
    return d;
}

ToolDef mutate_tool(std::string name, std::string title, std::string description, json schema, ToolHandler handler, bool destructive)
{
    ToolDef d;
    d.name         = std::move(name);
    d.title        = std::move(title);
    d.description  = std::move(description);
    d.input_schema = std::move(schema);
    d.destructive  = destructive;
    d.guard        = GuardClass::Mutate;
    d.handler      = std::move(handler);
    return d;
}

void settle(ToolContext& ctx)
{
    // Only a sync point: the change is made already, so a refusal here (a dialog it opened, a UI
    // rebuild it caused) neither undoes it nor fails the call.
    try {
        ctx.observe_ui([] { return json(); });
    } catch (const ToolFailure&) {
    }
}

Plater& ui_plater()
{
    if (wxTheApp == nullptr || wxGetApp().is_closing())
        throw ToolFailure("OrcaSlicer is shutting down");
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || wxGetApp().mainframe == nullptr)
        throw ToolFailure("OrcaSlicer is still starting; wait {for:\"ready\"} first");
    return *plater;
}

std::string arg_string(const json& args, const char* key, const std::string& fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : fallback;
}

int arg_int(const json& args, const char* key, int fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_number() ? it->get<int>() : fallback;
}

double arg_double(const json& args, const char* key, double fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_number() ? it->get<double>() : fallback;
}

bool arg_bool(const json& args, const char* key, bool fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

json page(const json& items, const json& args, std::size_t default_limit)
{
    const std::size_t total  = items.size();
    const std::size_t offset = std::min<std::size_t>(std::size_t(std::max(0, arg_int(args, "offset", 0))), total);
    const std::size_t limit  = std::size_t(std::max(1, arg_int(args, "limit", int(default_limit))));
    json              out    = json::array();
    for (std::size_t i = offset; i < total && out.size() < limit; ++i)
        out.push_back(items[i]);
    json result{{"total", total}, {"offset", offset}, {"items", std::move(out)}};
    if (offset + limit < total)
        result["next_offset"] = offset + limit;
    return result;
}

json poll_ui(ToolContext& ctx, const std::function<json()>& poll, std::chrono::milliseconds timeout, const std::string& what)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        json v = ctx.observe_ui(poll);
        if (!v.is_null())
            return v;
        if (ctx.cancelled())
            throw ToolFailure("Cancelled: OrcaSlicer is closing or the script was stopped");
        if (std::chrono::steady_clock::now() >= end)
            throw ToolFailure("Timed out waiting for " + what);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

}}} // namespace Slic3r::GUI::Mcp
