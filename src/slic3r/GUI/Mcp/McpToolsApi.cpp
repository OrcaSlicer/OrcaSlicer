// tool_search and tool_call: a generic way to every command of the catalog, those tools/list does not show among
// them: the menu items that take no arguments, the Design tab's commands and the operations plugins offer
// (plugin.<key>.<name>). tool_search's search also reaches the other catalogs an agent calls into: the Speed Dial's
// other actions, settings and the Python API.

#include "McpArguments.hpp"
#include "McpTools.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
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
#include <cctype>
#include <cstddef>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
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
            {{"name", def.key}, {"title", into_u8(NativeCommands::action_title(def))}, {"args", std::move(args)}});
    }
    return tools;
}

namespace {

// How many of each other catalog's matches tool_search shows beside the commands, and of the commands
// without a limit; each catalog's own search lists the rest.
constexpr std::size_t kCatalogHits = 5;

// What tool_search looks through: every command of `catalog`, in the order they were added, then what its
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
        throw ToolFailure("Unknown group: " + group + "; the groups are " + names, {{"code", "invalid_argument"}, {"groups", groups}});
    }
    return out;
}

// A catalog's best matches for tool_search, from `ranked`, its matches best first: {total, items, use}
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
// matching `words` (of `query`), for tool_search. A command's action is left out: the command is among the
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
        // Ranked as action_search ranks them, so the same query reads on there from these.
        out["actions"] = catalog_hits(rank_actions(others, words), "action_run {id, args}; action_search {query} lists the rest");
    }
    try {
        json settings = search_settings(query, kCatalogHits);
        if (settings["total"] != 0) {
            settings["use"]  = "settings_get {type, keys} reads one with its description; settings_set {type, values} changes it";
            out["settings"] = std::move(settings);
        }
    } catch (const ToolFailure&) {
    }
    return out;
}

// The Python API entries matching `query`, for tool_search, or null when Python is not running or none
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
        return nullptr; // script_search_api reports why
    }
    if (ranked.empty())
        return nullptr;
    json items = json::array();
    for (std::size_t i = 0; i < ranked.size() && i < kCatalogHits; ++i)
        items.push_back({{"name", ranked[i]["name"]}, {"kind", ranked[i]["kind"]}, {"signature", ranked[i]["signature"]},
                         {"summary", first_sentence(ranked[i]["doc"].get<std::string>())}});
    return {{"total", ranked.size()},
            {"items", std::move(items)},
            {"use", "script_run {code}; script_search_api {query} gives each entry's whole documentation"}};
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
// when it changes something, under the inspect guard and unjournaled when it only reads. Not listed; in the group
// Plugins, the place of the commands that manage the plugins.
std::shared_ptr<const NativeCommand> operation_command(const PluginOperation& op)
{
    auto def          = std::make_shared<NativeCommand>();
    def->key          = op.full_name();
    def->title        = op.name;
    def->group        = "Plugins";
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
// call finds one by name, and tool_search lists them; tools/list does not.
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
        throw ToolFailure(catalog.unknown_command(name),
                          {{"code", catalog.unknown_command_code(name)}, {"did_you_mean", catalog.similar(name)}});
    }
    const std::string                                       group = arg_string(args, "group");
    const std::vector<std::shared_ptr<const NativeCommand>> all   = search_inventory(catalog, group);
    const std::string                                       query = arg_string(args, "query");
    json                                                    items = json::array();
    if (query.empty() && group.empty()) {
        for (const auto& def : all)
            items.push_back(def->key);
        return {{"total", items.size()}, {"names", std::move(items)}};
    }
    // A group is few enough commands to say what each does, so an agent picks one from the listing rather than
    // looking each up; their schemas stay with {tool}, as the Design group's would take some 140 KB.
    if (query.empty()) {
        for (const auto& def : all) {
            json item{{"name", def->key}, {"summary", first_sentence(def->description)}, {"listed", def->listed}};
            if (!def->heading.empty())
                item["heading"] = def->heading;
            items.push_back(std::move(item));
        }
        return {{"total", items.size()},
                {"items", std::move(items)},
                {"use", "tool_search {tool} gives a command's description and schema; call a listed command by its name, "
                        "and tool_call {tool, args} runs one that is not listed"}};
    }
    // A command is called by its key; its title, a person's name for it, counts as its text. Among equal
    // matches a listed command comes first: the typed tools are what an agent calls, and many Speed Dial actions run one.
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
            {"use", "call a listed command by its name; tool_call {tool, args} runs one that is not listed"}};
}

namespace {

ParameterClass source(std::vector<std::string> tools) { return {ParameterClass::Source, std::move(tools)}; }
const ParameterClass kDescribed{ParameterClass::Described, {}};
const ParameterClass kFree{ParameterClass::Free, {}};

// The classes of a command's own parameters, which come before a name's: a name that means something else there, or
// that no other command shares.
const std::map<std::string, ParameterClass>& classes_by_path()
{
    static const std::map<std::string, ParameterClass> rows{
        // Finding and calling commands and actions.
        {"tool_search.tool", source({"tools/list", "tool_search"})},
        {"tool_describe.tool", source({"tools/list", "tool_search"})},
        {"tool_call.tool", source({"tools/list", "tool_search"})},
        {"tool_call.args", source({"tool_describe"})},
        {"action_run.id", source({"action_search"})},
        {"action_run.args", source({"action_search"})},
        {"action_search.section", source({"action_search"})},
        {"script_run.args", kFree},
        {"replay_run.vars", kFree},
        // Settings and preferences: the keys a tool takes are those its reader lists.
        {"settings_get.keys", source({"settings_list"})},
        {"settings_set.values", source({"settings_list"})},
        {"settings_list.where", kDescribed},
        {"settings_get.min_z", source({"object_list"})},
        {"settings_get.max_z", source({"object_list"})},
        {"settings_set.min_z", source({"object_list"})},
        {"settings_set.max_z", source({"object_list"})},
        {"project_get_settings.keys", source({"project_get_settings"})},
        {"project_set_settings.values", source({"project_get_settings"})},
        {"plate_get_settings.keys", source({"plate_get_settings"})},
        {"plate_set_settings.values", source({"plate_get_settings"})},
        {"preferences_get.keys", source({"preferences_get"})},
        {"preferences_set.values", source({"preferences_get"})},
        {"window_open.key", source({"preferences_get"})},
        // Presets and printer models.
        {"preset_select.name", source({"preset_list"})},
        {"preset_delete.name", source({"preset_list"})},
        {"settings_get.compare", source({"preset_list"})},
        {"printer_list_models.vendor", source({"printer_list_models"})},
        {"printer_install.models[].vendor", source({"printer_list_models"})},
        {"printer_uninstall.models[].vendor", source({"printer_list_models"})},
        // Input: a chord is the caller's; the menu an input opens is listed by the same call made without popup_item.
        {"keyboard_press.keys", kFree},
        {"widget_click.popup_item", source({"widget_click"})},
        {"keyboard_press.popup_item", source({"keyboard_press"})},
        {"mouse_gesture.popup_item", source({"mouse_gesture"})},
        {"widget_list.target", source({"window_list"})},
        {"window_set.target", source({"window_list"})},
        {"window_screenshot.target", source({"window_list", "widget_list"})},
        {"dialog_auto_answer.rules[].button", kDescribed},
        // A sliced plate's layers, by height too.
        {"plate_set_custom_gcode.add[].z", source({"plate_slice_stats"})},
        {"plate_set_custom_gcode.remove[].z", source({"plate_slice_stats"})},
        // The Design tab: the shapes a sketch draws are new; a sketch line as a mirror axis; a hole's point is the
        // caller's, a thread's may be a face; what a feature's card picks; the bodies a boolean takes.
        {"sketch_draw.entities", kFree},
        {"sketch_mirror.axis", source({"sketch_describe"})},
        {"hole.at", kFree},
        {"thread.at", source({"body_describe"})},
        {"feature_edit.refs", source({"feature_describe"})},
        {"boolean.target", source({"describe_design"})},
        {"boolean.tool", source({"describe_design"})},
        {"sweep.path", source({"describe_design"})},
    };
    return rows;
}

// The classes of the names the built-in commands give their parameters, every name but those that only a command's
// own entry classifies (classes_by_path()).
const std::map<std::string, ParameterClass>& classes_by_name()
{
    const ParameterClass objects = source({"object_list"}), plates = source({"plate_list"});
    const ParameterClass slots = source({"filament_list"}), presets = source({"preset_list"});
    const ParameterClass design = source({"describe_design"}), geometry = source({"body_describe"});
    const ParameterClass sketch = source({"sketch_describe"}), layers = source({"plate_slice_stats"});
    static const std::map<std::string, ParameterClass> rows{
        // The project.
        {"object", objects}, {"objects", objects}, {"object_id", objects}, {"object_ids", objects}, {"part", objects},
        {"parts", objects}, {"part_id", objects}, {"part_ids", objects}, {"instance", objects}, {"instances", objects},
        {"instance_object_ids", objects}, {"fill_object", objects}, {"order", objects}, {"replace", objects},
        {"plate", plates}, {"plates", plates},
        {"filament_slot", slots}, {"filament", slots}, {"components", slots}, {"merge_into", slots},
        {"layer", layers}, {"layer_range", layers}, {"layer_z", layers},
        // Presets, printer models and plugins.
        {"preset", presets}, {"printer", presets}, {"names", presets}, {"vendor", presets}, {"filament_type", presets},
        {"model", source({"printer_list_models"})}, {"nozzles", source({"printer_list_models"})},
        {"plugin", source({"plugin_list"})}, {"capability", source({"plugin_list"})},
        // The app's jobs, dialogs and widgets.
        {"job", source({"app_state"})}, {"button", source({"app_state"})}, {"title", source({"app_state"})},
        {"target", source({"widget_list"})},
        // The Design document: features, bodies, their faces and edges, and a sketch's entities.
        {"body", design}, {"source_body", design}, {"face_body", design}, {"feature", design}, {"before", design},
        {"sketch", design}, {"profiles", design}, {"cs_a", design}, {"cs_b", design}, {"plane_a", design},
        {"plane_b", design}, {"reference", design}, {"base", design},
        {"generation", source({"describe_design", "body_describe"})}, {"plane", source({"describe_design", "body_describe"})},
        {"face", geometry}, {"faces", geometry}, {"face_a", geometry}, {"face_b", geometry}, {"edge", geometry},
        {"edges", geometry}, {"edge_a", geometry}, {"edge_b", geometry}, {"vertex", geometry}, {"up_to_face", geometry},
        {"a", geometry}, {"b", geometry},
        {"entities", sketch}, {"entity", sketch}, {"refs", sketch}, {"ent_a", sketch}, {"ent_b", sketch},
        {"axis_entity", sketch}, {"loop", sketch},
        {"index", source({"sketch_list_constraints"})},
        {"field", source({"feature_describe"})}, {"fields", source({"feature_describe"})},
        // Described: the parameter's own description, or its items' values, lists what it takes.
        {"edge_kinds", kDescribed}, {"kinds", kDescribed}, {"include", kDescribed}, {"legend", kDescribed},
        {"modifiers", kDescribed}, {"prime_volume_mode", kDescribed}, {"shaper_type", kDescribed}, {"tab", kDescribed},
        {"group", kDescribed},
        // Free: the caller's own numbers, points, text, paths, names and shapes.
        {"accelerations", kFree}, {"across_flats", kFree}, {"angle", kFree}, {"angle_deg", kFree}, {"at", kFree},
        {"axis_a", kFree}, {"axis_b", kFree}, {"axis_x", kFree}, {"axis_y", kFree}, {"axis_z", kFree}, {"c0", kFree},
        {"c1", kFree}, {"cbore_depth", kFree}, {"cbore_diameter", kFree}, {"center", kFree}, {"code", kFree},
        {"color", kFree}, {"colour", kFree}, {"corner", kFree}, {"count", kFree}, {"crf", kFree},
        {"csink_angle", kFree}, {"csink_diameter", kFree}, {"ctrl", kFree}, {"damping", kFree}, {"delta", kFree},
        {"depth", kFree}, {"designation", kFree}, {"diameter", kFree}, {"direction", kFree}, {"distance", kFree},
        {"distance2", kFree}, {"dx", kFree}, {"dy", kFree}, {"dz", kFree}, {"end", kFree}, {"end_angle", kFree},
        {"end_angle_deg", kFree}, {"end_y", kFree}, {"entries", kFree}, {"expr", kFree}, {"filename", kFree},
        {"flush_multiplier", kFree}, {"flush_volumes_matrix", kFree}, {"font", kFree}, {"fps", kFree},
        {"freq_x", kFree}, {"freq_y", kFree}, {"gcode", kFree}, {"height", kFree}, {"jpeg_quality", kFree},
        {"keys", source({"settings_list"})}, {"layer_percent", kFree}, {"length", kFree}, {"limit", kFree},
        {"lines", kFree}, {"look_at", kFree}, {"match", kFree}, {"max_dim", kFree}, {"max_nodes", kFree},
        {"max_seconds", kFree}, {"max_web_nodes", kFree}, {"max_z", kFree}, {"merge_angle_deg", kFree},
        {"min_volume", kFree}, {"min_z", kFree}, {"move", kFree}, {"name", kFree}, {"normal", kFree},
        {"offset", kFree}, {"on_arc", kFree}, {"orbit", kFree}, {"out_dir", kFree}, {"p", kFree}, {"p0", kFree},
        {"p1", kFree}, {"p2", kFree}, {"path", kFree}, {"paths", kFree}, {"pitch", kFree}, {"pivot", kFree},
        {"pivot_x", kFree}, {"pivot_y", kFree}, {"pivot_z", kFree}, {"point", kFree}, {"points", kFree},
        {"position", kFree}, {"profile", kFree}, {"quality", kFree}, {"query", kFree}, {"radius", kFree},
        {"ratios", kFree}, {"replay", kFree}, {"rminor", kFree}, {"rotate", kFree}, {"rotation", kFree},
        {"rotation_deg", kFree}, {"save_path", kFree}, {"scale", kFree}, {"sidebar_width", kFree}, {"sides", kFree},
        {"size", kFree}, {"spacing", kFree}, {"speeds", kFree}, {"standard", kFree}, {"start", kFree},
        {"start_angle", kFree}, {"start_angle_deg", kFree}, {"start_y", kFree}, {"step", kFree}, {"steps", kFree},
        {"taper", kFree}, {"taper_deg", kFree}, {"text", kFree}, {"thickness", kFree}, {"timeout_s", kFree},
        {"to", kFree}, {"tolerance", kFree}, {"translate", kFree}, {"value", kFree}, {"waypoints", kFree},
        {"width", kFree}, {"x", kFree}, {"x_hint", kFree}, {"y", kFree}, {"z", kFree}, {"zoom", kFree},
    };
    return rows;
}

// A plugin's operation (plugin.<plugin>.<name>), whose parameters are named by the plugin and classified by nobody.
bool is_plugin_operation(const NativeCommand& command) { return command.key.rfind("plugin.", 0) == 0; }

// "number", or "number or null" for a list of types.
std::string type_text(const json& type)
{
    if (type.is_string())
        return type.get<std::string>();
    std::string out;
    for (const json& t : type)
        out += (out.empty() ? "" : " or ") + (t.is_string() ? t.get<std::string>() : t.dump());
    return out;
}

// Whether `schema` takes only true or false, null aside: such a parameter, as one with an enum, lists its values.
bool lists_its_values(const json& schema)
{
    if (schema.contains("enum"))
        return true;
    const json type = schema.value("type", json());
    if (type.is_string())
        return type == "boolean";
    if (!type.is_array() || type.empty())
        return false;
    return std::all_of(type.begin(), type.end(), [](const json& t) { return t == "boolean" || t == "null"; });
}

// Whether `schema` has fields of its own: an object's properties, or its items'.
bool has_fields(const json& schema)
{
    const auto items = schema.find("items");
    return schema.contains("properties") || (items != schema.end() && items->is_object() && items->contains("properties"));
}

// Builds the manual's parameters of one command.
struct ManualBuilder
{
    const NativeCommand& command;
    bool                 classify;
    bool                 complete = true;

    // The value `X` of "Type X:" or "Target X:" that starts `description`, when it is one of the command's own `type`
    // or `target` values: the parameter goes with it.
    json goes_with(const std::string& description) const
    {
        for (const char* arg : {"type", "target"}) {
            std::string lead = arg;
            lead[0]          = char(std::toupper(static_cast<unsigned char>(lead[0])));
            lead += ' ';
            if (description.rfind(lead, 0) != 0)
                continue;
            const std::size_t colon = description.find(':', lead.size());
            if (colon == std::string::npos)
                continue;
            const std::string value = description.substr(lead.size(), colon - lead.size());
            const json        own   = command.input_schema.value("properties", json::object()).value(arg, json::object());
            const json        values = own.value("enum", json::array());
            if (std::find(values.begin(), values.end(), value) != values.end())
                return {{arg, value}};
        }
        return nullptr;
    }

    // The shape of `schema`, the parameter at `path`, required as `required` says (null for an array's items or the
    // values of other keys). `classified`: whether `path` names a parameter parameter_class() reads, false for the
    // values of other keys and inside a plugin operation.
    json shape(const json& schema, const std::string& path, const json& required, bool classified)
    {
        json out = json::object();
        if (!schema.is_object()) {
            // A boolean subschema takes anything or nothing.
            out["schema"] = schema;
            complete      = false;
            return out;
        }
        static const char* const kExpressed[] = {"type",      "description", "default",   "enum",     "minimum",
                                                 "maximum",   "exclusiveMinimum", "minItems", "maxItems", "minLength",
                                                 "maxLength", "pattern",     "format",    "items",    "properties",
                                                 "required",  "additionalProperties"};
        bool raw = false;
        for (const auto& [key, value] : schema.items()) {
            const bool expressed = std::find(std::begin(kExpressed), std::end(kExpressed), key) != std::end(kExpressed);
            // A draft-04 exclusiveMinimum is a boolean that qualifies minimum.
            if (!expressed || (key == "exclusiveMinimum" && !value.is_number()) ||
                (key == "type" && !value.is_string() && !value.is_array()))
                raw = true;
        }
        if (!required.is_null())
            out["required"] = required;
        if (const auto type = schema.find("type"); type != schema.end() && (type->is_string() || type->is_array()))
            out["type"] = type_text(*type);
        const std::string description = schema.value("description", std::string());
        if (schema.contains("description"))
            out["description"] = description;
        if (schema.contains("default"))
            out["default"] = schema["default"];
        if (schema.contains("enum"))
            out["values"] = schema["enum"];
        static const std::pair<const char*, const char*> kBounds[] = {
            {"minimum", "min"},           {"maximum", "max"},           {"minItems", "min_items"}, {"maxItems", "max_items"},
            {"minLength", "min_length"},  {"maxLength", "max_length"},  {"pattern", "pattern"},    {"format", "format"}};
        for (const auto& [from, to] : kBounds)
            if (schema.contains(from))
                out[to] = schema[from];
        if (const auto excl = schema.find("exclusiveMinimum"); excl != schema.end() && excl->is_number())
            out["exclusive_min"] = *excl;
        if (json with = goes_with(description); !with.is_null())
            out["goes_with"] = std::move(with);
        if (const auto props = schema.find("properties"); props != schema.end()) {
            if (props->is_object())
                out["properties"] = fields(*props, schema.value("required", json::array()), path, classified);
            else
                raw = true;
        }
        if (const auto other = schema.find("additionalProperties"); other != schema.end()) {
            if (other->is_boolean()) {
                if (!other->get<bool>())
                    out["other_keys"] = false;
            } else
                out["other_keys"] = shape(*other, path, nullptr, false);
        }
        if (const auto items = schema.find("items"); items != schema.end())
            out["items"] = shape(*items, path + "[]", nullptr, classified);
        // A parameter without fields names where its values come from, unless it lists them itself. An array's items
        // are read through the array, which is the parameter.
        if (classified && required != nullptr && !has_fields(schema) && !lists_its_values(schema))
            if (const std::optional<ParameterClass> c = parameter_class(command.key, path); c && c->kind == ParameterClass::Source)
                out["values_from"] = c->tools;
        if (raw) {
            out["schema"] = schema;
            complete      = false;
        }
        return out;
    }

    // The parameters of the object whose properties are `props` and whose required are `required`, at `path` (empty
    // at the top, "<path>[]" for an array's items).
    json fields(const json& props, const json& required, const std::string& path, bool classified)
    {
        json out = json::array();
        for (const auto& [name, schema] : props.items()) {
            const bool needed = required.is_array() && std::find(required.begin(), required.end(), name) != required.end();
            json       p      = shape(schema, path.empty() ? name : path + "." + name, needed, classified);
            p["name"]         = name;
            out.push_back(std::move(p));
        }
        return out;
    }
};

// A list of names as a sentence's subject: "a", "a and b", "a, b and c".
std::string names_text(const json& names)
{
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i)
        out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i].get<std::string>();
    return out;
}

// The menu items that run `command`: its own Speed Dial item first, then the items folded into it with their arguments.
json menu_items(const NativeCommand& command)
{
    json out = json::array();
    if (command.has_own_action)
        out.push_back({{"title", command.title},
                       {"section", command.action_group.empty() ? command.group : command.action_group},
                       {"args", json::object()}});
    if (command.actions.is_array())
        for (const json& action : command.actions) {
            json item{{"title", action.value("title", std::string())},
                      {"section", action.value("group", command.group)},
                      {"args", action.value("args", json::object())}};
            if (const std::string input = action.value("input", std::string()); !input.empty())
                item["asks_for"] = {{"value", input}, {"argument", action.value("input_arg", input)}};
            out.push_back(std::move(item));
        }
    return out;
}

} // namespace

std::optional<ParameterClass> parameter_class(const std::string& command, const std::string& path)
{
    if (const auto it = classes_by_path().find(command + "." + path); it != classes_by_path().end())
        return it->second;
    const std::size_t dot  = path.rfind('.');
    const std::string name = dot == std::string::npos ? path : path.substr(dot + 1);
    if (const auto it = classes_by_name().find(name); it != classes_by_name().end())
        return it->second;
    return std::nullopt;
}

json command_manual(const NativeCommand& command)
{
    ManualBuilder builder{command, !is_plugin_operation(command)};
    const json&   schema = command.input_schema;
    json          rules  = json::array();
    json          params = json::array();
    if (!schema.is_object() || (schema.contains("type") && schema["type"] != "object")) {
        builder.complete = false;
    } else {
        json required = schema.value("required", json::array());
        // with_silent()'s two shapes: what the silent form requires once the call is not a person's.
        std::map<std::string, std::string> conditional;
        const auto cond = schema.find("if"), then = schema.find("then");
        if (cond != schema.end() || then != schema.end()) {
            const json needs = then != schema.end() && then->is_object() ? then->value("required", json()) : json();
            const bool shaped = cond != schema.end() && needs.is_array() && !needs.empty() && then->size() == 1;
            if (shaped && *cond == json{{"minProperties", 1}}) {
                rules.push_back("With any argument, " + names_text(needs) + (needs.size() == 1 ? " is" : " are") + " required.");
                for (const json& n : needs)
                    conditional[n.get<std::string>()] = "when any argument is given";
            } else if (const json forms = shaped && cond->size() == 1 && cond->contains("not") && (*cond)["not"].is_object() &&
                                                  (*cond)["not"].size() == 1 ?
                                              (*cond)["not"].value("enum", json()) :
                                              json();
                       forms.is_array()) {
                std::string listed;
                for (std::size_t i = 0; i < forms.size(); ++i)
                    listed += (i == 0 ? "" : i + 1 == forms.size() ? " or " : ", ") + forms[i].dump();
                rules.push_back("Unless the arguments are exactly " + listed + ", " + names_text(needs) +
                                (needs.size() == 1 ? " is" : " are") + " required.");
                for (const json& n : needs)
                    conditional[n.get<std::string>()] = "unless the arguments are exactly one of the forms rules gives";
            } else
                builder.complete = false;
        }
        for (const auto& [key, value] : schema.items())
            if (key != "type" && key != "properties" && key != "required" && key != "additionalProperties" && key != "if" &&
                key != "then" && key != "description")
                builder.complete = false;
        if (const auto props = schema.find("properties"); props != schema.end() && props->is_object()) {
            params = builder.fields(*props, required, std::string(), builder.classify);
            for (json& p : params)
                if (p.value("required", false) == false)
                    if (const auto c = conditional.find(p["name"].get<std::string>()); c != conditional.end())
                        p["required"] = c->second;
        }
        if (const auto other = schema.find("additionalProperties"); other != schema.end()) {
            if (*other == false)
                rules.push_back("No other arguments are accepted.");
            else if (!other->is_boolean())
                builder.complete = false;
        }
    }
    json out{{"name", command.key},
             {"title", command.title},
             {"group", command.group},
             {"listed", command.listed},
             {"call", command.listed ? command.key + " {args}" : "tool_call {tool: \"" + command.key + "\", args}"},
             {"access",
              {{"read_only", command.read_only},
               {"destructive", command.destructive},
               {"idempotent", command.idempotent},
               {"open_world", command.open_world}}},
             {"description", command.description},
             {"parameters", std::move(params)},
             {"rules", std::move(rules)},
             {"menu_items", menu_items(command)},
             {"examples", command.examples.is_null() ? json::array() : command.examples},
             {"complete", builder.complete}};
    if (!command.heading.empty())
        out["heading"] = command.heading;
    if (!builder.complete)
        out["input_schema"] = schema;
    return out;
}

json describe_command(const CommandCatalog& catalog, const std::string& name)
{
    if (const std::shared_ptr<const NativeCommand> def = catalog.lookup(name))
        return command_manual(*def);
    throw ToolFailure(catalog.unknown_command(name),
                      {{"code", catalog.unknown_command_code(name)}, {"did_you_mean", catalog.similar(name)}});
}

void register_plugin_operations(CommandCatalog& catalog)
{
    // Plugin operations resolve by name, beside the catalog's commands, without being listed.
    catalog.add_source(std::make_unique<PluginOperationSource>());
}

void register_api_tools(CommandCatalog& catalog, const std::string& design_index)
{
    // Its description ends with `design_index`: the Design commands' names, in a catalog that holds them.
    catalog.add(inspect_tool(
        "tool_search", L("Find tools, actions, settings and APIs"),
        "Find what does something, in one search, or look up one command. With query: the commands that match, best "
        "first, each with its group, the first sentence of its description and listed, whether the tool list shows it; "
        "one that is not listed (a menu item that takes no arguments, a Design tab command, or an operation a plugin "
        "offers, plugin.<plugin>.<name>) comes with its input schema, and its description when that says more, which "
        "nothing else shows. Beside them, the best few matches of each other catalog, with its total and how to use it: "
        "actions (the Speed Dial's plates, recent projects and plugin scripts and pages), settings, and python (the orca "
        "API a script calls). The reply says how to call each kind of match. With tool: that command's description and "
        "input schema, and its examples when it has some (the Design commands do). With group alone: that group's commands, "
        "each with the first sentence of its description, listed and, for a Design command, its heading. With no tool, "
        "query or group: every command's name. With a query, group keeps the command matches to one group; the other catalogs' "
        "matches stay as they are." +
            design_index,
        object_schema({{"tool", prop("string", "A command's name: its description and schema.")},
                       {"query", prop("string", "A few words saying what you want to do or find, e.g. \"brim\" or \"mesh normals\".")},
                       {"group", prop("string", "Only the commands of this group, its whole name in any case: Tools and "
                                                "actions; The app; The project; Objects; Parts; Plates; Presets and settings; "
                                                "Filaments; Printers and prints; Plugins, with the operations plugins offer; View "
                                                "and windows; Recordings, replays and scripts; Driving the GUI; or Design (the "
                                                "Design tab's, with the CAD feature on). A command outside Design is in the group "
                                                "of the place its name starts with (object_delete in Objects), but for "
                                                "window_screenshot and window_save_screenshot, which are in Recordings, replays "
                                                "and scripts. A group no command has is refused with the groups there are.")},
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

    catalog.add(inspect_tool(
        "tool_describe", L("Describe a tool"),
        "One command's manual, listed or not: a tool, a Design tab command, a menu item that takes no arguments, or an "
        "operation a plugin offers (plugin.<plugin>.<name>). It gives the description; every parameter, at every depth, "
        "with its type, whether the schema requires it, its default, its allowed values or range, the type or target it "
        "goes with, and values_from, the tools that list a value the schema can't; the schema's rules as sentences; the "
        "Speed Dial items that run it, with their arguments; its examples; how to call it; and access, its read-only, "
        "destructive, idempotent and open-world labels. What a handler requires or supplies beyond the schema is in the "
        "description. A part of a schema it can't express is kept raw: complete is false and input_schema is the whole schema.",
        object_schema({{"tool", prop("string", "A command's name.")}}, {"tool"}),
        [](const json& args, ToolContext&) {
            return ToolResult::ok(describe_command(NativeCommands::instance(), arg_string(args, "tool")));
        }));

    // Dispatched before it runs (resolve_call): the call runs as the target's own, with its guard,
    // mode, budget and journal flag, and is journaled as the target's call. This handler is never reached.
    NativeCommand call = mutate_tool(
        "tool_call", L("Call a tool or plugin operation"),
        "Call any command by name with its arguments: a tool, or one the tool list does not show, a menu item that takes no "
        "arguments, a Design tab command or an operation a plugin offers (see tool_search). It runs and is recorded as "
        "that command's own call, with its guards and reply budget.",
        object_schema({{"tool", prop("string", "The command to call.")},
                       {"args", {{"type", "object"}, {"description", "Its arguments."}}}},
                      {"tool"}),
        [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("tool_call is dispatched to its tool before it runs"); });
    // It calls any tool, script_run included, and plugin code.
    call.open_world = true;
    catalog.add(call);
}

}}} // namespace Slic3r::GUI::Mcp
