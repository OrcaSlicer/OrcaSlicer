// The place Presets and settings: preset_list, preset_select, preset_save, preset_delete, settings_list, settings_get
// and settings_set. Presets are selected through the same selectors a person uses, saved and deleted through the
// tab, and settings change through the Tab's own transaction (strict parsing, on_change callbacks, rollback on
// failure), so the UI and the project stay consistent. The project's and the plates' own settings, and the
// filament list's flushing, are read and written by the code here too, through the code of their sidebar controls
// and dialogs; their commands are in ProjectTools.cpp, PlateTools.cpp and FilamentTools.cpp. Also the commands made
// from the presets' and the settings mode's menu items.

#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpTargets.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/CreatePresetsDialog.hpp"
#include "slic3r/GUI/FilamentMapPanel.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PresetOps.hpp"
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
#include "libslic3r/FlushVolCalc.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/dataview.h>
#include <wx/defs.h>
#include <wx/string.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
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
    if (name == "process")
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

// The settings of a preset type, as its tab and settings_set take them.
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

// The refusal of `text`, which is no value of setting `key`: it names the value and what the setting takes, its kind
// and its range or values, as settings_get's meta gives them, so the caller can correct the value without reading the
// setting first ("Invalid value for wall_loops: \"three\"; it takes an integer from 0 to 1000").
ToolFailure invalid_value(const std::string& key, const std::string& text)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    std::string            takes; // what the setting takes, e.g. "an integer from 0 to 1000"; empty when nothing is known
    if (def != nullptr) {
        // A setting of several values (one per extruder, nozzle or filament) takes a list of what one value is.
        const bool             list = (int(def->type) & int(coVectorType)) != 0;
        const ConfigOptionType one  = list ? ConfigOptionType(int(def->type) - int(coVectorType)) : def->type;
        std::string            kind;
        switch (one) {
        case coInt: kind = list ? "integers" : "an integer"; break;
        case coFloat: kind = list ? "numbers" : "a number"; break;
        case coPercent: kind = list ? "percentages" : "a percentage (\"50%\")"; break;
        case coFloatOrPercent: kind = list ? "numbers or percentages" : "a number or a percentage (\"50%\")"; break;
        case coBool: kind = list ? "booleans" : "true or false"; break;
        case coEnum: {
            // The first values only where there are many: settings_get lists them all with their labels.
            constexpr std::size_t shown = 10;
            std::string           values;
            for (std::size_t i = 0; i < def->enum_values.size() && i < shown; ++i)
                values += (i > 0 ? ", " : "") + def->enum_values[i];
            if (def->enum_values.size() > shown)
                values += ", ... (settings_get lists all " + std::to_string(def->enum_values.size()) + ")";
            kind = values.empty() ? std::string() : std::string(list ? "values each one of " : "one of ") + values;
            break;
        }
        default: break; // text and points: their form is in the setting's description
        }
        const bool  low  = def->min > -FLT_MAX;
        const bool  high = def->max < FLT_MAX;
        std::string range;
        if (low && high)
            range = "from " + float_to_string_decimal_point(def->min) + " to " + float_to_string_decimal_point(def->max);
        else if (low)
            range = "at least " + float_to_string_decimal_point(def->min);
        else if (high)
            range = "at most " + float_to_string_decimal_point(def->max);
        if (one == coEnum) // its values say it all
            range.clear();
        if (!kind.empty())
            takes = list ? "a list of " + kind + (range.empty() ? "" : ", each " + range) :
                           kind + (range.empty() ? "" : (low && high ? " " : ", ") + range);
    }
    return ToolFailure("Invalid value for " + key + ": \"" + text + "\"" + (takes.empty() ? std::string() : "; it takes " + takes),
                       {{"code", "invalid_argument"}});
}

// Option keys whose key or label contains `query`, for "did you mean".
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

// Up to five keys of `within` like `key`, which a read or a write of settings does not know: those holding its first
// half, at least three letters, as a misspelt or shortened key does ("wall_loop" finds "wall_loops").
std::vector<std::string> similar_keys(const std::string& key, const std::vector<std::string>& within)
{
    return search_keys(key.substr(0, std::max<std::size_t>(3, key.size() / 2)), within, 5);
}

// The "Page > Group" of each setting on the settings tab of `type`, as the tabs register their rows
// when they build their pages; a setting with no row on a page has none. A printer's extruder settings
// have a row per extruder, "Extruder N" on a printer of several so that a search hit picks that
// extruder, and all of them are on its one Extruder page (TabPrinter::activate_option()).
std::map<std::string, std::string> setting_places(Preset::Type type)
{
    std::map<std::string, std::string> places;
    for (const Search::Option& row : wxGetApp().sidebar().settings_index().all_options())
        if (row.type == type) {
            const std::string key = row.opt_key();
            wxString          page = row.category;
            wxString          number;
            long              n = 0;
            if (type == Preset::TYPE_PRINTER && page.StartsWith("Extruder ", &number) && number.ToLong(&n))
                page = "Extruder";
            places.emplace(key.substr(0, key.find('#')), into_u8(page) + " > " + into_u8(row.group));
        }
    return places;
}

// Preset names that share the most words with `name`, those compatible with the printer first, for a
// name no preset has.
json similar_presets(const PresetCollection& presets, const std::string& name) { return Ops::similar_preset_names(presets, name); }

// The settings a call reads or writes: a type's preset (target global), an object's, a part's or a height range's
// overrides, or the project's own settings.
struct ConfigTarget
{
    Preset::Type        type = Preset::TYPE_INVALID; // a type's preset
    const Preset*       preset = nullptr;            // the preset read: the one being edited, or a saved one
    bool                edited = false;              // config is the edited preset's, unsaved changes included
    std::string         target = "global";           // the targets object, part and height_range
    ModelObject*        object = nullptr;
    ModelVolume*        volume = nullptr;
    int                 object_index = -1;
    std::optional<t_layer_height_range> range;       // the height range's heights, a key of the object's ranges
    Targets             named;                       // what the reply names: the object, or the part
    bool                project = false;             // the project's settings, read from `owned`
    std::shared_ptr<const DynamicPrintConfig> owned;
    const DynamicPrintConfig* config = nullptr;
    std::vector<std::string>  options; // the keys these settings have
};

// The project's settings: the project's own settings (PresetBundle::project_config) that a person sets for its plates,
// with the plate type selector and a prime tower's drag, and the filament grouping, which the Filament grouping dialog
// sets for each plate and which is listed for reading only. The same config holds the filament list's state, its
// colours, mixed filaments and flushing, which filament_list reads instead (filament_list_hint()).
bool project_key_read_only(const std::string& key)
{
    return key == "filament_map_mode" || key == "filament_map";
}

std::vector<std::string> project_options()
{
    std::vector<std::string> keys{"curr_bed_type", "wipe_tower_x", "wipe_tower_y"};
    // The filament grouping of a printer with several extruders.
    if (wxGetApp().preset_bundle->get_printer_extruder_count() > 1)
        keys.insert(keys.end(), {"filament_map_mode", "filament_map"});
    return keys;
}

// The flushing that the filament list's Flushing volumes and Purge mode buttons set: filament_list reads it and
// filament_set_flushing writes it, with the code project_set_settings runs for its own keys (set_project_values()).
const std::vector<std::string>& flushing_options()
{
    static const std::vector<std::string> keys{"flush_multiplier", "flush_volumes_matrix", "prime_volume_mode"};
    return keys;
}

// Where a key of the filament list's state is read and set, for a call that names it among the project's settings;
// empty for any other key.
std::string filament_list_hint(const std::string& key)
{
    if (key == "filament_colour")
        return "filament_list reads the slot colours and filament_set_colour sets one";
    if (key == "filament_is_mixed")
        return "filament_list reads which slots are mixed filaments";
    const std::vector<std::string>& flushing = flushing_options();
    if (std::find(flushing.begin(), flushing.end(), key) != flushing.end())
        return "filament_list reads it and filament_set_flushing sets it";
    return {};
}

} // namespace

std::vector<unsigned char> mixed_slots()
{
    const PresetBundle&        bundle = *wxGetApp().preset_bundle;
    std::vector<unsigned char> mixed;
    for (std::size_t i = 0; i < bundle.project_config.option<ConfigOptionStrings>("filament_colour")->size(); ++i)
        mixed.push_back(bundle.is_mixed_filament(i));
    return mixed;
}

namespace {

// The types settings_get looks through when keys come without one: the process, filament and printer settings being
// edited, since a key read for what it does rarely comes with the tab that holds it.
const char* const kSearchTypes[]{"process", "filament", "printer"};

// Where `key` is a setting, for a call that named it where it isn't one: the first of kSearchTypes that has it, else
// "project" for the project's own settings; empty when none does. UI thread, but for a key that names no config option,
// which is no project setting either, and is answered without the app.
std::string type_of_key(const std::string& key)
{
    for (const char* type : kSearchTypes)
        if (const std::vector<std::string>& options = preset_options(preset_type(type));
            std::find(options.begin(), options.end(), key) != options.end())
            return type;
    // The project's nozzle flow types, read and set with the printer's settings (resolve_target()).
    if (key == "nozzle_volume_type")
        return "printer";
    if (!print_config_def.has(key))
        return {};
    if (!filament_list_hint(key).empty())
        return "filament_list";
    const std::vector<std::string> project = project_options();
    return std::find(project.begin(), project.end(), key) != project.end() ? "project" : "";
}

} // namespace

void add_unknown_keys(json& out, const json& unknown, const std::vector<std::string>& within)
{
    if (unknown.empty())
        return;
    out["unknown_keys"] = unknown;
    json types          = json::object();
    json similar        = json::object();
    for (const json& key : unknown) {
        const std::string name = key.get<std::string>();
        if (std::string type = type_of_key(name); !type.empty())
            types[name] = std::move(type);
        else if (std::vector<std::string> like = similar_keys(name, within); !like.empty())
            similar[name] = std::move(like);
    }
    if (!types.empty())
        out["type_of"] = std::move(types);
    if (!similar.empty())
        out["similar"] = std::move(similar);
}

ToolFailure unknown_setting(const std::string& key, const std::string& where, json details)
{
    const json code = {{"code", "invalid_argument"}};
    if (const std::string hint = filament_list_hint(key); !hint.empty())
        return ToolFailure("\"" + key + "\" belongs to the filament list: " + hint, code);
    if (const std::string other = type_of_key(key); !other.empty() && other != where)
        return ToolFailure(other == "project" ? "\"" + key + "\" is one of the project's own settings: project_set_settings changes it" :
                                                "\"" + key + "\" is a " + other + " setting: settings_set takes it with type \"" + other + "\"",
                           code);
    details.update(code);
    return ToolFailure("Unknown setting \"" + key + "\" for the " + where + " settings", std::move(details));
}

DynamicPrintConfig project_settings()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    DynamicPrintConfig  out;
    for (const std::vector<std::string>& keys : {project_options(), flushing_options()})
        for (const std::string& key : keys)
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
    return out;
}

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

ToolResult set_project_values(const json& values_in, ToolContext& ctx, bool flushing)
{
    DynamicPrintConfig before;
    DynamicPrintConfig values; // checked, in the form project_settings() reads them
    // The matrix to write, given whole or as one entry of `basis`'s matrix.
    const auto checked_matrix = [&](const DynamicPrintConfig& basis) {
        const char* const  key = "flush_volumes_matrix";
        DynamicPrintConfig given;
        given.set_deserialize_strict(key, preset_setting_value(key, values_in[key], basis, {}, {}));
        const std::vector<unsigned char> mixed = mixed_slots();
        return check_flush_matrix(given.option<ConfigOptionFloats>(key)->values, mixed.size(),
                                  (values.has("flush_multiplier") ? values : before).option<ConfigOptionFloats>("flush_multiplier")->values,
                                  basis.option<ConfigOptionFloats>(key)->values, mixed);
    };
    ctx.run_ui([&]() -> json {
        ui_plater();
        before                                 = project_settings();
        const std::vector<std::string> options = flushing ? flushing_options() : project_options();
        for (auto it = values_in.begin(); it != values_in.end(); ++it) {
            const std::string& key = it.key();
            if (std::find(options.begin(), options.end(), key) == options.end())
                throw unknown_setting(key, flushing ? "filament list" : "project", {{"keys", options}});
            if (project_key_read_only(key))
                throw ToolFailure(key + " is read-only here: the Filament grouping dialog sets it for each plate, and every plate keeps "
                                        "its own grouping");
            if (it.value().is_null())
                throw ToolFailure(key + " always has a value; null is for object overrides and preset keys");
            const std::string text = preset_setting_value(key, it.value(), before, {}, {});
            try {
                values.set_deserialize_strict(key, text);
            } catch (const std::exception&) {
                throw invalid_value(key, text);
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
        return json();
    });
    json reply = ctx.run_ui([&]() -> json {
        Plater&       plater = ui_plater();
        PresetBundle& bundle = *wxGetApp().preset_bundle;
        if (values.has("curr_bed_type"))
            plater.sidebar().set_bed_type_accord_combox(values.opt_enum<BedType>("curr_bed_type"));
        bool update = false;
        if (values.has("flush_volumes_matrix") || values.has("flush_multiplier")) {
            // Without a matrix, the stored one, which the new multiplier scales.
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

namespace {

// preset_sync: signed out, a pick says to sign in, as its menu item did, and a call is refused.
AppActionRunResult preset_sync_command(const json&)
{
    if (const Ops::Outcome<Ops::Done> synced = Ops::sync_presets(); !synced)
        return NativeCommands::refused(synced.refusal(), AppActionRunResult::Level::Info);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// Whether settings_list's items carry their meta: as include_meta says, by default with where.
bool listed_with_meta(const json& args) { return arg_bool(args, "include_meta", args.contains("where")); }

// Resolves a call's type and target (settings_request_error() has checked them) to the config they read and write:
// a type's preset for the target global, the process's overrides of an object, a part or a height range for the
// others, which take what their rows offer (target_options()). The object or the part is the one the call names, else
// the one selected in Prepare, as for every command of one target. UI thread.
ConfigTarget resolve_target(const json& args, bool for_write)
{
    const std::string target = arg_string(args, "target", "global");
    const std::string type   = arg_string(args, "type", "process");
    PresetBundle&     bundle = *wxGetApp().preset_bundle;
    ConfigTarget      t;
    if (target != "global") {
        Plater& plater = ui_plater();
        Model&  model  = plater.model();
        json    names  = json::object();
        for (const char* key : {"object", "object_id", "part", "part_id"})
            if (args.contains(key))
                names[key] = args[key];
        // An instance or a part selected stands for its object.
        const Ops::Outcome<Targets> resolved = resolve_targets(model, &plater.get_view3D_canvas3D()->get_selection(), names,
                                                               target == "part" ? kParts : kObjects | kObjectOfSelection, true);
        if (!resolved)
            throw refused(resolved.refusal());
        t.target = target;
        t.named  = resolved.value();
        if (target == "part") {
            const auto [object, part] = t.named.parts.front();
            t.object_index            = object;
            t.object                  = model.objects[std::size_t(object)];
            t.volume                  = t.object->volumes[std::size_t(part)];
            t.config                  = &t.volume->config.get();
        } else {
            t.object_index = t.named.objects.front();
            t.object       = model.objects[std::size_t(t.object_index)];
            t.config       = &t.object->config.get();
        }
        if (target == "height_range") {
            // The range of those heights, as object_list gives them, to a tenth of a micron.
            const t_layer_height_range heights{arg_double(args, "min_z", -1.), arg_double(args, "max_z", -1.)};
            t_layer_config_ranges&     ranges = t.object->layer_config_ranges;
            const auto range = std::find_if(ranges.begin(), ranges.end(), [&heights](const auto& entry) {
                return std::abs(entry.first.first - heights.first) < EPSILON && std::abs(entry.first.second - heights.second) < EPSILON;
            });
            if (range == ranges.end()) {
                std::string listed;
                json        have = json::array();
                for (const auto& entry : ranges) {
                    listed += (listed.empty() ? "" : ", ") + height_range_text(entry.first);
                    have.push_back({{"min_z", entry.first.first}, {"max_z", entry.first.second}});
                }
                throw ToolFailure("object " + std::to_string(t.object_index) + " has no height range " + height_range_text(heights) +
                                      (listed.empty() ? "; it has none (object_set_height_ranges makes them)" : "; it has " + listed),
                                  {{"height_ranges", std::move(have)}});
            }
            t.range  = range->first;
            t.config = &range->second.get();
        }
        t.options = target_options(target, wxGetApp().filaments_cnt());
        return t;
    }
    t.type    = preset_type(type);
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
            throw ToolFailure("No " + type + " preset \"" + name + "\" (see preset_list)", {{"similar", similar_presets(collection(t.type), name)}});
        t.config = &t.preset->config;
        return t;
    }
    if (t.type == Preset::TYPE_FILAMENT) {
        const int slot = arg_int(args, "filament_slot", 1);
        if (slot < 1 || slot > int(bundle.filament_presets.size()))
            throw ToolFailure("filament_slot must be 1.." + std::to_string(bundle.filament_presets.size()));
        // A mixed filament's slot names its first component's preset, which belongs to that component's slot: a mixed
        // slot has no filament tab of its own.
        if (const std::optional<Ops::Refusal> refusal = Ops::mixed_slot_refusal(slot))
            throw refused(*refusal);
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
    if (t.type == Preset::TYPE_PRINTER) {
        // The nozzle flow type of each extruder, which its card in the sidebar picks and the project holds.
        auto printer = std::make_shared<DynamicPrintConfig>(t.preset->config);
        if (const ConfigOption* volumes = bundle.project_config.option("nozzle_volume_type"))
            printer->set_key_value("nozzle_volume_type", volumes->clone());
        t.owned  = printer;
        t.config = printer.get();
        t.options.push_back("nozzle_volume_type");
    }
    return t;
}

// The project's own settings, which project_get_settings reads. UI thread.
ConfigTarget project_target()
{
    ConfigTarget t;
    t.project = true;
    t.owned   = std::make_shared<const DynamicPrintConfig>(project_settings());
    t.config  = t.owned.get();
    t.options = project_options();
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

// settings_set's extruders_count, set alone (settings_request_error()): what a change of the Extruders field of the
// Printer settings does (TabPrinter::change_extruders_count()). `before` is the printer's settings before it. UI thread.
json set_extruders_count(const json& args, const DynamicPrintConfig& before)
{
    const std::size_t count = extruders_count_value(args["values"]["extruders_count"], before.opt_bool("single_extruder_multi_material"));
    auto*             tab   = dynamic_cast<TabPrinter*>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
    if (tab == nullptr)
        throw ToolFailure("The settings tab is not available");
    tab->change_extruders_count(count);
    // The Extruders field shows the count, which was not typed into it.
    tab->reload_config();
    const ConfigTarget after = resolve_target(args, false);
    return json{{"applied", {{"extruders_count", tab->m_extruders_count}}}, {"changed", changed_settings(before, *after.config)}};
}

// What settings_set's printer values set through the sidebar's nozzle cards: the printer variant a nozzle_diameter
// switches to (nozzle_diameter_variant()) and the printer preset that selects, and the nozzle flow type of each
// extruder a nozzle_volume_type gives.
struct NozzlePicks
{
    std::string                     variant;
    std::string                     preset;
    std::optional<std::vector<int>> flow_types;
};

// The nozzle flow types of `types` as nozzle_volume_type names them.
json flow_type_names(const std::vector<NozzleVolumeType>& types)
{
    json names = json::array();
    for (const NozzleVolumeType type : types)
        names.push_back(ConfigOptionEnum<NozzleVolumeType>(type).serialize());
    return names;
}

// The nozzle flow type of each extruder once `target`, a printer preset of another variant, is selected, read without
// selecting it: what the printer tab gives the project when it loads a preset of another base preset
// (TabPrinter::on_preset_loaded()), the types the app configuration remembers for that base, else the preset's
// default_nozzle_volume_type; `current`, the types the project has, for a preset of the same base. UI thread.
std::vector<int> flow_types_after_switch(const Preset& target, const std::vector<int>& current)
{
    PresetCollection& printers = wxGetApp().preset_bundle->printers;
    const auto        base     = [&printers](const Preset& preset) {
        const Preset* found = printers.get_preset_base(preset);
        return (found != nullptr ? found : &preset)->name;
    };
    const std::string name = base(target);
    if (name == base(printers.get_selected_preset()))
        return current;
    if (const auto* project = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")) {
        ConfigOptionEnumsGeneric remembered(*project);
        if (const std::string saved = wxGetApp().app_config->get_nozzle_volume_types_from_config(name);
            !saved.empty() && remembered.deserialize(saved))
            return remembered.values;
    }
    const auto* defaults = target.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type");
    return defaults != nullptr ? defaults->values : current;
}

// Takes the values the nozzle cards set out of `values` (key, serialized value) and checks them, before anything
// changes: a nozzle_diameter no variant has stays, to be typed into the Printer settings. A nozzle_diameter that selects
// another variant's preset goes with nozzle_volume_type alone, since the other values would be read against one preset
// and written to another. A flow type other than the one the extruder has, or with a variant switch will have
// (flow_types_after_switch()), is checked against the Flow combo it is picked in: the one shown, or with a variant
// switch the one the new preset shows. `printer` is the printer's settings, its nozzle_volume_type included
// (resolve_target()). UI thread.
NozzlePicks nozzle_picks(std::vector<std::pair<std::string, std::string>>& values, const DynamicPrintConfig& printer)
{
    NozzlePicks      picks;
    std::vector<int> types;
    for (auto v = values.begin(); v != values.end();) {
        if (v->first != "nozzle_diameter" && v->first != "nozzle_volume_type") {
            ++v;
            continue;
        }
        DynamicPrintConfig scratch;
        try {
            scratch.set_deserialize_strict(v->first, v->second);
        } catch (const std::exception&) {
            throw invalid_value(v->first, v->second);
        }
        if (v->first == "nozzle_diameter") {
            const auto* variant = printer.option<ConfigOptionString>("printer_variant");
            picks.variant       = nozzle_diameter_variant(scratch.option<ConfigOptionFloats>(v->first)->values,
                                                          wxGetApp().preset_bundle->printers.diameters_of_selected_printer(),
                                                          variant != nullptr ? variant->value : std::string());
            v                   = picks.variant.empty() ? std::next(v) : values.erase(v);
            continue;
        }
        types            = scratch.option<ConfigOptionEnumsGeneric>(v->first)->values;
        picks.flow_types = types;
        v                = values.erase(v);
    }
    const Preset* target = nullptr;
    if (!picks.variant.empty()) {
        target = wxGetApp().preset_bundle->get_similar_printer_preset({}, picks.variant);
        if (target == nullptr)
            throw ToolFailure("The printer has no preset of the nozzle variant " + picks.variant);
        picks.preset = target->name;
        if (!values.empty()) {
            std::string keys;
            for (const auto& [key, value] : values)
                keys += (keys.empty() ? "" : ", ") + key;
            throw ToolFailure("Set " + keys + " in a second call: a nozzle_diameter that selects the printer preset of another "
                              "variant (" + target->name + ") goes with nozzle_volume_type alone");
        }
    }
    if (!picks.flow_types)
        return picks;
    const auto*       current = printer.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    const std::size_t count   = current != nullptr ? current->size() : 0;
    if (types.size() != count)
        throw ToolFailure("nozzle_volume_type holds a nozzle flow type per extruder: " + std::to_string(count) + " extruders, " +
                          std::to_string(types.size()) + " given");
    // The types the extruders have when the picks are made: now, or once the switch has selected the new preset.
    const std::vector<int> before = target != nullptr ? flow_types_after_switch(*target, current->values) : current->values;
    const auto             name   = [](int type) { return ConfigOptionEnum<NozzleVolumeType>(NozzleVolumeType(type)).serialize(); };
    for (std::size_t e = 0; e < count; ++e) {
        if (e < before.size() && types[e] == before[e])
            continue;
        const std::vector<NozzleVolumeType> offered = target != nullptr ? Sidebar::nozzle_volume_types(target->config, int(e)) :
                                                                          ui_plater().sidebar().nozzle_volume_types(int(e));
        if (std::find(offered.begin(), offered.end(), NozzleVolumeType(types[e])) == offered.end()) {
            const std::string has   = e < before.size() ? ": the extruder has " + name(before[e]) + " there" : std::string();
            const std::string offer = offered.empty() ? "no nozzle flow type to pick" + has :
                                                        "the nozzle flow types " + flow_type_names(offered).dump() + ", not " +
                                                            name(types[e]);
            throw ToolFailure("The nozzle card of extruder " + std::to_string(e + 1) +
                                  (target != nullptr ? " with the printer preset " + target->name : std::string()) + " offers " + offer,
                              {{"offered", flow_type_names(offered)}});
        }
    }
    return picks;
}

// What nozzle_picks() checked, done as a person picks it in the sidebar: the variant, then the flow type of each
// extruder that has another, each read back after the picks. The switch fails only when its question about the
// printer's unsaved changes is cancelled, after `discarded`, reset_all's discarding of those changes, when the call
// asked for it. UI thread.
void pick_nozzles(const NozzlePicks& picks, bool discarded)
{
    Sidebar& sidebar = ui_plater().sidebar();
    if (!picks.variant.empty() && !sidebar.pick_nozzle_variant(picks.variant))
        throw ToolFailure("The printer preset " + picks.preset + " of the nozzle variant " + picks.variant + " was not selected" +
                          (discarded ? "; reset_all had discarded the printer's unsaved changes" : ""));
    if (!picks.flow_types)
        return;
    const auto flow_types = [] {
        return wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;
    };
    const std::vector<int>& wanted  = *picks.flow_types;
    const std::vector<int>  current = flow_types();
    for (std::size_t e = 0; e < wanted.size() && e < current.size(); ++e)
        if (current[e] != wanted[e])
            sidebar.pick_nozzle_volume_type(int(e), NozzleVolumeType(wanted[e]));
    // A guard for what nozzle_picks() rules out: an extruder whose Flow combo, after a variant switch whose flow types
    // it predicted wrongly, did not take the type asked for. The call then fails, saying what it changed.
    const std::vector<int> after = flow_types();
    const auto             name  = [](int type) { return ConfigOptionEnum<NozzleVolumeType>(NozzleVolumeType(type)).serialize(); };
    std::vector<std::string> wrong, set;
    for (std::size_t e = 0; e < wanted.size(); ++e) {
        const std::string extruder = "extruder " + std::to_string(e + 1);
        if (e < after.size() && e < current.size() && after[e] != current[e])
            set.push_back(extruder + " to " + name(after[e]));
        if (e >= after.size())
            wrong.push_back(extruder + " is not there");
        else if (after[e] != wanted[e])
            wrong.push_back(extruder + " has " + name(after[e]) + ", not " + name(wanted[e]));
    }
    if (wrong.empty())
        return;
    const auto listed = [](const std::vector<std::string>& items) {
        std::string text;
        for (std::size_t i = 0; i < items.size(); ++i)
            text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
        return text;
    };
    std::vector<std::string> changes;
    if (!picks.variant.empty())
        changes.push_back("selected the printer preset " + picks.preset);
    if (!set.empty())
        changes.push_back("set the nozzle flow type of " + listed(set));
    if (discarded)
        changes.push_back("discarded the printer's unsaved changes (reset_all)");
    json types = json::array();
    for (const int type : after)
        types.push_back(name(type));
    throw ToolFailure("nozzle_volume_type was not set as asked: " + listed(wrong) + ". The call " +
                          (changes.empty() ? std::string("changed nothing") : listed(changes)) + ".",
                      {{"nozzle_volume_type", std::move(types)}});
}

// The preset settings_get's compare names: ":saved" is the read preset as saved (so the difference is
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
            throw ToolFailure("No preset \"" + name + "\" to compare with (see preset_list)", {{"similar", similar_presets(presets, name)}});
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
// what a single-part object shows for its one part, the part's or modifier's own row, or the height range's. UI thread.
wxDataViewItem filament_row(const ConfigTarget& t)
{
    ObjectDataViewModel& rows = *wxGetApp().obj_list()->GetModel();
    if (t.range) {
        const wxDataViewItem row = rows.GetItemByLayerRange(t.object_index, *t.range);
        if (!row.IsOk())
            throw ToolFailure("The height range has no row in the object list");
        return row;
    }
    if (t.volume != nullptr && !t.volume->is_model_part() && !t.volume->is_modifier())
        throw ToolFailure("Only parts and modifiers take a filament; this one is a negative part, a support blocker or enforcer, "
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

// The 1-based filament slots that use the preset `name`: the physical ones, since a mixed filament's slot names its
// first component's preset without using it.
json filament_slots(const std::string& name)
{
    const PresetBundle&             bundle  = *wxGetApp().preset_bundle;
    const std::vector<std::string>& presets = bundle.filament_presets;
    json                            slots   = json::array();
    for (std::size_t s = 0; s < presets.size(); ++s)
        if (presets[s] == name && !bundle.is_mixed_filament(s))
            slots.push_back(int(s + 1));
    return slots;
}

// The first entry of a preset's string vector `key` ("" without one): a filament's vendor or type.
std::string first_value(const Preset& preset, const char* key)
{
    const auto* values = preset.config.option<ConfigOptionStrings>(key);
    return values != nullptr && !values->values.empty() ? values->values.front() : std::string();
}

// preset_list: an item per preset of the type, the default one only when it is alone, filtered by
// filter_preset_items(). A filament's vendor and type are the ones the filament selection dialogs show; a printer's or
// process's vendor is its vendor bundle (BBL), as the Printer Selection dialog has it. The Template vendor's presets
// are what Create Printer and Create Filament start from, which nothing installs: they are left out. With `printer`,
// a preset fits that printer preset as it would fit the selected one. UI thread.
json list_presets(const json& args)
{
    const Preset::Type type     = preset_type(arg_string(args, "type"));
    PresetBundle&      bundle   = *wxGetApp().preset_bundle;
    PresetCollection&  presets  = collection(type);
    const std::string  selected = presets.get_selected_preset_name();
    const Preset*      printer  = nullptr;
    if (args.contains("printer")) {
        const std::string name = arg_string(args, "printer");
        printer                = bundle.printers.find_preset(name, false, true);
        if (printer == nullptr)
            throw ToolFailure("No printer preset \"" + name + "\" (see preset_list {\"type\": \"printer\", \"include_uninstalled\": true})",
                              {{"similar", similar_presets(bundle.printers, name)}});
    }
    json items = json::array();
    for (size_t i = 0; i < presets.size(); ++i) {
        const Preset& p = presets.preset(i);
        if (p.is_default && presets.size() > 1)
            continue;
        const VendorProfile* vendor = presets.get_preset_with_vendor_profile(p).vendor;
        if (vendor != nullptr && vendor->id == PRESET_TEMPLATE_DIR)
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
        // Not offered by the selectors: a system printer or filament a person has not installed.
        if (!p.is_visible)
            j["installed"] = false;
        if (p.name == selected) {
            j["selected"] = true;
            if (presets.current_is_dirty())
                j["modified"] = true;
        }
        if (type == Preset::TYPE_FILAMENT) {
            for (const auto& [field, key] : {std::pair{"vendor", "filament_vendor"}, std::pair{"filament_type", "filament_type"}})
                if (std::string value = first_value(p, key); !value.empty())
                    j[field] = std::move(value);
            if (json slots = filament_slots(p.name); !slots.empty())
                j["filament_slots"] = std::move(slots);
        } else if (vendor != nullptr) {
            j["vendor"] = vendor->id;
        }
        if (type == Preset::TYPE_PRINTER)
            for (const auto& [field, key] : {std::pair{"printer_model", "printer_model"}, std::pair{"nozzle", "printer_variant"}})
                if (const auto* value = p.config.option<ConfigOptionString>(key); value != nullptr && !value->value.empty())
                    j[field] = value->value;
        items.push_back(std::move(j));
    }
    std::function<bool(const json&)> fits;
    if (printer != nullptr)
        fits = [&](const json& item) {
            const Preset* p = presets.find_preset(item["name"].get<std::string>(), false, true);
            return p != nullptr && is_compatible_with_printer(presets.get_preset_with_vendor_profile(*p),
                                                              bundle.printers.get_preset_with_vendor_profile(*printer));
        };
    return page(filter_preset_items(std::move(items), args, fits), args, 100);
}

} // namespace

ToolFailure preset_refused(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    // No preset has the name: often a printer or a system filament that is not installed, which no listing shows.
    if (code == "preset_not_offered")
        return refused(refusal,
                       "See preset_list; a printer model that is not installed is installed with printer_install "
                       "(printer_list_models lists them), a system filament with filament_install.",
                       "similar");
    if (code == "ambiguous_hidden_preset")
        return refused(refusal, "", "presets");
    if (code == "preset_incompatible")
        return refused(refusal, "preset_list include_incompatible lists such presets.");
    if (code == "transfer_not_offered" || code == "unsaved_preset_changes")
        return refused(refusal, "Pass preset_changes: \"discard\", or preset_save first.");
    return refused(refusal);
}

Ops::PresetChanges preset_changes_answer(const json& args)
{
    const std::string changes = arg_string(args, "preset_changes");
    return changes == "transfer" ? Ops::PresetChanges::Transfer :
           changes == "discard"  ? Ops::PresetChanges::Discard :
                                   Ops::PresetChanges::Ask;
}

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

std::vector<std::string> target_options(const std::string& target, int filaments)
{
    if (target == "object" || target == "part")
        return model_scope_options(SettingsFactory::get_fff_options(target == "part"), target == "part", filaments);
    if (target == "height_range")
        return layer_range_option_keys(filaments);
    return {};
}

std::vector<std::string> targets_taking(const std::string& key, int filaments)
{
    std::vector<std::string> targets;
    for (const char* target : {"object", "part", "height_range"}) {
        const std::vector<std::string> options = target_options(target, filaments);
        if (std::find(options.begin(), options.end(), key) != options.end())
            targets.emplace_back(target);
    }
    return targets;
}

std::string untaken_setting_error(const std::string& key, const std::string& target, int filaments)
{
    const std::vector<std::string> takers = targets_taking(key, filaments);
    if (std::find(takers.begin(), takers.end(), target) != takers.end())
        return {};
    const auto row = [](const std::string& name) {
        return name == "object" ? std::string("an object") : name == "part" ? std::string("a part") : std::string("a height range");
    };
    if (takers.empty())
        return key + " is set globally only, on the process (target global), not on " + row(target);
    std::string where;
    for (std::size_t i = 0; i < takers.size(); ++i)
        where += (i == 0 ? "" : i + 1 == takers.size() ? " or " : ", ") + row(takers[i]);
    return key + " is set on " + where + ", not on " + row(target);
}

json effective_setting(const std::string& key, const DynamicPrintConfig& process, const ModelObject& object, const ModelVolume* volume,
                       const DynamicPrintConfig* range)
{
    // An extruder of 0 is the object list's "Default": no filament of its own.
    const auto own = [&key](const DynamicPrintConfig& config) -> const ConfigOption* {
        const ConfigOption* opt = config.option(key);
        return opt != nullptr && (key != "extruder" || opt->getInt() != 0) ? opt : nullptr;
    };
    json out;
    if (const ConfigOption* opt = range != nullptr ? own(*range) : nullptr)
        out = {{"effective", opt->serialize()}, {"source", "height_range"}};
    else if (const ConfigOption* opt = volume != nullptr ? own(volume->config.get()) : nullptr)
        out = {{"effective", opt->serialize()}, {"source", "part"}};
    else if (const ConfigOption* opt = own(object.config.get()))
        out = {{"effective", opt->serialize()}, {"source", "object"}};
    else if (process.has(key))
        out = {{"effective", process.opt_serialize(key)}, {"source", "global"}};
    else if (key == "extruder")
        out = {{"effective", "1"}, {"source", "default"}}; // the process has no extruder; filament 1 prints
    else if (const ConfigOptionDef* def = print_config_def.get(key); def != nullptr && def->default_value)
        out = {{"effective", def->default_value->serialize()}, {"source", "default"}};
    if (volume == nullptr) {
        // Inside a height range that sets the key, the range's value replaces its parts' own; a region modifier's, mesh or
        // painted, applies still.
        const bool range_sets = range != nullptr && own(*range) != nullptr;
        json       parts      = json::array();
        for (std::size_t i = 0; i < object.volumes.size(); ++i) {
            const ModelVolume& v = *object.volumes[i];
            if (v.is_region_modifier() || (v.is_model_part() && !range_sets))
                if (const ConfigOption* opt = own(v.config.get()))
                    parts.push_back({{"part", int(i)}, {"value", opt->serialize()}});
        }
        if (!parts.empty())
            out["parts"] = std::move(parts);
    }
    // A region modifier, a mesh or a painted one, that sets none of the key prints, inside it, with the region it overlaps:
    // a model part's, or that of a region modifier before it in the object's list over the part (the slicer chains both
    // kinds alike, and region_config_from_model_volume() starts from that region's config). Where the modifier sits is not
    // worked out here, so where one of those volumes sets the key no value is the one it prints with: the object's chain
    // gives one, beside the volumes that set their own.
    if (volume != nullptr && volume->is_region_modifier() && range == nullptr && own(volume->config.get()) == nullptr) {
        json parts = json::array();
        for (std::size_t i = 0; i < object.volumes.size() && object.volumes[i] != volume; ++i) {
            const ModelVolume& v = *object.volumes[i];
            if (v.is_model_part() || v.is_region_modifier())
                if (const ConfigOption* opt = own(v.config.get()))
                    parts.push_back({{"part", int(i)}, {"value", opt->serialize()}});
        }
        if (!parts.empty())
            out = {{"effective", nullptr},
                   {"source", "overlapped_part"},
                   {"object_effective", out["effective"]},
                   {"object_source", out["source"]},
                   {"parts", std::move(parts)}};
    }
    // A height range applies over the object's parts, after their own values; a region modifier's own value applies over
    // it, and inside a region modifier that sets none the range's does, its region inheriting the part's region the range
    // is applied to (region_config_from_model_volume()).
    const bool ranges_apply = volume == nullptr || volume->is_model_part() ||
                              (volume->is_region_modifier() && own(volume->config.get()) == nullptr);
    if (range == nullptr && ranges_apply) {
        json ranges = json::array();
        for (const auto& [heights, config] : object.layer_config_ranges)
            if (const ConfigOption* opt = own(config.get()))
                ranges.push_back({{"min_z", heights.first}, {"max_z", heights.second}, {"value", opt->serialize()}});
        if (!ranges.empty())
            out["height_ranges"] = std::move(ranges);
    }
    return out;
}

json settings_list_page(const json& items, const json& args)
{
    return page(items, args, listed_with_meta(args) ? 100 : std::max<std::size_t>(items.size(), 1));
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
    // The dialog locks the choice to where an existing preset is (locked_save_location()).
    const std::optional<bool> locked = locked_save_location(presets, name);
    if (!locked)
        return arg_bool(args, "inside_project", presets.get_edited_preset().is_project_embedded);
    if (args.contains("inside_project") && args["inside_project"].get<bool>() != *locked)
        throw ToolFailure("Preset \"" + name + "\" is " + (*locked ? "kept inside the project" : "a user preset") +
                          " and is saved there; leave inside_project out");
    return *locked;
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

json settings_pages(const std::map<std::string, std::string>& places, const std::vector<std::string>& keys)
{
    // page -> group -> how many of `keys` it holds; the maps keep both in alphabetical order.
    std::map<std::string, std::map<std::string, int>> counted;
    for (const std::string& key : keys)
        if (const auto place = places.find(key); place != places.end())
            if (const std::size_t sep = place->second.find(" > "); sep != std::string::npos)
                ++counted[place->second.substr(0, sep)][place->second.substr(sep + 3)];
    json pages = json::array();
    for (const auto& [page, groups] : counted) {
        json listed = json::array();
        for (const auto& [group, settings] : groups)
            listed.push_back({{"group", group}, {"settings", settings}});
        pages.push_back({{"page", page}, {"groups", std::move(listed)}});
    }
    return {{"pages", std::move(pages)}};
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
    } else if (printer_extruder_options.count(key) == 0 && key != "extruder_offset" &&
               key != "nozzle_volume_type" /* the project's, read and set with the printer's settings */) {
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
        } catch (const std::exception&) {
            throw invalid_value(key, text);
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
        // What preset_list calls modified, in the settings the tab shows: the full comparison also lists
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
        throw ToolFailure(std::string("No ") + type + " preset \"" + name + "\" (see preset_list)", {{"similar", similar_presets(presets, name)}});
    // The tab's own rule (Tab::can_delete_preset()), worded for an agent.
    std::vector<std::string> inherited_by;
    switch (Tab::can_delete_preset(presets, *preset, &inherited_by)) {
    case Tab::DeleteBlock::System:
        throw ToolFailure("\"" + name + "\" is a system preset; only user presets and presets inside the project can be deleted");
    case Tab::DeleteBlock::FromBundle:
        throw ToolFailure("\"" + name + "\" comes from a preset bundle; only user presets and presets inside the project can be deleted");
    case Tab::DeleteBlock::InheritedFrom:
        throw ToolFailure("\"" + name + "\" cannot be deleted while other presets inherit from it; delete them first",
                          {{"inherited_by", inherited_by}});
    case Tab::DeleteBlock::None: break;
    }
    if (presets.get_preset_base(*preset) != preset)
        return json::object();
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
    // The Flushing volumes dialog's range.
    for (const double m : multipliers)
        if (m < g_min_flush_multiplier || m > g_max_flush_multiplier)
            throw ToolFailure("flush_multiplier " + to_config_string(json(m)) + " is outside " +
                              to_config_string(json(double(g_min_flush_multiplier))) + ".." +
                              to_config_string(json(double(g_max_flush_multiplier))) + ", the Flushing volumes dialog's range");
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

json filter_preset_items(json items, const json& args, const std::function<bool(const json&)>& fits)
{
    const bool        include_incompatible = arg_bool(args, "include_incompatible", false);
    const bool        include_uninstalled  = arg_bool(args, "include_uninstalled", false);
    const std::string query                = lower(arg_string(args, "query"));
    const std::string vendor               = lower(arg_string(args, "vendor"));
    const std::string filament_type        = lower(arg_string(args, "filament_type"));
    const std::string source               = arg_string(args, "source");
    const bool        other_printer        = args.contains("printer") && fits;
    json              out                  = json::array();
    for (json& item : items) {
        if (other_printer) {
            if (fits(item))
                item.erase("compatible");
            else
                item["compatible"] = false;
        }
        if ((!include_incompatible && !item.value("compatible", true)) || (!include_uninstalled && !item.value("installed", true)))
            continue;
        if (!query.empty() && lower(item.value("name", "")).find(query) == std::string::npos &&
            lower(item.value("alias", "")).find(query) == std::string::npos)
            continue;
        if ((!vendor.empty() && lower(item.value("vendor", "")) != vendor) ||
            (!filament_type.empty() && lower(item.value("filament_type", "")) != filament_type))
            continue;
        const bool project = item.value("project", false);
        if ((source == "system" && !item.value("system", false)) || (source == "user" && (!item.value("user", false) || project)) ||
            (source == "project" && !project))
            continue;
        out.push_back(std::move(item));
    }
    return out;
}

std::size_t extruders_count_value(const json& value, bool single_extruder_multi_material)
{
    if (single_extruder_multi_material)
        throw ToolFailure("Extruder count cannot be edited in single extruder multimaterial mode (single_extruder_multi_material)");
    long long count = 0;
    if (value.is_number_integer()) {
        count = value.get<long long>();
    } else if (value.is_string()) {
        const std::string text = value.get<std::string>();
        std::size_t       end  = 0;
        try {
            count = std::stoll(text, &end);
        } catch (const std::exception&) {
            end = 0;
        }
        if (end == 0 || end != text.size())
            count = 0;
    }
    if (count < 1 || count > (long long) MAXIMUM_EXTRUDER_NUMBER)
        throw ToolFailure("extruders_count is a whole number from 1 to " + std::to_string(MAXIMUM_EXTRUDER_NUMBER) +
                          ", as the Extruders field of the Printer settings takes; got " + value.dump());
    return std::size_t(count);
}

std::string nozzle_diameter_variant(const std::vector<double>& diameters, const std::vector<std::string>& variants,
                                    const std::string& current)
{
    if (diameters.empty())
        return {};
    const double diameter = diameters.front();
    if (std::any_of(diameters.begin(), diameters.end(), [diameter](double d) { return std::abs(d - diameter) > EPSILON; }))
        return {};
    for (const std::string& variant : variants) {
        // A plain diameter ("0.6"), not a named variant ("0.4 High Flow"), which shares its diameter with the plain one.
        std::size_t  parsed = 0;
        const double value  = string_to_double_decimal_point(variant, &parsed);
        if (!variant.empty() && parsed == variant.size() && std::abs(value - diameter) <= EPSILON)
            return variant == current ? std::string() : variant;
    }
    return {};
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

// The keys of kSearchTypes, each once, with the type that offers it first. UI thread.
std::vector<std::string> search_type_options(std::map<std::string, std::string>& type_of)
{
    std::vector<std::string> options;
    for (const char* type : kSearchTypes)
        for (const std::string& key : resolve_target(json{{"type", type}}, false).options)
            if (type_of.emplace(key, type).second)
                options.push_back(key);
    return options;
}

json read_target(const json& args, const ConfigTarget& t, bool listing = false);

// What a call on a target other than global acted on, as every command with a target names it: the object or the part
// (objects, parts) and, for a height range, its heights (height_range). UI thread.
json target_reply(const ConfigTarget& t)
{
    json out = targets_reply(ui_plater().model(), t.named);
    if (t.range)
        out["height_range"] = {{"min_z", t.range->first}, {"max_z", t.range->second}};
    return out;
}

// settings_get: the settings `args` asks for, read on the UI thread.
json read_settings(const json& args)
{
    // Named keys with no type look through kSearchTypes; each item names its type.
    if (args.contains("keys") && !args.contains("type") && arg_string(args, "target", "global") == "global" &&
        !args.contains("preset") && !args.contains("compare")) {
        std::map<std::string, std::string> type_of;
        const std::vector<std::string>     options = search_type_options(type_of);
        std::vector<std::string>           wanted;
        json                               unknown = json::array();
        for (const json& k : args["keys"]) {
            const std::string key = k.get<std::string>();
            if (type_of.count(key) == 0)
                unknown.push_back(key);
            else if (std::find(wanted.begin(), wanted.end(), key) == wanted.end())
                wanted.push_back(key);
        }
        std::map<std::string, json> found;
        for (const char* type : kSearchTypes) {
            json keys = json::array();
            for (const std::string& key : wanted)
                if (type_of[key] == type)
                    keys.push_back(key);
            if (keys.empty())
                continue;
            json typed = args;
            typed.erase("offset");
            typed.update({{"type", type}, {"keys", std::move(keys)}, {"limit", 500}});
            json read = read_settings(typed);
            for (json& item : read["items"]) {
                item["type"]          = type;
                const std::string key = item["key"];
                found[key]            = std::move(item);
            }
        }
        json items = json::array();
        for (const std::string& key : wanted)
            if (const auto it = found.find(key); it != found.end())
                items.push_back(std::move(it->second));
        json out = page(items, args, 100);
        add_unknown_keys(out, unknown, options);
        return out;
    }
    return read_target(args, resolve_target(args, false));
}

// The settings of `t` that `args` asks for: its keys or where, its whole listing without them (for a target
// other than global, what it overrides), or with compare what differs from another preset. `listing`: settings_list's,
// paged by settings_list_page(). A key a target does not take is refused with where it is set. UI thread.
json read_target(const json& args, const ConfigTarget& t, bool listing)
{
    const bool compare = args.contains("compare");
    // A preset's whole listing, hundreds of keys, is for scanning values; what one key takes
    // comes with it when it is named or listed with its group.
    const bool whole = !args.contains("keys") && !args.contains("where") && t.object == nullptr && !t.project;
    const bool meta  = listing ? listed_with_meta(args) : arg_bool(args, "include_meta", !compare && !whole);
    if (t.object != nullptr && (compare || args.contains("preset")))
        throw ToolFailure("preset and compare read a type's presets, which the targets object, part and height_range are not");
    // An object's or part's settings are process settings, on the process tab's pages.
    const std::map<std::string, std::string> places =
        t.project ? std::map<std::string, std::string>() : setting_places(t.object != nullptr ? Preset::TYPE_PRINT : t.type);
    std::vector<std::string> keys;
    if (args.contains("keys")) {
        for (const json& k : args["keys"])
            keys.push_back(k.get<std::string>());
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
            throw ToolFailure("No page or group \"" + arg_string(args, "where") + "\" in these settings", {{"groups", groups}});
    } else if (t.object != nullptr) {
        // What the target overrides: its own keys, a filament of 0 ("Default") being none.
        for (const std::string& key : t.config->keys())
            if (key != "extruder" || t.config->opt_int("extruder") != 0)
                keys.push_back(key);
    } else {
        keys = t.options;
    }
    // A target's overrides are listed whatever they are; a key named for a target is one it takes.
    const bool                      overrides = t.object != nullptr && !args.contains("keys");
    const std::vector<std::string>& process   = preset_options(Preset::TYPE_PRINT);
    std::vector<std::string>        known;
    json                            unknown = json::array();
    for (const std::string& key : keys) {
        const bool defined = print_config_def.get(key) != nullptr;
        if (defined && (overrides || std::find(t.options.begin(), t.options.end(), key) != t.options.end()))
            known.push_back(key);
        else if (defined && t.object != nullptr && std::find(process.begin(), process.end(), key) != process.end())
            throw ToolFailure(untaken_setting_error(key, t.target, wxGetApp().filaments_cnt()));
        else
            unknown.push_back(key);
    }
    const auto extruders = printer_extruders();
    const auto add_meta  = [&](json& item, const std::string& key) {
        item["meta"] = option_meta(*print_config_def.get(key));
        if (const auto place = places.find(key); place != places.end())
            item["meta"]["where"] = place->second;
        if (t.project) {
            if (key == "curr_bed_type")
                item["meta"]["offered"] = project_offered(key);
            if (project_key_read_only(key))
                item["meta"]["read_only"] = true;
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
        // What an object, a part or a height range prints with falls back to the process being edited, as its
        // settings tab shows it.
        const DynamicPrintConfig& global     = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        bool                      part_only  = false;
        bool                      range_only = false;
        bool                      overlapped = false;
        json                      items      = json::array();
        for (const std::string& key : known) {
            json item{{"key", key}};
            if (t.config->has(key))
                item["value"] = t.config->opt_serialize(key);
            else
                item["value"] = nullptr; // not overridden at this level
            if (t.object != nullptr) {
                item.update(effective_setting(key, global, *t.object, t.volume, t.range ? t.config : nullptr));
                part_only  = part_only || item.contains("parts") || item.value("source", "") == "part";
                range_only = range_only || item.contains("height_ranges") || item.value("source", "") == "height_range";
                overlapped = overlapped || item.value("source", "") == "overlapped_part";
            }
            if (meta)
                add_meta(item, key);
            items.push_back(std::move(item));
        }
        out = listing ? settings_list_page(items, args) : page(items, args, 100);
        std::string note;
        if (part_only)
            note = "A part's value (source \"part\", or in parts) applies only inside that part's volume.";
        if (range_only)
            note += std::string(note.empty() ? "" : " ") +
                    "A height range's value (source \"height_range\", or in height_ranges) applies only between its min_z and max_z.";
        if (overlapped)
            note += std::string(note.empty() ? "" : " ") +
                    "Inside a modifier, mesh or painted, that sets none of a key (source \"overlapped_part\"), the value of the "
                    "part it overlaps applies: that part's own in parts, else object_effective; which parts it overlaps is not "
                    "worked out.";
        if (!note.empty())
            out["note"] = note;
    }
    if (t.object != nullptr)
        out.update(target_reply(t));
    add_unknown_keys(out, unknown, t.options);
    return out;
}

// The settings settings_list lists for `args`' type, and the arguments it reads them with: a type's preset (process,
// printer, filament) as `args` names it, or for object and region the process preset, its options cut to those an object
// or a region takes, whose values they print with unless they set their own. settings_list_pages groups the same
// settings, so that a group's count is what settings_list lists there. UI thread.
std::pair<ConfigTarget, json> listed_settings(const json& args)
{
    const std::string type = arg_string(args, "type");
    if (type != "object" && type != "region")
        return {resolve_target(args, false), args};
    json process    = args;
    process["type"] = "process";
    ConfigTarget t  = resolve_target(process, false);
    // A region is a part, a modifier or a height range: what a height range takes, a part's and the layer height.
    const std::vector<std::string> taken = target_options(type == "object" ? "object" : "height_range", wxGetApp().filaments_cnt());
    t.options.erase(std::remove_if(t.options.begin(), t.options.end(),
                                   [&taken](const std::string& key) { return std::find(taken.begin(), taken.end(), key) == taken.end(); }),
                    t.options.end());
    return {std::move(t), std::move(process)};
}

// settings_list: a type's settings, a preset's or those an object or a region takes. UI thread.
json list_settings(const json& args)
{
    const auto [t, read] = listed_settings(args);
    json out             = read_target(read, t, true);
    if (arg_string(args, "type") == "region")
        for (json& item : out["items"])
            if (item["key"] == "layer_height")
                item["targets"] = json::array({"height_range"});
    return out;
}

// settings_list_pages: the pages and groups of the settings settings_list lists for `args`' type. UI thread.
json list_setting_pages(const json& args)
{
    const std::pair<ConfigTarget, json> listed = listed_settings(args);
    // The keys settings_list lists: those of the target's options that the config defines (read_target()).
    std::vector<std::string> keys;
    for (const std::string& key : listed.first.options)
        if (print_config_def.get(key) != nullptr)
            keys.push_back(key);
    return settings_pages(setting_places(listed.first.type), keys);
}

} // namespace

std::string settings_request_error(const json& args, bool writes)
{
    const std::string target = arg_string(args, "target", "global");
    const bool        names  = args.contains("object") || args.contains("object_id") || args.contains("part") || args.contains("part_id");
    const bool        range  = args.contains("min_z") || args.contains("max_z");
    if (target == "global" && names)
        return "object and part name the object or part of a target: give target object, part or height_range";
    if (target != "height_range" && range)
        return "min_z and max_z go with target height_range, the range of an object they name";
    if (target != "global") {
        // An object's, a part's and a height range's settings are process settings.
        if (const std::string type = arg_string(args, "type", "process"); type != "process")
            return "The " + type + " settings have no target " + target + ": an object's, a part's or a height range's own "
                   "settings are process settings (type process, or no type)";
        if (target != "part" && (args.contains("part") || args.contains("part_id")))
            return "part goes with target part: target " + target + " is of an object";
        if (target == "part" && args.contains("object") != args.contains("part"))
            return "target part names its part by object and part, or by neither for the part selected in Prepare";
        if (target == "height_range" && !(args.contains("min_z") && args.contains("max_z")))
            return "target height_range needs min_z and max_z, the range's heights as object_list lists them";
    }
    // settings_set writes values, or takes a preset back to its saved state; settings_get reads what keys or compare
    // names, or a target's overrides.
    if (writes) {
        if (!args.contains("values") && !args.contains("reset_all"))
            return "Nothing is named: give values to change settings, or reset_all to discard the unsaved changes of a type's "
                   "preset";
        // The extruders a count adds or takes away change what the printer's other values hold.
        const json values = args.value("values", json::object());
        if (values.is_object() && values.contains("extruders_count") &&
            (arg_string(args, "type", "process") != "printer" || values.size() > 1 || arg_bool(args, "reset_all", false)))
            return "extruders_count is set alone, with type printer: the extruders it adds take their settings in a following "
                   "call";
    } else if (target == "global" && !args.contains("keys") && !args.contains("compare")) {
        return "Nothing is named: give keys or compare to read settings, or a target other than global to read what it "
               "overrides; settings_list lists a type's settings, and settings_list_pages their pages";
    }
    return {};
}

json read_project_settings(const json& args) { return read_target(args, project_target()); }

void register_preset_tools(CommandCatalog& catalog)
{
    // The settings tools' type and target, as the Process panel's Global and Objects rows choose them.
    const json type_prop        = enum_prop({"printer", "process", "filament"}, "Which settings: the printer, process or filament "
                                                                                 "preset being edited.");
    const json target_prop      = enum_prop({"global", "object", "part", "height_range"},
                                            "Where they apply: global (default), the type's preset; object, part or `height_range`, "
                                            "the overrides an object, one of its parts or modifiers, or one of its height ranges "
                                            "carries, which are process settings.");
    const json object_prop = {{"type", "integer"},
                              {"minimum", 0},
                              {"description", "Object index from `object_list` (targets object, part and `height_range`); without it, "
                                              "the object selected in Prepare."}};
    const json part_prop   = {{"type", "integer"},
                              {"minimum", 0},
                              {"description", "Part index in the object (target part, with object); without both, the part selected "
                                              "in Prepare."}};
    const json min_z_prop  = {{"type", "number"}, {"minimum", 0}, {"description", "Target `height_range`: the range's bottom, mm."}};
    const json max_z_prop  = {{"type", "number"}, {"minimum", 0}, {"description", "Target `height_range`: the range's top, mm."}};
    const json read_preset_prop = prop("string", "Read this saved preset of the type (a name from `preset_list`) instead of the one "
                                                 "being edited.");

    catalog.add(inspect_tool(
        "preset_list", L("List presets"),
        "Printer, process or filament presets, each with its name, alias (the name its selector shows, when another), "
        "system, user, project (kept inside the project), inherits, selected (and modified), `compatible: false` or "
        "`installed: false` when so, and vendor; a filament its `filament_type` and the slots using it (`filament_slots`), a "
        "printer its `printer_model` and nozzle. Only compatible, installed presets unless asked: a fresh install leaves most "
        "system filaments and printers out of the selectors, and `preset_select` takes such a preset too. printer lists the "
        "filaments or processes that fit another printer preset, in place of the selected one.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"query", prop("string", "Case-insensitive part of the name or alias.")},
                       {"vendor", prop("string", "A vendor, in either case: a filament's maker (\"Bambu Lab\", \"Generic\"), a "
                                                 "printer's or process's vendor bundle (\"BBL\").")},
                       {"filament_type", prop("string", "Type filament: a filament type, in either case (\"PLA\").")},
                       {"printer", prop("string", "Types filament and process: list what fits this printer preset (a name from "
                                                  "`preset_list` type printer) in place of the selected printer.")},
                       {"source", enum_prop({"system", "user", "project"}, "Where the presets come from: the system's, a "
                                                                           "person's own, or kept inside the project.")},
                       {"include_incompatible", prop("boolean", "Also list presets incompatible with the printer.")},
                       {"include_uninstalled", prop("boolean", "Also list the system presets the selectors do not offer, which "
                                                               "a person has not installed.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Default 100."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}},
                      {"type"}),
        [](const json& args, ToolContext& ctx) {
            const std::string type = arg_string(args, "type");
            if (args.contains("printer") && type == "printer")
                throw ToolFailure("printer lists the filaments or processes that fit a printer preset, not printers");
            if (args.contains("filament_type") && type != "filament")
                throw ToolFailure("filament_type filters filaments (type filament)");
            return ToolResult::ok(ctx.run_ui([&] { return list_presets(args); }));
        }));

    catalog.add(reaching({"resolve_preset", "select_preset", "assign_filament_preset"}, mutate_tool(
        "preset_select", L("Select a preset"),
        "Select a printer, process or filament preset by the name its selector shows (or its internal name), as a person "
        "would. One the selectors do not offer (`preset_list` `include_uninstalled`) is shown in them first, until the app "
        "restarts; one the selector leaves out as incompatible with the printer is refused. With unsaved changes the Transfer-or-discard prompt opens "
        "(`blocked_by_dialog`) unless `preset_changes` answers it. A filament is assigned to `filament_slot` (1-based, default "
        "1); with several slots the filament tab keeps the preset and the changes it has, so nothing is asked. Reply: "
        "selected, shown (a preset it showed in the selectors); "
        "when presets had or have unsaved changes, discarded and modified list per preset the keys dropped and the keys "
        "still unsaved.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Preset name as `preset_list` shows it.")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}},
                       {"preset_changes", enum_prop({"transfer", "discard"}, "Unsaved changes of the presets the switch replaces: move "
                                                                       "them to the new preset or drop them, as the prompt's "
                                                                       "buttons do; transfer is refused where the prompt does "
                                                                       "not offer it (a printer's changes, a filament of "
                                                                       "another type). Omitted: the prompt opens.")}},
                      {"type", "name"}),
        [](const json& args, ToolContext& ctx) {
            const Preset::Type       type   = preset_type(arg_string(args, "type"));
            const std::string        name   = arg_string(args, "name");
            const Ops::PresetChanges answer = preset_changes_answer(args);
            json result = ctx.run_ui([&]() -> json {
                ui_plater();
                const int                slot = arg_int(args, "filament_slot", 1);
                const Ops::PresetKind    kind = type == Preset::TYPE_PRINTER  ? Ops::PresetKind::Printer :
                                                type == Preset::TYPE_FILAMENT ? Ops::PresetKind::Filament :
                                                                                Ops::PresetKind::Process;
                Ops::ResolvePresetArgs resolve;
                resolve.kind          = kind;
                resolve.name          = name;
                resolve.filament_slot = slot;
                const Ops::Outcome<Ops::ResolvedPreset> resolved = Ops::resolve_preset(resolve);
                if (!resolved)
                    throw preset_refused(resolved.refusal());
                const json before = unsaved_changes();
                json       out;
                if (kind == Ops::PresetKind::Filament) {
                    Ops::AssignFilamentArgs assign;
                    assign.filament_slot = slot;
                    assign.preset        = resolved.value().name;
                    assign.changes       = answer;
                    const Ops::Outcome<std::string> assigned = Ops::assign_filament_preset(assign);
                    if (!assigned)
                        throw preset_refused(assigned.refusal());
                    out = {{"type", "filament"}, {"filament_slot", slot}, {"selected", assigned.value()}};
                } else {
                    const Ops::Outcome<Ops::Done> selected = Ops::select_preset({kind, resolved.value().name, answer});
                    if (!selected)
                        throw preset_refused(selected.refusal());
                    out = {{"type", arg_string(args, "type")}, {"selected", resolved.value().name}};
                }
                if (!resolved.value().shown.empty())
                    out["shown"] = resolved.value().shown;
                out.update(unsaved_changes_outcome(before));
                return out;
            });
            settle(ctx);
            return ToolResult::ok(result);
        })));

    NativeCommand save = mutate_tool(
        "preset_save", L("Save a preset"),
        "Save the printer, process or filament preset being edited, with its unsaved changes, as a preset called name, "
        "which is then selected: what the tab's Save button does through its dialog. A filament is `filament_slot`'s "
        "(default 1), and every slot using the old preset takes the new one. Replacing another existing preset needs "
        "overwrite (pass it in replays and scripts meant to be rerun). Reply: saved, selected, user, project, inherits, "
        "`filament_slots`. A "
        "temperature warning or an unsaved-changes prompt makes it `blocked_by_dialog`. To keep only some edits, `settings_set "
        "{reset_all: true, values}` first.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Name to save under.")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based."}}},
                       {"inside_project", prop("boolean", "Keep it inside the project, not as a user preset. Default: where the "
                                                          "edited preset is. A preset of that name stays where it is.")},
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
                    throw ToolFailure("Detach is offered only in Developer mode; turn it on first with tool_call {\"tool\": "
                                      "\"settings_toggle_developer_mode\"}");
                Tab* tab = wxGetApp().get_tab(type);
                if (tab == nullptr)
                    throw ToolFailure("The settings tab is not available");
                // The filament tab edits one slot's preset; edit this slot's, as settings_set does.
                if (type == Preset::TYPE_FILAMENT)
                    resolve_target({{"type", "filament"}, {"filament_slot", arg_int(args, "filament_slot", 1)}}, true);
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
        });
    // A user preset syncs to the cloud account when preset sync is on.
    save.open_world = true;
    catalog.add(save);

    NativeCommand remove = mutate_tool(
        "preset_delete", L("Delete a preset"),
        "Delete a user preset or one inside the project, its file and its cloud copy, as the tab's Delete does, without "
        "asking. A selected preset is replaced as the tab does (its parent or the next preset). Another filament is deleted "
        "without changing the selection, and slots using it take the selected one; another process is selected, deleted "
        "and the selection restored; a printer must be selected first, and the physical printers that use only it are "
        "deleted with it. Refused: system, default and bundle presets, presets others inherit from, another preset while "
        "the edited one of its type has unsaved changes. Deleting a selected printer or process can "
        "ask about other presets' unsaved changes (`blocked_by_dialog`). Reply: deleted, selected, filaments (per slot), "
        "attached, and discarded/modified as `preset_select`'s.",
        object_schema({{"type", enum_prop({"printer", "process", "filament"}, "Preset category.")},
                       {"name", prop("string", "Preset name as `preset_list` shows it.")},
                       {"delete_attached", prop("boolean", "A custom printer with process or filament presets made for it alone is "
                                                           "refused without this: they are deleted with it, as the GUI "
                                                           "deletes them.")}},
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
                    tab->delete_preset(Ops::Prompts::Skip);
                } else if (type == Preset::TYPE_PRINTER) {
                    // Switching to another printer and back recomputes the process and the filaments, so it is no round trip.
                    throw ToolFailure("\"" + name + "\" is not the selected printer; select it first with preset_select");
                } else if (presets.current_is_dirty()) {
                    // Both routes select the edited preset again, which would drop or ask about its changes.
                    throw ToolFailure("The edited " + type_name + " \"" + selected + "\" has unsaved changes; preset_save or settings_set "
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
                        tab->delete_preset(Ops::Prompts::Skip);
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
        true);
    // It deletes the preset's cloud copy too.
    remove.open_world = true;
    catalog.add(remove);

    catalog.add(inspect_tool(
        "settings_list", L("List settings"),
        "All the settings of a type in one reply, each item with its `value` as OrcaSlicer writes it, and without `meta` unless "
        "`include_meta`. `process`, `printer` and `filament`: the preset being edited, or with `preset` a saved one, or "
        "with `filament_slot` a slot's. `object`: the process settings an object or an assembly takes; `region`: those a "
        "part, a "
        "modifier or a height range takes, whose `layer_height` item has targets [\"height_range\"], a height range alone "
        "taking it; in both, each with the process preset's value, which an object or a region prints with unless it "
        "sets its own. With `where`, the settings of one settings page or \"Page > Group\", as `settings_list_pages` lists them "
        "(an unknown one is refused with the list of them). Items with `meta`, which `where` gives them, come 100 a page. "
        "`settings_get` reads settings by key, and what an object, a part or a height range overrides.",
        object_schema({{"type", enum_prop({"process", "printer", "filament", "object", "region"},
                                          "Which settings: a preset's, or those an object or a region takes.")},
                       {"where", prop("string", "A settings page, or \"Page > Group\" as `meta.where` gives it, e.g. "
                                                "\"Strength > Walls\".")},
                       {"preset", prop("string", "Read this saved preset (a name from `preset_list`) instead of the one being "
                                                 "edited: of the type, or for object and region a process preset.")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1},
                                          {"description", "Type filament: the slot whose preset is read, 1-based (default 1), with "
                                                          "its unsaved changes when the filament tab is editing it."}}},
                       {"include_meta", prop("boolean", "Include labels, types and ranges. Default false, true with where.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500},
                                  {"description", "Default: all of them, or 100 with meta."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}},
                      {"type"}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                ui_plater(); // the presets and the settings pages exist once the main window does
                return list_settings(args);
            }));
        }));

    catalog.add(inspect_tool(
        "settings_list_pages", L("List settings pages"),
        "The pages of a type's settings tab and the groups on each, pages and groups in alphabetical order, each group with "
        "how many of the settings `settings_list` lists for the same type it holds: `settings_list {type, where}` lists that "
        "many there. A page and a \"Page > Group\" are each a `where` `settings_list` takes, which lists those settings with "
        "their values and what each does. A setting with no row on a page is in no group; `settings_list` without `where` "
        "lists it.",
        object_schema({{"type", enum_prop({"process", "printer", "filament", "object", "region"},
                                          "Which settings, as `settings_list` takes them: a preset's, or those an object or a "
                                          "region takes.")}},
                      {"type"}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                ui_plater(); // the presets and the settings pages exist once the main window does
                return list_setting_pages(args);
            }));
        }));

    catalog.add(with_target_ids(inspect_tool(
        "settings_get", L("Read settings"),
        "Read the settings named: each item has its `value`, as OrcaSlicer writes it, and `meta`: `label`, "
        "`description` (what the setting does, when to change it), `where` (its settings page and group), `type`, `unit`, "
        "range, allowed values and `mode`. Keys the settings lack go to `unknown_keys`, with `type_of` (where each is a "
        "setting: a type, or project) and `similar` (keys like the others). It needs `keys`, `compare` or a `target` other "
        "than global; `settings_list` lists a type's settings, a page at a time with `where`.\n"
        "- No `type`: `keys` are looked up in the process, filament and printer settings, and each item names its type, "
        "the one `settings_set` takes.\n"
        "- Target global: the preset being edited, or a saved `preset`; `compare` lists only the keys that differ (`value` "
        "and `other`). A per-variant value's `meta` has `variants` (what each entry is for) and `active` (the entry each "
        "extruder prints with).\n"
        "- Target object, part or `height_range`: without `keys`, what it overrides; with them, the process settings it "
        "takes (`settings_list` type object or region) and extruder, its filament; a key it does not take is refused with "
        "where it is set. Each item has `value` (its own, or null), `effective` (what it prints with) and `source`, the "
        "first of `height_range`, part, object, global or default to set it. An object's item lists in `parts` the parts "
        "and modifiers that override the key in their volume, and an object's or part's in `height_ranges` the ranges that "
        "apply to it. In a modifier (mesh or painted) that sets none of a key, the overlapped part's value applies, which "
        "is not worked out: where a part or an earlier modifier sets the key, the modifier's item has `effective` null, "
        "`source` `overlapped_part`, `object_effective` and `object_source`, and in `parts` those that set it. The reply "
        "names the target.\n"
        "`preferences_get` reads the app's preferences, `plate_get_settings` a plate's settings and `project_get_settings` the "
        "project's own.",
        object_schema({{"type", type_prop},
                       {"target", target_prop},
                       {"keys", array_prop({{"type", "string"}}, "Option keys, e.g. `[\"layer_height\", \"wall_loops\"]`.")},
                       {"object", object_prop},
                       {"part", part_prop},
                       {"object_id", id_prop("object")},
                       {"part_id", id_prop("part")},
                       {"min_z", min_z_prop},
                       {"max_z", max_z_prop},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1},
                                          {"description", "Type filament: the slot whose preset is read, 1-based (default 1), with "
                                                          "its unsaved changes when the filament tab is editing it."}}},
                       {"preset", read_preset_prop},
                       {"compare", prop("string", "Target global: a preset name, \":saved\" (the unsaved changes) or \":parent\" "
                                                  "(the preset it inherits from).")},
                       {"include_meta", prop("boolean", "Include labels, types and ranges. Default true, false with compare.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Default 100."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            // Refused before the UI is asked for anything: what the arguments alone decide.
            if (const std::string err = settings_request_error(args); !err.empty())
                throw ToolFailure(err);
            return ToolResult::ok(ctx.run_ui([&] {
                ui_plater(); // the presets and the settings pages exist once the main window does
                return read_settings(args);
            }));
        })));

    catalog.add(with_target_ids(mutate_tool(
        "settings_set", L("Change settings"),
        "Change settings as typing them into their settings tab would: a type's preset (target global, the default), or "
        "the overrides of an object, a part or a height range. Values are strings in OrcaSlicer's format (\"15%\", \"0.2\", "
        "\"1,2\") or JSON numbers, booleans and arrays (a point as `[x, y]`). All are parsed strictly first and applied "
        "together, so nothing changes if one is invalid; a key of another type is refused with that type named. Reply: "
        "`applied`, and `changed`, every setting the call changed, including those the app adjusted in turn. An object's or "
        "part's change is one undo step; a preset's is not in the undo history. Spiral vase and a few others confirm their "
        "adjustments in a dialog (`blocked_by_dialog`).\n"
        "- Target global: a null value takes a key back to its saved value, or with `reset_to` parent to the parent's; "
        "`reset_all` first discards every unsaved change, as the tab's undo button does (reply: `reverted`). A per-variant "
        "value (`settings_get` `meta.variants`) takes one value for the entries in use (`meta.active`), `{index, value}` "
        "for one entry, or an array of all.\n"
        "- Type printer: `extruders_count` goes alone, as the Printer settings' Extruders field sets it; "
        "`nozzle_volume_type` sets each extruder's nozzle flow type as its sidebar card does. A `nozzle_diameter` of another "
        "variant of the printer model selects that variant's preset, as the sidebar's nozzle diameter does, with only "
        "`nozzle_volume_type` beside it (other keys go in a second call); a flow type the new preset's nozzle card does not "
        "offer is refused with those it does. The switch asks about the printer's unsaved changes (`blocked_by_dialog`) "
        "unless `reset_all` discards them first, which stays if the switch is not made.\n"
        "- Target object, part or `height_range`: overrides of the keys its row offers in Prepare (`settings_list` type "
        "object or region), and `extruder`; a key it does not take is refused with where it is set. null removes one; a "
        "height range keeps its `layer_height`, checked against the printer's layer heights for the range's filament. "
        "`extruder` is the target's filament (1-based; 0 or null for Default, the object's), set as the Change Filament menu "
        "does. The reply names the target.\n"
        "`plate_set_settings` changes a plate's settings and `project_set_settings` the project's own. `preferences_set` changes "
        "the app's preferences; those it refuses (`preferences_get` marks them) change in the Preferences dialog: "
        "`window_open {\"window\": \"preferences\", \"tab\": \"<tab>\", \"key\": \"<key>\"}`, then `widget_set_value "
        "{\"target\": {\"name\": \"<key>\", \"within\": \"Preferences\"}, \"value\": ...}`.",
        object_schema({{"type", type_prop},
                       {"target", target_prop},
                       {"values", {{"type", "object"}, {"description", "`{key: value}`, or `{key: {index, value}}` for one entry"}}},
                       {"reset_all", prop("boolean", "Target global: discard the unsaved changes before applying values.")},
                       {"reset_to", enum_prop({"saved", "parent"}, "Target global: what a null value takes a key back to: the "
                                                                     "saved preset (default) or the preset it inherits from.")},
                       {"object", object_prop},
                       {"part", part_prop},
                       {"min_z", min_z_prop},
                       {"max_z", max_z_prop},
                       {"object_id", id_prop("object")},
                       {"part_id", id_prop("part")},
                       {"filament_slot", {{"type", "integer"}, {"minimum", 1},
                                          {"description", "Type filament: the slot whose preset is changed, 1-based (default 1). "
                                                          "The filament tab switches to it, which can ask about the unsaved "
                                                          "changes it holds (`blocked_by_dialog`)."}}}}),
        [](const json& args, ToolContext& ctx) {
            // Refused before the UI is asked for anything: what the arguments alone decide.
            if (const std::string err = settings_request_error(args, /*writes=*/true); !err.empty())
                throw ToolFailure(err);
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
                    throw ToolFailure(std::string(reset_all ? "reset_all" : "reset_to") + " is for a type's preset; on a target a null value removes an override");
                // What a null value takes a preset's key back to: its saved value, as the key's undo arrow does,
                // or the parent's, as its lock icon does.
                const Preset* reference = t.object != nullptr ? nullptr :
                                          arg_string(args, "reset_to", "saved") == "parent" ? &compare_preset(t, ":parent") :
                                                                                               &collection(t.type).get_selected_preset();
                const DynamicPrintConfig before = *t.config;
                if (t.type == Preset::TYPE_PRINTER && t.object == nullptr && values_in.contains("extruders_count"))
                    return set_extruders_count(args, before);
                // A preset's value is checked against what it will replace: after reset_all, the saved preset.
                const DynamicPrintConfig&                        basis = reset_all ? collection(t.type).get_selected_preset().config : *t.config;
                const auto                                       extruders = printer_extruders();
                std::vector<std::pair<std::string, std::string>> values;
                std::vector<std::string>                         removals;
                const std::vector<std::string>& process = preset_options(Preset::TYPE_PRINT);
                for (auto it = values_in.begin(); it != values_in.end(); ++it) {
                    if (std::find(t.options.begin(), t.options.end(), it.key()) == t.options.end()) {
                        json similar{{"similar", similar_keys(it.key(), t.options)}};
                        // A process setting the target does not take is set where its rows offer it.
                        if (t.object != nullptr && std::find(process.begin(), process.end(), it.key()) != process.end())
                            throw ToolFailure(untaken_setting_error(it.key(), t.target, wxGetApp().filaments_cnt()), std::move(similar));
                        throw unknown_setting(it.key(), arg_string(args, "type", "process"), std::move(similar));
                    }
                    // A height range's row shows its layer height, which it always has.
                    if (it.value().is_null() && t.range && it.key() == "layer_height")
                        throw ToolFailure("A height range keeps its layer_height; give it another value");
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
                        } catch (const std::exception&) {
                            throw invalid_value(key, value);
                        }
                    return scratch;
                };
                json reverted;
                if (t.object == nullptr) {
                    Tab* tab = wxGetApp().get_tab(t.type);
                    if (tab == nullptr)
                        throw ToolFailure("The settings tab is not available");
                    // Every value is checked before the first change: the nozzle cards' picks, then the rest parsed.
                    const NozzlePicks nozzles = t.type == Preset::TYPE_PRINTER ? nozzle_picks(values, *t.config) : NozzlePicks();
                    parse_values();
                    if (reset_all) {
                        reverted = collection(t.type).current_dirty_options();
                        tab->on_roll_back_value();
                    }
                    if (t.type == Preset::TYPE_PRINTER)
                        pick_nozzles(nozzles, reset_all && !reverted.empty());
                    try {
                        if (!values.empty())
                            tab->apply_preset_config_values(values);
                    } catch (const std::exception& e) {
                        throw ToolFailure(e.what());
                    }
                } else {
                    DynamicPrintConfig scratch = parse_values();
                    check_filament_numbers(scratch);
                    if (t.range)
                        check_height_range_layer_height(scratch, *t.config, [](int extruder) {
                            return std::make_pair(get_min_layer_height(extruder), get_max_layer_height(extruder));
                        });
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
                        ModelConfig& cfg = t.range             ? t.object->layer_config_ranges.at(*t.range) :
                                           t.volume != nullptr ? static_cast<ModelConfig&>(t.volume->config) :
                                                                 static_cast<ModelConfig&>(t.object->config);
                        for (const std::string& key : scratch.keys())
                            cfg.set_key_value(key, scratch.option(key)->clone());
                        for (const std::string& key : removals)
                            cfg.erase(key);
                        // The row of the object, the part or the height range shows whether it has settings of its own.
                        wxGetApp().obj_list()->object_config_options_changed(ObjectVolumeID{t.object, t.volume},
                                                                             t.range ? &*t.range : nullptr);
                        plater.changed_object(t.object_index);
                    }
                    if (set_extruder) {
                        wxDataViewItemArray rows;
                        rows.Add(extruder_row);
                        wxGetApp().obj_list()->set_extruder_for_items(rows, extruder);
                    }
                }
                // A preset's settings are read again, which a printer's nozzle flow types are copied into; a target's are
                // its own, changed in place.
                const ConfigTarget        reread = t.object == nullptr ? resolve_target(args, false) : ConfigTarget();
                const DynamicPrintConfig& after  = t.object == nullptr ? *reread.config : *t.config;
                json applied = json::object();
                for (auto it = values_in.begin(); it != values_in.end(); ++it)
                    applied[it.key()] = after.has(it.key()) ? json(after.opt_serialize(it.key())) : json();
                for (const std::string& key : removals)
                    applied[key] = nullptr;
                json out{{"applied", applied}, {"changed", changed_settings(before, after)}};
                if (reset_all)
                    out["reverted"] = std::move(reverted);
                if (values_in.contains("extruder") && t.object != nullptr) {
                    // A single-part object shows no part row; its filament is the object's.
                    const bool                on_object = t.volume != nullptr && t.object->volumes.size() == 1;
                    const DynamicPrintConfig& cfg       = on_object ? t.object->config.get() : after;
                    out["applied"]["extruder"]          = cfg.has("extruder") ? json(cfg.opt_serialize("extruder")) : json();
                    if (on_object)
                        out["note"] = "A single-part object's filament is the object's; it was set there.";
                }
                if (t.object != nullptr)
                    out.update(target_reply(t));
                return out;
            });
            settle(ctx);
            return ToolResult::ok(result);
        })));

    // ---- The presets and settings commands made from menu items ----
    using NativeCommands::action;
    const std::string preset_file = "a preset .json, a .zip of them, an .orca_printer, .orca_filament or .orca_bundle, or a .gcode";
    const std::string imported    = "and the message names the presets imported, replaced and kept; a .gcode loads the settings it was "
                                    "sliced with.";
    NativeCommand import_configs = reaching(
        {"import_presets"},
        menu_command("preset_import", L("Import Configs"),
                    "Import presets from " + preset_file + " by absolute path, without a dialog, " + imported +
                        " Called with no arguments it is Import Configs: the file dialog asks for the file (`blocked_by_dialog`).",
                    object_schema({{"path", MenuItems::input_file(preset_file.c_str())},
                                   {"on_conflict", enum_prop({"skip", "overwrite"}, "What happens to a preset that exists already. Default skip.")}},
                                  {"path"}),
                    json::array({action("import_configs", L("Import Configs"), L("Import"), "menu_open", json::object(),
                                        "Import presets from the file a dialog asks for. preset_import with a path imports " + preset_file +
                                            " without a dialog, " + imported)}),
                    [](const json& args) { return MenuItems::import_configs(args); }));
    import_configs.input_schema = with_silent(std::move(import_configs.input_schema), person_forms(import_configs));
    catalog.add(std::move(import_configs));

    // Ruling 16: the dialog opens on every call, and the caller answers it.
    const std::string export_dialog = "Export presets as a bundle or ZIP archives through the Export Preset Bundle dialog: click the "
                                      "label of the kind to export, check its rows (each named after a printer preset or a "
                                      "filament), press OK, then answer the result message.";
    catalog.add(menu_command(
        "preset_export", L("Export Preset Bundle"),
        export_dialog + " Every call opens the dialog (`blocked_by_dialog`). With path, OK writes into that folder instead of "
                        "asking for one, and a cancelled dialog is an error: nothing was exported.",
        object_schema({{"path", prop("string", "Absolute path of an existing folder, which the dialog's OK writes into.")}}),
        json::array({action("export_config", L("Export Preset Bundle"), L("Export"), "save", json::object(),
                            export_dialog + " preset_export with a folder as path has OK write there instead of asking for one.")}),
        [](const json& args) { return MenuItems::export_config(args); }));

    const std::string sync = "Synchronize user presets with the cloud account.";
    NativeCommand preset_sync = reaching({"sync_presets"},
                                         menu_command("preset_sync", L("Sync Presets"), sync + " Refused while no account is signed in.",
                                                     object_schema({}),
                                                     json::array({action("sync_presets", L("Sync Presets"), L("Presets"), "printer_sync_ok",
                                                                         json::object(), sync)}),
                                                     preset_sync_command));
    // It reaches the cloud account.
    preset_sync.open_world = true;
    catalog.add(std::move(preset_sync));

    const std::string migrate = "Migrate legacy user filament presets.";
    catalog.add(menu_command("preset_migrate_filaments", L("Migrate Filament Presets"), migrate, object_schema({}),
                            json::array({action("help_migrate_filament_presets", L("Migrate Filament Presets"), L("Help"), "",
                                                json::object(), migrate)}),
                            [](const json&) { return MenuItems::migrate_filament_presets(); }));

    // The Mode menu items; titles are L("...")-marked here so xgettext extracts them.
    struct ModeMenuItem
    {
        const char* key;
        const char* title;
        const char* mode;
        const char* name;
    };
    static const ModeMenuItem mode_menu_items[] = {{"mode_simple", L("Mode: Simple"), "simple", "Simple"},
                                        {"mode_advanced", L("Mode: Advanced"), "advanced", "Advanced"},
                                        {"mode_expert", L("Mode: Expert"), "expert", "Expert"}};
    json mode_actions = json::array();
    for (const ModeMenuItem& item : mode_menu_items)
        mode_actions.push_back(action(item.key, item.title, L("Mode"), "advanced", {{"mode", item.mode}},
                                      std::string("Switch the settings view to ") + item.name + " mode."));
    catalog.add(menu_command("settings_set_mode", L("Set the settings mode"),
                            "Switch the settings view to Simple, Advanced or Expert mode, which decides which settings the settings "
                            "pages show.",
                            object_schema({{"mode", enum_prop({"simple", "advanced", "expert"}, "The mode.")}}, {"mode"}),
                            std::move(mode_actions), [](const json& args) { return MenuItems::set_mode(arg_string(args, "mode")); }));

    const std::string developer = "Turn Developer mode on or off.";
    NativeCommand developer_mode = menu_command("settings_toggle_developer_mode", L("Toggle Developer Mode"), developer, object_schema({}),
                                               json::array({action("toggle_developer_mode", L("Toggle Developer Mode"), L("Mode"),
                                                                   "advanced", json::object(), developer)}),
                                               [](const json&) { return MenuItems::toggle_developer_mode(); });
    // A menu item with no argument form: tool_list lists it and tool_call runs it, the tool list does not show it.
    developer_mode.listed = false;
    catalog.add(std::move(developer_mode));
}

}}} // namespace Slic3r::GUI::Mcp
