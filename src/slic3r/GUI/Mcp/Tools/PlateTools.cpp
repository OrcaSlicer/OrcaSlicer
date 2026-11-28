// The place Plates: plate_list, plate_get_settings and plate_set_settings, a plate's own settings, written through
// the code of the Plate Settings dialog and the filament grouping; the plate commands made from menu items, which
// act on the plate a call names, else on the current plate, through the plate operations (Operations/PlateOps.hpp),
// plate_add running its menu item's function (NativeCommandMenuItems.hpp); plate_slice, which reports its own
// outcome per plate (see Operations/RunStatus), and plate_slice_stats, what the Preview shows of a sliced plate;
// plate_set_custom_gcode, the pauses, custom G-codes and filament changes of a plate's layers, through the Preview's
// layer slider; and calibration_generate_test, the Calibration menu's tests, which start a new project.

#include "slic3r/GUI/Mcp/AutomationRuntime.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpTargets.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/calib_dlg.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/FilamentMapPanel.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PlateSettingsDialog.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/TickCode.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/GCodeStatistics.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Utils.hpp"
#include "libvgcode/include/ColorRange.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"

#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using Ops::PlateObservation;
using Ops::PlateOutcome;
using Ops::SliceObservation;
using Ops::SliceTicketState;
using Ops::TicketStatus;

namespace {

bool is_plate_setting(const std::string& key)
{
    const std::vector<std::string>& keys = plate_setting_keys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// The plate settings plate_list and an unnamed plate_get_settings show: the filament grouping only on a printer with
// several extruders, whose plates show the filament map icon (a plate keeps a mode of its own on any printer). UI thread.
std::vector<std::string> listed_plate_keys()
{
    std::vector<std::string> keys = plate_setting_keys();
    if (wxGetApp().preset_bundle->get_printer_extruder_count() < 2)
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [](const std::string& key) { return key == "filament_map_mode" || key == "filament_map"; }),
                   keys.end());
    return keys;
}

// The plate the call's `plate` argument names, the current one without it. UI thread.
int scope_plate(const json& args)
{
    PartPlateList& plates = ui_plater().get_partplate_list();
    const int      index  = arg_int(args, "plate", plates.get_curr_plate_index());
    if (index < 0 || index >= plates.get_plate_count())
        throw ToolFailure(no_such_plate(index, plates.get_plate_count()));
    return index;
}

// How many filaments the filament grouping dialog groups: one per slot of the project.
int grouped_filaments()
{
    const auto* colours = ui_plater().config()->option<ConfigOptionStrings>("filament_colour");
    return colours != nullptr ? int(colours->size()) : 0;
}

// The extruder of each filament as the plate's filament grouping dialog shows it: the plate's own map, else the
// project's, extruder 1 for a filament it lacks. With `volumes`, the nozzle volume type of each filament the same way,
// Standard for one it lacks.
std::vector<int> shown_filament_map(const PartPlate& plate, bool volumes = false)
{
    const DynamicPrintConfig& project = wxGetApp().preset_bundle->project_config;
    std::vector<int> map = volumes ? plate.get_real_filament_volume_maps(project) : plate.get_real_filament_maps(project);
    map.resize(std::size_t(grouped_filaments()), volumes ? 0 : 1);
    return map;
}

// The filament_map_mode values the plate's filament grouping dialog offers: none on a printer with one extruder, whose
// plates show no filament map icon, and Auto For Match, which groups by the filaments a printer has loaded, once that
// printer is synced and has no filament track switch ready. UI thread.
std::vector<std::string> offered_map_modes()
{
    if (wxGetApp().preset_bundle->get_printer_extruder_count() < 2)
        return {};
    Plater&                  plater = ui_plater();
    std::vector<std::string> modes{"Auto For Flush"};
    if (plater.get_machine_sync_status() && !plater.sidebar().is_fila_switch_ready())
        modes.push_back("Auto For Match");
    modes.push_back("Manual");
    return modes;
}

// A plate's filament_map_mode as plate_set_settings takes it: an offered mode, or null, no mode of the plate's own, so
// that it follows the project's grouping (PartPlate::set_filament_map_mode() with fmmDefault; the plate's grouping
// dialog offers no page for it). UI thread.
FilamentMapMode filament_map_mode_value(const json& value)
{
    if (value.is_null())
        return fmmDefault;
    const std::vector<std::string> offered = offered_map_modes();
    if (!value.is_string() || std::find(offered.begin(), offered.end(), value.get<std::string>()) == offered.end())
        throw ToolFailure("filament_map_mode is a mode the plate's filament grouping offers (meta.offered of plate_get_settings), "
                          "or null for the project's grouping" +
                              std::string(value == "Auto For Match" ? "; Auto For Match groups by the filaments a printer has "
                                                                      "loaded, and is offered once that printer is synced" :
                                                                      ""),
                          {{"offered", offered}});
    return FilamentMapMode(ConfigOptionEnum<FilamentMapMode>::get_enum_values().at(value.get<std::string>()));
}

// A plate setting in the form plate_set_settings takes: the plate's own value, null where it follows the
// project's plate type, the process, Auto or the project's filament grouping; or with `effective` what the plate
// prints with.
json plate_value(PartPlate& plate, const std::string& key, bool effective)
{
    if (key == "filament_map_mode") {
        const FilamentMapMode mode = effective ? plate.get_real_filament_map_mode(wxGetApp().preset_bundle->project_config) :
                                                 plate.get_filament_map_mode();
        return mode == fmmDefault ? json() : json(ConfigOptionEnum<FilamentMapMode>(mode).serialize());
    }
    if (key == "filament_map") {
        const std::vector<int> own = plate.get_filament_maps();
        return effective ? json(shown_filament_map(plate)) : own.empty() ? json() : json(own);
    }
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

// Every listed setting of a plate (listed_plate_keys()): its own value, or with `effective` what it prints with.
json plate_values(PartPlate& plate, bool effective)
{
    json out = json::object();
    for (const std::string& key : listed_plate_keys())
        out[key] = plate_value(plate, key, effective);
    return out;
}

// plate_get_settings: every key, or those named, with the plate's own value and the one it prints with. UI thread.
json plate_settings(const json& args)
{
    const int                index = scope_plate(args);
    PartPlate&               plate = *ui_plater().get_partplate_list().get_plate(index);
    std::vector<std::string> keys  = listed_plate_keys();
    json                     unknown = json::array();
    if (args.contains("keys")) {
        keys.clear();
        for (const json& k : args["keys"]) {
            const std::string key = k.get<std::string>();
            if (is_plate_setting(key))
                keys.push_back(key);
            else
                unknown.push_back(key);
        }
    }
    json items = json::array();
    for (const std::string& key : keys) {
        json item{{"key", key}, {"value", plate_value(plate, key, false)}, {"effective", plate_value(plate, key, true)}};
        if (arg_bool(args, "include_meta", true)) {
            item["meta"] = option_meta(*print_config_def.get(key));
            if (key == "curr_bed_type")
                item["meta"]["offered"] = project_offered(key);
            if (key == "filament_map_mode")
                item["meta"]["offered"] = offered_map_modes();
        }
        items.push_back(std::move(item));
    }
    json out     = page(items, args, 100);
    out["plate"] = index;
    add_unknown_keys(out, unknown, listed_plate_keys());
    return out;
}

// plate_set_settings. The Plate Settings dialog's values go through its OK (Plater::apply_plate_settings()) with the
// plate's other values as they are, after selecting the plate as its settings icon does; the filament grouping's go
// through the OK of the dialog the plate's filament map icon opens (Plater::open_filament_map_setting_dialog()), which
// takes no undo snapshot of its own. Everything is checked before anything changes, a key a plate's settings lack
// before the app is read, and the call is one undo step.
ToolResult set_plate_settings(const json& args, ToolContext& ctx)
{
    const json values_in = args.value("values", json::object());
    if (values_in.empty())
        throw ToolFailure("values must be a non-empty object {key: value}");
    int  index = -1;
    json reply = ctx.run_ui([&]() -> json {
        for (auto it = values_in.begin(); it != values_in.end(); ++it)
            if (!is_plate_setting(it.key()))
                throw unknown_setting(it.key(), "plate", {{"keys", plate_setting_keys()}});
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
        // The filament grouping dialog's choices: its mode, and the map its Custom page sets.
        const FilamentMapMode           own_mode     = plate.get_filament_map_mode();
        FilamentMapMode                 map_mode     = own_mode;
        std::optional<std::vector<int>> map;
        const int                       extruders    = wxGetApp().preset_bundle->get_printer_extruder_count();
        for (auto it = values_in.begin(); it != values_in.end(); ++it) {
            const std::string& key   = it.key();
            const json&        value = it.value();
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
                    throw ToolFailure(key + " cannot be set while the project has mixed filaments (filament_list's mixed rows); "
                                            "null sets Auto");
                if (key == "first_layer_print_sequence")
                    first_layer = value.is_null() ? std::vector<int>() : filament_order(value, slots);
                else
                    other_layers = value.is_null() ? std::vector<LayerPrintSequence>() : layer_sequences(value, slots);
            } else {
                // The plate shows its filament map icon on a printer with several extruders alone.
                if (extruders < 2)
                    throw ToolFailure(key + " groups the filaments onto the extruders of a printer that has several; this one has one");
                if (key == "filament_map_mode") {
                    map_mode = filament_map_mode_value(value);
                } else {
                    if (!value.is_array() ||
                        std::any_of(value.begin(), value.end(), [](const json& extruder) { return !extruder.is_number_integer(); }))
                        throw ToolFailure("filament_map is the extruder of each filament, 1-based, e.g. [1, 2, 2]");
                    map = value.get<std::vector<int>>();
                }
            }
        }
        if (map) {
            const std::string mode = map_mode == fmmDefault ? std::string() : ConfigOptionEnum<FilamentMapMode>(map_mode).serialize();
            if (const std::string err = filament_map_error(mode, *map, grouped_filaments(), extruders); !err.empty())
                throw ToolFailure(err);
        }
        const bool settings = bed_type != plate.get_bed_type() || sequence != plate.get_print_seq() ||
                              first_layer != plate.get_first_layer_print_sequence() ||
                              other_layers != plate.get_other_layers_print_sequence() || spiral != own_spiral;
        // The grouping dialog's OK: in the Manual mode the map and nozzle volume types its Custom page shows become the
        // plate's own, and the slice is invalidated when the mode or what the plate prints with changes.
        const DynamicPrintConfig& project     = wxGetApp().preset_bundle->project_config;
        const std::vector<int>    old_maps    = plate.get_real_filament_maps(project);
        const std::vector<int>    old_volumes = plate.get_real_filament_volume_maps(project);
        const std::vector<int>    new_maps    = map ? *map : shown_filament_map(plate);
        const std::vector<int>    new_volumes = shown_filament_map(plate, true);
        const bool                manual      = map_mode == fmmManual;
        const bool                regrouped   = map_mode != own_mode || old_maps != new_maps || old_volumes != new_volumes;
        const bool                own_map     = manual && (plate.get_filament_maps() != new_maps ||
                                                    plate.get_filament_volume_maps() != new_volumes);
        const bool grouping_given = values_in.contains("filament_map_mode") || values_in.contains("filament_map");
        const bool grouping       = grouping_given && (regrouped || own_map);
        // The Custom page disables the dialog's OK while an extruder its map gives a filament the plate prints has no
        // nozzle of the volume type that filament needs.
        if (const std::optional<NozzleShortage> shortage = manual && grouping_given ?
                manual_map_nozzle_shortage(nozzle_inventory(*wxGetApp().preset_bundle), plate.get_extruders(true), new_maps,
                                           new_volumes) :
                std::nullopt) {
            const std::string type = ConfigOptionEnum<NozzleVolumeType>(shortage->volume_type).serialize();
            const char*       side = shortage->extruder == 0 ? " (left)" : shortage->extruder == 1 ? " (right)" : "";
            throw ToolFailure("The manual grouping gives extruder " + std::to_string(shortage->extruder + 1) + side +
                                  " a filament that prints with a " + type + " nozzle, and the extruder has no " + type +
                                  " nozzle, so the filament grouping dialog does not take this map: give that filament another "
                                  "extruder, or set the extruder's nozzles first",
                              {{"code", "no_nozzle_for_grouping"}, {"extruder", shortage->extruder + 1}, {"nozzle_volume_type", type}});
        }
        if (settings || grouping) {
            // The settings and filament map icons select their plate first; the dialogs work on the current plate.
            if (index != plates.get_curr_plate_index())
                plater.select_plate(index);
            Plater::SingleSnapshot single(&plater);
            plater.take_snapshot("Plate Settings");
            if (settings)
                plater.apply_plate_settings(index, bed_type, first_layer, other_layers, sequence, spiral);
            if (grouping) {
                if (map_mode != own_mode)
                    plate.set_filament_map_mode(map_mode);
                if (manual) {
                    plate.set_filament_maps(new_maps);
                    plate.set_filament_volume_maps(new_volumes);
                }
                if (regrouped) {
                    plate.update_slice_result_valid_state(false);
                    plater.set_plater_dirty(true);
                    plater.update();
                }
            }
        }
        const json after     = plate_values(plate, false);
        json       applied   = json::object();
        json       effective = json::object();
        json       changed   = json::object();
        for (auto it = values_in.begin(); it != values_in.end(); ++it) {
            applied[it.key()]   = after.value(it.key(), json());
            effective[it.key()] = plate_value(plate, it.key(), true);
        }
        for (auto it = after.begin(); it != after.end(); ++it)
            if (it.value() != before.value(it.key(), json()))
                changed[it.key()] = it.value();
        return json{{"plate", index}, {"applied", applied}, {"changed", changed}, {"effective", effective}};
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

// How many objects have an instance on plate `plate`; object_list lists them.
int objects_on_plate(const Model& model, PartPlateList& plates, int plate)
{
    int objects = 0;
    for (std::size_t o = 0; o < model.objects.size(); ++o) {
        const std::size_t instances = model.objects[o]->instances.size();
        std::size_t       j         = 0;
        while (j < instances && instance_plate(plates, int(o), int(j)) != plate)
            ++j;
        objects += j < instances;
    }
    return objects;
}

// plate_list: every plate's name, lock, settings as it prints them, what it sets itself, its objects, place on the bed,
// slice state, prime tower position and layer G-codes. UI thread.
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
        entry["name"]    = plate.get_plate_name();
        entry["locked"]  = plate.is_locked();
        entry["current"] = i == plates.get_curr_plate_index();
        json overrides   = json::array();
        for (const std::string& key : listed_plate_keys())
            if (!plate_value(plate, key, false).is_null())
                overrides.push_back(key);
        entry["overrides"] = std::move(overrides);
        entry["objects"]   = objects_on_plate(model, plates, i);
        entry.update(plate_place(plate));
        entry["sliced"]    = plate.is_slice_result_valid();
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

// ---- The plate commands made from menu items ----

// What a plate command made from menu items answers for a refusal: as information where its item told it so (no plates to
// edit, the only plate, no room for another, a slice running), else as an error.
AppActionRunResult plate_refused(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    const bool        info = refusal.kind == Ops::RefusalKind::Busy || code == "no_plates" || code == "last_plate" ||
                      code == "too_many_plates";
    return NativeCommands::refused(refusal, info ? AppActionRunResult::Level::Info : AppActionRunResult::Level::Error);
}

// The plate a call's `plate` argument names; none without it, for the current plate.
std::optional<int> named_plate(const json& args)
{
    return args.contains("plate") ? std::optional<int>(arg_int(args, "plate", 0)) : std::nullopt;
}

// The plates as a command that adds or removes one replies with them afterwards: each plate's index, name and how many
// objects stand on it, and the current plate. UI thread.
json plates_reply()
{
    Plater&        plater = ui_plater();
    PartPlateList& plates = plater.get_partplate_list();
    json           items  = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        items.push_back(
            {{"index", i}, {"name", plates.get_plate(i)->get_plate_name()}, {"objects", objects_on_plate(plater.model(), plates, i)}});
    return {{"plates", std::move(items)}, {"current", plates.get_curr_plate_index()}};
}

AppActionRunResult plate_delete_command(const json& args)
{
    const Ops::Outcome<int> deleted = Ops::delete_plate({named_plate(args)});
    if (!deleted)
        return plate_refused(deleted.refusal());
    json reply     = plates_reply();
    reply["plate"] = deleted.value();
    return menu_reply(reply);
}

AppActionRunResult plate_duplicate_command(const json& args)
{
    const Ops::Outcome<Ops::PlateCopy> made = Ops::duplicate_plate({named_plate(args)});
    if (!made)
        return plate_refused(made.refusal());
    json reply         = plates_reply();
    reply["plate"]     = made.value().plate;
    reply["new_plate"] = made.value().copy;
    return menu_reply(reply);
}

// plate_rename: a call with no arguments asks for the current plate's name in its name dialog, as the menu item does; any
// other gives it.
AppActionRunResult plate_rename_command(const json& args)
{
    Ops::RenamePlateArgs op;
    op.plate = named_plate(args);
    if (person_form(args)) {
        if (std::optional<Ops::Refusal> refusal = Ops::plates_refusal())
            return plate_refused(*refusal);
        PlateNameEditDialog dlg(wxGetApp().mainframe, wxID_ANY, _L("Edit Plate Name"));
        dlg.set_plate_name(from_u8(ui_plater().get_partplate_list().get_curr_plate()->get_plate_name()));
        // The person called it off, and knows nothing was renamed.
        if (dlg.ShowModal() != wxID_YES)
            return quiet_refusal(Ops::plate_not_renamed());
        op.name = into_u8(dlg.get_plate_name());
    } else {
        op.name = arg_string(args, "name");
    }
    const Ops::Outcome<int> renamed = Ops::rename_plate(op);
    if (!renamed)
        return plate_refused(renamed.refusal());
    const PartPlate& plate = *ui_plater().get_partplate_list().get_plate(renamed.value());
    return menu_reply({{"plate", renamed.value()}, {"name", plate.get_plate_name()}});
}

AppActionRunResult plate_lock_command(const json& args)
{
    Ops::SetPlateLockedArgs op;
    op.plate = named_plate(args);
    if (args.contains("locked"))
        op.locked = arg_bool(args, "locked", true);
    const Ops::Outcome<Ops::PlateLocked> locked = Ops::set_plate_locked(op);
    if (!locked)
        return plate_refused(locked.refusal());
    return menu_reply({{"plate", locked.value().plate}, {"locked", locked.value().locked}, {"changed", locked.value().changed}});
}

// plate_select: Ops::select_plate() also serves the commands that select a plate on the way, so the check of the menu items,
// that the project has plates to select, is made here.
AppActionRunResult plate_select_command(const json& args)
{
    if (std::optional<Ops::Refusal> refusal = Ops::plates_refusal())
        return plate_refused(*refusal);
    Ops::SelectPlateArgs op;
    op.plate = arg_int(args, "plate", ui_plater().get_partplate_list().get_curr_plate_index());
    if (const Ops::Outcome<Ops::Done> selected = Ops::select_plate(op); !selected)
        return plate_refused(selected.refusal());
    return menu_reply({{"plate", op.plate}});
}

} // namespace

json plate_place(PartPlate& plate)
{
    // The plate's shape on the bed, moved to where the plate list places the plate; the extended box holds the grabber
    // in front of it too.
    const BoundingBoxf3& box        = plate.get_bounding_box();
    const auto           rounded_mm =[](double v) { return rounded(v, 1000.) + 0.; };
    return {{"origin", {rounded_mm(box.min.x()), rounded_mm(box.min.y())}},
            {"size", {rounded_mm(box.size().x()), rounded_mm(box.size().y())}}};
}

const std::vector<std::string>& plate_setting_keys()
{
    static const std::vector<std::string> keys{"curr_bed_type",
                                               "print_sequence",
                                               "spiral_mode",
                                               "first_layer_print_sequence",
                                               "other_layers_print_sequence",
                                               "filament_map_mode",
                                               "filament_map"};
    return keys;
}

std::string filament_map_error(const std::string& mode, const std::vector<int>& map, int filaments, int extruders)
{
    if (mode != "Manual")
        return "filament_map is a plate's own grouping, which filament_map_mode \"Manual\" takes: the automatic modes group the "
               "filaments when the plate slices, and null follows the project's grouping";
    if (int(map.size()) != filaments)
        return "filament_map gives one extruder per filament: " + std::to_string(filaments) + " filaments, " +
               std::to_string(map.size()) + " given";
    for (const int extruder : map)
        if (extruder < 1 || extruder > extruders)
            return "filament_map names extruder " + std::to_string(extruder) + "; this printer has extruders 1 to " +
                   std::to_string(extruders);
    return {};
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

namespace {

// The plate_slice tool's view of its request: slice_status()'s state, which adds when the tool started
// waiting to what the operation's ticket holds.
struct SliceTicket
{
    SliceTicketState state;
    std::vector<int> plates;
};

} // namespace

json plate_summary(const json& statistics, const GCodeProcessorResult& result)
{
    const auto   hundredths = [](double value) { return std::round(value * 100.) / 100.; };
    const json&  summary    = statistics.at("summary");
    const double seconds    = summary.at("time").get<double>();
    json         out{{"time_s", std::lround(seconds)},
                     {"time", get_time_dhms(float(seconds))},
                     {"filament_g", hundredths(summary.at("weight").get<double>())},
                     {"filament_cost", summary.at("filament_cost")},
                     {"layers", summary.at("layer_count")},
                     {"filament_changes", summary.at("filament_changes")}};
    // The processor leaves the silent-mode time 0 when the printer has no such estimate.
    const PrintEstimatedStatistics& estimates = result.print_statistics;
    const double silent = estimates.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time;
    if (silent > 0.)
        out["silent_time_s"] = std::lround(silent);
    if (estimates.total_extruder_changes > 0)
        out["extruder_changes"] = estimates.total_extruder_changes;
    double max_z = 0.;
    for (const json& layer : statistics.at("layers"))
        max_z = std::max(max_z, layer.at("z").get<double>());
    out["max_z"] = max_z;

    json filaments = json::array();
    const auto add_slot = [&filaments] {
        filaments.push_back({{"slot", filaments.size() + 1}, {"m", 0.}, {"g", 0.}, {"model_g", 0.}, {"support_g", 0.},
                             {"flush_g", 0.}, {"tower_g", 0.}});
    };
    while (filaments.size() < result.filaments_count)
        add_slot();
    double length = 0.;
    for (const json& used : summary.at("filaments")) {
        const size_t slot = used.at("id").get<size_t>();
        while (filaments.size() < slot)
            add_slot();
        const auto grams = [&used, &hundredths](const char* use) { return hundredths(used.at(use).at("weight").get<double>()); };
        json&      f     = filaments[slot - 1];
        f["m"]           = hundredths(used.at("length").get<double>() / 1000.);
        f["g"]           = hundredths(used.at("weight").get<double>());
        f["model_g"]     = grams("model");
        f["support_g"]   = grams("support");
        f["flush_g"]     = grams("flush");
        f["tower_g"]     = grams("prime_tower");
        length += used.at("length").get<double>();
    }
    out["filament_m"] = hundredths(length / 1000.);
    out["filaments"]  = std::move(filaments);
    return out;
}

namespace {

// The plate plate_set_custom_gcode names (default: the current one), after the checks that need neither its
// layers nor a view.
int gcode_plate(Plater& plater, const json& args)
{
    if (plater.printer_technology() != ptFFF || plater.only_gcode_mode())
        throw ToolFailure("Layer G-codes are for the plates of an FFF project");
    const bool edits = arg_bool(args, "clear", false) || !args.value("add", json::array()).empty() ||
                       !args.value("remove", json::array()).empty();
    if (!edits)
        throw ToolFailure("Give add, remove or clear; plate_list lists each plate's layer_gcode");
    PartPlateList& plates = plater.get_partplate_list();
    const int      index  = arg_int(args, "plate", plates.get_curr_plate_index());
    if (index < 0 || index >= plates.get_plate_count())
        throw ToolFailure(no_such_plate(index, plates.get_plate_count()));
    PartPlate&        plate = *plates.get_plate(index);
    const std::string name  = "plate " + std::to_string(index);
    if (!plate.has_printable_instances())
        throw ToolFailure(name + " has nothing to print, so no layers to place codes on");
    if (plate.get_real_print_seq() == PrintSequence::ByObject)
        throw ToolFailure(name + " prints by object (print_sequence). Edit each instance's layer G-codes in the Preview.");
    // A slice would replace the layers under the edit.
    if (plater.is_slicing_any())
        throw ToolFailure("Slicing is in progress; app_wait {for: \"idle\"} first");
    return index;
}

// Whether the Preview shows the plate's layers: its layer slider holds that plate's layers and codes,
// as when a person opened it. A plate edited after its slice and still shown keeps them, so a person
// can go on editing it, and so can MCP.
bool preview_shows_plate(Plater& plater, int index)
{
    PartPlateList& plates  = plater.get_partplate_list();
    GLCanvas3D*    preview = plater.get_preview_canvas3D();
    if (index != plates.get_curr_plate_index() || !plater.is_preview_shown() || preview == nullptr)
        return false;
    GCodeViewer& viewer = preview->get_gcode_viewer();
    return viewer_holds(viewer, plates.get_plate(index)->get_slice_result()) &&
           viewer.get_layers_slider()->GetMaxValue() + 1 == int(preview->get_gcode_layers_zs().size());
}

// A plate's codes as plate_list's layer_gcode lists them: by height, z to the G-code's 0.001 mm, layer
// (1-based) where `zs` has a layer at or above z, the slider's rule for a stored height.
json plate_layer_gcode(const CustomGCode::Info& codes, const std::vector<double>& zs)
{
    std::vector<CustomGCode::Item> items = codes.gcodes;
    std::stable_sort(items.begin(), items.end());
    json out = json::array();
    for (const CustomGCode::Item& item : items) {
        json       code{{"z", std::round(item.print_z * 1000.) / 1000.}, {"type", custom_gcode_type_name(item.type)}};
        const auto layer = std::lower_bound(zs.begin(), zs.end(), item.print_z - epsilon());
        if (layer != zs.end())
            code["layer"] = int(layer - zs.begin()) + 1;
        const bool change = item.type == CustomGCode::ToolChange || item.type == CustomGCode::ColorChange;
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

// plate_set_custom_gcode's reply, once the plater has the slider's codes. UI thread.
json layer_gcode_reply(Plater& plater, int index, bool changed, const json& args)
{
    const Model&              model = plater.model();
    const auto                codes = model.plates_custom_gcodes.find(index);
    const std::vector<double> zs    = plater.get_preview_canvas3D()->get_gcode_layers_zs();
    json                      out{{"plate", index},
                                  {"layers", zs.size()},
                                  {"changed", changed},
                                  {"layer_gcode", codes != model.plates_custom_gcodes.end() ? plate_layer_gcode(codes->second, zs) : json::array()}};
    if (changed)
        out["note"] = "The plate's slice no longer matches its codes; slice it again before exporting.";
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    for (const json& item : args.value("add", json::array()))
        if (arg_string(item, "type") == "pause" && bundle.printers.get_edited_preset().config.opt_string("machine_pause_gcode").empty()) {
            out["warnings"] = json::array({"machine_pause_gcode is empty, so a pause adds no command (set it with settings_set type printer)"});
            break;
        }
    return out;
}

// Selects the plate plate_set_custom_gcode names and shows it in the Preview, unless the Preview shows its layers
// already, and returns its index. Refused calls leave the view alone. Only a plate with an up-to-date slice
// is opened: opening the Preview on one that is not starts a slice.
int show_plate_layers(Plater& plater, const json& args)
{
    const int index = gcode_plate(plater, args);
    if (preview_shows_plate(plater, index))
        return index;
    PartPlateList& plates = plater.get_partplate_list();
    if (!plates.get_plate(index)->is_slice_result_valid())
        throw ToolFailure("plate " + std::to_string(index) + " has no up-to-date slice to place codes on; slice it first (plate_slice {plates: " +
                          std::to_string(index) + "})");
    plater.select_view_3D("Preview");
    // What clicking a sliced plate in the Preview's plate toolbar does: select it and load its result, which
    // a plate switch alone leaves to the next slice while the Preview shows.
    plater.select_sliced_plate(index, true);
    if (MainFrame* frame = wxGetApp().mainframe)
        frame->select_tab(wxString(TAB_ID_PREVIEW));
    return index;
}

// The edit itself: the plate's layers are in the Preview's layer slider, which takes the codes, and the
// next frame (or this call, at once) hands them to the plater, as for a person's edit.
json set_custom_gcode_at_layer(Plater& plater, int index, const json& args, CustomGCode::Info& handed_over)
{
    gcode_plate(plater, args);
    if (!preview_shows_plate(plater, index))
        throw ToolFailure("The Preview no longer shows plate " + std::to_string(index) + "'s layers; call again");
    GLCanvas3D&         preview = *plater.get_preview_canvas3D();
    IMSlider&           slider  = *preview.get_gcode_viewer().get_layers_slider();
    const PresetBundle& bundle  = *wxGetApp().preset_bundle;
    const bool          spiral  = bundle.prints.get_edited_preset().config.opt_bool("spiral_mode");
    bool                changed = false;
    try {
        changed = edit_layer_gcode(slider, args, int(plater.get_extruder_colors_from_plater_config(nullptr).size()),
                                   !bundle.printers.get_edited_preset().config.opt_string("template_custom_gcode").empty(),
                                   slider.can_change_color() ? std::string() :
                                   spiral                    ? std::string("spiral vase (spiral_mode) is on") :
                                                               std::string("its objects print with different filaments"));
    } catch (const ToolFailure&) {
        // Only the slider refusing a change comes after earlier changes; hand those over, as the next frame would.
        preview.send_layer_ticks_changes();
        throw;
    }
    if (!changed)
        return layer_gcode_reply(plater, index, false, args);
    handed_over = slider.GetTicksValues();
    preview.send_layer_ticks_changes();
    return json();
}

} // namespace

const char* custom_gcode_type_name(CustomGCode::Type type)
{
    switch (type) {
    case CustomGCode::PausePrint: return "pause";
    case CustomGCode::Custom: return "custom";
    case CustomGCode::Template: return "template";
    case CustomGCode::ToolChange: return "change_filament";
    case CustomGCode::ColorChange: return "color_change";
    default: return "unknown";
    }
}

CustomGCode::Type custom_gcode_type_from_name(const std::string& name)
{
    for (const CustomGCode::Type type : {CustomGCode::PausePrint, CustomGCode::Custom, CustomGCode::Template, CustomGCode::ToolChange})
        if (name == custom_gcode_type_name(type))
            return type;
    return CustomGCode::Unknown;
}

bool edit_layer_gcode(IMSlider& slider, const json& args, int filaments, bool has_template, const std::string& no_filament_change)
{
    const int  layers = slider.GetMaxValue() + 1;
    const auto layer  = [](int tick) { return "layer " + std::to_string(tick + 1); };
    // `layer` counts from 1, as the slider's labels do; `z` is the layer whose top is at or above it, where
    // the slider puts a stored height.
    const auto item_tick = [&](const json& item) {
        const bool by_layer = item.contains("layer");
        if (by_layer == item.contains("z"))
            throw ToolFailure("Give layer or z for each code");
        if (by_layer) {
            const int n = arg_int(item, "layer", 0);
            if (n < 1 || n > layers)
                throw ToolFailure("layer must be 1.." + std::to_string(layers) + " (z " + mm(slider.GetMinValueD()) + ".." +
                                  mm(slider.GetMaxValueD()) + ")");
            return n - 1;
        }
        const double z    = arg_double(item, "z", 0.);
        const int    tick = slider.get_tick_from_value(z, true);
        if (tick < 0)
            throw ToolFailure("z " + mm(z) + " is above the top layer (" + mm(slider.GetMaxValueD()) + " mm)");
        return tick;
    };
    const auto listed = [](const std::map<int, TickCode>& codes) {
        std::string text;
        for (const auto& entry : codes)
            text += (text.empty() ? "" : ", ") + std::to_string(entry.first + 1);
        return codes.empty() ? std::string("no codes") : (codes.size() == 1 ? "a code on layer " : "codes on layers ") + text;
    };

    // Everything is checked on a copy first, so a refused call leaves the slider as it was.
    std::map<int, TickCode> before;
    for (const TickCode& code : slider.GetTicks())
        before.emplace(code.tick, code);
    std::map<int, TickCode> after = arg_bool(args, "clear", false) ? std::map<int, TickCode>() : before;
    for (const json& item : args.value("remove", json::array())) {
        const int tick = item_tick(item);
        if (after.erase(tick) == 0)
            throw ToolFailure("No code on " + layer(tick) + " (" + listed(after) + ")");
    }
    std::set<int> added;
    for (const json& item : args.value("add", json::array())) {
        const int               tick = item_tick(item);
        const CustomGCode::Type type = custom_gcode_type_from_name(arg_string(item, "type"));
        if (type == CustomGCode::Unknown)
            throw ToolFailure("type must be pause, custom, template or change_filament");
        if (!added.insert(tick).second)
            throw ToolFailure("Two codes for " + layer(tick));
        if (item.contains("filament") != (type == CustomGCode::ToolChange))
            throw ToolFailure(type == CustomGCode::ToolChange ? "change_filament needs filament" : "filament goes with type change_filament");
        if (item.contains("gcode") != (type == CustomGCode::Custom))
            throw ToolFailure(type == CustomGCode::Custom ? "custom needs gcode" : "gcode goes with type custom");
        TickCode code{tick, type};
        // The menus' rules: Add Custom Template only with a template, Change Filament only with two or more
        // filaments and while the slider allows it, and the custom G-code dialog's buffer.
        if (type == CustomGCode::Custom) {
            code.extra = arg_string(item, "gcode");
            if (code.extra.empty() || code.extra.size() > 1023)
                throw ToolFailure("gcode must be 1 to 1023 bytes, as the Custom G-code dialog takes");
        } else if (type == CustomGCode::Template && !has_template) {
            throw ToolFailure("The printer's template_custom_gcode is empty; set it (settings_set type printer) or use type custom");
        } else if (type == CustomGCode::ToolChange) {
            // The slider colours each filament change from the filaments of the slice.
            const int slots = int(slider.GetExtruderColors().size());
            if (slots != filaments)
                throw ToolFailure("The filaments changed since the plate was sliced; slice it again");
            if (slots < 2)
                throw ToolFailure("change_filament needs two or more filaments; on a printer without a tool changer, add a filament "
                                  "slot and turn on manual_filament_change");
            if (!no_filament_change.empty())
                throw ToolFailure("The plate takes no filament change: " + no_filament_change);
            code.extruder = arg_int(item, "filament", 0);
            if (code.extruder < 1 || code.extruder > slots)
                throw ToolFailure("filament must be 1.." + std::to_string(slots));
        }
        // One code per layer, as the slider's menus allow: on a layer with a code they only edit it.
        if (const auto it = after.find(tick); it != after.end() && it->second.type != type)
            throw ToolFailure(layer(tick) + " has a " + custom_gcode_type_name(it->second.type) + " code; remove it first (remove: [{layer: " +
                              std::to_string(tick + 1) + "}])");
        after[tick] = code;
    }

    // Then the menus' own edits: Delete, Add, Edit Custom G-code and Change Filament on a filament change.
    const auto same = [](const TickCode& a, const TickCode& b) {
        return a.type == b.type && (a.type != CustomGCode::ToolChange || a.extruder == b.extruder) &&
               (a.type != CustomGCode::Custom || a.extra == b.extra);
    };
    const auto edited_in_place = [](const TickCode& from, const TickCode& to) {
        return from.type == to.type && (to.type == CustomGCode::Custom || to.type == CustomGCode::ToolChange);
    };
    bool changed = false;
    // The plater invalidates the plate's slice on the type of the last edit, which must not be a colour
    // change's when other codes changed too.
    CustomGCode::Type event = CustomGCode::Unknown;
    for (const auto& [tick, code] : before) {
        const auto it = after.find(tick);
        if (it == after.end() || (!same(code, it->second) && !edited_in_place(code, it->second))) {
            slider.delete_tick(code);
            changed = true;
            if (code.type != CustomGCode::ColorChange)
                event = code.type;
        }
    }
    for (const auto& [tick, code] : after) {
        if (const auto it = before.find(tick); it != before.end() && same(it->second, code))
            continue;
        if (code.type == CustomGCode::Custom)
            slider.add_custom_gcode(code.extra, tick);
        else if (!slider.add_code_as_tick(code.type, code.type == CustomGCode::ToolChange ? code.extruder : -1, tick))
            throw ToolFailure(std::string("The layer slider refused the ") + custom_gcode_type_name(code.type) + " on " + layer(tick) +
                              (changed ? "; the codes before it changed (plate_list lists them)" : ""));
        changed = true;
        event   = code.type;
    }
    if (changed && event != CustomGCode::Unknown)
        slider.post_ticks_changed_event(event);
    return changed;
}

namespace {

// The view types the legend shows a colour range for; the logarithmic layer time repeats the linear one.
constexpr libvgcode::EViewType range_view_types[] = {
    libvgcode::EViewType::Height,       libvgcode::EViewType::Width,
    libvgcode::EViewType::Speed,        libvgcode::EViewType::ActualSpeed,
    libvgcode::EViewType::Acceleration, libvgcode::EViewType::Jerk,
    libvgcode::EViewType::VolumetricFlowRate, libvgcode::EViewType::ActualVolumetricFlowRate,
    libvgcode::EViewType::LayerTimeLinear,    libvgcode::EViewType::FanSpeed,
    libvgcode::EViewType::Temperature,        libvgcode::EViewType::PressureAdvance};

// preview_state() of the Preview's toolpaths, plus which legend rows are visible and the colour range of
// every view type. Ranges span the whole print, but count travels, wipes and Custom extrusions only
// while the legend shows them. UI thread.
json preview_view(GLCanvas3D& canvas)
{
    // The viewer recomputes its ranges at its next frame after a legend change; nothing draws while
    // the Preview is hidden, and nothing changes the legend then either.
    canvas.set_as_dirty();
    canvas.render();
    GCodeViewer&             viewer    = canvas.get_gcode_viewer();
    const libvgcode::Viewer& toolpaths = viewer.get_viewer();
    json                     view      = preview_state(viewer);
    json                     features  = json::object();
    for (const libvgcode::EGCodeExtrusionRole role : toolpaths.get_extrusion_roles())
        features[legend_feature_key(role)] = toolpaths.is_extrusion_role_visible(role);
    json options = json::object();
    for (const libvgcode::EOptionType type : toolpaths.get_options())
        if (const char* key = legend_option_key(type))
            options[key] = toolpaths.is_option_visible(type);
    view["legend"] = {{"features", std::move(features)}, {"options", std::move(options)}};
    json ranges    = json::object();
    for (const libvgcode::EViewType type : range_view_types) {
        // [FLT_MAX, -FLT_MAX] when the print has no such value.
        const std::array<float, 2>& range = toolpaths.get_color_range(type).get_range();
        if (range[0] <= range[1])
            ranges[GCodeViewer::view_type_to_config_name(type)] = {rounded(range[0], 1e4), rounded(range[1], 1e4)};
    }
    view["ranges"] = std::move(ranges);
    return view;
}

} // namespace

json preview_features(const json& statistics)
{
    const json&  summary = statistics.at("summary");
    const double total   = summary.at("time").get<double>();
    const auto   row     = [total](double seconds) {
        return json{{"time_s", rounded(seconds, 100.)}, {"percent", total > 0. ? rounded(100. * seconds / total, 10.) : 0.}};
    };
    json out = json::object();
    for (const auto& feature : summary.at("features").items()) {
        json r = row(feature.value().at("time").get<double>());
        r["g"] = rounded(feature.value().at("weight").get<double>(), 100.);
        out[feature.key()] = std::move(r);
    }
    json travel   = row(summary.at("travel_time").get<double>());
    travel["mm"]  = rounded(summary.at("travel_distance").get<double>(), 10.);
    out["Travel"] = std::move(travel);
    return out;
}

json preview_layers(const json& statistics, bool with_features)
{
    json   items = json::array();
    double below = 0.;
    for (const json& layer : statistics.at("layers")) {
        const double z = layer.at("z").get<double>();
        json         item{{"layer", items.size() + 1},
                          {"z", z},
                          {"height", rounded(z > below ? z - below : z, 1000.)},
                          {"time_s", rounded(layer.at("time").get<double>(), 100.)},
                          {"g", rounded(layer.at("weight").get<double>(), 1000.)}};
        if (with_features) {
            json features = json::object();
            for (const auto& feature : layer.at("features").items())
                features[feature.key()] = rounded(feature.value().at("time").get<double>(), 100.);
            item["features"] = std::move(features);
        }
        items.push_back(std::move(item));
        below = z;
    }
    return items;
}

json preview_objects(const json& statistics, const std::vector<std::string>& object_names)
{
    json out = json::array();
    for (const json& object : statistics.at("objects")) {
        const std::string name = object.at("name").get<std::string>();
        json              item{{"name", name},
                               {"instance", object.at("instance")},
                               {"g", rounded(object.at("weight").get<double>(), 100.)},
                               {"support_g", rounded(object.at("support_weight").get<double>(), 100.)}};
        const auto first = std::find(object_names.begin(), object_names.end(), name);
        if (first != object_names.end() && std::find(std::next(first), object_names.end(), name) == object_names.end())
            item["object"] = std::distance(object_names.begin(), first);
        out.push_back(std::move(item));
    }
    return out;
}

bool viewer_holds(const GCodeViewer& viewer, const GCodeProcessorResult* result)
{
    return result != nullptr && viewer.has_data() && viewer.last_result_id() == result->id;
}

json preview_state(GCodeViewer& viewer)
{
    IMSlider* layers = viewer.get_layers_slider();
    if (!viewer.has_data() || layers == nullptr)
        return nullptr;
    const bool silent = viewer.get_viewer().get_time_mode() == libvgcode::ETimeMode::Stealth;
    return json{{"view_type", GCodeViewer::view_type_to_config_name(viewer.get_view_type())},
                {"time_mode", silent ? "silent" : "normal"},
                {"layers", {layers->GetLowerValue() + 1, layers->GetHigherValue() + 1}},
                {"z", {rounded(layers->GetLowerValueD(), 1000.), rounded(layers->GetHigherValueD(), 1000.)}},
                {"layer_count", layers->GetMaxValue() + 1}};
}

namespace {

// A number for a message, as an agent writes it: 250, not 250.0.
std::string num(double v) { return v == std::floor(v) && std::abs(v) < 1e15 ? std::to_string((long long) v) : json(v).dump(); }

bool is_rrf(const DynamicPrintConfig& printer)
{
    const auto* flavor = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    return flavor != nullptr && flavor->value == GCodeFlavor::gcfRepRapFirmware;
}

bool is_belt(const DynamicPrintConfig& printer)
{
    const auto* belt = printer.option<ConfigOptionBool>("belt_printer");
    return belt != nullptr && belt->value;
}

// calibration_generate_test's test_model values, as each test's dialog lists its models.
std::vector<std::string> calib_test_models(CalibMode mode)
{
    switch (mode) {
    case CalibMode::Calib_Temp_Tower: return {"standard", "overhang"};
    case CalibMode::Calib_Input_shaping_freq:
    case CalibMode::Calib_Input_shaping_damp: return {"ringing_tower", "fast_tower"};
    case CalibMode::Calib_Cornering: return {"ringing_tower", "fast_tower", "scv_v2"};
    default: return {};
    }
}

// The rule of the test's dialog that `p` breaks, stated with the values given.
std::string calib_rule(CalibParamsError error, const Calib_Params& p, const DynamicPrintConfig& printer)
{
    const std::string range = "got start " + num(p.start) + ", end " + num(p.end) + ", step " + num(p.step);
    switch (error) {
    case CalibParamsError::BeltMethod: return "PA line and pattern tests are not enabled on belt printers; use method tower";
    case CalibParamsError::AccelBelowSpeed: return "every acceleration must be greater than every speed (they look swapped)";
    case CalibParamsError::Damping:
        if (p.mode == CalibMode::Calib_Input_shaping_freq)
            return "damping must be at least 0 and below 1; got " + num(p.start);
        return "input_shaping_damp needs 0 <= start < end <= 1; got start " + num(p.start) + ", end " + num(p.end);
    default: break;
    }
    switch (p.mode) {
    case CalibMode::Calib_Temp_Tower:
        return "temperature needs start at least end + 5 °C; got start " + num(p.start) + ", end " + num(p.end);
    case CalibMode::Calib_Vol_speed_Tower: return "max_volumetric needs start > 0, step > 0 and end >= start + step; " + range;
    case CalibMode::Calib_Retraction_tower: return "retraction needs start >= 0, step > 0 and end >= start + step; " + range;
    case CalibMode::Calib_VFA_Tower: return "vfa needs start > 10, step > 0 and end >= start + step; " + range;
    case CalibMode::Calib_Input_shaping_freq:
        return "input_shaping_freq needs 0 <= start < end <= 500 Hz" + std::string(is_rrf(printer) ? "" : ", and so do start_y and end_y") +
               "; got " + num(p.freqStartX) + "-" + num(p.freqEndX) + (is_rrf(printer) ? "" : ", Y " + num(p.freqStartY) + "-" + num(p.freqEndY));
    case CalibMode::Calib_Input_shaping_damp:
        return "input_shaping_damp needs freq_x and freq_y within 0-500 Hz; got " + num(p.freqStartX) + ", " + num(p.freqStartY);
    case CalibMode::Calib_Cornering: {
        const bool jd = has_junction_deviation(printer);
        return "cornering needs 0 <= start < end <= " + num(cornering_end_limits(printer).first) +
               (jd ? " mm (junction deviation)" : " mm/s (jerk)") + "; got start " + num(p.start) + ", end " + num(p.end);
    }
    default: return "pressure_advance needs start >= 0, step >= 0.001 and end >= start + step; " + range;
    }
}

// Where a test's measured value belongs, from the wiki's calibration pages.
json calib_result_keys(CalibMode mode, const DynamicPrintConfig& printer)
{
    switch (mode) {
    case CalibMode::Calib_Temp_Tower: return {{"type", "filament"}, {"keys", {"nozzle_temperature", "nozzle_temperature_initial_layer"}}};
    case CalibMode::Calib_Vol_speed_Tower: return {{"type", "filament"}, {"keys", {"filament_max_volumetric_speed"}}};
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_PA_Line: return {{"type", "filament"}, {"keys", {"enable_pressure_advance", "pressure_advance"}}};
    case CalibMode::Calib_PA_Pattern:
        return {{"type", "filament"},
                {"keys", {"enable_pressure_advance", "pressure_advance", "adaptive_pressure_advance", "adaptive_pressure_advance_model"}}};
    case CalibMode::Calib_Flow_Rate: return {{"type", "filament"}, {"keys", {"filament_flow_ratio"}}};
    case CalibMode::Calib_Retraction_tower: return {{"type", "filament"}, {"keys", {"filament_retraction_length"}}};
    case CalibMode::Calib_VFA_Tower:
        return {{"type", "printer"}, {"keys", {"resonance_avoidance", "min_resonance_avoidance_speed", "max_resonance_avoidance_speed"}}};
    case CalibMode::Calib_Cornering:
        return {{"type", "printer"},
                {"keys", has_junction_deviation(printer) ? json{"machine_max_junction_deviation"} : json{"machine_max_jerk_x", "machine_max_jerk_y"}}};
    default:
        return {{"type", "printer"},
                {"keys", {"input_shaping_emit", "input_shaping_type", "input_shaping_freq_x", "input_shaping_freq_y", "input_shaping_damp_x",
                          "input_shaping_damp_y"}}};
    }
}

// What VFA's auto-adjust does to `params`, as the dialog's answer Yes would: {layer_height, end, reason} when it
// changed them; a warning when the upper blocks are left above the filament's limit.
json apply_vfa_fit(Calib_Params& params, bool auto_adjust, const PresetBundle& bundle, std::vector<std::string>& warnings)
{
    const DynamicPrintConfig& printer = bundle.printers.get_edited_preset().config;
    const VfaSpeedFit         fit = fit_vfa_speed(params, printer, bundle.prints.get_edited_preset().config, bundle.filaments.get_edited_preset().config,
                                                  auto_adjust, vfa_candidate_layer_heights(bundle.prints, printer, params.extruder_id));
    const std::string over = "the end speed (" + num(params.end) + " mm/s) exceeds the filament's maximum volumetric speed (" +
                             num(fit.max_vol_speed) + " mm³/s)";
    switch (fit.kind) {
    case VfaSpeedFit::ReducedLayerHeight:
        params.vfa_layer_height = fit.layer_height;
        return {{"layer_height", fit.layer_height},
                {"reason", over + " at the default layer height (" + num(fit.default_lh) + " mm); the layer height of a profile of this printer reaches it"}};
    case VfaSpeedFit::LoweredEnd:
        params.vfa_layer_height = fit.layer_height;
        params.end              = fit.end;
        return {{"layer_height", fit.layer_height},
                {"end", fit.end},
                {"reason", over + " even at the smallest layer height of this printer's profiles; the end speed was lowered to what it reaches"}};
    case VfaSpeedFit::Clamped:
        warnings.push_back(over + ", which limits the outer wall to about " + std::to_string(std::lround(fit.speed_limit)) +
                           " mm/s: the blocks above print at that speed" +
                           (auto_adjust && params.nozzle_based_resize ? " (no compatible process profile to lower the layer height to)" :
                                                                        " (auto_adjust and auto_scale lower the layer height to reach it)"));
        return json();
    default: return json();
    }
}

} // namespace

Calib_Params calib_params_from_args(const json& args, const DynamicPrintConfig& printer, std::vector<std::string>& warnings)
{
    const std::string test = arg_string(args, "test");
    // The arguments each test takes, besides test, filament_slot, silent, discard_changes and preset_changes.
    std::vector<std::string> takes{"start", "end", "step"};
    CalibMode                mode = CalibMode::Calib_None;
    std::string              method;
    if (test == "temperature") {
        mode  = CalibMode::Calib_Temp_Tower;
        takes = {"start", "end", "auto_scale", "test_model"};
    } else if (test == "max_volumetric") {
        mode = CalibMode::Calib_Vol_speed_Tower;
    } else if (test == "pressure_advance") {
        method = arg_string(args, "method", "tower");
        mode   = method == "tower" ? CalibMode::Calib_PA_Tower : method == "line" ? CalibMode::Calib_PA_Line :
                 method == "pattern" ? CalibMode::Calib_PA_Pattern : CalibMode::Calib_None;
        if (mode == CalibMode::Calib_None)
            throw ToolFailure("method of pressure_advance is tower, line or pattern");
        takes.push_back("method");
        if (mode == CalibMode::Calib_PA_Line)
            takes.push_back("print_numbers");
        if (mode == CalibMode::Calib_PA_Pattern)
            takes.insert(takes.end(), {"accelerations", "speeds"});
    } else if (test == "flow_ratio") {
        method = arg_string(args, "method", "yolo");
        if (method != "pass1" && method != "pass2" && method != "yolo" && method != "yolo_perfectionist")
            throw ToolFailure("method of flow_ratio is pass1, pass2, yolo or yolo_perfectionist");
        mode  = CalibMode::Calib_Flow_Rate;
        takes = {"method", "top_surface_pattern"};
    } else if (test == "retraction") {
        mode = CalibMode::Calib_Retraction_tower;
    } else if (test == "vfa") {
        mode = CalibMode::Calib_VFA_Tower;
        takes.insert(takes.end(), {"auto_scale", "auto_adjust"});
    } else if (test == "input_shaping_freq") {
        mode  = CalibMode::Calib_Input_shaping_freq;
        takes = {"start", "end", "start_y", "end_y", "damping", "shaper_type", "test_model"};
    } else if (test == "input_shaping_damp") {
        mode  = CalibMode::Calib_Input_shaping_damp;
        takes = {"freq_x", "freq_y", "start", "end", "shaper_type", "test_model"};
    } else if (test == "cornering") {
        mode  = CalibMode::Calib_Cornering;
        takes = {"start", "end", "test_model"};
    } else {
        throw ToolFailure("Unknown test \"" + test + "\"");
    }
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "test" && it.key() != "filament_slot" && it.key() != "silent" && it.key() != "discard_changes" &&
            it.key() != "preset_changes" && std::find(takes.begin(), takes.end(), it.key()) == takes.end())
            throw ToolFailure(it.key() + " does not apply to " + test + (method.empty() ? "" : " with method " + method), {{"takes", takes}});

    // The dialog's radio picks the Bowden defaults; here the printer's extruder type does.
    const auto*  extruder_type = printer.option<ConfigOptionEnumsGeneric>("extruder_type");
    const bool   bowden        = extruder_type != nullptr && !extruder_type->values.empty() && extruder_type->values.front() == int(etBowden);
    const bool   rrf           = is_rrf(printer);
    Calib_Params p             = calib_default_params(mode, printer, bowden);
    if (mode == CalibMode::Calib_Input_shaping_freq) {
        if (rrf && (args.contains("start_y") || args.contains("end_y")))
            throw ToolFailure("RepRapFirmware takes one frequency range for both axes; leave start_y and end_y out");
        p.freqStartX = arg_double(args, "start", p.freqStartX);
        p.freqEndX   = arg_double(args, "end", p.freqEndX);
        p.freqStartY = arg_double(args, "start_y", p.freqStartX);
        p.freqEndY   = arg_double(args, "end_y", p.freqEndX);
        p.start      = arg_double(args, "damping", p.start);
    } else if (mode == CalibMode::Calib_Input_shaping_damp) {
        if (rrf && args.contains("freq_y"))
            throw ToolFailure("RepRapFirmware takes one frequency for both axes; leave freq_y out");
        p.freqStartX = arg_double(args, "freq_x", p.freqStartX);
        p.freqStartY = arg_double(args, "freq_y", p.freqStartX);
        p.start      = arg_double(args, "start", p.start);
        p.end        = arg_double(args, "end", p.end);
    } else {
        p.start = arg_double(args, "start", p.start);
        p.end   = arg_double(args, "end", p.end);
        p.step  = arg_double(args, "step", p.step);
    }

    if (mode == CalibMode::Calib_Temp_Tower) {
        // As the dialog's fields do when they lose focus: clamp, then round down to the tower's 5 °C blocks.
        for (auto [name, t] : {std::make_pair("start", &p.start), std::make_pair("end", &p.end)}) {
            const double given = *t;
            if (const double clamped = clamp_calib_temp(*t); clamped != *t) {
                *t = clamped;
                warnings.push_back(std::string(name) + " " + num(given) + " °C is outside the supported range " + num(kCalibTempMin) +
                                   "-" + num(kCalibTempMax) + " °C; clamped to " + num(*t) + " °C");
            }
            if (const double rounded = round_calib_temp(*t); rounded != *t) {
                warnings.push_back(std::string(name) + " " + num(*t) + " °C rounded down to " + num(rounded) + " °C, the tower's 5 °C blocks");
                *t = rounded;
            }
        }
    }
    p.nozzle_based_resize = arg_bool(args, "auto_scale", true);
    if (mode == CalibMode::Calib_PA_Line)
        p.print_numbers = arg_bool(args, "print_numbers", p.print_numbers);
    if (mode == CalibMode::Calib_PA_Pattern) {
        for (const json& a : args.value("accelerations", json::array()))
            p.accelerations.push_back(a.get<double>());
        for (const json& v : args.value("speeds", json::array()))
            p.speeds.push_back(v.get<double>());
    }

    if (const std::vector<std::string> models = calib_test_models(mode); !models.empty()) {
        const std::string model = arg_string(args, "test_model", models.front());
        const auto        it    = std::find(models.begin(), models.end(), model);
        if (it == models.end())
            throw ToolFailure("test_model of " + test + " is " + boost::algorithm::join(models, ", "));
        p.test_model = int(it - models.begin());
        // The temperature dialog shows its model picker on belt printers only.
        if (mode == CalibMode::Calib_Temp_Tower && args.contains("test_model") && !is_belt(printer))
            throw ToolFailure("test_model applies to the temperature test on belt printers only");
        if (mode == CalibMode::Calib_Temp_Tower && p.test_model == 1 && !boost::filesystem::exists(belt_temp_tower_asset(p)))
            throw ToolFailure("No belt temperature tower model covers " + num(p.start) + " to " + num(p.end) +
                              " °C; use a range the tower models cover, for example 230 to 190");
    }
    if (mode == CalibMode::Calib_Input_shaping_freq || mode == CalibMode::Calib_Input_shaping_damp) {
        const std::vector<std::string> types = shaper_type_values(printer);
        p.shaper_type                        = arg_string(args, "shaper_type", types.front());
        if (std::find(types.begin(), types.end(), p.shaper_type) == types.end())
            throw ToolFailure("shaper_type " + p.shaper_type + " is not offered for " +
                              (printer.has("gcode_flavor") ? printer.opt_serialize("gcode_flavor") : std::string("this")) +
                              " firmware: " + boost::algorithm::join(types, ", "));
    }

    if (const CalibParamsError error = check_calib_params(p, printer); error != CalibParamsError::None)
        throw ToolFailure(calib_rule(error, p, printer));
    if (mode == CalibMode::Calib_Cornering && p.end > cornering_end_limits(printer).second)
        warnings.push_back("end " + num(p.end) + " is above " + num(cornering_end_limits(printer).second) + ": high values may cause layer shifts");
    return p;
}

json calib_params_json(const json& args, const Calib_Params& p, const DynamicPrintConfig& printer)
{
    const bool rrf = is_rrf(printer);
    json       out;
    switch (p.mode) {
    case CalibMode::Calib_Temp_Tower:
        out = {{"start", p.start}, {"end", p.end}, {"auto_scale", p.nozzle_based_resize}};
        if (is_belt(printer))
            out["test_model"] = calib_test_models(p.mode)[size_t(p.test_model)];
        break;
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
        out = {{"method", arg_string(args, "method", "tower")}, {"start", p.start}, {"end", p.end}, {"step", p.step}};
        if (p.mode == CalibMode::Calib_PA_Line)
            out["print_numbers"] = p.print_numbers;
        // Left out, the pattern's are taken from the process when it runs.
        if (!p.accelerations.empty())
            out["accelerations"] = p.accelerations;
        if (!p.speeds.empty())
            out["speeds"] = p.speeds;
        break;
    case CalibMode::Calib_Flow_Rate:
        out = {{"method", arg_string(args, "method", "yolo")}, {"top_surface_pattern", arg_string(args, "top_surface_pattern", "archimedeanchords")}};
        break;
    case CalibMode::Calib_VFA_Tower:
        out = {{"start", p.start}, {"end", p.end}, {"step", p.step}, {"auto_scale", p.nozzle_based_resize}, {"auto_adjust", arg_bool(args, "auto_adjust", true)}};
        break;
    case CalibMode::Calib_Input_shaping_freq:
        out = {{"start", p.freqStartX}, {"end", p.freqEndX}};
        if (!rrf)
            out.update({{"start_y", p.freqStartY}, {"end_y", p.freqEndY}});
        out.update({{"damping", p.start}, {"shaper_type", p.shaper_type}, {"test_model", calib_test_models(p.mode)[size_t(p.test_model)]}});
        break;
    case CalibMode::Calib_Input_shaping_damp:
        out = {{"freq_x", p.freqStartX}};
        if (!rrf)
            out["freq_y"] = p.freqStartY;
        out.update({{"start", p.start}, {"end", p.end}, {"shaper_type", p.shaper_type}, {"test_model", calib_test_models(p.mode)[size_t(p.test_model)]}});
        break;
    case CalibMode::Calib_Cornering:
        out = {{"start", p.start}, {"end", p.end}, {"test_model", calib_test_models(p.mode)[size_t(p.test_model)]}};
        break;
    default: out = {{"start", p.start}, {"end", p.end}, {"step", p.step}}; break;
    }
    return out;
}

void register_plate_tools(CommandCatalog& catalog)
{
    NativeCommand calibration = mutate_tool(
        "calibration_generate_test", L("Generate a calibration test"),
        "Generate a Calibration menu test as its dialog's OK does: a new project named after the test, with the test model "
        "and the process, filament and printer presets changed as the test needs. It prints with `filament_slot` on the "
        "extruder the filament map gives it, so the filament tab points at that slot's preset. Omitted parameters take the "
        "dialog's defaults (pressure advance's by the printer's extruder type), but `filament_slot` defaults to 1, not the "
        "filament tab's; temperatures are clamped to 155-500 °C and rounded down to 5 °C. Fails, changing nothing, while the "
        "project has unsaved changes (pass `discard_changes`) or the presets do (pass `preset_changes`). With test alone it "
        "opens that test's dialog (`blocked_by_dialog`), whose OK runs it, unless `silent: true`, which runs it with the "
        "dialog's defaults. Reply: test, project, params (the values used, as the journal records them), objects, names, "
        "plates, `presets_changed` (its filament names the `filament_slot`, and with several extruders the 1-based extruder "
        "where the filament map decides it), `result_keys` (where the measured value belongs), warnings, adjusted (vfa), and "
        "discarded and modified as `project_new`. Then `plate_slice` and `project_export`. To save a measured value: "
        "`project_new {discard_changes, preset_changes: \"discard\"}`, `settings_set` the value, `preset_save`.",
        object_schema(
            {{"test", enum_prop({"temperature", "max_volumetric", "pressure_advance", "flow_ratio", "retraction", "vfa", "cornering",
                                 "input_shaping_freq", "input_shaping_damp"},
                                "The test, as the Calibration menu names it.")},
             {"filament_slot", {{"type", "integer"},
                                {"minimum", 1},
                                {"description", "The physical filament (1-based slot) to print with, as the dialog's filament "
                                                "list offers them; not a mixed one. Default 1."}}},
             {"method", enum_prop({"tower", "line", "pattern", "pass1", "pass2", "yolo", "yolo_perfectionist"},
                                  "`pressure_advance`: tower (default), line or pattern; `flow_ratio`: pass1, pass2, yolo (default) or "
                                  "`yolo_perfectionist`.")},
             {"start", prop("number", "Start of the swept value: °C, mm³/s, PA, mm, mm/s (vfa), Hz (`input_shaping_freq`), "
                                      "damping (`input_shaping_damp`), junction deviation mm or jerk mm/s (cornering).")},
             {"end", prop("number", "End of the swept value.")},
             {"step", prop("number", "Step: `max_volumetric`, `pressure_advance`, retraction, vfa.")},
             {"start_y", prop("number", "`input_shaping_freq`: start of the Y range; default the X one.")},
             {"end_y", prop("number", "`input_shaping_freq`: end of the Y range; default the X one.")},
             {"damping", prop("number", "`input_shaping_freq`: the damping ratio printed with.")},
             {"freq_x", prop("number", "`input_shaping_damp`: the X frequency, Hz.")},
             {"freq_y", prop("number", "`input_shaping_damp`: the Y frequency, Hz; default `freq_x`.")},
             {"shaper_type", prop("string", "Input shaping: an `input_shaping_type` the printer's firmware offers; default the first.")},
             {"test_model", enum_prop({"ringing_tower", "fast_tower", "scv_v2", "standard", "overhang"},
                                      "Input shaping: `ringing_tower` (default) or `fast_tower`; cornering: also `scv_v2`; temperature on "
                                      "belt printers: standard (default) or overhang.")},
             {"print_numbers", prop("boolean", "`pressure_advance` line: print the values (default true).")},
             {"accelerations", array_prop({{"type", "integer"}, {"minimum", 1}},
                                          "`pressure_advance` pattern: one test per acceleration and speed; default the process's.")},
             {"speeds", array_prop({{"type", "integer"}, {"minimum", 1}}, "`pressure_advance` pattern: default an optimal speed.")},
             {"top_surface_pattern", enum_prop({"archimedeanchords", "monotonic"}, "`flow_ratio` (default archimedeanchords).")},
             {"auto_scale", prop("boolean", "temperature, vfa: scale the model to the nozzle (default true).")},
             {"auto_adjust", prop("boolean", "vfa: lower the layer height, else the end speed, to stay within the filament's "
                                             "maximum volumetric speed (default true); only with `auto_scale`.")},
             {"discard_changes", prop("boolean", "Drop the project's unsaved changes rather than fail; for the presets', "
                                                 "see `preset_changes`.")},
             {"preset_changes", enum_prop({"keep", "discard"}, "The presets' unsaved changes: keep them for the test or drop them "
                                                        "first. Required when there are any.")}},
            {"test"}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            json       recorded = args;
            const bool discard  = arg_bool(args, "discard_changes", false) || runtime.jobs().discard_allowed_in(ctx.lineage());
            json       out      = ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (plater.printer_technology() != ptFFF)
                    throw ToolFailure("Calibration tests are for filament (FFF) printers");
                PresetBundle& bundle          = *wxGetApp().preset_bundle;
                const bool    discard_presets = arg_string(args, "preset_changes") == "discard";
                // Discarded edits are gone before the test runs, so its rules and defaults are the saved printer's.
                const DynamicPrintConfig& printer = discard_presets ? bundle.printers.get_selected_preset().config :
                                                                      bundle.printers.get_edited_preset().config;
                std::vector<std::string> warnings;
                Calib_Params             params = calib_params_from_args(args, printer, warnings);
                // The dialog's filament list offers the physical filaments; a mixed slot is none of them.
                const size_t physical = std::min(bundle.num_physical_filaments(), bundle.filament_presets.size());
                const int    slot     = arg_int(args, "filament_slot", 1);
                if (slot < 1 || size_t(slot) > physical)
                    throw ToolFailure(slot >= 1 && size_t(slot) <= bundle.filament_presets.size() ?
                                          "Filament slot " + std::to_string(slot) + " is a mixed filament; a test prints with a " +
                                              "physical one, slot 1.." + std::to_string(physical) :
                                          "filament_slot must be 1.." + std::to_string(physical) + ", the physical filaments");
                const int extruder = calib_filament_extruder(size_t(slot - 1));
                params.filament_id = slot - 1;
                params.extruder_id = std::max(extruder, 0);
                require_project_discardable(discard);
                // The GUI asks whether to keep or discard modified presets here; the caller answers with preset_changes.
                const json before = unsaved_changes();
                if (!before.empty() && !args.contains("preset_changes")) {
                    const json  modified = unsaved_changes_outcome(json::object()).value("modified", json::object());
                    std::string list;
                    for (auto it = modified.begin(); it != modified.end(); ++it)
                        list += std::string(list.empty() ? "" : "; ") + it.key() + " \"" + it.value()["name"].get<std::string>() +
                                "\": " + boost::algorithm::join(it.value()["keys"].get<std::vector<std::string>>(), ", ");
                    throw ToolFailure("Presets are modified (" + list + "). Pass preset_changes: \"discard\" to drop them first, or \"keep\" "
                                      "to run the test with them",
                                      {{"modified", modified}});
                }
                // Switching the filament tab to the test's filament must not ask about another preset's changes.
                const std::string& slot_preset = bundle.filament_presets[size_t(slot - 1)];
                if (!discard_presets && bundle.filaments.get_edited_preset().name != slot_preset && bundle.filaments.current_is_dirty())
                    throw ToolFailure("The filament tab edits \"" + bundle.filaments.get_edited_preset().name +
                                      "\", which has unsaved changes, and the test prints with filament slot " + std::to_string(slot) +
                                      " (\"" + slot_preset + "\"); pass preset_changes: \"discard\", or save or drop those changes first");
                if (discard_presets && wxGetApp().has_current_preset_changes())
                    wxGetApp().discard_current_preset_changes();
                const std::string switched_from = edit_filament_slot(slot);

                // The journal keeps VFA's end as asked, since the fit recomputes it from the presets.
                recorded.update(calib_params_json(args, params, printer));
                const json adjusted = params.mode == CalibMode::Calib_VFA_Tower ?
                                          apply_vfa_fit(params, arg_bool(args, "auto_adjust", true), bundle, warnings) :
                                          json();
                const DynamicPrintConfig process_before = bundle.prints.get_edited_preset().config;
                const DynamicPrintConfig filament_before = bundle.filaments.get_edited_preset().config;
                const DynamicPrintConfig printer_before = bundle.printers.get_edited_preset().config;
                switch (params.mode) {
                case CalibMode::Calib_Temp_Tower: plater.calib_temp(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_Vol_speed_Tower: plater.calib_max_vol_speed(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_PA_Tower:
                case CalibMode::Calib_PA_Line:
                case CalibMode::Calib_PA_Pattern: plater.calib_pa(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_Flow_Rate: {
                    const std::string method = arg_string(args, "method", "yolo");
                    plater.calib_flowrate(method == "yolo" || method == "yolo_perfectionist",
                                          method == "pass2" || method == "yolo_perfectionist" ? 2 : 1,
                                          arg_string(args, "top_surface_pattern") == "monotonic" ? ipMonotonic : ipArchimedeanChords,
                                          params.filament_id, params.extruder_id, Ops::Prompts::Skip);
                    break;
                }
                case CalibMode::Calib_Retraction_tower: plater.calib_retraction(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_VFA_Tower: plater.calib_VFA(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_Input_shaping_freq: plater.calib_input_shaping_freq(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_Input_shaping_damp: plater.calib_input_shaping_damp(params, Ops::Prompts::Skip); break;
                case CalibMode::Calib_Cornering: plater.Calib_Cornering(params, Ops::Prompts::Skip); break;
                default: break;
                }

                json presets_changed = json::object();
                const auto changed_preset = [&presets_changed](const char* type, const PresetCollection& presets, const DynamicPrintConfig& config_before) {
                    if (json changes = changed_settings(config_before, presets.get_edited_preset().config); !changes.empty())
                        presets_changed[type] = {{"name", presets.get_edited_preset().name}, {"changes", std::move(changes)}};
                };
                changed_preset("process", bundle.prints, process_before);
                changed_preset("printer", bundle.printers, printer_before);
                presets_changed["filament"] = {{"name", bundle.filaments.get_edited_preset().name},
                                               {"filament_slot", slot},
                                               {"changes", changed_settings(filament_before, bundle.filaments.get_edited_preset().config)}};
                // The extruder, as the dialog's filament list names it: only with several, once the filament map decides it.
                if (const auto* nozzles = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
                    nozzles != nullptr && nozzles->size() > 1 && extruder >= 0)
                    presets_changed["filament"]["extruder"] = extruder + 1;
                if (!switched_from.empty())
                    presets_changed["filament"]["tab_switched_from"] = switched_from;
                if (plater.model().objects.empty())
                    throw ToolFailure("The project was replaced but the test model was not added; see app_log",
                                      {{"presets_changed", presets_changed}});

                json reply = calib_params_json(args, params, printer);
                // The pattern's defaults are the process's acceleration and an optimal speed, which the test writes there.
                if (params.mode == CalibMode::Calib_PA_Pattern)
                    for (const auto& [key, option] : {std::make_pair("accelerations", "outer_wall_acceleration"), std::make_pair("speeds", "outer_wall_speed")})
                        if (!reply.contains(key))
                            if (const auto* opt = bundle.prints.get_edited_preset().config.option<ConfigOptionFloatsNullable>(option))
                                reply[key] = json::array({opt->get_at(0)});
                json o = {{"test", args["test"]}, {"project", into_u8(plater.get_project_name())}, {"params", std::move(reply)}};
                o.update(objects_summary(plater));
                o["presets_changed"] = std::move(presets_changed);
                o["result_keys"]     = calib_result_keys(params.mode, bundle.printers.get_edited_preset().config);
                if (!warnings.empty())
                    o["warnings"] = warnings;
                if (!adjusted.is_null())
                    o["adjusted"] = adjusted;
                o.update(unsaved_changes_outcome(before));
                return o;
            });
            ctx.set_journal_args(std::move(recorded));
            settle(ctx);
            return ToolResult::ok(out);
        });
    // The Calibration menu's items, each the dialog of its test. The tab strip's calib_sf glyph is drawn white for the dark
    // tab bar and vanishes on the Speed Dial's light tile, so each item borrows the matching settings-group icon instead.
    struct CalibrationMenuItem
    {
        const char* key;
        const char* title;
        const char* icon;
        const char* test;
        CalibKind   kind;
    };
    // Titles are L("...")-marked here so xgettext extracts them.
    static const CalibrationMenuItem calibration_menu_items[] = {
        {"calib_temperature", L("Temperature Calibration"), "param_temperature", "temperature", CalibKind::Temperature},
        {"calib_max_volumetric", L("Max Volumetric Speed Calibration"), "param_volumetric_speed", "max_volumetric",
         CalibKind::MaxVolumetric},
        {"calib_pressure_advance", L("Pressure Advance Calibration"), "param_flow_ratio_and_pressure_advance", "pressure_advance",
         CalibKind::PressureAdvance},
        {"calib_flow_ratio", L("Flow Ratio Calibration"), "param_flow_ratio_and_pressure_advance", "flow_ratio", CalibKind::FlowRatio},
        {"calib_retraction", L("Retraction Calibration"), "param_retraction", "retraction", CalibKind::Retraction},
        {"calib_cornering", L("Cornering Calibration"), "param_precision", "cornering", CalibKind::Cornering},
        {"calib_input_shaping_freq", L("Input Shaping Frequency Calibration"), "param_resonance_avoidance", "input_shaping_freq",
         CalibKind::InputShapingFreq},
        {"calib_input_shaping_damp", L("Input Shaping Damping Calibration"), "param_resonance_avoidance", "input_shaping_damp",
         CalibKind::InputShapingDamp},
        {"calib_vfa", L("VFA Calibration"), "param_speed", "vfa", CalibKind::VFA},
    };
    calibration.actions = json::array();
    for (const CalibrationMenuItem& item : calibration_menu_items)
        calibration.actions.push_back(NativeCommands::action(item.key, item.title, L("Calibration"), item.icon, {{"test", item.test}},
                                                             std::string("Open the ") + item.title +
                                                                 " dialog; calibration_generate_test with the test's parameters, or "
                                                                 "silent, runs it with no dialog."));
    catalog.add(NativeCommands::with_person_form(std::move(calibration), [](const json& action_args) {
        const std::string test = action_args.at("test");
        for (const CalibrationMenuItem& item : calibration_menu_items)
            if (test == item.test)
                return MenuItems::calib(item.kind);
        return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("Unknown test \"" + test + "\""), false};
    }));

    const json plate_prop = {{"type", "integer"}, {"minimum", 0}, {"description", "Plate index; default: the current plate."}};

    catalog.add(inspect_tool(
        "plate_get_settings", L("Read a plate's settings"),
        "Read a plate's own settings, as the Plate Settings dialog sets them: `curr_bed_type`, `print_sequence`, `spiral_mode`, "
        "`first_layer_print_sequence` and `other_layers_print_sequence`; and as its filament grouping sets them on a printer "
        "with several extruders: `filament_map_mode` and `filament_map` (the extruder of each filament, 1-based). Each with "
        "value (the plate's own, null where it follows the global setting or Auto), effective (what the plate prints with) "
        "and meta (meta.offered lists the plate types or grouping modes this printer has). All of them, or those named in "
        "keys. Reply: plate, items; `unknown_keys` for the keys a plate does not have, and similar the plate keys like them.",
        object_schema({{"plate", plate_prop},
                       {"keys", array_prop({{"type", "string"}}, "Setting keys, e.g. [\"curr_bed_type\"].")},
                       {"include_meta", prop("boolean", "Include labels, types and ranges. Default true.")}}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                ui_plater();
                return plate_settings(args);
            }));
        }));

    catalog.add(mutate_tool(
        "plate_set_settings", L("Change a plate's settings"),
        "Change a plate's own settings, which it selects and writes as the Plate Settings dialog's OK does: `curr_bed_type`, "
        "`print_sequence` (\"by layer\", \"by object\"), `spiral_mode` (true asks for confirmation), `first_layer_print_sequence` "
        "(every slot once, e.g. `[2, 1]`) and `other_layers_print_sequence` ([`{from: layer >= 2, to, order}`], to omitted for "
        "the last layer); and as the OK of its filament grouping does, with several extruders: `filament_map_mode` (an "
        "offered mode) and, with \"Manual\", `filament_map` (one extruder per filament, e.g. `[1, 2, 2]`; on a printer whose "
        "extruder holds several nozzles, refused as the dialog refuses it where an extruder it uses has no nozzle of the "
        "volume type its filaments print with). null returns a value to the global setting or Auto. Values are checked "
        "before anything changes, and the call is one undo step. Reply: plate, applied, changed, and effective for the "
        "settings the plate prints with.",
        object_schema({{"plate", plate_prop},
                       {"values", {{"type", "object"}, {"description", "{key: value}"}}}},
                      {"values"}),
        [](const json& args, ToolContext& ctx) { return set_plate_settings(args, ctx); }));

    catalog.add(inspect_tool(
        "plate_list", L("List plates"),
        "Plates with name, locked, current, objects (how many are on it; `object_list` lists them), and the settings they print with: `curr_bed_type`, "
        "`print_sequence`, `spiral_mode`, `first_layer_print_sequence` and `other_layers_print_sequence` (null: Auto), with several "
        "extruders `filament_map_mode` and `filament_map`; overrides "
        "lists those the plate sets itself (`plate_set_settings`). Also origin and size, the plate's place on the bed in "
        "mm: the `[x, y]` of its front left corner and its `[width, depth]`, in the bed coordinates object positions are "
        "given in, so that origin plus half the size is its middle; sliced (an up-to-date slice), `wipe_tower` `[x, y]` "
        "and `layer_gcode`: the plate's pauses, custom G-code, templates and filament changes, each `{z, layer, type, "
        "filament, color, gcode}`; layer is the Preview slider's number, given while the plate's slice is up to date.",
        object_schema({{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Default 100."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return list_plates(args); })); }));

    // The plate commands made from menu items: each acts on the plate the call names, else on the current plate, as its item
    // does, and its reply names the plate.

    const std::string plates_after = "plates (each plate afterwards: index, name and objects, how many stand on it) and current "
                                     "(the current plate)";
    catalog.add(own_action(menu_command("plate_add", L("Add Plate"),
                                        "Add a plate after the others and make it current, as Add Plate does. One undo step. "
                                        "Reply: plate (the new plate's index), " +
                                            plates_after + ".",
                                        object_schema({}), json(),
                                        [](const json&) {
                                            AppActionRunResult added = MenuItems::plate_add();
                                            // Add Plate makes the plate it adds current.
                                            if (added.level == AppActionRunResult::Level::Succeeded) {
                                                json reply     = plates_reply();
                                                reply["plate"] = ui_plater().get_partplate_list().get_curr_plate_index();
                                                added.reply    = reply.dump();
                                            }
                                            return added;
                                        }),
                           L("Plate"), "toolbar_add_plate"));
    catalog.add(reaching({"delete_plate"},
                         own_action(menu_command("plate_delete", L("Delete Plate"),
                                                "Delete a plate (plate, else the current one), as Delete Plate does. The objects on it "
                                                "stay in the project, outside the plates, and the plates after it move down one. The "
                                                "only plate is refused. One undo step. Reply: plate (the index it had), " +
                                                    plates_after + ".",
                                                object_schema({{"plate", plate_prop}}), json(), plate_delete_command),
                                    L("Plate"), "delete")));
    catalog.add(reaching({"duplicate_plate"},
                         own_action(menu_command("plate_duplicate", L("Duplicate Plate"),
                                                "Add a plate with a copy of each object on the plate (plate, else the current one), "
                                                "placed on it as on that plate, as Duplicate Current Plate does. The new "
                                                "plate comes last, and the current plate stays current. Refused when the project "
                                                "holds as many plates as it can, and while slicing. One undo step. Reply: plate (the "
                                                "plate copied), `new_plate` (the new plate's index), " +
                                                    plates_after + ".",
                                                object_schema({{"plate", plate_prop}}), json(), plate_duplicate_command),
                                    L("Plate"), "menu_copy")));
    // The name dialog's name, which every call but the action's gives.
    NativeCommand rename = own_action(menu_command("plate_rename", L("Rename Plate"),
                                                  "Name a plate (plate, else the current one), as its name icon does: a plate other "
                                                  "than the current one is made current first. name \"\" leaves it unnamed. With no "
                                                  "arguments, as its Speed Dial action, the name dialog asks for the current plate's "
                                                  "name (`blocked_by_dialog`), and a cancelled dialog renames nothing, which the call "
                                                  "is told. The name is one undo step; the name the plate has already changes "
                                                  "nothing, the current plate included. Reply: plate and name.",
                                                  object_schema({{"plate", plate_prop},
                                                                 {"name", prop("string", "The plate's name, in place of "
                                                                                         "the name dialog.")}},
                                                                {"name"}),
                                                  json(), plate_rename_command),
                                      L("Plate"), "plate_name_edit");
    rename.input_schema = with_silent(std::move(rename.input_schema), person_forms(rename));
    catalog.add(reaching({"rename_plate", "select_plate"}, std::move(rename)));
    catalog.add(reaching(
        {"set_plate_locked"},
        menu_command("plate_lock", L("Toggle Plate Lock"),
                    "Lock or unlock a plate (plate, else the current one), as its lock icon does: locked true locks it and false "
                    "unlocks it; without locked it toggles, as its Speed Dial action does. An arrange of every plate leaves a "
                    "locked one as it is. One undo step, none when the plate is in that state already. Reply: plate, locked, and "
                    "changed (false when it was so already).",
                    object_schema({{"plate", plate_prop},
                                   {"locked", prop("boolean", "true locks the plate, false unlocks it. Default: the other state.")}}),
                    json::array({NativeCommands::action("plate_toggle_lock", L("Toggle Plate Lock"), L("Plate"), "lock_normal",
                                                        json::object(), "Lock or unlock the current plate.")}),
                    plate_lock_command)));
    catalog.add(reaching(
        {"select_plate"},
        menu_command("plate_select", L("Go to Plate"),
                    "Make a plate the current one, as a click on it does: plate; without it, the current plate stays current. "
                    "An index the project lacks is an error. Reply: plate.",
                    object_schema({{"plate", plate_prop}}),
                    json::array({NativeCommands::action("plate_goto", L("Go to Plate"), L("Plate"), "go_next_plate", {{"plate", 0}},
                                                        "Make the first plate the current one.")}),
                    plate_select_command)));

    NativeCommand slice = reaching({"start_slice"}, mutate_tool(
        "plate_slice", L("Slice"),
        "Slice the current plate, all plates, or one plate by index, the way the toolbar's Slice button does, and wait for "
        "the result (or reply with a job id when it takes longer than the reply budget). Each requested plate reports ok, "
        "error, cancelled, empty or skipped; the whole operation is superseded when someone else started a slice or "
        "edited the project while it ran. An ok plate has `time_s` (`silent_time_s` when the printer estimates it), "
        "`filament_m` and `filament_g` including flush, `filament_cost`, layers, `max_z`, `filament_changes`, and filaments[i] for "
        "slot i+1 with m, g and the grams of `model_g`, `support_g`, `flush_g` and `tower_g`. Other changes are refused until it "
        "finishes; `job_cancel` stops it. The Preview tab shows the result, as after the Slice button; `preview: true`, alone "
        "or with plates \"all\", is the call of Slice and Preview or Slice All Plates, whose pick starts the slice as a job "
        "and shows its result as a notification.",
        object_schema({{"plates", {{"type", {"string", "integer"}}, {"description", "\"current\" (default), \"all\", or a plate index."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 7200}, {"description", "Stop waiting after this long (default 1800): the call fails, and the slice goes on unless `job_cancel` stops it."}}},
                       {"preview", prop("boolean", "Show the Preview tab, as every slice does: the argument of the Slice and "
                                                   "Preview and Slice All Plates actions.")}}),
        [](const json& args, ToolContext& ctx) {
            const json plates_arg = args.value("plates", json("current"));
            if (plates_arg.is_string() && plates_arg != "all" && plates_arg != "current")
                throw ToolFailure("plates is \"all\", \"current\" or a plate index");
            auto       ticket     = std::make_shared<SliceTicket>();
            // Held from before the wait below, so no call admitted meanwhile can change the project
            // under the slice, and a cancel meanwhile finds the operation.
            const uint64_t cancels_before = ctx.run_ui([&] {
                ctx.begin_operation();
                return json(ui_plater().slice_cancel_seq());
            }).get<uint64_t>();
            // A change made just before (settings_set, project_open) is validated by an update
            // scheduled a moment later; a slice requested first would be refused on the old state.
            poll_ui(
                ctx,
                [cancels_before]() -> json {
                    const Plater& plater = ui_plater();
                    const bool    pending = plater.is_background_process_update_scheduled();
                    return pending && plater.slice_cancel_seq() == cancels_before ? json() : json(true);
                },
                std::chrono::seconds(10), "the pending update of the slicing state");
            const bool cancelled = ctx.run_ui([&] {
                Plater&        plater = ui_plater();
                Ops::SliceArgs slice;
                slice.all_plates = plates_arg.is_string() && plates_arg.get<std::string>() == "all";
                if (plates_arg.is_number_integer())
                    slice.plate = plates_arg.get<int>();
                const Ops::Outcome<std::vector<int>> plates = Ops::slice_plates(slice);
                if (!plates)
                    throw refused(plates.refusal());
                ticket->plates = plates.value();
                // A cancel that came while the update was pending stops the slice before it starts.
                if (plater.slice_cancel_seq() != cancels_before)
                    return json(true);
                ticket->state.started = std::chrono::steady_clock::now();
                const Ops::Outcome<Ops::SliceTicket> started = Ops::start_slice(slice);
                if (!started)
                    throw refused(started.refusal());
                ticket->state.run_seq_before    = started.value().run_seq_before;
                ticket->state.revision_at_start = started.value().revision;
                ticket->state.request_seq_mine  = started.value().request_seq;
                ticket->state.seen_running      = started.value().slicing;
                ticket->state.adopted_run       = started.value().adopted_run;
                return json(false);
            }).get<bool>();
            if (cancelled) {
                json plates = json::array();
                for (int index : ticket->plates)
                    plates.push_back({{"index", index}, {"outcome", to_string(PlateOutcome::Cancelled)}});
                return ToolResult::ok({{"status", "done"}, {"plates", std::move(plates)}});
            }
            const auto timeout = std::chrono::seconds(arg_int(args, "timeout_s", 1800));
            json result = poll_ui(
                ctx,
                [ticket]() -> json {
                    Plater&          plater = ui_plater();
                    SliceObservation obs;
                    obs.slicing_any      = plater.is_slicing_any();
                    obs.request_seq      = plater.slice_request_seq();
                    obs.interrupted      = plater.slice_request_interrupted(ticket->state.request_seq_mine);
                    obs.current_revision = plater.project_revision();
                    const uint64_t first = ticket->state.adopted_run != 0 ? ticket->state.adopted_run : ticket->state.run_seq_before + 1;
                    for (uint64_t seq = first; seq <= plater.slice_run_seq(); ++seq)
                        if (auto run = plater.slice_run(seq))
                            obs.runs_since_start.push_back({run->seq, run->finished, run->cancelled, run->ok, run->error,
                                                            run->revision_at_finish, run->request_seq_at_finish,
                                                            run->request_seq_at_start, run->for_request, run->plate});
                    ticket->state.seen_running = ticket->state.seen_running || obs.slicing_any || !obs.runs_since_start.empty();
                    const TicketStatus status  = slice_status(ticket->state, obs, std::chrono::steady_clock::now());
                    if (status == TicketStatus::Pending)
                        return json();
                    json out{{"status", status == TicketStatus::Done ? "done" : "superseded"}};
                    PartPlateList&                list = plater.get_partplate_list();
                    std::vector<PlateObservation> observed;
                    for (int index : ticket->plates)
                        if (PartPlate* plate = list.get_plate(index))
                            observed.push_back({index, plate->is_slice_result_valid(), !plate->has_printable_instances(),
                                                plater.plate_validation_error(index)});
                    const auto results = classify_plates(ticket->plates, observed, own_runs(ticket->state, obs));
                    json       plates  = json::array();
                    for (size_t i = 0; i < ticket->plates.size(); ++i) {
                        json p{{"index", ticket->plates[i]}, {"outcome", to_string(results[i].outcome)}};
                        if (results[i].outcome == PlateOutcome::Ok) {
                            // The statistics are the plate's current result, so only while the
                            // project is as the operation sliced it and nobody sliced the plate since.
                            // They are read in this closure, which decides the outcome, since a later UI
                            // call would wait behind the preview's loading of the new result; the
                            // serializer's one pass over the moves is short next to that.
                            PartPlate*                  plate = list.get_plate(ticket->plates[i]);
                            const GCodeProcessorResult* gcode = plate != nullptr ? plate->get_slice_result() : nullptr;
                            if (gcode != nullptr && plate->is_slice_result_valid() &&
                                plater.project_revision() == ticket->state.revision_at_start &&
                                plate_result_is_own(ticket->plates[i], ticket->state, obs))
                                p.update(plate_summary(json::parse(gcode_statistics_json(*gcode)), *gcode));
                            else
                                p["note"] = "The plate changed after it was sliced; slice again for its statistics.";
                        }
                        if (!results[i].error.empty())
                            p["error"] = results[i].error;
                        plates.push_back(std::move(p));
                    }
                    out["plates"] = std::move(plates);
                    json notes = json::array();
                    for (const NotificationSummary& n : plater.get_notification_manager()->summaries())
                        if (n.level == "warning" || n.level == "error")
                            notes.push_back({{"level", n.level}, {"text", n.text}});
                    if (!notes.empty())
                        out["notifications"] = std::move(notes);
                    return out;
                },
                timeout, "slicing to finish");
            return ToolResult::ok(result);
        }));
    slice.budget_ms = 45000;
    slice.actions   = json::array(
        {NativeCommands::action("slice_and_preview", L("Slice and Preview"), L("Slice & Export"), "media_play", {{"preview", true}},
                                "Slice the current plate and show the Preview tab; the call waits for the result, and a pick starts "
                                "the slice as a job."),
         NativeCommands::action("slice_all", L("Slice All Plates"), L("Slice & Export"), "media_play",
                                {{"plates", "all"}, {"preview", true}},
                                "Slice every plate and show the Preview tab; the call waits for the results, and a pick starts the "
                                "slice as a job.")});
    catalog.add(std::move(slice));

    const json layer = {{"type", "integer"}, {"minimum", 1}, {"description", "1-based, as the Preview's layer slider numbers layers."}};
    const json z     = {{"type", "number"}, {"minimum", 0}, {"description", "mm: the layer whose top is at or above it."}};
    catalog.add(mutate_tool(
        "plate_set_custom_gcode", L("Set custom G-code at a layer"),
        "Add or remove pauses, custom G-code, custom templates and filament changes at the start of a layer, as the Preview "
        "layer slider's menu does; they are saved per plate in the project. They go on the plate's sliced layers, so the "
        "plate needs an up-to-date slice, or must be the one the Preview still shows after an earlier edit; the call selects "
        "it and shows the Preview. Applies clear, then remove, then add, all checked first. One code per layer: adding "
        "where a code of the same type is replaces it, and where another type is, is refused (remove it first). Not "
        "undoable, as in the slider. A change invalidates the "
        "plate's slice: slice again before exporting. Reply: the plate's codes as `plate_list`'s `layer_gcode` lists them, "
        "each with z and layer.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Default: the current plate."}}},
                       {"add", array_prop(object_schema({{"layer", layer},
                                                         {"z", z},
                                                         {"type", enum_prop({"pause", "custom", "template", "change_filament"},
                                                                            "template runs the printer's `template_custom_gcode`.")},
                                                         {"filament", {{"type", "integer"}, {"minimum", 1}, {"description", "`change_filament`: printed from this layer up."}}},
                                                         {"gcode", prop("string", "custom: the G-code.")}},
                                                        {"type"}),
                                          "Codes to add, each with layer or z.")},
                       {"remove", array_prop(object_schema({{"layer", layer}, {"z", z}}), "Codes to remove, by layer or z.")},
                       {"clear", prop("boolean", "Remove every code on the plate first.")}}),
        [](const json& args, ToolContext& ctx) {
            // A setting changed just before is applied by an update scheduled a moment later, which may
            // invalidate the plate.
            poll_ui(
                ctx, []() -> json { return ui_plater().is_background_process_update_scheduled() ? json() : json(true); },
                std::chrono::seconds(10), "the pending update of the slicing state");
            const int plate = ctx.run_ui([&] { return json(show_plate_layers(ui_plater(), args)); }).get<int>();
            // A replay edits the same plate, whichever is current then.
            json recorded     = args;
            recorded["plate"] = plate;
            ctx.set_journal_args(std::move(recorded));
            // The Preview loads the plate's G-code at its first frame when it shows for the first time.
            poll_ui(
                ctx,
                [plate]() -> json {
                    Plater& plater = ui_plater();
                    return preview_shows_plate(plater, plate) && !plater.is_slicing_any() ? json(true) : json();
                },
                std::chrono::seconds(15), "the Preview to show plate " + std::to_string(plate) + "'s layers");
            CustomGCode::Info handed_over;
            json              out = ctx.run_ui([&] { return set_custom_gcode_at_layer(ui_plater(), plate, args, handed_over); });
            if (!out.is_null())
                return ToolResult::ok(out);
            // The plater's handler writes the codes into the plate as the event arrives.
            return ToolResult::ok(poll_ui(
                ctx,
                [&]() -> json {
                    Plater&      plater = ui_plater();
                    const Model& model  = plater.model();
                    const auto   codes  = model.plates_custom_gcodes.find(plate);
                    return codes != model.plates_custom_gcodes.end() && codes->second == handed_over ? layer_gcode_reply(plater, plate, true, args) : json();
                },
                std::chrono::seconds(5), "the layer slider's codes to reach the project"));
        }));

    catalog.add(inspect_tool(
        "plate_slice_stats", L("Get slice statistics"),
        "What the Preview shows for a sliced plate. summary: `plate_slice`'s, plus prepare and travel time and travel "
        "distance. features: seconds, share of the print time and grams per feature type (the Line Type legend), and "
        "travel. layers: per layer (1-based, as `view_set`'s layer) Z, height, seconds and grams, paged; `layer_features` adds "
        "each feature type's seconds. objects: grams per object instance, with its `object_list` index when its name is "
        "unique. view, for the plate the Preview shows: view type, time mode, layer range, which legend rows are visible, "
        "and each view type's colour range for what the legend shows. Times are normal-mode estimates. A G-code file "
        "opened for viewing reads the same.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Plate index; default the current plate."}}},
                       {"include", array_prop({{"type", "string"}, {"enum", json::array({"features", "layers", "layer_features", "objects", "view"})}},
                                              "Parts besides the summary; default `[\"features\", \"view\"]`.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Layers per page (default 100)."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            const std::set<std::string> parts = args.contains("include") ? args["include"].get<std::set<std::string>>() :
                                                                           std::set<std::string>{"features", "view"};
            return ToolResult::ok(ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (plater.printer_technology() != ptFFF)
                    throw ToolFailure("The Preview needs an FFF printer");
                // The background process may be rewriting a result whatever its valid flag says.
                if (plater.is_slicing_any())
                    throw ToolFailure("Slicing is in progress; app_wait {\"for\": \"idle\"} first");
                PartPlateList& plates  = plater.get_partplate_list();
                const int      current = plates.get_curr_plate_index();
                const int      index   = arg_int(args, "plate", current);
                if (index >= plates.get_plate_count())
                    throw ToolFailure(no_such_plate(index, plates.get_plate_count()));
                GLCanvas3D* canvas = plater.get_preview_canvas3D();
                if (canvas == nullptr)
                    throw ToolFailure("The preview is not available");
                PartPlate*                  plate  = plates.get_plate(index);
                const GCodeProcessorResult* result = plate->get_slice_result();
                const bool                  shown  = index == current && viewer_holds(canvas->get_gcode_viewer(), result);
                // A G-code file opened for viewing fills the current plate's result, which the Preview
                // shows, without marking the plate sliced.
                const bool        gcode_file = plater.only_gcode_mode() && shown;
                const std::string name       = "Plate " + std::to_string(index);
                const std::string slice_it   = "plate_slice {\"plates\": " + std::to_string(index) + "}";
                if (result == nullptr || !(plate->is_slice_result_valid() || gcode_file))
                    throw ToolFailure(name + " has no up-to-date slice; " + slice_it + " first");
                // Of a project opened from a sliced 3MF, a plate the Preview has not shown keeps only the file's totals.
                if (result->moves.empty())
                    throw ToolFailure(name + " has no toolpaths, only the totals of a sliced 3MF; " + slice_it + " again for layer data");

                const json statistics      = json::parse(gcode_statistics_json(*result));
                const json& totals         = statistics.at("summary");
                json        summary        = plate_summary(statistics, *result);
                summary["prepare_time_s"]  = std::lround(totals.at("prepare_time").get<double>());
                summary["travel_time_s"]   = std::lround(totals.at("travel_time").get<double>());
                summary["travel_mm"]       = std::lround(totals.at("travel_distance").get<double>());
                json out{{"plate", index}, {"summary", std::move(summary)}};
                if (parts.count("features") > 0)
                    out["features"] = preview_features(statistics);
                if (parts.count("layers") > 0 || parts.count("layer_features") > 0)
                    out["layers"] = page(preview_layers(statistics, parts.count("layer_features") > 0), args, 100);
                if (parts.count("objects") > 0) {
                    std::vector<std::string> names;
                    for (const ModelObject* object : plater.model().objects)
                        names.push_back(object->name);
                    out["objects"] = preview_objects(statistics, names);
                }
                if (parts.count("view") > 0) {
                    out["view"] = shown ? preview_view(*canvas) : json();
                    if (index != current)
                        out["note"] = "The Preview shows plate " + std::to_string(current) + "; show plate " + std::to_string(index) +
                                      " there (plate_select {\"plate\": " + std::to_string(index) + "}) to read its legend";
                    else if (!shown)
                        out["note"] = "The Preview has not loaded this plate's toolpaths; show it with view_set {\"tab\": \"preview\"}";
                }
                return out;
            }));
        }));
}

}}} // namespace Slic3r::GUI::Mcp
