#include "NativeCommands.hpp"

#include "calib_dlg.hpp"
#include "Camera.hpp"
#include "DailyTips.hpp"
#include "GCodeViewer.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmoSVG.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "I18N.hpp"
#include "Shortcuts.hpp"
#include "IMSlider.hpp"
#include "MainFrame.hpp"
#include "NetworkTestDialog.hpp"
#include "Plater.hpp"
#include "PluginsDialog.hpp"
#include "PartPlate.hpp"
#include "PlateSettingsDialog.hpp"
#include "SceneBenchmark.hpp"
#include "DeviceCore/DevManager.h"
#include "Mcp/McpPaths.hpp"

#include "libslic3r/Config.hpp"
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <functional>
#include <initializer_list>
#include <libslic3r/Model.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include "libslic3r/libslic3r.h"
#include <memory>
#include "slic3r/GUI/ActionRegistry.hpp"
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <vector>
#include <wx/string.h>
#include <wx/utils.h>

class wxWindow;

namespace Slic3r { namespace GUI {

namespace {

// Plate ops are an FFF feature: SLA has a single plate and no plate UI, gcode-only mode has no
// editable project - so gate every plate op on FFF + the normal editor.
bool is_fff_plater(Plater* plater) { return plater && plater->printer_technology() == ptFFF && !plater->only_gcode_mode(); }

AppActionRunResult plate_unavailable() { return {AppActionRunResult::Level::Info, _L("Plates are a filament (FFF) feature.")}; }

// Switch to the Prepare (3D) panel so object/calibration ops have a live canvas + selection, and
// the notebook page label matches. A no-op when the 3D panel is already shown.
void ensure_3d_view(Plater* plater)
{
    if (plater && !plater->is_view3D_shown()) {
        plater->select_view_3D("3D");
        if (MainFrame* mf = wxGetApp().mainframe; mf)
            mf->select_tab(TAB_ID_PREPARE);
    }
}

// Object op guard + run: object ops read the Prepare canvas selection, so ensure that view first so
// a launch from another tab doesn't report a spuriously empty selection.
AppActionRunResult object_op(Plater* plater, bool (*ok)(Plater*), void (*op)(Plater*))
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first.")};
    ensure_3d_view(plater);
    if (!ok(plater))
        return {AppActionRunResult::Level::Info, _L("Select an object first.")};
    op(plater);
    return {AppActionRunResult::Level::Success};
}

// Jump the preview to a layer selected by a 0-100 percent of the layer range. The caller has already
// switched to Preview (which may request a slice); if a slicer result is present the slider is
// repositioned immediately, otherwise the jump is a no-op until the user re-slices.
void go_to_layer(Plater* plater, const std::string& param)
{
    if (!plater)
        return;
    double pct = 50.0;
    try {
        pct = std::stod(param);
    } catch (const std::exception&) {}
    pct = std::clamp(pct, 0.0, 100.0);

    GLCanvas3D* canvas = plater->get_current_canvas3D();
    if (!canvas)
        return;
    GCodeViewer& viewer = canvas->get_gcode_viewer();
    IMSlider* layers    = viewer.get_layers_slider();
    IMSlider* moves     = viewer.get_moves_slider();
    if (!layers || layers->GetMaxValue() <= 0)
        return;

    const double max = double(layers->GetMaxValue());
    const int target = int(std::lround(pct / 100.0 * max));
    layers->SetHigherValue(target);
    if (layers->is_one_layer())
        layers->SetLowerValue(target);
    layers->set_as_dirty();
    if (moves) {
        moves->SetHigherValue(moves->GetMaxValue());
        moves->set_as_dirty();
    }
}

// The path a command was given, checked for reading (`output` false) or writing. nullopt with an
// Error in `fail` when the parameter is unusable.
std::optional<boost::filesystem::path> path_param(const std::string& param, bool output, AppActionRunResult& fail)
{
    std::string err;
    auto        pp = Mcp::parse_path_param(param, err);
    std::optional<boost::filesystem::path> path;
    if (pp)
        path = output ? Mcp::resolve_output_path(pp->path, pp->overwrite, err) : Mcp::resolve_input_path(pp->path, err);
    if (!path)
        fail = {AppActionRunResult::Level::Error, from_u8(err)};
    return path;
}

AppActionRunResult written(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (boost::filesystem::exists(path, ec))
        return {AppActionRunResult::Level::Success, from_u8(path.string())};
    return {AppActionRunResult::Level::Error, _L("The file was not written.")};
}

// Select a named camera view. Plater::select_view dispatches to the current panel.
AppActionRunResult view_command(Plater* plater, const std::string& dir)
{
    if (plater)
        plater->select_view(dir);
    return {AppActionRunResult::Level::Success};
}

// Calibration wizards. Routes through MainFrame::run_calibration, the same entry point as the
// Calibration menu (which caches most of the wizard dialogs).
AppActionRunResult calib_command(CalibKind kind)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first.")};
    ensure_3d_view(wxGetApp().plater());
    mf->run_calibration(kind);
    return {AppActionRunResult::Level::Success};
}

constexpr const char* kCommandPrefix = "orca_command";

// Thin AppAction wrapper for one catalog entry: identity and presentation come from the catalog,
// run() routes back to it. The id is keyed by the stable catalog key (not the display title), so a
// rename or a UI-language switch never re-keys the action.
struct CommandAction : AppAction
{
    std::string command_key;

    AppActionRunResult run(const std::string& param) const override { return NativeCommands::run(command_key, param); }

    explicit CommandAction(const NativeCommand& c)
        : AppAction(AppActionId{AppAction::compose_id(kCommandPrefix, c.key, kOrcaSourceKey)}, I18N::translate_utf8(c.title), kOrcaSourceKey,
                    kOrcaSourceName)
        , command_key(c.key)
    {
        this->kind     = AppActionKind::Command;
        this->group    = I18N::translate_utf8(c.group);
        this->title_en = c.title;
        this->group_en = c.group;
        this->input    = c.input;
        this->icon     = c.icon;
        this->automation_description  = c.description;
        this->param_type              = NativeCommands::param_type_name(c.param.type);
        this->param_help              = c.param.help;
        this->completes_in_background = c.completion == CommandCompletion::Export;
    }
};

// What automation needs to call each command: a description for agents (English, never shown in the
// palette), the meaning of `param`, and whether the effect finishes after run() returns. Commands
// without an entry run immediately and ignore `param`.
void describe_commands(std::vector<NativeCommand>& commands)
{
    struct Detail
    {
        const char*       key;
        const char*       description;
        CommandParamType  param = CommandParamType::None;
        const char*       param_help = "";
        CommandCompletion completion = CommandCompletion::None;
    };
    static const Detail details[] = {
        {"slice_and_preview", "Slice the current plate and show the Preview tab. Returns at once; use the slice tool to wait for the result."},
        {"slice_all", "Slice every plate and show the Preview tab. Returns at once; use the slice tool to wait for results."},
        {"go_to_layer", "Show the Preview tab at a layer chosen as a percentage of the layer range.", CommandParamType::Percent, "0-100"},
        {"go_to_tab", "Switch to a main window tab.", CommandParamType::Tab, "tab id: home, prepare, preview, monitor, project, calibration, or a plugin page id"},
        {"load_project", "Open a project. With a path, opens that file without a dialog (asks first if the current project has unsaved changes).", CommandParamType::Path, "absolute path of a .3mf project"},
        {"save_project", "Save the project to its current file (asks for a name if it has none)."},
        {"save_project_as", "Save the project as a .3mf file. With a path, saves there without a dialog.", CommandParamType::Path, "absolute output path, or {\"path\": ..., \"overwrite\": true}"},
        {"new_project", "Start a new, empty project (asks first if the current one has unsaved changes)."},
        {"undo", "Undo the last change to the project."},
        {"redo", "Redo the last undone change."},
        {"open_preferences", "Open the Preferences dialog."},
        {"mode_simple", "Switch the settings view to Simple mode."},
        {"mode_advanced", "Switch the settings view to Advanced mode."},
        {"mode_expert", "Switch the settings view to Expert mode."},
        {"toggle_developer_mode", "Turn Developer mode on or off."},
        {"export_gcode", "Export the current plate's G-code. With a path, exports there without a dialog once the plate is sliced; the export runs in the background.", CommandParamType::Path, "absolute .gcode path, or {\"path\": ..., \"overwrite\": true}", CommandCompletion::Export},
        {"export_stl", "Export all objects as one STL. With a path, writes there without a dialog.", CommandParamType::Path, "absolute .stl path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_3mf", "Export the model as a .3mf without slicing data. With a path, writes there without a dialog.", CommandParamType::Path, "absolute .3mf path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_sliced_file", "Export the current plate's sliced file (.gcode.3mf). With a path, writes there without a dialog.", CommandParamType::Path, "absolute .3mf path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_all_sliced_file", "Export every plate's sliced file into one .gcode.3mf. With a path, writes there without a dialog.", CommandParamType::Path, "absolute .3mf path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_stl_multi", "Export each object as its own STL. With a directory, writes there without a dialog.", CommandParamType::Path, "absolute path of an existing directory"},
        {"export_drc_single", "Export all objects as one Draco (.drc) file."},
        {"export_drc_multi", "Export each object as its own Draco (.drc) file."},
        {"export_toolpaths_obj", "Export the sliced toolpaths as an OBJ."},
        {"export_config", "Export presets as a bundle (opens a dialog to choose them)."},
        {"import_file", "Import models (3MF/STL/STEP/SVG/OBJ/AMF). With a path, imports that file without a dialog.", CommandParamType::Path, "absolute path of a model file"},
        {"import_zip_archive", "Import models from a ZIP archive (opens a file dialog)."},
        {"import_configs", "Import configuration presets. With a path, loads that .ini/.json without a dialog.", CommandParamType::Path, "absolute path of a config file"},
        {"view_default", "Reset the 3D camera to the default view and fit the plate."},
        {"view_fit_bed", "Fit the build plate in the 3D view."},
        {"view_toggle_perspective", "Switch the 3D camera between perspective and orthographic."},
        {"reset_window_layout", "Restore the default window layout."},
        {"obj_delete", "Delete the selected objects or parts."},
        {"obj_delete_all", "Delete all objects."},
        {"select_all", "Select all objects in the 3D view."},
        {"deselect_all", "Clear the selection."},
        {"obj_mirror_x", "Mirror the selection along X."},
        {"obj_mirror_y", "Mirror the selection along Y."},
        {"obj_mirror_z", "Mirror the selection along Z."},
        {"obj_split_objects", "Split the selected object into separate objects."},
        {"obj_split_parts", "Split the selected object into parts."},
        {"obj_center", "Center the selection on its plate."},
        {"obj_drop", "Drop the selection onto the bed."},
        {"obj_fit_volume", "Scale the selection to fit the print volume."},
        {"obj_instances_up", "Add an instance of the selected object."},
        {"obj_instances_down", "Remove an instance of the selected object."},
        {"obj_arrange", "Arrange all objects on the plates (runs in the background)."},
        {"obj_orient", "Auto-orient the selection (runs in the background)."},
        {"add_primitive_svg", "Add an SVG as a new object, or as a part of the selected object. With a path, uses that file without a dialog; the part is created in the background (wait for idle), and the SVG gizmo opens on it.", CommandParamType::Path, "absolute path of an .svg file"},
        {"plate_add", "Add a plate."},
        {"plate_duplicate", "Duplicate the current plate."},
        {"plate_delete", "Delete the current plate."},
        {"plate_rename", "Rename the current plate (opens a dialog)."},
        {"plate_toggle_lock", "Lock or unlock the current plate."},
        {"plate_goto", "Make a plate the current one.", CommandParamType::Index, "0-based plate index"},
        {"sync_ams", "Synchronize the filament list from the connected printer's AMS."},
        {"preset_bundle", "Open the preset bundle manager."},
        {"sync_presets", "Synchronize user presets with the cloud account."},
        {"help_keyboard_shortcuts", "Show the keyboard shortcuts dialog."},
        {"help_setup_wizard", "Open the setup wizard."},
        {"help_open_config_folder", "Open the configuration folder in the file manager."},
        {"help_migrate_filament_presets", "Migrate legacy user filament presets."},
        {"help_troubleshoot", "Open the troubleshooting center."},
        {"help_network_test", "Open the network test dialog."},
        {"help_benchmark_3d_scene", "Benchmark rendering of the current 3D scene."},
        {"help_tip_of_the_day", "Show a tip of the day."},
        {"help_check_updates", "Check for a newer OrcaSlicer."},
        {"help_about", "Show the About dialog."},
        {"open_wiki", "Open the OrcaSlicer wiki in the browser."},
        {"open_youtube", "Open the OrcaSlicer YouTube channel in the browser."},
        {"open_plugins", "Open the plugins dialog."},
        {"refresh_plugins", "Reload plugins."},
        {"install_plugin", "Open the plugin hub to install a plugin."},
        {"install_local_plugin", "Install a plugin from a local file (opens a file dialog)."},
    };
    for (NativeCommand& c : commands) {
        auto it = std::find_if(std::begin(details), std::end(details), [&](const Detail& d) { return c.key == d.key; });
        if (it != std::end(details)) {
            c.description = it->description;
            c.param       = {it->param, it->param_help};
            c.completion  = it->completion;
        } else if (c.key.rfind("calib_", 0) == 0) {
            c.description = "Open the " + c.title + " wizard.";
        } else if (c.key.rfind("view_", 0) == 0) {
            c.description = "Point the 3D camera: " + c.title + ".";
        } else if (c.key.rfind("add_primitive_", 0) == 0 || c.key.rfind("add_handy_", 0) == 0) {
            c.description = "Add a " + c.title + " object to the current plate.";
        }
    }
}

std::vector<NativeCommand> build_command_catalog()
{
    std::vector<NativeCommand> out;
    // `title` and `group` are the untranslated English msgids, L("...")-marked at each call so xgettext
    // extracts them.
    auto add = [&](std::string key, std::string title, std::string group, std::function<AppActionRunResult(const std::string&)> runner,
                   std::string input = {}, std::string icon = {}) {
        out.push_back({std::move(key), std::move(title), std::move(group), std::move(input), std::move(icon), std::move(runner)});
    };
    // Presentation-first overload: keeps the tile icon next to the title/group it belongs to.
    auto add_with_icon = [&](std::string key, std::string title, std::string group, std::string icon,
                             std::function<AppActionRunResult(const std::string&)> runner, std::string input = {}) {
        add(std::move(key), std::move(title), std::move(group), std::move(runner), std::move(input), std::move(icon));
    };

    // ---- Slice & Export ----
    // Slices through the toolbar's path, so it counts as a user slice request (Plater::request_slice).
    add_with_icon("slice_and_preview", L("Slice and Preview"), L("Slice & Export"), "media_play", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->request_slice(false);
            plater->select_view_3D("Preview", false);
            if (MainFrame* mf = wxGetApp().mainframe; mf)
                mf->select_tab(TAB_ID_PREVIEW);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("slice_all", L("Slice All Plates"), L("Slice & Export"), "media_play", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->request_slice(true);
            if (MainFrame* mf = wxGetApp().mainframe; mf)
                mf->select_tab(TAB_ID_PREVIEW);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    add_with_icon(
        "go_to_layer", L("Go to layer (percent)"), L("Commands"), "height_range_layer",
        [](const std::string& param) {
            Plater* plater = wxGetApp().plater();
            if (plater) {
                plater->select_view_3D("Preview", false);
                if (MainFrame* mf = wxGetApp().mainframe; mf)
                    mf->select_tab(TAB_ID_PREVIEW);
                go_to_layer(plater, param);
            }
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "percent");

    // "go_to_tab" is two-phase: the palette collects the tab after activating it, then hands the tab
    // id back as `param` (same contract as go_to_layer's percent).
    add(
        "go_to_tab", L("Go to tab..."), L("Commands"),
        [](const std::string& param) {
            if (MainFrame* mf = wxGetApp().mainframe; mf && !param.empty())
                mf->select_tab(from_u8(param));
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "tab");

    add_with_icon("load_project", L("Load Project"), L("Commands"), "menu_open", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->load_project();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        plater->load_project(from_path(*path));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("save_project", L("Save Project"), L("Commands"), "menu_save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->save_project(false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("save_project_as", L("Save Project As"), L("Commands"), "menu_save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->save_project(true);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        if (plater->save_project(true, from_path(*path)) != wxID_YES && !boost::filesystem::exists(*path))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The project was not saved.")};
        return written(*path);
    });
    add_with_icon("new_project", L("New Project"), L("Commands"), "menu_open", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->new_project();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("undo", L("Undo"), L("Commands"), "menu_undo", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->undo();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("redo", L("Redo"), L("Commands"), "menu_redo", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->redo();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_preferences", L("Preferences"), L("Commands"), "cog", [](const std::string&) {
        wxGetApp().open_preferences();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Mode ----
    add_with_icon("mode_simple", L("Mode: Simple"), L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comSimple);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_advanced", L("Mode: Advanced"), L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comAdvanced);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_expert", L("Mode: Expert"), L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comExpert);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    // Mirrors Preferences > Developer > Developer mode: flip the flag, persist, refresh the UI.
    add_with_icon("toggle_developer_mode", L("Toggle Developer Mode"), L("Mode"), "advanced", [](const std::string&) {
        GUI_App& app = wxGetApp();
        const bool on = !app.app_config->get_bool("developer_mode");
        app.app_config->set_bool("developer_mode", on);
        app.app_config->save();
        app.update_mode();
        return AppActionRunResult{AppActionRunResult::Level::Success, on ? _L("Developer mode enabled.") : _L("Developer mode disabled.")};
    });

    // ---- Export pipeline ----
    add_with_icon("export_gcode", L("Export G-code"), L("Slice & Export"), "custom-gcode_gcode", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_gcode(false);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        if (plater->is_slicing_any())
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Slicing is in progress. Wait for it to finish and export again.")};
        if (!plater->export_gcode_to(*path))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The export could not start. Slice the plate first and check for slicing errors.")};
        return AppActionRunResult{AppActionRunResult::Level::Info, from_u8(path->string())};
    });
    add_with_icon("export_stl", L("Export STL"), L("Slice & Export"), "save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_stl();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        plater->export_stl(false, false, false, FT_STL, from_path(*path));
        return written(*path);
    });
    add_with_icon("export_3mf", L("Export 3MF"), L("Slice & Export"), "menu_save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_core_3mf();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        plater->export_core_3mf(from_path(*path));
        return written(*path);
    });
    auto export_sliced = [](bool all) {
        return [all](const std::string& param) {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return AppActionRunResult{};
            if (param.empty()) {
                plater->export_gcode_3mf(all);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            }
            AppActionRunResult fail;
            const auto         path = path_param(param, true, fail);
            if (!path)
                return fail;
            if (!plater->export_gcode_3mf(all, *path))
                return AppActionRunResult{AppActionRunResult::Level::Error, _L("The sliced file was not written. Slice first and check for slicing errors.")};
            return written(*path);
        };
    };
    add_with_icon("export_sliced_file", L("Export Sliced File"), L("Slice & Export"), "save", export_sliced(false));
    add_with_icon("export_all_sliced_file", L("Export All Sliced Files"), L("Slice & Export"), "save", export_sliced(true));

    // ---- Calibration ----
    // The tab-strip calib_sf glyph is drawn white for the dark tab bar and vanishes on the palette's
    // light tile, so each wizard borrows the matching settings-group icon instead (gray + accent green).
    add_with_icon("calib_temperature", L("Temperature Calibration"), L("Calibration"), "param_temperature",
        [](const std::string&) { return calib_command(CalibKind::Temperature); });
    add_with_icon("calib_max_volumetric", L("Max Volumetric Speed Calibration"), L("Calibration"), "param_volumetric_speed",
        [](const std::string&) { return calib_command(CalibKind::MaxVolumetric); });
    add_with_icon("calib_pressure_advance", L("Pressure Advance Calibration"), L("Calibration"), "param_flow_ratio_and_pressure_advance",
        [](const std::string&) { return calib_command(CalibKind::PressureAdvance); });
    add_with_icon("calib_flow_ratio", L("Flow Ratio Calibration"), L("Calibration"), "param_flow_ratio_and_pressure_advance",
        [](const std::string&) { return calib_command(CalibKind::FlowRatio); });
    add_with_icon("calib_retraction", L("Retraction Calibration"), L("Calibration"), "param_retraction",
        [](const std::string&) { return calib_command(CalibKind::Retraction); });
    add_with_icon("calib_cornering", L("Cornering Calibration"), L("Calibration"), "param_precision",
        [](const std::string&) { return calib_command(CalibKind::Cornering); });
    add_with_icon("calib_input_shaping_freq", L("Input Shaping Frequency Calibration"), L("Calibration"), "param_resonance_avoidance",
        [](const std::string&) { return calib_command(CalibKind::InputShapingFreq); });
    add_with_icon("calib_input_shaping_damp", L("Input Shaping Damping Calibration"), L("Calibration"), "param_resonance_avoidance",
        [](const std::string&) { return calib_command(CalibKind::InputShapingDamp); });
    add_with_icon("calib_vfa", L("VFA Calibration"), L("Calibration"), "param_speed", [](const std::string&) { return calib_command(CalibKind::VFA); });

    // ---- View ----
    // Titles are L("...")-marked here (not via a variable) so xgettext can extract them.
    for (auto [key, dir, title] :
         std::initializer_list<std::tuple<const char*, const char*, std::string>>{{"view_top", "top", L("View: Top")},
                                                                                  {"view_bottom", "bottom", L("View: Bottom")},
                                                                                  {"view_front", "front", L("View: Front")},
                                                                                  {"view_rear", "rear", L("View: Rear")},
                                                                                  {"view_left", "left", L("View: Left")},
                                                                                  {"view_right", "right", L("View: Right")},
                                                                                  {"view_iso", "iso", L("View: Isometric")}}) {
        std::string k = key, d = dir;
        add(k, title, L("View"),
            [d](const std::string&) { return view_command(wxGetApp().plater(), d); });
    }
    add("view_default", L("View: Default"), L("View"), [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->select_view("plate");
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_fit_bed", L("Fit Bed to View"), L("View"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_toggle_perspective", L("Toggle Perspective"), L("View"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->get_camera().select_next_type();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("reset_window_layout", L("Reset Window Layout"), L("View"), "toolbar_reset", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->reset_window_layout();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Object ----
    add_with_icon("obj_delete", L("Delete Selected"), L("Object"), "delete", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->remove_selected(); });
    });
    add_with_icon("obj_delete_all", L("Delete All Objects"), L("Object"), "delete", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_delete_all(); }, [](Plater* p) { p->delete_all_objects_from_model(); });
    });
    add("select_all", L("Select All Objects"), L("Object"), [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        ensure_3d_view(plater);
        plater->select_all();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("deselect_all", L("Deselect All"), L("Object"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->deselect_all();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("obj_mirror_x", L("Mirror X"), L("Object"), "menu_mirror_x", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::X); });
    });
    add_with_icon("obj_mirror_y", L("Mirror Y"), L("Object"), "menu_mirror_y", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Y); });
    });
    add_with_icon("obj_mirror_z", L("Mirror Z"), L("Object"), "menu_mirror_z", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Z); });
    });
    add_with_icon("obj_split_objects", L("Split to Objects"), L("Object"), "menu_split_objects", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_objects(); }, [](Plater* p) { p->split_object(true); });
    });
    add_with_icon("obj_split_parts", L("Split to Parts"), L("Object"), "menu_split_parts", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_volumes(); }, [](Plater* p) { p->split_volume(); });
    });
    add("obj_center", L("Center Selected on Plate"), L("Object"), [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->center_selection(); });
    });
    add_with_icon("obj_drop", L("Drop to Bed"), L("Object"), "toolbar_flatten", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->get_selection().can_drop(); }, [](Plater* p) { p->drop_selection(); });
    });
    add("obj_fit_volume", L("Scale to Fit Print Volume"), L("Object"), [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_scale_to_print_volume(); },
            [](Plater* p) { p->scale_selection_to_fit_print_volume(); });
    });
    add_with_icon("obj_instances_up", L("Increase Instances"), L("Object"), "instance_add", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_increase_instances(); }, [](Plater* p) { p->increase_instances(); });
    });
    add_with_icon("obj_instances_down", L("Decrease Instances"), L("Object"), "instance_remove", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_decrease_instances(); }, [](Plater* p) { p->decrease_instances(); });
    });
    add_with_icon("obj_arrange", L("Auto-Arrange"), L("Object"), "toolbar_arrange", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_arrange(); }, [](Plater* p) { p->arrange(); });
    });
    add_with_icon("obj_orient", L("Auto-Orient"), L("Object"), "toolbar_orient", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_arrange(); }, [](Plater* p) { p->orient(); });
    });

    // ---- Add Primitive ---- (the Add > Add Primitive submenu; creates a new object)
    auto add_primitive = [&](std::string key, std::string title, std::string icon, const char* type_name) {
        add_with_icon(std::move(key), std::move(title), L("Add Primitive"), std::move(icon), [type_name](const std::string&) {
            Plater* plater = wxGetApp().plater();
            if (plater) {
                ensure_3d_view(plater);
                if (ObjectList* list = wxGetApp().obj_list())
                    list->load_generic_subobject(type_name, ModelVolumeType::INVALID);
            }
            return AppActionRunResult{AppActionRunResult::Level::Success};
        });
    };
    add_primitive("add_primitive_cube", L("Cube"), "menu_obj_cube", "Cube");
    add_primitive("add_primitive_cylinder", L("Cylinder"), "menu_obj_cylinder", "Cylinder");
    add_primitive("add_primitive_sphere", L("Sphere"), "menu_obj_sphere", "Sphere");
    add_primitive("add_primitive_cone", L("Cone"), "menu_obj_cone", "Cone");
    add_primitive("add_primitive_disc", L("Disc"), "menu_obj_disc", "Disc");
    add_primitive("add_primitive_torus", L("Torus"), "menu_obj_torus", "Torus");
    add_with_icon("add_primitive_text", L("Text"), L("Add Primitive"), "menu_obj_text", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            ensure_3d_view(plater);
            if (GLCanvas3D* canvas = plater->canvas3D())
                canvas->clear_popup_menu_position();
            MenuFactory::add_text_volume(ModelVolumeType::INVALID);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("add_primitive_svg", L("SVG"), L("Add Primitive"), "menu_obj_svg", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        ensure_3d_view(plater);
        GLCanvas3D* canvas = plater->canvas3D();
        if (canvas != nullptr)
            canvas->clear_popup_menu_position();
        if (param.empty()) {
            MenuFactory::add_svg_volume(ModelVolumeType::INVALID);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        // What the dialog path does once a file is picked: a new object with nothing selected, else
        // a part of the selected object.
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        auto* svg = canvas != nullptr ? dynamic_cast<GLGizmoSVG*>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg)) : nullptr;
        if (svg == nullptr || !svg->create_volume(into_u8(from_path(*path)), ModelVolumeType::MODEL_PART))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The SVG file could not be added.")};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Add Handy models ---- (the Add > Add Handy models submenu)
    const std::vector<MenuFactory::HandyModel>& handy = MenuFactory::handy_models();
    for (std::size_t i = 0; i < handy.size(); ++i) {
        add("add_handy_" + std::string(handy[i].key), handy[i].label, L("Add Handy models"),
            [i](const std::string&) {
                if (Plater* plater = wxGetApp().plater())
                    ensure_3d_view(plater);
                MenuFactory::load_handy_model(i);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            });
    }

    // ---- Plate ----
    add_with_icon("plate_add", L("Add Plate"), L("Plate"), "toolbar_add_plate", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot add another plate (maximum reached).")};
        plater->add_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_duplicate", L("Duplicate Plate"), L("Plate"), "menu_copy", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot duplicate a plate (maximum reached).")};
        plater->duplicate_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_delete", L("Delete Plate"), L("Plate"), "delete", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_delete_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot delete the only plate.")};
        plater->delete_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_rename", L("Rename Plate"), L("Plate"), "plate_name_edit", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlate* curr = plater->get_partplate_list().get_curr_plate();
        PlateNameEditDialog dlg((wxWindow*) wxGetApp().mainframe, wxID_ANY, _L("Edit Plate Name"));
        dlg.set_plate_name(from_u8(curr->get_plate_name()));
        if (dlg.ShowModal() == wxID_YES)
            plater->rename_plate(plater->get_partplate_list().get_curr_plate_index(), dlg.get_plate_name().ToUTF8().data());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_toggle_lock", L("Toggle Plate Lock"), L("Plate"), "lock_normal", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int index       = plates.get_curr_plate_index();
        plater->take_snapshot("lock partplate");
        plates.lock_plate(index, !plates.is_locked(index));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_goto", L("Go to Plate"), L("Plate"), "go_next_plate", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int count       = plates.get_plate_count();
        if (count <= 0)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("No plates available.")};
        int index = 0;
        try {
            index = std::stoi(param);
        } catch (const std::exception&) {}
        index = std::clamp(index, 0, count - 1);
        plater->select_plate(index, false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Printer / device connection ----
    add_with_icon("sync_ams", L("Synchronize Filament List from AMS"), L("Printer"), "ams_fila_sync", [](const std::string&) {
        Plater* plater     = wxGetApp().plater();
        DeviceManager* dev = wxGetApp().getDeviceManager();
        if (dev && dev->get_selected_machine() && plater) {
            plater->sidebar().sync_ams_list();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("Connect a printer to synchronize the AMS filament list.")};
    });

    // ---- Presets / cloud ----
    add_with_icon("preset_bundle", L("Open Preset Bundle"), L("Presets"), "menu_edit_preset", [](const std::string&) {
        wxGetApp().open_presetbundledialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("sync_presets", L("Sync Presets"), L("Presets"), "printer_sync_ok", [](const std::string&) {
        if (!wxGetApp().is_user_login())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Sign in to sync presets.")};
        wxGetApp().restart_sync_user_preset();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Import ----
    add_with_icon("import_file", L("Import 3MF/STL/STEP/SVG/OBJ/AMF"), L("Import"), "menu_open", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
#ifdef __APPLE__
            plater->add_model();
#else
            plater->add_file();
#endif
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        if (plater->load_files(std::vector<boost::filesystem::path>{*path}).empty())
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Nothing was imported.")};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_zip_archive", L("Import ZIP Archive"), L("Import"), "menu_open", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->import_zip_archive();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_configs", L("Import Configs"), L("Import"), "menu_open", [](const std::string& param) {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            mf->load_config_file();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        if (!mf->load_config_file(path->string()))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The configuration was not loaded.")};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Export extras ----
    add_with_icon("export_stl_multi", L("Export All Objects as STLs"), L("Export"), "save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_stl(false, false, true);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        std::string err;
        auto        pp  = Mcp::parse_path_param(param, err);
        auto        dir = pp ? Mcp::resolve_output_dir(pp->path, err) : std::nullopt;
        if (!dir)
            return AppActionRunResult{AppActionRunResult::Level::Error, from_u8(err)};
        plater->export_stl(false, false, true, FT_STL, from_path(*dir));
        return AppActionRunResult{AppActionRunResult::Level::Success, from_u8(dir->string())};
    });
    add_with_icon("export_drc_single", L("Export All Objects as DRC (one file)"), L("Export"), "save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, false, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_drc_multi", L("Export All Objects as DRCs"), L("Export"), "save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, true, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_toolpaths_obj", L("Export Toolpaths as OBJ"), L("Export"), "custom-gcode_gcode", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_toolpaths_to_obj();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_config", L("Export Preset Bundle"), L("Export"), "save", [](const std::string&) {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->export_config();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Help ---- (mirrors the top-bar Help menu, plus the wiki/YouTube links)
    add("help_keyboard_shortcuts", L("Keyboard Shortcuts"), L("Help"), [](const std::string&) {
        wxGetApp().keyboard_shortcuts(ShortcutContext::Global);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_setup_wizard", L("Setup Wizard"), L("Help"), [](const std::string&) {
        wxGetApp().ShowUserGuide();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_open_config_folder", L("Show Configuration Folder"), L("Help"), "open_project", [](const std::string&) {
        Slic3r::GUI::desktop_open_datadir_folder();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_migrate_filament_presets", L("Migrate Filament Presets"), L("Help"), [](const std::string&) {
        wxGetApp().check_legacy_user_filament_presets(true);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_troubleshoot", L("Troubleshoot Center"), L("Help"), [](const std::string&) {
        wxGetApp().troubleshoot();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_network_test", L("Open Network Test"), L("Help"), [](const std::string&) {
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_benchmark_3d_scene", L("Benchmark 3D Scene"), L("Help"), [](const std::string&) {
        run_scene_benchmark();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_tip_of_the_day", L("Show Tip of the Day"), L("Help"), "info", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater()) {
            plater->get_dailytips()->open();
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->set_as_dirty();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_check_updates", L("Check for Updates"), L("Help"), "refresh", [](const std::string&) {
        wxGetApp().check_new_version_sf(true, 1);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_about", L("About OrcaSlicer"), L("Help"), "OrcaSlicer_gradient_circle", [](const std::string&) {
        Slic3r::GUI::about();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_wiki", L("Open Wiki"), L("Help"), "link_wiki_img", [](const std::string&) {
        wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("open_youtube", L("Open YouTube Channel"), L("Help"), [](const std::string&) {
        wxLaunchDefaultBrowser("https://www.youtube.com/@OfficialOrcaSlicer/videos", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Plugins ----
    add("open_plugins", L("Open Plugins"), L("Plugins"), [](const std::string&) {
        wxGetApp().open_plugins_dialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("refresh_plugins", L("Refresh Plugins"), L("Plugins"), [](const std::string&) {
        wxGetApp().refresh_plugins();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_plugin", L("Install Plugin"), L("Plugins"), [](const std::string&) {
        open_plugin_hub();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_local_plugin", L("Install Local Plugin"), L("Plugins"), [](const std::string&) {
        wxGetApp().install_local_plugin();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    describe_commands(out);
    return out;
}

} // namespace

const std::vector<NativeCommand>& NativeCommands::catalog()
{
    static const std::vector<NativeCommand> commands = build_command_catalog();
    return commands;
}

std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand& command)
{
    return std::make_unique<CommandAction>(command);
}

const NativeCommand* NativeCommands::find(const std::string& key)
{
    for (const NativeCommand& c : catalog())
        if (c.key == key)
            return &c;
    return nullptr;
}

const char* NativeCommands::param_type_name(CommandParamType type)
{
    switch (type) {
    case CommandParamType::Percent: return "percent";
    case CommandParamType::Tab: return "tab";
    case CommandParamType::Path: return "path";
    case CommandParamType::Index: return "index";
    case CommandParamType::Text: return "text";
    default: return "none";
    }
}

AppActionRunResult NativeCommands::run(const std::string& key, const std::string& param)
{
    GUI_App& app = wxGetApp();
    if (app.is_closing())
        return {};
    for (const NativeCommand& c : catalog())
        if (c.key == key)
            return c.runner(param);
    return {AppActionRunResult::Level::Info, _L("Unknown command.")};
}

}} // namespace Slic3r::GUI
