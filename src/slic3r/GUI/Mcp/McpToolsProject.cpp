// open_files, new_project, save_project and export: files in and out without file dialogs. generate_calibration_test: the
// Calibration menu's tests, which start a new project.

#include "slic3r/GUI/I18N.hpp"
#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/ExportOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/Operations/ProjectOps.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/calib_dlg.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

bool has_extension(const fs::path& p, std::initializer_list<const char*> exts)
{
    const std::string name = path_to_utf8(p.filename());
    for (const char* e : exts)
        if (boost::algorithm::iends_with(name, e))
            return true;
    return false;
}

json objects_summary(Plater& plater)
{
    json names = json::array();
    for (const ModelObject* o : plater.model().objects)
        names.push_back(o->name);
    return {{"objects", plater.model().objects.size()}, {"names", std::move(names)}, {"plates", plater.get_partplate_list().get_plate_count()}};
}

json project_json(const Ops::ProjectSummary& project)
{
    return {{"objects", project.object_names.size()}, {"names", project.object_names}, {"plates", project.plates}};
}

Ops::UnsavedChanges unsaved_changes_answer(bool discard) { return discard ? Ops::UnsavedChanges::Discard : Ops::UnsavedChanges::Refuse; }

// What a tool that replaces the project adds to the refusal of unsaved changes.
constexpr const char* kUnsavedChangesHint = "save_project first, or pass discard_changes: true.";

// Fails when the project has unsaved changes the caller did not say to drop.
void require_project_discardable(bool discard)
{
    if (const std::optional<Ops::Refusal> refusal = Ops::unsaved_changes_refusal(unsaved_changes_answer(discard)))
        throw refused(*refusal, kUnsavedChangesHint);
}

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

// generate_calibration_test's test_model values, as each test's dialog lists its models.
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
    case CalibMode::Calib_Temp_Tower: return {{"scope", "filament"}, {"keys", {"nozzle_temperature", "nozzle_temperature_initial_layer"}}};
    case CalibMode::Calib_Vol_speed_Tower: return {{"scope", "filament"}, {"keys", {"filament_max_volumetric_speed"}}};
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_PA_Line: return {{"scope", "filament"}, {"keys", {"enable_pressure_advance", "pressure_advance"}}};
    case CalibMode::Calib_PA_Pattern:
        return {{"scope", "filament"},
                {"keys", {"enable_pressure_advance", "pressure_advance", "adaptive_pressure_advance", "adaptive_pressure_advance_model"}}};
    case CalibMode::Calib_Flow_Rate: return {{"scope", "filament"}, {"keys", {"filament_flow_ratio"}}};
    case CalibMode::Calib_Retraction_tower: return {{"scope", "filament"}, {"keys", {"filament_retraction_length"}}};
    case CalibMode::Calib_VFA_Tower:
        return {{"scope", "printer"}, {"keys", {"resonance_avoidance", "min_resonance_avoidance_speed", "max_resonance_avoidance_speed"}}};
    case CalibMode::Calib_Cornering:
        return {{"scope", "printer"},
                {"keys", has_junction_deviation(printer) ? json{"machine_max_junction_deviation"} : json{"machine_max_jerk_x", "machine_max_jerk_y"}}};
    default:
        return {{"scope", "printer"},
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

// export's format for a path given without one, from its extension.
std::string format_of(const std::string& path)
{
    static const std::pair<const char*, const char*> formats[]{{".gcode.3mf", "sliced_3mf"}, {".gcode", "gcode"}, {".3mf", "3mf"}, {".stl", "stl"}};
    for (const auto& [ext, format] : formats)
        if (boost::algorithm::iends_with(path, ext))
            return format;
    throw ToolFailure("Give format (gcode, sliced_3mf, 3mf or stl), or a path ending in .gcode, .gcode.3mf, .3mf or .stl");
}

} // namespace

Calib_Params calib_params_from_args(const json& args, const DynamicPrintConfig& printer, std::vector<std::string>& warnings)
{
    const std::string test = arg_string(args, "test");
    // The arguments each test takes, besides test, discard_changes and preset_changes.
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
        if (it.key() != "test" && it.key() != "discard_changes" && it.key() != "preset_changes" &&
            std::find(takes.begin(), takes.end(), it.key()) == takes.end())
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

void register_project_tools(CommandCatalog& catalog)
{
    catalog.add(reaching({"open_project", "import_models"}, mutate_tool(
        "api", "open_files", L("Open files"),
        "Open model files (STL, 3MF, STEP, OBJ, AMF, SVG...), a project (.3mf) or G-code by absolute path, without a file "
        "dialog. Models are added to the current project. A .3mf opens as a project, replacing the current one, unless as "
        "is \"geometry\"; when the current project has unsaved changes that fails, changing nothing, unless "
        "discard_changes. Importing can open a dialog (STEP mesh options, for instance); the reply is then "
        "blocked_by_dialog.",
        object_schema({{"paths", array_prop({{"type", "string"}}, "Absolute paths.")},
                       {"as", enum_prop({"auto", "geometry", "project"}, "How to open .3mf files. Default auto (= project).")},
                       {"discard_changes", prop("boolean", "Drop the project's unsaved changes rather than fail when a .3mf "
                                                           "replaces it.")}},
                      {"paths"}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            std::vector<fs::path> paths;
            for (const json& p : args["paths"]) {
                std::string err;
                auto        resolved = resolve_input_path(p.get<std::string>(), err);
                if (!resolved)
                    throw ToolFailure(err);
                paths.push_back(*resolved);
            }
            if (paths.empty())
                throw ToolFailure("paths is empty");
            const std::string as      = arg_string(args, "as", "auto");
            // A run the user let drop unsaved changes (Run Script's "don't save") counts as discard_changes.
            const bool        discard = arg_bool(args, "discard_changes", false) || service.discard_authorized(ctx.lineage());
            json out = ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (paths.size() == 1 && has_extension(paths[0], {".gcode", ".bgcode"})) {
                    plater.load_gcode(from_path(paths[0]));
                    return {{"opened", "gcode"}, {"path", path_to_utf8(paths[0])}};
                }
                if (paths.size() == 1 && as != "geometry" && has_extension(paths[0], {".3mf"})) {
                    Ops::OpenProjectArgs open;
                    open.path    = paths[0];
                    open.unsaved = unsaved_changes_answer(discard);
                    const Ops::Outcome<Ops::ProjectSummary> opened = Ops::open_project(open);
                    if (!opened)
                        throw refused(opened.refusal(), kUnsavedChangesHint);
                    json o      = project_json(opened.value());
                    o["opened"] = "project";
                    return o;
                }
                Ops::ImportModelsArgs models;
                models.paths = paths;
                const Ops::Outcome<Ops::ImportModelsResult> imported = Ops::import_models(models);
                if (!imported)
                    throw refused(imported.refusal(), "See get_log for the reason.");
                json o      = objects_summary(plater);
                o["opened"] = "models";
                o["added"]  = imported.value().added;
                return o;
            });
            settle(ctx);
            return ToolResult::ok(out);
        })));

    // The Speed Dial's New Project too: with no arguments it asks as the tile does.
    catalog.add(NativeCommands::with_person_form(reaching({"new_project"}, mutate_tool(
        {}, "new_project", {},
        "Start a new, empty project. Called with no arguments, it asks as New Project does, about the unsaved changes "
        "if there are any: the reply is then blocked_by_dialog, and answer_dialog answers it. With arguments, or silent: "
        "true, it asks nothing: it fails, changing nothing, when the current project has unsaved changes, unless "
        "discard_changes. The presets' unsaved changes carry over to the new project unless preset_changes is \"discard\", as "
        "New Project's prompt offers. Reply: objects, names, plates; when presets had or have unsaved changes, discarded "
        "and modified list per preset the keys dropped and the keys still unsaved.",
        object_schema({{"discard_changes", prop("boolean", "Drop the project's unsaved changes rather than fail; for the "
                                                           "presets', see preset_changes.")},
                       {"preset_changes", enum_prop({"keep", "discard"}, "The presets' unsaved changes, not the project's: keep "
                                                                  "them for the new project (default) or drop them.")}}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            const bool discard         = arg_bool(args, "discard_changes", false) || service.discard_authorized(ctx.lineage());
            const bool discard_presets = arg_string(args, "preset_changes", "keep") == "discard";
            json       out             = ctx.run_ui([&] {
                ui_plater();
                const json          before = unsaved_changes();
                Ops::NewProjectArgs fresh;
                fresh.unsaved                = unsaved_changes_answer(discard);
                fresh.discard_preset_changes = discard_presets;
                const Ops::Outcome<Ops::ProjectSummary> started = Ops::new_project(fresh);
                if (!started)
                    throw refused(started.refusal(), kUnsavedChangesHint);
                json o = project_json(started.value());
                o.update(unsaved_changes_outcome(before));
                return o;
            });
            settle(ctx);
            return ToolResult::ok(out);
        })),
        L("New Project"), L("Commands"), "menu_open", [] {
            if (Plater* plater = wxGetApp().plater())
                plater->new_project();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }));

    catalog.add(mutate_tool(
        "api", "generate_calibration_test", L("Generate a calibration test"),
        "Generate a Calibration menu test as its dialog's OK does: a new project named after the test with the test "
        "model, and the process, filament and printer presets changed as the test needs (presets_changed). The test prints "
        "with filament slot 1, so the filament tab is pointed at slot 1's preset. Omitted parameters take the dialog's "
        "defaults (pressure advance's by the printer's extruder type); temperatures are clamped to 155-500 °C and rounded "
        "down to 5 °C. Fails, changing nothing, when the project has unsaved changes (pass discard_changes) or presets do "
        "(pass preset_changes). Reply: test, project, params (the values used, which the journal records), objects, names, "
        "plates, presets_changed, result_keys (where the measured value belongs), warnings, adjusted (vfa), and discarded "
        "and modified as new_project. Then slice and export. To save a measured value: new_project {discard_changes, "
        "preset_changes: \"discard\"}, set_settings the value, save_preset.",
        object_schema(
            {{"test", enum_prop({"temperature", "max_volumetric", "pressure_advance", "flow_ratio", "retraction", "vfa", "cornering",
                                 "input_shaping_freq", "input_shaping_damp"},
                                "The test, named as the calib_* actions.")},
             {"method", enum_prop({"tower", "line", "pattern", "pass1", "pass2", "yolo", "yolo_perfectionist"},
                                  "pressure_advance: tower (default), line or pattern; flow_ratio: pass1, pass2, yolo (default) or "
                                  "yolo_perfectionist.")},
             {"start", prop("number", "Start of the swept value: °C, mm³/s, PA, mm, mm/s (vfa), Hz (input_shaping_freq), "
                                      "damping (input_shaping_damp), junction deviation mm or jerk mm/s (cornering).")},
             {"end", prop("number", "End of the swept value.")},
             {"step", prop("number", "Step: max_volumetric, pressure_advance, retraction, vfa.")},
             {"start_y", prop("number", "input_shaping_freq: start of the Y range; default the X one.")},
             {"end_y", prop("number", "input_shaping_freq: end of the Y range; default the X one.")},
             {"damping", prop("number", "input_shaping_freq: the damping ratio printed with.")},
             {"freq_x", prop("number", "input_shaping_damp: the X frequency, Hz.")},
             {"freq_y", prop("number", "input_shaping_damp: the Y frequency, Hz; default freq_x.")},
             {"shaper_type", prop("string", "Input shaping: an input_shaping_type the printer's firmware offers; default the first.")},
             {"test_model", enum_prop({"ringing_tower", "fast_tower", "scv_v2", "standard", "overhang"},
                                      "Input shaping: ringing_tower (default) or fast_tower; cornering: also scv_v2; temperature on "
                                      "belt printers: standard (default) or overhang.")},
             {"print_numbers", prop("boolean", "pressure_advance line: print the values (default true).")},
             {"accelerations", array_prop({{"type", "integer"}, {"minimum", 1}},
                                          "pressure_advance pattern: one test per acceleration and speed; default the process's.")},
             {"speeds", array_prop({{"type", "integer"}, {"minimum", 1}}, "pressure_advance pattern: default an optimal speed.")},
             {"top_surface_pattern", enum_prop({"archimedeanchords", "monotonic"}, "flow_ratio (default archimedeanchords).")},
             {"auto_scale", prop("boolean", "temperature, vfa: scale the model to the nozzle (default true).")},
             {"auto_adjust", prop("boolean", "vfa: lower the layer height, else the end speed, to stay within the filament's "
                                             "maximum volumetric speed (default true).")},
             {"discard_changes", prop("boolean", "Drop the project's unsaved changes rather than fail; for the presets', "
                                                 "see preset_changes.")},
             {"preset_changes", enum_prop({"keep", "discard"}, "The presets' unsaved changes, not the project's: keep them for the "
                                                        "test or drop them first. Required when presets are modified.")}},
            {"test"}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            json       recorded = args;
            const bool discard  = arg_bool(args, "discard_changes", false) || service.discard_authorized(ctx.lineage());
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
                // The test prints with slot 1; switching the filament tab to it must not ask about another preset's changes.
                const std::string& slot1 = bundle.filament_presets.front();
                if (!discard_presets && bundle.filaments.get_edited_preset().name != slot1 && bundle.filaments.current_is_dirty())
                    throw ToolFailure("The filament tab edits \"" + bundle.filaments.get_edited_preset().name +
                                      "\", which has unsaved changes, and the test prints with filament slot 1 (\"" + slot1 +
                                      "\"); pass preset_changes: \"discard\", or save or drop those changes first");
                if (discard_presets && wxGetApp().has_current_preset_changes())
                    wxGetApp().discard_current_preset_changes();
                const std::string switched_from = edit_filament_slot(1);

                // The journal keeps VFA's end as asked, since the fit recomputes it from the presets.
                recorded.update(calib_params_json(args, params, printer));
                const json adjusted = params.mode == CalibMode::Calib_VFA_Tower ?
                                          apply_vfa_fit(params, arg_bool(args, "auto_adjust", true), bundle, warnings) :
                                          json();
                const DynamicPrintConfig process_before = bundle.prints.get_edited_preset().config;
                const DynamicPrintConfig filament_before = bundle.filaments.get_edited_preset().config;
                const DynamicPrintConfig printer_before = bundle.printers.get_edited_preset().config;
                switch (params.mode) {
                case CalibMode::Calib_Temp_Tower: plater.calib_temp(params, true); break;
                case CalibMode::Calib_Vol_speed_Tower: plater.calib_max_vol_speed(params, true); break;
                case CalibMode::Calib_PA_Tower:
                case CalibMode::Calib_PA_Line:
                case CalibMode::Calib_PA_Pattern: plater.calib_pa(params, true); break;
                case CalibMode::Calib_Flow_Rate: {
                    const std::string method = arg_string(args, "method", "yolo");
                    plater.calib_flowrate(method == "yolo" || method == "yolo_perfectionist",
                                          method == "pass2" || method == "yolo_perfectionist" ? 2 : 1,
                                          arg_string(args, "top_surface_pattern") == "monotonic" ? ipMonotonic : ipArchimedeanChords, true);
                    break;
                }
                case CalibMode::Calib_Retraction_tower: plater.calib_retraction(params, true); break;
                case CalibMode::Calib_VFA_Tower: plater.calib_VFA(params, true); break;
                case CalibMode::Calib_Input_shaping_freq: plater.calib_input_shaping_freq(params, true); break;
                case CalibMode::Calib_Input_shaping_damp: plater.calib_input_shaping_damp(params, true); break;
                case CalibMode::Calib_Cornering: plater.Calib_Cornering(params, true); break;
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
                                               {"filament_slot", 1},
                                               {"changes", changed_settings(filament_before, bundle.filaments.get_edited_preset().config)}};
                if (!switched_from.empty())
                    presets_changed["filament"]["tab_switched_from"] = switched_from;
                if (plater.model().objects.empty())
                    throw ToolFailure("The project was replaced but the test model was not added; see get_log",
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
        }));

    // The Speed Dial's Save Project too: with no arguments it saves as the tile does.
    catalog.add(NativeCommands::with_person_form(reaching({"save_project_as"}, mutate_tool(
        {}, "save_project", {},
        "Save the project. Called with no arguments, it saves as Save Project does: to the project's file, or, when it has "
        "none, through a file dialog (blocked_by_dialog; a native picker needs auto_answer_dialogs registered first). With "
        "path, it saves a .3mf there without a dialog, and the file becomes the project's.",
        object_schema({{"path", prop("string", "Absolute .3mf path.")}, {"overwrite", prop("boolean", "Replace an existing file.")}}, {"path"}),
        [](const json& args, ToolContext& ctx) {
            std::string err;
            auto        path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
            if (!path)
                throw ToolFailure(err);
            ctx.run_ui([&] {
                ui_plater();
                Ops::SaveProjectAsArgs save;
                save.path = *path;
                if (const Ops::Outcome<Ops::Done> saved = Ops::save_project_as(save); !saved)
                    throw refused(saved.refusal(), std::string(saved.refusal().code) == "sliced_file_name" ?
                                                       "That is export {format: \"sliced_3mf\"} (plates \"all\" for every plate)." :
                                                       "See get_log.");
                return json();
            });
            settle(ctx);
            return ToolResult::ok({{"path", path_to_utf8(*path)}, {"bytes", fs::file_size(*path)}});
        })),
        L("Save Project"), L("Commands"), "menu_save", [] {
            if (Plater* plater = wxGetApp().plater())
                plater->save_project(false);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }));

    catalog.add(reaching({"start_gcode_export", "export_sliced_3mf", "export_model_3mf", "export_stl"}, mutate_tool(
        "api", "export", L("Export"),
        "Write the project to a file without a dialog. gcode: the plate's G-code (slice first; it is written in the "
        "background, and the call returns once it is). sliced_3mf: the sliced plate, or all plates, as .gcode.3mf. 3mf: "
        "the model without slicing data. stl: all objects as one STL. Without format, the path's extension (.gcode, "
        ".gcode.3mf, .3mf, .stl) says which.",
        object_schema({{"format", enum_prop({"gcode", "sliced_3mf", "3mf", "stl"}, "What to write; default from the path's extension.")},
                       {"path", prop("string", "Absolute output path.")},
                       {"plates", {{"type", {"string", "integer"}},
                                   {"description", "\"current\" (default), a plate index, which is selected first, or \"all\" "
                                                   "(sliced_3mf only)."}}},
                       {"overwrite", prop("boolean", "Replace an existing file.")}},
                      {"path"}),
        [](const json& args, ToolContext& ctx) {
            const std::string format = args.contains("format") ? arg_string(args, "format") : format_of(arg_string(args, "path"));
            std::string       err;
            auto              path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
            if (!path)
                throw ToolFailure(err);
            const json plate = args.value("plates", json("current"));
            const bool all   = plate.is_string() && plate.get<std::string>() == "all";
            if (all && format != "sliced_3mf")
                throw ToolFailure("plates \"all\" applies to sliced_3mf only");
            std::optional<Ops::ExportTicket> ticket;
            ctx.run_ui([&]() -> json {
                // A G-code export finishes in the background: held from before it starts.
                if (format == "gcode")
                    ctx.begin_operation();
                ui_plater();
                const std::optional<int> index = plate.is_number_integer() ? std::optional<int>(plate.get<int>()) : std::nullopt;
                if (format == "gcode") {
                    Ops::ExportGcodeArgs gcode;
                    gcode.path  = *path;
                    gcode.plate = index;
                    const Ops::Outcome<Ops::ExportTicket> started = Ops::start_gcode_export(gcode);
                    if (!started) {
                        const std::string code = started.refusal().code;
                        throw refused(started.refusal(), code == "slicing_in_progress" ? "wait {for:\"idle\"} waits for it." :
                                                         code == "export_not_started"  ? "Check get_app_state.notifications." :
                                                                                         "");
                    }
                    ticket = started.value();
                    return json();
                }
                Ops::ExportSlicedArgs sliced;
                sliced.path       = *path;
                sliced.all_plates = all;
                sliced.plate      = index;
                Ops::ExportFileArgs file;
                file.path  = *path;
                file.plate = index;
                const Ops::Outcome<Ops::Done> written = format == "sliced_3mf" ? Ops::export_sliced_3mf(sliced) :
                                                        format == "3mf"        ? Ops::export_model_3mf(file) :
                                                                                 Ops::export_stl(file);
                if (!written)
                    throw refused(written.refusal(), std::string(written.refusal().code) == "file_not_written" ? "See get_log." : "");
                return json();
            });
            if (ticket)
                return await_gcode_export(ctx, *ticket);
            settle(ctx);
            return ToolResult::ok({{"path", path_to_utf8(*path)}, {"bytes", fs::file_size(*path)}});
        })));
}

}}} // namespace Slic3r::GUI::Mcp
