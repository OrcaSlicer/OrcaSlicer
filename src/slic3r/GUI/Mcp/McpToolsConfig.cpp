// list_presets, select_preset, get_settings and set_settings: presets are selected through the
// same selectors a person uses, and settings change through the Tab's own transaction (strict
// parsing, on_change callbacks, rollback on failure), so the UI and the project stay consistent.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/event.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

Preset::Type preset_type(const std::string& name)
{
    if (name == "process" || name == "print")
        return Preset::TYPE_PRINT;
    if (name == "filament")
        return Preset::TYPE_FILAMENT;
    if (name == "printer")
        return Preset::TYPE_PRINTER;
    throw ToolFailure("Unknown preset type " + name + " (printer, process, filament)");
}

PresetCollection& collection(Preset::Type type)
{
    PresetBundle& b = *wxGetApp().preset_bundle;
    switch (type) {
    case Preset::TYPE_PRINT: return b.prints;
    case Preset::TYPE_FILAMENT: return b.filaments;
    default: return b.printers;
    }
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

const char* mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comAdvanced: return "advanced";
    case comExpert: return "expert";
    case comDevelop: return "develop";
    default: return "simple";
    }
}

const char* type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat: return "float";
    case coFloats: return "floats";
    case coInt: return "int";
    case coInts: return "ints";
    case coString: return "string";
    case coStrings: return "strings";
    case coPercent: return "percent";
    case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent";
    case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point";
    case coPoints: return "points";
    case coPoint3: return "point3";
    case coBool: return "bool";
    case coBools: return "bools";
    case coEnum: return "enum";
    case coEnums: return "enums";
    default: return "other";
    }
}

json option_meta(const ConfigOptionDef& def)
{
    json meta{{"label", def.full_label.empty() ? def.label : def.full_label},
              {"type", type_name(def.type)},
              {"mode", mode_name(def.mode)}};
    if (!def.category.empty())
        meta["category"] = def.category;
    if (!def.sidetext.empty())
        meta["unit"] = def.sidetext;
    if (!def.tooltip.empty())
        meta["tooltip"] = def.tooltip.size() > 300 ? def.tooltip.substr(0, 300) + "..." : def.tooltip;
    if (def.min > -FLT_MAX && def.min != -FLT_MAX)
        meta["min"] = def.min;
    if (def.max < FLT_MAX && def.max != FLT_MAX)
        meta["max"] = def.max;
    if (!def.enum_values.empty())
        meta["values"] = def.enum_values;
    return meta;
}


// Option keys whose key or label contains `query`, for "did you mean" and for get_settings' query.
std::vector<std::string> search_keys(const std::string& query, const std::vector<std::string>& within, std::size_t limit)
{
    const std::string q = lower(query);
    std::vector<std::string> out;
    for (const std::string& key : within) {
        const ConfigOptionDef* def = print_config_def.get(key);
        const bool             hit = lower(key).find(q) != std::string::npos ||
                         (def != nullptr && (lower(def->label).find(q) != std::string::npos || lower(def->full_label).find(q) != std::string::npos));
        if (hit)
            out.push_back(key);
        if (out.size() >= limit)
            break;
    }
    return out;
}

struct ConfigTarget
{
    Preset::Type        type = Preset::TYPE_INVALID; // preset scopes
    ModelObject*        object = nullptr;            // object/volume scopes
    ModelVolume*        volume = nullptr;
    int                 object_index = -1;
    const DynamicPrintConfig* config = nullptr;
    std::vector<std::string>  options; // keys valid in this scope
};

// Resolves the scope arguments to the config they read and write. UI thread.
ConfigTarget resolve_target(const json& args, bool for_write)
{
    const std::string scope = arg_string(args, "scope", "print");
    PresetBundle&     bundle = *wxGetApp().preset_bundle;
    ConfigTarget      t;
    if (scope == "object" || scope == "volume") {
        Plater&     plater = ui_plater();
        const int   idx    = arg_int(args, "object", -1);
        ModelObjectPtrs& objects = plater.model().objects;
        if (idx < 0 || idx >= int(objects.size()))
            throw ToolFailure("object must be an index from list_objects (0.." + std::to_string(int(objects.size()) - 1) + ")");
        t.object       = objects[size_t(idx)];
        t.object_index = idx;
        if (scope == "volume") {
            const int v = arg_int(args, "volume", -1);
            if (v < 0 || v >= int(t.object->volumes.size()))
                throw ToolFailure("volume must be a part index from list_objects");
            t.volume = t.object->volumes[size_t(v)];
            t.config = &t.volume->config.get();
        } else {
            t.config = &t.object->config.get();
        }
        // Per-object overrides take print (process) settings.
        t.options = Preset::print_options();
        return t;
    }
    t.type    = preset_type(scope);
    t.options = t.type == Preset::TYPE_PRINT ? Preset::print_options() :
                t.type == Preset::TYPE_FILAMENT ? Preset::filament_options() : Preset::printer_options();
    if (t.type == Preset::TYPE_FILAMENT) {
        const int slot = arg_int(args, "filament_slot", 1);
        if (slot < 1 || slot > int(bundle.filament_presets.size()))
            throw ToolFailure("filament_slot must be 1.." + std::to_string(bundle.filament_presets.size()));
        const std::string& name = bundle.filament_presets[size_t(slot - 1)];
        Tab*               tab  = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
        if (for_write && tab != nullptr && bundle.filaments.get_edited_preset().name != name) {
            // The filament tab edits one preset at a time; switch it to this slot's preset first,
            // through the normal flow (which asks about unsaved changes).
            if (!tab->select_preset(name))
                throw ToolFailure("Switching the filament tab to \"" + name + "\" was cancelled");
            if (TabPresetComboBox* combo = tab->get_combo_box())
                combo->set_filament_idx(slot - 1);
        }
        if (bundle.filaments.get_edited_preset().name == name)
            t.config = &bundle.filaments.get_edited_preset().config;
        else if (const Preset* p = bundle.filaments.find_preset(name))
            t.config = &p->config;
        else
            throw ToolFailure("Filament slot " + std::to_string(slot) + " has no preset");
        return t;
    }
    t.config = &collection(t.type).get_edited_preset().config;
    return t;
}

json list_presets(const json& args)
{
    const Preset::Type type               = preset_type(arg_string(args, "type"));
    const bool         include_incompatible = arg_bool(args, "include_incompatible", false);
    const bool         include_hidden     = arg_bool(args, "include_hidden", false);
    const std::string  query              = lower(arg_string(args, "query"));
    PresetCollection&  presets            = collection(type);
    const std::string  selected           = presets.get_selected_preset_name();
    PresetBundle&      bundle             = *wxGetApp().preset_bundle;
    json               items              = json::array();
    for (size_t i = 0; i < presets.size(); ++i) {
        const Preset& p = presets.preset(i);
        if (p.is_default && presets.size() > 1)
            continue;
        if (!include_incompatible && !p.is_compatible)
            continue;
        if (!include_hidden && !p.is_visible)
            continue;
        if (!query.empty() && lower(p.name).find(query) == std::string::npos && lower(p.alias).find(query) == std::string::npos)
            continue;
        json j{{"name", p.name}, {"system", p.is_system}, {"user", p.is_user()}};
        if (!p.alias.empty() && p.alias != p.name)
            j["alias"] = p.alias;
        if (!p.is_compatible)
            j["compatible"] = false;
        if (!p.is_visible)
            j["visible"] = false;
        if (p.name == selected) {
            j["selected"] = true;
            if (presets.current_is_dirty())
                j["modified"] = true;
        }
        if (type == Preset::TYPE_FILAMENT) {
            json slots = json::array();
            for (size_t s = 0; s < bundle.filament_presets.size(); ++s)
                if (bundle.filament_presets[s] == p.name)
                    slots.push_back(int(s + 1));
            if (!slots.empty())
                j["filament_slots"] = std::move(slots);
        }
        items.push_back(std::move(j));
    }
    return page(items, args, 100);
}

} // namespace

std::string to_config_string(const json& v, const std::string& key)
{
    if (v.is_array() && !key.empty())
        if (const ConfigOptionDef* def = print_config_def.get(key); def != nullptr && def->type == coStrings) {
            std::vector<std::string> values;
            for (const json& e : v)
                values.push_back(to_config_string(e));
            return ConfigOptionStrings(values).serialize();
        }
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer() || v.is_number_unsigned())
        return std::to_string(v.get<long long>());
    if (v.is_number()) {
        std::ostringstream out;
        out.precision(15);
        out << v.get<double>();
        return out.str();
    }
    if (v.is_array()) {
        std::string out;
        for (const json& e : v) {
            if (!out.empty())
                out += ",";
            out += to_config_string(e);
        }
        return out;
    }
    throw ToolFailure("Unsupported value " + v.dump());
}

void register_config_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();
    const json    scope_prop = enum_prop({"print", "process", "filament", "printer", "object", "volume"},
                                         "Which settings: the process (print), filament or printer preset being edited, or an "
                                         "object's / part's own overrides.");

    reg.add(inspect_tool(
        "list_presets", "List presets",
        "Printer, process or filament presets with flags: system or user, selected (and modified), compatible with the "
        "selected printer, and for filaments the slots using them. Only compatible, visible presets unless asked.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"query", prop("string", "Case-insensitive part of the name.")},
                       {"include_incompatible", prop("boolean", "Also list presets incompatible with the printer.")},
                       {"include_hidden", prop("boolean", "Also list hidden presets.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}},
                      {"type"}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return list_presets(args); })); }));

    reg.add(mutate_tool(
        "select_preset", "Select a preset",
        "Select a printer, process or filament preset by the name its selector shows (or its internal name), as a person "
        "would - which may open the unsaved-changes dialog (the reply is then blocked_by_dialog). A filament is assigned to "
        "filament_slot (1-based, default 1).",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Preset name as list_presets shows it.")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}}},
                      {"type", "name"}),
        [](const json& args, ToolContext& ctx) {
            const Preset::Type type = preset_type(arg_string(args, "type"));
            const std::string  name = arg_string(args, "name");
            json result = ctx.run_ui([&]() -> json {
                ui_plater();
                PresetBundle& bundle = *wxGetApp().preset_bundle;
                if (type == Preset::TYPE_FILAMENT) {
                    const int slot = arg_int(args, "filament_slot", 1);
                    auto&     combos = wxGetApp().plater()->sidebar().combos_filament();
                    if (slot < 1 || slot > int(combos.size()) || combos[size_t(slot - 1)] == nullptr)
                        throw ToolFailure("filament_slot must be 1.." + std::to_string(combos.size()));
                    PlaterPresetComboBox* combo = combos[size_t(slot - 1)];
                    const auto            match = combo->find_by_name(name);
                    if (match.ambiguous)
                        throw ToolFailure("\"" + name + "\" is ambiguous in filament slot " + std::to_string(slot) + "'s selector; use the internal name");
                    if (match.item < 0)
                        throw ToolFailure("Filament slot " + std::to_string(slot) + " does not offer \"" + name + "\" (see list_presets)");
                    // The sidebar's own selection path: mapping, flush volumes, badges, project state.
                    combo->SetSelection(match.item);
                    wxCommandEvent evt(wxEVT_COMBOBOX, combo->GetId());
                    evt.SetEventObject(combo);
                    evt.SetInt(match.item);
                    evt.SetString(combo->GetString(match.item));
                    combo->GetEventHandler()->ProcessEvent(evt);
                    return {{"type", "filament"}, {"filament_slot", slot}, {"selected", bundle.filament_presets[size_t(slot - 1)]}};
                }
                Tab* tab = wxGetApp().get_tab(type);
                if (tab == nullptr)
                    throw ToolFailure("The settings tab is not available");
                std::string target = name;
                if (TabPresetComboBox* combo = tab->get_combo_box(); combo != nullptr && combo->GetCount() > 0) {
                    const auto match = combo->find_by_name(name);
                    if (match.ambiguous)
                        throw ToolFailure("\"" + name + "\" is ambiguous; use the internal name");
                    if (match.item < 0)
                        throw ToolFailure("The selector does not offer \"" + name + "\" (see list_presets)");
                    target = match.internal_name;
                }
                if (!tab->select_preset(target))
                    throw ToolFailure("Selecting \"" + target + "\" was cancelled");
                return {{"type", arg_string(args, "type")}, {"selected", collection(type).get_selected_preset_name()}};
            });
            settle(ctx);
            return ToolResult::ok(result);
        }));

    reg.add(inspect_tool(
        "get_settings", "Read settings",
        "Read setting values (as OrcaSlicer serializes them) with label, type, unit, range, allowed values and mode. Name "
        "keys, or search with query (matches key or label); without either, every option of the scope is listed, paged. "
        "For object/volume scopes only the overrides that object or part carries are listed.",
        object_schema({{"scope", scope_prop},
                       {"keys", array_prop({{"type", "string"}}, "Option keys, e.g. [\"layer_height\", \"wall_loops\"].")},
                       {"query", prop("string", "Search keys and labels.")},
                       {"object", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (object/volume scope)."}}},
                       {"volume", {{"type", "integer"}, {"minimum", 0}, {"description", "Part index (volume scope)."}}},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}},
                       {"include_meta", prop("boolean", "Include labels, types and ranges. Default true.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                const ConfigTarget t     = resolve_target(args, false);
                const bool         meta  = arg_bool(args, "include_meta", true);
                std::vector<std::string> keys;
                if (args.contains("keys")) {
                    for (const json& k : args["keys"])
                        keys.push_back(k.get<std::string>());
                } else if (args.contains("query")) {
                    keys = search_keys(arg_string(args, "query"), t.options, 200);
                } else if (t.object != nullptr) {
                    keys = t.config->keys();
                } else {
                    keys = t.options;
                }
                json items   = json::array();
                json unknown = json::array();
                for (const std::string& key : keys) {
                    const ConfigOptionDef* def = print_config_def.get(key);
                    if (def == nullptr || std::find(t.options.begin(), t.options.end(), key) == t.options.end()) {
                        unknown.push_back(key);
                        continue;
                    }
                    json item{{"key", key}};
                    if (t.config->has(key))
                        item["value"] = t.config->opt_serialize(key);
                    else
                        item["value"] = nullptr; // not overridden at this level
                    if (meta)
                        item["meta"] = option_meta(*def);
                    items.push_back(std::move(item));
                }
                json out = page(items, args, 100);
                if (!unknown.empty())
                    out["unknown_keys"] = unknown;
                return out;
            }));
        }));

    reg.add(mutate_tool(
        "set_settings", "Change settings",
        "Change settings as if typed into the settings tab: all values are parsed strictly first and applied together, and "
        "nothing changes if any is invalid. Values may be strings in OrcaSlicer's format (\"15%\", \"0.2\", \"1,2\") or JSON "
        "numbers, booleans and arrays. object/volume scopes set per-object or per-part overrides; a null value removes an "
        "override.",
        object_schema({{"scope", scope_prop},
                       {"values", {{"type", "object"}, {"description", "{key: value}"}}},
                       {"object", {{"type", "integer"}, {"minimum", 0}}},
                       {"volume", {{"type", "integer"}, {"minimum", 0}}},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}}}},
                      {"values"}),
        [](const json& args, ToolContext& ctx) {
            if (!args["values"].is_object() || args["values"].empty())
                throw ToolFailure("values must be a non-empty object {key: value}");
            json result = ctx.run_ui([&]() -> json {
                ui_plater();
                const ConfigTarget t = resolve_target(args, true);
                std::vector<std::pair<std::string, std::string>> values;
                std::vector<std::string>                         removals;
                for (auto it = args["values"].begin(); it != args["values"].end(); ++it) {
                    if (std::find(t.options.begin(), t.options.end(), it.key()) == t.options.end())
                        throw ToolFailure("Unknown setting \"" + it.key() + "\" for this scope",
                                          {{"similar", search_keys(it.key().substr(0, std::max<std::size_t>(3, it.key().size() / 2)), t.options, 5)}});
                    if (it.value().is_null()) {
                        if (t.object == nullptr)
                            throw ToolFailure("null removes an object/part override; preset settings always have a value");
                        removals.push_back(it.key());
                    } else {
                        values.emplace_back(it.key(), to_config_string(it.value(), it.key()));
                    }
                }
                if (t.object == nullptr) {
                    Tab* tab = wxGetApp().get_tab(t.type);
                    if (tab == nullptr)
                        throw ToolFailure("The settings tab is not available");
                    try {
                        tab->apply_preset_config_values(values);
                    } catch (const std::exception& e) {
                        throw ToolFailure(e.what());
                    }
                } else {
                    // Strict-parse everything into a scratch config before touching the model.
                    DynamicPrintConfig scratch;
                    for (const auto& [key, value] : values)
                        try {
                            scratch.set_deserialize_strict(key, value);
                        } catch (const std::exception& e) {
                            throw ToolFailure("Invalid value for " + key + ": " + e.what());
                        }
                    Plater& plater = ui_plater();
                    plater.take_snapshot("Change object settings");
                    ModelConfig& cfg = t.volume != nullptr ? static_cast<ModelConfig&>(t.volume->config) : static_cast<ModelConfig&>(t.object->config);
                    for (const auto& [key, value] : values)
                        cfg.set_key_value(key, scratch.option(key)->clone());
                    for (const std::string& key : removals)
                        cfg.erase(key);
                    wxGetApp().obj_list()->object_config_options_changed(ObjectVolumeID{t.object, t.volume});
                    plater.changed_object(t.object_index);
                }
                const ConfigTarget after = resolve_target(args, false);
                json applied = json::object();
                for (const auto& [key, value] : values)
                    applied[key] = after.config->has(key) ? json(after.config->opt_serialize(key)) : json();
                for (const std::string& key : removals)
                    applied[key] = nullptr;
                return {{"applied", applied}};
            });
            settle(ctx);
            return ToolResult::ok(result);
        }));
}

}}} // namespace Slic3r::GUI::Mcp
