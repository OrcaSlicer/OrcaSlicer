#include "libslic3r/libslic3r.h"
#include "DeviceManager.hpp"
#include "libslic3r/Time.hpp"
#include "libslic3r/Thread.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"

#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "GUI_App.hpp"
#include "ReleaseNote.hpp"
#include <thread>
#include <mutex>
#include <codecvt>
#include <boost/foreach.hpp>
#include <boost/typeof/typeof.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "fast_float/fast_float.h"
#include <wx/dir.h>

#define CALI_DEBUG
#define MINUTE_30 1800000    //ms
#define TIME_OUT  5000       //ms

#define ORCA_NETWORK_DEBUG

namespace pt = boost::property_tree;

float string_to_float(const std::string& str_value) {
    float value = 0.0;
    fast_float::from_chars(str_value.c_str(), str_value.c_str() + str_value.size(), value);
    return value;
}

wxString get_stage_string(int stage)
{
    switch(stage) {
    case 0:
        //return _L("Printing");
        return "";
    case 1:
        return _L("Auto bed leveling");
    case 2:
        return _L("Heatbed preheating");
    case 3:
        return _L("Sweeping XY mech mode");
    case 4:
        return _L("Changing filament");
    case 5:
        return _L("M400 pause");
    case 6:
        return _L("Paused due to filament runout");
    case 7:
        return _L("Heating hotend");
    case 8:
        return _L("Calibrating extrusion");
    case 9:
        return _L("Scanning bed surface");
    case 10:
        return _L("Inspecting first layer");
    case 11:
        return _L("Identifying build plate type");
    case 12:
        return _L("Calibrating Micro Lidar");
    case 13:
        return _L("Homing toolhead");
    case 14:
        return _L("Cleaning nozzle tip");
    case 15:
        return _L("Checking extruder temperature");
    case 16:
        return _L("Printing was paused by the user");
    case 17:
        return _L("Pause of front cover falling");
    case 18:
        return _L("Calibrating the micro lidar");
    case 19:
        return _L("Calibrating extrusion flow");
    case 20:
        return _L("Paused due to nozzle temperature malfunction");
    case 21:
        return _L("Paused due to heat bed temperature malfunction");
    case 22:
        return _L("Filament unloading");
    case 23:
        return _L("Skip step pause");
    case 24:
        return _L("Filament loading");
    case 25:
        return _L("Motor noise calibration");
    case 26:
        return _L("Paused due to AMS lost");
    case 27:
        return _L("Paused due to low speed of the heat break fan");
    case 28:
        return _L("Paused due to chamber temperature control error");
    case 29:
        return _L("Cooling chamber");
    case 30:
        return _L("Paused by the G-code inserted by user");
    case 31:
        return _L("Motor noise showoff");
    case 32:
        return _L("Nozzle filament covered detected pause");
    case 33:
        return _L("Cutter error pause");
    case 34:
        return _L("First layer error pause");
    case 35:
        return _L("Nozzle clog pause");
    default:
        ;
    }
    return "";
}

std::string to_string_nozzle_diameter(float nozzle_diameter)
{
    float eps = 1e-3;
    if (abs(nozzle_diameter - 0.2) < eps) {
        return "0.2";
    }
    else if (abs(nozzle_diameter - 0.4) < eps) {
        return "0.4";
    }
    else if (abs(nozzle_diameter - 0.6) < eps) {
        return "0.6";
    }
    else if (abs(nozzle_diameter - 0.8) < eps) {
        return "0.8";
    }
    return "0";
}

void sanitizeToUtf8(std::string& str) {
    std::string result;
    size_t i = 0;

    while (i < str.size()) {
        unsigned char c = str[i];
        size_t remainingBytes = 0;
        bool valid = true;

        if ((c & 0x80) == 0x00) { // 1-byte character (ASCII)
            remainingBytes = 0;
        }
        else if ((c & 0xE0) == 0xC0) { // 2-byte character
            remainingBytes = 1;
        }
        else if ((c & 0xF0) == 0xE0) { // 3-byte character
            remainingBytes = 2;
        }
        else if ((c & 0xF8) == 0xF0) { // 4-byte character
            remainingBytes = 3;
        }
        else {
            valid = false; // Invalid first byte
        }

        if (valid && i + remainingBytes < str.size()) {
            for (size_t j = 1; j <= remainingBytes; ++j) {
                if ((str[i + j] & 0xC0) != 0x80) {
                    valid = false; // Invalid continuation byte
                    break;
                }
            }
        }
        else {
            valid = false; // Truncated character
        }

        if (valid) {
            // Append valid UTF-8 character
            result.append(str, i, remainingBytes + 1);
            i += remainingBytes + 1;
        }
        else {
            // Replace invalid character with space
            result += ' ';
            ++i; // Skip the invalid byte
        }
    }
    str = std::move(result);
}

namespace Slic3r {

/* Common Functions */
void split_string(std::string s, std::vector<std::string>& v) {

    std::string t = "";
    for (int i = 0; i < s.length(); ++i) {
        if (s[i] == ',') {
            v.push_back(t);
            t = "";
        }
        else {
            t.push_back(s[i]);
        }
    }
    v.push_back(t);
}

wxString generate_nozzle_id(NozzleVolumeType nozzle_type, const std::string& diameter)
{
    // HS00-0.4
    std::string nozzle_id = "H";
    switch (nozzle_type) {
    case NozzleVolumeType::nvtNormal: {
        nozzle_id += "S";
        break;
    }
    case NozzleVolumeType::nvtBigTraffic: {
        nozzle_id += "H";
        break;
    }
    default:
        nozzle_id += "H";
        break;
    }
    nozzle_id += "00";
    nozzle_id += "-";
    nozzle_id += diameter;
    return nozzle_id;
}

NozzleVolumeType convert_to_nozzle_type(const std::string &str)
{
    if (str.size() < 8) {
        assert(false);
        return NozzleVolumeType::nvtNormal;
    }
    NozzleVolumeType res = NozzleVolumeType::nvtNormal;
    if (str[1] == 'S')
        res = NozzleVolumeType::nvtNormal;
    else if (str[1] == 'H')
        res = NozzleVolumeType::nvtBigTraffic;
    return res;
}

PrinterArch get_printer_arch_by_str(std::string arch_str)
{
    if (arch_str == "i3") {
        return PrinterArch::ARCH_I3;
    }
    else if (arch_str == "core_xy") {
        return PrinterArch::ARCH_CORE_XY;
    }

    return PrinterArch::ARCH_CORE_XY;
}

void AmsTray::update_color_from_str(std::string color)
{
    if (color.empty()) return;

    if (this->color.compare(color) == 0)
        return;

    wx_color = "#" + wxString::FromUTF8(color);
    this->color = color;
}

wxColour AmsTray::get_color()
{
    return AmsTray::decode_color(color);
}

void AmsTray::reset()
{
    tag_uid = "";
    setting_id = "";
    filament_setting_id = "";
    type = "";
    sub_brands = "";
    color = "";
    weight = "";
    diameter = "";
    temp = "";
    time = "";
    bed_temp_type = "";
    bed_temp = "";
    nozzle_temp_max = "";
    nozzle_temp_min = "";
    xcam_info = "";
    uuid = "";
    k = 0.0f;
    n = 0.0f;
    is_bbl = false;
    hold_count = 0;
    remain = 0;
}


bool AmsTray::is_tray_info_ready()
{
    if (color.empty())
        return false;
    if (type.empty())
        return false;
    //if (setting_id.empty())
        //return false;
    return true;
}

bool AmsTray::is_unset_third_filament()
{
    if (this->is_bbl)
        return false;

    if (color.empty() || type.empty())
        return true;
    return false;
}

std::string AmsTray::get_display_filament_type()
{
    if (type == "PLA-S")
        return "Sup.PLA";
    else if (type == "PA-S")
        return "Sup.PA";
    else
        return type;
    return type;
}

std::string AmsTray::get_filament_type()
{
    if (type == "Sup.PLA") {
        return "PLA-S";
    } else if (type == "Sup.PA") {
        return "PA-S";
    } else if (type == "Support W") {
        return "PLA-S";
    } else if (type == "Support G") {
        return "PA-S";
    } else if (type == "Support") {
        if (setting_id == "GFS00") {
            type = "PLA-S";
        } else if (setting_id == "GFS01") {
            type = "PA-S";
        } else {
            return "PLA-S";
        }
    } else {
        return type;
    }
    return type;
}

bool HMSItem::parse_hms_info(unsigned attr, unsigned code)
{
    bool result = true;
    unsigned int model_id_int = (attr >> 24) & 0xFF;
    this->module_id = (ModuleID)model_id_int;
    this->module_num = (attr >> 16) & 0xFF;
    this->part_id    = (attr >> 8) & 0xFF;
    this->reserved   = (attr >> 0) & 0xFF;
    unsigned msg_level_int = code >> 16;
    if (msg_level_int < (unsigned)HMS_MSG_LEVEL_MAX)
        this->msg_level = (HMSMessageLevel)msg_level_int;
    else
        this->msg_level = HMS_UNKNOWN;
    this->msg_code = code & 0xFFFF;
    return result;
}

std::string HMSItem::get_long_error_code()
{
    char buf[64];
    ::sprintf(buf, "%02X%02X%02X00000%1X%04X",
        this->module_id,
        this->module_num,
        this->part_id,
        (int)this->msg_level,
        this->msg_code);
    return std::string(buf);
}

wxString HMSItem::get_module_name(ModuleID module_id)
{
    switch (module_id)
    {
    case MODULE_MC:
        return "MC";
    case MODULE_MAINBOARD:
        return "MainBoard";
    case MODULE_AMS:
        return "AMS";
    case MODULE_TH:
        return "TH";
    case MODULE_XCAM:
        return "XCam";
    default:
        wxString text = _L("Unknown") + wxString::Format("0x%x", (unsigned)module_id);
        return text;
    }
    return "";
}

wxString HMSItem::get_hms_msg_level_str(HMSMessageLevel level)
{
    switch(level) {
    case HMS_FATAL:
        return _L("Fatal");
    case HMS_SERIOUS:
        return _L("Serious");
    case HMS_COMMON:
        return _L("Common");
    case HMS_INFO:
        return _L("Info");
    default:
        return _L("Unknown");
    }
    return "";
}

std::string MachineObject::parse_printer_type(std::string type_str)
{
    if (type_str.compare("3DPrinter-X1") == 0) {
        return "BL-P002";
    } else if (type_str.compare("3DPrinter-X1-Carbon") == 0) {
        return "BL-P001";
    } else if (type_str.compare("BL-P001") == 0) {
        return type_str;
    } else if (type_str.compare("BL-P002") == 0) {
        return type_str;
    } else {
        return DeviceManager::parse_printer_type(type_str);
    }
    return "";
}
std::string MachineObject::get_preset_printer_model_name(std::string printer_type)
{
    return DeviceManager::get_printer_display_name(printer_type);
}

std::string MachineObject::get_preset_printer_thumbnail_img(std::string printer_type)
{
    return DeviceManager::get_printer_thumbnail_img(printer_type);
}

wxString MachineObject::get_printer_type_display_str()
{
    std::string display_name =  get_preset_printer_model_name(printer_type);
    if (!display_name.empty())
        return display_name;
    else
        return _L("Unknown");
}

std::string MachineObject::get_printer_thumbnail_img_str()
{
    std::string img_str = get_preset_printer_thumbnail_img(printer_type);
    std::string img_url;

    if (!img_str.empty()) {
        img_url =  Slic3r::resources_dir() + "\\printers\\image\\" + img_str;
        if (fs::exists(img_url + ".svg")) {
            return img_url;
        }
        else {
            img_url = img_str;
        }
    }
    else {
        img_url =  "printer_thumbnail";
    }

    return img_url;
}

std::string MachineObject::get_ftp_folder()
{
    return DeviceManager::get_ftp_folder(printer_type);
}

std::string MachineObject::get_access_code() const
{
    if (get_user_access_code().empty())
        return access_code;
    return get_user_access_code();
}

void MachineObject::set_access_code(std::string code, bool only_refresh)
{
    this->access_code = code;
    if (only_refresh) {
        AppConfig* config = GUI::wxGetApp().app_config;
        if (config && !code.empty()) {
            GUI::wxGetApp().app_config->set_str("access_code", dev_id, code);
            DeviceManager::update_local_machine(*this);
        }
    }
}

void MachineObject::erase_user_access_code()
{
    this->user_access_code = "";
    AppConfig* config = GUI::wxGetApp().app_config;
    if (config) {
        GUI::wxGetApp().app_config->erase("user_access_code", dev_id);
        //GUI::wxGetApp().app_config->save();
    }
}

void MachineObject::set_user_access_code(std::string code, bool only_refresh)
{
    this->user_access_code = code;
    if (only_refresh && !code.empty()) {
        AppConfig* config = GUI::wxGetApp().app_config;
        if (config && !code.empty()) {
            GUI::wxGetApp().app_config->set_str("user_access_code", dev_id, code);
            DeviceManager::update_local_machine(*this);
        }
    }
}

std::string MachineObject::get_user_access_code() const
{
    AppConfig* config = GUI::wxGetApp().app_config;
    if (config) {
        return GUI::wxGetApp().app_config->get("user_access_code", dev_id);
    }
    return "";
}

bool MachineObject::is_lan_mode_printer() const
{
    bool result = false;
    if (!dev_connection_type.empty() && dev_connection_type == "lan")
        return true;
    return result;
}

PrinterSeries MachineObject::get_printer_series() const
{
    std::string series =  DeviceManager::get_printer_series(printer_type);
    if (series == "series_x1" || series == "series_o")
        return PrinterSeries::SERIES_X1;
    else if (series == "series_p1p")
        return PrinterSeries::SERIES_P1P;
    else
        return PrinterSeries::SERIES_P1P;
}

PrinterArch MachineObject::get_printer_arch() const
{
    return DeviceManager::get_printer_arch(printer_type);
}

std::string MachineObject::get_printer_ams_type() const
{
    return DeviceManager::get_printer_ams_type(printer_type);
}

bool MachineObject::get_printer_is_enclosed() const
{
    return DeviceManager::get_printer_is_enclosed(printer_type);
}

void MachineObject::reload_printer_settings()
{
    print_json.load_compatible_settings("", "");
    parse_json("{}");
}

MachineObject::MachineObject(NetworkAgent* agent, std::string name, std::string id, std::string ip)
    :dev_name(name),
    dev_id(id),
    dev_ip(ip),
    subtask_(nullptr),
    model_task(nullptr),
    slice_info(nullptr),
    m_is_online(false),
    vt_tray(std::to_string(VIRTUAL_TRAY_ID))
{
    m_agent = agent;

    reset();

    /* temprature fields */
    bed_temp            = 0.0f;
    bed_temp_target     = 0.0f;
    chamber_temp        = 0.0f;
    chamber_temp_target = 0.0f;
    frame_temp          = 0.0f;

    /* ams fileds */
    ams_exist_bits = 0;
    tray_exist_bits = 0;
    tray_is_bbl_bits = 0;
    ams_rfid_status = 0;
    is_ams_need_update = false;
    ams_insert_flag = false;
    ams_power_on_flag = false;
    ams_calibrate_remain_flag = false;

    /* signals */
    wifi_signal = "";

    /* upgrade */
    upgrade_force_upgrade = false;
    upgrade_new_version = false;
    upgrade_consistency_request = false;

    /* cooling */
    heatbreak_fan_speed = 0;
    cooling_fan_speed = 0;
    big_fan1_speed = 0;
    big_fan2_speed = 0;
    fan_gear = 0;

    /* printing */
    mc_print_stage = 0;
    mc_print_error_code = 0;
    print_error = 0;
    mc_print_line_number = 0;
    mc_print_percent = 0;
    mc_print_sub_stage = 0;
    mc_left_time = 0;
    home_flag = -1;
    hw_switch_state = 0;
    printing_speed_lvl   = PrintingSpeedLevel::SPEED_LEVEL_INVALID;

    has_ipcam = true; // default true

    m_extder_data.current_extder_id     = MAIN_NOZZLE_ID;
    m_extder_data.target_extder_id      = MAIN_NOZZLE_ID;
    m_extder_data.total_extder_count    = 1;
    Extder nozzle;
    nozzle.id               = MAIN_NOZZLE_ID;
    nozzle.nozzle_id        = MAIN_NOZZLE_ID;
    nozzle.target_nozzle_id = MAIN_NOZZLE_ID;
    m_extder_data.extders.push_back(nozzle);
}

MachineObject::~MachineObject()
{
    if (subtask_) {
        delete subtask_;
        subtask_ = nullptr;
    }

    if (model_task) {
        delete model_task;
        model_task = nullptr;
    }

    if (get_slice_info_thread) {
        if (get_slice_info_thread->joinable()) {
            get_slice_info_thread->join();
            get_slice_info_thread = nullptr;
        }
    }

    if (slice_info) {
        delete slice_info;
        slice_info = nullptr;
    }

    for (auto it = amsList.begin(); it != amsList.end(); it++) {
        for (auto tray_it = it->second->trayList.begin(); tray_it != it->second->trayList.end(); tray_it++) {
            if (tray_it->second) {
                delete tray_it->second;
                tray_it->second = nullptr;
            }
        }
        it->second->trayList.clear();
    }
    amsList.clear();
}

bool MachineObject::check_valid_ip()
{
    if (dev_ip.empty()) {
        return false;
    }

    return true;
}

void MachineObject::_parse_print_option_ack(int option)
{
    xcam_auto_recovery_step_loss = ((option >> (int)PRINT_OP_AUTO_RECOVERY) & 0x01) != 0;
}

bool MachineObject::is_in_extrusion_cali()
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_extrusion_cali_start_time);
    if (diff.count() < EXTRUSION_OMIT_TIME) {
        mc_print_percent = 0;
        return true;
    }

    if (is_in_printing_status(print_status)
        && print_type == "system"
        && boost::contains(m_gcode_file, "extrusion_cali")
        )
    {
        return true;
    }
    return false;
}

bool MachineObject::is_extrusion_cali_finished()
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_extrusion_cali_start_time);
    if (diff.count() < EXTRUSION_OMIT_TIME) {
        return false;
    }

    if (boost::contains(m_gcode_file, "extrusion_cali")
        && this->mc_print_percent == 100)
        return true;
    else
        return false;
}

void MachineObject::_parse_tray_now(std::string tray_now)
{
    if (tray_now.empty()) {
        return;
    } else {
        try {
            int tray_now_int = atoi(tray_now.c_str());

            if (tray_now_int == 255) {
                m_ams_id = "0";
                m_tray_id = "0";
                m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id  = "";
                m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id = "";

                if (m_tray_now == std::to_string(255)) {
                    m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id  = std::to_string(255);
                    m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id = "0";
                }
            }
            else if (tray_now_int == 254) {
                m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id  = std::to_string(254);
                m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id = "0";
            }
            else {
                if (tray_now_int >= 0x80 &&  tray_now_int <= 0x87) {
                    m_ams_id = std::to_string(tray_now_int);
                } else {
                    m_ams_id = std::to_string(tray_now_int >> 2);
                }

                m_tray_id                                          = std::to_string(tray_now_int & 0x3);
                m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id  = m_ams_id;
                m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id = m_tray_id;
            }
        }
        catch(...) {
        }
    }

    m_tray_now = tray_now;
}

Ams *MachineObject::get_curr_Ams()
{
    auto it = amsList.find(m_ams_id);
    if (it != amsList.end())
        return it->second;
    return nullptr;
}

AmsTray *MachineObject::get_curr_tray()
{
    if (m_tray_now.compare(std::to_string(VIRTUAL_TRAY_ID)) == 0) {
        return &vt_tray;
    }

    Ams* curr_ams = get_curr_Ams();
    if (!curr_ams) return nullptr;

    try {
        int tray_index = atoi(m_tray_now.c_str());
        int ams_index = atoi(curr_ams->id.c_str());

        std::string tray_now_index = std::to_string(tray_index - ams_index * 4);
        auto it = curr_ams->trayList.find(tray_now_index);
        if (it != curr_ams->trayList.end())
            return it->second;
    }
    catch (...) {
        ;
    }

    return nullptr;
}

AmsTray *MachineObject::get_ams_tray(std::string ams_id, std::string tray_id)
{
    auto it = amsList.find(ams_id);
    if (it == amsList.end()) return nullptr;
    if (!it->second) return nullptr;

    auto iter = it->second->trayList.find(tray_id);
    if (iter != it->second->trayList.end())
        return iter->second;
    else
        return nullptr;
}

void MachineObject::_parse_ams_status(int ams_status)
{
    ams_status_sub = ams_status & 0xFF;
    int ams_status_main_int = (ams_status & 0xFF00) >> 8;
    if (ams_status_main_int == (int)AmsStatusMain::AMS_STATUS_MAIN_IDLE) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_IDLE;
    } else if (ams_status_main_int == (int) AmsStatusMain::AMS_STATUS_MAIN_FILAMENT_CHANGE) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_FILAMENT_CHANGE;
    } else if (ams_status_main_int == (int) AmsStatusMain::AMS_STATUS_MAIN_RFID_IDENTIFYING) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_RFID_IDENTIFYING;
    } else if (ams_status_main_int == (int) AmsStatusMain::AMS_STATUS_MAIN_ASSIST) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_ASSIST;
    } else if (ams_status_main_int == (int) AmsStatusMain::AMS_STATUS_MAIN_CALIBRATION) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_CALIBRATION;
    } else if (ams_status_main_int == (int) AmsStatusMain::AMS_STATUS_MAIN_SELF_CHECK) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_SELF_CHECK;
    } else if (ams_status_main_int == (int) AmsStatusMain::AMS_STATUS_MAIN_DEBUG) {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_DEBUG;
    } else {
        ams_status_main = AmsStatusMain::AMS_STATUS_MAIN_UNKNOWN;
    }

    BOOST_LOG_TRIVIAL(trace) << "ams_debug: main = " << ams_status_main_int << ", sub = " << ams_status_sub;
}

bool MachineObject::can_unload_filament()
{
    bool result = false;
    if (!has_ams())
        return true;

    if (ams_status_main == AMS_STATUS_MAIN_IDLE && hw_switch_state == 1 && m_tray_now == "255") {
        return true;
    }
    return result;
}

bool MachineObject::is_support_ams_mapping()
{
    return true;
}

static float calc_color_distance(wxColour c1, wxColour c2)
{
    float lab[2][3];
    RGB2Lab(c1.Red(), c1.Green(), c1.Blue(), &lab[0][0], &lab[0][1], &lab[0][2]);
    RGB2Lab(c2.Red(), c2.Green(), c2.Blue(), &lab[1][0], &lab[1][1], &lab[1][2]);

    return DeltaE76(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
}

void MachineObject::get_ams_colors(std::vector<wxColour> &ams_colors) {
    ams_colors.clear();
    ams_colors.reserve(amsList.size());
    for (auto ams = amsList.begin(); ams != amsList.end(); ams++) {
        for (auto tray = ams->second->trayList.begin(); tray != ams->second->trayList.end(); tray++) {
            if (tray->second->is_tray_info_ready()) {
                auto ams_color = AmsTray::decode_color(tray->second->color);
                ams_colors.emplace_back(ams_color);
            }
        }
    }
}

int MachineObject::ams_filament_mapping(std::vector<FilamentInfo> filaments, std::vector<FilamentInfo> &result, std::vector<int> exclude_id)
{
    if (filaments.empty())
        return -1;

    // tray_index : tray_color
    std::map<int, FilamentInfo> tray_filaments;
    for (auto ams = amsList.begin(); ams != amsList.end(); ams++) {

        std::string ams_id = ams->second->id;

        for (auto tray = ams->second->trayList.begin(); tray != ams->second->trayList.end(); tray++) {
            int ams_id = atoi(ams->first.c_str());
            int tray_id = atoi(tray->first.c_str());
            int tray_index = ams_id * 4 + tray_id;
            // skip exclude id
            for (int i = 0; i < exclude_id.size(); i++) {
                if (tray_index == exclude_id[i])
                    continue;
            }
            // push
            if (tray->second->is_tray_info_ready()) {
                FilamentInfo info;
                info.color = tray->second->color;
                info.type = tray->second->get_filament_type();
                info.id = tray_index;
                info.filament_id = tray->second->setting_id;
                info.ctype = tray->second->ctype;
                info.colors = tray->second->cols;

                /*for new ams mapping*/
                info.ams_id = ams->first.c_str();
                info.slot_id = tray->first.c_str();

                tray_filaments.emplace(std::make_pair(tray_index, info));
            }
        }
    }

    // tray info list
    std::vector<FilamentInfo> tray_info_list;
    int flament_index_id = 0;
    for (auto ams = amsList.begin(); ams != amsList.end(); ams++) {
        for (auto tray = ams->second->trayList.begin(); tray != ams->second->trayList.end(); tray++) {

            FilamentInfo info;
            info.id = flament_index_id;
            info.tray_id = flament_index_id;
            info.color = tray->second->color;
            info.type = tray->second->get_filament_type();
            info.ctype = tray->second->ctype;
            info.colors = tray->second->cols;


            /*for new ams mapping*/
            info.ams_id = ams->second->id;
            info.slot_id = tray->second->id;

            tray_info_list.push_back(info);
            flament_index_id++;
        }
    }


    // is_support_ams_mapping
    if (!is_support_ams_mapping()) {
        BOOST_LOG_TRIVIAL(info) << "ams_mapping: do not support, use order mapping";
        result.clear();
        for (int i = 0; i < filaments.size(); i++) {
            FilamentInfo info;
            info.id = filaments[i].id;
            int ams_id = filaments[i].id / 4;
            auto ams_it = amsList.find(std::to_string(ams_id));
            if (ams_it == amsList.end()) {
                info.tray_id = -1;
                info.mapping_result = (int)MappingResult::MAPPING_RESULT_EXCEED;
            } else {
                info.tray_id = filaments[i].id;
                int tray_id = filaments[i].id % 4;
                auto tray_it = ams_it->second->trayList.find(std::to_string(tray_id));
                if (tray_it != ams_it->second->trayList.end()) {
                    if (!tray_it->second->is_exists || tray_it->second->is_unset_third_filament()) {
                        ;
                    } else {
                        if (filaments[i].type == tray_it->second->get_filament_type()) {
                            info.color = tray_it->second->color;
                            info.type = tray_it->second->get_filament_type();
                            info.ctype = tray_it->second->ctype;
                            std::vector<wxColour> cols;
                            info.colors = tray_it->second->cols;
                        } else {
                            info.tray_id = -1;
                            info.mapping_result = (int)MappingResult::MAPPING_RESULT_TYPE_MISMATCH;
                        }
                    }
                }
            }
            result.push_back(info);
        }
        return 1;
    }

    char buffer[256];
    std::vector<std::vector<DisValue>> distance_map;

    // print title
    ::sprintf(buffer, "F(id)");
    std::string line = std::string(buffer);
    for (auto tray = tray_filaments.begin(); tray != tray_filaments.end(); tray++) {
        ::sprintf(buffer, "   AMS%02d", tray->second.id+1);
        line += std::string(buffer);
    }
    BOOST_LOG_TRIVIAL(info) << "ams_mapping_distance:" << line;

    for (int i = 0; i < filaments.size(); i++) {
        std::vector<DisValue> rol;
        ::sprintf(buffer, "F(%02d)", filaments[i].id+1);
        line = std::string(buffer);
        for (auto tray = tray_filaments.begin(); tray != tray_filaments.end(); tray++) {
            DisValue val;
            val.tray_id = tray->second.id;
            wxColour c = wxColour(filaments[i].color);
            wxColour tray_c = AmsTray::decode_color(tray->second.color);
            val.distance = calc_color_distance(c, tray_c);
            if (filaments[i].type != tray->second.type) {
                val.distance = 999999;
                val.is_type_match = false;
            } else {
                if (c.Alpha() != tray_c.Alpha())
                    val.distance = 999999;
                val.is_type_match = true;
            }
            ::sprintf(buffer, "  %6.0f", val.distance);
            line += std::string(buffer);
            rol.push_back(val);
        }
        BOOST_LOG_TRIVIAL(info) << "ams_mapping_distance:" << line;
        distance_map.push_back(rol);
    }

    // mapping algorithm
    for (int i = 0; i < filaments.size(); i++) {
        FilamentInfo info;
        info.id = filaments[i].id;
        info.tray_id = -1;
        result.push_back(info);
    }

    std::set<int> picked_src;
    std::set<int> picked_tar;
    for (int k = 0; k < distance_map.size(); k++) {
        float min_val = INT_MAX;
        int picked_src_idx = -1;
        int picked_tar_idx = -1;
        for (int i = 0; i < distance_map.size(); i++) {
            if (picked_src.find(i) != picked_src.end())
                continue;
            for (int j = 0; j < distance_map[i].size(); j++) {
                if (picked_tar.find(j) != picked_tar.end()){
                    if (distance_map[i][j].is_same_color
                        && distance_map[i][j].is_type_match
                        && distance_map[i][j].distance < (float)0.0001) {
                        min_val = distance_map[i][j].distance;
                        picked_src_idx = i;
                        picked_tar_idx = j;
                    }
                    continue;
                }

                if (distance_map[i][j].is_same_color
                    && distance_map[i][j].is_type_match) {
                    if (min_val > distance_map[i][j].distance) {

                        min_val = distance_map[i][j].distance;
                        picked_src_idx = i;
                        picked_tar_idx = j;
                    }
                    else if (min_val == distance_map[i][j].distance&& filaments[picked_src_idx].filament_id!= tray_filaments[picked_tar_idx].filament_id && filaments[i].filament_id == tray_filaments[j].filament_id) {

                        picked_src_idx = i;
                        picked_tar_idx = j;
                    }
                }
            }
        }
        if (picked_src_idx >= 0 && picked_tar_idx >= 0) {
            auto tray = tray_filaments.find(distance_map[k][picked_tar_idx].tray_id);
            if (tray != tray_filaments.end()) {
                result[picked_src_idx].tray_id  = tray->first;
                result[picked_src_idx].color    = tray->second.color;
                result[picked_src_idx].type     = tray->second.type;
                result[picked_src_idx].distance = tray->second.distance;
                result[picked_src_idx].filament_id = tray->second.filament_id;
                result[picked_src_idx].ctype = tray->second.ctype;
                result[picked_src_idx].colors = tray->second.colors;


                /*for new ams mapping*/
                result[picked_src_idx].ams_id = tray->second.ams_id;
                result[picked_src_idx].slot_id = tray->second.slot_id;
            }
            else {
                FilamentInfo info;
                info.tray_id = -1;
            }
            ::sprintf(buffer, "ams_mapping, picked F(%02d) AMS(%02d), distance=%6.0f", picked_src_idx+1, picked_tar_idx+1,
                distance_map[picked_src_idx][picked_tar_idx].distance);
            BOOST_LOG_TRIVIAL(info) << std::string(buffer);
            picked_src.insert(picked_src_idx);
            picked_tar.insert(picked_tar_idx);
        }
    }

    std::vector<FilamentInfo> cache_map_result = result;

    //check ams mapping result
    if (is_valid_mapping_result(result, true)) {
        return 0;
    }

    reset_mapping_result(result);
    try {
        // try to use ordering ams mapping
        bool order_mapping_result = true;
        for (int i = 0; i < filaments.size(); i++) {
            if (i >= tray_info_list.size()) {
                order_mapping_result = false;
                break;
            }
            if (tray_info_list[i].tray_id == -1) {
                result[i].tray_id = tray_info_list[i].tray_id;
            } else {
                if (!tray_info_list[i].type.empty() && tray_info_list[i].type != filaments[i].type) {
                    order_mapping_result = false;
                    break;
                } else {
                    result[i].tray_id = tray_info_list[i].tray_id;
                    result[i].color = tray_info_list[i].color;
                    result[i].type = tray_info_list[i].type;
                    result[i].ctype = tray_info_list[i].ctype;
                    result[i].colors = tray_info_list[i].colors;

                    /*for new ams mapping*/
                    result[i].ams_id = tray_info_list[i].ams_id;
                    result[i].slot_id = tray_info_list[i].slot_id;
                }
            }
        }

        //check order mapping result
        if (is_valid_mapping_result(result, true)) {
            return 0;
        }
    } catch(...) {
        reset_mapping_result(result);
        return -1;
    }

    // try to match some color
    reset_mapping_result(result);
    result = cache_map_result;
    for (auto it = result.begin(); it != result.end(); it++) {
        if (it->distance >= 6000) {
            it->tray_id = -1;
        }
    }

    return 0;
}

bool MachineObject::is_valid_mapping_result(std::vector<FilamentInfo>& result, bool check_empty_slot)
{
    bool valid_ams_mapping_result = true;
    if (result.empty()) return false;

    for (int i = 0; i < result.size(); i++) {
        // invalid mapping result
        if (result[i].tray_id < 0)
            valid_ams_mapping_result = false;
        else {
            int ams_id = result[i].tray_id / 4;
            auto ams_item = amsList.find(std::to_string(ams_id));
            if (ams_item == amsList.end()) {
                result[i].tray_id = -1;
                valid_ams_mapping_result = false;
            } else {
                if (check_empty_slot) {
                    int  tray_id   = result[i].tray_id % 4;
                    auto tray_item = ams_item->second->trayList.find(std::to_string(tray_id));
                    if (tray_item == ams_item->second->trayList.end()) {
                        result[i].tray_id        = -1;
                        valid_ams_mapping_result = false;
                    } else {
                        if (!tray_item->second->is_exists) {
                            result[i].tray_id        = -1;
                            valid_ams_mapping_result = false;
                        }
                    }
                }
            }
        }
    }
    return valid_ams_mapping_result;
}

bool MachineObject::is_mapping_exceed_filament(std::vector<FilamentInfo> & result, int &exceed_index)
{
    bool is_exceed = false;
    for (int i = 0; i < result.size(); i++) {
        int ams_id = result[i].tray_id / 4;
        if (amsList.find(std::to_string(ams_id)) == amsList.end()) {
            exceed_index = result[i].tray_id;
            result[i].tray_id = -1;
            is_exceed = true;
            break;
        }
        if (result[i].mapping_result == MappingResult::MAPPING_RESULT_EXCEED) {
            exceed_index = result[i].id;
            is_exceed = true;
            break;
        }
    }
    return is_exceed;
}

void MachineObject::reset_mapping_result(std::vector<FilamentInfo>& result)
{
    for (int i = 0; i < result.size(); i++) {
        result[i].tray_id = -1;
        result[i].distance = 99999;
        result[i].mapping_result = 0;
    }
}

bool MachineObject::is_main_extruder_on_left() const
{
    // only means the extruder is on the left hand when extruder id is 0
    return false;
}

bool MachineObject::is_multi_extruders() const
{
    return m_extder_data.total_extder_count > 1;
}

bool MachineObject::is_bbl_filament(std::string tag_uid)
{
    if (tag_uid.empty())
        return false;

    for (int i = 0; i < tag_uid.length(); i++) {
        if (tag_uid[i] != '0')
            return true;
    }

    return false;
}

std::string MachineObject::light_effect_str(LIGHT_EFFECT effect)
{
    switch (effect)
    {
    case LIGHT_EFFECT::LIGHT_EFFECT_ON:
        return "on";
    case LIGHT_EFFECT::LIGHT_EFFECT_OFF:
        return "off";
    case LIGHT_EFFECT::LIGHT_EFFECT_FLASHING:
        return "flashing";
    default:
        return "unknown";
    }
    return "unknown";
}

MachineObject::LIGHT_EFFECT MachineObject::light_effect_parse(std::string effect_str)
{
    if (effect_str.compare("on") == 0)
        return LIGHT_EFFECT::LIGHT_EFFECT_ON;
    else if (effect_str.compare("off") == 0)
        return LIGHT_EFFECT::LIGHT_EFFECT_OFF;
    else if (effect_str.compare("flashing") == 0)
        return LIGHT_EFFECT::LIGHT_EFFECT_FLASHING;
    else
        return LIGHT_EFFECT::LIGHT_EFFECT_UNKOWN;
    return LIGHT_EFFECT::LIGHT_EFFECT_UNKOWN;
}


std::string MachineObject::get_firmware_type_str()
{
    /*if (firmware_type == PrinterFirmwareType::FIRMWARE_TYPE_ENGINEER)
        return "engineer";
    else if (firmware_type == PrinterFirmwareType::FIRMWARE_TYPE_PRODUCTION)
        return "product";*/

    // return product by default;
    // always return product, printer do not push this field
    return "product";
}

std::string MachineObject::get_lifecycle_type_str()
{
    /*if (lifecycle == PrinterFirmwareType::FIRMWARE_TYPE_ENGINEER)
        return "engineer";
    else if (lifecycle == PrinterFirmwareType::FIRMWARE_TYPE_PRODUCTION)
        return "product";*/

    // return product by default;
    // always return product, printer do not push this field
    return "product";
}

bool MachineObject::is_in_upgrading()
{
    return upgrade_display_state == (int)UpgradingInProgress;
}

bool MachineObject::is_upgrading_avalable()
{
    return upgrade_display_state == (int)UpgradingAvaliable;
}

int MachineObject::get_upgrade_percent()
{
    if (upgrade_progress.empty())
        return 0;
    try {
        int result = atoi(upgrade_progress.c_str());
        return result;
    } catch(...) {
        ;
    }
    return 0;
}

std::string MachineObject::get_ota_version()
{
    auto it = module_vers.find("ota");
    if (it != module_vers.end()) {
        //double check name
        if (it->second.name == "ota") {
            return it->second.sw_ver;
        }
    }
    return "";
}

bool MachineObject::check_version_valid()
{
    bool valid = true;
    for (auto module : module_vers) {
        if (module.second.sn.empty()
            && module.first != "ota"
            && module.first != "xm")
            return false;
        if (module.second.sw_ver.empty())
            return false;
    }
    get_version_retry = 0;
    return valid;
}

wxString MachineObject::get_upgrade_result_str(int err_code)
{
    switch(err_code) {
    case UpgradeNoError:
        return _L("Update successful.");
    case UpgradeDownloadFailed:
        return _L("Downloading failed.");
    case UpgradeVerfifyFailed:
        return _L("Verification failed.");
    case UpgradeFlashFailed:
        return _L("Update failed.");
    case UpgradePrinting:
        return _L("Update failed.");
    default:
        return _L("Update failed.");
    }
    return "";
}

std::map<int, MachineObject::ModuleVersionInfo> MachineObject::get_ams_version()
{
    std::vector<std::string> multi_tray_ams_type = {"ams", "n3f"};
    std::map<int, ModuleVersionInfo> result;
    for (int i = 0; i < 8; i++) {
        std::string ams_id;
        for (auto type : multi_tray_ams_type)
        {
            ams_id = type + "/" + std::to_string(i);
            auto it = module_vers.find(ams_id);
            if (it != module_vers.end()) {
                result.emplace(std::pair(i, it->second));
            }
        }
    }

    std::string single_tray_ams_type = "n3s";
    int n3s_start_id = 128;
    for (int i = n3s_start_id; i < n3s_start_id + 8; i++) {
        std::string ams_id;
        ams_id = single_tray_ams_type + "/" + std::to_string(i);
        auto it = module_vers.find(ams_id);
        if (it != module_vers.end()) {
            result.emplace(std::pair(i, it->second));
        }
    }
    return result;
}

void MachineObject::store_version_info(const ModuleVersionInfo& info)
{
    if (info.isAirPump())
    {
        air_pump_version_info = info;
    }
    else if (info.isLaszer())
    {
        laser_version_info = info;
    }
    else if (info.isCuttingModule())
    {
        cutting_module_version_info = info;
    }

    module_vers.emplace(info.name, info);
}

bool MachineObject::is_system_printing()
{
    if (is_in_calibration() && is_in_printing_status(print_status))
        return true;
    //FIXME
    //if (print_type == "system" && is_in_printing_status(print_status))
        //return true;
    return false;
}

bool MachineObject::check_pa_result_validation(PACalibResult& result)
{
    if (result.k_value < 0 || result.k_value > 10)
        return false;

    return true;
}

bool MachineObject::is_axis_at_home(std::string axis)
{
    if (home_flag < 0)
        return true;

    if (axis == "X") {
        return (home_flag & 1) == 1;
    } else if (axis == "Y") {
        return (home_flag >> 1 & 1) == 1;
    } else if (axis == "Z") {
        return (home_flag >> 2 & 1) == 1;
    } else {
        return true;
    }
}

bool MachineObject::is_filament_at_extruder()
{
    if (hw_switch_state == 1)
        return true;
    else if (hw_switch_state == 0)
        return false;
    else {
       //default
        return true;
    }
}

wxString MachineObject::get_curr_stage()
{
    if (stage_list_info.empty()) {
        return "";
    }
    return get_stage_string(stage_curr);
}

int MachineObject::get_curr_stage_idx()
{
    int result = -1;
    for (int i = 0; i < stage_list_info.size(); i++) {
        if (stage_list_info[i] == stage_curr) {
            return i;
        }
    }
    return -1;
}

bool MachineObject::is_in_calibration()
{
    // gcode file: auto_cali_for_user.gcode or auto_cali_for_user_param
    if (boost::contains(m_gcode_file, "auto_cali_for_user")
        && stage_curr != 0) {
        return true;
    } else {
        // reset
        if (stage_curr != 0) {
            calibration_done = false;
        }
    }
    return false;
}



bool MachineObject::is_calibration_done()
{
    return calibration_done;
}

bool MachineObject::is_calibration_running()
{
    if (is_in_calibration() && is_in_printing_status(print_status))
        return true;
    return false;
}

void MachineObject::parse_state_changed_event()
{
    // parse calibration done
    if (last_mc_print_stage != mc_print_stage) {
        if (mc_print_stage == 1 && boost::contains(m_gcode_file, "auto_cali_for_user")) {
            calibration_done = true;
        } else {
            calibration_done = false;
        }
    }
    last_mc_print_stage = mc_print_stage;
}

void MachineObject::parse_status(int flag)
{
    is_220V_voltage = ((flag >> 3) & 0x1) != 0;
    if (xcam_auto_recovery_hold_count > 0)
        xcam_auto_recovery_hold_count--;
    else {
        xcam_auto_recovery_step_loss = ((flag >> 4) & 0x1) != 0;
    }

    camera_recording            = ((flag >> 5) & 0x1) != 0;
    ams_calibrate_remain_flag   = ((flag >> 7) & 0x1) != 0;

    sdcard_state = MachineObject::SdcardState(get_flag_bits(flag, 8, 2));

    if (ams_print_option_count > 0)
        ams_print_option_count--;
    else {
        ams_auto_switch_filament_flag = ((flag >> 10) & 0x1) != 0;
    }

    is_support_flow_calibration = ((flag >> 15) & 0x1) != 0;
    is_support_pa_calibration = ((flag >> 16) & 0x1) != 0;

    if (xcam_prompt_sound_hold_count > 0)
        xcam_prompt_sound_hold_count--;
    else {
        xcam_allow_prompt_sound = ((flag >> 17) & 0x1) != 0;
    }

    is_support_prompt_sound = ((flag >> 18) & 0x1) != 0;
    is_support_filament_tangle_detect = ((flag >> 19) & 0x1) != 0;

    if (xcam_filament_tangle_detect_count > 0)
        xcam_filament_tangle_detect_count--;
    else {
        xcam_filament_tangle_detect = ((flag >> 20) & 0x1) != 0;
    }

    /*if(!is_support_motor_noise_cali){
        is_support_motor_noise_cali = ((flag >> 21) & 0x1) != 0;
    }*/
    is_support_motor_noise_cali = ((flag >> 21) & 0x1) != 0;

    is_support_user_preset = ((flag >> 22) & 0x1) != 0;

    is_support_nozzle_blob_detection = ((flag >> 25) & 0x1) != 0;
    nozzle_blob_detection_enabled = ((flag >> 24) & 0x1) != 0;

    is_support_air_print_detection = ((flag >> 29) & 0x1) != 0;
    ams_air_print_status = ((flag >> 28) & 0x1) != 0;

    /*if (!is_support_p1s_plus) {
        auto supported_plus = ((flag >> 27) & 0x1) != 0;
        auto installed_plus = ((flag >> 26) & 0x1) != 0;

        if (installed_plus && supported_plus) {
            is_support_p1s_plus = true;
        }
        else {
            is_support_p1s_plus = false;
        }
    }*/

    is_support_upgrade_kit = ((flag >> 27) & 0x1) != 0;
    installed_upgrade_kit = ((flag >> 26) & 0x1) != 0;

    is_support_agora = ((flag >> 30) & 0x1) != 0;
    if (is_support_agora)
        is_support_tunnel_mqtt = false;
}

PrintingSpeedLevel MachineObject::_parse_printing_speed_lvl(int lvl)
{
    if (lvl < (int)SPEED_LEVEL_COUNT)
        return PrintingSpeedLevel(lvl);

    return PrintingSpeedLevel::SPEED_LEVEL_INVALID;
}

int MachineObject::get_bed_temperature_limit()
{
    if (get_printer_series() == PrinterSeries::SERIES_X1) {
        if (is_220V_voltage)
            return 110;
        else {
            return 120;
        }
    } else {
        int limit = bed_temperature_limit < 0?BED_TEMP_LIMIT:bed_temperature_limit;
        return limit;
    }
    return BED_TEMP_LIMIT;
}

bool MachineObject::is_makeworld_subtask()
{
    if (model_task && model_task->design_id > 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " model task id: " << model_task->task_id << " is makeworld model";
        return true;
    }
    return false;
}

bool MachineObject::is_sdcard_printing()
{
    if (can_abort()
        && (obj_subtask_id.compare("0") == 0 || obj_subtask_id.empty())
        && (profile_id_ == "0" || profile_id_.empty())
        && (project_id_ == "0" || project_id_.empty()))
        return true;
    else
        return false;
}

MachineObject::SdcardState MachineObject::get_sdcard_state()
{
    return sdcard_state;
}

bool MachineObject::is_timelapse()
{
    return camera_timelapse;
}

bool MachineObject::is_recording_enable()
{
    return camera_recording_when_printing;
}

bool MachineObject::is_recording()
{
    return camera_recording;
}

int MachineObject::get_liveview_remote()
{
    if (is_support_agora) {
        return liveview_remote == LVR_None ? LVR_Agora : liveview_remote == LVR_Tutk ? LVR_TutkAgora : liveview_remote;
    }
    return liveview_remote;
}

int MachineObject::get_file_remote()
{
    if (is_support_agora)
        file_remote = file_remote == FR_None ? FR_Agora : file_remote == FR_Tutk ? FR_TutkAgora : file_remote;
    return file_remote;
}

std::string MachineObject::parse_version()
{
    auto ota_version = module_vers.find("ota");
    if (ota_version != module_vers.end()) return ota_version->second.sw_ver;
    auto series = get_printer_series();
    if (series == PrinterSeries::SERIES_X1) {
        auto rv1126_version = module_vers.find("rv1126");
        if (rv1126_version != module_vers.end()) return rv1126_version->second.sw_ver;
    } else if (series == PrinterSeries::SERIES_P1P) {
        auto esp32_version = module_vers.find("esp32");
        if (esp32_version != module_vers.end()) return esp32_version->second.sw_ver;
    }
    return "";
}

void MachineObject::parse_version_func()
{
}

bool MachineObject::is_studio_cmd(int sequence_id)
{
    if (sequence_id >= START_SEQ_ID && sequence_id < END_SEQ_ID) {
        return true;
    }
    return false;
}

int MachineObject::command_get_version(bool with_retry)
{
    BOOST_LOG_TRIVIAL(info) << "command_get_version";
    json j;
    j["info"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["info"]["command"] = "get_version";
    if (with_retry)
        get_version_retry = GET_VERSION_RETRYS;
    return this->publish_json(j.dump(), 1);
}

int MachineObject::command_get_access_code() {
    BOOST_LOG_TRIVIAL(info) << "command_get_access_code";
    json j;
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["command"] = "get_access_code";

    return this->publish_json(j.dump());
}


int MachineObject::command_request_push_all(bool request_now)
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_request_push);

    if (diff.count() < REQUEST_PUSH_MIN_TIME) {
        if (request_now) {
            BOOST_LOG_TRIVIAL(trace) << "static: command_request_push_all, dev_id=" << dev_id;
            last_request_push = std::chrono::system_clock::now();
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "static: command_request_push_all: send request too fast, dev_id=" << dev_id;
            return -1;
        }
    } else {
        BOOST_LOG_TRIVIAL(trace) << "static: command_request_push_all, dev_id=" << dev_id;
        last_request_push = std::chrono::system_clock::now();
    }

    json j;
    j["pushing"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["pushing"]["command"]     = "pushall";
    j["pushing"]["version"]     =  1;
    j["pushing"]["push_target"] =  1;
    return this->publish_json(j.dump());
}

int MachineObject::command_pushing(std::string cmd)
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_request_start);
    if (diff.count() < REQUEST_START_MIN_TIME) {
        BOOST_LOG_TRIVIAL(trace) << "static: command_request_start: send request too fast, dev_id=" << dev_id;
        return -1;
    }
    else {
        BOOST_LOG_TRIVIAL(trace) << "static: command_request_start, dev_id=" << dev_id;
        last_request_start = std::chrono::system_clock::now();
    }

    if (cmd == "start" || cmd == "stop") {
        json j;
        j["pushing"]["command"] = cmd;
        j["pushing"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return this->publish_json(j.dump());
    }
    return -1;
}

int MachineObject::command_clean_print_error(std::string subtask_id, int print_error)
{
    BOOST_LOG_TRIVIAL(info) << "command_clean_print_error, id = " << subtask_id;
    json j;
    j["print"]["command"] = "clean_print_error";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["subtask_id"] = subtask_id;
    j["print"]["print_error"] = print_error;

    return this->publish_json(j.dump());
}

int MachineObject::command_upgrade_confirm()
{
    BOOST_LOG_TRIVIAL(info) << "command_upgrade_confirm";
    json j;
    j["upgrade"]["command"] = "upgrade_confirm";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["src_id"] = 1; // 1 for slicer
    return this->publish_json(j.dump());
}

int MachineObject::command_consistency_upgrade_confirm()
{
    BOOST_LOG_TRIVIAL(info) << "command_consistency_upgrade_confirm";
    json j;
    j["upgrade"]["command"] = "consistency_confirm";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["src_id"] = 1; // 1 for slicer
    return this->publish_json(j.dump());
}

int MachineObject::command_upgrade_firmware(FirmwareInfo info)
{
    std::string version     = info.version;
    std::string dst_url     = info.url;
    std::string module_name = info.module_type;

    json j;
    j["upgrade"]["command"]     = "start";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["url"]         = info.url;
    j["upgrade"]["module"]      = info.module_type;
    j["upgrade"]["version"]     = info.version;
    j["upgrade"]["src_id"]      = 1;

    return this->publish_json(j.dump());
}

int MachineObject::command_upgrade_module(std::string url, std::string module_type, std::string version)
{
    json j;
    j["upgrade"]["command"] = "start";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["url"] = url;
    j["upgrade"]["module"] = module_type;
    j["upgrade"]["version"] = version;
    j["upgrade"]["src_id"] = 1;

    return this->publish_json(j.dump());
}

int MachineObject::command_xyz_abs()
{
    return this->publish_gcode("G90 \n");
}

int MachineObject::command_auto_leveling()
{
    return this->publish_gcode("G29 \n");
}

int MachineObject::command_go_home()
{
    return this->publish_gcode("G28 \n");
}

int MachineObject::command_go_home2()
{
    BOOST_LOG_TRIVIAL(info) << "New protocol of command_go_home2";
    json j;
    j["print"]["command"]     = "back_to_center";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    return this->publish_json(j.dump());
}

int MachineObject::command_control_fan(FanType fan_type, bool on_off)
{
    std::string gcode = (boost::format("M106 P%1% S%2% \n") % (int)fan_type % (on_off ? 255 : 0)).str();
    return this->publish_gcode(gcode);
}

int MachineObject::command_control_fan_val(FanType fan_type, int val)
{
    std::string gcode = (boost::format("M106 P%1% S%2% \n") % (int)fan_type % (val)).str();
    return this->publish_gcode(gcode);
}


int MachineObject::command_task_abort()
{
    BOOST_LOG_TRIVIAL(trace) << "command_task_abort: ";
    json j;
    j["print"]["command"] = "stop";
    j["print"]["param"] = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j.dump(), 1);
}

int MachineObject::command_task_cancel(std::string job_id)
{
    BOOST_LOG_TRIVIAL(trace) << "command_task_cancel: " << job_id;
    json j;
    j["print"]["command"] = "stop";
    j["print"]["param"] = "";
    j["print"]["job_id"] = job_id;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j.dump(), 1);
}

int MachineObject::command_task_pause()
{
    json j;
    j["print"]["command"] = "pause";
    j["print"]["param"] = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j.dump(), 1);
}

int MachineObject::command_task_resume()
{
    json j;
    j["print"]["command"] = "resume";
    j["print"]["param"] = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j.dump(), 1);
}

int MachineObject::command_set_bed(int temp)
{
    std::string gcode_str = (boost::format("M140 S%1%\n") % temp).str();
    return this->publish_gcode(gcode_str);
}

int MachineObject::command_set_nozzle(int temp)
{
    std::string gcode_str = (boost::format("M104 S%1%\n") % temp).str();
    return this->publish_gcode(gcode_str);
}

int MachineObject::command_set_chamber(int temp)
{
    json j;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"] = "set_ctt";
    j["print"]["ctt_val"] = temp;

    return this->publish_json(j.dump(), 1);
}

//int MachineObject::command_ams_switch(int tray_index, int old_temp, int new_temp)
//{
//    BOOST_LOG_TRIVIAL(trace) << "ams_switch to " << tray_index << " with temp: " << old_temp << ", " << new_temp;
//    if (old_temp < 0) old_temp = FILAMENT_DEF_TEMP;
//    if (new_temp < 0) new_temp = FILAMENT_DEF_TEMP;
//
//    std::string gcode = "";
//    int result = 0;
//
//    //command
//    if (is_support_command_ams_switch) {
//        command_ams_change_filament(tray_index, old_temp, new_temp);
//    }
//    else {
//        std::string gcode = "";
//        if (tray_index == 255) {
//            gcode = DeviceManager::load_gcode(printer_type, "ams_unload.gcode");
//        }
//        else {
//            // include VIRTUAL_TRAY_ID
//            gcode = DeviceManager::load_gcode(printer_type, "ams_load.gcode");
//            boost::replace_all(gcode, "[next_extruder]", std::to_string(tray_index));
//            boost::replace_all(gcode, "[new_filament_temp]", std::to_string(new_temp));
//        }
//
//        result = this->publish_gcode(gcode);
//    }
//
//    return result;
//}

int MachineObject::command_ams_change_filament(bool load, std::string ams_id, std::string slot_id, int old_temp, int new_temp)
{
    json j;
    try {
        auto tray_id = 0;
        if (ams_id < "16") {
            tray_id = atoi(ams_id.c_str()) * 4 + atoi(slot_id.c_str());
        }
        // TODO: Orca hack
        if (ams_id == "254")
            ams_id = "255";


        j["print"]["command"]     = "ams_change_filament";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["curr_temp"]   = old_temp;
        j["print"]["tar_temp"]    = new_temp;
        j["print"]["ams_id"]      = atoi(ams_id.c_str());

        if (!load) {
            j["print"]["target"]  = 255;
            j["print"]["slot_id"] = 255; // the new protocol to mark unload

        } else {
            if (tray_id == 0) {
                j["print"]["target"]  = atoi(ams_id.c_str());
            } else {
                j["print"]["target"]  = tray_id;
            }

            j["print"]["slot_id"] = atoi(slot_id.c_str());
        }
    } catch (const std::exception &) {}

    return this->publish_json(j.dump());
}

int MachineObject::command_ams_user_settings(int ams_id, bool start_read_opt, bool tray_read_opt, bool remain_flag)
{
    json j;
    j["print"]["command"] = "ams_user_setting";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["ams_id"] = ams_id;
    j["print"]["startup_read_option"]   = start_read_opt;
    j["print"]["tray_read_option"]      = tray_read_opt;
    j["print"]["calibrate_remain_flag"] = remain_flag;

    ams_insert_flag = tray_read_opt;
    ams_power_on_flag = start_read_opt;
    ams_calibrate_remain_flag = remain_flag;
    ams_user_setting_hold_count = HOLD_COUNT_MAX;

    return this->publish_json(j.dump());
}

int MachineObject::command_ams_calibrate(int ams_id)
{
    std::string gcode_cmd = (boost::format("M620 C%1% \n") % ams_id).str();
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode_cmd;
    return this->publish_gcode(gcode_cmd);
}

int MachineObject::command_ams_filament_settings(int ams_id, int slot_id, std::string filament_id, std::string setting_id, std::string tray_color, std::string tray_type, int nozzle_temp_min, int nozzle_temp_max)
{
    int tag_tray_id = 0;
    int tag_ams_id  = ams_id;
    int tag_slot_id = slot_id;

    if (tag_ams_id == VIRTUAL_TRAY_ID) {
        tag_tray_id = VIRTUAL_TRAY_ID;
        tag_ams_id  = 255; // TODO: Orca hack
    } else {
        tag_tray_id = tag_slot_id;
    }


    BOOST_LOG_TRIVIAL(info) << "command_ams_filament_settings, ams_id = " << tag_ams_id << ", slot_id = " << tag_slot_id << ", tray_id = " << tag_tray_id << ", tray_color = " << tray_color
                            << ", tray_type = " << tray_type << ", filament_id = " << filament_id
                            << ", setting_id = " << setting_id << ", temp_min: = " << nozzle_temp_min << ", temp_max: = " << nozzle_temp_max;
    json j;
    j["print"]["command"]       = "ams_filament_setting";
    j["print"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["ams_id"]        = tag_ams_id;
    j["print"]["slot_id"]       = tag_slot_id;
    j["print"]["tray_id"]       = tag_tray_id;
    j["print"]["tray_info_idx"] = filament_id;
    j["print"]["setting_id"]    = setting_id;
    // format "FFFFFFFF"   RGBA
    j["print"]["tray_color"]        = tray_color;
    j["print"]["nozzle_temp_min"]   = nozzle_temp_min;
    j["print"]["nozzle_temp_max"]   = nozzle_temp_max;
    j["print"]["tray_type"]         = tray_type;

    return this->publish_json(j.dump());
}

int MachineObject::command_ams_refresh_rfid(std::string tray_id)
{
    std::string gcode_cmd = (boost::format("M620 R%1% \n") % tray_id).str();
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode_cmd;
    return this->publish_gcode(gcode_cmd);
}

int MachineObject::command_ams_select_tray(std::string tray_id)
{
    std::string gcode_cmd = (boost::format("M620 P%1% \n") % tray_id).str();
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode_cmd;
    return this->publish_gcode(gcode_cmd);
}

int MachineObject::command_ams_control(std::string action)
{
    //valid actions
    if (action == "resume" || action == "reset" || action == "pause" || action == "done") {
        json j;
        j["print"]["command"] = "ams_control";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["param"] = action;
        return this->publish_json(j.dump());
    }
    return -1;
}


int MachineObject::command_set_chamber_light(LIGHT_EFFECT effect, int on_time, int off_time, int loops, int interval)
{
    json j;
    j["system"]["command"] = "ledctrl";
    j["system"]["led_node"] = "chamber_light";
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["led_mode"] = light_effect_str(effect);
    j["system"]["led_on_time"] = on_time;
    j["system"]["led_off_time"] = off_time;
    j["system"]["loop_times"] = loops;
    j["system"]["interval_time"] = interval;

    return this->publish_json(j.dump());
}


int MachineObject::command_set_printer_nozzle(std::string nozzle_type, float diameter)
{
    nozzle_setting_hold_count = HOLD_COUNT_MAX * 2;
    BOOST_LOG_TRIVIAL(info) << "command_set_printer_nozzle, nozzle_type = " << nozzle_type << " diameter = " << diameter;
    json j;
    j["system"]["command"] = "set_accessories";
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["accessory_type"] = "nozzle";
    j["system"]["nozzle_type"] = nozzle_type;
    j["system"]["nozzle_diameter"] = diameter;
    return this->publish_json(j.dump());
}


int MachineObject::command_set_work_light(LIGHT_EFFECT effect, int on_time, int off_time, int loops, int interval)
{
    json j;
    j["system"]["command"] = "ledctrl";
    j["system"]["led_node"] = "work_light";
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["led_mode"] = light_effect_str(effect);
    j["system"]["led_on_time"] = on_time;
    j["system"]["led_off_time"] = off_time;
    j["system"]["loop_times"] = loops;
    j["system"]["interval_time"] = interval;

    return this->publish_json(j.dump());
}

int MachineObject::command_start_extrusion_cali(int tray_index, int nozzle_temp, int bed_temp, float max_volumetric_speed, std::string setting_id)
{
    BOOST_LOG_TRIVIAL(trace) << "extrusion_cali: tray_id = " << tray_index << ", nozzle_temp = " << nozzle_temp << ", bed_temp = " << bed_temp
                            << ", max_volumetric_speed = " << max_volumetric_speed;

    json j;
    j["print"]["command"] = "extrusion_cali";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["tray_id"] = tray_index;
    //j["print"]["setting_id"] = setting_id;
    //j["print"]["name"] = "";
    j["print"]["nozzle_temp"] = nozzle_temp;
    j["print"]["bed_temp"] = bed_temp;
    j["print"]["max_volumetric_speed"] = max_volumetric_speed;

    // enter extusion cali
    last_extrusion_cali_start_time = std::chrono::system_clock::now();
    BOOST_LOG_TRIVIAL(trace) << "extrusion_cali: " << j.dump();
    return this->publish_json(j.dump());
}

int MachineObject::command_stop_extrusion_cali()
{
    BOOST_LOG_TRIVIAL(trace) << "extrusion_cali: stop";
    if (is_in_extrusion_cali()) {
        return command_task_abort();
    }
    return 0;
}

int MachineObject::command_extrusion_cali_set(int tray_index, std::string setting_id, std::string name, float k, float n, int bed_temp, int nozzle_temp, float max_volumetric_speed)
{
    BOOST_LOG_TRIVIAL(trace) << "extrusion_cali: tray_id = " << tray_index << ", setting_id = " << setting_id << ", k = " << k
                            << ", n = " << n;
    json j;
    j["print"]["command"] = "extrusion_cali_set";
    j["print"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["tray_id"]       = tray_index;
    //j["print"]["setting_id"]    = setting_id;
    //j["print"]["name"]          = name;
    j["print"]["k_value"]       = k;
    j["print"]["n_coef"]        = 1.4f;     // fixed n
    //j["print"]["n_coef"]        = n;
    if (bed_temp >= 0 && nozzle_temp >= 0 && max_volumetric_speed >= 0) {
        j["print"]["bed_temp"]      = bed_temp;
        j["print"]["nozzle_temp"]   = nozzle_temp;
        j["print"]["max_volumetric_speed"] = max_volumetric_speed;
    }
    return this->publish_json(j.dump());
}


int MachineObject::command_set_printing_speed(PrintingSpeedLevel lvl)
{
    json j;
    j["print"]["command"] = "print_speed";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["param"] = std::to_string((int)lvl);

    return this->publish_json(j.dump());
}

int MachineObject::command_set_printing_option(bool auto_recovery)
{
    int print_option = (int)auto_recovery << (int)PRINT_OP_AUTO_RECOVERY;
    json j;
    j["print"]["command"]       = "print_option";
    j["print"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["option"]        = print_option;
    j["print"]["auto_recovery"] = auto_recovery;

    return this->publish_json(j.dump());
}

int MachineObject::command_nozzle_blob_detect(bool nozzle_blob_detect)
{
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_blob_detect"] = nozzle_blob_detect;
    nozzle_blob_detection_enabled = nozzle_blob_detect;
    return this->publish_json(j.dump());
}

int MachineObject::command_set_prompt_sound(bool prompt_sound){
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["sound_enable"] = prompt_sound;

    return this->publish_json(j.dump());
}

int MachineObject::command_set_filament_tangle_detect(bool filament_tangle_detect) {
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["filament_tangle_detect"] = filament_tangle_detect;

    return this->publish_json(j.dump());
}

int MachineObject::command_ams_switch_filament(bool switch_filament)
{
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["auto_switch_filament"] = switch_filament;

    ams_auto_switch_filament_flag = switch_filament;
    BOOST_LOG_TRIVIAL(trace) << "command_ams_filament_settings:" << switch_filament;
    ams_print_option_count = HOLD_COUNT_MAX;

    return this->publish_json(j.dump());
}

int MachineObject::command_ams_air_print_detect(bool air_print_detect)
{
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["air_print_detect"] = air_print_detect;

    ams_air_print_status = air_print_detect;
    BOOST_LOG_TRIVIAL(trace) << "command_ams_air_print_detect:" << air_print_detect;

    return this->publish_json(j.dump());
}


int MachineObject::command_axis_control(std::string axis, double unit, double input_val, int speed)
{
    double value = input_val;
    if (!is_core_xy()) {
        if ( axis.compare("Y") == 0
            || axis.compare("Z")  == 0) {
            value = -1.0 * input_val;
        }
    }

    char cmd[256];
    if (axis.compare("X") == 0
        || axis.compare("Y") == 0
        || axis.compare("Z") == 0) {
        sprintf(cmd, "M211 S \nM211 X1 Y1 Z1\nM1002 push_ref_mode\nG91 \nG1 %s%0.1f F%d\nM1002 pop_ref_mode\nM211 R\n", axis.c_str(), value * unit, speed);
    }
    else if (axis.compare("E") == 0) {
        sprintf(cmd, "M83 \nG0 %s%0.1f F%d\n", axis.c_str(), value * unit, speed);
        extruder_axis_status = (value >= 0.0f)? LOAD : UNLOAD;
    }
    else {
        return -1;
    }


    return this->publish_gcode(cmd);
}


bool MachineObject::is_support_command_calibration()
{
    if (get_printer_series() == PrinterSeries::SERIES_X1) {
        auto ap_ver_it = module_vers.find("rv1126");
        if (ap_ver_it != module_vers.end()) {
            if (ap_ver_it->second.sw_ver.compare("00.00.15.79") < 0)
                return false;
        }
    }
    return true;
}

int MachineObject::command_start_calibration(bool vibration, bool bed_leveling, bool xcam_cali, bool motor_noise)
{
    if (!is_support_command_calibration()) {
        // fixed gcode file
        json j;
        j["print"]["command"] = "gcode_file";
        j["print"]["param"] = "/usr/etc/print/auto_cali_for_user.gcode";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return this->publish_json(j.dump());
    } else {
        json j;
        j["print"]["command"] = "calibration";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["option"]=       (motor_noise  ? 1 << 3 : 0)
                                +   (vibration    ? 1 << 2 : 0)
                                +   (bed_leveling ? 1 << 1 : 0)
                                +   (xcam_cali    ? 1 << 0 : 0);
        return this->publish_json(j.dump());
    }
}

int MachineObject::command_start_pa_calibration(const X1CCalibInfos &pa_data, int mode)
{
    CNumericLocalesSetter locales_setter;

    pa_calib_results.clear();
    json j;
    j["print"]["command"]         = "extrusion_cali";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(pa_data.calib_datas[0].nozzle_diameter);
    j["print"]["mode"]            = mode;

    std::string filament_ids;
    for (int i = 0; i < pa_data.calib_datas.size(); ++i) {
        j["print"]["filaments"][i]["tray_id"]              = pa_data.calib_datas[i].tray_id;
        j["print"]["filaments"][i]["extruder_id"]          = pa_data.calib_datas[i].extruder_id;
        j["print"]["filaments"][i]["bed_temp"]             = pa_data.calib_datas[i].bed_temp;
        j["print"]["filaments"][i]["filament_id"]          = pa_data.calib_datas[i].filament_id;
        j["print"]["filaments"][i]["setting_id"]           = pa_data.calib_datas[i].setting_id;
        j["print"]["filaments"][i]["nozzle_temp"]          = pa_data.calib_datas[i].nozzle_temp;
        j["print"]["filaments"][i]["ams_id"]               = pa_data.calib_datas[i].ams_id;
        j["print"]["filaments"][i]["slot_id"]              = pa_data.calib_datas[i].slot_id;
        j["print"]["filaments"][i]["nozzle_id"]            = generate_nozzle_id(pa_data.calib_datas[i].nozzle_volume_type,to_string_nozzle_diameter(pa_data.calib_datas[i].nozzle_diameter)).ToStdString();
        j["print"]["filaments"][i]["nozzle_diameter"]      = to_string_nozzle_diameter(pa_data.calib_datas[i].nozzle_diameter);
        j["print"]["filaments"][i]["max_volumetric_speed"] = std::to_string(pa_data.calib_datas[i].max_volumetric_speed);

        if (i > 0) filament_ids += ",";
        filament_ids += pa_data.calib_datas[i].filament_id;
    }

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali: " << j.dump();

    try {
        json js;
        js["cali_type"]       = "cali_pa_auto";
        js["nozzle_diameter"] = pa_data.calib_datas[0].nozzle_diameter;
        js["filament_id"]     = filament_ids;
        js["printer_type"]    = this->printer_type;
        NetworkAgent *agent   = GUI::wxGetApp().getAgent();
        if (agent) agent->track_event("cali", js.dump());
    } catch (...) {}

    return this->publish_json(j.dump());
}

int MachineObject::command_set_pa_calibration(const std::vector<PACalibResult> &pa_calib_values, bool is_auto_cali)
{
    CNumericLocalesSetter locales_setter;

    if (pa_calib_values.size() > 0) {
        json j;
        j["print"]["command"]     = "extrusion_cali_set";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(pa_calib_values[0].nozzle_diameter);

        for (int i = 0; i < pa_calib_values.size(); ++i) {
            if (pa_calib_values[i].tray_id >= 0)
                j["print"]["filaments"][i]["tray_id"] = pa_calib_values[i].tray_id;
            if (pa_calib_values[i].cali_idx >= 0)
                j["print"]["filaments"][i]["cali_idx"] = pa_calib_values[i].cali_idx;
            j["print"]["filaments"][i]["tray_id"]     = pa_calib_values[i].tray_id;
            j["print"]["filaments"][i]["extruder_id"] = pa_calib_values[i].extruder_id;
            j["print"]["filaments"][i]["nozzle_id"]   = generate_nozzle_id(pa_calib_values[i].nozzle_volume_type, to_string_nozzle_diameter(pa_calib_values[i].nozzle_diameter)).ToStdString();
            j["print"]["filaments"][i]["nozzle_diameter"] = to_string_nozzle_diameter(pa_calib_values[i].nozzle_diameter);
            j["print"]["filaments"][i]["ams_id"]      = pa_calib_values[i].ams_id;
            j["print"]["filaments"][i]["slot_id"]     = pa_calib_values[i].slot_id;
            j["print"]["filaments"][i]["filament_id"] = pa_calib_values[i].filament_id;
            j["print"]["filaments"][i]["setting_id"]  = pa_calib_values[i].setting_id;
            j["print"]["filaments"][i]["name"]        = pa_calib_values[i].name;
            j["print"]["filaments"][i]["k_value"]     = std::to_string(pa_calib_values[i].k_value);
            if (is_auto_cali)
                j["print"]["filaments"][i]["n_coef"] = std::to_string(pa_calib_values[i].n_coef);
            else
                j["print"]["filaments"][i]["n_coef"]  = "0.0";
        }

        BOOST_LOG_TRIVIAL(info) << "extrusion_cali_set: " << j.dump();
        return this->publish_json(j.dump());
    }

    return -1;
}

int MachineObject::command_delete_pa_calibration(const PACalibIndexInfo& pa_calib)
{
    json j;
    j["print"]["command"]         = "extrusion_cali_del";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["extruder_id"]     = pa_calib.extruder_id;
    j["print"]["nozzle_id"]       = generate_nozzle_id(pa_calib.nozzle_volume_type, to_string_nozzle_diameter(pa_calib.nozzle_diameter)).ToStdString();
    j["print"]["filament_id"]     = pa_calib.filament_id;
    j["print"]["cali_idx"]        = pa_calib.cali_idx;
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(pa_calib.nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_del: " << j.dump();
    return this->publish_json(j.dump());
}

int MachineObject::command_get_pa_calibration_tab(const PACalibExtruderInfo &calib_info)
{
    reset_pa_cali_history_result();

    json j;
    j["print"]["command"]         = "extrusion_cali_get";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["filament_id"]     = calib_info.filament_id;
    if (calib_info.use_extruder_id)
        j["print"]["extruder_id"] = calib_info.extruder_id;
    if (calib_info.use_nozzle_volume_type)
        j["print"]["nozzle_id"] = generate_nozzle_id(calib_info.nozzle_volume_type, to_string_nozzle_diameter(calib_info.nozzle_diameter)).ToStdString();
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(calib_info.nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_get: " << j.dump();
    request_tab_from_bbs = true;
    return this->publish_json(j.dump());
}

int MachineObject::command_get_pa_calibration_result(float nozzle_diameter)
{
    json j;
    j["print"]["command"]         = "extrusion_cali_get_result";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_get_result: " << j.dump();
    return this->publish_json(j.dump());
}

int MachineObject::commnad_select_pa_calibration(const PACalibIndexInfo& pa_calib_info)
{
    json j;
    j["print"]["command"]         = "extrusion_cali_sel";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["tray_id"]         = pa_calib_info.tray_id;
    j["print"]["ams_id"]          = pa_calib_info.ams_id;
    j["print"]["slot_id"]         = pa_calib_info.slot_id;
    j["print"]["cali_idx"]        = pa_calib_info.cali_idx;
    j["print"]["filament_id"]     = pa_calib_info.filament_id;
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(pa_calib_info.nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_sel: " << j.dump();
    return this->publish_json(j.dump());
}

int MachineObject::command_start_flow_ratio_calibration(const X1CCalibInfos& calib_data)
{
    CNumericLocalesSetter locales_setter;

    if (calib_data.calib_datas.size() > 0) {
        json j;
        j["print"]["command"]     = "flowrate_cali";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["tray_id"] = calib_data.calib_datas[0].tray_id;
        j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(calib_data.calib_datas[0].nozzle_diameter);

        std::string filament_ids;
        for (int i = 0; i < calib_data.calib_datas.size(); ++i) {
            j["print"]["filaments"][i]["tray_id"]              = calib_data.calib_datas[i].tray_id;
            j["print"]["filaments"][i]["bed_temp"]             = calib_data.calib_datas[i].bed_temp;
            j["print"]["filaments"][i]["filament_id"]          = calib_data.calib_datas[i].filament_id;
            j["print"]["filaments"][i]["setting_id"]           = calib_data.calib_datas[i].setting_id;
            j["print"]["filaments"][i]["nozzle_temp"]          = calib_data.calib_datas[i].nozzle_temp;
            j["print"]["filaments"][i]["def_flow_ratio"]       = std::to_string(calib_data.calib_datas[i].flow_rate);
            j["print"]["filaments"][i]["max_volumetric_speed"] = std::to_string(calib_data.calib_datas[i].max_volumetric_speed);

            if (i > 0)
                filament_ids += ",";
            filament_ids += calib_data.calib_datas[i].filament_id;
        }

        BOOST_LOG_TRIVIAL(info) << "flowrate_cali: " << j.dump();
        return this->publish_json(j.dump());
    }
    return -1;
}

int MachineObject::command_get_flow_ratio_calibration_result(float nozzle_diameter)
{
    json j;
    j["print"]["command"]         = "flowrate_get_result";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "flowrate_get_result: " << j.dump();
    return this->publish_json(j.dump());
}

int MachineObject::command_ipcam_record(bool on_off)
{
    BOOST_LOG_TRIVIAL(info) << "command_ipcam_record = " << on_off;
    json j;
    j["camera"]["command"] = "ipcam_record_set";
    j["camera"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["camera"]["control"] = on_off ? "enable" : "disable";
    camera_recording_hold_count = HOLD_COUNT_CAMERA;
    this->camera_recording_when_printing = on_off;
    return this->publish_json(j.dump());
}

int MachineObject::command_ipcam_timelapse(bool on_off)
{
    BOOST_LOG_TRIVIAL(info) << "command_ipcam_timelapse " << on_off;
    json j;
    j["camera"]["command"] = "ipcam_timelapse";
    j["camera"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["camera"]["control"] = on_off ? "enable" : "disable";
    camera_timelapse_hold_count = HOLD_COUNT_CAMERA;
    this->camera_timelapse = on_off;
    return this->publish_json(j.dump());
}

int MachineObject::command_ipcam_resolution_set(std::string resolution)
{
    BOOST_LOG_TRIVIAL(info) << "command:ipcam_resolution_set" << ", resolution:" << resolution;
    json j;
    j["camera"]["command"] = "ipcam_resolution_set";
    j["camera"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["camera"]["resolution"] = resolution;
    camera_resolution_hold_count = HOLD_COUNT_CAMERA;
    camera_recording_hold_count = HOLD_COUNT_CAMERA;
    this->camera_resolution = resolution;
    return this->publish_json(j.dump());
}

int MachineObject::command_xcam_control(std::string module_name, bool on_off, std::string lvl)
{
    json j;
    j["xcam"]["command"] = "xcam_control_set";
    j["xcam"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["xcam"]["module_name"] = module_name;
    j["xcam"]["control"] = on_off;
    j["xcam"]["enable"] = on_off;       //old protocol
    j["xcam"]["print_halt"]  = true;    //old protocol
    if (!lvl.empty()) {
        j["xcam"]["halt_print_sensitivity"] = lvl;
    }
    BOOST_LOG_TRIVIAL(info) << "command:xcam_control_set" << ", module_name:" << module_name << ", control:" << on_off << ", halt_print_sensitivity:" << lvl;
    return this->publish_json(j.dump());
}

int MachineObject::command_xcam_control_ai_monitoring(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false:true;

    xcam_ai_monitoring = on_off;
    xcam_ai_monitoring_hold_count = HOLD_COUNT_MAX;
    xcam_ai_monitoring_sensitivity = lvl;
    return command_xcam_control("printing_monitor", on_off, lvl);
}

int MachineObject::command_xcam_control_buildplate_marker_detector(bool on_off)
{
    xcam_buildplate_marker_detector = on_off;
    xcam_buildplate_marker_hold_count = HOLD_COUNT_MAX;
    return command_xcam_control("buildplate_marker_detector", on_off);
}

int MachineObject::command_xcam_control_first_layer_inspector(bool on_off, bool print_halt)
{
    xcam_first_layer_inspector = on_off;
    xcam_first_layer_hold_count = HOLD_COUNT_MAX;
    return command_xcam_control("first_layer_inspector", on_off);
}

int MachineObject::command_xcam_control_auto_recovery_step_loss(bool on_off)
{
    xcam_auto_recovery_step_loss = on_off;
    xcam_auto_recovery_hold_count = HOLD_COUNT_MAX;
    return command_set_printing_option(on_off);
}

int MachineObject::command_xcam_control_allow_prompt_sound(bool on_off)
{
    xcam_allow_prompt_sound = on_off;
    xcam_prompt_sound_hold_count = HOLD_COUNT_MAX;
    return command_set_prompt_sound(on_off);
}

int MachineObject::command_xcam_control_filament_tangle_detect(bool on_off)
{
    xcam_filament_tangle_detect = on_off;
    xcam_filament_tangle_detect_count = HOLD_COUNT_MAX;
    return command_set_filament_tangle_detect(on_off);
}

void MachineObject::set_bind_status(std::string status)
{
    bind_user_name = status;
}

std::string MachineObject::get_bind_str()
{
    std::string default_result = "N/A";
    if (bind_user_name.compare("null") == 0) {
        return "Free";
    }
    else if (!bind_user_name.empty()) {
        return bind_user_name;
    }
    return default_result;
}

bool MachineObject::can_print()
{
    if (print_status.compare("RUNNING") == 0) {
        return false;
    }
    if (print_status.compare("IDLE") == 0 || print_status.compare("FINISH") == 0) {
        return true;
    }
    return true;
}

bool MachineObject::can_resume()
{
    if (print_status.compare("PAUSE") == 0)
        return true;
    return false;
}

bool MachineObject::can_pause()
{
    if (print_status.compare("RUNNING") == 0)
        return true;
    return false;
}

bool MachineObject::can_abort()
{
    return MachineObject::is_in_printing_status(print_status);
}

bool MachineObject::is_in_printing_status(std::string status)
{
    if (status.compare("PAUSE") == 0
        || status.compare("RUNNING") == 0
        || status.compare("SLICING") == 0
        || status.compare("PREPARE") == 0) {
        return true;
    }
    return false;
}


bool MachineObject::is_in_printing()
{
    /* use print_status if print_status is valid */
    if (!print_status.empty())
        return MachineObject::is_in_printing_status(print_status);
    else {
        return MachineObject::is_in_printing_status(iot_print_status);
    }
    return false;
}

bool MachineObject::is_in_prepare()
{
    return print_status == "PREPARE";
}

bool MachineObject::is_printing_finished()
{
    if (print_status.compare("FINISH") == 0
        || print_status.compare("FAILED") == 0) {
        return true;
    }
    return false;
}

bool MachineObject::is_core_xy()
{
    if (get_printer_arch() == PrinterArch::ARCH_CORE_XY)
        return true;
    return false;
}

void MachineObject::reset_update_time()
{
    BOOST_LOG_TRIVIAL(trace) << "reset reset_update_time, dev_id =" << dev_id;
    last_update_time = std::chrono::system_clock::now();
    subscribe_counter = 3;
}

void MachineObject::reset()
{
    BOOST_LOG_TRIVIAL(trace) << "reset dev_id=" << dev_id;
    last_update_time = std::chrono::system_clock::now();
    m_push_count = 0;
    is_220V_voltage = false;
    get_version_retry = 0;
    camera_recording = false;
    camera_recording_when_printing = false;
    camera_timelapse = false;
    //camera_resolution = "";
    printing_speed_mag = 100;
    gcode_file_prepare_percent = 0;
    iot_print_status = "";
    print_status = "";
    last_mc_print_stage = -1;
    m_new_ver_list_exist = false;
    extruder_axis_status = LOAD;
    network_wired = false;
    dev_connection_name = "";
    subscribe_counter = 3;
    job_id_ = "";
    m_plate_index = -1;

    // reset print_json
    json empty_j;
    print_json.diff2all_base_reset(empty_j);

    vt_tray.reset();

    subtask_ = nullptr;

}

void MachineObject::set_print_state(std::string status)
{
    print_status = status;
}

int MachineObject::connect(bool is_anonymous, bool use_openssl)
{
    if (dev_ip.empty()) return -1;
    std::string username;
    std::string password;
    if (!is_anonymous) {
        username = "bblp";
        password = get_access_code();
    }
    if (m_agent) {
        try {
            return m_agent->connect_printer(dev_id, dev_ip, username, password, use_openssl);
        } catch (...) {
            ;
        }
    }
    return -1;
}

int MachineObject::disconnect()
{
    if (m_agent) {
        return m_agent->disconnect_printer();
    }
    return -1;
}

bool MachineObject::is_connected()
{
    std::chrono::system_clock::time_point curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_update_time);
    if (diff.count() > DISCONNECT_TIMEOUT) {
        BOOST_LOG_TRIVIAL(trace) << "machine_object: dev_id=" << dev_id <<", diff count = " << diff.count();
        return false;
    }

    if (!is_lan_mode_printer()) {
        NetworkAgent* m_agent = Slic3r::GUI::wxGetApp().getAgent();
        if (m_agent) {
            return m_agent->is_server_connected();
        }
    }
    return true;
}

bool MachineObject::is_connecting()
{
    return is_connected() && m_push_count == 0;
}

void MachineObject::set_online_state(bool on_off)
{
    m_is_online = on_off;
    if (!on_off) m_active_state = NotActive;
}

bool MachineObject::is_info_ready()
{
    if (module_vers.empty())
        return false;

    std::chrono::system_clock::time_point curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::microseconds>(last_push_time - curr_time);
    if (m_push_count > 0 && diff.count() < PUSHINFO_TIMEOUT) {
        return true;
    }
    return false;
}


std::vector<std::string> MachineObject::get_resolution_supported()
{
    return camera_resolution_supported;
}

std::vector<std::string> MachineObject::get_compatible_machine()
{
    return DeviceManager::get_compatible_machine(printer_type);
}

bool MachineObject::is_camera_busy_off()
{
    if (get_printer_series() == PrinterSeries::SERIES_P1P)
        return is_in_prepare() || is_in_upgrading();
    return false;
}

int MachineObject::publish_json(std::string json_str, int qos, int flag)
{
    int rtn = 0;
    if (is_lan_mode_printer()) {
        rtn = local_publish_json(json_str, qos, flag);
    } else {
        rtn = cloud_publish_json(json_str, qos, flag);
    }

    if (rtn == 0) {
        BOOST_LOG_TRIVIAL(info) << "publish_json: " << json_str << " code: " << rtn;
    } else {
        BOOST_LOG_TRIVIAL(error) << "publish_json: " << json_str << " code: " << rtn;
    }

    return rtn;
}

int MachineObject::cloud_publish_json(std::string json_str, int qos, int flag)
{
    int result = -1;
    if (m_agent)
        result = m_agent->send_message(dev_id, json_str, qos, flag);

    return result;
}

int MachineObject::local_publish_json(std::string json_str, int qos, int flag)
{
    int result = -1;
    if (m_agent) {
        result = m_agent->send_message_to_printer(dev_id, json_str, qos, flag);
    }
    return result;
}

std::string MachineObject::setting_id_to_type(std::string setting_id, std::string tray_type)
{
    std::string type;
    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (preset_bundle) {
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {

            if (it->filament_id.compare(setting_id) == 0 && it->is_system) {
                std::string display_filament_type;
                it->config.get_filament_type(display_filament_type);
                type = display_filament_type;
                break;
            }
        }
    }

    if (tray_type != type || type.empty()) {
        if (type.empty()) { type = tray_type; }
        BOOST_LOG_TRIVIAL(info) << "The values of tray_info_idx and tray_type do not match tray_info_idx " << setting_id << " tray_type " << tray_type << " system_type" << type;
    }
    return type;
}

template <class ENUM>
static ENUM enum_index_of(char const *key, char const **enum_names, int enum_count, ENUM defl = static_cast<ENUM>(0))
{
    for (int i = 0; i < enum_count; ++i)
        if (strcmp(enum_names[i], key) == 0) return static_cast<ENUM>(i);
    return defl;
}

int MachineObject::parse_json(std::string payload, bool key_field_only)
{
#ifdef ORCA_NETWORK_DEBUG
    BOOST_LOG_TRIVIAL(info) << "parse_json: payload = " << payload;
    flush_logs();
#endif

    parse_msg_count++;
    std::chrono::system_clock::time_point clock_start = std::chrono::system_clock::now();
    this->set_online_state(true);

    std::chrono::system_clock::time_point curr_time = std::chrono::system_clock::now();
    auto diff1 = std::chrono::duration_cast<std::chrono::microseconds>(curr_time - last_update_time);

    /* update last received time */
    last_update_time = std::chrono::system_clock::now();

    json j_pre;
    bool parse_ok = false;
    try {
        j_pre = json::parse(payload);
        parse_ok = true;
    }
    catch(...) {
        parse_ok = false;
        /* post process payload */
        sanitizeToUtf8(payload);
        BOOST_LOG_TRIVIAL(info) << "parse_json: sanitize to utf8";
    }

    try {
        bool restored_json = false;
        json j;
        if (!parse_ok)
            j_pre = json::parse(payload);
        CNumericLocalesSetter locales_setter;
        if (j_pre.empty()) {
            return 0;
        }
        if (j_pre.contains("print")) {
            if (m_active_state == NotActive) m_active_state = Active;
            if (j_pre["print"].contains("command")) {
                if (j_pre["print"]["command"].get<std::string>() == "push_status") {
                    if (j_pre["print"].contains("msg")) {
                        if (j_pre["print"]["msg"].get<int>() == 0) {           //all message
                            BOOST_LOG_TRIVIAL(trace) << "static: get push_all msg, dev_id=" << dev_id;
                            m_push_count++;
                            if (!printer_type.empty())
                                print_json.load_compatible_settings(printer_type, "");
                            print_json.diff2all_base_reset(j_pre);
                        } else if (j_pre["print"]["msg"].get<int>() == 1) {    //diff message
                            if (print_json.diff2all(j_pre, j) == 0) {
                                restored_json = true;
                            } else {
                                BOOST_LOG_TRIVIAL(trace) << "parse_json: restore failed! count = " << parse_msg_count;
                                if (print_json.is_need_request()) {
                                    BOOST_LOG_TRIVIAL(trace) << "parse_json: need request pushall, count = " << parse_msg_count;
                                    // request new push
                                    GUI::wxGetApp().CallAfter([this]{
                                        this->command_request_push_all();
                                    });
                                    return -1;
                                }
                                return -1;
                            }
                        } else {
                            BOOST_LOG_TRIVIAL(warning) << "unsupported msg_type=" << j_pre["print"]["msg"].get<std::string>();
                        }
                    }
                    else {
                        if (!printer_type.empty() && connection_type() == "lan")
                            print_json.load_compatible_settings(printer_type, "");
                        print_json.diff2all_base_reset(j_pre);
                    }
                }
            }
        }
        if (j_pre.contains("system")) {
            if (j_pre["system"].contains("command")) {
                if (j_pre["system"]["command"].get<std::string>() == "get_access_code") {
                    if (j_pre["system"].contains("access_code")) {
                        std::string access_code = j_pre["system"]["access_code"].get<std::string>();
                        if (!access_code.empty()) {
                            set_access_code(access_code);
                            set_user_access_code(access_code);
                        }
                    }
                }
            }
        }
        if (!restored_json) {
            j = j_pre;
        }

        uint64_t t_utc = j.value("t_utc", 0ULL);
        if (t_utc > 0) {
            last_utc_time = std::chrono::system_clock::time_point(t_utc * 1ms);
            std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
            auto millisec_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            auto delay = millisec_since_epoch - t_utc;  //ms

            std::string message_type = is_lan_mode_printer() ? "Local Mqtt" : is_tunnel_mqtt ? "Tunnel Mqtt" : "Cloud Mqtt";
            if (!message_delay.empty()) {
                const auto& [first_type, first_time_stamp, first_delay] = message_delay.front();
                const auto& [last_type, last_time_stap, last_delay] = message_delay.back();

                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ", message delay, last time stamp: " << last_time_stap;
                if (last_time_stap - first_time_stamp >= MINUTE_30) {
                    // record, excluding current data
                    int total = message_delay.size();
                    int local_mqtt = 0;
                    int tunnel_mqtt = 0;
                    int cloud_mqtt = 0;

                    int local_mqtt_timeout = 0;
                    int tunnel_mqtt_timeout = 0;
                    int cloud_mqtt_timeout = 0;

                    for (const auto& [type, time_stamp, delay] : message_delay) {
                        if (type == "Local Mqtt") {
                            local_mqtt++;
                            if (delay >= TIME_OUT) {
                                local_mqtt_timeout++;
                            }
                        }
                        else if (type == "Tunnel Mqtt") {
                            tunnel_mqtt++;
                            if (delay >= TIME_OUT) {
                                tunnel_mqtt_timeout++;
                            }
                        }
                        else if (type == "Cloud Mqtt"){
                            cloud_mqtt++;
                            if (delay >= TIME_OUT) {
                                cloud_mqtt_timeout++;
                            }
                        }
                    }

                    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ", message delay, message total: " << total;
                    try {
                        if (m_agent) {
                            json j_message;

                            // Convert timestamp to time
                            std::time_t t = time_t(last_time_stap / 1000);  //s
                            std::tm* now_tm = std::localtime(&t);
                            char buffer[80];
                            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", now_tm);

                            std::string time_str = std::string(buffer);
                            j_message["time"] = time_str;
                            j_message["total"] = total;
                            j_message["local_mqtt"] = std::to_string(local_mqtt_timeout) + "/" + std::to_string(local_mqtt);
                            j_message["tunnel_mqtt"] = std::to_string(tunnel_mqtt_timeout) + "/" + std::to_string(tunnel_mqtt);
                            j_message["cloud_mqtt"] = std::to_string(cloud_mqtt_timeout) + "/" + std::to_string(cloud_mqtt);

                            m_agent->track_event("message_delay", j_message.dump());
                        }
                    }
                    catch (...) {}

                    message_delay.clear();
                    message_delay.shrink_to_fit();
                }
            }
            message_delay.push_back(std::make_tuple(message_type, t_utc, delay));
        }
        else
            last_utc_time = last_update_time;

        BOOST_LOG_TRIVIAL(trace) << "parse_json: dev_id=" << dev_id << ", playload=" << j.dump(4);

        // Parse version info first, as if version arrive or change, 'print' need parse again with new compatible settings
        try {
            if (j.contains("info")) {
                if (j["info"].contains("command") && j["info"]["command"].get<std::string>() == "get_version") {
                    json j_module = j["info"]["module"];
                    module_vers.clear();
                    for (auto it = j_module.begin(); it != j_module.end(); it++) {
                        ModuleVersionInfo ver_info;
                        ver_info.name = (*it)["name"].get<std::string>();
                        if ((*it).contains("product_name"))
                            ver_info.product_name = wxString::FromUTF8((*it)["product_name"].get<string>());
                        if ((*it).contains("sw_ver"))
                            ver_info.sw_ver = (*it)["sw_ver"].get<std::string>();
                        if ((*it).contains("sw_new_ver"))
                            ver_info.sw_new_ver = (*it)["sw_new_ver"].get<std::string>();
                        if ((*it).contains("sn"))
                            ver_info.sn = (*it)["sn"].get<std::string>();
                        if ((*it).contains("hw_ver"))
                            ver_info.hw_ver = (*it)["hw_ver"].get<std::string>();
                        if((*it).contains("flag"))
                            ver_info.firmware_status= (*it)["flag"].get<int>();

                        store_version_info(ver_info);
                        if (ver_info.name == "ota") {
                            NetworkAgent* agent = GUI::wxGetApp().getAgent();
                            if (agent) {
                                std::string dev_ota_str = "dev_ota_ver:" + this->dev_id;
                                agent->track_update_property(dev_ota_str, ver_info.sw_ver);
                            }
                        }
                    }

                    parse_version_func();

                    bool get_version_result = true;
                    if (j["info"].contains("result"))
                        if (j["info"]["result"].get<std::string>() == "fail")
                            get_version_result = false;
                    if ((!check_version_valid() && get_version_retry-- >= 0)
                        && get_version_result) {
                            BOOST_LOG_TRIVIAL(info) << "get_version_retry = " << get_version_retry;
                            boost::thread retry = boost::thread([this] {
                                boost::this_thread::sleep_for(boost::chrono::milliseconds(RETRY_INTERNAL));
                                GUI::wxGetApp().CallAfter([this] {
                                    this->command_get_version(false);
                            });
                        });
                    }
                }
                std::string version = parse_version();
                if (!version.empty() && print_json.load_compatible_settings(printer_type, version)) {
                    // reload because compatible settings changed
                    j.clear();
                    print_json.diff2all(json{}, j);
                }

            }
        } catch (...) {}


        if (j.contains("print")) {
            json jj = j["print"];
            int sequence_id = 0;
            if (jj.contains("sequence_id")) {
                if (jj["sequence_id"].is_string()) {
                    std::string str_seq = jj["sequence_id"].get<std::string>();
                    try {
                        sequence_id = stoi(str_seq);
                    }
                    catch(...) {
                        ;
                    }
                }
            }

            if (!key_field_only) {
                if (!DeviceManager::EnableMultiMachine && !is_support_agora) {
                    if (jj.contains("support_tunnel_mqtt")) {
                        if (jj["support_tunnel_mqtt"].is_boolean()) {
                            is_support_tunnel_mqtt = jj["support_tunnel_mqtt"].get<bool>();
                        }
                    }
                }

                //supported function
                if (jj.contains("support_chamber_temp_edit")) {
                    if (jj["support_chamber_temp_edit"].is_boolean()) {
                        is_support_chamber_edit = jj["support_chamber_temp_edit"].get<bool>();
                    }
                }

                if (jj.contains("support_extrusion_cali")) {
                    if (jj["support_extrusion_cali"].is_boolean()) {
                        is_support_extrusion_cali = jj["support_extrusion_cali"].get<bool>();
                    }
                }

                if (jj.contains("support_first_layer_inspect")) {
                    if (jj["support_first_layer_inspect"].is_boolean()) {
                        is_support_first_layer_inspect = jj["support_first_layer_inspect"].get<bool>();
                    }
                }

                if (jj.contains("support_ai_monitoring")) {
                    if (jj["support_ai_monitoring"].is_boolean()) {
                        is_support_ai_monitoring = jj["support_ai_monitoring"].get<bool>();
                    }
                }

                if (jj.contains("support_lidar_calibration")) {
                    if (jj["support_lidar_calibration"].is_boolean()) {
                        is_support_lidar_calibration = jj["support_lidar_calibration"].get<bool>();
                    }
                }

                if (jj.contains("support_build_plate_marker_detect")) {
                    if (jj["support_build_plate_marker_detect"].is_boolean()) {
                        is_support_build_plate_marker_detect = jj["support_build_plate_marker_detect"].get<bool>();
                    }
                }

                if (jj.contains("support_flow_calibration")) {
                    if (jj["support_flow_calibration"].is_boolean()) {
                        is_support_auto_flow_calibration = jj["support_flow_calibration"].get<bool>();
                    }
                }

                if (jj.contains("support_print_without_sd")) {
                    if (jj["support_print_without_sd"].is_boolean()) {
                        is_support_print_without_sd = jj["support_print_without_sd"].get<bool>();
                    }
                }

                if (jj.contains("support_print_all")) {
                    if (jj["support_print_all"].is_boolean()) {
                        is_support_print_all = jj["support_print_all"].get<bool>();
                    }
                }
                if (jj.contains("support_send_to_sd")) {
                    if (jj["support_send_to_sd"].is_boolean()) {
                        is_support_send_to_sdcard = jj["support_send_to_sd"].get<bool>();
                    }
                }

                if (jj.contains("support_aux_fan")) {
                    if (jj["support_aux_fan"].is_boolean()) {
                        is_support_aux_fan = jj["support_aux_fan"].get<bool>();
                    }
                }

                if (jj.contains("support_chamber_fan")) {
                    if (jj["support_chamber_fan"].is_boolean()) {
                        is_support_chamber_fan = jj["support_chamber_fan"].get<bool>();
                    }
                }

                if (jj.contains("support_filament_backup")) {
                    if (jj["support_filament_backup"].is_boolean()) {
                        is_support_filament_backup = jj["support_filament_backup"].get<bool>();
                    }
                }

                if (jj.contains("support_update_remain")) {
                    if (jj["support_update_remain"].is_boolean()) {
                        is_support_update_remain = jj["support_update_remain"].get<bool>();
                    }
                }

                if (jj.contains("support_auto_leveling")) {
                    if (jj["support_auto_leveling"].is_boolean()) {
                        is_support_auto_leveling = jj["support_auto_leveling"].get<bool>();
                    }
                }

                if (jj.contains("support_auto_recovery_step_loss")) {
                    if (jj["support_auto_recovery_step_loss"].is_boolean()) {
                        is_support_auto_recovery_step_loss = jj["support_auto_recovery_step_loss"].get<bool>();
                    }
                }

                if (jj.contains("support_ams_humidity")) {
                    if (jj["support_ams_humidity"].is_boolean()) {
                        is_support_ams_humidity = jj["support_ams_humidity"].get<bool>();
                    }
                }

                if (jj.contains("support_prompt_sound")) {
                    if (jj["support_prompt_sound"].is_boolean()) {
                        is_support_prompt_sound = jj["support_prompt_sound"].get<bool>();
                    }
                }

                if (jj.contains("support_filament_tangle_detect")) {
                    if (jj["support_filament_tangle_detect"].is_boolean()) {
                        is_support_filament_tangle_detect = jj["support_filament_tangle_detect"].get<bool>();
                    }
                }

                if (jj.contains("support_1080dpi")) {
                    if (jj["support_1080dpi"].is_boolean()) {
                        is_support_1080dpi = jj["support_1080dpi"].get<bool>();
                    }
                }

                if (jj.contains("support_cloud_print_only")) {
                    if (jj["support_cloud_print_only"].is_boolean()) {
                        is_support_cloud_print_only = jj["support_cloud_print_only"].get<bool>();
                    }
                }

                if (jj.contains("support_command_ams_switch")) {
                    if (jj["support_command_ams_switch"].is_boolean()) {
                        is_support_command_ams_switch = jj["support_command_ams_switch"].get<bool>();
                    }
                }

                if (jj.contains("support_mqtt_alive")) {
                    if (jj["support_mqtt_alive"].is_boolean()) {
                        is_support_mqtt_alive = jj["support_mqtt_alive"].get<bool>();
                    }
                }

                if (jj.contains("support_motor_noise_cali")) {
                    if (jj["support_motor_noise_cali"].is_boolean()) {
                        is_support_motor_noise_cali = jj["support_motor_noise_cali"].get<bool>();
                    }
                }

                if (jj.contains("support_timelapse")) {
                    if (jj["support_timelapse"].is_boolean()) {
                        is_support_timelapse = jj["support_timelapse"].get<bool>();
                    }
                }

                if (jj.contains("support_user_preset")) {
                    if (jj["support_user_preset"].is_boolean()) {
                        is_support_user_preset = jj["support_user_preset"].get<bool>();
                    }
                }

                if (jj.contains("nozzle_max_temperature")) {
                    if (jj["nozzle_max_temperature"].is_number_integer()) {
                        nozzle_max_temperature = jj["nozzle_max_temperature"].get<int>();
                    }
                }

                if (jj.contains("bed_temperature_limit")) {
                    if (jj["bed_temperature_limit"].is_number_integer()) {
                        bed_temperature_limit = jj["bed_temperature_limit"].get<int>();
                    }
                }
            }

            if (jj.contains("command")) {
                if (jj["command"].get<std::string>() == "ams_change_filament") {
                    if (jj.contains("errno")) {
                        if (jj["errno"].is_number()) {
                            if (jj["errno"].get<int>() == -2) {
                                wxString text = _L("The current chamber temperature or the target chamber temperature exceeds 45\u2103. " /* 45°C */
                                                   "In order to avoid extruder clogging, low temperature filament (PLA/PETG/TPU) is not allowed to be loaded.");
                                GUI::wxGetApp().push_notification(text);
                            }
                        }
                    }
                }

                if (jj["command"].get<std::string>() == "set_ctt") {
                    if (m_agent && is_studio_cmd(sequence_id)) {
                        if (jj["errno"].is_number()) {
                            wxString text;
                            if (jj["errno"].get<int>() == -2) {
                                 text = _L("Low temperature filament (PLA/PETG/TPU) is loaded in the extruder. "
                                           "In order to avoid extruder clogging, it is not allowed to set the chamber temperature above 45\u2103." /* 45°C */);
                            }
                            else if (jj["errno"].get<int>() == -4) {
                                 text = _L("When you set the chamber temperature below 40\u2103, the chamber temperature control will not be activated, "
                                           "and the target chamber temperature will automatically be set to 0\u2103." /* 0°C */);
                            }
                            if(!text.empty()){
#if __WXOSX__
                            set_ctt_dlg(text);
#else
                            GUI::wxGetApp().push_notification(text);
#endif
                            }
                        }
                    }
                }
                if (jj["command"].get<std::string>() == "push_status") {
                    m_push_count++;
                    last_push_time = last_update_time;
#pragma region printing
                    // U0 firmware
                    if (jj.contains("print_type")) {
                        print_type = jj["print_type"].get<std::string>();
                    }
                    if (jj.contains("mc_percent")) {
                        if (jj["mc_percent"].is_string())
                            mc_print_percent = stoi(j["print"]["mc_percent"].get<std::string>());
                        else if (jj["mc_percent"].is_number_integer())
                            mc_print_percent = j["print"]["mc_percent"].get<int>();
                    }
                    if (jj.contains("mc_print_sub_stage")) {
                        if (jj["mc_print_sub_stage"].is_number_integer())
                            mc_print_sub_stage = j["print"]["mc_print_sub_stage"].get<int>();
                    }
                    /* printing */
                    if (jj.contains("mc_print_stage")) {
                        if (jj["mc_print_stage"].is_string())
                            mc_print_stage = atoi(jj["mc_print_stage"].get<std::string>().c_str());
                        if (jj["mc_print_stage"].is_number())
                            mc_print_stage = jj["mc_print_stage"].get<int>();
                    }
                    if (jj.contains("mc_print_error_code")) {
                        if (jj["mc_print_error_code"].is_number())
                            mc_print_error_code = jj["mc_print_error_code"].get<int>();
                    }
                    if (jj.contains("mc_remaining_time")) {
                        if (jj["mc_remaining_time"].is_string())
                            mc_left_time = stoi(j["print"]["mc_remaining_time"].get<std::string>()) * 60;
                        else if (jj["mc_remaining_time"].is_number_integer())
                            mc_left_time = j["print"]["mc_remaining_time"].get<int>() * 60;
                    }
                    if (jj.contains("print_error")) {
                        if (jj["print_error"].is_number())
                            print_error = jj["print_error"].get<int>();
                    }

                    if (jj.contains("sdcard")) {
                        if (jj["sdcard"].get<bool>())
                            sdcard_state = MachineObject::SdcardState::HAS_SDCARD_NORMAL;
                        else
                            sdcard_state = MachineObject::SdcardState::NO_SDCARD;
                    } else {
                        sdcard_state = MachineObject::SdcardState::NO_SDCARD;
                    }

                    if (!key_field_only) {
                        if (jj.contains("home_flag")) {
                            home_flag = jj["home_flag"].get<int>();
                            parse_status(home_flag);
                        }

                        /*the param is invalid in np for Yeshu*/
                        if (jj.contains("hw_switch_state")) {
                            hw_switch_state = jj["hw_switch_state"].get<int>();
                            m_extder_data.extders[MAIN_NOZZLE_ID].ext_has_filament = hw_switch_state;
                        }
                        if (jj.contains("mc_print_line_number")) {
                            if (jj["mc_print_line_number"].is_string() && !jj["mc_print_line_number"].is_null())
                                mc_print_line_number = atoi(jj["mc_print_line_number"].get<std::string>().c_str());
                        }
                    }
                    if (!key_field_only) {
                        if (jj.contains("flag3")) {
                            int flag3 = jj["flag3"].get<int>();
                            is_support_filament_setting_inprinting =  get_flag_bits(flag3, 3);
                            is_enable_ams_np =  get_flag_bits(flag3, 9);
                        }
                    }
                    if (!key_field_only) {
                        if (jj.contains("net")) {
                            if (jj["net"].contains("conf")) {
                                network_wired = (jj["net"]["conf"].get<int>() & (0x1)) != 0;
                            }
                        }
                    }
#pragma endregion

#pragma region online
                    if (!key_field_only) {
                        // parse online info
                        try {
                            if (jj.contains("online")) {
                                if (jj["online"].contains("ahb")) {
                                    if (jj["online"]["ahb"].get<bool>()) {
                                        online_ahb = true;
                                    } else {
                                        online_ahb = false;
                                    }
                                }
                                if (jj["online"].contains("rfid")) {
                                    if (jj["online"]["rfid"].get<bool>()) {
                                        online_rfid = true;
                                    } else {
                                        online_rfid = false;
                                    }
                                }
                                std::string str = jj.dump();
                                if (jj["online"].contains("version")) {
                                    online_version = jj["online"]["version"].get<int>();
                                }
                                if (last_online_version != online_version) {
                                    last_online_version = online_version;
                                    GUI::wxGetApp().CallAfter([this] {
                                        this->command_get_version();
                                        });
                                }
                            }
                        } catch (...) {
                            ;
                        }
                    }
#pragma endregion

#pragma region print_task
                    if (jj.contains("gcode_state")) {
                        this->set_print_state(jj["gcode_state"].get<std::string>());
                    }
                    if (jj.contains("job_id")) {
                        is_support_wait_sending_finish = true;
                        this->job_id_ = jj["job_id"].get<std::string>();
                    }
                    else {
                        is_support_wait_sending_finish = false;
                    }

                    if (jj.contains("subtask_name")) {
                        subtask_name = jj["subtask_name"].get<std::string>();
                    }

                    if (!key_field_only) {
                        if (jj.contains("printer_type")) {
                            printer_type = parse_printer_type(jj["printer_type"].get<std::string>());
                        }

                        if (jj.contains("layer_num")) {
                            curr_layer = jj["layer_num"].get<int>();
                        }
                        if (jj.contains("total_layer_num")) {
                            total_layers = jj["total_layer_num"].get<int>();
                            if (total_layers == 0)
                                is_support_layer_num = false;
                            else
                                is_support_layer_num = true;
                        }
                        else {
                            is_support_layer_num = false;
                        }
                        if (jj.contains("queue_number")) {
                            this->queue_number = jj["queue_number"].get<int>();
                        }
                        else {
                            this->queue_number = 0;
                        }

                        if (jj.contains("task_id")) {
                            this->task_id_ = jj["task_id"].get<std::string>();
                        }

                        if (jj.contains("gcode_file"))
                            this->m_gcode_file = jj["gcode_file"].get<std::string>();
                        if (jj.contains("gcode_file_prepare_percent")) {
                            std::string percent_str = jj["gcode_file_prepare_percent"].get<std::string>();
                            if (!percent_str.empty()) {
                                try {
                                    this->gcode_file_prepare_percent = atoi(percent_str.c_str());
                                }
                                catch (...) {}
                            }
                        }
                    }

                    if (jj.contains("project_id")
                        && jj.contains("profile_id")
                        && jj.contains("subtask_id")
                        ) {
                        obj_subtask_id = jj["subtask_id"].get<std::string>();

                        int plate_index = -1;
                        /* parse local plate_index from task */
                        if (obj_subtask_id.compare("0") == 0 && jj["profile_id"].get<std::string>() != "0") {
                            if (jj.contains("gcode_file")) {
                                m_gcode_file = jj["gcode_file"].get<std::string>();
                                int idx_start = m_gcode_file.find_last_of("_") + 1;
                                int idx_end = m_gcode_file.find_last_of(".");
                                if (idx_start > 0 && idx_end > idx_start) {
                                    try {
                                        plate_index = atoi(m_gcode_file.substr(idx_start, idx_end - idx_start).c_str());
                                        this->m_plate_index = plate_index;
                                    }
                                    catch (...) {
                                        ;
                                    }
                                }
                            }
                        }
                        update_slice_info(jj["project_id"].get<std::string>(), jj["profile_id"].get<std::string>(), jj["subtask_id"].get<std::string>(), plate_index);
                        BBLSubTask* curr_task = get_subtask();
                        if (curr_task) {
                            curr_task->task_progress = mc_print_percent;
                            curr_task->printing_status = print_status;
                            curr_task->task_id = jj["subtask_id"].get<std::string>();
                        }
                    }

#pragma endregion

#pragma region status
                    if (!key_field_only) {
                        /* temperature */
                        if (jj.contains("bed_temper")) {
                            if (jj["bed_temper"].is_number()) {
                                bed_temp = jj["bed_temper"].get<float>();
                            }
                        }
                        if (jj.contains("bed_target_temper")) {
                            if (jj["bed_target_temper"].is_number()) {
                                bed_temp_target = jj["bed_target_temper"].get<float>();
                            }
                        }
                        if (jj.contains("frame_temper")) {
                            if (jj["frame_temper"].is_number()) {
                                frame_temp = jj["frame_temper"].get<float>();
                            }
                        }
                        if (jj.contains("nozzle_temper")) {
                            if (jj["nozzle_temper"].is_number()) {
                                if (m_extder_data.extders.size() == 1) {
                                    m_extder_data.extders[MAIN_NOZZLE_ID].temp = jj["nozzle_temper"].get<float>();
                                }
                            }
                        }
                        if (jj.contains("nozzle_target_temper")) {
                            if (jj["nozzle_target_temper"].is_number()) {
                                if (m_extder_data.extders.size() == 1) {
                                    m_extder_data.extders[MAIN_NOZZLE_ID].target_temp = jj["nozzle_target_temper"].get<float>();
                                }
                            }
                        }
                        if (jj.contains("chamber_temper")) {
                            if (jj["chamber_temper"].is_number()) {
                                chamber_temp = jj["chamber_temper"].get<float>();
                            }
                        }
                        if (jj.contains("ctt")) {
                            if (jj["ctt"].is_number()) {
                                chamber_temp_target = jj["ctt"].get<float>();
                            }
                        }
                        /* signals */
                        if (jj.contains("link_th_state"))
                            link_th = jj["link_th_state"].get<std::string>();
                        if (jj.contains("link_ams_state"))
                            link_ams = jj["link_ams_state"].get<std::string>();
                        if (jj.contains("wifi_signal"))
                            wifi_signal = jj["wifi_signal"].get<std::string>();

                        /* cooling */
                        if (jj.contains("fan_gear")) {
                            fan_gear = jj["fan_gear"].get<std::uint32_t>();
                            big_fan2_speed = (int)((fan_gear & 0x00FF0000) >> 16);
                            big_fan1_speed = (int)((fan_gear & 0x0000FF00) >> 8);
                            cooling_fan_speed = (int)((fan_gear & 0x000000FF) >> 0);
                        }
                        else {
                            if (jj.contains("cooling_fan_speed")) {
                                cooling_fan_speed = stoi(jj["cooling_fan_speed"].get<std::string>());
                                cooling_fan_speed = round(floor(cooling_fan_speed / float(1.5)) * float(25.5));
                            }
                            else {
                                cooling_fan_speed = 0;
                            }

                            if (jj.contains("big_fan1_speed")) {
                                big_fan1_speed = stoi(jj["big_fan1_speed"].get<std::string>());
                                big_fan1_speed = round( floor(big_fan1_speed / float(1.5)) * float(25.5) );
                            }
                            else {
                                big_fan1_speed = 0;
                            }

                            if (jj.contains("big_fan2_speed")) {
                                big_fan2_speed = stoi(jj["big_fan2_speed"].get<std::string>());
                                big_fan2_speed = round( floor(big_fan2_speed / float(1.5)) * float(25.5) );
                            }
                            else {
                                big_fan2_speed = 0;
                            }
                        }

                        if (jj.contains("heatbreak_fan_speed")) {
                            heatbreak_fan_speed = stoi(jj["heatbreak_fan_speed"].get<std::string>());
                        }

                        /* parse speed */
                        try {
                            if (jj.contains("spd_lvl")) {
                                printing_speed_lvl = (PrintingSpeedLevel)jj["spd_lvl"].get<int>();
			    }
                            if (jj.contains("spd_mag")) {
                                printing_speed_mag = jj["spd_mag"].get<int>();
                            }
                            if (jj.contains("heatbreak_fan_speed")) {
                                heatbreak_fan_speed = stoi(jj["heatbreak_fan_speed"].get<std::string>());
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }

                    try {
                        if (jj.contains("stg")) {
                            stage_list_info.clear();
                            if (jj["stg"].is_array()) {
                                for (auto it = jj["stg"].begin(); it != jj["stg"].end(); it++) {
                                    for (auto kv = (*it).begin(); kv != (*it).end(); kv++) {
                                        stage_list_info.push_back(kv.value().get<int>());
                                    }
                                }
                            }
                        }
                        if (jj.contains("stg_cur")) {
                            stage_curr = jj["stg_cur"].get<int>();
                        }
                    }
                    catch (...) {
                        ;
                    }

                    if (!key_field_only) {
                        /*get filam_bak*/
                        try {
                            m_extder_data.extders[MAIN_NOZZLE_ID].filam_bak.clear();

                            if (jj.contains("filam_bak")) {
                                is_support_show_filament_backup = true;
                                if (jj["filam_bak"].is_array()) {
                                    for (auto it = jj["filam_bak"].begin(); it != jj["filam_bak"].end(); it++) {
                                        const auto& filam_bak_val = it.value().get<int>();
                                        m_extder_data.extders[MAIN_NOZZLE_ID].filam_bak.push_back(filam_bak_val);
                                    }
                                }
                            }
                            else {
                                is_support_show_filament_backup = false;
                            }
                        }
                        catch (...) {
                            ;
                        }

                        /* get fimware type */
                        try {
                            if (jj.contains("mess_production_state")) {
                                if (jj["mess_production_state"].get<std::string>() == "engineer")
                                    firmware_type = PrinterFirmwareType::FIRMWARE_TYPE_ENGINEER;
                                else if (jj["mess_production_state"].get<std::string>() == "product")
                                    firmware_type = PrinterFirmwareType::FIRMWARE_TYPE_PRODUCTION;
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }
                    if (!key_field_only) {
                        try {
                            if (jj.contains("lifecycle")) {
                                if (jj["lifecycle"].get<std::string>() == "engineer")
                                    lifecycle = PrinterFirmwareType::FIRMWARE_TYPE_ENGINEER;
                                else if (jj["lifecycle"].get<std::string>() == "product")
                                    lifecycle = PrinterFirmwareType::FIRMWARE_TYPE_PRODUCTION;
                            }
                        }
                        catch (...) {
                            ;
                        }

                        try {
                            if (jj.contains("lights_report") && jj["lights_report"].is_array()) {
                                for (auto it = jj["lights_report"].begin(); it != jj["lights_report"].end(); it++) {
                                    if ((*it)["node"].get<std::string>().compare("chamber_light") == 0)
                                        chamber_light = light_effect_parse((*it)["mode"].get<std::string>());
                                    if ((*it)["node"].get<std::string>().compare("work_light") == 0)
                                        work_light = light_effect_parse((*it)["mode"].get<std::string>());
                                }
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }
#pragma endregion
                    if (!key_field_only) {
                        try {
                            if (jj.contains("nozzle_diameter")) {
                                if (nozzle_setting_hold_count > 0) {
                                    nozzle_setting_hold_count--;
                                } else {
                                    float nozzle_diameter = 0.0f;
                                    if (jj["nozzle_diameter"].is_number_float()) {
                                        nozzle_diameter = jj["nozzle_diameter"].get<float>();
                                    }
                                    else if (jj["nozzle_diameter"].is_string()) {
                                        nozzle_diameter = string_to_float(jj["nozzle_diameter"].get<std::string>());
                                    }

                                    if (nozzle_diameter == 0.0f) {m_extder_data.extders[MAIN_NOZZLE_ID].current_nozzle_diameter = 0.0f;}
                                    else { m_extder_data.extders[MAIN_NOZZLE_ID].current_nozzle_diameter = round(nozzle_diameter * 10) / 10;}
                                }
                            }
                        }
                        catch(...) {
                            ;
                        }

                        try {
                            if (jj.contains("nozzle_type")) {

                                if (nozzle_setting_hold_count > 0) {
                                    nozzle_setting_hold_count--;
                                }
                                else {
                                    if (jj["nozzle_type"].is_string()) {
                                        auto nozzle_type = jj["nozzle_type"].get<std::string>();
                                        if (nozzle_type.empty()) {
                                            m_extder_data.extders[MAIN_NOZZLE_ID].current_nozzle_type = NozzleType::ntUndefine;
                                        }
                                        else {
                                            m_extder_data.extders[MAIN_NOZZLE_ID].current_nozzle_type = NozzleTypeStrToEumn[nozzle_type];
                                        }
                                    }
                                }
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }

#pragma region upgrade
                    try {
                        if (jj.contains("upgrade_state")) {
                            if (jj["upgrade_state"].contains("status"))
                                upgrade_status = jj["upgrade_state"]["status"].get<std::string>();
                            if (jj["upgrade_state"].contains("progress")) {
                                upgrade_progress = jj["upgrade_state"]["progress"].get<std::string>();
                            } if (jj["upgrade_state"].contains("new_version_state"))
                                upgrade_new_version = jj["upgrade_state"]["new_version_state"].get<int>() == 1 ? true : false;
                            if (!check_enable_np(jj) && jj["upgrade_state"].contains("ams_new_version_number"))/* is not used in new np, by AP*/
                                ams_new_version_number = jj["upgrade_state"]["ams_new_version_number"].get<std::string>();
                            if (jj["upgrade_state"].contains("ota_new_version_number"))
                                ota_new_version_number = jj["upgrade_state"]["ota_new_version_number"].get<std::string>();
                            if (jj["upgrade_state"].contains("ahb_new_version_number"))
                                ahb_new_version_number = jj["upgrade_state"]["ahb_new_version_number"].get<std::string>();
                            if (jj["upgrade_state"].contains("module"))
                                upgrade_module = jj["upgrade_state"]["module"].get<std::string>();
                            if (jj["upgrade_state"].contains("message"))
                                upgrade_message = jj["upgrade_state"]["message"].get<std::string>();
                            if (jj["upgrade_state"].contains("consistency_request"))
                                upgrade_consistency_request = jj["upgrade_state"]["consistency_request"].get<bool>();
                            if (jj["upgrade_state"].contains("force_upgrade"))
                                upgrade_force_upgrade = jj["upgrade_state"]["force_upgrade"].get<bool>();
                            if (jj["upgrade_state"].contains("err_code"))
                                upgrade_err_code = jj["upgrade_state"]["err_code"].get<int>();
                            if (jj["upgrade_state"].contains("dis_state")) {
                                if (upgrade_display_state != jj["upgrade_state"]["dis_state"].get<int>()
                                    && jj["upgrade_state"]["dis_state"].get<int>() == 3) {
                                    GUI::wxGetApp().CallAfter([this] {
                                        this->command_get_version();
                                        });
                                }
                                if (upgrade_display_hold_count > 0)
                                    upgrade_display_hold_count--;
                                else
                                    upgrade_display_state = jj["upgrade_state"]["dis_state"].get<int>();
                            }
                            else {
                                if (upgrade_display_hold_count > 0)
                                    upgrade_display_hold_count--;
                                else {
                                    //BBS compatibility with old version
                                    if (upgrade_status == "DOWNLOADING"
                                        || upgrade_status == "FLASHING"
                                        || upgrade_status == "UPGRADE_REQUEST"
                                        || upgrade_status == "PRE_FLASH_START"
                                        || upgrade_status == "PRE_FLASH_SUCCESS") {
                                        upgrade_display_state = (int)UpgradingDisplayState::UpgradingInProgress;
                                    }
                                    else if (upgrade_status == "UPGRADE_SUCCESS"
                                        || upgrade_status == "DOWNLOAD_FAIL"
                                        || upgrade_status == "FLASH_FAIL"
                                        || upgrade_status == "PRE_FLASH_FAIL"
                                        || upgrade_status == "UPGRADE_FAIL") {
                                        upgrade_display_state = (int)UpgradingDisplayState::UpgradingFinished;
                                    }
                                    else {
                                        if (upgrade_new_version) {
                                            upgrade_display_state = (int)UpgradingDisplayState::UpgradingAvaliable;
                                        }
                                        else {
                                            upgrade_display_state = (int)UpgradingDisplayState::UpgradingUnavaliable;
                                        }
                                    }
                                }
                            }
                            // new ver list
                            if (jj["upgrade_state"].contains("new_ver_list")) {
                                m_new_ver_list_exist = true;
                                new_ver_list.clear();
                                for (auto ver_item = jj["upgrade_state"]["new_ver_list"].begin(); ver_item != jj["upgrade_state"]["new_ver_list"].end(); ver_item++) {
                                    ModuleVersionInfo ver_info;
                                    if (ver_item->contains("name"))
                                        ver_info.name = (*ver_item)["name"].get<std::string>();
                                    else
                                        continue;

                                    if (ver_item->contains("cur_ver"))
                                        ver_info.sw_ver = (*ver_item)["cur_ver"].get<std::string>();
                                    if (ver_item->contains("new_ver"))
                                        ver_info.sw_new_ver = (*ver_item)["new_ver"].get<std::string>();

                                    if (ver_info.name == "ota") {
                                        ota_new_version_number = ver_info.sw_new_ver;
                                    }

                                    new_ver_list.insert(std::make_pair(ver_info.name, ver_info));
                                }
                            }
                            else {
                                new_ver_list.clear();
                            }
                        }
                    }
                    catch (...) {
                        ;
                    }
#pragma endregion

#pragma region  camera
                    if (!key_field_only) {
                        // parse camera info
                        try {
                            if (jj.contains("ipcam")) {
                                json const & ipcam = jj["ipcam"];
                                if (ipcam.contains("ipcam_record")) {
                                    if (camera_recording_hold_count > 0)
                                        camera_recording_hold_count--;
                                    else {
                                        if (ipcam["ipcam_record"].get<std::string>() == "enable") {
                                            camera_recording_when_printing = true;
                                        }
                                        else {
                                            camera_recording_when_printing = false;
                                        }
                                    }
                                }
                                if (ipcam.contains("timelapse")) {
                                    if (camera_timelapse_hold_count > 0)
                                        camera_timelapse_hold_count--;
                                    else {
                                        if (ipcam["timelapse"].get<std::string>() == "enable") {
                                            camera_timelapse = true;
                                        }
                                        else {
                                            camera_timelapse = false;
                                        }
                                    }
                                }
                                if (ipcam.contains("ipcam_dev")) {
                                    if (ipcam["ipcam_dev"].get<std::string>() == "1") {
                                        has_ipcam = true;
                                    } else {
                                        has_ipcam = false;
                                    }
                                }
                                if (ipcam.contains("resolution")) {
                                    if (camera_resolution_hold_count > 0)
                                        camera_resolution_hold_count--;
                                    else {
                                        camera_resolution = ipcam["resolution"].get<std::string>();
                                    }
                                }
                                if (ipcam.contains("resolution_supported")) {
                                    std::vector<std::string> resolution_supported;
                                    for (auto res : ipcam["resolution_supported"])
                                        resolution_supported.emplace_back(res.get<std::string>());
                                    camera_resolution_supported.swap(resolution_supported);
                                }
                                if (ipcam.contains("liveview")) {
                                    char const *local_protos[] = {"none", "disabled", "local", "rtsps", "rtsp"};
                                    liveview_local = enum_index_of(ipcam["liveview"].value<std::string>("local", "none").c_str(), local_protos, 5, LiveviewLocal::LVL_None);
                                    char const *remote_protos[] = {"none", "tutk", "agora", "tutk_agaro"};
                                    liveview_remote = enum_index_of(ipcam["liveview"].value<std::string>("remote", "none").c_str(), remote_protos, 4, LiveviewRemote::LVR_None);
                                }
                                if (ipcam.contains("file")) {
                                    char const *local_protos[] = {"none", "local"};
                                    file_local  = enum_index_of(ipcam["file"].value<std::string>("local", "none").c_str(), local_protos, 2, FileLocal::FL_None);
                                    char const *remote_protos[] = {"none", "tutk", "agora", "tutk_agaro"};
                                    file_remote = enum_index_of(ipcam["file"].value<std::string>("remote", "none").c_str(), remote_protos, 4, FileRemote::FR_None);
                                    file_model_download = ipcam["file"].value<std::string>("model_download", "disabled") == "enabled";
                                }
                                virtual_camera = ipcam.value<std::string>("virtual_camera", "disabled") == "enabled";
                                if (ipcam.contains("rtsp_url")) {
                                    local_rtsp_url = ipcam["rtsp_url"].get<std::string>();
                                    liveview_local = local_rtsp_url.empty() ? LVL_None : local_rtsp_url == "disable"
                                            ? LVL_Disable : boost::algorithm::starts_with(local_rtsp_url, "rtsps") ? LVL_Rtsps : LVL_Rtsp;
                                }
                                if (ipcam.contains("tutk_server")) {
                                    tutk_state = ipcam["tutk_server"].get<std::string>();
                                }
                            }
                        }
                        catch (...) {
                            ;
                        }

                        try {
                            if (jj.contains("xcam")) {
                                if (xcam_ai_monitoring_hold_count > 0)
                                    xcam_ai_monitoring_hold_count--;
                                else {
                                    if (jj["xcam"].contains("printing_monitor")) {
                                        // new protocol
                                        xcam_ai_monitoring = jj["xcam"]["printing_monitor"].get<bool>();
                                    } else {
                                        // old version protocol
                                        if (jj["xcam"].contains("spaghetti_detector")) {
                                            xcam_ai_monitoring = jj["xcam"]["spaghetti_detector"].get<bool>();
                                            if (jj["xcam"].contains("print_halt")) {
                                                bool print_halt = jj["xcam"]["print_halt"].get<bool>();
                                                if (print_halt) { xcam_ai_monitoring_sensitivity = "medium"; }
                                            }
                                        }
                                    }
                                    if (jj["xcam"].contains("halt_print_sensitivity")) {
                                        xcam_ai_monitoring_sensitivity = jj["xcam"]["halt_print_sensitivity"].get<std::string>();
                                    }
                                }

                                if (xcam_first_layer_hold_count > 0)
                                    xcam_first_layer_hold_count--;
                                else {
                                    if (jj["xcam"].contains("first_layer_inspector")) {
                                        xcam_first_layer_inspector = jj["xcam"]["first_layer_inspector"].get<bool>();
                                    }
                                }

                                if (xcam_buildplate_marker_hold_count > 0)
                                    xcam_buildplate_marker_hold_count--;
                                else {
                                    if (jj["xcam"].contains("buildplate_marker_detector")) {
                                        xcam_buildplate_marker_detector = jj["xcam"]["buildplate_marker_detector"].get<bool>();
                                        is_support_build_plate_marker_detect = true;
                                    } else {
                                        is_support_build_plate_marker_detect = false;
                                    }
                                }
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }
#pragma endregion

#pragma region hms
                    if (!key_field_only) {
                        // parse hms msg
                        try {
                            hms_list.clear();
                            if (jj.contains("hms")) {
                                if (jj["hms"].is_array()) {
                                    for (auto it = jj["hms"].begin(); it != jj["hms"].end(); it++) {
                                        HMSItem item;
                                        if ((*it).contains("attr") && (*it).contains("code")) {
                                            unsigned attr = (*it)["attr"].get<unsigned>();
                                            unsigned code = (*it)["code"].get<unsigned>();
                                            item.parse_hms_info(attr, code);
                                        }
                                        hms_list.push_back(item);
                                    }
                                }
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }
#pragma endregion


#pragma region push_ams
                    /* ams status */
                    try {
                        if (jj.contains("ams_status")) {
                            int ams_status = jj["ams_status"].get<int>();
                            this->_parse_ams_status(ams_status);
                        }
                        std::string str_j = jj.dump();
                        if (jj.contains("cali_version")) {
                            cali_version = jj["cali_version"].get<int>();
                        }
                        else {
                            cali_version = -1;
                        }
                        std::string str = jj.dump();
                    }
                    catch (...) {
                        ;
                    }
                    update_printer_preset_name();
                    update_filament_list();
                    if (jj.contains("ams")) {
                        if (jj["ams"].contains("ams")) {
                            long int last_ams_exist_bits = ams_exist_bits;
                            long int last_tray_exist_bits = tray_exist_bits;
                            long int last_is_bbl_bits = tray_is_bbl_bits;
                            long int last_read_done_bits = tray_read_done_bits;
                            long int last_ams_version = ams_version;
                            if (jj["ams"].contains("ams_exist_bits")) {
                                ams_exist_bits = stol(jj["ams"]["ams_exist_bits"].get<std::string>(), nullptr, 16);
                            }
                            if (jj["ams"].contains("tray_exist_bits")) {
                                tray_exist_bits = stol(jj["ams"]["tray_exist_bits"].get<std::string>(), nullptr, 16);
                            }
                            if (!key_field_only) {
                                if (jj["ams"].contains("tray_read_done_bits")) {
                                    tray_read_done_bits = stol(jj["ams"]["tray_read_done_bits"].get<std::string>(), nullptr, 16);
                                }
                                if (jj["ams"].contains("tray_reading_bits")) {
                                    tray_reading_bits = stol(jj["ams"]["tray_reading_bits"].get<std::string>(), nullptr, 16);
                                }
                                if (jj["ams"].contains("tray_is_bbl_bits")) {
                                    tray_is_bbl_bits = stol(jj["ams"]["tray_is_bbl_bits"].get<std::string>(), nullptr, 16);
                                }
                                if (jj["ams"].contains("version")) {
                                    if (jj["ams"]["version"].is_number())
                                        ams_version = jj["ams"]["version"].get<int>();
                                }
                                if (jj["ams"].contains("tray_now")) {
                                    this->_parse_tray_now(jj["ams"]["tray_now"].get<std::string>());
                                }
                                if (jj["ams"].contains("tray_tar")) {
                                    m_tray_tar = jj["ams"]["tray_tar"].get<std::string>();
                                }
                                if (jj["ams"].contains("ams_rfid_status"))
                                    ams_rfid_status = jj["ams"]["ams_rfid_status"].get<int>();

                                if (jj["ams"].contains("insert_flag") || jj["ams"].contains("power_on_flag")
                                    || jj["ams"].contains("calibrate_remain_flag")) {
                                    if (ams_user_setting_hold_count > 0) {
                                        ams_user_setting_hold_count--;
                                    } else {
                                        if (jj["ams"].contains("insert_flag")) {
                                            ams_insert_flag = jj["ams"]["insert_flag"].get<bool>();
                                        }
                                        if (jj["ams"].contains("power_on_flag")) {
                                            ams_power_on_flag = jj["ams"]["power_on_flag"].get<bool>();
                                        }
                                        if (jj["ams"].contains("calibrate_remain_flag")) {
                                            ams_calibrate_remain_flag = jj["ams"]["calibrate_remain_flag"].get<bool>();
                                        }
                                    }
                                }
                                if (ams_exist_bits != last_ams_exist_bits
                                    || tray_exist_bits != last_tray_exist_bits
                                    || tray_is_bbl_bits != last_is_bbl_bits
                                    || tray_read_done_bits != last_read_done_bits
                                    || ams_version != last_ams_version) {
                                    is_ams_need_update = true;
                                }
                                else {
                                    is_ams_need_update = false;
                                }

                                json j_ams = jj["ams"]["ams"];
                                std::set<std::string> ams_id_set;

                                for (auto it = amsList.begin(); it != amsList.end(); it++) {
                                    ams_id_set.insert(it->first);
                                }

                                for (auto it = j_ams.begin(); it != j_ams.end(); it++) {
                                    if (!it->contains("id")) continue;
                                    std::string ams_id = (*it)["id"].get<std::string>();

                                    int nozzle_id = MAIN_NOZZLE_ID; // Default nozzle id
                                    int type_id = 1;   // 0:dummy 1:ams 2:ams-lite 3:n3f 4:n3s

                                    /*ams info*/
                                    if (it->contains("info")) {
                                        std::string info = (*it)["info"].get<std::string>();
                                        type_id = get_flag_bits(info, 0, 4);
                                        nozzle_id = get_flag_bits(info, 8, 4);
                                    }

                                    /*AMS without initialization*/
                                    if (nozzle_id == 0xE) {
                                        continue;
                                    }

                                    ams_id_set.erase(ams_id);
                                    Ams* curr_ams = nullptr;
                                    auto ams_it = amsList.find(ams_id);
                                    if (ams_it == amsList.end()) {
                                        Ams* new_ams    = new Ams(ams_id, nozzle_id, type_id);
                                        amsList.insert(std::make_pair(ams_id, new_ams));
                                        // new ams added event
                                        curr_ams = new_ams;
                                    } else {
                                        curr_ams = ams_it->second;
                                    }
                                    if (!curr_ams) continue;

                                    /*set ams type flag*/
                                    curr_ams->type = type_id;


                                    /*set ams exist flag*/
                                    try {
                                        if (!ams_id.empty()) {
                                            int ams_id_int = atoi(ams_id.c_str());

                                            if (type_id < 4) {
                                                curr_ams->is_exists = (ams_exist_bits & (1 << ams_id_int)) != 0 ? true : false;
                                            } else {
                                                curr_ams->is_exists = get_flag_bits(ams_exist_bits, 4 + (ams_id_int - 128));
                                            }
                                        }
                                    } catch (...) {
                                        ;
                                    }

                                    if (it->contains("dry_time") && (*it)["dry_time"].is_number())
                                    {
                                        curr_ams->left_dry_time = (*it)["dry_time"].get<int>();
                                    }

                                    if (it->contains("humidity")) {
                                        std::string humidity = (*it)["humidity"].get<std::string>();

                                        try {
                                            curr_ams->humidity = atoi(humidity.c_str());
                                        }
                                        catch (...) {
                                            ;
                                        }
                                    }

                                    if (it->contains("humidity_raw"))
                                    {
                                        std::string humidity_raw = (*it)["humidity_raw"].get<std::string>();

                                        try {
                                            curr_ams->humidity_raw = atoi(humidity_raw.c_str());
                                        } catch (...) {
                                            ;
                                        }

                                        if (curr_ams->humidity_raw != -1)
                                        {
                                            if (curr_ams->humidity_raw < 20)
                                            {
                                                curr_ams->humidity = 5;
                                            }
                                            else if (curr_ams->humidity_raw < 40)
                                            {
                                                curr_ams->humidity = 4;
                                            }
                                            else if (curr_ams->humidity_raw < 60)
                                            {
                                                curr_ams->humidity = 3;
                                            }
                                            else if (curr_ams->humidity_raw < 80)
                                            {
                                                curr_ams->humidity = 2;
                                            }
                                            else
                                            {
                                                curr_ams->humidity = 1;
                                            }
                                        }
                                    }


                                    if (it->contains("temp"))
                                    {
                                        std::string temp = (*it)["temp"].get<std::string>();
                                        try
                                        {
                                            curr_ams->current_temperature = string_to_float(temp);
                                        }
                                        catch (...)
                                        {
                                            curr_ams->current_temperature = INVALID_AMS_TEMPERATURE;
                                        }
                                    }

                                    if (it->contains("tray")) {
                                        std::set<std::string> tray_id_set;
                                        for (auto it = curr_ams->trayList.begin(); it != curr_ams->trayList.end(); it++) {
                                            tray_id_set.insert(it->first);
                                        }
                                        for (auto tray_it = (*it)["tray"].begin(); tray_it != (*it)["tray"].end(); tray_it++) {
                                            if (!tray_it->contains("id")) continue;
                                            std::string tray_id = (*tray_it)["id"].get<std::string>();
                                            tray_id_set.erase(tray_id);
                                            // compare tray_list
                                            AmsTray* curr_tray = nullptr;
                                            auto tray_iter = curr_ams->trayList.find(tray_id);
                                            if (tray_iter == curr_ams->trayList.end()) {
                                                AmsTray* new_tray = new AmsTray(tray_id);
                                                curr_ams->trayList.insert(std::make_pair(tray_id, new_tray));
                                                curr_tray = new_tray;
                                            }
                                            else {
                                                curr_tray = tray_iter->second;
                                            }
                                            if (!curr_tray) continue;

                                            if (curr_tray->hold_count > 0) {
                                                curr_tray->hold_count--;
                                                continue;
                                            }

                                            curr_tray->id = (*tray_it)["id"].get<std::string>();
                                            if (tray_it->contains("tag_uid"))
                                                curr_tray->tag_uid          = (*tray_it)["tag_uid"].get<std::string>();
                                            else
                                                curr_tray->tag_uid = "0";
                                            if (tray_it->contains("tray_info_idx") && tray_it->contains("tray_type")) {
                                                curr_tray->setting_id       = (*tray_it)["tray_info_idx"].get<std::string>();
                                                //std::string type            = (*tray_it)["tray_type"].get<std::string>();
                                                std::string type = setting_id_to_type(curr_tray->setting_id, (*tray_it)["tray_type"].get<std::string>());
                                                if (curr_tray->setting_id == "GFS00") {
                                                    curr_tray->type = "PLA-S";
                                                }
                                                else if (curr_tray->setting_id == "GFS01") {
                                                    curr_tray->type = "PA-S";
                                                } else {
                                                    curr_tray->type = type;
                                                }
                                            } else {
                                                curr_tray->setting_id = "";
                                                curr_tray->type       = "";
                                            }
                                            if (tray_it->contains("tray_sub_brands"))
                                                curr_tray->sub_brands       = (*tray_it)["tray_sub_brands"].get<std::string>();
                                            else
                                                curr_tray->sub_brands = "";
                                            if (tray_it->contains("tray_weight"))
                                                curr_tray->weight           = (*tray_it)["tray_weight"].get<std::string>();
                                            else
                                                curr_tray->weight = "";
                                            if (tray_it->contains("tray_diameter"))
                                                curr_tray->diameter         = (*tray_it)["tray_diameter"].get<std::string>();
                                            else
                                                curr_tray->diameter = "";
                                            if (tray_it->contains("tray_temp"))
                                                curr_tray->temp             = (*tray_it)["tray_temp"].get<std::string>();
                                            else
                                                curr_tray->temp = "";
                                            if (tray_it->contains("tray_time"))
                                                curr_tray->time             = (*tray_it)["tray_time"].get<std::string>();
                                            else
                                                curr_tray->time = "";
                                            if (tray_it->contains("bed_temp_type"))
                                                curr_tray->bed_temp_type    = (*tray_it)["bed_temp_type"].get<std::string>();
                                            else
                                                curr_tray->bed_temp_type = "";
                                            if (tray_it->contains("bed_temp"))
                                                curr_tray->bed_temp         = (*tray_it)["bed_temp"].get<std::string>();
                                            else
                                                curr_tray->bed_temp = "";
                                            if (tray_it->contains("tray_color")) {
                                                auto color = (*tray_it)["tray_color"].get<std::string>();
                                                curr_tray->update_color_from_str(color);
                                            } else {
                                                curr_tray->color = "";
                                            }
                                            if (tray_it->contains("nozzle_temp_max")) {
                                                curr_tray->nozzle_temp_max = (*tray_it)["nozzle_temp_max"].get<std::string>();
                                            }
                                            else
                                                curr_tray->nozzle_temp_max = "";
                                            if (tray_it->contains("nozzle_temp_min"))
                                                curr_tray->nozzle_temp_min = (*tray_it)["nozzle_temp_min"].get<std::string>();
                                            else
                                                curr_tray->nozzle_temp_min = "";
                                            if (tray_it->contains("xcam_info"))
                                                curr_tray->xcam_info = (*tray_it)["xcam_info"].get<std::string>();
                                            else
                                                curr_tray->xcam_info = "";
                                            if (tray_it->contains("tray_uuid"))
                                                curr_tray->uuid = (*tray_it)["tray_uuid"].get<std::string>();
                                            else
                                                curr_tray->uuid = "0";

                                            if (tray_it->contains("ctype"))
                                                curr_tray->ctype = (*tray_it)["ctype"].get<int>();
                                            else
                                                curr_tray->ctype = 0;
                                            curr_tray->cols.clear();
                                            if (tray_it->contains("cols")) {
                                                if ((*tray_it)["cols"].is_array()) {
                                                    for (auto it = (*tray_it)["cols"].begin(); it != (*tray_it)["cols"].end(); it++) {
                                                       curr_tray->cols.push_back(it.value().get<std::string>());
                                                    }
                                                }
                                            }

                                            if (tray_it->contains("remain")) {
                                                curr_tray->remain = (*tray_it)["remain"].get<int>();
                                            } else {
                                                curr_tray->remain = -1;
                                            }
                                            int ams_id_int = 0;
                                            int tray_id_int = 0;
                                            try {
                                                if (!ams_id.empty() && !curr_tray->id.empty()) {
                                                    ams_id_int = atoi(ams_id.c_str());
                                                    tray_id_int = atoi(curr_tray->id.c_str());

                                                    if (type_id < 4) {
                                                        curr_tray->is_exists = (tray_exist_bits & (1 << (ams_id_int * 4 + tray_id_int))) != 0 ? true : false;
                                                    }
                                                    else {
                                                        curr_tray->is_exists = get_flag_bits(tray_exist_bits, 16 + (ams_id_int - 128));
                                                    }

                                                }
                                            }
                                            catch (...) {
                                            }
                                            if (tray_it->contains("setting_id")) {
                                                curr_tray->filament_setting_id = (*tray_it)["setting_id"].get<std::string>();
                                            }
                                            auto curr_time = std::chrono::system_clock::now();
                                            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - extrusion_cali_set_hold_start);
                                            if (diff.count() > HOLD_TIMEOUT || diff.count() < 0
                                                || ams_id_int != (extrusion_cali_set_tray_id / 4)
                                                || tray_id_int != (extrusion_cali_set_tray_id % 4)) {
                                                if (tray_it->contains("k")) {
                                                    curr_tray->k = (*tray_it)["k"].get<float>();
                                                }
                                                if (tray_it->contains("n")) {
                                                    curr_tray->n = (*tray_it)["n"].get<float>();
                                                }
                                            }

                                            std::string temp = tray_it->dump();

                                            if (tray_it->contains("cali_idx")) {
                                                curr_tray->cali_idx = (*tray_it)["cali_idx"].get<int>();
                                            }
                                        }
                                        // remove not in trayList
                                        for (auto tray_it = tray_id_set.begin(); tray_it != tray_id_set.end(); tray_it++) {
                                            std::string tray_id = *tray_it;
                                            auto tray = curr_ams->trayList.find(tray_id);
                                            if (tray != curr_ams->trayList.end()) {
                                                curr_ams->trayList.erase(tray_id);
                                                BOOST_LOG_TRIVIAL(trace) << "parse_json: remove ams_id=" << ams_id << ", tray_id=" << tray_id;
                                            }
                                        }
                                    }
                                }
                                // remove not in amsList
                                for (auto it = ams_id_set.begin(); it != ams_id_set.end(); it++) {
                                    std::string ams_id = *it;
                                    auto ams = amsList.find(ams_id);
                                    if (ams != amsList.end()) {
                                        BOOST_LOG_TRIVIAL(trace) << "parse_json: remove ams_id=" << ams_id;
                                        amsList.erase(ams_id);
                                    }
                                }
                            }
                        }
                    }

                    /* vitrual tray*/
                    if (!key_field_only) {
                        try {
                            if (jj.contains("vt_tray")) {
                                auto main_slot = parse_vt_tray(jj["vt_tray"].get<json>());
                                main_slot.id = std::to_string(VIRTUAL_TRAY_ID);

                                is_ams_need_update |= vt_tray != main_slot;
                                vt_tray = main_slot;
                            }
                            else {
                                ams_support_virtual_tray = false;
                                is_support_extrusion_cali = false;
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }

                    /*parse new print data*/
                    try {
                        parse_new_info(jj);
                    } catch (...) {}
#pragma endregion
                } else if (jj["command"].get<std::string>() == "gcode_line") {
                    //ack of gcode_line
                    BOOST_LOG_TRIVIAL(debug) << "parse_json, ack of gcode_line = " << j.dump(4);
                } else if (jj["command"].get<std::string>() == "project_prepare") {
                    //ack of project file
                    BOOST_LOG_TRIVIAL(info) << "parse_json, ack of project_prepare = " << j.dump(4);
                    if (m_agent) {
                        if (jj.contains("job_id")) {
                            this->job_id_ = jj["job_id"].get<std::string>();
                        }
                    }

                } else if (jj["command"].get<std::string>() == "project_file") {
                    //ack of project file
                    BOOST_LOG_TRIVIAL(debug) << "parse_json, ack of project_file = " << j.dump(4);
                    std::string result;
                    if (jj.contains("result")) {
                        result = jj["result"].get<std::string>();
                        if (result == "FAIL") {
                            wxString text = _L("Failed to start print job");
                            GUI::wxGetApp().push_notification(text);
                        }
                    }
                } else if (jj["command"].get<std::string>() == "ams_filament_setting" && !key_field_only) {
                    if (jj.contains("result") && jj.contains("reason")) {
                        if (jj["result"].get<std::string>() == "fail") {
                            if (jj.contains("err_code")) {
                                auto err_code = jj["err_code"].get<int>();
                                print_error   = err_code;
                            }
                        }
                    }


                    // BBS trigger ams UI update
                    ams_version = -1;

                    if (jj["ams_id"].is_number()) {
                        int ams_id = jj["ams_id"].get<int>();
                        int tray_id = 0;
                        if (jj.contains("tray_id")) {
                            tray_id = jj["tray_id"].get<int>();
                        }
                        if (ams_id == 255 && tray_id == VIRTUAL_TRAY_ID) {
                            BOOST_LOG_TRIVIAL(info) << "ams_filament_setting, parse tray info";
                            vt_tray.nozzle_temp_max = std::to_string(jj["nozzle_temp_max"].get<int>());
                            vt_tray.nozzle_temp_min = std::to_string(jj["nozzle_temp_min"].get<int>());
                            vt_tray.color = jj["tray_color"].get<std::string>();
                            vt_tray.setting_id = jj["tray_info_idx"].get<std::string>();
                            //vt_tray.type = jj["tray_type"].get<std::string>();
                            vt_tray.type = setting_id_to_type(vt_tray.setting_id, jj["tray_type"].get<std::string>());
                            // delay update
                            vt_tray.set_hold_count();
                        } else {
                            auto ams_it = amsList.find(std::to_string(ams_id));
                            if (ams_it != amsList.end()) {
                                tray_id = jj["tray_id"].get<int>();
                                auto tray_it = ams_it->second->trayList.find(std::to_string(tray_id));
                                if (tray_it != ams_it->second->trayList.end()) {
                                    BOOST_LOG_TRIVIAL(trace) << "ams_filament_setting, parse tray info";
                                    tray_it->second->nozzle_temp_max = std::to_string(jj["nozzle_temp_max"].get<int>());
                                    tray_it->second->nozzle_temp_min = std::to_string(jj["nozzle_temp_min"].get<int>());
                                    //tray_it->second->type = jj["tray_type"].get<std::string>();
                                    tray_it->second->color = jj["tray_color"].get<std::string>();

                                    /*tray_it->second->cols.clear();
                                    if (jj.contains("cols")) {
                                        if (jj["cols"].is_array()) {
                                            for (auto it = jj["cols"].begin(); it != jj["cols"].end(); it++) {
                                                tray_it->second->cols.push_back(it.value().get<std::string>());
                                            }
                                        }
                                    }*/

                                    tray_it->second->setting_id = jj["tray_info_idx"].get<std::string>();
                                    tray_it->second->type = setting_id_to_type(tray_it->second->setting_id, jj["tray_type"].get<std::string>());
                                    // delay update
                                    tray_it->second->set_hold_count();
                                } else {
                                    BOOST_LOG_TRIVIAL(warning) << "ams_filament_setting, can not find in trayList, tray_id=" << tray_id;
                                }
                            } else {
                                BOOST_LOG_TRIVIAL(warning) << "ams_filament_setting, can not find in amsList, ams_id=" << ams_id;
                            }
                        }
                    }
                } else if (jj["command"].get<std::string>() == "xcam_control_set" && !key_field_only) {
                    if (jj.contains("module_name")) {
                        if (jj.contains("enable") || jj.contains("control")) {
                            bool enable = false;
                            if (jj.contains("enable"))
                                enable = jj["enable"].get<bool>();
                            else if (jj.contains("control"))
                                enable = jj["control"].get<bool>();
                            else {
                                ;
                            }

                            if (jj["module_name"].get<std::string>() == "first_layer_inspector") {
                                xcam_first_layer_inspector = enable;
                                xcam_first_layer_hold_count = HOLD_COUNT_MAX;
                            }
                            else if (jj["module_name"].get<std::string>() == "buildplate_marker_detector") {
                                xcam_buildplate_marker_detector = enable;
                                xcam_buildplate_marker_hold_count = HOLD_COUNT_MAX;
                            }
                            else if (jj["module_name"].get<std::string>() == "printing_monitor") {
                                xcam_ai_monitoring = enable;
                                xcam_ai_monitoring_hold_count = HOLD_COUNT_MAX;
                                if (jj.contains("halt_print_sensitivity")) {
                                    xcam_ai_monitoring_sensitivity = jj["halt_print_sensitivity"].get<std::string>();
                                }
                            }
                            else if (jj["module_name"].get<std::string>() == "spaghetti_detector") {
                                // old protocol
                                xcam_ai_monitoring = enable;
                                xcam_ai_monitoring_hold_count = HOLD_COUNT_MAX;
                                if (jj.contains("print_halt")) {
                                    if (jj["print_halt"].get<bool>())
                                        xcam_ai_monitoring_sensitivity = "medium";
                                }
                            }
                        }
                    }
                } else if(jj["command"].get<std::string>() == "print_option") {
                     try {
                          if (jj.contains("option")) {
                              if (jj["option"].is_number()) {
                                  int option = jj["option"].get<int>();
                                  _parse_print_option_ack(option);
                              }
                          }
                          if (jj.contains("auto_recovery")) {
                              xcam_auto_recovery_step_loss = jj["auto_recovery"].get<bool>();
                          }
                     }
                     catch(...) {
                     }
                } else if (jj["command"].get<std::string>() == "extrusion_cali" || jj["command"].get<std::string>() == "flowrate_cali") {
                    if (jj.contains("result")) {
                        if (jj["result"].get<std::string>() == "success") {
                            ;
                        }
                        else if (jj["result"].get<std::string>() == "fail") {
                            std::string cali_mode = jj["command"].get<std::string>();
                            std::string reason = jj["reason"].get<std::string>();
                            wxString info = "";
                            if (reason == "invalid nozzle_diameter" || reason == "nozzle_diameter is not supported") {
                                info = _L("This calibration does not support the currently selected nozzle diameter");
                            }
                            else if (reason == "invalid handle_flowrate_cali param") {
                                info = _L("Current flowrate cali param is invalid");
                            }
                            else if (reason == "nozzle_diameter is not matched") {
                                info = _L("Selected diameter and machine diameter do not match");
                            }
                            else if (reason == "generate auto filament cali gcode failure") {
                                info = _L("Failed to generate cali G-code");
                            }
                            else {
                                info = reason;
                            }
                            GUI::wxGetApp().push_notification(info, _L("Calibration error"), UserNotificationStyle::UNS_WARNING_CONFIRM);
                            BOOST_LOG_TRIVIAL(info) << cali_mode << " result fail, reason = " << reason;
                        }
                    }
                } else if (jj["command"].get<std::string>() == "extrusion_cali_set") {
                    if (jj.contains("result") && jj.contains("reason")) {
                        if (jj["result"].get<std::string>() == "fail") {
                            if (jj.contains("err_code")) {
                                auto err_code = jj["err_code"].get<int>();
                                print_error   = err_code;
                            }
                        }
                    }
#ifdef CALI_DEBUG
                    std::string str = jj.dump();
                    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_set: " << str;
#endif
                    int ams_id = -1;
                    int tray_id = -1;
                    int curr_tray_id = -1;
                    if (jj.contains("tray_id")) {
                        try {
                            curr_tray_id = jj["tray_id"].get<int>();
                            if (curr_tray_id == VIRTUAL_TRAY_ID)
                                tray_id = curr_tray_id;
                            else if (curr_tray_id >= 0 && curr_tray_id < 16){
                                ams_id = curr_tray_id / 4;
                                tray_id = curr_tray_id % 4;
                            } else {
                                BOOST_LOG_TRIVIAL(trace) << "extrusion_cali_set: unsupported tray_id = " << curr_tray_id;
                            }
                        }
                        catch(...) {
                            ;
                        }
                    }
                    if (tray_id == VIRTUAL_TRAY_ID) {
                        if (jj.contains("k_value"))
                            vt_tray.k = jj["k_value"].get<float>();
                        if (jj.contains("n_coef"))
                            vt_tray.n = jj["n_coef"].get<float>();
                    } else {
                        auto ams_item = this->amsList.find(std::to_string(ams_id));
                        if (ams_item != this->amsList.end()) {
                            auto tray_item = ams_item->second->trayList.find(std::to_string(tray_id));
                            if (tray_item != ams_item->second->trayList.end()) {
                                if (jj.contains("k_value"))
                                    tray_item->second->k = jj["k_value"].get<float>();
                                if (jj.contains("n_coef"))
                                    tray_item->second->n = jj["n_coef"].get<float>();
                            }
                        }
                    }
                    extrusion_cali_set_tray_id = curr_tray_id;
                    extrusion_cali_set_hold_start = std::chrono::system_clock::now();
                }
                else if (jj["command"].get<std::string>() == "extrusion_cali_sel") {
                    if (jj.contains("result") && jj.contains("reason")) {
                        if (jj["result"].get<std::string>() == "fail") {
                            if (jj.contains("err_code")) {
                                auto err_code = jj["err_code"].get<int>();
                                print_error   = err_code;
                            }
                        }
                    }

#ifdef CALI_DEBUG
                    std::string str = jj.dump();
                    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_sel: " << str;
#endif
                    int ams_id       = -1;
                    int tray_id      = -1;
                    int curr_tray_id = -1;
                    if (jj.contains("tray_id")) {
                        try {
                            curr_tray_id = jj["tray_id"].get<int>();
                            if (curr_tray_id == VIRTUAL_TRAY_ID)
                                tray_id = curr_tray_id;
                            else if (curr_tray_id >= 0 && curr_tray_id < 16) {
                                ams_id  = curr_tray_id / 4;
                                tray_id = curr_tray_id % 4;
                            } else {
                                BOOST_LOG_TRIVIAL(trace) << "extrusion_cali_sel: unsupported tray_id = " << curr_tray_id;
                            }
                        } catch (...) {
                            ;
                        }
                    }
                    if (tray_id == VIRTUAL_TRAY_ID) {
                        if (jj.contains("cali_idx")) {
                            vt_tray.cali_idx = jj["cali_idx"].get<int>();
                            vt_tray.set_hold_count();
                        }
                    } else {
                        auto ams_item = this->amsList.find(std::to_string(ams_id));
                        if (ams_item != this->amsList.end()) {
                            auto tray_item = ams_item->second->trayList.find(std::to_string(tray_id));
                            if (tray_item != ams_item->second->trayList.end()) {
                                if (jj.contains("cali_idx")) {
                                    tray_item->second->cali_idx = jj["cali_idx"].get<int>();
                                    tray_item->second->set_hold_count();
                                }
                            }
                        }
                    }
                }
                else if (jj["command"].get<std::string>() == "extrusion_cali_get") {
                    std::string str = jj.dump();
                    if (request_tab_from_bbs) {
                        BOOST_LOG_TRIVIAL(info) << "bbs extrusion_cali_get: " << str;
                        request_tab_from_bbs = false;
                        reset_pa_cali_history_result();
                        bool is_succeed = true;
                        if (jj.contains("result") && jj.contains("reason")) {
                            if (jj["result"].get<std::string>() == "fail") {
                                if (jj.contains("err_code")) {
                                    auto err_code = jj["err_code"].get<int>();
                                    print_error   = err_code;
                                }
                                is_succeed = false;
                            }
                        }

                        if (is_succeed) {
                            last_cali_version = cali_version;
                            has_get_pa_calib_tab = true;
                        }

                        if (jj.contains("filaments") && jj["filaments"].is_array()) {
                            try {
                                for (auto it = jj["filaments"].begin(); it != jj["filaments"].end(); it++) {
                                    PACalibResult pa_calib_result;
                                    pa_calib_result.filament_id = (*it)["filament_id"].get<std::string>();
                                    pa_calib_result.name        = (*it)["name"].get<std::string>();
                                    pa_calib_result.cali_idx    = (*it)["cali_idx"].get<int>();

                                    if ((*it).contains("setting_id")) {
                                        pa_calib_result.setting_id  = (*it)["setting_id"].get<std::string>();
                                    }

                                    if ((*it).contains("extruder_id")) {
                                        pa_calib_result.extruder_id = (*it)["extruder_id"].get<int>();
                                    }

                                    if ((*it).contains("nozzle_id")) {
                                        pa_calib_result.nozzle_volume_type = convert_to_nozzle_type((*it)["nozzle_id"].get<std::string>());
                                    }

                                    if (jj["nozzle_diameter"].is_number_float()) {
                                        pa_calib_result.nozzle_diameter = jj["nozzle_diameter"].get<float>();
                                    } else if (jj["nozzle_diameter"].is_string()) {
                                        pa_calib_result.nozzle_diameter = string_to_float(jj["nozzle_diameter"].get<std::string>());
                                    }

                                    if ((*it)["k_value"].is_number_float())
                                        pa_calib_result.k_value = (*it)["k_value"].get<float>();
                                    else if ((*it)["k_value"].is_string())
                                        pa_calib_result.k_value = string_to_float((*it)["k_value"].get<std::string>());

                                    if ((*it)["n_coef"].is_number_float())
                                        pa_calib_result.n_coef = (*it)["n_coef"].get<float>();
                                    else if ((*it)["n_coef"].is_string())
                                        pa_calib_result.n_coef = string_to_float((*it)["n_coef"].get<std::string>());

                                    if (check_pa_result_validation(pa_calib_result))
                                        pa_calib_tab.push_back(pa_calib_result);
                                    else {
                                        BOOST_LOG_TRIVIAL(info) << "pa result is invalid";
                                    }
                                }

                            }
                            catch (...) {

                            }
                        }
                        // notify cali history to update
                    } else {
                        BOOST_LOG_TRIVIAL(info) << "printer extrusion_cali_get: " << str;
                    }
                }
                else if (jj["command"].get<std::string>() == "extrusion_cali_get_result") {
                    std::string str = jj.dump();
                    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_get_result: " << str;
                    reset_pa_cali_result();
                    bool is_succeed = true;
                    if (jj.contains("result") && jj.contains("reason")) {
                        if (jj["result"].get<std::string>() == "fail") {
                            if (jj.contains("err_code")) {
                                auto err_code = jj["err_code"].get<int>();
                                print_error   = err_code;
                                is_succeed    = false;
                            }
                        }
                    }

                    if (is_succeed)
                        get_pa_calib_result = true;

                    if (jj.contains("filaments") && jj["filaments"].is_array()) {
                        try {
                            for (auto it = jj["filaments"].begin(); it != jj["filaments"].end(); it++) {
                                PACalibResult pa_calib_result;
                                pa_calib_result.tray_id     = (*it)["tray_id"].get<int>();
                                pa_calib_result.filament_id = (*it)["filament_id"].get<std::string>();

                                if ((*it).contains("setting_id")) {
                                    pa_calib_result.setting_id  = (*it)["setting_id"].get<std::string>();
                                }

                                if (jj["nozzle_diameter"].is_number_float()) {
                                    pa_calib_result.nozzle_diameter = jj["nozzle_diameter"].get<float>();
                                } else if (jj["nozzle_diameter"].is_string()) {
                                    pa_calib_result.nozzle_diameter = string_to_float(jj["nozzle_diameter"].get<std::string>());
                                }

                                if (it->contains("ams_id")) {
                                    pa_calib_result.ams_id = (*it)["ams_id"].get<int>();
                                } else {
                                    pa_calib_result.ams_id = 0;
                                }

                                if (it->contains("slot_id")) {
                                    pa_calib_result.slot_id = (*it)["slot_id"].get<int>();
                                } else {
                                    pa_calib_result.slot_id = 0;
                                }

                                if (it->contains("extruder_id")) {
                                    pa_calib_result.extruder_id = (*it)["extruder_id"].get<int>();
                                } else {
                                    pa_calib_result.extruder_id = -1;
                                }

                                if (it->contains("nozzle_id")) {
                                    pa_calib_result.nozzle_volume_type = convert_to_nozzle_type((*it)["nozzle_id"].get<std::string>());
                                } else {
                                    pa_calib_result.nozzle_volume_type = NozzleVolumeType::nvtNormal;
                                }

                                if ((*it)["k_value"].is_number_float())
                                    pa_calib_result.k_value = (*it)["k_value"].get<float>();
                                else if ((*it)["k_value"].is_string())
                                    pa_calib_result.k_value = string_to_float((*it)["k_value"].get<std::string>());

                                if ((*it)["n_coef"].is_number_float())
                                    pa_calib_result.n_coef = (*it)["n_coef"].get<float>();
                                else if ((*it)["n_coef"].is_string())
                                    pa_calib_result.n_coef = string_to_float((*it)["n_coef"].get<std::string>());

                                if (it->contains("confidence")) {
                                    pa_calib_result.confidence = (*it)["confidence"].get<int>();
                                } else {
                                    pa_calib_result.confidence = 0;
                                }

                                if (check_pa_result_validation(pa_calib_result))
                                    pa_calib_results.push_back(pa_calib_result);
                                else {
                                    BOOST_LOG_TRIVIAL(info) << "pa result is invalid";
                                }
                            }
                        } catch (...) {}
                    }

                    if (pa_calib_results.empty()) {
                        BOOST_LOG_TRIVIAL(info) << "no pa calib result";
                    }
                }
                else if (jj["command"].get<std::string>() == "flowrate_get_result" && !key_field_only) {
                    this->reset_flow_rate_cali_result();

                    get_flow_calib_result = true;
                    if (jj.contains("filaments") && jj["filaments"].is_array()) {
                        try {
#ifdef CALI_DEBUG
                            std::string str = jj.dump();
                            BOOST_LOG_TRIVIAL(info) << "flowrate_get_result: " << str;
#endif
                            for (auto it = jj["filaments"].begin(); it != jj["filaments"].end(); it++) {
                                FlowRatioCalibResult flow_ratio_calib_result;
                                flow_ratio_calib_result.tray_id     = (*it)["tray_id"].get<int>();
                                flow_ratio_calib_result.filament_id = (*it)["filament_id"].get<std::string>();
                                flow_ratio_calib_result.setting_id  = (*it)["setting_id"].get<std::string>();
                                flow_ratio_calib_result.nozzle_diameter = string_to_float(jj["nozzle_diameter"].get<std::string>());
                                flow_ratio_calib_result.flow_ratio      = string_to_float((*it)["flow_ratio"].get<std::string>());
                                if (it->contains("confidence")) {
                                    flow_ratio_calib_result.confidence = (*it)["confidence"].get<int>();
                                } else {
                                    flow_ratio_calib_result.confidence = 0;
                                }

                                flow_ratio_results.push_back(flow_ratio_calib_result);
                            }

                        } catch (...) {}
                    }
                }
            }
        }

        if (!key_field_only) {
            try {
                if (j.contains("camera")) {
                    if (j["camera"].contains("command")) {
                        if (j["camera"]["command"].get<std::string>() == "ipcam_timelapse") {
                            if (j["camera"]["control"].get<std::string>() == "enable")
                                this->camera_timelapse = true;
                            if (j["camera"]["control"].get<std::string>() == "disable")
                                this->camera_timelapse = false;
                            BOOST_LOG_TRIVIAL(info) << "ack of timelapse = " << camera_timelapse;
                        } else if (j["camera"]["command"].get<std::string>() == "ipcam_record_set") {
                            if (j["camera"]["control"].get<std::string>() == "enable")
                                this->camera_recording_when_printing = true;
                            if (j["camera"]["control"].get<std::string>() == "disable")
                                this->camera_recording_when_printing = false;
                            BOOST_LOG_TRIVIAL(info) << "ack of ipcam_record_set " << camera_recording_when_printing;
                        } else if (j["camera"]["command"].get<std::string>() == "ipcam_resolution_set") {
                            this->camera_resolution = j["camera"]["resolution"].get<std::string>();
                            BOOST_LOG_TRIVIAL(info) << "ack of resolution = " << camera_resolution;
                        }
                    }
                }
            } catch (...) {}
        }

        if (!key_field_only) {
            // upgrade
            try {
                if (j.contains("upgrade")) {
                    if (j["upgrade"].contains("command")) {
                        if (j["upgrade"]["command"].get<std::string>() == "upgrade_confirm") {
                            this->upgrade_display_state = UpgradingInProgress;
                            upgrade_display_hold_count = HOLD_COUNT_MAX;
                            BOOST_LOG_TRIVIAL(info) << "ack of upgrade_confirm";
                        }
                    }
                }
            }
            catch (...) {
                ;
            }
        }

        // event info
        try {
            if (j.contains("event")) {
                if (j["event"].contains("event")) {
                    if (j["event"]["event"].get<std::string>() == "client.disconnected")
                        set_online_state(false);
                    else if (j["event"]["event"].get<std::string>() == "client.connected")
                        set_online_state(true);
                }
            }
        }
        catch (...)  {}

	if (!key_field_only) {
            if (m_active_state == Active && !module_vers.empty() && check_version_valid()
                && !is_camera_busy_off()) {
                m_active_state = UpdateToDate;
                parse_version_func();
                if (is_support_tunnel_mqtt && connection_type() != "lan") {
                    m_agent->start_subscribe("tunnel");
               }
                parse_state_changed_event();
            }
	}
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(trace) << "parse_json failed! dev_id=" << this->dev_id <<", payload = " << payload;
    }

    std::chrono::system_clock::time_point clock_stop = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(clock_stop - clock_start);
    if (diff.count() > 10.0f) {
        BOOST_LOG_TRIVIAL(trace) << "parse_json timeout = " << diff.count();
    }
    DeviceManager::update_local_machine(*this);
    return 0;
}

void MachineObject::set_ctt_dlg( wxString text){
    if (!m_set_ctt_dlg) {
        m_set_ctt_dlg = true;
        auto print_error_dlg = new GUI::SecondaryCheckDialog(nullptr, wxID_ANY, _L("Warning"), GUI::SecondaryCheckDialog::ButtonStyle::ONLY_CONFIRM);
        print_error_dlg->update_text(text);
        print_error_dlg->Bind(wxEVT_SHOW, [this](auto& e) {
            if (!e.IsShown()) {
                m_set_ctt_dlg = false;
            }
            });
        print_error_dlg->Bind(wxEVT_CLOSE_WINDOW, [this](auto& e) {
            e.Skip();
            m_set_ctt_dlg = false;
            });
        print_error_dlg->on_show();

    }
}

int MachineObject::publish_gcode(std::string gcode_str)
{
    json j;
    j["print"]["command"] = "gcode_line";
    j["print"]["param"] = gcode_str;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return publish_json(j.dump());
}

BBLSubTask* MachineObject::get_subtask()
{
    if (!subtask_)
        subtask_ = new BBLSubTask(nullptr);
    return subtask_;
}

BBLModelTask* MachineObject::get_modeltask()
{
    return model_task;
}

void MachineObject::set_modeltask(BBLModelTask* task)
{
    model_task = task;
}

void MachineObject::update_model_task()
{
    if (request_model_result > 10) return;
    if (!m_agent) return;
    if (!model_task) return;
    if (!subtask_) return;
    if (model_task->task_id != subtask_->task_id) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " times: " << request_model_result << " model_task_id !=subtask_id";
        return;
    }
    if (model_task->instance_id <= 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " times: " << request_model_result << " instance_id <= 0";
        return;
    }

    if ((!subtask_id_.empty() && last_subtask_id_ != subtask_id_) || get_model_mall_result_need_retry) {
        if (!subtask_id_.empty() && last_subtask_id_ != subtask_id_) {
            BOOST_LOG_TRIVIAL(info) << "update_model_task: last=" << last_subtask_id_ << ", curr=" << subtask_id_;
            last_subtask_id_     = subtask_id_;
            request_model_result = 0;
        }
        if (get_model_mall_result_need_retry) {
            BOOST_LOG_TRIVIAL(info) << "need retry";
            get_model_mall_result_need_retry = false;
        }
    } else {
        BOOST_LOG_TRIVIAL(info) << "subtask_id_ no change and do not need retry";
        return;
    }

    int curr_instance_id = model_task->instance_id;
    if (rating_info) {
        delete rating_info;
        rating_info = nullptr;
    }
    get_model_task_thread = new boost::thread([this, curr_instance_id, token = std::weak_ptr<int>(m_token)]{
        if (token.expired()) { return; }
        try {
            std::string  rating_result;
            unsigned int http_code = 404;
            std::string  http_error;
            int          res = -1;
            res = m_agent->get_model_mall_rating_result(curr_instance_id, rating_result, http_code, http_error);
            request_model_result++;
            BOOST_LOG_TRIVIAL(info) << "request times: " << request_model_result << " http code: " << http_code;
            auto rating_info = new RatingInfo();
            rating_info->http_code = http_code;
            if (0 == res && 200 == http_code) {
                try {
                    json rating_json = json::parse(rating_result);
                    if (rating_json.contains("id")) {
                        rating_info->rating_id = rating_json["id"].get<unsigned int>();
                        //rating id is necessary info, so rating id must have
                        request_model_result            = 0;
                        rating_info->request_successful = true;
                        BOOST_LOG_TRIVIAL(info) << "get rating id";
                    } else {
                        rating_info->request_successful = false;
                        BOOST_LOG_TRIVIAL(info) << "can not get rating id";
                        Slic3r::GUI::wxGetApp().CallAfter([this, token, rating_info]() {
                            if (!token.expired()) this->rating_info = rating_info;
                        });
                        return;
                    }
                    if (rating_json.contains("score")) {
                            rating_info->start_count = rating_json["score"].get<int>();
                        }
                    if (rating_json.contains("content"))
                        rating_info->content = rating_json["content"].get<std::string>();
                    if (rating_json.contains("successPrinted"))
                        rating_info->success_printed = rating_json["successPrinted"].get<bool>();
                    if (rating_json.contains("images")) {
                        rating_info->image_url_paths = rating_json["images"].get<std::vector<std::string>>();
                    }
                    Slic3r::GUI::wxGetApp().CallAfter([this, token, rating_info]() {
                        if (!token.expired()) this->rating_info = rating_info;
                    });
                } catch (...) {
                    BOOST_LOG_TRIVIAL(info) << "parse model mall result json failed";
                }
            }
            else {
                rating_info->request_successful = false;
                Slic3r::GUI::wxGetApp().CallAfter([this, token, rating_info]() {
                    if (!token.expired()) this->rating_info = rating_info;
                });
                BOOST_LOG_TRIVIAL(info) << "model mall result request failed, request time: " << request_model_result << " http_code: " << http_code
                                        << " error msg: " << http_error;
                return;
            }
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(info) << "get mall model rating id failed and hide scoring page";
        }
    });
}

void MachineObject::update_slice_info(std::string project_id, std::string profile_id, std::string subtask_id, int plate_idx)
{
    if (!m_agent) return;

    if (project_id_ != project_id || profile_id_ != profile_id || slice_info == nullptr || subtask_id_ != subtask_id) {
        project_id_ = project_id;
        profile_id_ = profile_id;
        subtask_id_ = subtask_id;

        if (project_id.empty()
            || profile_id.empty()
            || subtask_id.empty()) {
            return;
        }

        if (project_id.compare("0") == 0
            || profile_id.compare("0") == 0
            || subtask_id.compare("0") == 0) return;

        BOOST_LOG_TRIVIAL(trace) << "slice_info: start";
        slice_info = new BBLSliceInfo();
        get_slice_info_thread = new boost::thread([this, project_id, profile_id, subtask_id, plate_idx] {
            int plate_index = -1;

            if (!m_agent) return;

            if (plate_idx >= 0) {
                plate_index = plate_idx;
            }
            else {
                std::string subtask_json;
                unsigned http_code = 0;
                std::string http_body;
                if (m_agent->get_subtask_info(subtask_id, &subtask_json, &http_code, &http_body) == 0) {
                    try {
                        if (!subtask_json.empty()) {

                            json task_j = json::parse(subtask_json);
                            if (task_j.contains("content")) {
                                std::string content_str = task_j["content"].get<std::string>();
                                json content_j = json::parse(content_str);
                                plate_index = content_j["info"]["plate_idx"].get<int>();
                            }

                            if (task_j.contains("context") && task_j["context"].contains("plates")) {
                                for (int i = 0; i < task_j["context"]["plates"].size(); i++) {
                                    if (task_j["context"]["plates"][i].contains("index") && task_j["context"]["plates"][i]["index"].get<int>() == plate_index) {
                                        if (task_j["context"]["plates"][i].contains("thumbnail") && task_j["context"]["plates"][i]["thumbnail"].contains("url")) {
                                            slice_info->thumbnail_url = task_j["context"]["plates"][i]["thumbnail"]["url"].get<std::string>();
                                        }
                                        if (task_j["context"]["plates"][i].contains("prediction")) {
                                            slice_info->prediction = task_j["context"]["plates"][i]["prediction"].get<int>();
                                        }
                                        if (task_j["context"]["plates"][i].contains("weight")) {
                                            slice_info->weight = task_j["context"]["plates"][i]["weight"].get<float>();
                                        }
                                        if (!task_j["context"]["plates"][i]["filaments"].is_null()) {
                                            for (auto filament : task_j["context"]["plates"][i]["filaments"]) {
                                                FilamentInfo f;
                                                if(filament.contains("color")){
                                                    f.color = filament["color"].get<std::string>();
                                                }
                                                if (filament.contains("type")) {
                                                    f.type = filament["type"].get<std::string>();
                                                }
                                                if (filament.contains("used_g")) {
                                                    f.used_g = stof(filament["used_g"].get<std::string>());
                                                }
                                                if (filament.contains("used_m")) {
                                                    f.used_m = stof(filament["used_m"].get<std::string>());
                                                }
                                                slice_info->filaments_info.push_back(f);
                                            }
                                        }
                                        BOOST_LOG_TRIVIAL(trace) << "task_info: thumbnail url=" << slice_info->thumbnail_url;
                                    }
                                }
                            }
                            else {
                                BOOST_LOG_TRIVIAL(error) << "task_info: no context or plates";
                            }
                        }
                    }
                    catch (...) {
                    }
                }
                else {
                    BOOST_LOG_TRIVIAL(error) << "task_info: get subtask id failed!";
                }
            }

            this->m_plate_index = plate_index;
            });
    }
}

void MachineObject::get_firmware_info()
{
    m_firmware_valid = false;
    if (m_firmware_thread_started)
        return;

    boost::thread update_info_thread = Slic3r::create_thread(
        [&] {
            m_firmware_thread_started = true;
            int          result = 0;
            unsigned int http_code;
            std::string  http_body;
            if (!m_agent) return;
            result = m_agent->get_printer_firmware(dev_id, &http_code, &http_body);
            if (result < 0) {
                // get upgrade list failed
                return;
            }
            try {
                json j = json::parse(http_body);
                if (j.contains("devices") && !j["devices"].is_null()) {
                    firmware_list.clear();
                    for (json::iterator it = j["devices"].begin(); it != j["devices"].end(); it++) {
                        if ((*it)["dev_id"].get<std::string>() == this->dev_id) {
                            try {
                                json firmware = (*it)["firmware"];
                                for (json::iterator firmware_it = firmware.begin(); firmware_it != firmware.end(); firmware_it++) {
                                    FirmwareInfo item;
                                    item.version = (*firmware_it)["version"].get<std::string>();
                                    item.url = (*firmware_it)["url"].get<std::string>();
                                    if ((*firmware_it).contains("description"))
                                        item.description = (*firmware_it)["description"].get<std::string>();
                                    item.module_type = "ota";
                                    int name_start = item.url.find_last_of('/') + 1;
                                    if (name_start > 0) {
                                        item.name = item.url.substr(name_start, item.url.length() - name_start);
                                        firmware_list.push_back(item);
                                    }
                                    else {
                                        BOOST_LOG_TRIVIAL(trace) << "skip";
                                    }
                                }
                            }
                            catch (...) {}
                            try {
                                if ((*it).contains("ams")) {
                                    json ams_list = (*it)["ams"];
                                    if (ams_list.size() > 0) {
                                        auto ams_front = ams_list.front();
                                        json firmware_ams = (ams_front)["firmware"];
                                        for (json::iterator ams_it = firmware_ams.begin(); ams_it != firmware_ams.end(); ams_it++) {
                                            FirmwareInfo item;
                                            item.version = (*ams_it)["version"].get<std::string>();
                                            item.url = (*ams_it)["url"].get<std::string>();
                                            if ((*ams_it).contains("description"))
                                                item.description = (*ams_it)["description"].get<std::string>();
                                            item.module_type = "ams";
                                            int name_start = item.url.find_last_of('/') + 1;
                                            if (name_start > 0) {
                                                item.name = item.url.substr(name_start, item.url.length() - name_start);
                                                firmware_list.push_back(item);
                                            }
                                            else {
                                                BOOST_LOG_TRIVIAL(trace) << "skip";
                                            }
                                        }
                                    }
                                }
                            }
                            catch (...) {
                                ;
                            }
                        }
                    }
                }
            }
            catch (...) {
                return;
            }
            m_firmware_thread_started = false;
            m_firmware_valid = true;
        }
    );
    return;
}

bool MachineObject::is_firmware_info_valid()
{
    return m_firmware_valid;
}

std::string MachineObject::get_string_from_fantype(FanType type)
{
    switch (type) {
    case FanType::COOLING_FAN:
        return "cooling_fan";
    case FanType::BIG_COOLING_FAN:
        return "big_cooling_fan";
    case FanType::CHAMBER_FAN:
        return "chamber_fan";
    default:
        return "";
    }
    return "";
}

AmsTray MachineObject::parse_vt_tray(json vtray)
{
    auto vt_tray = AmsTray(std::to_string(VIRTUAL_TRAY_ID));

    if (vtray.contains("id"))
        vt_tray.id = vtray["id"].get<std::string>();
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - extrusion_cali_set_hold_start);
    if (diff.count() > HOLD_TIMEOUT || diff.count() < 0
        || extrusion_cali_set_tray_id != VIRTUAL_TRAY_ID) {
        if (vtray.contains("k"))
            vt_tray.k = vtray["k"].get<float>();
        if (vtray.contains("n"))
            vt_tray.n = vtray["n"].get<float>();
    }
    ams_support_virtual_tray = true;

    if (vt_tray.hold_count > 0) {
        vt_tray.hold_count--;
    }
    else {
        if (vtray.contains("tag_uid"))
            vt_tray.tag_uid = vtray["tag_uid"].get<std::string>();
        else
            vt_tray.tag_uid = "0";
        if (vtray.contains("tray_info_idx") && vtray.contains("tray_type")) {
            vt_tray.setting_id = vtray["tray_info_idx"].get<std::string>();
            //std::string type = vtray["tray_type"].get<std::string>();
            std::string type = setting_id_to_type(vt_tray.setting_id, vtray["tray_type"].get<std::string>());
            if (vt_tray.setting_id == "GFS00") {
                vt_tray.type = "PLA-S";
            }
            else if (vt_tray.setting_id == "GFS01") {
                vt_tray.type = "PA-S";
            }
            else {
                vt_tray.type = type;
            }
        }
        else {
            vt_tray.setting_id = "";
            vt_tray.type = "";
        }
        if (vtray.contains("tray_sub_brands"))
            vt_tray.sub_brands = vtray["tray_sub_brands"].get<std::string>();
        else
            vt_tray.sub_brands = "";
        if (vtray.contains("tray_weight"))
            vt_tray.weight = vtray["tray_weight"].get<std::string>();
        else
            vt_tray.weight = "";
        if (vtray.contains("tray_diameter"))
            vt_tray.diameter = vtray["tray_diameter"].get<std::string>();
        else
            vt_tray.diameter = "";
        if (vtray.contains("tray_temp"))
            vt_tray.temp = vtray["tray_temp"].get<std::string>();
        else
            vt_tray.temp = "";
        if (vtray.contains("tray_time"))
            vt_tray.time = vtray["tray_time"].get<std::string>();
        else
            vt_tray.time = "";
        if (vtray.contains("bed_temp_type"))
            vt_tray.bed_temp_type = vtray["bed_temp_type"].get<std::string>();
        else
            vt_tray.bed_temp_type = "";
        if (vtray.contains("bed_temp"))
            vt_tray.bed_temp = vtray["bed_temp"].get<std::string>();
        else
            vt_tray.bed_temp = "";
        if (vtray.contains("tray_color")) {
            auto color = vtray["tray_color"].get<std::string>();
            vt_tray.update_color_from_str(color);
        }
        else {
            vt_tray.color = "";
        }
        if (vtray.contains("nozzle_temp_max"))
            vt_tray.nozzle_temp_max = vtray["nozzle_temp_max"].get<std::string>();
        else
            vt_tray.nozzle_temp_max = "";
        if (vtray.contains("nozzle_temp_min"))
            vt_tray.nozzle_temp_min = vtray["nozzle_temp_min"].get<std::string>();
        else
            vt_tray.nozzle_temp_min = "";
        if (vtray.contains("xcam_info"))
            vt_tray.xcam_info = vtray["xcam_info"].get<std::string>();
        else
            vt_tray.xcam_info = "";
        if (vtray.contains("tray_uuid"))
            vt_tray.uuid = vtray["tray_uuid"].get<std::string>();
        else
            vt_tray.uuid = "0";

        if (vtray.contains("cali_idx"))
            vt_tray.cali_idx = vtray["cali_idx"].get<int>();
        else
            vt_tray.cali_idx = -1;
        vt_tray.cols.clear();
        if (vtray.contains("cols")) {
            if (vtray.is_array()) {
                for (auto it = vtray.begin(); it != vtray.end(); it++) {
                    vt_tray.cols.push_back(it.value().get<std::string>());
                }
            }
        }

        if (vtray.contains("remain")) {
            vt_tray.remain = vtray["remain"].get<int>();
        }
        else {
            vt_tray.remain = -1;
        }
    }

    return vt_tray;
}

bool MachineObject::check_enable_np(const json& print) const
{
    if (print.contains("cfg") && print.contains("fun") && print.contains("aux") && print.contains("stat"))
    {
        return true;
    }

    return false;
}

void MachineObject::parse_new_info(json print)
{
    is_enable_np = check_enable_np(print);
    if (!is_enable_np)
    {
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "using new print data for parsing";

    /*cfg*/
    std::string cfg = print["cfg"].get<std::string>();

    BOOST_LOG_TRIVIAL(info) << "new print data cfg = " << cfg;

    if(!cfg.empty()){
        if (ams_user_setting_hold_count > 0) ams_user_setting_hold_count--;
        if (camera_recording_hold_count > 0) camera_recording_hold_count--;
        if (camera_resolution_hold_count > 0) camera_resolution_hold_count--;
        if (camera_timelapse_hold_count > 0) camera_timelapse_hold_count--;
        //if (xcam_buildplate_marker_hold_count > 0) xcam_buildplate_marker_hold_count--;first_layer_inspector
        if (xcam_first_layer_hold_count > 0) xcam_first_layer_hold_count--;
        if (xcam_ai_monitoring_hold_count > 0) xcam_ai_monitoring_hold_count--;
        if (xcam_auto_recovery_hold_count > 0) xcam_auto_recovery_hold_count--;
        if (ams_print_option_count > 0)        ams_print_option_count--;
        if (xcam_prompt_sound_hold_count > 0) xcam_prompt_sound_hold_count--;
        if (xcam_filament_tangle_detect_count > 0)xcam_filament_tangle_detect_count--;
        if (nozzle_setting_hold_count > 0)nozzle_setting_hold_count--;



        ams_insert_flag = get_flag_bits(cfg, 0);
        ams_power_on_flag = get_flag_bits(cfg, 1);
        upgrade_force_upgrade = get_flag_bits(cfg, 2);
        camera_recording_when_printing = get_flag_bits(cfg, 3);
        camera_resolution = get_flag_bits(cfg, 4) == 0 ? "720p" : "1080p";
        camera_timelapse = get_flag_bits(cfg, 5);
        tutk_state = get_flag_bits(cfg, 6) == 1 ? "disable" : "";
        chamber_light = get_flag_bits(cfg, 7) == 1 ? LIGHT_EFFECT::LIGHT_EFFECT_ON : LIGHT_EFFECT::LIGHT_EFFECT_OFF;
        printing_speed_lvl = (PrintingSpeedLevel)get_flag_bits(cfg, 8, 3);
        //is_support_build_plate_marker_detect = get_flag_bits(cfg, 12); todo yangcong

        xcam_first_layer_inspector = get_flag_bits(cfg, 13);

        switch (get_flag_bits(cfg, 14, 2))
        {
        case 0:
            xcam_ai_monitoring_sensitivity = "never_halt";
            break;
        case 1:
            xcam_ai_monitoring_sensitivity = "low";
            break;
        case 2:
            xcam_ai_monitoring_sensitivity = "medium";
            break;
        case 3:
            xcam_ai_monitoring_sensitivity = "high";
            break;
        }

        xcam_ai_monitoring = get_flag_bits(cfg, 16);
        xcam_auto_recovery_step_loss = get_flag_bits(cfg, 17);
        ams_calibrate_remain_flag = get_flag_bits(cfg, 18);
        ams_auto_switch_filament_flag = get_flag_bits(cfg, 19);
        xcam_allow_prompt_sound = get_flag_bits(cfg, 23);
        xcam_filament_tangle_detect = get_flag_bits(cfg, 24);
        nozzle_blob_detection_enabled = get_flag_bits(cfg, 25);
        installed_upgrade_kit = get_flag_bits(cfg, 26);
    }

    /*fun*/
    std::string fun = print["fun"].get<std::string>();
    BOOST_LOG_TRIVIAL(info) << "new print data fun = " << fun;

    if (!fun.empty()) {

        is_support_agora    = get_flag_bits(fun, 1);
        if (is_support_agora) is_support_tunnel_mqtt = false;

        is_220V_voltage  = get_flag_bits(fun, 3) == 0?false:true;
        is_support_flow_calibration = get_flag_bits(fun, 6);
        is_support_pa_calibration = get_flag_bits(fun, 7);
        is_support_prompt_sound = get_flag_bits(fun, 8);
        is_support_filament_tangle_detect = get_flag_bits(fun, 9);
        is_support_motor_noise_cali = get_flag_bits(fun, 10);
        is_support_user_preset = get_flag_bits(fun, 11);
        is_support_nozzle_blob_detection = get_flag_bits(fun, 13);
        is_support_upgrade_kit = get_flag_bits(cfg, 14);
        is_support_command_homing = get_flag_bits(fun, 32);
        is_support_brtc = get_flag_bits(fun, 31);
    }

    /*aux*/
    std::string aux = print["aux"].get<std::string>();

    BOOST_LOG_TRIVIAL(info) << "new print data aux = " << aux;

    if (!aux.empty()) {
         sdcard_state = MachineObject::SdcardState(get_flag_bits(aux, 12, 2));
    }

    /*stat*/
    std::string stat = print["stat"].get<std::string>();

    BOOST_LOG_TRIVIAL(info) << "new print data stat = " << stat;

    if (!stat.empty()) {
        camera_recording = get_flag_bits(stat, 7);
    }

    /*device*/
    if (print.contains("device")) {
        json const& device = print["device"];

        if (device.contains("type")) {
            int type = device["type"]; //FDM:1<<0 Laser:1<< Cut:1<<2
        }

        if (device.contains("bed_temp")) {
            bed_temp        = get_flag_bits(device["bed_temp"].get<int>(), 0, 16);
            bed_temp_target = get_flag_bits(device["bed_temp"].get<int>(), 16, 16);
        }

        if (device.contains("cham_temp")) {
            chamber_temp = get_flag_bits(device["cham_temp"].get<int>(), 0, 16);
            chamber_temp_target = get_flag_bits(device["cham_temp"].get<int>(), 16, 16);
        }

        if (device.contains("fan")) {
            big_fan1_speed = get_flag_bits(device["fan"].get<int>(), 0, 3);
            big_fan2_speed = get_flag_bits(device["fan"].get<int>(), 4, 3);
            cooling_fan_speed = get_flag_bits(device["fan"].get<int>(), 8, 3);
            heatbreak_fan_speed = get_flag_bits(device["fan"].get<int>(), 12, 3);
        }

        if (device.contains("nozzle")) {
            json const &nozzle = device["nozzle"];

            m_nozzle_data = NozzleData();

            m_nozzle_data.extder_exist      = get_flag_bits(nozzle["exist"].get<int>(), 0, 16);
            m_nozzle_data.cut_exist         = get_flag_bits(nozzle["exist"].get<int>(), 16, 16);
            m_nozzle_data.state             = get_flag_bits(nozzle["state"].get<int>(), 0, 4);

            for (auto it = nozzle["info"].begin(); it != nozzle["info"].end(); it++) {
                Nozzle nozzle_obj;
                auto   njon = it.value();
                std::string type = njon["type"].get<std::string>();
                nozzle_obj.id           = njon["id"].get<int>();

                if (type.length() >= 4) {
                    if (type.substr(2, 2) == std::string("00")) {
                        nozzle_obj.nozzle_type = NozzleType::ntStainlessSteel;
                    } else if (type.substr(2, 2) == std::string("01")) {
                        nozzle_obj.nozzle_type = NozzleType::ntHardenedSteel;
                    }
                } else {
                    nozzle_obj.nozzle_type = NozzleType::ntUndefine;
                }

                nozzle_obj.diameter     = njon["diameter"].get<float>();
                nozzle_obj.max_temp     = njon["tm"].get<int>();
                nozzle_obj.wear         = njon["wear"].get<int>();
                if (nozzle_obj.diameter == 0.0f) {nozzle_obj.diameter = 0.4f;}
                m_nozzle_data.nozzles.push_back(nozzle_obj);
            }
        }

        if (device.contains("extruder")) {
            json const& extruder = device["extruder"];

            auto extder_data = ExtderData();
            extder_data.total_extder_count        = get_flag_bits(extruder["state"].get<int>(), 0, 4);


            extder_data.current_extder_id         = get_flag_bits(extruder["state"].get<int>(), 4, 4);
            extder_data.target_extder_id          = get_flag_bits(extruder["state"].get<int>(), 8, 4);

            for (auto it = extruder["info"].begin(); it != extruder["info"].end(); it++) {

                Extder extder_obj;
                auto   njon = it.value();

                extder_obj.id                  = njon["id"].get<int>();

                extder_obj.filam_bak.clear();
                is_support_show_filament_backup = njon.contains("filam_bak");
                if (is_support_show_filament_backup)
                {
                    const json& filam_bak_items = njon["filam_bak"];
                    for (const auto& filam_bak_item : filam_bak_items)
                    {
                        const auto& filam_bak_val = filam_bak_item.get<int>();
                        extder_obj.filam_bak.emplace_back(filam_bak_val);
                    }
                }

                extder_obj.ext_has_filament    = get_flag_bits(njon["info"].get<int>(), 1);
                extder_obj.buffer_has_filament = get_flag_bits(njon["info"].get<int>(), 2);
                extder_obj.nozzle_exist        = get_flag_bits(njon["info"].get<int>(), 3);
                extder_obj.temp                = get_flag_bits(njon["temp"].get<int>(), 0, 16);
                extder_obj.target_temp         = get_flag_bits(njon["temp"].get<int>(), 16, 16);

                AmsSlot spre;
                spre.slot_id  = std::to_string(get_flag_bits(njon["spre"].get<int>(), 0, 8));
                spre.ams_id = std::to_string(get_flag_bits(njon["spre"].get<int>(), 8, 8));

                AmsSlot snow;
                snow.slot_id = std::to_string(get_flag_bits(njon["snow"].get<int>(), 0, 8));
                snow.ams_id = std::to_string(get_flag_bits(njon["snow"].get<int>(), 8, 8));

                AmsSlot star;
                star.slot_id  = std::to_string(get_flag_bits(njon["star"].get<int>(), 0, 8));
                star.ams_id = std::to_string(get_flag_bits(njon["star"].get<int>(), 8, 8));

                extder_obj.nozzle_id = njon["hnow"].get<int>();
                extder_obj.target_nozzle_id =  njon["htar"].get<int>();

                extder_obj.spre      = spre;
                extder_obj.snow      = snow;
                extder_obj.star      = star;
                extder_obj.ams_stat  = get_flag_bits(njon["stat"].get<int>(), 0, 16);
                extder_obj.rfid_stat = get_flag_bits(njon["stat"].get<int>(), 16, 16);

                //current nozzle info
                if (extder_obj.nozzle_id == 0xff) {
                    extder_obj.current_nozzle_type = NozzleType::ntUndefine;
                    extder_obj.current_nozzle_diameter = 0.4f;
                } else {
                    for (auto i = 0; i < m_nozzle_data.nozzles.size(); i++) {
                        if (m_nozzle_data.nozzles[i].id == extder_obj.nozzle_id) {
                            extder_obj.current_nozzle_type      = m_nozzle_data.nozzles[i].nozzle_type;
                            extder_obj.current_nozzle_diameter  = m_nozzle_data.nozzles[i].diameter;
                        }
                    }
                }
                extder_data.extders.push_back(extder_obj);
            }

            if (extder_data.extders.size() <= 0) {
                // def data
                extder_data.current_extder_id  = 0;
                extder_data.target_extder_id   = 0;
                extder_data.total_extder_count = 1;
                Extder nozzle;
                extder_data.extders.push_back(nozzle);
            }

            m_extder_data = extder_data;
        }

        if (device.contains("ctc")) {
            json const& ctc = device["ctc"];
            int state = get_flag_bits(ctc["state"].get<int>(), 0, 4);

            if (ctc.contains("info")) {
                json const &info = ctc["info"];
                chamber_temp = get_flag_bits(info["temp"].get<int>(), 0, 16);
                chamber_temp_target  = get_flag_bits(info["temp"].get<int>(), 16, 16);
            }


        }
    }
}

bool MachineObject::is_nozzle_data_invalid()
{
    for (const auto &ext : m_extder_data.extders)
    {
        if (ext.current_nozzle_type == NozzleType::ntUndefine ||
            ext.current_nozzle_diameter <= 0.0f) {
            return true;
        }
    }

    return false;
}

int MachineObject::get_flag_bits(std::string str, int start, int count) const
{
    try {
        unsigned long long decimal_value = std::stoull(str, nullptr, 16);
        unsigned long long mask = (1ULL << count) - 1;
        int flag = (decimal_value >> start) & mask;
        return flag;
    } catch (...) {
        return 0;
    }
}

int MachineObject::get_flag_bits(int num, int start, int count, int base) const
{
    try {
        unsigned long long mask = (1ULL << count) - 1;
        unsigned long long value;
        if (base == 10) {
            value = static_cast<unsigned long long>(num);
        } else if (base == 16) {
            value = static_cast<unsigned long long>(std::stoul(std::to_string(num), nullptr, 16));
        } else {
            throw std::invalid_argument("Unsupported base");
        }

        int flag = (value >> start) & mask;
        return flag;
    } catch (...) {
        return 0;
    }
}

void MachineObject::update_filament_list()
{
    PresetBundle *preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;

    // custom filament
    typedef std::map<std::string, std::pair<int, int>> map_pair;
    std::map<std::string, map_pair>                    map_list;
    for (auto &pair : m_nozzle_filament_data) {
        map_list[pair.second.printer_preset_name] = map_pair{};
    }
    for (auto &preset : preset_bundle->filaments()) {
        if (preset.is_user() && preset.inherits() == "") {
            ConfigOption *       printer_opt  = const_cast<Preset &>(preset).config.option("compatible_printers");
            ConfigOptionStrings *printer_strs = dynamic_cast<ConfigOptionStrings *>(printer_opt);
            for (const std::string &printer_str : printer_strs->values) {
                if (map_list.find(printer_str) != map_list.end()) {
                    auto &        filament_list = map_list[printer_str];
                    ConfigOption *opt_min  = const_cast<Preset &>(preset).config.option("nozzle_temperature_range_low");
                    int           min_temp = -1;
                    if (opt_min) {
                        ConfigOptionInts *opt_min_ints = dynamic_cast<ConfigOptionInts *>(opt_min);
                        min_temp                       = opt_min_ints->get_at(0);
                    }
                    ConfigOption *opt_max  = const_cast<Preset &>(preset).config.option("nozzle_temperature_range_high");
                    int           max_temp = -1;
                    if (opt_max) {
                        ConfigOptionInts *opt_max_ints = dynamic_cast<ConfigOptionInts *>(opt_max);
                        max_temp                       = opt_max_ints->get_at(0);
                    }
                    filament_list[preset.filament_id] = std::make_pair(min_temp, max_temp);
                    break;
                }
            }
        }
    }

    for (auto& pair : m_nozzle_filament_data) {
        auto & m_printer_preset_name = pair.second.printer_preset_name;
        auto & m_filament_list       = pair.second.filament_list;
        auto & m_checked_filament    = pair.second.checked_filament;
        auto & filament_list         = map_list[m_printer_preset_name];

        for (auto it = filament_list.begin(); it != filament_list.end(); it++) {
            if (m_filament_list.find(it->first) != m_filament_list.end()) {
                assert(it->first.size() == 8 && it->first[0] == 'P');

                if (it->second.first != m_filament_list[it->first].first) {
                    BOOST_LOG_TRIVIAL(info) << "old min temp is not equal to new min temp and filament id: " << it->first;
                    continue;
                }

                if (it->second.second != m_filament_list[it->first].second) {
                    BOOST_LOG_TRIVIAL(info) << "old max temp is not equal to new max temp and filament id: " << it->first;
                    continue;
                }

                m_filament_list.erase(it->first);
            }
        }

        for (auto it = m_filament_list.begin(); it != m_filament_list.end(); it++) { m_checked_filament.erase(it->first); }

        m_filament_list = filament_list;
    }
}

void MachineObject::update_printer_preset_name()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "start update preset_name";
    PresetBundle *     preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) return;
    auto               printer_model = MachineObject::get_preset_printer_model_name(this->printer_type);
    std::set<std::string> diameter_set;
    for (auto &nozzle : m_extder_data.extders) {
        float diameter = nozzle.current_nozzle_diameter;
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1) << diameter;
        std::string nozzle_diameter_str = stream.str();
        diameter_set.insert(nozzle_diameter_str);
        if (m_nozzle_filament_data.find(nozzle_diameter_str) != m_nozzle_filament_data.end()) continue;
        auto data = FilamentData();
        auto printer_set = preset_bundle->get_printer_names_by_printer_type_and_nozzle(printer_model, nozzle_diameter_str);
        if (printer_set.size() > 0) {
            data.printer_preset_name = *printer_set.begin();
            m_nozzle_filament_data[nozzle_diameter_str] = data;
        }
        else
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " update printer preset name failed: "<< "printer_type: " << printer_type << "nozzle_diameter_str" << nozzle_diameter_str;
    }

    for (auto iter = m_nozzle_filament_data.begin(); iter != m_nozzle_filament_data.end();)
    {
        if (diameter_set.find(iter->first) == diameter_set.end())
        {
            iter = m_nozzle_filament_data.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void MachineObject::check_ams_filament_valid()
{
    PresetBundle * preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;
    auto printer_model = MachineObject::get_preset_printer_model_name(this->printer_type);
    std::map<std::string, std::set<std::string>> need_checked_filament_id;
    for (auto &ams_pair : amsList) {
        auto ams_id = ams_pair.first;
        auto &ams = ams_pair.second;
        std::ostringstream stream;
        if (ams->nozzle < 0 || ams->nozzle >= m_extder_data.extders.size()) {
            return;
        }
        stream << std::fixed << std::setprecision(1) << m_extder_data.extders[ams->nozzle].current_nozzle_diameter;
        std::string nozzle_diameter_str = stream.str();
        assert(nozzle_diameter_str.size() == 3);
        if (m_nozzle_filament_data.find(nozzle_diameter_str) == m_nozzle_filament_data.end()) {
            //assert(false);
            continue;
        }
        auto &data = m_nozzle_filament_data[nozzle_diameter_str];
        auto &filament_list = data.filament_list;
        auto &checked_filament = data.checked_filament;
        for (const auto &[slot_id, curr_tray] : ams->trayList) {

            if (curr_tray->setting_id.size() == 8 && curr_tray->setting_id[0] == 'P' && filament_list.find(curr_tray->setting_id) == filament_list.end()) {
                if (checked_filament.find(curr_tray->setting_id) == checked_filament.end()) {
                    need_checked_filament_id[nozzle_diameter_str].insert(curr_tray->setting_id);
                    wxColour color = *wxWHITE;
                    char     col_buf[10];
                    sprintf(col_buf, "%02X%02X%02XFF", (int) color.Red(), (int) color.Green(), (int) color.Blue());
                    try {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " ams settings_id is not exist in filament_list and reset, ams_id: " << ams_id << " tray_id"
                                                << slot_id << "filament_id: " << curr_tray->setting_id;

                        command_ams_filament_settings(std::stoi(ams_id), std::stoi(slot_id), "", "", std::string(col_buf), "", 0, 0);
                        continue;
                    } catch (...) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " stoi error and ams_id: " << ams_id << " tray_id" << slot_id;
                    }
                }
            }
            if (curr_tray->setting_id.size() == 8 && curr_tray->setting_id[0] == 'P' && curr_tray->nozzle_temp_min != "" && curr_tray->nozzle_temp_max != "") {
                if (checked_filament.find(curr_tray->setting_id) == checked_filament.end()) {
                    need_checked_filament_id[nozzle_diameter_str].insert(curr_tray->setting_id);
                    try {
                        std::string preset_setting_id;
                        bool        is_equation = preset_bundle->check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(printer_model, nozzle_diameter_str,
                                                                                                                               curr_tray->setting_id, curr_tray->tag_uid,
                                                                                                                               curr_tray->nozzle_temp_min,
                                                                                                                               curr_tray->nozzle_temp_max, preset_setting_id);
                        if (!is_equation) {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " ams filament is not match min max temp and reset, ams_id: " << ams_id << " tray_id"
                                                    << slot_id << "filament_id: " << curr_tray->setting_id;


                            command_ams_filament_settings(std::stoi(ams_id), std::stoi(slot_id), curr_tray->setting_id, preset_setting_id, curr_tray->color, curr_tray->type,
                                                          std::stoi(curr_tray->nozzle_temp_min), std::stoi(curr_tray->nozzle_temp_max));
                        }
                        continue;
                    } catch (...) {
                        BOOST_LOG_TRIVIAL(info) << "check fail and curr_tray ams_id" << ams_id << " curr_tray tray_id" << slot_id;
                    }
                }
            }
        }
    }

    /*for (auto vt_tray : vt_slot)*/ do{
        int vt_id = std::stoi(vt_tray.id);
        int index = 255 - vt_id;
        if (index >= m_extder_data.total_extder_count) {
            BOOST_LOG_TRIVIAL(error) << " vt_tray id map for nozzle id is not exist, index is: " << index << " nozzle count" << m_extder_data.total_extder_count;
            continue;
        }
        auto diameter = m_extder_data.extders[index].current_nozzle_diameter;
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1) << diameter;
        std::string nozzle_diameter_str = stream.str();
        if (m_nozzle_filament_data.find(nozzle_diameter_str) == m_nozzle_filament_data.end()) {
            continue;
        }
        auto &data = m_nozzle_filament_data[nozzle_diameter_str];
        auto &checked_filament = data.checked_filament;
        auto &filament_list    = data.filament_list;
        if (vt_tray.setting_id.size() == 8 && vt_tray.setting_id[0] == 'P' && filament_list.find(vt_tray.setting_id) == filament_list.end()) {
            if (checked_filament.find(vt_tray.setting_id) == checked_filament.end()) {
                need_checked_filament_id[nozzle_diameter_str].insert(vt_tray.setting_id);
                wxColour color = *wxWHITE;
                char     col_buf[10];
                sprintf(col_buf, "%02X%02X%02XFF", (int) color.Red(), (int) color.Green(), (int) color.Blue());
                try {
                    BOOST_LOG_TRIVIAL(info) << "vt_tray.setting_id is not exist in filament_list and reset vt_tray and the filament_id is: " << vt_tray.setting_id;
                    command_ams_filament_settings(vt_id, 0, "", "", std::string(col_buf), "", 0, 0);
                    continue;
                } catch (...) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " stoi error and tray_id" << vt_tray.id;
                }
            }
        }
        if (vt_tray.setting_id.size() == 8 && vt_tray.setting_id[0] == 'P' && vt_tray.nozzle_temp_min != "" && vt_tray.nozzle_temp_max != "") {
            if (checked_filament.find(vt_tray.setting_id) == checked_filament.end()) {
                need_checked_filament_id[nozzle_diameter_str].insert(vt_tray.setting_id);
                try {
                    std::string        preset_setting_id;
                    PresetBundle *     preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;
                    std::ostringstream stream;
                    stream << std::fixed << std::setprecision(1) << m_extder_data.extders[MAIN_NOZZLE_ID].current_nozzle_diameter;
                    std::string nozzle_diameter_str = stream.str();
                    bool        is_equation = preset_bundle->check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(MachineObject::get_preset_printer_model_name(
                                                                                                                               this->printer_type),
                                                                                                                           nozzle_diameter_str, vt_tray.setting_id,
                                                                                                                           vt_tray.tag_uid, vt_tray.nozzle_temp_min,
                                                                                                                           vt_tray.nozzle_temp_max, preset_setting_id);
                    if (!is_equation) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__
                                                << " vt_tray filament is not match min max temp and reset, filament_id: " << vt_tray.setting_id;
                        command_ams_filament_settings(vt_id, 0, vt_tray.setting_id, preset_setting_id, vt_tray.color, vt_tray.type, std::stoi(vt_tray.nozzle_temp_min),
                                                          std::stoi(vt_tray.nozzle_temp_max));

                    }
                } catch (...) {
                    BOOST_LOG_TRIVIAL(info) << "check fail and vt_tray.id" << vt_tray.id;
                }
            }
        }
    } while (0);

    for (auto &diameter_pair : m_nozzle_filament_data) {
        auto &diameter = diameter_pair.first;
        auto &data     = diameter_pair.second;
        for (auto &filament_id : need_checked_filament_id[diameter]) {
            data.checked_filament.insert(filament_id);
        }
    }
}

bool DeviceManager::EnableMultiMachine = false;
bool DeviceManager::key_field_only = false;

std::vector<float> nozzle_diameter_list{ 0.2f,0.4f,0.6f,0.8f };
std::vector<std::string> nozzle_type_list{ "hardened_steel", "stainless_steel" };

DeviceManager::DeviceManager(NetworkAgent* agent)
{
    m_agent = agent;

    // Load saved local machines
    if (agent) {
        AppConfig*  config         = GUI::wxGetApp().app_config;
        const auto local_machines = config->get_local_machines();
        for (auto& it : local_machines) {
            const auto&    m         = it.second;
            MachineObject* obj       = new MachineObject(m_agent, m.dev_name, m.dev_id, m.dev_ip);
            obj->printer_type        = m.printer_type;
            obj->dev_connection_type = "lan";
            obj->bind_state          = "free";
            obj->bind_sec_link       = "secure";
            obj->m_is_online         = true;
            obj->last_alive          = Slic3r::Utils::get_current_time_utc();
            obj->set_access_code(config->get("access_code", m.dev_id), false);
            obj->set_user_access_code(config->get("user_access_code", m.dev_id), false);
            if (obj->has_access_right()) {
                localMachineList.insert(std::make_pair(m.dev_id, obj));
            } else {
                config->erase_local_machine(m.dev_id);
                delete obj;
            }
        }
    }
}

void DeviceManager::update_local_machine(const MachineObject& m)
{
    AppConfig* config = GUI::wxGetApp().app_config;
    if (config) {
        if (m.is_lan_mode_printer()) {
            if (m.has_access_right()) {
                BBLocalMachine local_machine;
                local_machine.dev_id       = m.dev_id;
                local_machine.dev_name     = m.dev_name;
                local_machine.dev_ip       = m.dev_ip;
                local_machine.printer_type = m.printer_type;
                config->update_local_machine(local_machine);
            }
        } else {
            config->erase_local_machine(m.dev_id);
        }
    }
}

DeviceManager::~DeviceManager()
{
    for (auto it = localMachineList.begin(); it != localMachineList.end(); it++) {
        if (it->second) {
            delete it->second;
            it->second = nullptr;
        }
    }
    localMachineList.clear();

    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        if (it->second) {
            delete it->second;
            it->second = nullptr;
        }
    }
    userMachineList.clear();
}


float DeviceManager::nozzle_diameter_conver(int diame)
{
    if (diame < nozzle_diameter_list.size() && diame >= 0) {
        return nozzle_diameter_list[diame];
    }
    return 0.4f;
}

int DeviceManager::nozzle_diameter_conver(float diame)
{
    int index = -1;
    for (int i = 0; i < nozzle_diameter_list.size(); i++) {
        if (nozzle_diameter_list[i] == diame) {
            index = i;
        }
    }
    return index;
}

std::string DeviceManager::nozzle_type_conver(int type)
{
    if (type < nozzle_type_list.size() && type >= 0) {
        return nozzle_type_list[type];
    }
    return "";
}

int DeviceManager::nozzle_type_conver(std::string& type)
{
    int index = -1;
    for (int i = 0; i < nozzle_type_list.size(); i++) {
        if (nozzle_type_list[i] == type) {
            index = i;
        }
    }
    return index;
}

void DeviceManager::set_agent(NetworkAgent* agent)
{
    m_agent = agent;
}

void DeviceManager::keep_alive()
{
    MachineObject* obj = this->get_selected_machine();
    if (obj) {
        if (obj->keep_alive_count == 0) {
            obj->last_keep_alive = std::chrono::system_clock::now();
        }
        obj->keep_alive_count++;
        std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
        auto internal = std::chrono::duration_cast<std::chrono::milliseconds>(start - obj->last_keep_alive);
        if (internal.count() > TIMEOUT_FOR_KEEPALIVE && (internal.count() < 1000 * 60 * 60 * 300) ) {
            BOOST_LOG_TRIVIAL(info) << "keep alive = " << internal.count() << ", count = " << obj->keep_alive_count;
            obj->command_request_push_all();
            obj->last_keep_alive = start;
        }
        else if(obj->m_push_count == 0){
            BOOST_LOG_TRIVIAL(info) << "keep alive = " << internal.count() << ", push_count = 0, count = " << obj->keep_alive_count;
            obj->command_request_push_all();
            obj->last_keep_alive = start;
        }
    }
}

void DeviceManager::check_pushing()
{
    keep_alive();
    MachineObject* obj = this->get_selected_machine();
    if (obj && !obj->is_support_mqtt_alive) {
        std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
        auto internal = std::chrono::duration_cast<std::chrono::milliseconds>(start - obj->last_update_time);
        if (internal.count() > TIMEOUT_FOR_STRAT && internal.count() < 1000 * 60 * 60 * 300) {
            BOOST_LOG_TRIVIAL(info) << "command_pushing: diff = " << internal.count();
            obj->command_pushing("start");
        }
    }
}



void DeviceManager::on_machine_alive(std::string json_str)
{
    try {
        BOOST_LOG_TRIVIAL(trace) << "DeviceManager::SsdpDiscovery, json" << json_str;
        json j = json::parse(json_str);
        std::string dev_name        = j["dev_name"].get<std::string>();
        std::string dev_id          = j["dev_id"].get<std::string>();
        std::string dev_ip          = j["dev_ip"].get<std::string>();
        std::string printer_type_str= j["dev_type"].get<std::string>();
        std::string printer_signal  = j["dev_signal"].get<std::string>();
        std::string connect_type    = j["connect_type"].get<std::string>();
        std::string bind_state      = j["bind_state"].get<std::string>();

        if (connect_type == "farm") {
            connect_type ="lan";
            bind_state   = "free";
        }

        std::string sec_link = "";
        std::string ssdp_version = "";
        if (j.contains("sec_link")) {
            sec_link = j["sec_link"].get<std::string>();
        }
        if (j.contains("ssdp_version")) {
            ssdp_version = j["ssdp_version"].get<std::string>();
        }
        std::string connection_name = "";
        if (j.contains("connection_name")) {
            connection_name = j["connection_name"].get<std::string>();
        }

        MachineObject* obj;

        /* update userMachineList info */
        auto it = userMachineList.find(dev_id);
        if (it != userMachineList.end()) {
            it->second->dev_ip                  = dev_ip;
            it->second->bind_state              = bind_state;
            it->second->bind_sec_link           = sec_link;
            it->second->dev_connection_type     = connect_type;
            it->second->bind_ssdp_version       = ssdp_version;
        }

        /* update localMachineList */
        it = localMachineList.find(dev_id);
        if (it != localMachineList.end()) {
            // update properties
            /* ip changed */
            obj = it->second;

            if (obj->dev_ip.compare(dev_ip) != 0) {
                if ( connection_name.empty() ) {
                    BOOST_LOG_TRIVIAL(info) << "MachineObject IP changed from " << Slic3r::GUI::wxGetApp().format_IP(obj->dev_ip) << " to " << Slic3r::GUI::wxGetApp().format_IP(dev_ip);
                    obj->dev_ip = dev_ip;
                }
                else {
                    if ( obj->dev_connection_name.empty() || obj->dev_connection_name.compare(connection_name) == 0) {
                        BOOST_LOG_TRIVIAL(info) << "MachineObject IP changed from " << Slic3r::GUI::wxGetApp().format_IP(obj->dev_ip) << " to " << Slic3r::GUI::wxGetApp().format_IP(dev_ip) << " connection_name is " << connection_name;
                        if(obj->dev_connection_name.empty()){obj->dev_connection_name = connection_name;}
                        obj->dev_ip = dev_ip;
                    }

                }
                /* ip changed reconnect mqtt */
            }


            obj->wifi_signal        = printer_signal;
            obj->dev_connection_type= connect_type;
            obj->bind_state         = bind_state;
            obj->bind_sec_link      = sec_link;
            obj->bind_ssdp_version = ssdp_version;
            obj->printer_type = MachineObject::parse_printer_type(printer_type_str);

            // U0 firmware
            if (obj->dev_connection_type.empty() && obj->bind_state.empty())
                obj->bind_state = "free";

            BOOST_LOG_TRIVIAL(debug) << "SsdpDiscovery:: Update Machine Info, printer_sn = " << dev_id << ", signal = " << printer_signal;
            obj->last_alive = Slic3r::Utils::get_current_time_utc();
            obj->m_is_online = true;

            /* if (!obj->dev_ip.empty()) {
                 Slic3r::GUI::wxGetApp().app_config->set_str("ip_address", obj->dev_id, obj->dev_ip);
                 Slic3r::GUI::wxGetApp().app_config->save();
             }*/
        }
        else {
            /* insert a new machine */
            obj = new MachineObject(m_agent, dev_name, dev_id, dev_ip);
            obj->printer_type = MachineObject::parse_printer_type(printer_type_str);
            obj->wifi_signal = printer_signal;
            obj->dev_connection_type = connect_type;
            obj->bind_state     = bind_state;
            obj->bind_sec_link  = sec_link;
            obj->dev_connection_name = connection_name;
            obj->bind_ssdp_version = ssdp_version;
            obj->m_is_online = true;

            //load access code
            AppConfig* config = Slic3r::GUI::wxGetApp().app_config;
            if (config) {
                obj->set_access_code(Slic3r::GUI::wxGetApp().app_config->get("access_code", dev_id));
                obj->set_user_access_code(Slic3r::GUI::wxGetApp().app_config->get("user_access_code", dev_id));
            }
            localMachineList.insert(std::make_pair(dev_id, obj));

            /* if (!obj->dev_ip.empty()) {
                 Slic3r::GUI::wxGetApp().app_config->set_str("ip_address", obj->dev_id, obj->dev_ip);
                 Slic3r::GUI::wxGetApp().app_config->save();
             }*/


            BOOST_LOG_TRIVIAL(info) << "SsdpDiscovery::New Machine, ip = " << Slic3r::GUI::wxGetApp().format_IP(dev_ip) << ", printer_name= " << dev_name << ", printer_type = " << printer_type_str << ", signal = " << printer_signal;
        }
        update_local_machine(*obj);
    }
    catch (...) {
        ;
    }
}

MachineObject* DeviceManager::insert_local_device(const BBLocalMachine& machine, std::string connection_type, std::string bind_state, std::string version, std::string access_code)
{
    MachineObject* obj;
    auto           it = localMachineList.find(machine.dev_id);
    if (it != localMachineList.end()) {
        obj = it->second;
    } else {
        obj = new MachineObject(m_agent, machine.dev_name, machine.dev_id, machine.dev_ip);
        localMachineList.insert(std::make_pair(machine.dev_id, obj));
    }
    obj->printer_type = MachineObject::parse_printer_type(machine.printer_type);
    obj->dev_connection_type = connection_type;
    obj->bind_state = bind_state;
    obj->bind_sec_link = "secure";
    obj->bind_ssdp_version = version;
    obj->m_is_online = true;
    obj->last_alive = Slic3r::Utils::get_current_time_utc();
    obj->set_access_code(access_code, false);
    obj->set_user_access_code(access_code, false);

    update_local_machine(*obj);

    return obj;
}

void DeviceManager::disconnect_all()
{

}

int DeviceManager::query_bind_status(std::string &msg)
{
    if (!m_agent) {
        msg = "";
        return -1;
    }

    BOOST_LOG_TRIVIAL(trace) << "DeviceManager::query_bind_status";
    std::map<std::string, MachineObject*>::iterator it;
    std::vector<std::string> query_list;
    for (it = localMachineList.begin(); it != localMachineList.end(); it++) {
        query_list.push_back(it->first);
    }

    unsigned int http_code;
    std::string http_body;
    int result = m_agent->query_bind_status(query_list, &http_code, &http_body);

    if (result < 0) {
        msg = (boost::format("code=%1%,body=%2") % http_code % http_body).str();
    } else {
        msg = "";
        try {
            json j = json::parse(http_body);
            if (j.contains("bind_list")) {

                for (auto& item : j["bind_list"]) {
                    auto it = localMachineList.find(item["dev_id"].get<std::string>());
                    if (it != localMachineList.end()) {
                        if (!item["user_id"].is_null())
                            it->second->bind_user_id = item["user_id"].get<std::string>();
                        if (!item["user_name"].is_null())
                            it->second->bind_user_name = item["user_name"].get<std::string>();
                        else
                            it->second->bind_user_name = "Free";
                    }
                }
            }
        } catch(...) {
            ;
        }
    }
    return result;
}

MachineObject* DeviceManager::get_local_selected_machine()
{
    return get_local_machine(local_selected_machine);
}

void DeviceManager::reload_printer_settings()
{
    for (auto obj : this->userMachineList)
        obj.second->reload_printer_settings();
}

MachineObject* DeviceManager::get_default_machine() {

    std::string dev_id;
    if (m_agent) {
        m_agent->get_user_selected_machine();
    }
    if (dev_id.empty()) return nullptr;

    auto it = userMachineList.find(dev_id);
    if (it == userMachineList.end()) return nullptr;
    return it->second;
}

MachineObject* DeviceManager::get_local_machine(std::string dev_id)
{
    if (dev_id.empty()) return nullptr;
    auto it = localMachineList.find(dev_id);
    if (it == localMachineList.end()) return nullptr;
    return it->second;
}

void DeviceManager::erase_user_machine(std::string dev_id)
{
    userMachineList.erase(dev_id);
}

MachineObject* DeviceManager::get_user_machine(std::string dev_id)
{
    if (!Slic3r::GUI::wxGetApp().is_user_login())
        return nullptr;

    std::map<std::string, MachineObject*>::iterator it = userMachineList.find(dev_id);
    if (it == userMachineList.end()) return nullptr;
    return it->second;
}

MachineObject* DeviceManager::get_my_machine(std::string dev_id)
{
    auto list = get_my_machine_list();
    auto it = list.find(dev_id);
    if (it != list.end()) {
        return it->second;
    }
    return nullptr;
}

void DeviceManager::clean_user_info()
{
    BOOST_LOG_TRIVIAL(trace) << "DeviceManager::clean_user_info";
    // reset selected_machine
    selected_machine = "";
    local_selected_machine = "";

    // clean access code
    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        it->second->set_access_code("");
    }
    // clean user list
    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        if (it->second) {
            delete it->second;
            it->second = nullptr;
        }
    }
    userMachineList.clear();
}

bool DeviceManager::set_selected_machine(std::string dev_id, bool need_disconnect)
{
    BOOST_LOG_TRIVIAL(info) << "set_selected_machine=" << dev_id;
    auto my_machine_list = get_my_machine_list();
    auto it = my_machine_list.find(dev_id);

    // disconnect last
    auto last_selected = my_machine_list.find(selected_machine);
    if (last_selected != my_machine_list.end()) {
        last_selected->second->m_active_state = MachineObject::NotActive;
        if (last_selected->second->connection_type() == "lan") {
            if (last_selected->second->is_connecting() && !need_disconnect)
                return false;

            if (!need_disconnect) {m_agent->disconnect_printer(); }
        }
    }

    // connect curr
    if (it != my_machine_list.end()) {
        if (selected_machine == dev_id) {
            if (it->second->connection_type() != "lan") {
                // only reset update time
                it->second->reset_update_time();

                // check subscribe state
                Slic3r::GUI::wxGetApp().on_start_subscribe_again(dev_id);

                return true;
            } else {
                // lan mode printer reconnect printer
                if (m_agent) {
                    if (!need_disconnect) {m_agent->disconnect_printer();}
                    it->second->reset();
#if !BBL_RELEASE_TO_PUBLIC
                    it->second->connect(false, Slic3r::GUI::wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true" ? true : false);
#else
                    it->second->connect(false, it->second->local_use_ssl_for_mqtt);
#endif
                    it->second->set_lan_mode_connection_state(true);
                }
            }
        } else {
            if (m_agent) {
                if (it->second->connection_type() != "lan" || it->second->connection_type().empty()) {
                    if (m_agent->get_user_selected_machine() == dev_id) {
                        it->second->reset_update_time();
                    }
                    else {
                        BOOST_LOG_TRIVIAL(info) << "static: set_selected_machine: same dev_id = " << dev_id;
                        m_agent->set_user_selected_machine(dev_id);
                        it->second->reset();
                    }
                } else {
                    BOOST_LOG_TRIVIAL(info) << "static: set_selected_machine: same dev_id = empty";
                    it->second->reset();
#if !BBL_RELEASE_TO_PUBLIC
                    it->second->connect(false, Slic3r::GUI::wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true" ? true : false);
#else
                    it->second->connect(false, it->second->local_use_ssl_for_mqtt);
#endif
                    m_agent->set_user_selected_machine(dev_id);
                    it->second->set_lan_mode_connection_state(true);
                }
            }
        }
        for (auto& data : it->second->m_nozzle_filament_data) {
            data.second.checked_filament.clear();
        }
    }
    selected_machine = dev_id;
    return true;
}

MachineObject* DeviceManager::get_selected_machine()
{
    if (selected_machine.empty()) return nullptr;

    MachineObject* obj = get_user_machine(selected_machine);
    if (obj)
        return obj;

    // return local machine has access code
    auto it = localMachineList.find(selected_machine);
    if (it != localMachineList.end()) {
        if (it->second->has_access_right())
            return it->second;
    }
    return nullptr;
}

void DeviceManager::add_user_subscribe()
{
    /* user machine */
    std::vector<std::string> dev_list;
    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        dev_list.push_back(it->first);
        BOOST_LOG_TRIVIAL(trace) << "add_user_subscribe: " << it->first;
    }
    m_agent->add_subscribe(dev_list);
}

void DeviceManager::del_user_subscribe()
{
    /* user machine */
    std::vector<std::string> dev_list;
    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        dev_list.push_back(it->first);
        BOOST_LOG_TRIVIAL(trace) << "del_user_subscribe: " << it->first;
    }
    m_agent->del_subscribe(dev_list);
}

void DeviceManager::subscribe_device_list(std::vector<std::string> dev_list)
{
    std::vector<std::string> unsub_list;
    subscribe_list_cache.clear();
    for (auto& it : subscribe_list_cache) {
        if (it != selected_machine) {
            unsub_list.push_back(it);
            BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: unsub dev id = " << it;
        }
    }
    BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: unsub_list size = " << unsub_list.size();

    if (!selected_machine.empty()) {
        subscribe_list_cache.push_back(selected_machine);
    }
    for (auto& it : dev_list) {
        subscribe_list_cache.push_back(it);
        BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: sub dev id = " << it;
    }
    BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: sub_list size = " << subscribe_list_cache.size();
    if (!unsub_list.empty())
        m_agent->del_subscribe(unsub_list);
    if (!dev_list.empty())
        m_agent->add_subscribe(subscribe_list_cache);
}

std::map<std::string, MachineObject*> DeviceManager::get_my_machine_list()
{
    std::map<std::string, MachineObject*> result;

    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        if (!it->second)
            continue;
        if (!it->second->is_lan_mode_printer())
            result.insert(std::make_pair(it->first, it->second));
    }

    for (auto it = localMachineList.begin(); it != localMachineList.end(); it++) {
        if (!it->second)
            continue;
        if (it->second->has_access_right() && it->second->is_avaliable() && it->second->is_lan_mode_printer()) {
            // remove redundant in userMachineList
            if (result.find(it->first) == result.end()) {
                result.emplace(std::make_pair(it->first, it->second));
            }
        }
    }
    return result;
}

std::map<std::string, MachineObject*> DeviceManager::get_my_cloud_machine_list()
{
    std::map<std::string, MachineObject*> result;

    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        if (!it->second)
            continue;
        if (!it->second->is_lan_mode_printer())
            result.emplace(*it);
    }
    return result;
}

std::string DeviceManager::get_first_online_user_machine() {
    for (auto it = userMachineList.begin(); it != userMachineList.end(); it++) {
        if (it->second && it->second->is_online()) {
            return it->second->dev_id;
        }
    }
    return "";
}

void DeviceManager::modify_device_name(std::string dev_id, std::string dev_name)
{
    BOOST_LOG_TRIVIAL(trace) << "modify_device_name";
    if (m_agent) {
        int result = m_agent->modify_printer_name(dev_id, dev_name);
        if (result == 0) {
            update_user_machine_list_info();
        }
    }
}

void DeviceManager::parse_user_print_info(std::string body)
{
    BOOST_LOG_TRIVIAL(trace) << "DeviceManager::parse_user_print_info";
    std::lock_guard<std::mutex> lock(listMutex);
    std::set<std::string> new_list;
    try {
        json j = json::parse(body);
        if (j.contains("devices") && !j["devices"].is_null()) {
            for (auto& elem : j["devices"]) {
                MachineObject* obj = nullptr;
                std::string dev_id;
                if (!elem["dev_id"].is_null()) {
                    dev_id = elem["dev_id"].get<std::string>();
                    new_list.insert(dev_id);
                }
                std::map<std::string, MachineObject*>::iterator iter = userMachineList.find(dev_id);
                if (iter != userMachineList.end()) {
                    /* update field */
                    obj = iter->second;
                    obj->dev_id = dev_id;
                }
                else {
                    obj = new MachineObject(m_agent, "", "", "");
                    if (m_agent) {
                        obj->set_bind_status(m_agent->get_user_name());
                    }

                    if (obj->dev_ip.empty()) {
                        obj->dev_ip = Slic3r::GUI::wxGetApp().app_config->get("ip_address", dev_id);
                    }
                    userMachineList.insert(std::make_pair(dev_id, obj));
                }

                if (!obj) continue;

                if (!elem["dev_id"].is_null())
                    obj->dev_id = elem["dev_id"].get<std::string>();
                if (!elem["dev_name"].is_null())
                    obj->dev_name = elem["dev_name"].get<std::string>();
                if (!elem["dev_online"].is_null())
                    obj->m_is_online = elem["dev_online"].get<bool>();
                if (elem.contains("dev_model_name") && !elem["dev_model_name"].is_null())
                    obj->printer_type = elem["dev_model_name"].get<std::string>();
                if (!elem["task_status"].is_null())
                    obj->iot_print_status = elem["task_status"].get<std::string>();
                if (elem.contains("dev_product_name") && !elem["dev_product_name"].is_null())
                    obj->product_name = elem["dev_product_name"].get<std::string>();
                if (elem.contains("dev_access_code") && !elem["dev_access_code"].is_null()) {
                    std::string acc_code = elem["dev_access_code"].get<std::string>();
                    acc_code.erase(std::remove(acc_code.begin(), acc_code.end(), '\n'), acc_code.end());
                    obj->set_access_code(acc_code);
                }
            }

            //remove MachineObject from userMachineList
            std::map<std::string, MachineObject*>::iterator iterat;
            for (iterat = userMachineList.begin(); iterat != userMachineList.end(); ) {
                if (new_list.find(iterat->first) == new_list.end()) {
                    iterat = userMachineList.erase(iterat);
                }
                else {
                    iterat++;
                }
            }
        }
    }
    catch (std::exception& e) {
        ;
    }
}

void DeviceManager::update_user_machine_list_info()
{
    if (!m_agent) return;

    BOOST_LOG_TRIVIAL(debug) << "update_user_machine_list_info";
    unsigned int http_code;
    std::string body;
    int result = m_agent->get_user_print_info(&http_code, &body);
    if (result == 0) {
        parse_user_print_info(body);
    }
}


std::map<std::string ,MachineObject*> DeviceManager::get_local_machine_list()
{
    std::map<std::string, MachineObject*> result;
    std::map<std::string, MachineObject*>::iterator it;

    for (it = localMachineList.begin(); it != localMachineList.end(); it++) {
        if (it->second->m_is_online) {
            result.insert(std::make_pair(it->first, it->second));
        }
    }

    return result;
}

void DeviceManager::load_last_machine()
{
    // Get all available machines, include cloud machines and lan machines that have access right
    auto all_machines = get_my_machine_list();
    if (all_machines.empty())
        return;

    // Then connect to the machine we last selected if available
    const std::string last_monitor_machine = m_agent ? m_agent->get_user_selected_machine() : "";
    const auto        last_machine         = all_machines.find(last_monitor_machine);
    if (last_machine != all_machines.end()) {
        this->set_selected_machine(last_machine->second->dev_id);
    } else {
        // If not, then select the first available one
        this->set_selected_machine(all_machines.begin()->second->dev_id);
    }
}

json DeviceManager::filaments_blacklist = json::object();



std::string DeviceManager::parse_printer_type(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "printer_type");
}
std::string DeviceManager::get_printer_display_name(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "display_name");
}
std::string DeviceManager::get_ftp_folder(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "ftp_folder");
}
PrinterArch DeviceManager::get_printer_arch(std::string type_str)
{
    return get_printer_arch_by_str(get_value_from_config<std::string>(type_str, "printer_arch"));
}
std::string DeviceManager::get_printer_thumbnail_img(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "printer_thumbnail_image");
}
std::string DeviceManager::get_printer_ams_type(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "use_ams_type");
}
std::string DeviceManager::get_printer_series(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "printer_series");
}
std::string DeviceManager::get_printer_diagram_img(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "printer_connect_help_image");
}
std::string DeviceManager::get_printer_ams_img(std::string type_str)
{
    return get_value_from_config<std::string>(type_str, "printer_use_ams_image");
}

bool DeviceManager::get_printer_is_enclosed(std::string type_str) {
    return get_value_from_config<bool>(type_str, "printer_is_enclosed");
}

std::vector<std::string> DeviceManager::get_resolution_supported(std::string type_str)
{
    std::vector<std::string> resolution_supported;

    std::string config_file = Slic3r::resources_dir() + "/printers/" + type_str + ".json";
    boost::nowide::ifstream json_file(config_file.c_str());
    try {
        json jj;
        if (json_file.is_open()) {
            json_file >> jj;
            if (jj.contains("00.00.00.00")) {
                json const& printer = jj["00.00.00.00"];
                if (printer.contains("camera_resolution")) {
                    for (auto res : printer["camera_resolution"])
                        resolution_supported.emplace_back(res.get<std::string>());
                }
            }
        }
    }
    catch (...) {}
    return resolution_supported;
}

std::vector<std::string> DeviceManager::get_compatible_machine(std::string type_str)
{
    std::vector<std::string> compatible_machine;
    std::string config_file = Slic3r::resources_dir() + "/printers/" + type_str + ".json";
    boost::nowide::ifstream json_file(config_file.c_str());
    try {
        json jj;
        if (json_file.is_open()) {
            json_file >> jj;
            if (jj.contains("00.00.00.00")) {
                json const& printer = jj["00.00.00.00"];
                if (printer.contains("compatible_machine")) {
                    for (auto res : printer["compatible_machine"])
                        compatible_machine.emplace_back(res.get<std::string>());
                }
            }
        }
    }
    catch (...) {}
    return compatible_machine;
}

boost::bimaps::bimap<std::string, std::string> DeviceManager::get_all_model_id_with_name()
{
    boost::bimaps::bimap<std::string, std::string> models;
    std::vector<wxString> m_files;

    wxDir dir(Slic3r::resources_dir() + "/printers/");
    if (!dir.IsOpened()) {
        return models;
    }

    wxString filename;
    bool     hasFile = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES);
    while (hasFile) {
        m_files.push_back(filename);
        hasFile = dir.GetNext(&filename);
    }

    for (wxString file : m_files) {
        if (!file.Lower().ends_with(".json")) continue;

        std::string config_file = Slic3r::resources_dir() + "/printers/" + file.ToStdString();
        boost::nowide::ifstream json_file(config_file.c_str());

        try {
            json jj;
            if (json_file.is_open()) {
                json_file >> jj;
                if (jj.contains("00.00.00.00")) {
                    json const &printer = jj["00.00.00.00"];

                    std::string model_id;
                    std::string display_name;
                    if (printer.contains("model_id")) {model_id = printer["model_id"].get<std::string>();}
                    if (printer.contains("display_name")) {display_name = printer["display_name"].get<std::string>();}

                    if (!model_id.empty() && !display_name.empty()) {
                        models.left.insert(make_pair(model_id, display_name));
                    }
                }
            }
        } catch (...) {}
    }

    return models;
}

bool DeviceManager::load_filaments_blacklist_config()
{
    filaments_blacklist = json::object();

    std::string config_file = Slic3r::resources_dir() + "/printers/filaments_blacklist.json";
    boost::nowide::ifstream json_file(config_file.c_str());

    try {
        if (json_file.is_open()) {
            json_file >> filaments_blacklist;
            return true;
        }
        else {
            BOOST_LOG_TRIVIAL(error) << "load filaments blacklist config failed, file = " << config_file;
        }
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(error) << "load filaments blacklist config failed, file = " << config_file;
        return false;
    }
    return true;
}

void DeviceManager::check_filaments_in_blacklist(std::string tag_vendor, std::string tag_type, bool& in_blacklist, std::string& ac, std::string& info)
{
    std::unordered_map<std::string, wxString> blacklist_prompt =
    {
        {"TPU: not supported", _L("TPU is not supported by AMS.")},
        {"Bambu PET-CF/PA6-CF: not supported",  _L("Bambu PET-CF/PA6-CF is not supported by AMS.")},
        {"PVA: flexible", _L("Damp PVA will become flexible and get stuck inside AMS, please take care to dry it before use.")},
        {"CF/GF: hard and brittle", _L("CF/GF filaments are hard and brittle, it's easy to break or get stuck in AMS, please use with caution.")}
    };

    in_blacklist = false;

    if (filaments_blacklist.contains("blacklist")) {
        for (auto prohibited_filament : filaments_blacklist["blacklist"]) {

            std::string vendor;
            std::string type;
            std::string action;
            std::string description;

            if (prohibited_filament.contains("vendor") &&
                prohibited_filament.contains("type") &&
                prohibited_filament.contains("action") &&
                prohibited_filament.contains("description"))
            {
                vendor = prohibited_filament["vendor"].get<std::string>();
                type = prohibited_filament["type"].get<std::string>();
                if (GUI::wxGetApp().app_config->get("skip_ams_blacklist_check") == "true") {
                    action = "warning";
                } else {
                    action = prohibited_filament["action"].get<std::string>();
                }
                description = prohibited_filament["description"].get<std::string>();

                description = blacklist_prompt[description].ToUTF8().data();
            }
            else {
                return;
            }

            std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
            std::transform(tag_vendor.begin(), tag_vendor.end(), tag_vendor.begin(), ::tolower);
            std::transform(tag_type.begin(), tag_type.end(), tag_type.begin(), ::tolower);
            std::transform(type.begin(), type.end(), type.begin(), ::tolower);

            //third party
            if (vendor == "third party") {
                if ("bambulab" != vendor && tag_type == type) {
                    in_blacklist = true;
                    ac = action;
                    info = description;
                    return;
                }
            }
            else {
                if (vendor == tag_vendor && tag_type == type) {
                    in_blacklist = true;
                    ac = action;
                    info = description;
                    return;
                }
            }
        }
    }
}

std::string DeviceManager::load_gcode(std::string type_str, std::string gcode_file)
{
    std::string gcode_full_path = Slic3r::resources_dir() + "/printers/" + gcode_file;
    std::ifstream gcode(encode_path(gcode_full_path.c_str()).c_str());
    try {
        std::stringstream gcode_str;
        if (gcode.is_open()) {
            gcode_str << gcode.rdbuf();
            gcode.close();
            return gcode_str.str();
        }
    } catch(...) {
        BOOST_LOG_TRIVIAL(error) << "load gcode file failed, file = " << gcode_file << ", path = " << gcode_full_path;
    }


    return "";
}

void change_the_opacity(wxColour& colour)
{
    if (colour.Alpha() == 255) {
        colour = wxColour(colour.Red(), colour.Green(), colour.Blue(), 254);
    }
}
} // namespace Slic3r
