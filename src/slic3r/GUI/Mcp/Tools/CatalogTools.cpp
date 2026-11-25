// The place Tools and actions: tool_list, tool_describe and tool_call, a way to every command of the catalog, those
// tools/list does not show among them: the menu items that take no arguments, the Design tab's commands and the
// operations plugins offer (plugin.<key>.<name>). tool_list lists those, tool_describe gives any command's manual,
// built from its schema, and tool_call runs any of them. action_list and action_run: every Speed Dial action
// (built-in commands, plates, recent projects, plugin scripts and pages, and on request settings) is listed and run
// through these two.

#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpPersonContext.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/ActionIds.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginOperations.hpp"

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

// What a plugin operation's command description starts with, before the operation's own description.
constexpr const char* kOfferedByPlugin = "Offered by a plugin. ";

// The longest description tool_list gives whole; a longer one is given by its first sentence.
constexpr std::size_t kWholeDescription = 300;

// A plugin's operation as a command: a job (plugin code may take long), under the mutate guard and journaled
// when it changes something, under the inspect guard and unjournaled when it only reads. Not listed; in the group
// Plugins, the place of the commands that manage the plugins.
std::shared_ptr<const NativeCommand> operation_command(const PluginOperation& op)
{
    auto def          = std::make_shared<NativeCommand>();
    def->key          = op.full_name();
    def->title        = op.name;
    def->group        = "Plugins";
    def->description  = kOfferedByPlugin + op.description;
    def->input_schema = op.schema;
    def->read_only    = !op.changes;
    def->guard        = op.changes ? GuardClass::Mutate : GuardClass::Inspect;
    def->journal      = op.changes;
    // Plugin code runs under its plugin's grants, which may reach files, programs and the network.
    def->open_world   = true;
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
// call finds one by name, and tool_list lists them; tools/list does not.
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

json unlisted_commands(const CommandCatalog& catalog, const std::string& group)
{
    std::vector<std::shared_ptr<const NativeCommand>> all;
    for (const NativeCommand& def : catalog.commands())
        all.emplace_back(std::shared_ptr<const NativeCommand>(), &def); // the catalog owns it
    for (std::shared_ptr<const NativeCommand>& def : catalog.source_commands())
        all.push_back(std::move(def));
    // Every group the catalog's commands are in, listed or not, in the order they first appear: a name is a group
    // whether or not the tool list shows all its commands.
    std::vector<std::string> groups;
    for (const auto& def : all)
        if (std::find(groups.begin(), groups.end(), def->group) == groups.end())
            groups.push_back(def->group);
    std::string chosen;
    if (!group.empty()) {
        const auto it = std::find_if(groups.begin(), groups.end(), [&group](const std::string& g) { return boost::algorithm::iequals(g, group); });
        if (it == groups.end()) {
            // The Design commands left out of this run, which a call of any of them is refused with too.
            if (boost::algorithm::iequals(group, kDesignGroup))
                if (const std::string* why = catalog.left_out_reason("design_unavailable"))
                    throw ToolFailure(std::string("The Design tab's commands are left out of this run: ") + *why,
                                      {{"code", "design_unavailable"}});
            std::string names;
            for (const std::string& name : groups)
                names += (names.empty() ? "" : ", ") + name;
            throw ToolFailure("Unknown group: " + group + "; the groups are " + names, {{"code", "invalid_argument"}, {"groups", groups}});
        }
        chosen = *it;
    }
    json items = json::array();
    for (const auto& def : all) {
        if (def->listed || (!chosen.empty() && def->group != chosen))
            continue;
        // An operation's own description, after what every operation's starts with.
        std::string text = def->description;
        if (text.rfind(kOfferedByPlugin, 0) == 0)
            text.erase(0, std::char_traits<char>::length(kOfferedByPlugin));
        json item{{"name", def->key}, {"group", def->group}, {"does", text.size() <= kWholeDescription ? text : first_sentence(text)}};
        if (!def->heading.empty())
            item["heading"] = def->heading;
        items.push_back(std::move(item));
    }
    json out{{"items", std::move(items)}, {"use", "tool_describe {tool} gives a command's manual; tool_call {tool, args} runs it"}};
    if (!chosen.empty() && out["items"].empty())
        out["note"] = "tools/list shows every command of " + chosen;
    return out;
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
        {"tool_describe.tool", source({"tools/list", "tool_list"})},
        {"tool_call.tool", source({"tools/list", "tool_list"})},
        {"tool_call.args", source({"tool_describe"})},
        {"action_run.id", source({"action_list"})},
        {"action_run.args", source({"action_list"})},
        {"action_list.section", source({"action_list"})},
        {"script_list_api.within", source({"script_list_api"})},
        {"script_describe_api.name", source({"script_list_api"})},
        {"script_run.args", kFree},
        {"replay_run.vars", kFree},
        // Settings and preferences: the keys a tool takes are those its reader lists.
        {"settings_get.keys", source({"settings_list"})},
        {"settings_set.values", source({"settings_list"})},
        {"settings_list.where", source({"settings_list_pages"})},
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
        {"printer_install.models", source({"printer_list_models"})},
        {"printer_install.models[].vendor", source({"printer_list_models"})},
        {"printer_uninstall.models", source({"printer_list_models"})},
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
        {"dialog_answer.dialog", source({"app_state"})},
        // A sliced plate's layers, by height too.
        {"plate_set_custom_gcode.add[].z", source({"plate_slice_stats"})},
        {"plate_set_custom_gcode.remove[].z", source({"plate_slice_stats"})},
        // The Design tab: the shapes a sketch draws are new; a sketch line as a mirror axis; a hole's or a thread's
        // point may be a body's face or edge; what a feature's card picks; the bodies a boolean takes.
        {"sketch_draw.entities", kFree},
        {"sketch_mirror.axis", source({"sketch_describe"})},
        {"solid_hole.at", source({"body_describe"})},
        {"solid_thread.at", source({"body_describe"})},
        {"feature_edit.refs", source({"feature_describe"})},
        {"solid_boolean.target", source({"design_describe"})},
        {"solid_boolean.tool", source({"design_describe"})},
        {"solid_sweep.path", source({"design_describe"})},
    };
    return rows;
}

// The classes of the names the built-in commands give their parameters, every name but those that only a command's
// own entry classifies (classes_by_path()).
const std::map<std::string, ParameterClass>& classes_by_name()
{
    const ParameterClass objects = source({"object_list"}), plates = source({"plate_list"});
    const ParameterClass slots = source({"filament_list"}), presets = source({"preset_list"});
    const ParameterClass design = source({"design_describe"}), geometry = source({"body_describe"});
    const ParameterClass sketch = source({"sketch_describe"}), layers = source({"plate_slice_stats"});
    static const std::map<std::string, ParameterClass> rows{
        // The project.
        {"object", objects}, {"objects", objects}, {"object_id", objects}, {"object_ids", objects}, {"part", objects},
        {"parts", objects}, {"part_id", objects}, {"part_ids", objects}, {"instance", objects}, {"instances", objects},
        {"instance_id", objects}, {"instance_ids", objects}, {"fill_object", objects}, {"fill_object_id", objects},
        {"order", objects}, {"order_ids", objects}, {"replace", objects}, {"replace_ids", objects},
        {"plate", plates}, {"plates", plates},
        {"filament_slot", slots}, {"filament", slots}, {"components", slots}, {"merge_into", slots},
        {"prime_volume_mode", slots},
        {"layer", layers}, {"layer_range", layers}, {"layer_z", layers},
        // Presets, printer models and plugins.
        {"preset", presets}, {"printer", presets}, {"names", presets}, {"vendor", presets}, {"filament_type", presets},
        {"model", source({"printer_list_models"})}, {"nozzles", source({"printer_list_models"})},
        // A setting's values, which settings_get gives in its meta.
        {"shaper_type", source({"settings_get"})},
        {"plugin", source({"plugin_list"})}, {"capability", source({"plugin_list"})},
        // The app's jobs, dialogs and widgets.
        {"job", source({"app_state"})}, {"button", source({"app_state"})}, {"title", source({"app_state"})},
        {"target", source({"widget_list"})},
        // The Design document: features, bodies, their faces and edges, and a sketch's entities.
        {"body", design}, {"source_body", design}, {"face_body", design}, {"feature", design}, {"before", design},
        {"sketch", design}, {"profiles", design}, {"cs_a", design}, {"cs_b", design}, {"plane_a", design},
        {"plane_b", design}, {"reference", design}, {"base", design},
        {"generation", source({"design_describe", "body_describe"})}, {"plane", source({"design_describe", "body_describe"})},
        {"face", geometry}, {"faces", geometry}, {"face_a", geometry}, {"face_b", geometry}, {"edge", geometry},
        {"edges", geometry}, {"edge_a", geometry}, {"edge_b", geometry}, {"vertex", geometry}, {"up_to_face", geometry},
        {"a", geometry}, {"b", geometry},
        {"entities", sketch}, {"entity", sketch}, {"refs", sketch}, {"ent_a", sketch}, {"ent_b", sketch},
        {"axis_entity", sketch}, {"loop", sketch},
        {"index", source({"sketch_list_constraints"})},
        {"field", source({"feature_describe"})}, {"fields", source({"feature_describe"})},
        // Described: the parameter's own description, or its items' values, lists what it takes.
        {"edge_kinds", kDescribed}, {"kinds", kDescribed}, {"include", kDescribed}, {"legend", kDescribed},
        {"modifiers", kDescribed}, {"tab", kDescribed}, {"group", kDescribed},
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

// Whether every name `schema` requires is one of its properties, which the manual marks required; a name with no
// property is a constraint no parameter carries.
bool requires_only_its_fields(const json& schema)
{
    const auto required = schema.find("required");
    if (required == schema.end())
        return true;
    const auto props = schema.find("properties");
    if (!required->is_array())
        return false;
    return std::all_of(required->begin(), required->end(), [&](const json& name) {
        return name.is_string() && props != schema.end() && props->is_object() && props->contains(name.get<std::string>());
    });
}

// Whether `schema` describes an object, whose other keys the manual then states: its type says so, or it has
// properties.
bool takes_an_object(const json& schema)
{
    const json type = schema.value("type", json());
    return schema.contains("properties") || type == "object" ||
           (type.is_array() && std::find(type.begin(), type.end(), "object") != type.end());
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

    // The value `X` of "Type X:" or "Target X:" that starts `description`, X marked as code or not, when it is one of
    // the command's own `type` or `target` values: the parameter goes with it.
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
            std::string value = description.substr(lead.size(), colon - lead.size());
            if (value.size() > 2 && value.front() == '`' && value.back() == '`')
                value = value.substr(1, value.size() - 2);
            // A plugin's schema may give the property as a boolean subschema, which lists no values.
            const auto props = command.input_schema.find("properties");
            if (props == command.input_schema.end() || !props->is_object())
                continue;
            const auto own = props->find(arg);
            if (own == props->end() || !own->is_object())
                continue;
            const json values = own->value("enum", json::array());
            if (values.is_array() && std::find(values.begin(), values.end(), value) != values.end())
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
        if (!requires_only_its_fields(schema))
            raw = true;
        // What an object takes besides its properties: nothing (false) or values of a shape; anything (true) below.
        const auto other = schema.find("additionalProperties");
        if (other != schema.end() && *other != true)
            out["other_keys"] = *other == false ? json(false) : shape(*other, path, nullptr, false);
        if (const auto items = schema.find("items"); items != schema.end())
            out["items"] = shape(*items, path + "[]", nullptr, classified);
        // A parameter names where its values come from, unless it lists them itself. One with fields is read through
        // them, and names a source besides only where its command's own entry gives one (printer_install's models), as
        // a name such as plane is a reference in one command and a cut plane's fields in another. An array's items are
        // read through the array, which is the parameter.
        if (classified && required != nullptr && !lists_its_values(schema)) {
            std::optional<ParameterClass> c;
            if (!has_fields(schema))
                c = parameter_class(command.key, path);
            else if (const auto row = classes_by_path().find(command.key + "." + path); row != classes_by_path().end())
                c = row->second;
            if (c && c->kind == ParameterClass::Source)
                out["values_from"] = c->tools;
        }
        // An open object takes any other key, unless a keyword kept raw (allOf, $ref) may close it: its schema says.
        if (!raw && !out.contains("other_keys") && (other != schema.end() || takes_an_object(schema)))
            out["other_keys"] = true;
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
        // A top-level description has no place in the manual, whose description is the command's.
        for (const auto& [key, value] : schema.items())
            if (key != "type" && key != "properties" && key != "required" && key != "additionalProperties" && key != "if" &&
                key != "then")
                builder.complete = false;
        if (!requires_only_its_fields(schema))
            builder.complete = false;
        const auto props = schema.find("properties");
        if (props != schema.end() && !props->is_object())
            builder.complete = false;
        // What the top level holds that the manual can't express may close it (allOf, $ref), so an open top level is
        // said to be one only when the rest of it is expressed.
        const bool top_expressed = builder.complete;
        if (props != schema.end() && props->is_object()) {
            params = builder.fields(*props, required, std::string(), builder.classify);
            for (json& p : params)
                if (p.value("required", false) == false)
                    if (const auto c = conditional.find(p["name"].get<std::string>()); c != conditional.end())
                        p["required"] = c->second;
        }
        // A call's other arguments are refused only when the schema closes its top level (validate_value()).
        if (const auto other = schema.find("additionalProperties"); other == schema.end() || *other == true) {
            if (top_expressed)
                rules.push_back("Other arguments are accepted.");
        } else if (*other == false)
            rules.push_back("No other arguments are accepted.");
        else
            builder.complete = false;
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

// What tool_list's group argument takes, from the places' headings in their order and the commands outside their
// first word's place, so that a new heading or exception is named with no edit here.
std::string group_help()
{
    std::string groups;
    for (const std::string& heading : NativeCommands::place_headings())
        groups += heading + "; ";
    std::map<std::string, json> elsewhere;
    for (const auto& [key, heading] : NativeCommands::place_exceptions())
        elsewhere[heading].push_back("`" + key + "`");
    std::string exceptions;
    for (const auto& [heading, keys] : elsewhere)
        exceptions += (exceptions.empty() ? ", but " : "; ") + names_text(keys) + (keys.size() == 1 ? " is in " : " are in ") +
                      heading;
    return "Only this group's commands, its whole name in any case: " + groups +
           "or Design (with the CAD feature on). A command outside Design is in the group of the place its name starts "
           "with (`object_delete` in Objects)" +
           exceptions +
           ". An unknown group is refused with the groups there are, and Design, when this run leaves its commands out, "
           "with the reason.";
}

} // namespace

void register_catalog_tools(CommandCatalog& catalog, const std::string& design_index)
{
    // Its description ends with `design_index`: the Design commands' names, in a catalog that holds them.
    catalog.add(inspect_tool(
        "tool_list", L("List more tools"),
        "The commands the tool list does not show, each with what it does: the Design tab's (group Design, with the CAD "
        "feature on, each under its heading), the menu items that take no arguments (in their places' groups), and the "
        "operations plugins offer (group Plugins, `plugin.<plugin>.<name>`). The reply says how to read a command's manual "
        "and how to run one. `group` keeps one group's; one whose commands the tool list all shows gives none and a note." +
            design_index,
        object_schema({{"group", prop("string", group_help())}}),
        [](const json& args, ToolContext&) {
            return ToolResult::ok(unlisted_commands(NativeCommands::instance(), arg_string(args, "group")));
        }));

    catalog.add(inspect_tool(
        "tool_describe", L("Describe a tool"),
        "One command's manual, listed or not: a tool, a Design tab command, a menu item that takes no arguments, or an "
        "operation a plugin offers (`plugin.<plugin>.<name>`). It gives the description; every parameter, at every depth, "
        "with its type, whether the schema requires it, its default, its allowed values or range, the type or target it "
        "goes with, and `values_from`, the tools that list a value the schema can't; the schema's rules as sentences; the "
        "Speed Dial items that run it, with their arguments; its examples; how to call it; and `access`, its read-only, "
        "destructive, idempotent and open-world labels. What a handler requires or supplies beyond the schema is in the "
        "description. A part of a schema it can't express is kept raw: `complete` is false and `input_schema` is the whole schema.",
        object_schema({{"tool", prop("string", "A command's name.")}}, {"tool"}),
        [](const json& args, ToolContext&) {
            return ToolResult::ok(describe_command(NativeCommands::instance(), arg_string(args, "tool")));
        }));

    // Dispatched before it runs (resolve_call): the call runs as the target's own, with its guard,
    // mode, budget and journal flag, and is journaled as the target's call. This handler is never reached.
    NativeCommand call = mutate_tool(
        "tool_call", L("Call a tool or plugin operation"),
        "Call any command by name with its arguments: a tool, or one the tool list does not show, a menu item that takes no "
        "arguments, a Design tab command or an operation a plugin offers (`tool_list` lists them; `tool_describe` gives any "
        "command's manual). It runs and is recorded as that command's own call, with its guards and reply budget.",
        object_schema({{"tool", prop("string", "The command to call.")},
                       {"args", {{"type", "object"}, {"description", "Its arguments."}}}},
                      {"tool"}),
        [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("tool_call is dispatched to its tool before it runs"); });
    // It calls any tool, script_run included, and plugin code.
    call.open_world = true;
    catalog.add(call);

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
