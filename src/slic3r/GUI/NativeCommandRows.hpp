#pragma once

#include "ActionRegistry.hpp" // for AppActionRunResult

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

enum class CalibKind : int;

// The functions of the Speed Dial's menu rows that are tiles of a command: each does what its row does, as a
// person's pick of the row does it: on what is selected in the Prepare view, on the current plate, or on the app and
// its windows. A command made from rows calls the function of the row its variant argument names, and a typed tool
// calls one when a call's arguments are that row's tile's (Mcp::person_form()), or, where the row does what the tool
// does (Auto-Arrange), for a person's pick of the tile alone (NativeCommands::with_row_forms()). They are defined in
// NativeCommands.cpp, beside the helpers the rows share. UI thread.
namespace Rows {

// ---- The project ----
AppActionRunResult load_project();
AppActionRunResult save_project_as();
AppActionRunResult import_file();
AppActionRunResult import_zip_archive();
AppActionRunResult export_gcode();
AppActionRunResult export_stl();
AppActionRunResult export_3mf();
// The current plate's sliced file, or with `all_plates` every plate's in one file.
AppActionRunResult export_sliced_file(bool all_plates);
// Every object as its own STL file, into the folder a dialog asks for; a project with no objects is refused.
AppActionRunResult export_stl_multi();
AppActionRunResult export_drc_single();
// Every object as its own Draco file, into the folder a dialog asks for; a project with no objects is refused.
AppActionRunResult export_drc_multi();
// The toolpaths the Preview shows as an OBJ, into the file a dialog asks for; refused while it shows none.
AppActionRunResult export_toolpaths_obj();

// ---- Objects and parts ----
// Handy model `index` of MenuFactory::handy_models().
AppActionRunResult add_handy(std::size_t index);
AppActionRunResult add_primitive_text();
// An SVG as a new object, or a part of the selected object: with no arguments from the file a dialog asks for,
// else from the file `args` names ({"path"}).
AppActionRunResult add_primitive_svg(const nlohmann::json& args);
AppActionRunResult obj_arrange();

// ---- Plates ----
AppActionRunResult plate_add();
// The calibration test's dialog, as the Calibration menu opens it.
AppActionRunResult calib(CalibKind kind);

// ---- Presets and settings ----
// With no arguments the Import Configs file dialog, else the file `args` names ({"path", "on_conflict"?}): a preset
// .json, a .zip of them, an .orca_printer, .orca_filament or .orca_bundle, or a .gcode, whose settings it loads.
AppActionRunResult import_configs(const nlohmann::json& args);
// The Export Preset Bundle dialog, whose OK writes into the existing folder `args` names ({"path"}) when it names one.
AppActionRunResult export_config(const nlohmann::json& args);
AppActionRunResult migrate_filament_presets();
// The settings mode `mode` names: simple, advanced or expert.
AppActionRunResult set_mode(const std::string& mode);
AppActionRunResult toggle_developer_mode();

// ---- The app ----
// Undo, or with `redo` Redo, of an automated call: on the history `args` names ({"history"?}: design or project), else
// the shown tab's. A refusal is thrown as ToolFailure.
AppActionRunResult undo_redo(const nlohmann::json& args, bool redo);
// A person's pick of Undo, or Redo: what the Edit menu's item does, on the history `args` names or else the shown
// tab's; nothing where the item would be greyed.
AppActionRunResult picked_undo_redo(const nlohmann::json& args, bool redo);
// The web page `page` names in the default browser: wiki, youtube or plugin_hub.
AppActionRunResult open_web_page(const std::string& page);
AppActionRunResult check_for_updates();
AppActionRunResult show_config_folder();
AppActionRunResult benchmark_3d_scene();

// ---- View and windows ----
AppActionRunResult toggle_perspective();
AppActionRunResult reset_window_layout();
// The window or dialog `window` names: preferences (on the tab and at the key `args` names, {"tab"?, "key"?}), plugins,
// action_replay, preset_bundle, keyboard_shortcuts, setup_wizard, network_test, troubleshoot, tip_of_the_day or about.
AppActionRunResult open_window(const std::string& window, const nlohmann::json& args);
// The names open_window()'s tab gives the Preferences dialog's tabs, in the dialog's order.
std::vector<std::string> preferences_tabs();
AppActionRunResult take_screenshot();

// ---- Filaments, printers and plugins ----
AppActionRunResult sync_ams();
AppActionRunResult send_gcode();
// With no arguments the plugin file a dialog asks for, else the file `args` names ({"path"}).
AppActionRunResult install_local_plugin(const nlohmann::json& args);
AppActionRunResult refresh_plugins();

// ---- Scripts ----
AppActionRunResult toggle_run_script_panel();

// The schema of the argument of a command that opens or imports a file, in place of its file dialog: the absolute path
// of `what`.
nlohmann::json input_file(const char* what);

} // namespace Rows

} // namespace GUI
} // namespace Slic3r
