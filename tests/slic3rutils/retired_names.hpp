#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

// Names of the retired vocabulary of the commands outside the Design tab, which the catalog's cases check are no
// command: a retired name has no alias.

// The old names of the 42 tools that are named place first, then verb (the design's Mapping), and of the three that
// are split: open_files into project_open and project_import, get_settings and set_settings into the settings,
// preferences, plate and project commands.
inline const std::vector<std::string> kRetiredTools{
    "search_tools",        "call_tool",          "generate_calibration_test", "export",
    "list_presets",        "select_preset",      "save_preset",               "delete_preset",
    "list_plates",         "list_objects",       "transform",                 "add_part",
    "arrange",             "slice",              "set_height_ranges",         "set_variable_layer_height",
    "set_custom_gcode_at_layer", "get_slice_stats", "get_app_state",          "wait",
    "get_log",             "cancel_job",         "run_replay",                "export_replay",
    "run_script",          "search_python_api",  "answer_dialog",             "auto_answer_dialogs",
    "select_objects",      "screenshot",         "set_view",                  "encode_recording",
    "search_actions",      "run_action",         "list_windows",              "set_window",
    "list_widgets",        "click",              "set_widget_value",          "press_keys",
    "pick_gizmo_file",     "run_javascript",     "open_files",                "get_settings",
    "set_settings"};

// The ids of the menu rows that are a tile of a command of another name (the design's Mapping), each the key of
// its tile: those of the project, the objects, the parts and the plates, then those of the presets, the app, the view
// and the rest.
inline const std::vector<std::string> kRetiredRows{
    "new_project",              "load_project",               "import_file",                   "import_zip_archive",
    "save_project",             "save_project_as",            "export_gcode",                  "export_sliced_file",
    "export_all_sliced_file",   "export_3mf",                 "export_stl",                    "export_stl_multi",
    "export_drc_single",        "export_drc_multi",           "export_toolpaths_obj",          "select_all",
    "deselect_all",             "add_primitive_cube",         "add_primitive_cylinder",        "add_primitive_sphere",
    "add_primitive_cone",       "add_primitive_disc",         "add_primitive_torus",           "add_handy_orca_cube",
    "add_handy_orcasliced_combo", "add_handy_orca_badge",     "add_handy_orca_tolerance_test", "add_handy_3dbenchy",
    "add_handy_cali_cat",       "add_handy_autodesk_fdm_test", "add_handy_voron_cube",         "add_handy_stanford_bunny",
    "add_handy_orca_string_hell", "add_primitive_text",       "add_primitive_svg",             "obj_delete",
    "obj_delete_all",           "obj_rename",                 "obj_set_instances",             "obj_instances_up",
    "obj_instances_down",       "obj_instances_to_objects",   "obj_split_objects",             "obj_split_parts",
    "obj_assemble",             "obj_type_part",              "obj_type_negative",             "obj_type_modifier",
    "obj_type_support_blocker", "obj_type_support_enforcer",  "obj_type_precise_seam",         "obj_order_by_name",
    "obj_order_by_filament",    "obj_order_by_height",        "obj_order_by_size",             "obj_drop",
    "obj_mirror_x",             "obj_mirror_y",               "obj_mirror_z",                  "obj_center",
    "obj_fit_volume",           "obj_arrange",                "obj_orient",                    "plate_toggle_lock",
    "plate_goto",               "slice_and_preview",          "slice_all",                     "calib_temperature",
    "calib_flow_ratio",         "calib_pressure_advance",     "calib_retraction",              "calib_max_volumetric",
    "calib_vfa",                "calib_cornering",            "calib_input_shaping_freq",      "calib_input_shaping_damp",
    "import_configs",           "export_config",              "sync_presets",                  "help_migrate_filament_presets",
    "mode_simple",              "mode_advanced",              "mode_expert",                   "toggle_developer_mode",
    "open_wiki",                "open_youtube",               "install_plugin",                "help_check_updates",
    "help_open_config_folder",  "help_benchmark_3d_scene",    "view_top",                      "view_bottom",
    "view_front",               "view_rear",                  "view_left",                     "view_right",
    "view_iso",                 "view_default",               "view_fit_bed",                  "view_toggle_perspective",
    "go_to_tab",                "go_to_layer",                "reset_window_layout",           "open_preferences",
    "open_plugins",             "open_action_replay",         "preset_bundle",                 "help_keyboard_shortcuts",
    "help_setup_wizard",        "help_network_test",          "help_troubleshoot",             "help_tip_of_the_day",
    "help_about",               "take_screenshot",            "sync_ams",                      "send_gcode",
    "install_local_plugin",     "refresh_plugins",            "start_recording",               "stop_recording",
    "toggle_run_script_panel"};

// The names of kRetiredTools and kRetiredRows that are words of `text`, as a text an agent reads would name a command:
// a word is a run of letters, digits, underscores and hyphens, compared in its case, so project_export holds no export
// and a right-click menu no click.
inline std::set<std::string> retired_names_in(const std::string& text)
{
    std::set<std::string> found;
    std::string           word;
    const auto            flush = [&found, &word] {
        const auto named = [&word](const std::vector<std::string>& names) {
            return std::find(names.begin(), names.end(), word) != names.end();
        };
        if (named(kRetiredTools) || named(kRetiredRows))
            found.insert(word);
        word.clear();
    };
    for (const char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')
            word += ch;
        else
            flush();
    }
    flush();
    return found;
}
