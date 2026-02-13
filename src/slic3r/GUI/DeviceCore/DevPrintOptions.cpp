#include "DevPrintOptions.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r
{

void DevPrintOptionsParser::Parse(DevPrintOptions* opts, const nlohmann::json& print_json)
{
    try
    {
        if (print_json.contains("spd_lvl"))
        {
            opts->m_speed_level = static_cast<DevPrintingSpeedLevel>(print_json["spd_lvl"].get<int>());
        }

        if (print_json.contains("cfg"))
        {
            const std::string& cfg = print_json["cfg"].get<std::string>();
            opts->m_speed_level = (DevPrintingSpeedLevel)DevUtil::get_flag_bits(cfg, 8, 3);
        }
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "DevPrintOptionsParser::Parse: Failed to parse print options from JSON." << e.what();
    }
}

void DevPrintOptionsParser::ParseDetectionV1_0(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json)
{
    try {
        if (print_json.contains("xcam")) {
            if (time(nullptr) - opts->xcam_ai_monitoring_hold_start > HOLD_TIME_3SEC) {
                if (print_json["xcam"].contains("printing_monitor")) {
                    // new protocol
                    opts->xcam_ai_monitoring = print_json["xcam"]["printing_monitor"].get<bool>();
                } else {
                    // old version protocol
                    if (print_json["xcam"].contains("spaghetti_detector")) {
                        opts->xcam_ai_monitoring = print_json["xcam"]["spaghetti_detector"].get<bool>();
                        if (print_json["xcam"].contains("print_halt")) {
                            bool print_halt = print_json["xcam"]["print_halt"].get<bool>();
                            if (print_halt) { opts->xcam_ai_monitoring_sensitivity = "medium"; }
                        }
                    }
                }
                if (print_json["xcam"].contains("halt_print_sensitivity")) {
                    opts->xcam_ai_monitoring_sensitivity = print_json["xcam"]["halt_print_sensitivity"].get<std::string>();
                }
            }

            if (time(nullptr) - opts->xcam_first_layer_hold_start > HOLD_TIME_3SEC) {
                if (print_json["xcam"].contains("first_layer_inspector")) { opts->xcam_first_layer_inspector = print_json["xcam"]["first_layer_inspector"].get<bool>(); }
            }

            if (time(nullptr) - opts->xcam_buildplate_marker_hold_start > HOLD_TIME_3SEC) {
                if (print_json["xcam"].contains("buildplate_marker_detector")) {
                    opts->xcam_buildplate_marker_detector      = print_json["xcam"]["buildplate_marker_detector"].get<bool>();
                    obj->is_support_build_plate_marker_detect = true;
                } else {
                    obj->is_support_build_plate_marker_detect = false;
                }
            }
        }
    } catch (...) {
        ;
    }

}

void DevPrintOptionsParser::ParseDetectionV1_1(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json,bool enable)
{
    if (print_json["module_name"].get<std::string>() == "first_layer_inspector") {
        if (time(nullptr) - opts->xcam_first_layer_hold_start > HOLD_TIME_3SEC) {
            opts->xcam_first_layer_inspector = enable;
        }
    } else if (print_json["module_name"].get<std::string>() == "buildplate_marker_detector") {
        if (time(nullptr) - opts->xcam_buildplate_marker_hold_start > HOLD_TIME_3SEC) {
            opts->xcam_buildplate_marker_detector = enable;
        }
    } else if (print_json["module_name"].get<std::string>() == "printing_monitor") {
        if (time(nullptr) - opts->xcam_ai_monitoring_hold_start > HOLD_TIME_3SEC) {
            opts->xcam_ai_monitoring = enable;
            if (print_json.contains("halt_print_sensitivity")) {
                opts->xcam_ai_monitoring_sensitivity = print_json["halt_print_sensitivity"].get<std::string>();
            }
        }
    } else if (print_json["module_name"].get<std::string>() == "spaghetti_detector") {
        if (time(nullptr) - opts->xcam_ai_monitoring_hold_start > HOLD_TIME_3SEC) {
            // old protocol
            opts->xcam_ai_monitoring = enable;
            if (print_json.contains("print_halt")) {
                if (print_json["print_halt"].get<bool>()) { opts->xcam_ai_monitoring_sensitivity = "medium"; }
            }
        }
    }
}

void DevPrintOptionsParser::ParseDetectionV1_2(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json) {

    try {
        if (print_json.contains("option")) {
            if (print_json["option"].is_number()) {
                int option = print_json["option"].get<int>();
                if (time(nullptr) - opts->xcam_auto_recovery_hold_start > HOLD_TIME_3SEC) { opts->xcam_auto_recovery_step_loss = ((option & 0x01) != 0); }
            }
        }

        if (time(nullptr) - opts->xcam_auto_recovery_hold_start > HOLD_TIME_3SEC) {
            if (print_json.contains("auto_recovery")) { opts->xcam_auto_recovery_step_loss = print_json["auto_recovery"].get<bool>(); }
        }
    } catch (...) {}

}

void DevPrintOptionsParser::ParseDetectionV2_0(DevPrintOptions *opts, std::string print_json)
{

    if (time(nullptr) - opts->xcam_first_layer_hold_start > HOLD_TIME_3SEC) {
        opts->xcam_first_layer_inspector = DevUtil::get_flag_bits(print_json, 12);
    }

    if (time(nullptr) - opts->xcam_ai_monitoring_hold_start > HOLD_COUNT_MAX) {
        opts->xcam_ai_monitoring = DevUtil::get_flag_bits(print_json, 15);

        switch (DevUtil::get_flag_bits(print_json, 13, 2)) {
        case 0: opts->xcam_ai_monitoring_sensitivity = "never_halt"; break;
        case 1: opts->xcam_ai_monitoring_sensitivity = "low"; break;
        case 2: opts->xcam_ai_monitoring_sensitivity = "medium"; break;
        case 3: opts->xcam_ai_monitoring_sensitivity = "high"; break;
        default: break;
        }
    }

    if (time(nullptr) - opts->xcam_auto_recovery_hold_start > HOLD_COUNT_MAX){
        opts->xcam_auto_recovery_step_loss =DevUtil::get_flag_bits(print_json, 16);
    }

    if (time(nullptr) - opts->xcam_prompt_sound_hold_start > HOLD_TIME_3SEC) {
        opts->xcam_allow_prompt_sound = DevUtil::get_flag_bits(print_json, 22);
    }

    if (time(nullptr) - opts->xcam_filament_tangle_detect_hold_start > HOLD_TIME_3SEC) {
        opts->xcam_filament_tangle_detect = DevUtil::get_flag_bits(print_json, 23);
    }
}

void DevPrintOptionsParser::ParseDetectionV2_1(DevPrintOptions *opts, std::string cfg) {
    if (time(nullptr) - opts->idel_heating_protect_hold_strat > HOLD_TIME_3SEC)
        opts->idel_heating_protect_enabled = DevUtil::get_flag_bits(cfg, 32, 2);
}

void DevPrintOptions::SetPrintingSpeedLevel(DevPrintingSpeedLevel speed_level)
{
    if (speed_level >= SPEED_LEVEL_INVALID && speed_level < SPEED_LEVEL_COUNT)
    {
        m_speed_level = speed_level;
    }
    else
    {
        m_speed_level = SPEED_LEVEL_INVALID; // Reset to invalid if out of range
    }
}

int DevPrintOptions::command_xcam_control_ai_monitoring(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false : true;

    xcam_ai_monitoring             = on_off;
    xcam_ai_monitoring_hold_start  = time(nullptr);
    xcam_ai_monitoring_sensitivity = lvl;
    return command_xcam_control("printing_monitor", on_off, m_obj, lvl);
}

int DevPrintOptions::command_xcam_control_idelheatingprotect_detector(bool on_off)
{
    idel_heating_protect_enabled    = on_off;
    idel_heating_protect_hold_strat = time(nullptr);
    return command_set_against_continued_heating_mode(on_off);
}

int DevPrintOptions::command_xcam_control_buildplate_marker_detector(bool on_off)
{
    xcam_buildplate_marker_detector   = on_off;
    xcam_buildplate_marker_hold_start = time(nullptr);
    return command_xcam_control("buildplate_marker_detector", on_off ,m_obj);
}

int DevPrintOptions::command_xcam_control_first_layer_inspector(bool on_off, bool print_halt)
{
    xcam_first_layer_inspector  = on_off;
    xcam_first_layer_hold_start = time(nullptr);
    return command_xcam_control("first_layer_inspector", on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_auto_recovery_step_loss(bool on_off)
{
    xcam_auto_recovery_step_loss  = on_off;
    xcam_auto_recovery_hold_start = time(nullptr);
    return command_set_printing_option(on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_allow_prompt_sound(bool on_off)
{
    xcam_allow_prompt_sound      = on_off;
    xcam_prompt_sound_hold_start = time(nullptr);
    return command_set_prompt_sound(on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_filament_tangle_detect(bool on_off)
{
    xcam_filament_tangle_detect            = on_off;
    xcam_filament_tangle_detect_hold_start = time(nullptr);
    return command_set_filament_tangle_detect(on_off, m_obj);
}

void DevPrintOptions::parse_auto_recovery_step_loss_status(int flag) {
    if (time(nullptr) - xcam_auto_recovery_hold_start > HOLD_TIME_3SEC) {
        xcam_auto_recovery_step_loss = ((flag >> 4) & 0x1) != 0;
    }
}

void DevPrintOptions::parse_allow_prompt_sound_status(int flag)
{
    if (time(nullptr) - xcam_prompt_sound_hold_start > HOLD_TIME_3SEC) {
        xcam_allow_prompt_sound = ((flag >> 17) & 0x1) != 0;
    }
}

void DevPrintOptions::parse_filament_tangle_detect_status(int flag)
{
    if (time(nullptr) - xcam_filament_tangle_detect_hold_start > HOLD_TIME_3SEC) {
        xcam_filament_tangle_detect = ((flag >> 20) & 0x1) != 0;
    }
}

int DevPrintOptions::command_xcam_control(std::string module_name, bool on_off , MachineObject *obj, std::string lvl)
{
    json j;
    j["xcam"]["command"]     = "xcam_control_set";
    j["xcam"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["xcam"]["module_name"] = module_name;
    j["xcam"]["control"]     = on_off;
    j["xcam"]["enable"]      = on_off; // old protocol
    j["xcam"]["print_halt"]  = true;   // old protocol
    if (!lvl.empty()) { j["xcam"]["halt_print_sensitivity"] = lvl; }
    BOOST_LOG_TRIVIAL(info) << "command:xcam_control_set" << ", module_name:" << module_name << ", control:" << on_off << ", halt_print_sensitivity:" << lvl;
    return obj->publish_json(j);
}

int DevPrintOptions::command_set_against_continued_heating_mode(bool on_off)
{
    json j;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"]     = "set_against_continued_heating_mode";
    j["print"]["enable"]      = on_off;
    return m_obj->publish_json(j);
}

int DevPrintOptions::command_set_printing_option(bool auto_recovery, MachineObject *obj)
{
    json j;
    j["print"]["command"]       = "print_option";
    j["print"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["option"]        = (int) auto_recovery;
    j["print"]["auto_recovery"] = auto_recovery;

    return obj->publish_json(j);
}

int DevPrintOptions::command_set_prompt_sound(bool prompt_sound, MachineObject *obj)
{
    json j;
    j["print"]["command"]      = "print_option";
    j["print"]["sequence_id"]  = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["sound_enable"] = prompt_sound;

    return obj->publish_json(j);
}

int DevPrintOptions::command_set_filament_tangle_detect(bool filament_tangle_detect, MachineObject *obj)
{
    json j;
    j["print"]["command"]                = "print_option";
    j["print"]["sequence_id"]            = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["filament_tangle_detect"] = filament_tangle_detect;

    return obj->publish_json(j);
}







}
// namespace Slic3r