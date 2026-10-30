// list_presets, select_preset, save_preset, delete_preset, get_settings, set_settings and list_plates: presets
// are selected through the same selectors a person uses, saved and deleted through the tab, and settings change
// through the Tab's own transaction (strict parsing, on_change callbacks, rollback on failure), or for the
// project's and the plates' own settings through the code of their sidebar controls, dialogs and plate icons,
// so the UI and the project stay consistent. Preferences are only read here; they change in the Preferences
// dialog, whose handlers carry their effects.

#include "McpService.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/CreatePresetsDialog.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/SavePresetDialog.hpp"
#include "slic3r/GUI/SettingsIndex.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/WipeTowerDialog.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/FlushVolCalc.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/dataview.h>
#include <wx/event.h>
#include <wx/string.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
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

// The settings of a preset type, as its tab and set_settings take them.
const std::vector<std::string>& preset_options(Preset::Type type)
{
    return type == Preset::TYPE_PRINT ? Preset::print_options() : type == Preset::TYPE_FILAMENT ? Preset::filament_options() : Preset::printer_options();
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

// The "Page > Group" of each setting on the settings tab of `type`, as the tabs register their rows
// when they build their pages; a setting with no row on a page has none.
std::map<std::string, std::string> setting_places(Preset::Type type)
{
    std::map<std::string, std::string> places;
    for (const Search::Option& row : wxGetApp().sidebar().settings_index().all_options())
        if (row.type == type) {
            const std::string key = row.opt_key();
            places.emplace(key.substr(0, key.find('#')), into_u8(row.category) + " > " + into_u8(row.group));
        }
    return places;
}

// Preset names that share the most words with `name`, those compatible with the printer first, for a
// name no preset has.
json similar_presets(const PresetCollection& presets, const std::string& name)
{
    std::vector<std::string> words;
    std::istringstream       in(lower(name));
    for (std::string w; in >> w;)
        words.push_back(w);
    std::vector<std::pair<int, std::string>> scored;
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const Preset& p = presets.preset(i);
        if (p.is_default)
            continue;
        const std::string n       = lower(p.name);
        const int         matched = int(std::count_if(words.begin(), words.end(), [&n](const std::string& w) { return n.find(w) != std::string::npos; }));
        if (matched > 0)
            scored.emplace_back(-(2 * matched + (p.is_compatible ? 1 : 0)), p.name);
    }
    std::sort(scored.begin(), scored.end());
    json out = json::array();
    for (std::size_t i = 0; i < scored.size() && i < 5; ++i)
        out.push_back(scored[i].second);
    return out;
}

struct ConfigTarget
{
    Preset::Type        type = Preset::TYPE_INVALID; // preset scopes
    const Preset*       preset = nullptr;            // the preset read: the one being edited, or a saved one
    bool                edited = false;              // config is the edited preset's, unsaved changes included
    ModelObject*        object = nullptr;            // object/volume scopes
    ModelVolume*        volume = nullptr;
    int                 object_index = -1;
    bool                project = false;             // the project scope, read from `owned`
    std::shared_ptr<const DynamicPrintConfig> owned;
    const DynamicPrintConfig* config = nullptr;
    std::vector<std::string>  options; // keys valid in this scope
};

// The project scope: the project's own settings (PresetBundle::project_config) that a person sets in the
// sidebar, on the canvas or in their dialogs, and the filament slot facts they depend on, which are set
// elsewhere and listed for reading only.
bool project_key_read_only(const std::string& key)
{
    return key == "filament_is_mixed" || key == "filament_map_mode" || key == "filament_map";
}

std::vector<std::string> project_options()
{
    std::vector<std::string> keys{"curr_bed_type",    "filament_colour",   "filament_is_mixed", "flush_volumes_matrix",
                                  "flush_multiplier", "prime_volume_mode", "wipe_tower_x",      "wipe_tower_y"};
    // The filament grouping of a printer with several extruders.
    if (wxGetApp().preset_bundle->get_printer_extruder_count() > 1)
        keys.insert(keys.end(), {"filament_map_mode", "filament_map"});
    return keys;
}

// The project scope's values: project_config's, with the wipe tower position one entry per plate and a
// mixed flag per slot, since a project can store fewer of either. UI thread.
DynamicPrintConfig project_settings()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    DynamicPrintConfig  out;
    for (const std::string& key : project_options())
        if (const ConfigOption* opt = bundle.project_config.option(key))
            out.set_key_value(key, opt->clone());
    const int plates = ui_plater().get_partplate_list().get_plate_count();
    for (const char* key : {"wipe_tower_x", "wipe_tower_y"}) {
        // What the canvas reads for each plate (get_at() takes the first entry for a plate without one).
        ConfigOptionFloats* position = out.option<ConfigOptionFloats>(key, true);
        std::vector<double> per_plate;
        for (int i = 0; i < plates && !position->values.empty(); ++i)
            per_plate.push_back(position->get_at(std::size_t(i)));
        position->values = std::move(per_plate);
    }
    std::vector<unsigned char> mixed;
    for (std::size_t i = 0; i < bundle.project_config.option<ConfigOptionStrings>("filament_colour")->size(); ++i)
        mixed.push_back(bundle.is_mixed_filament(i));
    out.set_key_value("filament_is_mixed", new ConfigOptionBools(std::move(mixed)));
    return out;
}

// The values a project setting's control offers this printer; empty while the control is hidden. UI thread.
std::vector<std::string> project_offered(const std::string& key)
{
    PresetBundle&             bundle  = *wxGetApp().preset_bundle;
    const DynamicPrintConfig& printer = bundle.printers.get_edited_preset().config;
    std::vector<std::string>  out;
    if (key == "curr_bed_type") {
        // The sidebar shows its plate type selector for these printers only (Sidebar::update_all_preset_comboboxes()).
        if (bundle.is_bbl_vendor() || printer.opt_bool("support_multi_bed_types"))
            for (const BedType type : ui_plater().sidebar().get_cur_combox_bed_types())
                out.push_back(print_config_def.get(key)->enum_values[std::size_t(type) - 1]);
    } else if (key == "prime_volume_mode") {
        // The Purge mode button shows for printers with several nozzles per extruder or fast purge
        // (TabPrinter::on_preset_loaded()); its dialog offers Default and Fast, or else Default and Saving.
        const auto* nozzles  = printer.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
        const bool  several  = nozzles != nullptr && std::any_of(nozzles->values.begin(), nozzles->values.end(), [](int v) {
                                 return v > 1 && v != ConfigOptionIntsNullable::nil_value();
                             });
        const auto* fast_opt = printer.option<ConfigOptionBool>("support_fast_purge_mode");
        const bool  fast     = fast_opt != nullptr && fast_opt->value;
        if (several || fast)
            out = {"Default", fast ? "Fast" : "Saving"};
    }
    return out;
}

// set_settings on the project scope. Every key is written by the code its control runs: the plate type
// selector, a colour button's picker, the Flushing volumes and Purge mode dialogs' OK, a wipe tower drag.
// Everything is checked before anything changes. Colours go first, in a closure of their own, so the
// flushing volumes their change recalculates ("Auto flush after changing") are in place before a matrix
// given in the same call replaces them.
ToolResult set_project_settings(const json& args, ToolContext& ctx)
{
    const json values_in = args.value("values", json::object());
    if (values_in.empty())
        throw ToolFailure("values must be a non-empty object {key: value}");
    if (args.contains("reset_all") || args.contains("reset_to"))
        throw ToolFailure("reset_all and reset_to are for preset scopes; project settings always have a value");
    DynamicPrintConfig before;
    DynamicPrintConfig values; // checked, in the form the scope reads them
    // The matrix to write, given whole or as one entry of `basis`'s matrix.
    const auto checked_matrix = [&](const DynamicPrintConfig& basis) {
        const char* const  key = "flush_volumes_matrix";
        DynamicPrintConfig given;
        given.set_deserialize_strict(key, preset_setting_value(key, values_in[key], basis, {}, {}));
        const std::vector<unsigned char>& mixed = before.option<ConfigOptionBools>("filament_is_mixed")->values;
        return check_flush_matrix(given.option<ConfigOptionFloats>(key)->values, mixed.size(),
                                  (values.has("flush_multiplier") ? values : before).option<ConfigOptionFloats>("flush_multiplier")->values,
                                  basis.option<ConfigOptionFloats>(key)->values, mixed);
    };
    ctx.run_ui([&]() -> json {
        ui_plater();
        before                                 = project_settings();
        const std::vector<std::string> options = project_options();
        for (auto it = values_in.begin(); it != values_in.end(); ++it) {
            const std::string& key = it.key();
            if (std::find(options.begin(), options.end(), key) == options.end())
                throw ToolFailure("Unknown setting \"" + key + "\" for the project scope", {{"keys", options}});
            if (key == "filament_is_mixed")
                throw ToolFailure("filament_is_mixed is read-only: mixed filaments are made in the sidebar's mixed filament list");
            if (project_key_read_only(key))
                throw ToolFailure(key + " is read-only here: the Filament grouping dialog sets it for each plate, and every plate keeps "
                                        "its own grouping");
            if (it.value().is_null())
                throw ToolFailure(key + " always has a value; null is for object overrides and preset keys");
            const std::string text = preset_setting_value(key, it.value(), before, {}, {});
            try {
                values.set_deserialize_strict(key, text);
            } catch (const std::exception& e) {
                throw ToolFailure("Invalid value for " + key + ": " + e.what());
            }
        }
        for (const char* key : {"curr_bed_type", "prime_volume_mode"}) {
            if (!values.has(key))
                continue;
            const std::vector<std::string> offered = project_offered(key);
            if (offered.empty())
                throw ToolFailure(std::string(key) + ": this printer has " +
                                  (std::string(key) == "curr_bed_type" ? "a single plate type" : "no purge mode to choose"));
            if (std::find(offered.begin(), offered.end(), values.opt_serialize(key)) == offered.end())
                throw ToolFailure("This printer does not offer " + std::string(key) + " \"" + values.opt_serialize(key) + "\"",
                                  {{"offered", offered}});
        }
        const std::vector<unsigned char>& mixed = before.option<ConfigOptionBools>("filament_is_mixed")->values;
        if (values.has("filament_colour")) {
            auto* colours   = values.option<ConfigOptionStrings>("filament_colour");
            colours->values = check_filament_colours(colours->values, before.option<ConfigOptionStrings>("filament_colour")->values, mixed);
        }
        if (values.has("flush_multiplier")) {
            auto*             multipliers = values.option<ConfigOptionFloats>("flush_multiplier");
            const std::size_t nozzles     = before.option<ConfigOptionFloats>("flush_multiplier")->size();
            multipliers->values           = check_flush_multipliers(multipliers->values, nozzles);
        }
        if (values.has("flush_volumes_matrix"))
            values.option<ConfigOptionFloats>("flush_volumes_matrix")->values = checked_matrix(before);
        for (const char* key : {"wipe_tower_x", "wipe_tower_y"}) {
            if (!values.has(key))
                continue;
            const std::vector<double>& position = values.option<ConfigOptionFloats>(key)->values;
            const std::vector<double>& current  = before.option<ConfigOptionFloats>(key)->values;
            if (position.size() != current.size())
                throw ToolFailure(std::string(key) + " holds one position per plate (" + std::to_string(current.size()) +
                                  "); set one plate's with {\"index\": plate, \"value\": mm}");
            for (std::size_t i = 0; i < position.size(); ++i) {
                const Vec2d  size   = ui_plater().get_partplate_list().get_plate(int(i))->get_size();
                const double extent = std::string(key) == "wipe_tower_x" ? size.x() : size.y();
                if (position[i] != current[i] && (position[i] < 0. || position[i] > extent))
                    throw ToolFailure("The prime tower of plate " + std::to_string(i) + " would be off the plate: " + key + " is 0.." +
                                      to_config_string(json(extent)) + " mm from the plate's origin");
            }
        }
        // A colour button's picker, then its EVT_FILAMENT_COLOR_CHANGED, slot by slot.
        if (values.has("filament_colour")) {
            const std::vector<std::string>& colours = values.option<ConfigOptionStrings>("filament_colour")->values;
            const std::vector<std::string>& current = before.option<ConfigOptionStrings>("filament_colour")->values;
            for (std::size_t i = 0; i < colours.size(); ++i) {
                if (colours[i] == current[i])
                    continue;
                auto& combos = ui_plater().sidebar().combos_filament();
                auto  combo  = std::find_if(combos.begin(), combos.end(),
                                            [i](PlaterPresetComboBox* c) { return c != nullptr && c->get_filament_idx() == int(i); });
                if (combo == combos.end())
                    throw ToolFailure("Filament slot " + std::to_string(i + 1) + " has no colour button");
                (*combo)->sync_colour_config({colours[i]}, false);
                wxCommandEvent* evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
                evt->SetInt(int(i));
                wxQueueEvent(&ui_plater(), evt);
            }
        }
        return json();
    });
    settle(ctx);
    json reply = ctx.run_ui([&]() -> json {
        Plater&       plater = ui_plater();
        PresetBundle& bundle = *wxGetApp().preset_bundle;
        if (values.has("curr_bed_type"))
            plater.sidebar().set_bed_type_accord_combox(values.opt_enum<BedType>("curr_bed_type"));
        // One entry given: the others are as the colour change may just have recalculated them.
        if (values_in.contains("flush_volumes_matrix") && values_in["flush_volumes_matrix"].is_object())
            values.option<ConfigOptionFloats>("flush_volumes_matrix")->values = checked_matrix(project_settings());
        bool update = false;
        if (values.has("flush_volumes_matrix") || values.has("flush_multiplier")) {
            // Without a matrix, the one the colour change may just have recalculated.
            const DynamicPrintConfig& source = values.has("flush_volumes_matrix") ? values : bundle.project_config;
            const DynamicPrintConfig& multipliers = values.has("flush_multiplier") ? values : bundle.project_config;
            apply_flushing_volumes(source.option<ConfigOptionFloats>("flush_volumes_matrix")->values,
                                   multipliers.option<ConfigOptionFloats>("flush_multiplier")->values);
            // What the Flushing volumes button does after its dialog; update() below schedules the
            // background process, as the event the dialog's OK posts does.
            plater.get_view3D_canvas3D()->reload_scene(true);
            update = true;
        }
        if (values.has("prime_volume_mode")) {
            const auto mode = values.opt_enum<PrimeVolumeMode>("prime_volume_mode");
            bundle.project_config.set_key_value("prime_volume_mode", new ConfigOptionEnum<PrimeVolumeMode>(mode));
            update = true;
        }
        // A tower drag, plate by plate: one undo step first, which takes the old positions along
        // (Plater::take_snapshot()), then both coordinates of each plate whose tower moved.
        const auto position = [&](const char* key, std::size_t plate) {
            return (values.has(key) ? values : before).option<ConfigOptionFloats>(key)->values[plate];
        };
        std::vector<std::size_t> moved;
        for (std::size_t i = 0; i < before.option<ConfigOptionFloats>("wipe_tower_x")->size(); ++i)
            if (position("wipe_tower_x", i) != before.option<ConfigOptionFloats>("wipe_tower_x")->values[i] ||
                position("wipe_tower_y", i) != before.option<ConfigOptionFloats>("wipe_tower_y")->values[i])
                moved.push_back(i);
        if (!moved.empty()) {
            plater.take_snapshot("Move object");
            for (const std::size_t i : moved)
                for (const char* key : {"wipe_tower_x", "wipe_tower_y"}) {
                    const ConfigOptionFloat value(position(key, i));
                    bundle.project_config.option<ConfigOptionFloats>(key, true)->set_at(&value, i, 0);
                }
            update = true;
        }
        // Reloading the scene re-places a tower that does not fit where it was put, as after a drag.
        if (update)
            plater.update();
        const DynamicPrintConfig after   = project_settings();
        json                     applied = json::object();
        for (auto it = values_in.begin(); it != values_in.end(); ++it)
            applied[it.key()] = after.opt_serialize(it.key());
        json out{{"applied", applied}, {"changed", changed_settings(before, after)}};
        for (const char* key : {"wipe_tower_x", "wipe_tower_y"})
            if (values.has(key) && *values.option(key) != *after.option(key))
                out["note"] = "A prime tower did not fit where it was put; the app moved it (see applied).";
        return out;
    });
    settle(ctx);
    return ToolResult::ok(reply);
}

// The plate scope: what the Plate Settings dialog sets on one plate, and the plate's name and lock, which
// its icons set.
const char* const plate_scope_keys[] = {"curr_bed_type", "print_sequence", "spiral_mode", "first_layer_print_sequence",
                                        "other_layers_print_sequence", "name", "locked"};

bool is_plate_setting(const std::string& key) { return key != "name" && key != "locked"; }

// The plate the scope's `plate` argument names, the current one without it. UI thread.
int scope_plate(const json& args)
{
    PartPlateList& plates = ui_plater().get_partplate_list();
    const int      index  = arg_int(args, "plate", plates.get_curr_plate_index());
    if (index < 0 || index >= plates.get_plate_count())
        throw ToolFailure("plate must be 0.." + std::to_string(plates.get_plate_count() - 1));
    return index;
}

// A plate setting in the form set_settings takes: the plate's own value, null where it follows the
// project's plate type, the process or Auto; or with `effective` what the plate prints with.
json plate_value(PartPlate& plate, const std::string& key, bool effective)
{
    if (key == "name")
        return plate.get_plate_name();
    if (key == "locked")
        return plate.is_locked();
    if (key == "curr_bed_type") {
        const BedType type = plate.get_bed_type(effective);
        return type == btDefault ? json() : json(ConfigOptionEnum<BedType>(type).serialize());
    }
    if (key == "print_sequence") {
        const PrintSequence sequence = effective ? plate.get_real_print_seq() : plate.get_print_seq();
        return sequence == PrintSequence::ByDefault ? json() : json(ConfigOptionEnum<PrintSequence>(sequence).serialize());
    }
    if (key == "spiral_mode")
        return effective || plate.has_spiral_mode_config() ? json(plate.get_spiral_vase_mode()) : json();
    if (key == "first_layer_print_sequence") {
        const std::vector<int> order = plate.get_first_layer_print_sequence();
        return order.empty() ? json() : json(order);
    }
    const std::vector<LayerPrintSequence> ranges = plate.get_other_layers_print_sequence();
    return ranges.empty() ? json() : layer_sequences_json(ranges);
}

// Every plate scope value of a plate: its own, or with `effective` what it prints with.
json plate_values(PartPlate& plate, bool effective)
{
    json out = json::object();
    for (const char* key : plate_scope_keys)
        out[key] = plate_value(plate, key, effective);
    return out;
}

// get_settings on the plate scope: every key, or those named, with the plate's own value and the one it
// prints with. UI thread.
json plate_settings(const json& args)
{
    if (args.contains("compare") || args.contains("preset"))
        throw ToolFailure("preset and compare are for the process, filament and printer scopes");
    const int  index = scope_plate(args);
    PartPlate& plate = *ui_plater().get_partplate_list().get_plate(index);
    std::vector<std::string> keys(std::begin(plate_scope_keys), std::end(plate_scope_keys));
    json                     unknown = json::array();
    if (args.contains("keys")) {
        keys.clear();
        for (const json& k : args["keys"]) {
            const std::string key = k.get<std::string>();
            if (std::find(std::begin(plate_scope_keys), std::end(plate_scope_keys), key) != std::end(plate_scope_keys))
                keys.push_back(key);
            else
                unknown.push_back(key);
        }
    }
    json items = json::array();
    for (const std::string& key : keys) {
        json item{{"key", key}, {"value", plate_value(plate, key, false)}};
        if (is_plate_setting(key)) {
            item["effective"] = plate_value(plate, key, true);
            if (arg_bool(args, "include_meta", true)) {
                item["meta"] = option_meta(*print_config_def.get(key));
                if (key == "curr_bed_type")
                    item["meta"]["offered"] = project_offered(key);
            }
        }
        items.push_back(std::move(item));
    }
    json out     = page(items, args, 100);
    out["plate"] = index;
    if (!unknown.empty())
        out["unknown_keys"] = unknown;
    return out;
}

// set_settings on the plate scope. The Plate Settings dialog's values go through its OK
// (Plater::apply_plate_settings()) with the plate's other values as they are, after selecting the plate as
// its settings icon does; the lock and the name as their icons do. Everything is checked before anything
// changes, and the call is one undo step.
ToolResult set_plate_settings(const json& args, ToolContext& ctx)
{
    const json values_in = args.value("values", json::object());
    if (values_in.empty())
        throw ToolFailure("values must be a non-empty object {key: value}");
    if (args.contains("reset_all") || args.contains("reset_to"))
        throw ToolFailure("reset_all and reset_to are for preset scopes; here a null value takes the global setting or Auto");
    int  index = -1;
    json reply = ctx.run_ui([&]() -> json {
        Plater& plater = ui_plater();
        if (plater.printer_technology() != ptFFF || plater.only_gcode_mode())
            throw ToolFailure("Plates are a filament (FFF) feature");
        index                 = scope_plate(args);
        PartPlateList& plates = plater.get_partplate_list();
        PartPlate&     plate  = *plates.get_plate(index);
        const json     before = plate_values(plate, false);
        // The dialog's choices, starting from the plate's own.
        const int                       own_spiral   = plate.has_spiral_mode_config() ? (plate.get_spiral_vase_mode() ? 1 : 2) : 0;
        BedType                         bed_type     = plate.get_bed_type();
        PrintSequence                   sequence     = plate.get_print_seq();
        int                             spiral       = own_spiral;
        std::vector<int>                first_layer  = plate.get_first_layer_print_sequence();
        std::vector<LayerPrintSequence> other_layers = plate.get_other_layers_print_sequence();
        const int                       slots        = wxGetApp().filaments_cnt();
        for (auto it = values_in.begin(); it != values_in.end(); ++it) {
            const std::string& key   = it.key();
            const json&        value = it.value();
            if (std::find(std::begin(plate_scope_keys), std::end(plate_scope_keys), key) == std::end(plate_scope_keys))
                throw ToolFailure("Unknown setting \"" + key + "\" for the plate scope",
                                  {{"keys", std::vector<std::string>(std::begin(plate_scope_keys), std::end(plate_scope_keys))}});
            if (key == "name" && !value.is_string())
                throw ToolFailure("name takes a string (\"\" for none)");
            if (key == "locked" && !value.is_boolean())
                throw ToolFailure("locked takes true or false");
            if (key == "curr_bed_type") {
                bed_type = btDefault; // "Same as Global Plate Type"
                if (value.is_null())
                    continue;
                const std::vector<std::string> offered = project_offered(key);
                if (offered.empty())
                    throw ToolFailure("curr_bed_type: this printer has a single plate type");
                if (!value.is_string() || std::find(offered.begin(), offered.end(), value.get<std::string>()) == offered.end())
                    throw ToolFailure("This printer does not offer curr_bed_type " + value.dump(), {{"offered", offered}});
                bed_type = BedType(ConfigOptionEnum<BedType>::get_enum_values().at(value.get<std::string>()));
            } else if (key == "print_sequence") {
                const auto& names = ConfigOptionEnum<PrintSequence>::get_enum_values();
                if (!value.is_null() && (!value.is_string() || names.count(value.get<std::string>()) == 0))
                    throw ToolFailure("print_sequence is \"by layer\", \"by object\" or null (the process's)");
                sequence = value.is_null() ? PrintSequence::ByDefault : PrintSequence(names.at(value.get<std::string>()));
            } else if (key == "spiral_mode") {
                if (!value.is_null() && !value.is_boolean())
                    throw ToolFailure("spiral_mode is true, false or null (the process's)");
                spiral = value.is_null() ? 0 : value.get<bool>() ? 1 : 2;
            } else if (key == "first_layer_print_sequence" || key == "other_layers_print_sequence") {
                // The dialog greys the custom orders out while a slot is a mixed filament, which prints a different
                // physical filament from layer to layer.
                if (!value.is_null() && wxGetApp().preset_bundle->num_mixed_filaments() > 0)
                    throw ToolFailure(key + " cannot be set while the project has mixed filaments (see the project scope's "
                                            "filament_is_mixed); null sets Auto");
                if (key == "first_layer_print_sequence")
                    first_layer = value.is_null() ? std::vector<int>() : filament_order(value, slots);
                else
                    other_layers = value.is_null() ? std::vector<LayerPrintSequence>() : layer_sequences(value, slots);
            }
        }
        const bool settings = bed_type != plate.get_bed_type() || sequence != plate.get_print_seq() ||
                              first_layer != plate.get_first_layer_print_sequence() ||
                              other_layers != plate.get_other_layers_print_sequence() || spiral != own_spiral;
        const bool rename = values_in.contains("name") && values_in["name"].get<std::string>() != plate.get_plate_name();
        // The settings and name icons select their plate first; the dialogs work on the current plate.
        if ((settings || rename) && index != plates.get_curr_plate_index())
            plater.select_plate(index);
        Plater::SingleSnapshot single(&plater);
        if (settings) {
            plater.take_snapshot("Plate Settings");
            plater.apply_plate_settings(index, bed_type, first_layer, other_layers, sequence, spiral);
        }
        if (values_in.contains("locked") && values_in["locked"].get<bool>() != plate.is_locked()) {
            plater.take_snapshot("lock partplate");
            plates.lock_plate(index, values_in["locked"].get<bool>());
            plater.set_current_canvas_as_dirty();
        }
        if (rename)
            plater.rename_plate(index, values_in["name"].get<std::string>());
        const json after     = plate_values(plate, false);
        json       applied   = json::object();
        json       effective = json::object();
        json       changed   = json::object();
        for (auto it = values_in.begin(); it != values_in.end(); ++it) {
            applied[it.key()] = after[it.key()];
            if (is_plate_setting(it.key()))
                effective[it.key()] = plate_value(plate, it.key(), true);
        }
        for (const char* key : plate_scope_keys)
            if (after[key] != before[key])
                changed[key] = after[key];
        json out{{"plate", index}, {"applied", applied}, {"changed", changed}};
        if (!effective.empty())
            out["effective"] = std::move(effective);
        return out;
    });
    // A replay sets the same plate, whichever is current then.
    if (!args.contains("plate")) {
        json recorded     = args;
        recorded["plate"] = index;
        ctx.set_journal_args(std::move(recorded));
    }
    settle(ctx);
    return ToolResult::ok(reply);
}

// list_plates: every plate's settings as it prints them, what it sets itself, its objects, slice state,
// prime tower position and layer G-codes. UI thread.
json list_plates(const json& args)
{
    Plater&                   plater  = ui_plater();
    PartPlateList&            plates  = plater.get_partplate_list();
    const Model&              model   = plater.model();
    const DynamicPrintConfig& project = wxGetApp().preset_bundle->project_config;
    const auto*               tower_x = project.option<ConfigOptionFloats>("wipe_tower_x");
    const auto*               tower_y = project.option<ConfigOptionFloats>("wipe_tower_y");
    json                      items   = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate& plate = *plates.get_plate(i);
        json       entry = plate_values(plate, true);
        entry["index"]   = i;
        entry["current"] = i == plates.get_curr_plate_index();
        json overrides   = json::array();
        for (const char* key : plate_scope_keys)
            if (is_plate_setting(key) && !plate_value(plate, key, false).is_null())
                overrides.push_back(key);
        entry["overrides"] = std::move(overrides);
        json objects       = json::array();
        for (std::size_t o = 0; o < model.objects.size(); ++o)
            for (std::size_t j = 0; j < model.objects[o]->instances.size(); ++j)
                if (plates.find_instance(int(o), int(j)) == i)
                    objects.push_back({int(o), int(j)});
        entry["objects"] = std::move(objects);
        entry["sliced"]  = plate.is_slice_result_valid();
        if (tower_x != nullptr && tower_y != nullptr && !tower_x->empty() && !tower_y->empty())
            entry["wipe_tower"] = {tower_x->get_at(std::size_t(i)), tower_y->get_at(std::size_t(i))};
        // Layer numbers only from an up-to-date slice that no slice is rewriting.
        const auto          codes = model.plates_custom_gcodes.find(i);
        std::vector<double> layer_zs;
        if (codes != model.plates_custom_gcodes.end() && !codes->second.gcodes.empty() && plate.is_slice_result_valid() &&
            !plater.is_slicing_any() && plate.get_slice_result() != nullptr)
            layer_zs = gcode_layer_zs(*plate.get_slice_result());
        entry["layer_gcode"] = codes != model.plates_custom_gcodes.end() ? layer_gcode_json(codes->second, layer_zs) : json::array();
        items.push_back(std::move(entry));
    }
    return page(items, args, 100);
}

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
        const bool is_part = t.volume != nullptr;
        t.options          = model_scope_options(SettingsFactory::get_options(is_part), is_part, wxGetApp().filaments_cnt());
        if (!for_write) {
            // Reads take any print setting too: the object prints with every one of them, from its
            // override or the process, which `effective` says. Writes keep to what the object list
            // offers, so no override is made that the GUI can't show.
            for (const std::string& key : preset_options(Preset::TYPE_PRINT))
                if (std::find(t.options.begin(), t.options.end(), key) == t.options.end())
                    t.options.push_back(key);
        }
        return t;
    }
    if (scope == "project") {
        t.project = true;
        t.owned   = std::make_shared<const DynamicPrintConfig>(project_settings());
        t.config  = t.owned.get();
        t.options = project_options();
        return t;
    }
    t.type    = preset_type(scope);
    t.options = preset_options(t.type);
    if (args.contains("preset")) {
        // A saved preset, selected or not, read without selecting it.
        if (for_write)
            throw ToolFailure("Only the preset being edited can be changed; select it first");
        if (args.contains("filament_slot"))
            throw ToolFailure("preset or filament_slot, not both");
        const std::string name = arg_string(args, "preset");
        // `real`: the saved preset, not the edited copy find_preset() gives for the selected one.
        t.preset = collection(t.type).find_preset(name, false, true);
        if (t.preset == nullptr)
            throw ToolFailure("No " + scope + " preset \"" + name + "\" (see list_presets)", {{"similar", similar_presets(collection(t.type), name)}});
        t.config = &t.preset->config;
        return t;
    }
    if (t.type == Preset::TYPE_FILAMENT) {
        const int slot = arg_int(args, "filament_slot", 1);
        if (slot < 1 || slot > int(bundle.filament_presets.size()))
            throw ToolFailure("filament_slot must be 1.." + std::to_string(bundle.filament_presets.size()));
        const std::string& name = bundle.filament_presets[size_t(slot - 1)];
        if (for_write)
            edit_filament_slot(slot);
        t.edited = bundle.filaments.get_edited_preset().name == name;
        t.preset = t.edited ? &bundle.filaments.get_edited_preset() : bundle.filaments.find_preset(name);
        if (t.preset == nullptr)
            throw ToolFailure("Filament slot " + std::to_string(slot) + " has no preset");
        t.config = &t.preset->config;
        return t;
    }
    t.edited = true;
    t.preset = &collection(t.type).get_edited_preset();
    t.config = &t.preset->config;
    return t;
}

// The printer's extruder types and the project's nozzle volume types, one per extruder: together
// they pick the entry of a per-variant vector each extruder prints with. UI thread.
std::pair<std::vector<int>, std::vector<int>> printer_extruders()
{
    const PresetBundle& bundle  = *wxGetApp().preset_bundle;
    const auto*         types   = bundle.printers.get_edited_preset().config.option<ConfigOptionEnumsGeneric>("extruder_type");
    const auto*         volumes = bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    return {types != nullptr ? types->values : std::vector<int>(), volumes != nullptr ? volumes->values : std::vector<int>()};
}

// The preset get_settings' compare names: ":saved" is the read preset as saved (so the difference is
// its unsaved changes), ":parent" what the tab's lock icons compare against, else a saved preset by name.
const Preset& compare_preset(const ConfigTarget& t, const std::string& name)
{
    PresetCollection& presets = collection(t.type);
    const Preset*     other   = nullptr;
    if (name == ":saved") {
        other = presets.find_preset(t.preset->name, false, true);
        if (other == nullptr)
            throw ToolFailure("\"" + t.preset->name + "\" has no saved copy");
    } else if (name == ":parent") {
        // A system preset or a user preset that inherits nothing is its own parent, as for the lock icons.
        other = t.edited ? presets.get_selected_preset_parent() :
                presets.get_preset_base(*t.preset) == t.preset ? t.preset : presets.get_preset_parent(*t.preset);
        if (other == nullptr)
            throw ToolFailure("\"" + t.preset->name + "\" inherits from no preset");
    } else {
        other = presets.find_preset(name, false, true);
        if (other == nullptr)
            throw ToolFailure("No preset \"" + name + "\" to compare with (see list_presets)", {{"similar", similar_presets(presets, name)}});
    }
    // The Compare presets dialog compares presets of one printer technology only.
    if (other->printer_technology() != t.preset->printer_technology())
        throw ToolFailure("\"" + other->name + "\" is for another printer technology");
    return *other;
}

// Options whose value is a filament slot number, 0 meaning the default.
const char* const filament_number_keys[] = {"extruder",
                                            "outer_wall_filament_id",
                                            "inner_wall_filament_id",
                                            "sparse_infill_filament_id",
                                            "internal_solid_filament_id",
                                            "top_surface_filament_id",
                                            "bottom_surface_filament_id",
                                            "support_filament",
                                            "support_interface_filament"};

// Refuses the filament numbers the object list's combos do not offer: past the last slot, and for
// support a mixed slot, which the support code cannot resolve. Checked before anything changes,
// since ObjectList::set_extruder_for_items() ignores a number past the last slot silently. UI thread.
void check_filament_numbers(const DynamicPrintConfig& values)
{
    const int slots = int(ui_plater().get_extruder_colors_from_plater_config().size());
    for (const char* key : filament_number_keys) {
        if (!values.has(key))
            continue;
        const int n = values.opt_int(key);
        if (n < 0 || n > slots)
            throw ToolFailure("Filament " + std::to_string(n) + " does not exist; this project has " + std::to_string(slots) +
                              " filament slots (0 is the default)");
        if (n > 0 && std::string(key).rfind("support_", 0) == 0 && wxGetApp().preset_bundle->is_mixed_filament(size_t(n - 1)))
            throw ToolFailure(std::string(key) + " takes a physical filament; slot " + std::to_string(n) + " is a mixed filament");
    }
}

// The object list row whose Change Filament sets the target's filament: the object's, which is also
// what a single-part object shows for its one part, or the part's or modifier's own row. UI thread.
wxDataViewItem filament_row(const ConfigTarget& t)
{
    ObjectDataViewModel& rows = *wxGetApp().obj_list()->GetModel();
    if (t.volume != nullptr && !t.volume->is_model_part() && !t.volume->is_modifier())
        throw ToolFailure("Only parts and modifiers take a filament; this volume is a negative part, a support blocker or enforcer, "
                          "or a seam modifier");
    if (t.volume == nullptr || t.object->volumes.size() == 1)
        return rows.GetItemById(t.object_index);
    // MCP's part index is the model's; the object list orders its rows by type.
    const int            model_index = int(std::find(t.object->volumes.begin(), t.object->volumes.end(), t.volume) - t.object->volumes.begin());
    const wxDataViewItem row = rows.GetItemByVolumeId(t.object_index, rows.get_real_volume_index_in_ui(t.object_index, model_index));
    if (!row.IsOk() || (rows.GetItemType(row) & itVolume) == 0)
        throw ToolFailure("This part has no row in the object list (a cut connector); set the object's filament instead");
    return row;
}

// A changed option as the tab marks it, key or key#index: the key, and the entry (-1 for the whole value).
std::pair<std::string, int> option_entry(const std::string& id)
{
    const std::size_t hash = id.find('#');
    return {id.substr(0, hash), hash == std::string::npos ? -1 : std::atoi(id.c_str() + hash + 1)};
}

// One entry of a vector, or the whole value with index -1; null where the config lacks it.
json entry_value(const DynamicPrintConfig& config, const std::string& key, int index)
{
    const ConfigOption* opt = config.option(key);
    if (opt == nullptr)
        return nullptr;
    if (index < 0)
        return opt->serialize();
    const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt);
    return vec != nullptr && index < int(vec->size()) ? json(vec->vserialize()[std::size_t(index)]) : json();
}

// The keys of `changes` ({key or key#index: value}), each once.
json change_keys(const json& changes)
{
    json keys = json::array();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        if (const std::string key = option_entry(it.key()).first; std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    return keys;
}

// The 1-based filament slots that use the preset `name`.
json filament_slots(const std::string& name)
{
    const std::vector<std::string>& presets = wxGetApp().preset_bundle->filament_presets;
    json                            slots   = json::array();
    for (std::size_t s = 0; s < presets.size(); ++s)
        if (presets[s] == name)
            slots.push_back(int(s + 1));
    return slots;
}

json list_presets(const json& args)
{
    const Preset::Type type               = preset_type(arg_string(args, "type"));
    const bool         include_incompatible = arg_bool(args, "include_incompatible", false);
    const bool         include_hidden     = arg_bool(args, "include_hidden", false);
    const std::string  query              = lower(arg_string(args, "query"));
    PresetCollection&  presets            = collection(type);
    const std::string  selected           = presets.get_selected_preset_name();
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
        if (!p.inherits().empty())
            j["inherits"] = p.inherits();
        if (p.is_project_embedded)
            j["project"] = true;
        if (!p.is_compatible)
            j["compatible"] = false;
        if (!p.is_visible)
            j["visible"] = false;
        if (p.name == selected) {
            j["selected"] = true;
            if (presets.current_is_dirty())
                j["modified"] = true;
        }
        if (type == Preset::TYPE_FILAMENT)
            if (json slots = filament_slots(p.name); !slots.empty())
                j["filament_slots"] = std::move(slots);
        items.push_back(std::move(j));
    }
    return page(items, args, 100);
}

} // namespace

std::vector<std::string> model_scope_options(const std::vector<std::string>& offered, bool is_part, int filaments)
{
    std::vector<std::string> keys;
    for (const std::string& key : offered)
        if (const ConfigOptionDef* def = print_config_def.get(key);
            def != nullptr && !SettingsFactory::is_improper_category(def->category, filaments, !is_part))
            keys.push_back(key);
    // The filament column and the Change Filament menu, not a setting the list offers.
    keys.push_back("extruder");
    return keys;
}

json effective_setting(const std::string& key, const DynamicPrintConfig& process, const ModelObject& object, const ModelVolume* volume)
{
    // An extruder of 0 is the object list's "Default": no filament of its own.
    const auto own = [&key](const ModelConfig& config) -> const ConfigOption* {
        const ConfigOption* opt = config.option(key);
        return opt != nullptr && (key != "extruder" || opt->getInt() != 0) ? opt : nullptr;
    };
    json out;
    if (const ConfigOption* opt = volume != nullptr ? own(volume->config) : nullptr)
        out = {{"effective", opt->serialize()}, {"source", "part"}};
    else if (const ConfigOption* opt = own(object.config))
        out = {{"effective", opt->serialize()}, {"source", "object"}};
    else if (process.has(key))
        out = {{"effective", process.opt_serialize(key)}, {"source", "process"}};
    else if (key == "extruder")
        out = {{"effective", "1"}, {"source", "default"}}; // the process has no extruder; filament 1 prints
    else if (const ConfigOptionDef* def = print_config_def.get(key); def != nullptr && def->default_value)
        out = {{"effective", def->default_value->serialize()}, {"source", "default"}};
    if (volume == nullptr) {
        json parts = json::array();
        for (std::size_t i = 0; i < object.volumes.size(); ++i) {
            const ModelVolume& v = *object.volumes[i];
            if (v.is_model_part() || v.is_modifier())
                if (const ConfigOption* opt = own(v.config))
                    parts.push_back({{"volume", int(i)}, {"value", opt->serialize()}});
        }
        if (!parts.empty())
            out["parts"] = std::move(parts);
    }
    return out;
}

bool save_preset_inside_project(const PresetCollection& presets, const std::string& name, const json& args)
{
    wxString               info;
    const PresetNameStatus status = check_preset_name(presets, name, info);
    if (status == PresetNameStatus::Invalid) {
        std::string why = into_u8(info);
        std::replace_if(why.begin(), why.end(), [](char c) { return c == '\n' || c == '\t'; }, ' ');
        throw ToolFailure("Cannot save as \"" + name + "\": " + why);
    }
    // The dialog warns only for another preset of that name; saving the selected one again is its plain Save.
    const Preset* existing = presets.find_preset(name, false);
    if (status == PresetNameStatus::Warning && !arg_bool(args, "overwrite", false))
        throw ToolFailure("Preset \"" + name + "\" already exists" +
                          (existing != nullptr && !existing->is_compatible ? " and is incompatible with the current printer" : "") +
                          "; pass overwrite: true to replace it");
    if (existing == nullptr)
        return arg_bool(args, "inside_project", presets.get_edited_preset().is_project_embedded);
    // The dialog locks the choice to where the existing preset is.
    if (args.contains("inside_project") && args["inside_project"].get<bool>() != existing->is_project_embedded)
        throw ToolFailure("Preset \"" + name + "\" is " + (existing->is_project_embedded ? "kept inside the project" : "a user preset") +
                          " and is saved there; leave inside_project out");
    return existing->is_project_embedded;
}

json option_meta(const ConfigOptionDef& def)
{
    json meta{{"label", def.full_label.empty() ? def.label : def.full_label},
              {"type", type_name(def.type)},
              {"mode", mode_name(def.mode)}};
    if (!def.tooltip.empty())
        meta["description"] = def.tooltip;
    if (!def.category.empty())
        meta["category"] = def.category;
    if (!def.sidetext.empty())
        meta["unit"] = def.sidetext;
    if (def.min > -FLT_MAX && def.min != -FLT_MAX)
        meta["min"] = def.min;
    if (def.max < FLT_MAX && def.max != FLT_MAX)
        meta["max"] = def.max;
    if (!def.enum_values.empty())
        meta["values"] = def.enum_values;
    // What the GUI's combo shows for each value, so a person's wording maps to a value.
    if (!def.enum_labels.empty() && def.enum_labels.size() == def.enum_values.size())
        meta["labels"] = def.enum_labels;
    return meta;
}

std::vector<std::string> rank_settings(const std::string& query, const std::vector<std::string>& within, std::size_t limit)
{
    const auto words_of = [](const std::string& text) {
        std::vector<std::string> words;
        std::string              word;
        for (const char c : lower(text) + ' ') {
            if (std::isalnum(static_cast<unsigned char>(c)))
                word += c;
            else if (!word.empty())
                words.push_back(std::exchange(word, {}));
        }
        return words;
    };
    const auto has = [](const std::vector<std::string>& words, const std::string& w) {
        return std::any_of(words.begin(), words.end(), [&w](const std::string& word) { return word.compare(0, w.size(), w) == 0; });
    };
    // "to" or "of" starts a word of most help texts, so short words count only when the query has no longer one.
    std::vector<std::string> words = words_of(query);
    std::vector<std::string> longer;
    std::copy_if(words.begin(), words.end(), std::back_inserter(longer), [](const std::string& w) { return w.size() >= 3; });
    if (!longer.empty())
        words = std::move(longer);
    // (query words matched, those matched in the key or label) per key.
    std::vector<std::pair<std::pair<int, int>, std::string>> scored;
    std::set<std::string>                                    seen; // a scope's options can list a key twice
    for (const std::string& key : within) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr || !seen.insert(key).second)
            continue;
        const std::vector<std::string> name    = words_of(key + ' ' + def->label + ' ' + def->full_label);
        const std::vector<std::string> tooltip = words_of(def->tooltip);
        std::pair<int, int>            score;
        for (const std::string& w : words)
            if (has(name, w)) {
                ++score.first;
                ++score.second;
            } else if (has(tooltip, w))
                ++score.first;
        if (score.first > 0)
            scored.emplace_back(score, key);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (std::size_t i = 0; i < scored.size() && i < limit; ++i)
        out.push_back(scored[i].second);
    return out;
}

json variant_entries(const std::string& key, const DynamicPrintConfig& config, const std::vector<int>& extruder_types,
                     const std::vector<int>& nozzle_volume_types)
{
    const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key));
    if (values == nullptr || values->size() < 2)
        return nullptr;
    // The keys naming each variant's extruder and kind; the machine limits hold a normal and a
    // silent mode value per variant.
    std::string  id_key;
    std::string  variant_key;
    unsigned int stride = 1;
    if (print_options_with_variant.count(key) > 0) {
        id_key      = "print_extruder_id";
        variant_key = "print_extruder_variant";
    } else if (filament_options_with_variant.count(key) > 0) {
        variant_key = "filament_extruder_variant"; // a filament's variants carry no extruder
    } else if (printer_options_with_variant_1.count(key) > 0 || printer_options_with_variant_2.count(key) > 0) {
        id_key      = "printer_extruder_id";
        variant_key = "printer_extruder_variant";
        stride      = printer_options_with_variant_2.count(key) > 0 ? 2 : 1;
    } else if (printer_extruder_options.count(key) == 0 && key != "extruder_offset") {
        return nullptr;
    }
    json names = json::array();
    if (variant_key.empty()) {
        // One entry per extruder, as the printer's Extruder page edits it.
        for (std::size_t i = 0; i < values->size(); ++i)
            names.push_back("extruder " + std::to_string(i + 1));
        return {{"variants", std::move(names)}};
    }
    const auto* ids      = id_key.empty() ? nullptr : config.option<ConfigOptionInts>(id_key);
    const auto* variants = config.option<ConfigOptionStrings>(variant_key);
    for (std::size_t i = 0; i < values->size(); ++i) {
        const std::size_t v    = i / stride;
        std::string       name = variants != nullptr && v < variants->size() ? variants->values[v] : "entry " + std::to_string(i);
        if (extruder_types.size() > 1 && ids != nullptr && v < ids->size())
            name = "extruder " + std::to_string(ids->values[v]) + ": " + name;
        if (stride == 2)
            name += i % stride == 0 ? " (normal mode)" : " (silent mode)";
        names.push_back(std::move(name));
    }
    // The entry each extruder prints with, as the slicer and the tab's variant switch pick it.
    json active = json::array();
    for (std::size_t e = 0; e < extruder_types.size(); ++e) {
        const auto volume = e < nozzle_volume_types.size() ? NozzleVolumeType(nozzle_volume_types[e]) : nvtStandard;
        const int  index  = config.get_index_for_extruder(int(e + 1), id_key, ExtruderType(extruder_types[e]), volume, variant_key, stride);
        active.push_back(index >= 0 && index < int(values->size()) ? json(index) : json());
    }
    return {{"variants", std::move(names)}, {"active", std::move(active)}};
}

std::string preset_setting_value(const std::string& key, const json& value, const DynamicPrintConfig& config,
                                 const std::vector<int>& extruder_types, const std::vector<int>& nozzle_volume_types)
{
    const auto* current = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key));
    // "0 = Direct Drive Standard (in use), 1 = Direct Drive High Flow", and the same as details.
    const auto entries = [&](json& details) {
        details = variant_entries(key, config, extruder_types, nozzle_volume_types);
        if (details.is_null())
            return std::string();
        const json  active = details.value("active", json::array());
        std::string text;
        for (std::size_t i = 0; i < details["variants"].size(); ++i) {
            text += (i > 0 ? ", " : "") + std::to_string(i) + " = " + details["variants"][i].get<std::string>();
            if (std::find(active.begin(), active.end(), json(i)) != active.end())
                text += " (in use)";
        }
        return text;
    };
    // Parses into a scratch config, so a bad value fails here with its key.
    const auto parse = [&key](const std::string& text, DynamicPrintConfig& scratch) {
        try {
            scratch.set_deserialize_strict(key, text);
        } catch (const std::exception& e) {
            throw ToolFailure("Invalid value for " + key + ": " + e.what());
        }
        return dynamic_cast<const ConfigOptionVectorBase*>(scratch.option(key));
    };
    if (value.is_object()) {
        if (current == nullptr)
            throw ToolFailure(key + " is a single value; give it without index");
        if (value.size() != 2 || !value.contains("index") || !value["index"].is_number_integer() || !value.contains("value"))
            throw ToolFailure("One entry of " + key + " is given as {\"index\": i, \"value\": v}");
        const int index = value["index"].get<int>();
        if (index < 0 || index >= int(current->size())) {
            json              details;
            const std::string list = entries(details);
            throw ToolFailure(key + " has " + std::to_string(current->size()) + " entries" + (list.empty() ? "" : ": " + list), details);
        }
        DynamicPrintConfig                 scratch;
        const ConfigOptionVectorBase*      entry = parse(to_config_string(value["value"], key), scratch);
        if (entry == nullptr || entry->size() != 1)
            throw ToolFailure("Entry " + std::to_string(index) + " of " + key + " takes one value");
        std::unique_ptr<ConfigOption> updated(current->clone());
        static_cast<ConfigOptionVectorBase*>(updated.get())->set_at(entry, std::size_t(index), 0);
        return updated->serialize();
    }
    const std::string text = to_config_string(value, key);
    if (current != nullptr && current->size() > 1) {
        json              details;
        const std::string list = entries(details);
        DynamicPrintConfig            scratch;
        const ConfigOptionVectorBase* given = list.empty() ? nullptr : parse(text, scratch);
        if (!value.is_array() && given != nullptr && given->size() == 1 && details.contains("active")) {
            // One value for a per-variant key goes to the entries in use, as typing into the tab's
            // field writes the variant it shows; the other variants keep theirs. An array is every
            // entry, so one of a single value is refused below.
            std::unique_ptr<ConfigOption> updated(current->clone());
            bool                          written = false;
            for (const json& index : details["active"])
                if (index.is_number_integer()) {
                    static_cast<ConfigOptionVectorBase*>(updated.get())->set_at(given, index.get<std::size_t>(), 0);
                    written = true;
                }
            if (written)
                return updated->serialize();
        }
        if (given != nullptr && given->size() != current->size())
            throw ToolFailure(key + " holds " + std::to_string(current->size()) + " values, one per " +
                                  (details.contains("active") ? "extruder variant" : "extruder") + ": " + list +
                                  ". Set one with {\"index\": i, \"value\": v}, or all of them as an array.",
                              details);
    }
    return text;
}

json preset_diff(const Preset& left, const Preset& right, const std::vector<std::string>& keys)
{
    // The Compare presets dialog's rule: printer and filament vectors are compared entry by entry, and
    // a printer with fewer extruders is compared from the other side.
    const bool deep    = left.type == Preset::TYPE_PRINTER || left.type == Preset::TYPE_FILAMENT;
    const auto colours = [](const Preset& p) {
        const auto* opt = p.config.option<ConfigOptionStrings>("extruder_colour");
        return opt != nullptr ? opt->size() : std::size_t(0);
    };
    const std::vector<std::string> dirty = left.type == Preset::TYPE_PRINTER && colours(left) < colours(right) ?
                                               PresetCollection::dirty_options(&right, &left, deep) :
                                               PresetCollection::dirty_options(&left, &right, deep);
    const auto settings_id = [](const std::string& key) {
        const std::string suffix = "_settings_id";
        return key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    json items = json::array();
    for (const std::string& id : dirty) {
        const auto [key, index] = option_entry(id);
        if (settings_id(key) || std::find(keys.begin(), keys.end(), key) == keys.end())
            continue;
        json item{{"key", key}};
        if (index >= 0)
            item["index"] = index;
        item["value"] = entry_value(left.config, key, index);
        item["other"] = entry_value(right.config, key, index);
        items.push_back(std::move(item));
    }
    return items;
}

json discarded_changes(const json& changes, const DynamicPrintConfig& config)
{
    json gone = json::object();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const auto [key, index] = option_entry(it.key());
        if (entry_value(config, key, index) != it.value())
            gone[it.key()] = it.value();
    }
    return change_keys(gone);
}

json unsaved_changes()
{
    json out = json::object();
    for (const char* type : {"process", "filament", "printer"}) {
        // What list_presets calls modified, in the settings the tab shows: the full comparison also lists
        // bookkeeping keys that only one of the two configs has.
        const PresetCollection& presets = collection(preset_type(type));
        if (!presets.current_is_dirty())
            continue;
        const std::vector<std::string>& options = preset_options(presets.type());
        const DynamicPrintConfig&       config  = presets.get_edited_preset().config;
        json                            changes = json::object();
        for (const std::string& id : presets.current_dirty_options(true)) {
            const auto [key, index] = option_entry(id);
            if (std::find(options.begin(), options.end(), key) != options.end())
                changes[id] = entry_value(config, key, index);
        }
        if (!changes.empty())
            out[type] = {{"name", presets.get_edited_preset().name}, {"changes", std::move(changes)}};
    }
    return out;
}

json unsaved_changes_outcome(const json& before)
{
    json discarded = json::object();
    for (auto it = before.begin(); it != before.end(); ++it)
        if (json keys = discarded_changes(it.value()["changes"], collection(preset_type(it.key())).get_edited_preset().config); !keys.empty())
            discarded[it.key()] = {{"name", it.value()["name"]}, {"keys", std::move(keys)}};
    json       modified = json::object();
    const json after    = unsaved_changes();
    for (auto it = after.begin(); it != after.end(); ++it)
        modified[it.key()] = {{"name", it.value()["name"]}, {"keys", change_keys(it.value()["changes"])}};
    json out = json::object();
    if (!discarded.empty())
        out["discarded"] = std::move(discarded);
    if (!modified.empty())
        out["modified"] = std::move(modified);
    return out;
}

json check_preset_deletable(const PresetCollection& presets, const std::string& name, bool delete_attached, const PresetCollection& prints,
                            const PresetCollection& filaments)
{
    const char* const type = presets.type() == Preset::TYPE_PRINT ? "process" : presets.type() == Preset::TYPE_FILAMENT ? "filament" : "printer";
    const Preset*     preset = presets.find_preset(name, false);
    if (preset == nullptr)
        throw ToolFailure(std::string("No ") + type + " preset \"" + name + "\" (see list_presets)", {{"similar", similar_presets(presets, name)}});
    // The tab shows its Delete button for these only (Tab::update_btns_enabling()).
    if (preset->is_system || preset->is_default)
        throw ToolFailure("\"" + name + "\" is a system preset; only user presets and presets inside the project can be deleted");
    if (preset->is_from_bundle())
        throw ToolFailure("\"" + name + "\" comes from a preset bundle; only user presets and presets inside the project can be deleted");
    if (presets.get_preset_base(*preset) != preset)
        return json::object();
    // A base preset: the tab refuses it while presets inherit from it, since they would no longer load.
    json children = json::array();
    for (const Preset& p : presets)
        if (p.inherits() == preset->name)
            children.push_back(p.name);
    if (!children.empty())
        throw ToolFailure("\"" + name + "\" cannot be deleted while other presets inherit from it; delete them first",
                          {{"inherited_by", std::move(children)}});
    // A custom printer takes the user presets made for it alone with it (Tab::select_preset()).
    json attached = json::object();
    if (presets.type() == Preset::TYPE_PRINTER)
        for (const auto& [key, made_for] : {std::pair{"process", &prints}, std::pair{"filament", &filaments}}) {
            json names = json::array();
            for (const Preset* p : made_for->get_user_presets_for_printer(name))
                names.push_back(p->name);
            if (!names.empty())
                attached[key] = std::move(names);
        }
    if (!attached.empty() && !delete_attached)
        throw ToolFailure("\"" + name + "\" has presets made for it alone (process: " +
                              std::to_string(attached.value("process", json::array()).size()) +
                              ", filament: " + std::to_string(attached.value("filament", json::array()).size()) +
                              "), which are deleted with it; pass delete_attached: true",
                          {{"attached", attached}});
    return attached;
}

std::vector<std::string> check_filament_colours(std::vector<std::string> colours, const std::vector<std::string>& current,
                                                const std::vector<unsigned char>& mixed)
{
    if (colours.size() != current.size())
        throw ToolFailure("filament_colour takes one colour per filament slot: " + std::to_string(current.size()) + ", not " +
                          std::to_string(colours.size()));
    for (std::size_t i = 0; i < colours.size(); ++i) {
        std::string& colour = colours[i];
        if (lower(colour) == lower(current[i])) {
            colour = current[i]; // unchanged, whatever its form
            continue;
        }
        if (i < mixed.size() && mixed[i] != 0)
            throw ToolFailure("Slot " + std::to_string(i + 1) + " is a mixed filament; its colour comes from its components");
        if (colour.size() != 7 || colour[0] != '#' ||
            !std::all_of(colour.begin() + 1, colour.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            throw ToolFailure("Slot " + std::to_string(i + 1) + "'s colour \"" + colour + "\" is not #RRGGBB");
        // As the colour pickers store it.
        std::transform(colour.begin(), colour.end(), colour.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    }
    return colours;
}

std::vector<double> check_flush_multipliers(std::vector<double> multipliers, std::size_t nozzles)
{
    if (multipliers.size() == 1)
        multipliers = std::vector<double>(nozzles, multipliers.front());
    if (multipliers.size() != nozzles)
        throw ToolFailure("flush_multiplier takes one value per nozzle (" + std::to_string(nozzles) + "), or one for all");
    // The Flushing volumes dialog's range (g_min_flush_multiplier, g_max_flush_multiplier).
    for (const double m : multipliers)
        if (m < 0. || m > 3.)
            throw ToolFailure("flush_multiplier " + to_config_string(json(m)) + " is outside 0..3, the Flushing volumes dialog's range");
    return multipliers;
}

std::vector<double> check_flush_matrix(std::vector<double> matrix, std::size_t slots, const std::vector<double>& multipliers,
                                       const std::vector<double>& current, const std::vector<unsigned char>& mixed)
{
    const std::size_t cells = slots * slots;
    if (matrix.size() != multipliers.size() * cells)
        throw ToolFailure("flush_volumes_matrix takes " + std::to_string(multipliers.size() * cells) + " values: " +
                          std::to_string(slots) + "x" + std::to_string(slots) + " for each of " + std::to_string(multipliers.size()) +
                          " nozzle(s), in config order over every filament slot");
    const auto is_mixed = [&mixed](std::size_t slot) { return slot < mixed.size() && mixed[slot] != 0; };
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        const std::size_t nozzle = i / cells, from = i % cells / slots, to = i % slots;
        const std::string where  = "flush_volumes_matrix[" + std::to_string(i) + "] (nozzle " + std::to_string(nozzle + 1) +
                                  ", from slot " + std::to_string(from + 1) + " to slot " + std::to_string(to + 1) + ")";
        // The dialog shows physical slots only and keeps the rest; a mixed filament is never flushed.
        if (is_mixed(from) || is_mixed(to)) {
            if (current.size() == matrix.size() && matrix[i] != current[i])
                throw ToolFailure(where + " belongs to a mixed filament, which is never flushed; keep its value " +
                                  to_config_string(json(current[i])));
            continue;
        }
        if (matrix[i] < 0.)
            throw ToolFailure(where + " is negative");
        if (from == to && matrix[i] != 0.)
            throw ToolFailure(where + " must be 0: no flush from a filament to itself");
        // The limit the dialog applies before storing, which scales with the multiplier.
        if (multipliers[nozzle] > 0.)
            matrix[i] = std::min(matrix[i], double(g_max_flush_volume) / multipliers[nozzle]);
    }
    return matrix;
}

std::vector<int> filament_order(const json& order, int slots)
{
    std::vector<int> out;
    if (order.is_array())
        for (const json& slot : order)
            out.push_back(slot.is_number_integer() ? slot.get<int>() : 0);
    std::vector<int> sorted = out;
    std::sort(sorted.begin(), sorted.end());
    std::vector<int> every(std::size_t(std::max(slots, 0)));
    std::iota(every.begin(), every.end(), 1);
    if (sorted != every)
        throw ToolFailure("A filament order lists every filament slot 1.." + std::to_string(slots) + " once, e.g. [2, 1]; got " +
                          order.dump());
    return out;
}

std::vector<LayerPrintSequence> layer_sequences(const json& ranges, int slots)
{
    // The dialog's "End" of a range: up to the last layer.
    constexpr int last_layer = INT_MAX - 1;
    if (!ranges.is_array() || ranges.empty())
        throw ToolFailure("other_layers_print_sequence is [{from, to, order}, ...] or null for Auto");
    std::vector<LayerPrintSequence> out;
    for (const json& range : ranges) {
        const bool shaped = range.is_object() && range.contains("from") && range["from"].is_number_integer() && range.contains("order") &&
                            (!range.contains("to") || range["to"].is_number_integer()) && range.size() == (range.contains("to") ? 3u : 2u);
        if (!shaped)
            throw ToolFailure("A range is {\"from\": layer, \"to\": layer, \"order\": [slots]}, to optional (the last layer); got " + range.dump());
        const int from = range["from"].get<int>();
        const int to   = range.value("to", last_layer);
        if (from < 2)
            throw ToolFailure("Ranges start at layer 2 or later; the first layer's order is first_layer_print_sequence");
        if (to < from)
            throw ToolFailure("Range " + range.dump() + " ends before it starts");
        out.push_back({{from, std::min(to, last_layer)}, filament_order(range["order"], slots)});
    }
    std::sort(out.begin(), out.end(), [](const LayerPrintSequence& a, const LayerPrintSequence& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < out.size(); ++i)
        if (out[i].first.first <= out[i - 1].first.second)
            throw ToolFailure("Layer ranges overlap: one " +
                              (out[i - 1].first.second == last_layer ? std::string("runs to the last layer") :
                                                                       "ends at layer " + std::to_string(out[i - 1].first.second)) +
                              ", the next starts at layer " + std::to_string(out[i].first.first));
    return out;
}

json layer_sequences_json(const std::vector<LayerPrintSequence>& ranges)
{
    json out = json::array();
    for (const LayerPrintSequence& range : ranges) {
        json item{{"from", range.first.first}, {"order", range.second}};
        // The dialog's "End" is INT_MAX - 1, the plate tab's default INT_MAX.
        if (range.first.second < INT_MAX - 1)
            item["to"] = range.first.second;
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<double> gcode_layer_zs(const GCodeProcessorResult& result)
{
    // GCodeViewer::load() and libvgcode::Layers::update(): layers numbered consecutively over the moves
    // there are, each labelled with its last extrusion's height, or a belt print's with its print Z.
    const bool          belt = result.belt_tilt_angle > 0.f;
    std::vector<double> zs;
    unsigned int        layer = std::numeric_limits<unsigned int>::max();
    for (std::size_t i = 1; i < result.moves.size(); ++i) {
        const GCodeProcessorResult::MoveVertex& move = result.moves[i];
        if (move.layer_id != layer) {
            layer = move.layer_id;
            zs.push_back(belt ? double(move.print_z) : 0.);
        }
        if (!belt && move.type == EMoveType::Extrude && move.extrusion_role != erCustom)
            zs.back() = double(move.position.z());
    }
    return zs;
}

json layer_gcode_json(const CustomGCode::Info& codes, const std::vector<double>& layer_zs)
{
    std::vector<CustomGCode::Item> items = codes.gcodes;
    std::stable_sort(items.begin(), items.end());
    json out = json::array();
    for (const CustomGCode::Item& item : items) {
        json code{{"z", std::round(item.print_z * 1000.) / 1000.}};
        // IMSlider::get_tick_from_value(z, true): the first layer whose top is at or above the height.
        const auto layer = std::lower_bound(layer_zs.begin(), layer_zs.end(), item.print_z - epsilon());
        if (layer != layer_zs.end())
            code["layer"] = int(layer - layer_zs.begin()) + 1;
        const bool change = item.type == CustomGCode::ToolChange || item.type == CustomGCode::ColorChange;
        switch (item.type) {
        case CustomGCode::PausePrint: code["type"] = "pause"; break;
        case CustomGCode::Custom: code["type"] = "custom"; break;
        case CustomGCode::Template: code["type"] = "template"; break;
        case CustomGCode::ToolChange: code["type"] = "change_filament"; break;
        case CustomGCode::ColorChange: code["type"] = "color_change"; break;
        default: code["type"] = "unknown"; break;
        }
        if (change && item.extruder > 0)
            code["filament"] = item.extruder;
        if (change && !item.color.empty())
            code["color"] = item.color;
        if (item.type == CustomGCode::Custom && !item.extra.empty())
            code["gcode"] = item.extra;
        out.push_back(std::move(code));
    }
    return out;
}

json changed_settings(const DynamicPrintConfig& before, const DynamicPrintConfig& after)
{
    json changed = json::object();
    for (const std::string& key : after.keys())
        if (const ConfigOption* old = before.option(key); old == nullptr || *old != *after.option(key))
            changed[key] = after.opt_serialize(key);
    for (const std::string& key : before.keys())
        if (!after.has(key))
            changed[key] = nullptr;
    return changed;
}

json preferences_settings(const AppConfig& config, const json& args)
{
    if (!args.contains("keys") || args.contains("query") || args.contains("preset") || args.contains("compare"))
        throw ToolFailure("The preferences scope reads the keys named in keys, nothing else: it lists no preferences unasked, "
                          "since the app configuration also holds account and device state");
    json items = json::array(), unknown = json::array();
    for (const json& k : args["keys"]) {
        std::string value;
        if (config.get("app", k.get<std::string>(), value))
            items.push_back({{"key", k}, {"value", value}});
        else
            unknown.push_back(k);
    }
    json out = page(items, args, 100);
    if (!unknown.empty()) {
        out["unknown_keys"] = unknown;
        // Many preferences are stored only once they are changed.
        out["note"] = "unknown_keys are not stored: a preference never changed shows its default in the dialog (most "
                      "checkboxes unchecked), or the key is not a preference.";
    }
    return out;
}

std::string edit_filament_slot(int slot)
{
    PresetBundle&      bundle = *wxGetApp().preset_bundle;
    const std::string& name   = bundle.filament_presets[size_t(slot - 1)];
    const std::string  before = bundle.filaments.get_edited_preset().name;
    Tab*               tab    = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
    if (tab == nullptr || before == name)
        return {};
    // The filament tab edits one preset at a time; the normal switch asks about its unsaved changes.
    if (!tab->select_preset(name))
        throw ToolFailure("Switching the filament tab to \"" + name + "\" was cancelled");
    if (TabPresetComboBox* combo = tab->get_combo_box())
        combo->set_filament_idx(slot - 1);
    return before;
}

std::string to_config_string(const json& v, const std::string& key)
{
    const ConfigOptionDef* def = v.is_array() && !key.empty() ? print_config_def.get(key) : nullptr;
    // A point given as [x, y] is written "XxY", the way point options read it.
    const auto point = [](const json& e) -> std::optional<std::string> {
        if (!e.is_array() || e.size() != 2 || !e[0].is_number() || !e[1].is_number())
            return std::nullopt;
        return to_config_string(e[0]) + "x" + to_config_string(e[1]);
    };
    if (def != nullptr && def->type == coStrings) {
        std::vector<std::string> values;
        for (const json& e : v)
            values.push_back(to_config_string(e));
        return ConfigOptionStrings(values).serialize();
    }
    if (def != nullptr && def->type == coPoint)
        if (auto p = point(v))
            return *p;
    if (def != nullptr && def->type == coPoints) {
        std::string out;
        for (const json& e : v) {
            if (!out.empty())
                out += ",";
            const auto p = point(e);
            out += p ? *p : to_config_string(e);
        }
        return out;
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

namespace {

// get_settings: the settings `args` asks for, read on the UI thread.
json read_settings(const json& args)
{
    // A search with no scope looks through the process, filament and printer settings being edited, since a
    // question about an effect rarely knows which tab answers it; each match names its scope.
    if (args.contains("query") && !args.contains("scope") && !args.contains("preset") && !args.contains("compare")) {
        const char* const                  scopes[]{"process", "filament", "printer"};
        std::vector<std::string>           options;
        std::map<std::string, std::string> scope_of;
        for (const char* scope : scopes)
            for (const std::string& key : resolve_target(json{{"scope", scope}}, false).options)
                if (scope_of.emplace(key, scope).second)
                    options.push_back(key);
        const std::vector<std::string> ranked = rank_settings(arg_string(args, "query"), options, 200);
        std::map<std::string, json>    found;
        for (const char* scope : scopes) {
            json keys = json::array();
            for (const std::string& key : ranked)
                if (scope_of[key] == scope)
                    keys.push_back(key);
            if (keys.empty())
                continue;
            json scoped = args;
            scoped.erase("query");
            scoped.erase("offset");
            scoped.update({{"scope", scope}, {"keys", std::move(keys)}, {"limit", 500}});
            json read = read_settings(scoped);
            for (json& item : read["items"]) {
                item["scope"]         = scope;
                const std::string key = item["key"];
                found[key]            = std::move(item);
            }
        }
        json items = json::array();
        for (const std::string& key : ranked)
            if (const auto it = found.find(key); it != found.end())
                items.push_back(std::move(it->second));
        return page(items, args, 20);
    }
    if (arg_string(args, "scope") == "plate")
        return plate_settings(args);
    if (arg_string(args, "scope") == "preferences")
        return preferences_settings(*wxGetApp().app_config, args);
    const ConfigTarget t       = resolve_target(args, false);
    const bool         compare = args.contains("compare");
    if (int(args.contains("keys")) + int(args.contains("query")) + int(args.contains("where")) > 1)
        throw ToolFailure("Give one of keys, query or where");
    // A preset's whole listing, hundreds of keys, is for scanning values; what one key takes
    // comes with it when it is named, searched for or listed with its group.
    const bool whole = !args.contains("keys") && !args.contains("query") && !args.contains("where") && t.object == nullptr && !t.project;
    const bool meta  = arg_bool(args, "include_meta", !compare && !whole);
    if ((t.object != nullptr || t.project) && (compare || args.contains("preset")))
        throw ToolFailure("preset and compare are for the process, filament and printer scopes");
    // An object's or part's settings are process settings, on the process tab's pages.
    const std::map<std::string, std::string> places =
        t.project ? std::map<std::string, std::string>() : setting_places(t.object != nullptr ? Preset::TYPE_PRINT : t.type);
    std::vector<std::string> keys;
    if (args.contains("keys")) {
        for (const json& k : args["keys"])
            keys.push_back(k.get<std::string>());
    } else if (args.contains("query")) {
        keys = rank_settings(arg_string(args, "query"), t.options, 200);
    } else if (args.contains("where")) {
        const std::string     where = lower(arg_string(args, "where"));
        std::set<std::string> groups;
        for (const std::string& key : t.options)
            if (const auto place = places.find(key); place != places.end()) {
                if (lower(place->second) == where || lower(place->second).rfind(where + " > ", 0) == 0)
                    keys.push_back(key);
                groups.insert(place->second);
            }
        if (keys.empty())
            throw ToolFailure("No page or group \"" + arg_string(args, "where") + "\" in this scope", {{"groups", groups}});
    } else if (t.object != nullptr) {
        keys = t.config->keys();
    } else {
        keys = t.options;
    }
    std::vector<std::string> known;
    json                     unknown = json::array();
    for (const std::string& key : keys) {
        if (print_config_def.get(key) != nullptr && std::find(t.options.begin(), t.options.end(), key) != t.options.end())
            known.push_back(key);
        else
            unknown.push_back(key);
    }
    const auto extruders = printer_extruders();
    const auto add_meta  = [&](json& item, const std::string& key) {
        item["meta"] = option_meta(*print_config_def.get(key));
        if (const auto place = places.find(key); place != places.end())
            item["meta"]["where"] = place->second;
        if (t.project) {
            if (key == "curr_bed_type" || key == "prime_volume_mode")
                item["meta"]["offered"] = project_offered(key);
            if (project_key_read_only(key))
                item["meta"]["read_only"] = true;
            if (key == "flush_volumes_matrix")
                item["meta"]["description"] = "Purge volume in cubic millimeters for each change from one filament (row) to another "
                                              "(column), stored before flush_multiplier: the Flushing volumes dialog shows "
                                              "and edits each entry multiplied by its nozzle's flush_multiplier, and the "
                                              "G-code purges that product.";
        } else if (t.object == nullptr) {
            if (json v = variant_entries(key, *t.config, extruders.first, extruders.second); !v.is_null())
                item["meta"].update(v);
        }
    };
    json out;
    if (compare) {
        const Preset& other = compare_preset(t, arg_string(args, "compare"));
        json          items = preset_diff(*t.preset, other, known);
        if (meta)
            for (json& item : items)
                add_meta(item, item["key"].get<std::string>());
        const bool edited = t.edited && collection(t.type).current_is_dirty();
        out               = page(items, args, 100);
        out["left"]       = t.preset->name + (edited ? " (edited)" : "");
        out["right"]      = other.name;
    } else {
        // What an object or part prints with falls back to the process being edited, as its
        // settings tab shows it.
        const DynamicPrintConfig& process   = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        bool                      part_only = false;
        json                      items     = json::array();
        for (const std::string& key : known) {
            json item{{"key", key}};
            if (t.config->has(key))
                item["value"] = t.config->opt_serialize(key);
            else
                item["value"] = nullptr; // not overridden at this level
            if (t.object != nullptr) {
                item.update(effective_setting(key, process, *t.object, t.volume));
                part_only = part_only || item.contains("parts") || item.value("source", "") == "part";
            }
            if (meta)
                add_meta(item, key);
            items.push_back(std::move(item));
        }
        // A search's matches carry their descriptions, and those past the best few rarely
        // fit what was asked.
        out = page(items, args, args.contains("query") ? 20 : 100);
        if (part_only)
            out["note"] = "A part's value (source \"part\", or in parts) applies only inside that part's volume.";
    }
    if (!unknown.empty())
        out["unknown_keys"] = unknown;
    return out;
}

} // namespace

void register_config_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();
    const json    scope_prop = enum_prop({"print", "process", "filament", "printer", "object", "volume", "project", "plate", "preferences"},
                                         "Which settings: the process (print), filament or printer preset being edited, an "
                                         "object's / part's own overrides, the project's own settings, a plate's, or the "
                                         "app's preferences (read only).");
    const json    plate_prop = {{"type", "integer"}, {"minimum", 0}, {"description", "Plate index (plate scope; default: the current plate)."}};

    reg.add(inspect_tool(
        "list_presets", L("List presets"),
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
        "select_preset", L("Select a preset"),
        "Select a printer, process or filament preset by the name its selector shows (or its internal name), as a person "
        "would. With unsaved changes the Transfer-or-discard prompt opens (blocked_by_dialog) unless changes answers it. A "
        "filament is assigned to filament_slot (1-based, default 1). Reply: selected; when presets had or have unsaved "
        "changes, discarded and modified list per preset the keys dropped and the keys still unsaved.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Preset name as list_presets shows it.")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}},
                       {"changes", enum_prop({"transfer", "discard"}, "Unsaved changes of the presets the switch replaces: move "
                                                                       "them to the new preset or drop them, as the prompt's "
                                                                       "buttons do. Omitted: the prompt opens.")}},
                      {"type", "name"}),
        [](const json& args, ToolContext& ctx) {
            const Preset::Type type    = preset_type(arg_string(args, "type"));
            const std::string  name    = arg_string(args, "name");
            const std::string  changes = arg_string(args, "changes");
            json result = ctx.run_ui([&]() -> json {
                Plater&       plater = ui_plater();
                PresetBundle& bundle = *wxGetApp().preset_bundle;
                Tab*          tab    = wxGetApp().get_tab(type);
                if (tab == nullptr)
                    throw ToolFailure("The settings tab is not available");
                const int             slot = arg_int(args, "filament_slot", 1);
                PlaterPresetComboBox* filament_combo = nullptr;
                int                   filament_item  = -1;
                std::string           target         = name;
                if (type == Preset::TYPE_FILAMENT) {
                    auto& combos = plater.sidebar().combos_filament();
                    if (slot < 1 || slot > int(combos.size()) || combos[size_t(slot - 1)] == nullptr)
                        throw ToolFailure("filament_slot must be 1.." + std::to_string(combos.size()));
                    filament_combo   = combos[size_t(slot - 1)];
                    const auto match = filament_combo->find_by_name(name);
                    if (match.ambiguous)
                        throw ToolFailure("\"" + name + "\" is ambiguous in filament slot " + std::to_string(slot) + "'s selector; use the internal name");
                    if (match.item < 0)
                        throw ToolFailure("Filament slot " + std::to_string(slot) + " does not offer \"" + name + "\" (see list_presets)");
                    filament_item = match.item;
                    target        = match.internal_name;
                } else if (TabPresetComboBox* combo = tab->get_combo_box(); combo != nullptr && combo->GetCount() > 0) {
                    const auto match = combo->find_by_name(name);
                    if (match.ambiguous)
                        throw ToolFailure("\"" + name + "\" is ambiguous; use the internal name");
                    if (match.item < 0)
                        throw ToolFailure("The selector does not offer \"" + name + "\" (see list_presets)");
                    target = match.internal_name;
                }
                // The prompt about the tab's own changes offers Transfer only where can_transfer_changes_to() says;
                // refused here, before anything changes. The sidebar switches the filament tab for a single filament only.
                const bool switches_tab = type != Preset::TYPE_FILAMENT || !plater.sidebar().is_multifilament();
                if (changes == "transfer" && switches_tab && collection(type).current_is_dirty() && !tab->can_transfer_changes_to(target))
                    throw ToolFailure(std::string("The unsaved-changes prompt offers no Transfer here: ") +
                                      (type == Preset::TYPE_PRINTER ? "a printer's changes stay with it" :
                                                                      "\"" + target + "\" is a filament of another type") +
                                      ". Pass changes: \"discard\", or save_preset first.");
                const json before = unsaved_changes();
                tab->m_dirty_preset_answer = changes == "transfer" ? Tab::DirtyPresetAnswer::Transfer :
                                             changes == "discard"  ? Tab::DirtyPresetAnswer::Discard :
                                                                     Tab::DirtyPresetAnswer::Ask;
                ScopeGuard ask_again([tab] { tab->m_dirty_preset_answer = Tab::DirtyPresetAnswer::Ask; });
                json out;
                if (filament_combo != nullptr) {
                    // The sidebar's own selection path: mapping, flush volumes, badges, project state.
                    filament_combo->SetSelection(filament_item);
                    wxCommandEvent evt(wxEVT_COMBOBOX, filament_combo->GetId());
                    evt.SetEventObject(filament_combo);
                    evt.SetInt(filament_item);
                    evt.SetString(filament_combo->GetString(filament_item));
                    filament_combo->GetEventHandler()->ProcessEvent(evt);
                    out = {{"type", "filament"}, {"filament_slot", slot}, {"selected", bundle.filament_presets[size_t(slot - 1)]}};
                } else {
                    if (!tab->select_preset(target))
                        throw ToolFailure("Selecting \"" + target + "\" was cancelled");
                    out = {{"type", arg_string(args, "type")}, {"selected", collection(type).get_selected_preset_name()}};
                }
                out.update(unsaved_changes_outcome(before));
                return out;
            });
            settle(ctx);
            return ToolResult::ok(result);
        }));

    reg.add(mutate_tool(
        "save_preset", L("Save a preset"),
        "Save the printer, process or filament preset being edited, unsaved changes included, under name, as the tab's Save "
        "and the Save preset dialog's OK do; it is then selected. A filament is filament_slot's (default 1), and every slot "
        "using the old preset takes the new one. Replacing another existing preset needs overwrite (pass it in scripts meant "
        "to be rerun). Reply: saved, user, project, inherits, filament_slots. A temperature warning or an unsaved-changes "
        "prompt makes it blocked_by_dialog. To keep only some edits, set_settings {reset_all: true, values} first.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Name to save under.")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}},
                       {"inside_project", prop("boolean", "Keep it inside the project, not as a user preset. Default: where the "
                                                          "edited preset is.")},
                       {"detach", prop("boolean", "Copy inherited values and drop the parent (Developer mode).")},
                       {"overwrite", prop("boolean", "Replace another preset of that name.")}},
                      {"type", "name"}),
        [](const json& args, ToolContext& ctx) {
            const Preset::Type type   = preset_type(arg_string(args, "type"));
            const std::string  name   = arg_string(args, "name");
            const bool         detach = arg_bool(args, "detach", false);
            json result = ctx.run_ui([&]() -> json {
                ui_plater();
                // The dialog shows its detach checkbox only in Developer mode.
                if (detach && wxGetApp().get_mode() != comDevelop)
                    throw ToolFailure("Detach is offered only in Developer mode; run_action orca_command:toggle_developer_mode:orca first");
                Tab* tab = wxGetApp().get_tab(type);
                if (tab == nullptr)
                    throw ToolFailure("The settings tab is not available");
                // The filament tab edits one slot's preset; edit this slot's, as set_settings does.
                if (type == Preset::TYPE_FILAMENT)
                    resolve_target({{"scope", "filament"}, {"filament_slot", arg_int(args, "filament_slot", 1)}}, true);
                PresetCollection& presets        = collection(type);
                const bool        inside_project = save_preset_inside_project(presets, name, args);
                // With the name given, the tab saves without opening the dialog.
                if (!tab->save_preset(name, detach, inside_project))
                    throw ToolFailure("Saving was cancelled");
                // After the save, so a cancelled save leaves the physical printer alone.
                if (type == Preset::TYPE_PRINTER)
                    SavePresetDialog::update_physical_printers(SavePresetDialog::ChangePreset, name);
                const Preset* saved = presets.find_preset(name, false);
                if (saved == nullptr)
                    throw ToolFailure("The preset was not saved");
                json out{{"saved", saved->name},
                         {"type", arg_string(args, "type")},
                         {"user", saved->is_user()},
                         {"project", saved->is_project_embedded},
                         {"selected", presets.get_selected_preset_name() == saved->name}};
                if (!saved->inherits().empty())
                    out["inherits"] = saved->inherits();
                if (type == Preset::TYPE_FILAMENT)
                    out["filament_slots"] = filament_slots(saved->name);
                return out;
            });
            settle(ctx);
            return ToolResult::ok(result);
        }));

    reg.add(mutate_tool(
        "delete_preset", L("Delete a preset"),
        "Delete a user preset or one inside the project, its file and its cloud copy, as the tab's Delete does, without "
        "asking. A selected preset is replaced as the tab does (its parent or the next preset). Another filament is deleted "
        "without changing the selection, and slots using it take the selected one; another process is selected, deleted "
        "and the selection restored; a printer must be selected first. Refused: system presets, presets others inherit from, "
        "another preset while the edited one of its type has unsaved changes. Deleting a selected printer or process can "
        "ask about other presets' unsaved changes (blocked_by_dialog). Reply: deleted, selected, filaments (per slot), "
        "attached, and discarded/modified as select_preset's.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Preset name as list_presets shows it.")},
                       {"delete_attached", prop("boolean", "A custom printer: also delete the process and filament presets made "
                                                           "for it alone, which the GUI deletes with it.")}},
                      {"type", "name"}),
        [](const json& args, ToolContext& ctx) {
            const std::string  type_name = arg_string(args, "type");
            const Preset::Type type      = preset_type(type_name);
            const std::string  name      = arg_string(args, "name");
            json result = ctx.run_ui([&]() -> json {
                Plater&       plater = ui_plater();
                PresetBundle& bundle = *wxGetApp().preset_bundle;
                Tab*          tab    = wxGetApp().get_tab(type);
                if (tab == nullptr)
                    throw ToolFailure("The settings tab is not available");
                PresetCollection& presets  = collection(type);
                const json        attached = check_preset_deletable(presets, name, arg_bool(args, "delete_attached", false), bundle.prints,
                                                                    bundle.filaments);
                const std::string selected = presets.get_selected_preset_name();
                const json        before   = unsaved_changes();
                if (name == selected) {
                    // With a physical printer selected, the tab's Delete only takes the preset off that printer.
                    if (TabPresetComboBox* combo = tab->get_combo_box(); type == Preset::TYPE_PRINTER && combo != nullptr && combo->is_selected_physical_printer())
                        throw ToolFailure("A physical printer is selected; select the printer preset itself first");
                    tab->delete_preset(false);
                } else if (type == Preset::TYPE_PRINTER) {
                    // Switching to another printer and back recomputes the process and the filaments, so it is no round trip.
                    throw ToolFailure("\"" + name + "\" is not the selected printer; select it first with select_preset");
                } else if (presets.current_is_dirty()) {
                    // Both routes select the edited preset again, which would drop or ask about its changes.
                    throw ToolFailure("The edited " + type_name + " \"" + selected + "\" has unsaved changes; save_preset or set_settings "
                                      "{reset_all: true} first");
                } else if (type == Preset::TYPE_FILAMENT) {
                    delete_unselected_filament_preset(name);
                    // What the Edit filament dialog's opener refreshes once it closes (Plater::priv::on_modify_filament()).
                    wxGetApp().mainframe->update_side_preset_ui();
                    plater.update_ui_from_settings();
                    plater.sidebar().update_all_preset_comboboxes();
                } else {
                    // The tab deletes only its selected process, so this is what a person does: select, delete, select back.
                    if (!tab->select_preset(name))
                        throw ToolFailure("Selecting \"" + name + "\" to delete it was cancelled");
                    if (presets.get_selected_preset_name() == name)
                        tab->delete_preset(false);
                    if (presets.get_selected_preset_name() != selected)
                        tab->select_preset(selected);
                }
                if (presets.find_preset(name, false) != nullptr)
                    throw ToolFailure("\"" + name + "\" was not deleted: a preset switch was cancelled");
                json out{{"deleted", name},
                         {"type", type_name},
                         {"selected", presets.get_selected_preset_name()},
                         {"filaments", bundle.filament_presets}};
                if (!attached.empty())
                    out["attached"] = attached;
                out.update(unsaved_changes_outcome(before));
                return out;
            });
            settle(ctx);
            return ToolResult::ok(result);
        },
        true));

    reg.add(inspect_tool(
        "get_settings", L("Read settings"),
        "Read setting values (as OrcaSlicer serializes them) with meta: label, description (what the setting does, when to "
        "change it, what it affects), where (its settings page and group), type, unit, range, allowed values and their "
        "labels, and mode. Name keys; find the settings for an effect with query, a few words matched in keys, labels and "
        "descriptions, best matches first; or list one page or group with where (an unknown one lists them all). Without "
        "these, every option of the scope is listed, paged, without meta in a preset scope. A per-variant value lists in "
        "meta its variants (what each entry is for) and active (the entry each "
        "extruder prints with). preset reads a saved preset without selecting it; compare lists only the keys that differ "
        "from another preset, each with value and other. For object/volume scopes only the overrides that object or part "
        "carries are listed, each with effective (the value it prints with) and source (part, object, process or default); "
        "an object's item lists in parts the parts and modifiers that override the key inside their own volume. The project "
        "scope has the plate type, slot colours, flushing volumes, purge mode, prime tower position per plate, which slots are "
        "mixed and, with several extruders, the filament grouping; meta.offered lists the choices this printer has. The plate "
        "scope lists a plate's curr_bed_type, print_sequence, spiral_mode, first_layer_print_sequence, "
        "other_layers_print_sequence, name and locked, each with value (the plate's own, null where it follows the global "
        "setting or Auto) and effective. The preferences scope reads the named keys of the app configuration (the Preferences "
        "dialog's settings, keys as its controls are named), values as stored, and lists no keys unasked.",
        object_schema({{"scope", scope_prop},
                       {"keys", array_prop({{"type", "string"}}, "Option keys, e.g. [\"layer_height\", \"wall_loops\"].")},
                       {"query", prop("string", "A few distinctive words, e.g. \"stringing\" or \"ooze travel\"; the best 20 "
                                                "matches unless limit says otherwise.")},
                       {"where", prop("string", "A settings page, or \"Page > Group\" as meta.where gives it, e.g. "
                                                "\"Strength > Walls\".")},
                       {"object", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (object/volume scope)."}}},
                       {"volume", {{"type", "integer"}, {"minimum", 0}, {"description", "Part index (volume scope)."}}},
                       {"plate", plate_prop},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}},
                       {"preset", prop("string", "Preset scopes: read this saved preset (a name from list_presets) instead of the "
                                                 "one being edited.")},
                       {"compare", prop("string", "Preset scopes: a preset name, \":saved\" (the unsaved changes) or \":parent\" "
                                                  "(the preset it inherits from).")},
                       {"include_meta", prop("boolean", "Include labels, types and ranges. Default true, false for a preset "
                                                       "scope's whole listing and with compare.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                return read_settings(args);
            }));
        }));

    reg.add(mutate_tool(
        "set_settings", L("Change settings"),
        "Change settings as if typed into the settings tab: all values are parsed strictly first and applied together, and "
        "nothing changes if any is invalid. Values may be strings in OrcaSlicer's format (\"15%\", \"0.2\", \"1,2\") or JSON "
        "numbers, booleans and arrays (a point as [x, y]). object/volume scopes set per-object or per-part overrides, of the "
        "keys the object list offers there; a null value removes an override. extruder is the object's or part's filament "
        "(1-based; 0 or null is Default), set as the Change Filament menu does. A preset's per-variant value (get_settings "
        "meta.variants) takes a single value for the entries in use (meta.active, as the tab writes the variant it shows), "
        "{\"index\": i, \"value\": v} for one entry, or an array of all. "
        "In a preset scope, null takes a key back to its saved value (reset_to parent: the parent's), and reset_all first "
        "discards every unsaved change, as the tab's undo button does (reply: reverted), then applies values. Reply: applied, "
        "and changed, every setting the call changed, including those the app adjusted in turn. Spiral vase and some others "
        "confirm their adjustments in a dialog (blocked_by_dialog). The project scope writes as the sidebar's controls do: "
        "curr_bed_type, filament_colour (#RRGGBB per slot), flush_multiplier, flush_volumes_matrix (per nozzle, slots x slots "
        "from row to column, in config order over every slot, mixed ones left as they are; before the multiplier, which the "
        "G-code header applies; a colour change can recalculate it, so set it last), prime_volume_mode, and wipe_tower_x/y "
        "(mm from the plate's origin, one per plate; undoable). The plate scope writes as the Plate Settings dialog's OK does, "
        "after selecting the plate as its icon does: curr_bed_type, print_sequence (\"by layer\", \"by object\"), "
        "spiral_mode (true asks for confirmation: blocked_by_dialog), first_layer_print_sequence (every slot once, e.g. [2, "
        "1]) and other_layers_print_sequence ([{from: layer >= 2, to, order}], to omitted for the last layer), null for the "
        "global setting or Auto; name, and locked (true or false). One undo step.",
        object_schema({{"scope", scope_prop},
                       {"values", {{"type", "object"}, {"description", "{key: value}, or {key: {index, value}} for one entry"}}},
                       {"reset_all", prop("boolean", "Preset scopes: discard the unsaved changes before applying values.")},
                       {"reset_to", enum_prop({"saved", "parent"}, "Preset scopes: what a null value takes a key back to: the "
                                                                     "saved preset (default) or the preset it inherits from.")},
                       {"object", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (object/volume scope)."}}},
                       {"volume", {{"type", "integer"}, {"minimum", 0}, {"description", "Part index (volume scope)."}}},
                       {"plate", plate_prop},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}}}),
        [](const json& args, ToolContext& ctx) {
            // A preference's control carries its effects (theme, language, network, this server's own port), which
            // writing the key would skip.
            if (arg_string(args, "scope") == "preferences")
                throw ToolFailure("Preferences change in the Preferences dialog: run_action orca_command:open_preferences:orca "
                                  "with param \"<tab>:<key>\", then ui_input {\"target\": {\"name\": \"<key>\", \"within\": "
                                  "\"Preferences\"}, \"value\": ...}");
            if (arg_string(args, "scope") == "project")
                return set_project_settings(args, ctx);
            if (arg_string(args, "scope") == "plate")
                return set_plate_settings(args, ctx);
            const json values_in = args.value("values", json::object());
            const bool reset_all = arg_bool(args, "reset_all", false);
            if (values_in.empty() && !reset_all)
                throw ToolFailure("values must be a non-empty object {key: value}, unless reset_all is true");
            if (args.contains("reset_to") && std::none_of(values_in.begin(), values_in.end(), [](const json& v) { return v.is_null(); }))
                throw ToolFailure("reset_to applies to the keys given as null");
            json result = ctx.run_ui([&]() -> json {
                ui_plater();
                const ConfigTarget t = resolve_target(args, true);
                if ((reset_all || args.contains("reset_to")) && t.object != nullptr)
                    throw ToolFailure(std::string(reset_all ? "reset_all" : "reset_to") + " is for preset scopes; here a null value removes an override");
                // What a null value takes a preset's key back to: its saved value, as the key's undo arrow does,
                // or the parent's, as its lock icon does.
                const Preset* reference = t.object != nullptr ? nullptr :
                                          arg_string(args, "reset_to", "saved") == "parent" ? &compare_preset(t, ":parent") :
                                                                                               &collection(t.type).get_selected_preset();
                // A preset's value is checked against what it will replace: after reset_all, the saved preset.
                const DynamicPrintConfig&                        basis = reset_all ? collection(t.type).get_selected_preset().config : *t.config;
                const auto                                       extruders = printer_extruders();
                const DynamicPrintConfig                         before    = *t.config;
                std::vector<std::pair<std::string, std::string>> values;
                std::vector<std::string>                         removals;
                for (auto it = values_in.begin(); it != values_in.end(); ++it) {
                    if (std::find(t.options.begin(), t.options.end(), it.key()) == t.options.end())
                        throw ToolFailure(t.object != nullptr && print_config_def.get(it.key()) != nullptr ?
                                              "The object list does not offer \"" + it.key() + "\" for " +
                                                  (t.volume != nullptr ? "a part or modifier" : "an object") :
                                              "Unknown setting \"" + it.key() + "\" for this scope",
                                          {{"similar", search_keys(it.key().substr(0, std::max<std::size_t>(3, it.key().size() / 2)), t.options, 5)}});
                    if (it.value().is_null() && reference != nullptr) {
                        if (!reference->config.has(it.key()))
                            throw ToolFailure("\"" + reference->name + "\" has no " + it.key());
                        values.emplace_back(it.key(), reference->config.opt_serialize(it.key()));
                    } else if (it.value().is_null()) {
                        removals.push_back(it.key());
                    } else if (t.object == nullptr) {
                        values.emplace_back(it.key(), preset_setting_value(it.key(), it.value(), basis, extruders.first, extruders.second));
                    } else if (it.value().is_object()) {
                        throw ToolFailure("{\"index\", \"value\"} sets one entry of a preset's setting; an override takes the whole value");
                    } else {
                        values.emplace_back(it.key(), to_config_string(it.value(), it.key()));
                    }
                }
                // Strict-parses every value into a scratch config, so a bad one fails before anything changes.
                const auto parse_values = [&values] {
                    DynamicPrintConfig scratch;
                    for (const auto& [key, value] : values)
                        try {
                            scratch.set_deserialize_strict(key, value);
                        } catch (const std::exception& e) {
                            throw ToolFailure("Invalid value for " + key + ": " + e.what());
                        }
                    return scratch;
                };
                json reverted;
                if (t.object == nullptr) {
                    Tab* tab = wxGetApp().get_tab(t.type);
                    if (tab == nullptr)
                        throw ToolFailure("The settings tab is not available");
                    if (reset_all) {
                        parse_values();
                        reverted = collection(t.type).current_dirty_options();
                        tab->on_roll_back_value();
                    }
                    try {
                        tab->apply_preset_config_values(values);
                    } catch (const std::exception& e) {
                        throw ToolFailure(e.what());
                    }
                } else {
                    DynamicPrintConfig scratch = parse_values();
                    check_filament_numbers(scratch);
                    // The filament goes through the object list's Change Filament, as a person sets it;
                    // null is its "Default" (0).
                    const bool     set_extruder = scratch.has("extruder") ||
                                              std::find(removals.begin(), removals.end(), "extruder") != removals.end();
                    const int      extruder     = scratch.has("extruder") ? scratch.opt_int("extruder") : 0;
                    wxDataViewItem extruder_row = set_extruder ? filament_row(t) : wxDataViewItem();
                    scratch.erase("extruder");
                    removals.erase(std::remove(removals.begin(), removals.end(), "extruder"), removals.end());

                    Plater& plater = ui_plater();
                    // One undo step for the whole call: the filament's own snapshot folds into this one.
                    Plater::SingleSnapshot single(&plater);
                    if (!scratch.empty() || !removals.empty()) {
                        plater.take_snapshot("Change object settings");
                        ModelConfig& cfg = t.volume != nullptr ? static_cast<ModelConfig&>(t.volume->config) : static_cast<ModelConfig&>(t.object->config);
                        for (const std::string& key : scratch.keys())
                            cfg.set_key_value(key, scratch.option(key)->clone());
                        for (const std::string& key : removals)
                            cfg.erase(key);
                        wxGetApp().obj_list()->object_config_options_changed(ObjectVolumeID{t.object, t.volume});
                        plater.changed_object(t.object_index);
                    }
                    if (set_extruder) {
                        wxDataViewItemArray rows;
                        rows.Add(extruder_row);
                        wxGetApp().obj_list()->set_extruder_for_items(rows, extruder);
                    }
                }
                const ConfigTarget after = resolve_target(args, false);
                json applied = json::object();
                for (const auto& [key, value] : values)
                    applied[key] = after.config->has(key) ? json(after.config->opt_serialize(key)) : json();
                for (const std::string& key : removals)
                    applied[key] = nullptr;
                json out{{"applied", applied}, {"changed", changed_settings(before, *after.config)}};
                if (reset_all)
                    out["reverted"] = std::move(reverted);
                if (values_in.contains("extruder")) {
                    // A single-part object shows no part row; its filament is the object's.
                    const bool                on_object = after.volume != nullptr && after.object->volumes.size() == 1;
                    const DynamicPrintConfig& cfg       = on_object ? after.object->config.get() : *after.config;
                    out["applied"]["extruder"]          = cfg.has("extruder") ? json(cfg.opt_serialize("extruder")) : json();
                    if (on_object)
                        out["note"] = "A single-part object's filament is the object's; it was set there.";
                }
                return out;
            });
            settle(ctx);
            return ToolResult::ok(result);
        }));

    reg.add(inspect_tool(
        "list_plates", L("List plates"),
        "Plates with name, locked, current, objects ([object, instance]), and the settings they print with: curr_bed_type, "
        "print_sequence, spiral_mode, first_layer_print_sequence and other_layers_print_sequence (null: Auto); overrides "
        "lists those the plate sets itself (set_settings scope plate). Also sliced (an up-to-date slice), wipe_tower [x, y] "
        "and layer_gcode: the plate's pauses, custom G-code, templates and filament changes, each {z, layer, type, "
        "filament, color, gcode}; layer is the Preview slider's number, given while the plate's slice is up to date.",
        object_schema({{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return list_plates(args); })); }));
}

}}} // namespace Slic3r::GUI::Mcp
