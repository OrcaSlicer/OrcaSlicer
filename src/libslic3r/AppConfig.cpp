#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "AppConfig.hpp"
//BBS
#include "Preset.hpp"
#include "Exception.hpp"
#include "LocalesUtils.hpp"
#include "Thread.hpp"
#include "format.hpp"
#include "nlohmann/json.hpp"

#include <utility>
#include <vector>
#include <stdexcept>

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/format/format_fwd.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#ifdef WIN32
//FIXME replace the two following includes with <boost/md5.hpp> after it becomes mainstream.
#include <boost/uuid/detail/md5.hpp>
#include <boost/algorithm/hex.hpp>
#endif

#define USE_JSON_CONFIG

using namespace nlohmann;

namespace Slic3r {

static const std::string VERSION_CHECK_URL = "https://check-version.orcaslicer.com/latest";
static const std::string PROFILE_UPDATE_URL = "https://api.github.com/repos/OrcaSlicer/orcaslicer-profiles/releases/tags";
static const std::string MODELS_STR = "models";

const std::string AppConfig::SECTION_FILAMENTS = "filaments";
const std::string AppConfig::SECTION_MATERIALS = "sla_materials";
const std::string AppConfig::SECTION_EMBOSS_STYLE = "font";

std::string AppConfig::get_language_code()
{
    std::string get_lang = get("language");
    if (get_lang.empty()) return "";

    if (get_lang == "zh_CN")
    {
        get_lang = "zh-cn";
    }
    else
    {
        if (get_lang.length() >= 2) { get_lang = get_lang.substr(0, 2); }
    }

    return get_lang;
}

std::string AppConfig::get_hms_host()
{
    std::string sel = get("iot_environment");
    std::string host = "";
// #if !BBL_RELEASE_TO_PUBLIC
//     if (sel == ENV_DEV_HOST)
//         host = "e-dev.bambu-lab.com";
//     else if (sel == ENV_QAT_HOST)
//         host = "e-qa.bambu-lab.com";
//     else if (sel == ENV_PRE_HOST)
//         host = "e-pre.bambu-lab.com";
//     else if (sel == ENV_PRODUCT_HOST)
//         host = "e.bambulab.com";
//     return host;
// #else
    return "e.bambulab.com";
// #endif
}

bool AppConfig::get_stealth_mode()
{
    // always return true when user did not finish setup wizard yet
    if (!get_bool("firstguide","finish")) {
        return true;
    }
    return get_bool("stealth_mode");
}

void AppConfig::reset()
{
    m_storage.clear();
    set_defaults();
};

// Override missing or keys with their defaults.
void AppConfig::set_defaults()
{
    if (m_mode == EAppMode::Editor) {
#ifdef SUPPORT_AUTO_CENTER
        // Reset the empty fields to defaults.
        if (get("autocenter").empty())
            set_bool("autocenter", true);
#endif

#ifdef SUPPORT_BACKGROUND_PROCESSING
        // Disable background processing by default as it is not stable.
        if (get("background_processing").empty())
            set_bool("background_processing", false);
#endif
        if (get("auto_slice_after_change").empty())
            set_bool("auto_slice_after_change", false);

        if (get("auto_slice_change_delay_seconds").empty())
            set("auto_slice_change_delay_seconds", "1");

        if (get("drop_project_action").empty())
            set_bool("drop_project_action", true);

#ifdef _WIN32
        if (get("associate_3mf").empty())
            set_bool("associate_3mf", false);
        if (get("associate_stl").empty())
            set_bool("associate_stl", false);
        if (get("associate_step").empty())
            set_bool("associate_step", false);

#endif // _WIN32

        // remove old 'use_legacy_opengl' parameter from this config, if present
        if (!get("use_legacy_opengl").empty())
            erase("app", "use_legacy_opengl");

#ifdef __APPLE__
        if (get("use_retina_opengl").empty())
            set_bool("use_retina_opengl", true);
#endif

        if (get("single_instance").empty())
            set_bool("single_instance", false);

#ifdef SUPPORT_REMEMBER_OUTPUT_PATH
        if (get("remember_output_path").empty())
            set_bool("remember_output_path", true);

        if (get("remember_output_path_removable").empty())
            set_bool("remember_output_path_removable", true);
#endif
        if (get("toolkit_size").empty())
            set("toolkit_size", "100");

#if ENABLE_ENVIRONMENT_MAP
        if (get("use_environment_map").empty())
            set("use_environment_map", false);
#endif // ENABLE_ENVIRONMENT_MAP

        if (get("use_inches").empty())
            set("use_inches", "0");

        if (get("default_page").empty())
            set("default_page", "0");
    }
    else {
#ifdef _WIN32
        if (get("associate_gcode").empty())
            set_bool("associate_gcode", false);
#endif // _WIN32
    }

    if (get("use_perspective_camera").empty())
        set_bool("use_perspective_camera", true);

    if (get("auto_perspective").empty())
        set_bool("auto_perspective", false);

    if (get("use_free_camera").empty())
        set_bool("use_free_camera", false);

    if (get("camera_navigation_style").empty())
        set("camera_navigation_style", "0");

    if (get("swap_mouse_buttons").empty())
        set_bool("swap_mouse_buttons", false);

    if (get("reverse_mouse_wheel_zoom").empty())
        set_bool("reverse_mouse_wheel_zoom", false);

    if (get("enable_append_color_by_sync_ams").empty())
        set_bool("enable_append_color_by_sync_ams", true);
    if (get("enable_merge_color_by_sync_ams").empty())
        set_bool("enable_merge_color_by_sync_ams", false);
    if (get("ams_sync_match_full_use_color_dist").empty())
        set_bool("ams_sync_match_full_use_color_dist", false);

    if (get("camera_orbit_mult").empty())
        set("camera_orbit_mult", "1.0");

    if (get("export_sources_full_pathnames").empty())
        set_bool("export_sources_full_pathnames", false);

    if (get("zoom_to_mouse").empty())
        set_bool("zoom_to_mouse", false);

//#ifdef SUPPORT_SHOW_HINTS
    if (get("show_hints").empty())
        set_bool("show_hints", false);
//#endif
    if (get("enable_multi_machine").empty())
        set_bool("enable_multi_machine", false);

    if (get("show_gcode_window").empty())
        set_bool("show_gcode_window", true);

    if (get("show_3d_navigator").empty())
        set_bool("show_3d_navigator", true);

    if (get("show_outline").empty())
        set_bool("show_outline", false);
    
    if (get("show_axes").empty())
        set_bool("show_axes", true);

#ifdef _WIN32

//#ifdef SUPPORT_3D_CONNEXION
    if (get("use_legacy_3DConnexion").empty())
        set_bool("use_legacy_3DConnexion", true);
//#endif

#ifdef SUPPORT_DARK_MODE
    if (get("dark_color_mode").empty())
        set("dark_color_mode", "0");
#endif

//#ifdef SUPPORT_SYS_MENU
    if (get("sys_menu_enabled").empty())
        set("sys_menu_enabled", "1");
//#endif
#endif // _WIN32

    // BBS
    /*if (get("3mf_include_gcode").empty())
        set_bool("3mf_include_gcode", true);*/

    if (get("developer_mode").empty())
        set_bool("developer_mode", false);

    if (get("enable_ssl_for_mqtt").empty())
        set_bool("enable_ssl_for_mqtt", true);

    if (get("enable_ssl_for_ftp").empty())
        set_bool("enable_ssl_for_ftp", true);

    if (get("log_severity_level").empty())
        set("log_severity_level", "warning");

    if (get("internal_developer_mode").empty())
        set_bool("internal_developer_mode", false);

    // BBS
    if (get("preset_folder").empty())
        set("preset_folder", "");

    // BBS
    if (get("slicer_uuid").empty()) {
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        set("slicer_uuid", to_string(uuid));
    }

    // Orca
    if (get("stealth_mode").empty()) {
        set_bool("stealth_mode", false);
    }
    if (get("allow_abnormal_storage").empty()) {
        set_bool("allow_abnormal_storage", false);
    }
    if (get("legacy_networking").empty()) {
        set_bool("legacy_networking", false);
    }

    if(get("check_stable_update_only").empty()) {
        set_bool("check_stable_update_only", false);
    }

    // Orca
    if(get("show_splash_screen").empty()) {
        set_bool("show_splash_screen", true);
    }

    if(get("auto_arrange").empty()) {
        set_bool("auto_arrange", true);
    }

    if (get("show_model_mesh").empty()) {
        set_bool("show_model_mesh", false);
    }

    if (get("show_model_shadow").empty()) {
        set_bool("show_model_shadow", true);
    }

    if (get("show_build_edges").empty()) {
        set_bool("show_build_edgets", false);
    }

    if (get("show_daily_tips").empty()) {
        set_bool("show_daily_tips", true);
    }

    if (get("auto_calculate_flush").empty()){
        set("auto_calculate_flush","all");
    }

    if (get("show_canvas_zoom_button").empty()) {
        set_bool("show_canvas_zoom_button", true);
    }

    if (get("remember_printer_config").empty()) {
        set_bool("remember_printer_config", true);
    }

    if (get("group_filament_presets").empty()) {
        set("group_filament_presets", "1"); // All "0" / None "1" / By Type "2" / By Vendor "3"
    }

    if (get("enable_high_low_temp_mixed_printing").empty()){
        set_bool("enable_high_low_temp_mixed_printing", false);
    }

    if (get("ignore_ext_filament_in_filament_map").empty()){
        set_bool("ignore_ext_filament_in_filament_map", false);
    }

    if (get("pop_up_filament_map_dialog").empty()){
        set_bool("pop_up_filament_map_dialog", false);
    }

    if (get("prefered_filament_map_mode").empty()){
        set("prefered_filament_map_mode",ConfigOptionEnum<FilamentMapMode>::get_enum_names()[FilamentMapMode::fmmAutoForFlush]);
    }

    if (get("show_home_page").empty()) {
        set_bool("show_home_page", true);
    }

    if (get("show_printable_box").empty()) {
        set_bool("show_printable_box", true);
    }

    if (get("units").empty()) {
         set("units", "0");
    }

    if (get("sync_user_preset").empty()) {
        set_bool("sync_user_preset", false);
    }

    if (get("keyboard_supported").empty()) {
        set("keyboard_supported", std::string("none/alt/control/shift"));
    }

    if (get("mouse_supported").empty()) {
        set("mouse_supported", "mouse left/mouse middle/mouse right");
    }

    if (get("privacy_version").empty()) {
        set("privacy_version", "00.00.00.00");
    }

    if (get("rotate_view").empty()) {
        set("rotate_view", "none/mouse left");
    }

    if (get("move_view").empty()) {
        set("move_view", "none/mouse left");
    }

    if (get("zoom_view").empty()) {
        set("zoom_view", "none/mouse left");
    }

    if (get("precise_control").empty()) {
        set("precise_control", "none/mouse left");
    }

    if (get("download_path").empty()) {
        set("download_path", "");
    }

    if (get("mouse_wheel").empty()) {
        set("mouse_wheel", "0");
    }

    if (get(SETTING_PROJECT_LOAD_BEHAVIOUR).empty()) {
        set(SETTING_PROJECT_LOAD_BEHAVIOUR, OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT);
    }

    if (get("max_recent_count").empty()) {
        set("max_recent_count", "18");
    }

    if (get("recent_models").empty()) {
        set("recent_models", "0");
    }

    // if (get("staff_pick_switch").empty()) {
    //     set_bool("staff_pick_switch", false);
    // }

    if (get("sync_system_preset").empty()) {
        set_bool("sync_system_preset", true);
    }

    if (get("backup_switch").empty() || get("version") < "01.06.00.00") {
        set_bool("backup_switch", true);
    }

    if (get("backup_interval").empty()) {
        set("backup_interval", "10");
    }

    if (get("curr_bed_type").empty()) {
        set("curr_bed_type", "1");
    }

    if (get("sending_interval").empty()) {
        set("sending_interval", "5");
    }

    if (get("max_send").empty()) {
        set("max_send", "3");
    }

// #if BBL_RELEASE_TO_PUBLIC
    if (get("iot_environment").empty()) {
        set("iot_environment", "3");
    }
// #else
//     if (get("iot_environment").empty()) {
//         set("iot_environment", "1");
//     }
// #endif

    if (get("allow_ip_resolve").empty())
        set_bool("allow_ip_resolve", true);

    if (get("presets", "filament_colors").empty()) {
        set_str("presets", "filament_colors", "#F2754E");
    }

    if (get("print", "bed_leveling").empty()) {
        set_str("print", "bed_leveling", "1");
    }
    if (get("print", "flow_cali").empty()) {
        set_str("print", "flow_cali", "1");
    }
    if (get("print", "timelapse").empty()) {
        set_str("print", "timelapse", "1");
    }

    if (get("enable_step_mesh_setting").empty()) {
        set_bool("enable_step_mesh_setting", true);
    }
    if (get("linear_defletion", "angle_defletion").empty()) {
        set("linear_defletion", "0.003");
        set("angle_defletion", "0.5");
    }
    if (get("is_split_compound").empty()) {
        set_bool("is_split_compound", false);
    }

    // Remove legacy window positions/sizes
    erase("app", "main_frame_maximized");
    erase("app", "main_frame_pos");
    erase("app", "main_frame_size");
    erase("app", "object_settings_maximized");
    erase("app", "object_settings_pos");
    erase("app", "object_settings_size");
    erase("app", "severity_level");
}

#ifdef WIN32
static std::string appconfig_md5_hash_line(const std::string_view data)
{
    //FIXME replace the two following includes with <boost/md5.hpp> after it becomes mainstream.
    // return boost::md5(data).hex_str_value();
    // boost::uuids::detail::md5 is an internal namespace thus it may change in the future.
    // Also this implementation is not the fastest, it was designed for short blocks of text.
    using boost::uuids::detail::md5;
    md5              md5_hash;
    // unsigned int[4], 128 bits
    md5::digest_type md5_digest{};
    std::string      md5_digest_str;
    md5_hash.process_bytes(data.data(), data.size());
    md5_hash.get_digest(md5_digest);
    boost::algorithm::hex(md5_digest, md5_digest + std::size(md5_digest), std::back_inserter(md5_digest_str));
    // MD5 hash is 32 HEX digits long.
    assert(md5_digest_str.size() == 32);
    // This line will be emited at the end of the file.
    return "# MD5 checksum " + md5_digest_str + "\n";
}

// Assume that the last line with the comment inside the config file contains a checksum and that the user didn't modify the config file.
static bool verify_config_file_checksum(boost::nowide::ifstream &ifs)
{
    auto read_whole_config_file = [&ifs]() -> std::string {
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    };

    ifs.seekg(0, boost::nowide::ifstream::beg);
    std::string whole_config = read_whole_config_file();

    // The checksum should be on the last line in the config file.
    if (size_t last_comment_pos = whole_config.find_last_of('#'); last_comment_pos != std::string::npos) {
        // Split read config into two parts, one with checksum, and the second part is part with configuration from the checksum was computed.
        // Verify existence and validity of the MD5 checksum line at the end of the file.
        // When the checksum isn't found, the checksum was not saved correctly, it was removed or it is an older config file without the checksum.
        // If the checksum is incorrect, then the file was either not saved correctly or modified.
        if (std::string_view(whole_config.c_str() + last_comment_pos, whole_config.size() - last_comment_pos) == appconfig_md5_hash_line({ whole_config.data(), last_comment_pos }))
            return true;
    }
    return false;
}
#endif



#ifdef USE_JSON_CONFIG
std::string AppConfig::load()
{
    json j;

    // 1) Read the complete config file into a boost::property_tree.
    namespace pt = boost::property_tree;
    pt::ptree tree;
    boost::nowide::ifstream ifs;
    bool recovered = false;
    std::string error_message;

    try {
        ifs.open(AppConfig::loading_path());

#ifdef WIN32
        std::stringstream input_stream;
        input_stream << ifs.rdbuf();
        std::string total_string = input_stream.str();
        size_t last_pos = total_string.find_last_of('}');
        std::string left_string = total_string.substr(0, last_pos+1);
        //skip the "\n"
        std::string right_string = total_string.substr(last_pos+2);

        std::string md5_str = appconfig_md5_hash_line({left_string.data()});
        // Verify the checksum of the config file without taking just for debugging purpose.
        if (md5_str != right_string)
            BOOST_LOG_TRIVIAL(info) << "The configuration file " << AppConfig::loading_path() <<
            " has a wrong MD5 checksum or the checksum is missing. This may indicate a file corruption or a harmless user edit.";
        j = json::parse(left_string);
#else
        ifs >> j;
#endif
    }
    catch(nlohmann::detail::parse_error &err) {
#ifdef WIN32
        // The configuration file is corrupted, try replacing it with the backup configuration.
        ifs.close();
        std::string backup_path = (boost::format("%1%.bak") % AppConfig::loading_path()).str();
        if (boost::filesystem::exists(backup_path)) {
            // Compute checksum of the configuration backup file and try to load configuration from it when the checksum is correct.
            boost::nowide::ifstream backup_ifs(backup_path);
            std::stringstream back_input_stream;
            back_input_stream << backup_ifs.rdbuf();
            std::string back_total_string = back_input_stream.str();
            size_t back_last_pos = back_total_string.find_last_of('}');
            std::string back_left_string = back_total_string.substr(0, back_last_pos+1);
            std::string back_right_string = back_total_string.substr(back_last_pos+2);

            std::string back_md5_str = appconfig_md5_hash_line({back_left_string.data()});
            // Verify the checksum of the config file without taking just for debugging purpose.
            if (back_md5_str != back_right_string) {
                BOOST_LOG_TRIVIAL(error) << format("Both \"%1%\" and \"%2%\" are corrupted. It isn't possible to restore configuration from the backup.", AppConfig::loading_path(), backup_path);
                backup_ifs.close();
                boost::filesystem::remove(backup_path);
            }
            else if (std::string error_message; copy_file(backup_path, AppConfig::loading_path(), error_message, false) != SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << format("Configuration file \"%1%\" is corrupted. Failed to restore from backup \"%2%\": %3%", AppConfig::loading_path(), backup_path, error_message);
                backup_ifs.close();
                boost::filesystem::remove(backup_path);
            }
            else {
                BOOST_LOG_TRIVIAL(info) << format("Configuration file \"%1%\" was corrupted. It has been succesfully restored from the backup \"%2%\".", AppConfig::loading_path(), backup_path);
                // Try parse configuration file after restore from backup.
                j = json::parse(back_left_string);
                recovered = true;
            }
        }
        else
#endif // WIN32
            BOOST_LOG_TRIVIAL(info) << format("Failed to parse configuration file \"%1%\": %2%", AppConfig::loading_path(), err.what());

        if (!recovered)
            return err.what();
    }

    try {
        for (auto it = j.begin(); it != j.end(); it++) {
            if (it.key() == MODELS_STR) {
                for (auto& j_model : it.value()) {
                    // This is a vendor section listing enabled model / variants
                    const auto vendor_name = j_model["vendor"].get<std::string>();
                    auto& vendor = m_vendors[vendor_name];
                    const auto model_name = j_model["model"].get<std::string>();
                    std::vector<std::string> variants;
                    if (!unescape_strings_cstyle(j_model["nozzle_diameter"], variants)) { continue; }
                    for (const auto& variant : variants) {
                        vendor[model_name].insert(variant);
                    }
                }
            } else if (it.key() == SECTION_FILAMENTS) {
                json j_filaments = it.value();
                for (auto& element : j_filaments) {
                    m_storage[it.key()][element] = "true";
                }
            } else if (it.key() == "presets") {
                for (auto iter = it.value().begin(); iter != it.value().end(); iter++) {
                    if (iter.key() == "filaments") {
                        int idx = 0;
                        for(auto& element: iter.value()) {
                            if (idx == 0)
                                m_storage[it.key()]["filament"] = element;
                            else {
                                auto n = std::to_string(idx);
                                if (n.length() == 1) n = "0" + n;
                                m_storage[it.key()]["filament_" + n] = element;
                            }
                            idx++;
                        }
                    } else {
                        m_storage[it.key()][iter.key()] = iter.value().get<std::string>();
                    }
                }
            } else if (it.key() == "calis") {
                for (auto &calis_j : it.value()) {
                    PrinterCaliInfo cali_info;
                    if (calis_j.contains("dev_id"))
                        cali_info.dev_id = calis_j["dev_id"].get<std::string>();
                    if (calis_j.contains("cali_finished"))
                        cali_info.cali_finished = bool(calis_j["cali_finished"].get<int>());
                    if (calis_j.contains("flow_ratio"))
                        cali_info.cache_flow_ratio = calis_j["flow_ratio"].get<float>();
                    if (calis_j.contains("cache_flow_rate_calibration_type"))
                        cali_info.cache_flow_rate_calibration_type = static_cast<FlowRatioCalibrationType>(calis_j["cache_flow_rate_calibration_type"].get<int>());
                    if (calis_j.contains("presets")) {
                        cali_info.selected_presets.clear();
                        for (auto cali_it = calis_j["presets"].begin(); cali_it != calis_j["presets"].end(); cali_it++) {
                            CaliPresetInfo preset_info;
                            preset_info.tray_id     = cali_it.value()["tray_id"].get<int>();
                            preset_info.nozzle_diameter = cali_it.value()["nozzle_diameter"].get<float>();
                            preset_info.filament_id     = cali_it.value()["filament_id"].get<std::string>();
                            preset_info.setting_id      = cali_it.value()["setting_id"].get<std::string>();
                            preset_info.name            = cali_it.value()["name"].get<std::string>();
                            if (cali_it.value().contains("extruder_id"))
                                preset_info.extruder_id = cali_it.value()["extruder_id"].get<int>();
                            if (cali_it.value().contains("nozzle_volume_type"))
                                preset_info.nozzle_volume_type  = NozzleVolumeType(cali_it.value()["nozzle_volume_type"].get<int>());
                            if (cali_it.value().contains("bed_type"))
                                preset_info.bed_type = BedType(cali_it.value()["bed_type"].get<int>());
                            cali_info.selected_presets.push_back(preset_info);
                        }
                    }
                    m_printer_cali_infos.emplace_back(cali_info);
                }
            } else if (it.key() == "orca_presets") {
                for (auto& j_model : it.value()) {
                    m_printer_settings[j_model["machine"].get<std::string>()] = j_model;
                }
            } else if (it.key() == "local_machines") {
                for (auto m = it.value().begin(); m != it.value().end(); ++m) {
                    const auto&    p = m.value();
                    BBLocalMachine local_machine;
                    local_machine.dev_id = m.key();
                    if (p.contains("dev_name"))
                        local_machine.dev_name = p["dev_name"].get<std::string>();
                    if (p.contains("dev_ip"))
                        local_machine.dev_ip = p["dev_ip"].get<std::string>();
                    if (p.contains("printer_type"))
                        local_machine.printer_type = p["printer_type"].get<std::string>();
                    m_local_machines[local_machine.dev_id] = local_machine;
                }
            } else {
                if (it.value().is_object()) {
                    for (auto iter = it.value().begin(); iter != it.value().end(); iter++) {
                        if (iter.value().is_boolean()) {
                            if (iter.value()) {
                                m_storage[it.key()][iter.key()] = "true";
                            } else {
                                m_storage[it.key()][iter.key()] = "false";
                            }
                        } else if (iter.key() == "filament_presets") {
                            m_filament_presets = iter.value().get<std::vector<std::string>>();
                        } else if (iter.key() == "filament_colors") {
                            m_filament_colors = iter.value().get<std::vector<std::string>>();
                        } else if(iter.key() == "filament_multi_colors") {
                           m_filament_multi_colors = iter.value().get<std::vector<std::string>>();
                        } else if(iter.key() == "filament_color_types") {
                           m_filament_color_types = iter.value().get<std::vector<std::string>>();
                        } else {
                            if (iter.value().is_string())
                                m_storage[it.key()][iter.key()] = iter.value().get<std::string>();
                            else {
                                BOOST_LOG_TRIVIAL(warning) << "load config warning...";
                            }
                        }
                    }
                }
            }
        }
    } catch(std::exception err) {
        BOOST_LOG_TRIVIAL(info) << format("parse app config \"%1%\", error: %2%", AppConfig::loading_path(), err.what());

        return err.what();
    }

    // Figure out if datadir has legacy presets
    auto ini_ver = Semver::parse(get("version"));
    m_legacy_datadir = false;
    if (ini_ver) {
        m_orig_version = *ini_ver;
        ini_ver->set_metadata(boost::none);
        ini_ver->set_prerelease(boost::none);
    }

    // Legacy conversion
    if (m_mode == EAppMode::Editor) {
        // Convert [extras] "physical_printer" to [presets] "physical_printer",
        // remove the [extras] section if it becomes empty.
        if (auto it_section = m_storage.find("extras"); it_section != m_storage.end()) {
            if (auto it_physical_printer = it_section->second.find("physical_printer"); it_physical_printer != it_section->second.end()) {
                m_storage["presets"]["physical_printer"] = it_physical_printer->second;
                it_section->second.erase(it_physical_printer);
            }
            if (it_section->second.empty())
                m_storage.erase(it_section);
        }
    }

    // Override missing or keys with their defaults.
    this->set_defaults();
    m_dirty = false;
    return "";
}

void AppConfig::save()
{
    if (! is_main_thread_active())
        throw CriticalException("Calling AppConfig::save() from a worker thread!");

    // The config is first written to a file with a PID suffix and then moved
    // to avoid race conditions with multiple instances of Slic3r
    const auto path = config_path();
    std::string path_pid = (boost::format("%1%.%2%") % path % get_current_pid()).str();

    json j;

    std::stringstream config_ss;
    if (m_mode == EAppMode::Editor)
        j["header"] = Slic3r::header_slic3r_generated();
    else
        j["header"] = Slic3r::header_gcodeviewer_generated();

    // Make sure the "no" category is written first.
    for (const auto& kvp : m_storage["app"]) {
        if (kvp.second == "true") {
            j["app"][kvp.first] = true;
            continue;
        }
        if (kvp.second == "false") {
            j["app"][kvp.first] = false;
            continue;
        }
        j["app"][kvp.first] = kvp.second;
    }

    for (const auto &filament_preset : m_filament_presets) {
        j["app"]["filament_presets"].push_back(filament_preset);
    }

    for (const auto &filament_color : m_filament_colors) {
        j["app"]["filament_colors"].push_back(filament_color);
    }
    for (const auto &filament_multi_color : m_filament_multi_colors) {
       j["app"]["filament_multi_colors"].push_back(filament_multi_color);
    }

    for (const auto &filament_color_type : m_filament_color_types) {
       j["app"]["filament_color_types"].push_back(filament_color_type);
    }

    for (const auto &cali_info : m_printer_cali_infos) {
        json cali_json;
        cali_json["dev_id"]             = cali_info.dev_id;
        cali_json["flow_ratio"]         = cali_info.cache_flow_ratio;
        cali_json["cali_finished"]      = cali_info.cali_finished ? 1 : 0;
        cali_json["cache_flow_rate_calibration_type"] = static_cast<int>(cali_info.cache_flow_rate_calibration_type);
        for (auto filament_preset : cali_info.selected_presets) {
            json preset_json;
            preset_json["tray_id"] = filament_preset.tray_id;
            preset_json["extruder_id"]      = filament_preset.extruder_id;
            preset_json["nozzle_volume_type"]  = int(filament_preset.nozzle_volume_type);
            preset_json["bed_type"] = int(filament_preset.bed_type);
            preset_json["nozzle_diameter"]  = filament_preset.nozzle_diameter;
            preset_json["filament_id"]      = filament_preset.filament_id;
            preset_json["setting_id"]       = filament_preset.setting_id;
            preset_json["name"]             = filament_preset.name;
            cali_json["presets"].push_back(preset_json);
        }
        j["calis"].push_back(cali_json);
    }

    // Write the other categories.
    for (const auto& category : m_storage) {
        if (category.first.empty())
            continue;
        if (category.first == SECTION_FILAMENTS) {
            json j_filaments;
            for (const auto& kvp: category.second) {
                j_filaments.push_back(kvp.first);
            }
            j[category.first] = j_filaments;
            continue;
        } else if (category.first == "presets") {
            json j_filament_array;
            for(const auto& kvp : category.second) {
                if (boost::starts_with(kvp.first, "filament") && kvp.first != "filament_colors" && kvp.first != "filament_multi_colors" && kvp.first != "filament_color_types") {
                    j_filament_array.push_back(kvp.second);
                } else {
                    j[category.first][kvp.first] = kvp.second;
                }
            }
            j["presets"]["filaments"] = j_filament_array;
            continue;
        }
        for (const auto& kvp : category.second) {
            if (kvp.second == "true") {
                j[category.first][kvp.first] = true;
                continue;
            }
            if (kvp.second == "false") {
                j[category.first][kvp.first] = false;
                continue;
            }
            j[category.first][kvp.first] = kvp.second;
        }
    }

    // Write vendor sections
    for (const auto& vendor : m_vendors) {
        size_t size_sum = 0;
        for (const auto& model : vendor.second) { size_sum += model.second.size(); }
        if (size_sum == 0) { continue; }

        for (const auto& model : vendor.second) {
            if (model.second.empty()) { continue; }
            const std::vector<std::string> variants(model.second.begin(), model.second.end());
            const auto escaped = escape_strings_cstyle(variants);
            //j[VENDOR_PREFIX + vendor.first][MODEL_PREFIX + model.first] = escaped;
            json j_model;
            j_model["vendor"] = vendor.first;
            j_model["model"] = model.first;
            j_model["nozzle_diameter"] = escaped;
            j[MODELS_STR].push_back(j_model);
        }
    }

    // write machine settings
    for (const auto& preset : m_printer_settings) {
        j["orca_presets"].push_back(preset.second);
    }
    for (const auto& local_machine : m_local_machines) {
        json m_json;
        m_json["dev_name"]         = local_machine.second.dev_name;
        m_json["dev_ip"]           = local_machine.second.dev_ip;
        m_json["printer_type"]     = local_machine.second.printer_type;

        j["local_machines"][local_machine.first] = m_json;
    }
    boost::nowide::ofstream c;
    c.open(path_pid, std::ios::out | std::ios::trunc);
    c << std::setw(4) << j << std::endl;

#ifdef WIN32
    // WIN32 specific: The final "rename_file()" call is not safe in case of an application crash, there is no atomic "rename file" API
    // provided by Windows (sic!). Therefore we save a MD5 checksum to be able to verify file corruption. In addition,
    // we save the config file into a backup first before moving it to the final destination.
    c << appconfig_md5_hash_line(j.dump(4));
#endif

    c.close();
    if (c.fail()) {
      BOOST_LOG_TRIVIAL(error) << "Failed to write new configuration to " << path_pid << "; aborting attempt to overwrite original configuration";
      return;
    }

#ifdef WIN32
    // Make a backup of the configuration file before copying it to the final destination.
    std::string error_message;
    std::string backup_path = (boost::format("%1%.bak") % path).str();
    // Copy configuration file with PID suffix into the configuration file with "bak" suffix.
    if (copy_file(path_pid, backup_path, error_message, false) != SUCCESS)
        BOOST_LOG_TRIVIAL(error) << "Copying from " << path_pid << " to " << backup_path << " failed. Failed to create a backup configuration.";
#endif

    // Rename the config atomically.
    // On Windows, the rename is likely NOT atomic, thus it may fail if PrusaSlicer crashes on another thread in the meanwhile.
    // To cope with that, we already made a backup of the config on Windows.
    rename_file(path_pid, path);
    m_dirty = false;
}

#else

std::string AppConfig::load()
{
    // 1) Read the complete config file into a boost::property_tree.
    namespace pt = boost::property_tree;
    pt::ptree tree;
    boost::nowide::ifstream ifs;
    bool                    recovered = false;

    try {
        ifs.open(AppConfig::loading_path());
#ifdef WIN32
        // Verify the checksum of the config file without taking just for debugging purpose.
        if (!verify_config_file_checksum(ifs))
            BOOST_LOG_TRIVIAL(info) << "The configuration file " << AppConfig::loading_path() <<
            " has a wrong MD5 checksum or the checksum is missing. This may indicate a file corruption or a harmless user edit.";

        ifs.seekg(0, boost::nowide::ifstream::beg);
#endif
        pt::read_ini(ifs, tree);
    }
    catch (pt::ptree_error& ex) {
#ifdef WIN32
        // The configuration file is corrupted, try replacing it with the backup configuration.
        ifs.close();
        std::string backup_path = (boost::format("%1%.bak") % AppConfig::loading_path()).str();
        if (boost::filesystem::exists(backup_path)) {
            // Compute checksum of the configuration backup file and try to load configuration from it when the checksum is correct.
            boost::nowide::ifstream backup_ifs(backup_path);
            if (!verify_config_file_checksum(backup_ifs)) {
                BOOST_LOG_TRIVIAL(error) << format("Both \"%1%\" and \"%2%\" are corrupted. It isn't possible to restore configuration from the backup.", AppConfig::loading_path(), backup_path);
                backup_ifs.close();
                boost::filesystem::remove(backup_path);
            }
            else if (std::string error_message; copy_file(backup_path, AppConfig::loading_path(), error_message, false) != SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << format("Configuration file \"%1%\" is corrupted. Failed to restore from backup \"%2%\": %3%", AppConfig::loading_path(), backup_path, error_message);
                backup_ifs.close();
                boost::filesystem::remove(backup_path);
            }
            else {
                BOOST_LOG_TRIVIAL(info) << format("Configuration file \"%1%\" was corrupted. It has been succesfully restored from the backup \"%2%\".", AppConfig::loading_path(), backup_path);
                // Try parse configuration file after restore from backup.
                try {
                    ifs.open(AppConfig::loading_path());
                    pt::read_ini(ifs, tree);
                    recovered = true;
                }
                catch (pt::ptree_error& ex) {
                    BOOST_LOG_TRIVIAL(info) << format("Failed to parse configuration file \"%1%\" after it has been restored from backup: %2%", AppConfig::loading_path(), ex.what());
                }
            }
        }
        else
#endif // WIN32
            BOOST_LOG_TRIVIAL(info) << format("Failed to parse configuration file \"%1%\": %2%", AppConfig::loading_path(), ex.what());
        if (!recovered) {
            // Report the initial error of parsing PrusaSlicer.ini.
            // Error while parsing config file. We'll customize the error message and rethrow to be displayed.
            // ! But to avoid the use of _utf8 (related to use of wxWidgets)
            // we will rethrow this exception from the place of load() call, if returned value wouldn't be empty
            /*
            throw Slic3r::RuntimeError(
                _utf8(L("Error parsing Prusa config file, it is probably corrupted. "
                        "Try to manually delete the file to recover from the error. Your user profiles will not be affected.")) +
                "\n\n" + AppConfig::config_path() + "\n\n" + ex.what());
            */
            return ex.what();
        }
    }

    // 2) Parse the property_tree, extract the sections and key / value pairs.
    for (const auto& section : tree) {
        if (section.second.empty()) {
            // This may be a top level (no section) entry, or an empty section.
            std::string data = section.second.data();
            if (!data.empty())
                // If there is a non-empty data, then it must be a top-level (without a section) config entry.
                m_storage[""][section.first] = data;
        }
        else if (boost::starts_with(section.first, VENDOR_PREFIX)) {
            // This is a vendor section listing enabled model / variants
            const auto vendor_name = section.first.substr(VENDOR_PREFIX.size());
            auto& vendor = m_vendors[vendor_name];
            for (const auto& kvp : section.second) {
                if (!boost::starts_with(kvp.first, MODEL_PREFIX)) { continue; }
                const auto model_name = kvp.first.substr(MODEL_PREFIX.size());
                std::vector<std::string> variants;
                if (!unescape_strings_cstyle(kvp.second.data(), variants)) { continue; }
                for (const auto& variant : variants) {
                    vendor[model_name].insert(variant);
                }
            }
        }
        else {
            // This must be a section name. Read the entries of a section.
            std::map<std::string, std::string>& storage = m_storage[section.first];
            for (auto& kvp : section.second)
                storage[kvp.first] = kvp.second.data();
        }
    }

    // Figure out if datadir has legacy presets
    auto ini_ver = Semver::parse(get("version"));
    m_legacy_datadir = false;
    if (ini_ver) {
        m_orig_version = *ini_ver;
        // Make 1.40.0 alphas compare well
        ini_ver->set_metadata(boost::none);
        ini_ver->set_prerelease(boost::none);
        //m_legacy_datadir = ini_ver < Semver(1, 40, 0);
    }

    // Legacy conversion
    if (m_mode == EAppMode::Editor) {
        // Convert [extras] "physical_printer" to [presets] "physical_printer",
        // remove the [extras] section if it becomes empty.
        if (auto it_section = m_storage.find("extras"); it_section != m_storage.end()) {
            if (auto it_physical_printer = it_section->second.find("physical_printer"); it_physical_printer != it_section->second.end()) {
                m_storage["presets"]["physical_printer"] = it_physical_printer->second;
                it_section->second.erase(it_physical_printer);
            }
            if (it_section->second.empty())
                m_storage.erase(it_section);
        }
    }

    // Override missing or keys with their defaults.
    this->set_defaults();
    m_dirty = false;
    return "";
}

void AppConfig::save()
{
    if (! is_main_thread_active())
        throw CriticalException("Calling AppConfig::save() from a worker thread!");

    // The config is first written to a file with a PID suffix and then moved
    // to avoid race conditions with multiple instances of Slic3r
    const auto path = config_path();
    std::string path_pid = (boost::format("%1%.%2%") % path % get_current_pid()).str();

    std::stringstream config_ss;
    if (m_mode == EAppMode::Editor)
        config_ss << "# " << Slic3r::header_slic3r_generated() << std::endl;
    else
        config_ss << "# " << Slic3r::header_gcodeviewer_generated() << std::endl;
    // Make sure the "no" category is written first.
    for (const auto& kvp : m_storage[""])
        config_ss << kvp.first << " = " << kvp.second << std::endl;
    // Write the other categories.
    for (const auto& category : m_storage) {
    	if (category.first.empty())
    		continue;
        config_ss << std::endl << "[" << category.first << "]" << std::endl;
        for (const auto& kvp : category.second)
            config_ss << kvp.first << " = " << kvp.second << std::endl;
	}
    // Write vendor sections
    for (const auto &vendor : m_vendors) {
        size_t size_sum = 0;
        for (const auto &model : vendor.second) { size_sum += model.second.size(); }
        if (size_sum == 0) { continue; }

        config_ss << std::endl << "[" << VENDOR_PREFIX << vendor.first << "]" << std::endl;

        for (const auto &model : vendor.second) {
            if (model.second.empty()) { continue; }
            const std::vector<std::string> variants(model.second.begin(), model.second.end());
            const auto escaped = escape_strings_cstyle(variants);
            config_ss << MODEL_PREFIX << model.first << " = " << escaped << std::endl;
        }
    }
    // One empty line before the MD5 sum.
    config_ss << std::endl;

    std::string config_str = config_ss.str();
    boost::nowide::ofstream c;
    c.open(path_pid, std::ios::out | std::ios::trunc);
    c << config_str;
#ifdef WIN32
    // WIN32 specific: The final "rename_file()" call is not safe in case of an application crash, there is no atomic "rename file" API
    // provided by Windows (sic!). Therefore we save a MD5 checksum to be able to verify file corruption. In addition,
    // we save the config file into a backup first before moving it to the final destination.
    c << appconfig_md5_hash_line(config_str);
#endif
    c.close();
    if (c.fail()) {
      BOOST_LOG_TRIVIAL(error) << "Failed to write new configuration to " << path_pid << "; aborting attempt to overwrite original configuration";
      return;
    }

#ifdef WIN32
    // Make a backup of the configuration file before copying it to the final destination.
    std::string error_message;
    std::string backup_path = (boost::format("%1%.bak") % path).str();
    // Copy configuration file with PID suffix into the configuration file with "bak" suffix.
    if (copy_file(path_pid, backup_path, error_message, false) != SUCCESS)
        BOOST_LOG_TRIVIAL(error) << "Copying from " << path_pid << " to " << backup_path << " failed. Failed to create a backup configuration.";
#endif

    // Rename the config atomically.
    // On Windows, the rename is likely NOT atomic, thus it may fail if PrusaSlicer crashes on another thread in the meanwhile.
    // To cope with that, we already made a backup of the config on Windows.
    rename_file(path_pid, path);
    m_dirty = false;
}
#endif

bool AppConfig::get_variant(const std::string &vendor, const std::string &model, const std::string &variant) const
{
    const auto it_v = m_vendors.find(vendor);
    if (it_v == m_vendors.end()) { return false; }
    const auto it_m = it_v->second.find(model);
    return it_m == it_v->second.end() ? false : it_m->second.find(variant) != it_m->second.end();
}

void AppConfig::set_variant(const std::string &vendor, const std::string &model, const std::string &variant, bool enable)
{
    if (enable) {
        if (get_variant(vendor, model, variant)) { return; }
        m_vendors[vendor][model].insert(variant);
    } else {
        auto it_v = m_vendors.find(vendor);
        if (it_v == m_vendors.end()) { return; }
        auto it_m = it_v->second.find(model);
        if (it_m == it_v->second.end()) { return; }
        auto it_var = it_m->second.find(variant);
        if (it_var == it_m->second.end()) { return; }
        it_m->second.erase(it_var);
    }
    // If we got here, there was an update
    m_dirty = true;
}

void AppConfig::set_vendors(const AppConfig &from)
{
    m_vendors = from.m_vendors;
    m_dirty = true;
}

void AppConfig::save_printer_cali_infos(const PrinterCaliInfo &cali_info, bool need_change_status)
{
    auto iter = std::find_if(m_printer_cali_infos.begin(), m_printer_cali_infos.end(), [&cali_info](const PrinterCaliInfo &cali_info_item) {
        return cali_info_item.dev_id == cali_info.dev_id;
    });

    if (iter == m_printer_cali_infos.end()) {
        m_printer_cali_infos.emplace_back(cali_info);
    } else {
        if (need_change_status) {
            (*iter).cali_finished = cali_info.cali_finished;
        }
        (*iter).cache_flow_ratio = cali_info.cache_flow_ratio;
        (*iter).selected_presets = cali_info.selected_presets;
        (*iter).cache_flow_rate_calibration_type = cali_info.cache_flow_rate_calibration_type;
    }
    m_dirty = true;
}

std::string AppConfig::get_last_dir() const
{
    const auto it = m_storage.find("recent");
    if (it != m_storage.end()) {
        {
            const auto it2 = it->second.find("last_opened_folder");
            if (it2 != it->second.end() && ! it2->second.empty())
                return it2->second;
        }
        {
            const auto it2 = it->second.find("settings_folder");
            if (it2 != it->second.end() && ! it2->second.empty())
                return it2->second;
        }
    }
    return std::string();
}

std::vector<std::string> AppConfig::get_recent_projects() const
{
    std::vector<std::string> ret;
    const auto it = m_storage.find("recent_projects");
    if (it != m_storage.end())
    {
        for (const std::map<std::string, std::string>::value_type& item : it->second)
        {
            ret.push_back(item.second);
        }
    }
    return ret;
}

void AppConfig::set_recent_projects(const std::vector<std::string>& recent_projects)
{
    auto it = m_storage.find("recent_projects");
    if (it == m_storage.end())
        it = m_storage.insert(std::map<std::string, std::map<std::string, std::string>>::value_type("recent_projects", std::map<std::string, std::string>())).first;

    it->second.clear();
    for (unsigned int i = 0; i < (unsigned int)recent_projects.size(); ++i)
    {
        auto n = std::to_string(i + 1);
        if (n.length() == 1) n = "00" + n;
        else if (n.length() == 2) n = "0" + n;
        it->second[n] = recent_projects[i];
    }
}

void AppConfig::set_mouse_device(const std::string& name, double translation_speed, double translation_deadzone,
                                 float rotation_speed, float rotation_deadzone, double zoom_speed, bool swap_yz, bool invert_x, bool invert_y, bool invert_z, bool invert_yaw, bool invert_pitch, bool invert_roll)
{
    std::string key = std::string("mouse_device:") + name;
    auto it = m_storage.find(key);
    if (it == m_storage.end())
        it = m_storage.insert(std::map<std::string, std::map<std::string, std::string>>::value_type(key, std::map<std::string, std::string>())).first;

    it->second.clear();
    it->second["translation_speed"] = float_to_string_decimal_point(translation_speed);
    it->second["translation_deadzone"] = float_to_string_decimal_point(translation_deadzone);
    it->second["rotation_speed"] = float_to_string_decimal_point(rotation_speed);
    it->second["rotation_deadzone"] = float_to_string_decimal_point(rotation_deadzone);
    it->second["zoom_speed"] = float_to_string_decimal_point(zoom_speed);
    it->second["swap_yz"] = swap_yz ? "1" : "0";
    it->second["invert_x"] = invert_x ? "1" : "0";
    it->second["invert_y"] = invert_y ? "1" : "0";
    it->second["invert_z"] = invert_z ? "1" : "0";
    it->second["invert_yaw"] = invert_yaw ? "1" : "0";
    it->second["invert_pitch"] = invert_pitch ? "1" : "0";
    it->second["invert_roll"] = invert_roll ? "1" : "0";
}

std::vector<std::string> AppConfig::get_mouse_device_names() const
{
    static constexpr const char   *prefix     = "mouse_device:";
    static const size_t  prefix_len = strlen(prefix);
    std::vector<std::string> out;
    for (const auto& key_value_pair : m_storage)
        if (boost::starts_with(key_value_pair.first, prefix) && key_value_pair.first.size() > prefix_len)
            out.emplace_back(key_value_pair.first.substr(prefix_len));
    return out;
}

void AppConfig::update_config_dir(const std::string &dir)
{
    this->set("recent", "settings_folder", dir);
}

void AppConfig::update_skein_dir(const std::string &dir)
{
    if (is_shapes_dir(dir))
        return; // do not save "shapes gallery" directory
    this->set("recent", "last_opened_folder", dir);
}
/*
std::string AppConfig::get_last_output_dir(const std::string &alt) const
{

    const auto it = m_storage.find("");
    if (it != m_storage.end()) {
        const auto it2 = it->second.find("last_export_path");
        const auto it3 = it->second.find("remember_output_path");
        if (it2 != it->second.end() && it3 != it->second.end() && ! it2->second.empty() && it3->second == "1")
            return it2->second;
    }
    return alt;
}

void AppConfig::update_last_output_dir(const std::string &dir)
{
    this->set("", "last_export_path", dir);
}
*/
std::string AppConfig::get_last_output_dir(const std::string& alt, const bool removable) const
{
	std::string s1 = ("last_export_path");
	const auto it = m_storage.find("app");
	if (it != m_storage.end()) {
		const auto it2 = it->second.find(s1);
		if (it2 != it->second.end() && !it2->second.empty())
			return it2->second;
	}
	return is_shapes_dir(alt) ? get_last_dir() : alt;
}

void AppConfig::update_last_output_dir(const std::string& dir, const bool removable)
{
	this->set("app", ("last_export_path"), dir);
}

// BBS: backup
std::string AppConfig::get_last_backup_dir() const
{
	const auto it = m_storage.find("app");
	if (it != m_storage.end()) {
		const auto it2 = it->second.find("last_backup_path");
		if (it2 != it->second.end())
			return it2->second;
	}
	return "";
}

// BBS: backup
void AppConfig::update_last_backup_dir(const std::string& dir)
{
	this->set("app", "last_backup_path", dir);
    this->save();
}

std::string AppConfig::get_region()
{
// #if BBL_RELEASE_TO_PUBLIC
    return this->get("region");
// #else
//     std::string sel = get("iot_environment");
//     std::string region;
//     if (sel == ENV_DEV_HOST)
//         region = "ENV_CN_DEV";
//     else if (sel == ENV_QAT_HOST)
//         region = "ENV_CN_QA";
//     else if (sel == ENV_PRE_HOST)
//         region = "ENV_CN_PRE";
//     if (region.empty())
//         return this->get("region");
//     return region;
// #endif
}

std::string AppConfig::get_country_code()
{
    std::string region = get_region();
// #if !BBL_RELEASE_TO_PUBLIC
//     if (is_engineering_region()) { return region; }
// #endif
    if (region == "CHN" || region == "China")
        return "CN";
    else if (region == "USA")
        return "US";
    else if (region == "Asia-Pacific")
        return "Others";
    else if (region == "Europe")
        return "US";
    else if (region == "North America")
        return "US";
    else
        return "Others";
    return "";

}

bool AppConfig::is_engineering_region(){
    std::string sel = get("iot_environment");
    std::string region;
    if (sel == ENV_DEV_HOST
        || sel == ENV_QAT_HOST
        ||sel == ENV_PRE_HOST)
        return true;
    return false;
}

void AppConfig::save_custom_color_to_config(const std::vector<std::string> &colors)
{
    auto set_colors = [](std::map<std::string, std::string> &data, const std::vector<std::string> &colors) {
        for (size_t i = 0; i < colors.size(); i++) {
            data[std::to_string(10 + i)] = colors[i]; // for map sort:10 begin
        }
    };
    if (colors.size() > 0) {
        if (!has_section("custom_color_list")) {
            std::map<std::string, std::string> data;
            set_colors(data, colors);
            set_section("custom_color_list", data);
        } else {
            auto data        = get_section("custom_color_list");
            auto data_modify = const_cast<std::map<std::string, std::string> *>(&data);
            set_colors(*data_modify, colors);
            set_section("custom_color_list", *data_modify);
        }
    }
}

std::vector<std::string> AppConfig::get_custom_color_from_config()
{
    std::vector<std::string> colors;
    if (has_section("custom_color_list")) {
        auto data = get_section("custom_color_list");
        for (auto iter : data) {
            colors.push_back(iter.second);
        }
    }
    return colors;
}

void AppConfig::save_nozzle_volume_types_to_config(const std::string& printer_name, const std::string& nozzle_volume_types)
{
    if (!has_section("nozzle_volume_types")) {
        std::map<std::string, std::string> data;
        data[printer_name] = nozzle_volume_types;
        set_section("nozzle_volume_types", data);
    } else {
        auto data        = get_section("nozzle_volume_types");
        auto data_modify = const_cast<std::map<std::string, std::string>&>(data);
        data_modify[printer_name] = nozzle_volume_types;
        set_section("nozzle_volume_types", data_modify);
    }
}

std::string AppConfig::get_nozzle_volume_types_from_config(const std::string& printer_name)
{
    std::string nozzle_volume_types;
    if (has_section("nozzle_volume_types")) {
        auto data        = get_section("nozzle_volume_types");
        if (data.find(printer_name) != data.end())
            nozzle_volume_types = data[printer_name];
    }

    return nozzle_volume_types;
}

void AppConfig::reset_selections()
{
    auto it = m_storage.find("presets");
    if (it != m_storage.end()) {
        it->second.erase(PRESET_PRINT_NAME);
        it->second.erase(PRESET_FILAMENT_NAME);
        it->second.erase("sla_print");
        it->second.erase("sla_material");
        it->second.erase(PRESET_PRINTER_NAME);
        it->second.erase("physical_printer");
        m_dirty = true;
    }
}

std::string AppConfig::config_path()
{
#ifdef USE_JSON_CONFIG
    std::string path = (m_mode == EAppMode::Editor) ?
        (boost::filesystem::path(Slic3r::data_dir()) / (SLIC3R_APP_KEY ".conf")).make_preferred().string() :
        (boost::filesystem::path(Slic3r::data_dir()) / (GCODEVIEWER_APP_KEY ".conf")).make_preferred().string();
#else
    std::string path = (m_mode == EAppMode::Editor) ?
        (boost::filesystem::path(Slic3r::data_dir()) / (SLIC3R_APP_KEY ".ini")).make_preferred().string() :
        (boost::filesystem::path(Slic3r::data_dir()) / (GCODEVIEWER_APP_KEY ".ini")).make_preferred().string();
#endif

    return path;
}

std::string AppConfig::version_check_url() const
{
    auto from_settings = get("version_check_url");
    return from_settings.empty() ? VERSION_CHECK_URL : from_settings;
}

std::string AppConfig::profile_update_url() const
{
    return PROFILE_UPDATE_URL;
}

bool AppConfig::exists()
{
    return boost::filesystem::exists(config_path());
}

}; // namespace Slic3r
