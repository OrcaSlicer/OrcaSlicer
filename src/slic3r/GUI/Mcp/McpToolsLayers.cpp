// set_layer_ranges: an object's height range modifiers (the object list's Height Range Modifier),
// given as the whole set of ranges. set_layer_height_profile: the Variable layer height panel's
// Adaptive, Smooth and Reset on an object. And how list_objects reports both. set_layer_gcode: the
// pauses, custom G-codes and filament changes of a plate's layers, through the Preview's layer slider.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TickCode.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/libslic3r.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// A height as messages show it: 15, 12.5, 0.08.
std::string mm(double z)
{
    std::ostringstream out;
    out << z;
    return out.str();
}

std::string range_text(const Slic3r::t_layer_height_range& range) { return mm(range.first) + "-" + mm(range.second); }

ToolFailure overlap(const Slic3r::t_layer_height_range& lower, const Slic3r::t_layer_height_range& upper)
{
    return ToolFailure("Ranges " + range_text(lower) + " and " + range_text(upper) +
                       " overlap; ranges may touch, one's max_z being the next one's min_z");
}

// For "did you mean": keys holding the first half of `key`, those that begin as it does first.
json similar_keys(const std::string& key, const std::vector<std::string>& keys)
{
    const std::string        part = key.substr(0, std::max<std::size_t>(3, key.size() / 2));
    std::vector<std::string> hits;
    std::copy_if(keys.begin(), keys.end(), std::back_inserter(hits), [&part](const std::string& k) { return k.find(part) != std::string::npos; });
    const auto common = [&key](const std::string& k) { return std::mismatch(k.begin(), k.end(), key.begin(), key.end()).first - k.begin(); };
    std::stable_sort(hits.begin(), hits.end(), [&common](const std::string& a, const std::string& b) { return common(a) > common(b); });
    hits.resize(std::min<std::size_t>(hits.size(), 5));
    return hits;
}

bool same_ranges(const t_layer_config_ranges& a, const t_layer_config_ranges& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) { return x.first == y.first && x.second.get() == y.second.get(); });
}

// What the GUI tells a person about an object's layer heights, as warnings: the dialog it shows before
// variable layer heights or height ranges when the process mixes colours in sublayers, and what the
// object list's variable layer height mark implies for the ranges.
json layer_height_warnings(const ModelObject& object)
{
    json       warnings = json::array();
    const bool variable = !layer_height_profile_json(object.layer_height_profile.get()).is_null();
    if (!variable && object.layer_config_ranges.empty())
        return warnings;
    const auto* mixed = wxGetApp().preset_bundle->prints.get_edited_preset().config.option<ConfigOptionBool>("enable_mixed_color_sublayer");
    if (mixed != nullptr && mixed->value)
        warnings.push_back("Using variable layer height together with mixed color sublayer may result in poor color mixing quality.");
    if (variable && !object.layer_config_ranges.empty())
        warnings.push_back("The object's variable layer height profile, not the ranges' layer_height, decides its layer heights.");
    return warnings;
}

// The object a layer call names, on an FFF printer.
ModelObject& layer_object(Plater& plater, const json& args)
{
    const ModelObjectPtrs& objects = plater.model().objects;
    const int              oi      = arg_int(args, "object", -1);
    if (oi < 0 || oi >= int(objects.size()))
        throw ToolFailure("object index out of range");
    if (plater.printer_technology() != ptFFF)
        throw ToolFailure("Height range modifiers and variable layer heights are for FFF printers");
    return *objects[std::size_t(oi)];
}

// The object set_layer_height_profile names, after the checks that need no view, such as an argument
// that goes with another action.
ModelObject& profile_object(Plater& plater, const json& args)
{
    ModelObject&      object = layer_object(plater, args);
    const std::string action = arg_string(args, "action");
    if (args.contains("quality") && action != "adaptive")
        throw ToolFailure("quality goes with action adaptive");
    if ((args.contains("radius") || args.contains("keep_min")) && action != "smooth")
        throw ToolFailure("radius and keep_min go with action smooth");
    return object;
}

// What the panel's Adaptive, Smooth and Reset buttons run, on the object selected as the panel's
// object is, without opening the panel. Reset of an object without a profile changes nothing.
json set_layer_height_profile(Plater& plater, const json& args)
{
    ModelObject&      object = profile_object(plater, args);
    const int         oi     = arg_int(args, "object", -1);
    const std::string action = arg_string(args, "action");
    GLCanvas3D&       canvas = require_prepare_scene(plater);

    if (action != "reset" || !object.layer_height_profile.empty()) {
        select_rows(&object, nullptr);
        // The rule that enables the toolbar's Variable layer height item, with the reason it fails.
        if (!plater.can_layers_editing())
            throw ToolFailure(object.max_z() <= SINKING_Z_THRESHOLD ?
                                  "object " + std::to_string(oi) + " is below the bed, where its layer heights can't be edited" :
                              !canvas.is_layers_editing_allowed() ?
                                  "The variable layer height tool needs an OpenGL shader that this system did not load" :
                                  "object " + std::to_string(oi) + " could not be selected for the variable layer height tool");
        if (action == "adaptive")
            canvas.adaptive_layer_height_profile(float(arg_double(args, "quality", 0.5)));
        else if (action == "smooth")
            canvas.smooth_layer_height_profile(HeightProfileSmoothingParams(unsigned(arg_int(args, "radius", 5)), arg_bool(args, "keep_min", false)));
        else
            canvas.reset_layer_height_profile();
    }

    json out{{"object", oi}, {"height", object.instance_bounding_box(0).max.z()}};
    if (json profile = layer_height_profile_json(object.layer_height_profile.get()); !profile.is_null())
        out["layer_height_profile"] = std::move(profile);
    if (json warnings = layer_height_warnings(object); !warnings.empty())
        out["warnings"] = std::move(warnings);
    return out;
}

// What the object list does when a person adds, edits and removes ranges, with the whole set given at
// once. Everything the arguments can tell is checked before anything changes.
json set_layer_ranges(Plater& plater, const json& args)
{
    ModelObject*          object = &layer_object(plater, args);
    const int             oi     = arg_int(args, "object", -1);
    ObjectList*           list   = wxGetApp().obj_list();
    t_layer_config_ranges ranges = layer_config_ranges_from_json(args.value("layer_config_ranges", json()), list->get_default_layer_config(oi),
                                                                 wxGetApp().filaments_cnt(), [](int extruder) {
                                                                     return std::make_pair(get_min_layer_height(extruder), get_max_layer_height(extruder));
                                                                 });
    require_prepare_scene(plater);

    if (!same_ranges(ranges, object->layer_config_ranges)) {
        // The object's row, as right-clicking it selects it, so no row or sidebar editor of the old ranges
        // stays in use while they are replaced.
        select_rows(object, nullptr);
        plater.take_snapshot("Edit height ranges");
        object->layer_config_ranges = std::move(ranges);
        list->reload_layer_rows(oi);
        plater.changed_object(oi);
    }

    json out{{"object", oi},
             {"height", object->instance_bounding_box(0).max.z()},
             {"layer_config_ranges", layer_config_ranges_json(object->layer_config_ranges)}};
    if (json warnings = layer_height_warnings(*object); !warnings.empty())
        out["warnings"] = std::move(warnings);
    return out;
}

// The plate set_layer_gcode names (default: the current one), after the checks that need neither its
// layers nor a view.
int gcode_plate(Plater& plater, const json& args)
{
    if (plater.printer_technology() != ptFFF || plater.only_gcode_mode())
        throw ToolFailure("Layer G-codes are for the plates of an FFF project");
    const bool edits = arg_bool(args, "clear", false) || !args.value("add", json::array()).empty() ||
                       !args.value("remove", json::array()).empty();
    if (!edits)
        throw ToolFailure("Give add, remove or clear; list_plates lists each plate's layer_gcode");
    PartPlateList& plates = plater.get_partplate_list();
    const int      index  = arg_int(args, "plate", plates.get_curr_plate_index());
    if (index < 0 || index >= plates.get_plate_count())
        throw ToolFailure("plate must be 0.." + std::to_string(plates.get_plate_count() - 1));
    PartPlate&        plate = *plates.get_plate(index);
    const std::string name  = "plate " + std::to_string(index);
    // The slider's menus offer no codes on a plate printed by object, and it erases them all when it
    // shows one; a sliced G-code shown without its objects (.gcode.3mf) has no menus either.
    if (!plate.has_printable_instances())
        throw ToolFailure(name + " has nothing to print, so no layers to place codes on");
    if (plate.get_real_print_seq() == PrintSequence::ByObject)
        throw ToolFailure(name + " prints by object (print_sequence), which takes no layer G-codes");
    // A slice would replace the layers under the edit.
    if (plater.is_slicing_any())
        throw ToolFailure("Slicing is in progress; wait {for: \"idle\"} first");
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

// A plate's codes as list_plates' layer_gcode lists them: by height, z to the G-code's 0.001 mm, layer
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

// set_layer_gcode's reply, once the plater has the slider's codes. UI thread.
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
            out["warnings"] = json::array({"machine_pause_gcode is empty, so a pause adds no command (set it with set_settings scope printer)"});
            break;
        }
    return out;
}

// Selects the plate set_layer_gcode names and shows it in the Preview, unless the Preview shows its layers
// already, and returns its index. Refused calls leave the view alone. Only a plate with an up-to-date slice
// is opened: opening the Preview on one that is not starts a slice.
int show_plate_layers(Plater& plater, const json& args)
{
    const int index = gcode_plate(plater, args);
    if (preview_shows_plate(plater, index))
        return index;
    PartPlateList& plates = plater.get_partplate_list();
    if (!plates.get_plate(index)->is_slice_result_valid())
        throw ToolFailure("plate " + std::to_string(index) + " has no up-to-date slice to place codes on; slice it first (slice {plates: " +
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
json set_layer_gcode(Plater& plater, int index, const json& args, CustomGCode::Info& handed_over)
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

std::vector<std::string> layer_range_option_keys(int filaments)
{
    // A range's page in the sidebar edits layer_height and the per-region settings, of which the object
    // list's Add settings menus offer a part or a range those with a category (no internal option),
    // leaving out support settings, and per-feature filaments and wipe options while there is one
    // filament (SettingsFactory's is_improper_category()).
    std::vector<std::string> keys{"layer_height"};
    for (const std::string& key : PrintRegionConfig().keys()) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr || def->category.empty() || def->category == "Support material" ||
            (filaments == 1 && (def->category == "Extruders" || def->category == "Wipe options")))
            continue;
        keys.push_back(key);
    }
    // The filament column of a range's row.
    keys.push_back("extruder");
    return keys;
}

t_layer_config_ranges layer_config_ranges_from_json(const json& ranges, const DynamicPrintConfig& defaults, int filaments,
                                                    const std::function<std::pair<double, double>(int extruder)>& layer_height_bounds)
{
    if (!ranges.is_array())
        throw ToolFailure("layer_config_ranges must be a list of {min_z, max_z, values}");
    const std::vector<std::string> keys = layer_range_option_keys(filaments);
    t_layer_config_ranges          out;
    for (const json& r : ranges) {
        if (!r.is_object())
            throw ToolFailure("A range is {min_z, max_z, values}");
        const Slic3r::t_layer_height_range range{arg_double(r, "min_z", -1.), arg_double(r, "max_z", -1.)};
        if (!(range.first >= 0. && range.first < range.second))
            throw ToolFailure("A height range needs 0 <= min_z < max_z, not " + range_text(range));
        const json values = r.value("values", json::object());
        if (!values.is_object())
            throw ToolFailure("A range's values are {key: value}");
        DynamicPrintConfig config = defaults;
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
                throw ToolFailure("\"" + it.key() +
                                      "\" is not a height range setting; a range takes layer_height, extruder and per-region "
                                      "process settings such as sparse_infill_density",
                                  {{"similar", similar_keys(it.key(), keys)}});
            try {
                config.set_deserialize_strict(it.key(), to_config_string(it.value(), it.key()));
            } catch (const std::exception& e) {
                throw ToolFailure("Invalid value for " + it.key() + ": " + e.what());
            }
        }
        const int extruder = config.opt_int("extruder");
        if (extruder < 0 || extruder > filaments)
            throw ToolFailure("extruder " + std::to_string(extruder) + " is not a filament: 0 (the object's) or " +
                              (filaments > 1 ? "1-" + std::to_string(filaments) : std::string("1")));
        // Only a layer height given is checked, as the range editor checks only what a person types.
        if (values.contains("layer_height")) {
            const double                    height = config.opt_float("layer_height");
            const std::pair<double, double> bounds = layer_height_bounds(extruder);
            if (!(height >= bounds.first && height <= bounds.second))
                throw ToolFailure("layer_height " + mm(height) + " is outside this printer's " + mm(bounds.first) + "-" + mm(bounds.second));
        }
        if (out.count(range) != 0)
            throw overlap(range, range);
        out[range].assign_config(std::move(config));
    }
    // Sorted by min_z; the slicer would trim an overlap silently, which from an agent is always a mistake.
    const Slic3r::t_layer_height_range* lower = nullptr;
    for (const auto& entry : out) {
        if (lower != nullptr && entry.first.first + EPSILON < lower->second)
            throw overlap(*lower, entry.first);
        lower = &entry.first;
    }
    return out;
}

json layer_config_ranges_json(const t_layer_config_ranges& ranges)
{
    json out = json::array();
    for (const auto& [range, config] : ranges) {
        json values = json::object();
        for (const std::string& key : config.keys())
            values[key] = config.opt_serialize(key);
        out.push_back({{"min_z", range.first}, {"max_z", range.second}, {"values", std::move(values)}});
    }
    return out;
}

json layer_height_profile_json(const std::vector<double>& profile)
{
    // The object list marks an object's layer height as variable when its profile is more than a flat
    // bottom-to-top pair.
    if (profile.size() <= 4)
        return nullptr;
    double low = profile[1], high = profile[1];
    for (std::size_t i = 3; i < profile.size(); i += 2) {
        low  = std::min(low, profile[i]);
        high = std::max(high, profile[i]);
    }
    // Computed heights, to the G-code's 0.001 mm.
    const auto um = [](double h) { return std::round(h * 1000.) / 1000.; };
    return {{"points", profile.size() / 2}, {"min_height", um(low)}, {"max_height", um(high)}};
}

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
            throw ToolFailure("The printer's template_custom_gcode is empty; set it (set_settings scope printer) or use type custom");
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
                              (changed ? "; the codes before it changed (list_plates lists them)" : ""));
        changed = true;
        event   = code.type;
    }
    if (changed && event != CustomGCode::Unknown)
        slider.post_ticks_changed_event(event);
    return changed;
}

void register_layer_tools(McpService& service)
{
    ToolRegistry& reg   = service.registry();
    const json    range = object_schema(
        {{"min_z", {{"type", "number"}, {"minimum", 0}, {"description", "Bottom, mm above the bed."}}},
         {"max_z", {{"type", "number"}, {"minimum", 0}, {"description", "Top, mm above the bed."}}},
         {"values", {{"type", "object"},
                     {"description", "{key: value} as set_settings takes them: layer_height, extruder (0 = the object's "
                                     "filament) and per-region process settings such as sparse_infill_density or wall_loops."}}}},
        {"min_z", "max_z"});

    reg.add(mutate_tool(
        "set_layer_ranges", "Set height range modifiers",
        "Replace an object's height range modifiers (Height Range Modifier in the object list): Z ranges in mm, each printed "
        "with its own layer_height, filament and process settings, starting from the object's layer height and filament. "
        "Ranges may touch but not overlap; [] removes them all. One undo step; the object is selected. Replies with the "
        "object's height and its ranges as list_objects shows them, and warnings, such as a variable layer height profile "
        "deciding the layer heights instead.",
        object_schema({{"object", {{"type", "integer"}, {"minimum", 0}}},
                       {"layer_config_ranges", array_prop(range, "The object's ranges afterwards, in any order.")}},
                      {"object", "layer_config_ranges"}),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] { return set_layer_ranges(ui_plater(), args); });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "set_layer_height_profile", "Set a variable layer height profile",
        "Run the Variable layer height panel's Adaptive, Smooth or Reset on an object, without opening the panel: adaptive "
        "computes layer heights from the object's shape (inside a height range, the range's layer_height), smooth smooths "
        "its profile (or the one its height ranges give), reset goes back to its layer height and ranges. One undo step; "
        "the object is selected. Replies with the object's height, its layer_height_profile {points, min_height, "
        "max_height} as list_objects shows it (absent after reset), and warnings.",
        object_schema({{"object", {{"type", "integer"}, {"minimum", 0}}},
                       {"action", enum_prop({"adaptive", "smooth", "reset"}, "The panel's button.")},
                       {"quality", {{"type", "number"}, {"minimum", 0}, {"maximum", 1},
                                    {"description", "adaptive: 0 = quality .. 1 = speed (default 0.5, as the panel)."}}},
                       {"radius", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}, {"description", "smooth: default 5."}}},
                       {"keep_min", prop("boolean", "smooth: keep the minimum layer heights (default false).")}},
                      {"object", "action"}),
        [](const json& args, ToolContext& ctx) {
            // Refused calls leave the view alone. The Prepare canvas edits layer heights only once it has drawn:
            // GLCanvas3D::init() makes the texture the tool needs, and the app starts on Home.
            ctx.run_ui([&] {
                Plater& plater = ui_plater();
                profile_object(plater, args);
                require_prepare_scene(plater);
                return json();
            });
            poll_ui(
                ctx,
                []() -> json {
                    const GLCanvas3D* canvas = ui_plater().canvas3D();
                    return canvas->is_initialized() && !canvas->is_reload_delayed() ? json(true) : json();
                },
                std::chrono::seconds(15), "the Prepare view to draw");
            json out = ctx.run_ui([&] { return set_layer_height_profile(ui_plater(), args); });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    const json layer = {{"type", "integer"}, {"minimum", 1}, {"description", "1-based, as the Preview's layer slider numbers layers."}};
    const json z     = {{"type", "number"}, {"minimum", 0}, {"description", "mm: the layer whose top is at or above it."}};
    reg.add(mutate_tool(
        "set_layer_gcode", "Set layer G-codes",
        "Add or remove pauses, custom G-code, custom templates and filament changes at the start of a layer, as the Preview "
        "layer slider's menu does; they are saved per plate in the project. They go on the plate's sliced layers, so the "
        "plate needs an up-to-date slice, or must be the one the Preview still shows after an earlier edit; the call selects "
        "it and shows the Preview. Applies clear, then remove, then add, all checked first. One code per layer: a custom "
        "code or filament change where there is one replaces it. Not undoable, as in the slider. A change invalidates the "
        "plate's slice: slice again before exporting. Replies with the plate's codes as list_plates' layer_gcode lists "
        "them, each with z and layer.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Default: the current plate."}}},
                       {"add", array_prop(object_schema({{"layer", layer},
                                                         {"z", z},
                                                         {"type", enum_prop({"pause", "custom", "template", "change_filament"},
                                                                            "template runs the printer's template_custom_gcode.")},
                                                         {"filament", {{"type", "integer"}, {"minimum", 1}, {"description", "change_filament: printed from this layer up."}}},
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
            json              out = ctx.run_ui([&] { return set_layer_gcode(ui_plater(), plate, args, handed_over); });
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
}

}}} // namespace Slic3r::GUI::Mcp
