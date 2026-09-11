#include "DevPrintOptions.h"
#include "DevUtil.h"

#include <cassert>

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r
{

DevPrintOptions::DevPrintOptions(MachineObject* obj) : m_obj(obj)
{
    m_detection_list = {
        {PrintOptionEnum::AI_Monitoring, &m_ai_monitoring_detection},
        {PrintOptionEnum::First_Layer_Detection, &m_first_layer_detection},
        {PrintOptionEnum::Buildplate_Mark_Detection, &m_buildplate_mark_detection},
        {PrintOptionEnum::Buildplate_Align_Detection, &m_buildplate_align_detection},
        {PrintOptionEnum::Auto_Recovery_Detection, &m_auto_recovery_detection},
        {PrintOptionEnum::Allow_Prompt_Sound_Detection, &m_allow_prompt_sound_detection},
        {PrintOptionEnum::Filament_Tangle_Detection, &m_filament_tangle_detection},
        {PrintOptionEnum::Idle_Heating_Protect_Detection, &m_idel_heating_protect_detection},
        {PrintOptionEnum::Purify_Air_At_Print_End, &m_purify_air_at_print_end},
        {PrintOptionEnum::Snapshot_Detection, &m_snapshot_detection},
        {PrintOptionEnum::FOD_Check_Detection, &m_fod_check_detection},
        {PrintOptionEnum::Displacement_Detection, &m_displacement_detection},
        {PrintOptionEnum::Smart_Nozzle_Blob_Detection, &m_smart_nozzle_blob_detection},
    };
}

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

        // Orca: xcam.cfg is an integer bitfield (distinct from the top-level cfg string) that
        //       carries the plate-align/FOD/displacement current values.
        if (print_json.contains("xcam") && print_json["xcam"].contains("cfg")) {
            int xcam_cfg = print_json["xcam"]["cfg"].get<int>();
            if (time(nullptr) - opts->m_buildplate_align_detection.detect_hold_start > HOLD_TIME_3SEC)
                opts->m_buildplate_align_detection.current_detect_value = DevUtil::get_flag_bits(xcam_cfg, 20);
            if (time(nullptr) - opts->m_fod_check_detection.detect_hold_start > HOLD_TIME_3SEC)
                opts->m_fod_check_detection.current_detect_value = DevUtil::get_flag_bits(xcam_cfg, 21);
            if (time(nullptr) - opts->m_displacement_detection.detect_hold_start > HOLD_TIME_3SEC)
                opts->m_displacement_detection.current_detect_value = DevUtil::get_flag_bits(xcam_cfg, 22);
        }

        // Orca: fun2 support bits (distinct from the top-level cfg/fun fields). ORCA's DevUtil has no
        //       no-border extractor, so the MachineObject helper is used here instead.
        if (print_json.contains("fun2") && print_json["fun2"].is_string()) {
            std::string fun2 = print_json["fun2"].get<std::string>();
            if (!fun2.empty()) {
                opts->m_buildplate_align_detection.is_support_detect  = opts->m_obj->get_flag_bits_no_border(fun2, 2) == 1;
                opts->m_purify_air_at_print_end.is_support_detect     = opts->m_obj->get_flag_bits_no_border(fun2, 4);
                opts->m_fod_check_detection.is_support_detect         = opts->m_obj->get_flag_bits_no_border(fun2, 13);
                opts->m_displacement_detection.is_support_detect      = opts->m_obj->get_flag_bits_no_border(fun2, 14);
                opts->m_smart_nozzle_blob_detection.is_support_detect = opts->m_obj->get_flag_bits_no_border(fun2, 15);
            }
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
            if (time(nullptr) - opts->m_ai_monitoring_detection.detect_hold_start > HOLD_TIME_3SEC) {
                if (print_json["xcam"].contains("printing_monitor")) {
                    // new protocol
                    opts->m_ai_monitoring_detection.current_detect_value = print_json["xcam"]["printing_monitor"].get<bool>();
                } else {
                    // old version protocol
                    if (print_json["xcam"].contains("spaghetti_detector")) {
                        opts->m_ai_monitoring_detection.current_detect_value = print_json["xcam"]["spaghetti_detector"].get<bool>();
                        if (print_json["xcam"].contains("print_halt")) {
                            bool print_halt = print_json["xcam"]["print_halt"].get<bool>();
                            if (print_halt) { opts->m_ai_monitoring_detection.current_detect_sensitivity_value = "medium"; }
                        }
                    }
                }
                if (print_json["xcam"].contains("halt_print_sensitivity")) {
                    opts->m_ai_monitoring_detection.current_detect_sensitivity_value = print_json["xcam"]["halt_print_sensitivity"].get<std::string>();
                }
            }

            if (time(nullptr) - opts->m_first_layer_detection.detect_hold_start > HOLD_TIME_3SEC) {
                if (print_json["xcam"].contains("first_layer_inspector")) { opts->m_first_layer_detection.current_detect_value = print_json["xcam"]["first_layer_inspector"].get<bool>(); }
            }

            if (time(nullptr) - opts->m_buildplate_mark_detection.detect_hold_start > HOLD_TIME_3SEC) {
                if (print_json["xcam"].contains("buildplate_marker_detector")) {
                    opts->m_buildplate_mark_detection.current_detect_value    = print_json["xcam"]["buildplate_marker_detector"].get<bool>();
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
        if (time(nullptr) - opts->m_first_layer_detection.detect_hold_start > HOLD_TIME_3SEC) {
            opts->m_first_layer_detection.current_detect_value = enable;
        }
    } else if (print_json["module_name"].get<std::string>() == "buildplate_marker_detector") {
        if (time(nullptr) - opts->m_buildplate_mark_detection.detect_hold_start > HOLD_TIME_3SEC) {
            opts->m_buildplate_mark_detection.current_detect_value = enable;
        }
    } else if (print_json["module_name"].get<std::string>() == "printing_monitor") {
        if (time(nullptr) - opts->m_ai_monitoring_detection.detect_hold_start > HOLD_TIME_3SEC) {
            opts->m_ai_monitoring_detection.current_detect_value = enable;
            if (print_json.contains("halt_print_sensitivity")) {
                opts->m_ai_monitoring_detection.current_detect_sensitivity_value = print_json["halt_print_sensitivity"].get<std::string>();
            }
        }
    } else if (print_json["module_name"].get<std::string>() == "spaghetti_detector") {
        if (time(nullptr) - opts->m_ai_monitoring_detection.detect_hold_start > HOLD_TIME_3SEC) {
            // old protocol
            opts->m_ai_monitoring_detection.current_detect_value = enable;
            if (print_json.contains("print_halt")) {
                if (print_json["print_halt"].get<bool>()) { opts->m_ai_monitoring_detection.current_detect_sensitivity_value = "medium"; }
            }
        }
    }
}

void DevPrintOptionsParser::ParseDetectionV1_2(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json) {

    try {
        if (print_json.contains("option")) {
            if (print_json["option"].is_number()) {
                int option = print_json["option"].get<int>();
                if (time(nullptr) - opts->m_auto_recovery_detection.detect_hold_start > HOLD_TIME_3SEC) { opts->m_auto_recovery_detection.current_detect_value = ((option & 0x01) != 0); }
            }
        }

        if (time(nullptr) - opts->m_auto_recovery_detection.detect_hold_start > HOLD_TIME_3SEC) {
            if (print_json.contains("auto_recovery")) { opts->m_auto_recovery_detection.current_detect_value = print_json["auto_recovery"].get<bool>(); }
        }
    } catch (...) {}

}

void DevPrintOptionsParser::ParseDetectionV2_0(DevPrintOptions *opts, std::string print_json)
{

    if (time(nullptr) - opts->m_first_layer_detection.detect_hold_start > HOLD_TIME_3SEC) {
        opts->m_first_layer_detection.current_detect_value = DevUtil::get_flag_bits(print_json, 12);
    }

    if (time(nullptr) - opts->m_ai_monitoring_detection.detect_hold_start > HOLD_COUNT_MAX) {
        opts->m_ai_monitoring_detection.current_detect_value = DevUtil::get_flag_bits(print_json, 15);

        switch (DevUtil::get_flag_bits(print_json, 13, 2)) {
        case 0: opts->m_ai_monitoring_detection.current_detect_sensitivity_value = "never_halt"; break;
        case 1: opts->m_ai_monitoring_detection.current_detect_sensitivity_value = "low"; break;
        case 2: opts->m_ai_monitoring_detection.current_detect_sensitivity_value = "medium"; break;
        case 3: opts->m_ai_monitoring_detection.current_detect_sensitivity_value = "high"; break;
        default: break;
        }
    }

    if (time(nullptr) - opts->m_auto_recovery_detection.detect_hold_start > HOLD_COUNT_MAX){
        opts->m_auto_recovery_detection.current_detect_value = DevUtil::get_flag_bits(print_json, 16);
    }

    if (time(nullptr) - opts->m_allow_prompt_sound_detection.detect_hold_start > HOLD_TIME_3SEC) {
        opts->m_allow_prompt_sound_detection.current_detect_value = DevUtil::get_flag_bits(print_json, 22);
    }

    if (time(nullptr) - opts->m_filament_tangle_detection.detect_hold_start > HOLD_TIME_3SEC) {
        opts->m_filament_tangle_detection.current_detect_value = DevUtil::get_flag_bits(print_json, 23);
    }
}

void DevPrintOptionsParser::ParseDetectionV2_1(DevPrintOptions *opts, std::string cfg) {
    if (time(nullptr) - opts->m_idel_heating_protect_detection.detect_hold_start > HOLD_TIME_3SEC)
        opts->m_idel_heating_protect_detection.current_detect_value = DevUtil::get_flag_bits(cfg, 32, 2);

    // Orca: further top-level cfg-string detection bits, parsed here alongside idle-heating.
    if (time(nullptr) - opts->m_purify_air_at_print_end.detect_hold_start > HOLD_TIME_3SEC)
        opts->m_purify_air_at_print_end.current_detect_value = DevUtil::get_flag_bits(cfg, 36, 2);

    // Smart nozzle blob detection mode: cfg bits 43-44 (2 bits), 0=off, 1=on, 2=auto
    if (time(nullptr) - opts->m_smart_nozzle_blob_detection.detect_hold_start > HOLD_TIME_3SEC)
        opts->m_smart_nozzle_blob_detection.current_detect_value = DevUtil::get_flag_bits(cfg, 43, 2);

    if (time(nullptr) - opts->m_snapshot_detection.detect_hold_start > HOLD_TIME_3SEC) {
        opts->m_snapshot_detection.current_detect_value = DevUtil::get_flag_bits(cfg, 38, 2);
        opts->m_snapshot_detection.is_support_detect =
            opts->m_snapshot_detection.current_detect_value != 0 && opts->m_snapshot_detection.current_detect_value != 3;
    }
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

PrintOptionData* DevPrintOptions::GetDetectionOption(PrintOptionEnum print_option)
{
    auto it = m_detection_list.find(print_option);
    if (it != m_detection_list.end())
    {
        return it->second;
    }
    return nullptr;
}

int DevPrintOptions::command_xcam_control_ai_monitoring(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false : true;

    m_ai_monitoring_detection.current_detect_value             = on_off;
    m_ai_monitoring_detection.detect_hold_start                = time(nullptr);
    m_ai_monitoring_detection.current_detect_sensitivity_value = lvl;
    return command_xcam_control("printing_monitor", on_off, m_obj, lvl);
}

int DevPrintOptions::command_xcam_control_idelheatingprotect_detector(bool on_off)
{
    m_idel_heating_protect_detection.current_detect_value = on_off;
    m_idel_heating_protect_detection.detect_hold_start    = time(nullptr);
    return command_set_against_continued_heating_mode(on_off);
}

int DevPrintOptions::command_xcam_control_buildplate_marker_detector(bool on_off)
{
    m_buildplate_mark_detection.current_detect_value = on_off;
    m_buildplate_mark_detection.detect_hold_start    = time(nullptr);
    return command_xcam_control("buildplate_marker_detector", on_off ,m_obj);
}

int DevPrintOptions::command_xcam_control_first_layer_inspector(bool on_off, bool print_halt)
{
    m_first_layer_detection.current_detect_value = on_off;
    m_first_layer_detection.detect_hold_start    = time(nullptr);
    return command_xcam_control("first_layer_inspector", on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_auto_recovery_step_loss(bool on_off)
{
    m_auto_recovery_detection.current_detect_value = on_off;
    m_auto_recovery_detection.detect_hold_start    = time(nullptr);
    return command_set_printing_option(on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_allow_prompt_sound(bool on_off)
{
    m_allow_prompt_sound_detection.current_detect_value = on_off;
    m_allow_prompt_sound_detection.detect_hold_start    = time(nullptr);
    return command_set_prompt_sound(on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_filament_tangle_detect(bool on_off)
{
    m_filament_tangle_detection.current_detect_value = on_off;
    m_filament_tangle_detection.detect_hold_start    = time(nullptr);
    return command_set_filament_tangle_detect(on_off, m_obj);
}

void DevPrintOptions::parse_auto_recovery_step_loss_status(int flag) {
    if (time(nullptr) - m_auto_recovery_detection.detect_hold_start > HOLD_TIME_3SEC) {
        m_auto_recovery_detection.current_detect_value = ((flag >> 4) & 0x1) != 0;
    }
}

void DevPrintOptions::parse_allow_prompt_sound_status(int flag)
{
    if (time(nullptr) - m_allow_prompt_sound_detection.detect_hold_start > HOLD_TIME_3SEC) {
        m_allow_prompt_sound_detection.current_detect_value = ((flag >> 17) & 0x1) != 0;
    }
}

void DevPrintOptions::parse_filament_tangle_detect_status(int flag)
{
    if (time(nullptr) - m_filament_tangle_detection.detect_hold_start > HOLD_TIME_3SEC) {
        m_filament_tangle_detection.current_detect_value = ((flag >> 20) & 0x1) != 0;
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

int DevPrintOptions::command_xcam_control_build_plate_align_detector(bool on_off)
{
    m_buildplate_align_detection.current_detect_value = on_off;
    m_buildplate_align_detection.detect_hold_start    = time(nullptr);
    return command_xcam_control("plate_offset_switch", on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_fod_check(bool on_off)
{
    m_fod_check_detection.current_detect_value = on_off;
    m_fod_check_detection.detect_hold_start    = time(nullptr);
    return command_xcam_control("fod_check", on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_displacement_detection(bool on_off)
{
    m_displacement_detection.current_detect_value = on_off;
    m_displacement_detection.detect_hold_start    = time(nullptr);
    return command_xcam_control("model_movement_check", on_off, m_obj);
}

int DevPrintOptions::command_xcam_control_purify_air_at_print_end(int on_off)
{
    m_purify_air_at_print_end.current_detect_value = on_off;
    m_purify_air_at_print_end.detect_hold_start    = time(nullptr);
    return command_set_purify_air_at_print_end((PurifyAirAtPrintEndState) on_off, m_obj);
}

int DevPrintOptions::command_set_purify_air_at_print_end(PurifyAirAtPrintEndState state, MachineObject *obj)
{
    json j;
    j["print"]["command"]     = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    switch (state)
    {
        case PurifyAirAtPrintEndState::PurifyAirDisable:   j["print"]["air_purification"] = 0; break;
        case PurifyAirAtPrintEndState::PurifyAirByInside:  j["print"]["air_purification"] = 1; break;
        case PurifyAirAtPrintEndState::PurifyAirByOutside: j["print"]["air_purification"] = 2; break;
        default: assert(0);
    }
    return obj->publish_json(j);
}

int DevPrintOptions::command_snapshot_control(int on_off)
{
    m_snapshot_detection.current_detect_value = on_off;
    m_snapshot_detection.detect_hold_start    = time(nullptr);
    return command_set_snapshot_control(on_off, m_obj);
}

int DevPrintOptions::command_set_snapshot_control(int on_off, MachineObject *obj)
{
    json j;
    j["camera"]["command"]     = "ipcam_cap_pic_set";
    j["camera"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["camera"]["control"]     = on_off ? "enable" : "disable";
    return obj->publish_json(j);
}

int DevPrintOptions::command_smart_nozzle_blob_detect_mode(int mode)
{
    json j;
    j["print"]["command"]               = "print_option";
    j["print"]["sequence_id"]           = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_blob_detect_v2"] = mode;  // int: 0=off, 1=on, 2=auto
    m_smart_nozzle_blob_detection.current_detect_value = mode;
    m_smart_nozzle_blob_detection.detect_hold_start    = time(nullptr);
    return m_obj->publish_json(j);
}

}
// namespace Slic3r
