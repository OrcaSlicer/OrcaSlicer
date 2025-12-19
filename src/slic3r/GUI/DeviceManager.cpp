#include "libslic3r/libslic3r.h"
#include "DeviceManager.hpp"
#include "libslic3r/Time.hpp"
#include "libslic3r/Thread.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "GuiColor.hpp"

#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "DeviceErrorDialog.hpp"
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
#include <wx/dir.h>
#include "fast_float/fast_float.h"

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevExtensionTool.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevNozzleSystem.h"
#include "DeviceCore/DevBed.h"
#include "DeviceCore/DevLamp.h"
#include "DeviceCore/DevFan.h"
#include "DeviceCore/DevStorage.h"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevCtrl.h"
#include "DeviceCore/DevInfo.h"
#include "DeviceCore/DevPrintOptions.h"
#include "DeviceCore/DevPrintTaskInfo.h"
#include "DeviceCore/DevHMS.h"

#include "DeviceCore/DevMapping.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevUtil.h"


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

int get_tray_id_by_ams_id_and_slot_id(int ams_id, int slot_id)
{
    if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
        return ams_id;
    } else {
        return ams_id * 4 + slot_id;
    }
}

wxString Slic3r::get_stage_string(int stage)
{
    switch(stage) {
    case 0:
        return _L("Printing");
    case 1:
        return _L("Auto bed leveling");
    case 2:
        return _L("Heatbed preheating");
    case 3:
        return _L("Vibration compensation");
    case 4:
        return _L("Changing filament");
    case 5:
        return _L("M400 pause");
    case 6:
        return _L("Paused (filament ran out)");
    case 7:
        return _L("Heating nozzle");
    case 8:
        return _L("Calibrating dynamic flow");
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
        return _L("Paused by the user");
    case 17:
        return _L("Pause (front cover fall off)");
    case 18:
        return _L("Calibrating the micro lidar");
    case 19:
        return _L("Calibrating flow ratio");
    case 20:
        return _L("Pause (nozzle temperature malfunction)");
    case 21:
        return _L("Pause (heatbed temperature malfunction)");
    case 22:
        return _L("Filament unloading");
    case 23:
        return _L("Pause (step loss)");
    case 24:
        return _L("Filament loading");
    case 25:
        return _L("Motor noise cancellation");
    case 26:
        return _L("Pause (AMS offline)");
    case 27:
        return _L("Pause (low speed of the heatbreak fan)");
    case 28:
        return _L("Pause (chamber temperature control problem)");
    case 29:
        return _L("Cooling chamber");
    case 30:
        return _L("Pause (G-code inserted by user)");
    case 31:
        return _L("Motor noise showoff");
    case 32:
        return _L("Pause (nozzle clumping)");
    case 33:
        return _L("Pause (cutter error)");
    case 34:
        return _L("Pause (first layer error)");
    case 35:
        return _L("Pause (nozzle clog)");
    case 36:
        return _L("Measuring motion precision");
    case 37:
        return _L("Enhancing motion precision");
    case 38:
        return _L("Measure motion accuracy");
    case 39:
        return _L("Nozzle offset calibration");
    case 40:
        return _L("high temperature auto bed leveling");
    case 41:
        return _L("Auto Check: Quick Release Lever");
    case 42:
        return _L("Auto Check: Door and Upper Cover");
    case 43:
        return _L("Laser Calibration");
    case 44:
        return _L("Auto Check: Platform");
    case 45:
        return _L("Confirming BirdsEye Camera location");
    case 46:
        return _L("Calibrating BirdsEye Camera");
    case 47:
        return _L("Auto bed leveling -phase 1");
    case 48:
        return _L("Auto bed leveling -phase 2");
    case 49:
        return _L("Heating chamber");
    case 50:
        return _L("Cooling heatbed");
    case 51:
        return _L("Printing calibration lines");
    case 52:
        return _L("Auto Check: Material");
    case 53:
        return _L("Live View Camera Calibration");
    case 54:
        return _L("Waiting for heatbed to reach target temperature");
    case 55:
        return _L("Auto Check: Material Position");
    case 56:
        return _L("Cutting Module Offset Calibration");
    case 57:
        return _L("Measuring Surface");
    case 58:
        return _L("Thermal Preconditioning for first layer optimization");
    case 65:
        return _L("Calibrating the detection position of nozzle clumping"); // N7
    default:
        BOOST_LOG_TRIVIAL(info) << "stage = " << stage;
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

static wxString _generate_nozzle_id(NozzleVolumeType nozzle_type, const std::string& diameter)
{
    // HS00-0.4
    std::string nozzle_id = "H";
    switch (nozzle_type) {
    case NozzleVolumeType::nvtStandard: {
        nozzle_id += "S";
        break;
    }
    case NozzleVolumeType::nvtHighFlow: {
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
        return NozzleVolumeType::nvtStandard;
    }
    NozzleVolumeType res = NozzleVolumeType::nvtStandard;
    if (str[1] == 'S')
        res = NozzleVolumeType::nvtStandard;
    else if (str[1] == 'H')
        res = NozzleVolumeType::nvtHighFlow;
    return res;
}

wxString MachineObject::get_printer_type_display_str() const
{
    std::string display_name = DevPrinterConfigUtil::get_printer_display_name(printer_type);
    if (!display_name.empty())
        return display_name;
    else
        return _L("Unknown");
}

std::string MachineObject::get_printer_thumbnail_img_str() const
{
    std::string img_str = DevPrinterConfigUtil::get_printer_thumbnail_img(printer_type);
    std::string img_url;

     if (!img_str.empty())
     {
        img_url = Slic3r::resources_dir() + "\\images\\" + img_str ;
        if (fs::exists(img_url + ".svg"))
        {
            return img_url;
        }
        else
        {
            img_url = img_str;
        }
     }
     else
     {
        img_url =  "printer_thumbnail";
     }

    return img_url;
}

std::string MachineObject::get_auto_pa_cali_thumbnail_img_str() const
{
    return DevPrinterConfigUtil::get_printer_auto_pa_cali_image(printer_type);
}

std::string MachineObject::get_ftp_folder()
{
    return DevPrinterConfigUtil::get_ftp_folder(printer_type);
}

bool MachineObject::HasRecentCloudMessage()
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_cloud_msg_time_);
    return diff.count() < 5000;
}

bool MachineObject::HasRecentLanMessage()
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_lan_msg_time_);
    return diff.count() < 5000;
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
        if (config) {
            if (!code.empty()) {
                GUI::wxGetApp().app_config->set_str("access_code", get_dev_id(), code);
                DeviceManager::update_local_machine(*this);
            } else {
                GUI::wxGetApp().app_config->erase("access_code", get_dev_id());
            }
        }
    }
}

void MachineObject::erase_user_access_code()
{
    this->user_access_code = "";
    AppConfig* config = GUI::wxGetApp().app_config;
    if (config) {
        GUI::wxGetApp().app_config->erase("user_access_code", get_dev_id());
        //GUI::wxGetApp().app_config->save();
    }
}

void MachineObject::set_user_access_code(std::string code, bool only_refresh)
{
    this->user_access_code = code;
    if (only_refresh && !code.empty()) {
        AppConfig* config = GUI::wxGetApp().app_config;
        if (config && !code.empty()) {
            GUI::wxGetApp().app_config->set_str("user_access_code", get_dev_id(), code);
            DeviceManager::update_local_machine(*this);
        }
    }
}

std::string MachineObject::get_user_access_code() const
{
    AppConfig* config = GUI::wxGetApp().app_config;
    if (config) {
        return GUI::wxGetApp().app_config->get("user_access_code", get_dev_id());
    }
    return "";
}

std::string MachineObject::get_show_printer_type() const
{
    std::string printer_type = this->printer_type;
    if (this->is_support_upgrade_kit && this->installed_upgrade_kit)
        printer_type = "C12";
    return printer_type;
}
PrinterSeries MachineObject::get_printer_series() const
{
    std::string series =  DevPrinterConfigUtil::get_printer_series_str(printer_type);
    if (series == "series_x1" || series == "series_o")
        return PrinterSeries::SERIES_X1;
    else if (series == "series_p1p")
        return PrinterSeries::SERIES_P1P;
    else
        return PrinterSeries::SERIES_P1P;
}

PrinterArch MachineObject::get_printer_arch() const
{
    return DevPrinterConfigUtil::get_printer_arch(printer_type);
}

std::string MachineObject::get_printer_ams_type() const
{
    return DevPrinterConfigUtil::get_printer_use_ams_type(printer_type);
}

bool MachineObject::is_series_n(const std::string& series_str) { return series_str == "series_n";  }
bool MachineObject::is_series_p(const std::string& series_str) { return series_str == "series_p1p";}
bool MachineObject::is_series_x(const std::string& series_str) { return series_str == "series_x1"; }
bool MachineObject::is_series_o(const std::string& series_str) { return series_str == "series_o";  }

bool MachineObject::is_series_n() const { return is_series_n(DevPrinterConfigUtil::get_printer_series_str(printer_type)); }
bool MachineObject::is_series_p() const { return is_series_p(DevPrinterConfigUtil::get_printer_series_str(printer_type)); }
bool MachineObject::is_series_x() const { return is_series_x(DevPrinterConfigUtil::get_printer_series_str(printer_type)); }
bool MachineObject::is_series_o() const { return is_series_o(DevPrinterConfigUtil::get_printer_series_str(printer_type)); }

std::string MachineObject::get_printer_series_str() const{ return DevPrinterConfigUtil::get_printer_series_str(printer_type);};

void MachineObject::reload_printer_settings()
{
    print_json.load_compatible_settings("", "");
    parse_json("cloud", "{}");
}

MachineObject::MachineObject(DeviceManager* manager, NetworkAgent* agent, std::string name, std::string id, std::string ip)
    :dev_name(name),
    dev_id(id),
    dev_ip(ip),
    subtask_(nullptr),
    model_task(nullptr),
    slice_info(nullptr),
    m_is_online(false)
{
    m_manager = manager;
    m_agent = agent;

    reset();

    /* temprature fields */

    chamber_temp        = 0.0f;
    chamber_temp_target = 0.0f;
    frame_temp          = 0.0f;

    /* ams fileds */
    ams_exist_bits = 0;
    tray_exist_bits = 0;
    tray_is_bbl_bits = 0;

    /* signals */
    wifi_signal = "";

    /* upgrade */
    upgrade_force_upgrade = false;
    upgrade_new_version = false;
    upgrade_consistency_request = false;


    /* printing */
    mc_print_stage = 0;
    mc_print_error_code = 0;
    print_error = 0;
    mc_print_line_number = 0;
    mc_print_percent = 0;
    mc_print_sub_stage = 0;
    mc_left_time = 0;
    hw_switch_state = 0;

    has_ipcam = true; // default true


    auto vslot = DevAmsTray(std::to_string(VIRTUAL_TRAY_MAIN_ID));
    vt_slot.push_back(vslot);

    {
        m_lamp = new DevLamp(this);
        m_fan = new DevFan(this);
        m_bed = new DevBed(this);
        m_storage       = new DevStorage(this);
        m_extder_system = new DevExtderSystem(this);
        m_extension_tool = DevExtensionTool::Create(this);
        m_nozzle_system = new DevNozzleSystem(this);
        m_fila_system   = new DevFilaSystem(this);
        m_hms_system    = new DevHMS(this);
        m_config = new DevConfig(this);

        m_ctrl = new DevCtrl(this);
        m_print_options = new DevPrintOptions(this);
    }
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

    free_slice_info();

    while (!m_command_error_code_dlgs.empty()) {
        delete *m_command_error_code_dlgs.begin();/*element will auto remove from m_command_error_code_dlgs on deleted*/
    }

    {
        delete m_lamp;
        m_lamp = nullptr;

        delete m_fan;
        m_fan  = nullptr;

        delete m_bed;
        m_bed = nullptr;

        delete m_extder_system;
        m_extder_system = nullptr;

        delete m_nozzle_system;
        m_nozzle_system = nullptr;

        delete m_ctrl;
        m_ctrl = nullptr;

        delete m_fila_system;
        m_fila_system = nullptr;

        delete m_hms_system;
        m_hms_system = nullptr;

        delete m_config;
        m_config = nullptr;

        delete m_print_options;
        m_print_options = nullptr;
    }
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


DevAmsTray *MachineObject::get_curr_tray()
{
    const std::string& cur_ams_id = m_extder_system->GetCurrentAmsId();
    if (cur_ams_id.compare(std::to_string(VIRTUAL_TRAY_MAIN_ID)) == 0) {
        return &vt_slot[0];
    }

    DevAms* curr_ams = get_curr_Ams();
    if (!curr_ams) return nullptr;

    auto it = curr_ams->GetTrays().find(m_extder_system->GetCurrentSlotId());
    if (it != curr_ams->GetTrays().end())
    {
        return it->second;
    }

    return nullptr;
}

std::string MachineObject::get_filament_id(std::string ams_id, std::string tray_id) const {
    return this->get_tray(ams_id, tray_id).setting_id;
}

std::string MachineObject::get_filament_type(const std::string& ams_id, const std::string& tray_id) const {
    return this->get_tray(ams_id, tray_id).get_filament_type();
}

std::string MachineObject::get_filament_display_type(const std::string& ams_id, const std::string& tray_id) const {
    return this->get_tray(ams_id, tray_id).get_display_filament_type();
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
    if (!HasAms())
        return true;

    if (ams_status_main == AMS_STATUS_MAIN_IDLE && hw_switch_state == 1 && m_extder_system->GetCurrentAmsId() == "255") {
        return true;
    }
    return result;
}

void MachineObject::get_ams_colors(std::vector<wxColour> &ams_colors) {
    m_fila_system->CollectAmsColors(ams_colors);
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

bool MachineObject::is_in_upgrading() const
{
    return upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress;
}

bool MachineObject::is_upgrading_avalable()
{
    return upgrade_display_state == DevFirmwareUpgradingState::UpgradingAvaliable;
}

int MachineObject::get_upgrade_percent() const
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

std::map<int, DevFirmwareVersionInfo> MachineObject::get_ams_version()
{
    std::vector<std::string> multi_tray_ams_type = {"ams", "n3f"};
    std::map<int, DevFirmwareVersionInfo> result;
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

void MachineObject::clear_version_info()
{
    air_pump_version_info = DevFirmwareVersionInfo();
    laser_version_info = DevFirmwareVersionInfo();
    cutting_module_version_info = DevFirmwareVersionInfo();
    extinguish_version_info = DevFirmwareVersionInfo();
    module_vers.clear();
}

void MachineObject::store_version_info(const DevFirmwareVersionInfo& info)
{
    if (info.isAirPump()) {
        air_pump_version_info = info;
    } else if (info.isLaszer()) {
        laser_version_info = info;
    } else if (info.isCuttingModule()) {
        cutting_module_version_info = info;
    } else if (info.isExtinguishSystem()) {
        extinguish_version_info = info;
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
    if (m_home_flag == 0) { return true; }

    if (axis == "X") {
        return (m_home_flag & 1) == 1;
    } else if (axis == "Y") {
        return ((m_home_flag >> 1) & 1) == 1;
    } else if (axis == "Z") {
        return ((m_home_flag >> 2) & 1) == 1;
    }

    return true;
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

void MachineObject::parse_home_flag(int flag)
{
    m_home_flag = flag;

    is_220V_voltage = ((flag >> 3) & 0x1) != 0;
    if (time(nullptr) - xcam_auto_recovery_hold_start > HOLD_TIME_3SEC) {
        xcam_auto_recovery_step_loss = ((flag >> 4) & 0x1) != 0;
    }

    camera_recording            = ((flag >> 5) & 0x1) != 0;

    if (time(nullptr) - ams_user_setting_start > HOLD_COUNT_MAX)
    {
        m_fila_system->GetAmsSystemSetting().SetDetectRemainEnabled(((flag >> 7) & 0x1) != 0);
    }

   // sdcard_state = MachineObject::SdcardState(get_flag_bits(flag, 8, 2));
   m_storage->set_sdcard_state(get_flag_bits(flag, 8, 2));


    if (time(nullptr) - ams_switch_filament_start > HOLD_TIME_3SEC)
    {
        m_fila_system->GetAmsSystemSetting().SetAutoRefillEnabled(((flag >> 10) & 0x1) != 0);
    }

    is_support_flow_calibration = ((flag >> 15) & 0x1) != 0;
    if (this->is_series_o()) { is_support_flow_calibration = false; } // todo: Temp modification due to incorrect machine push message for H2D

    is_support_pa_calibration = ((flag >> 16) & 0x1) != 0;
    if (this->is_series_p()) { is_support_pa_calibration = false; } // todo: Temp modification due to incorrect machine push message for P

    if (time(nullptr) - xcam_prompt_sound_hold_start > HOLD_TIME_3SEC) {
        xcam_allow_prompt_sound = ((flag >> 17) & 0x1) != 0;
    }

    is_support_prompt_sound = ((flag >> 18) & 0x1) != 0;
    is_support_filament_tangle_detect = ((flag >> 19) & 0x1) != 0;

    if (time(nullptr) - xcam_filament_tangle_detect_hold_start > HOLD_TIME_3SEC) {
        xcam_filament_tangle_detect = ((flag >> 20) & 0x1) != 0;
    }

    /*if(!is_support_motor_noise_cali){
        is_support_motor_noise_cali = ((flag >> 21) & 0x1) != 0;
    }*/
    is_support_motor_noise_cali = ((flag >> 21) & 0x1) != 0;

    is_support_user_preset = ((flag >> 22) & 0x1) != 0;

    is_support_nozzle_blob_detection = ((flag >> 25) & 0x1) != 0;

    if (time(nullptr) - nozzle_blob_detection_hold_start > HOLD_TIME_3SEC) {
        nozzle_blob_detection_enabled = ((flag >> 24) & 0x1) != 0;
    }

    is_support_air_print_detection = ((flag >> 29) & 0x1) != 0;
    if (auto ptr = m_fila_system->GetAmsFirmwareSwitch().lock();
        ptr->GetCurrentFirmwareIdxRun() == DevAmsSystemFirmwareSwitch::IDX_AMS_AMS2_AMSHT) {
        is_support_air_print_detection = false;// special case, for the firmware, air print is not supported
    }
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

bool MachineObject::is_filament_installed()
{
    if (m_extder_system->GetTotalExtderCount() > 0) {
        // right//or single
        auto ext = m_extder_system->m_extders[MAIN_EXTRUDER_ID];
        if (ext.m_ext_has_filament) {
            return true;
        }
    }
    /*left*/
    if (m_extder_system->GetTotalExtderCount() > 1) {
        auto ext = m_extder_system->m_extders[DEPUTY_EXTRUDER_ID];
        if (ext.m_ext_has_filament) {
            return true;
        }
    }
    return false;
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

bool MachineObject::canEnableTimelapse(wxString &error_message) const
{
    if (!is_support_timelapse) {
        error_message = _L("Timelapse is not supported on this printer.");
        return false;
    }

    if (is_support_internal_timelapse)
    {
        return true;
    }

    if (m_storage->get_sdcard_state() != DevStorage::SdcardState::HAS_SDCARD_NORMAL) {
        if (m_storage->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD) {
            error_message = _L("Timelapse is not supported while the storage does not exist.");
        } else if (m_storage->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL) {
            error_message = _L("Timelapse is not supported while the storage is unavailable.");
        } else if (m_storage->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_READONLY) {
            error_message = _L("Timelapse is not supported while the storage is readonly.");
        }

        return false;
    }

    return true;
}

int MachineObject::command_get_version(bool with_retry)
{
    BOOST_LOG_TRIVIAL(info) << "command_get_version";
    json j;
    j["info"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["info"]["command"] = "get_version";
    if (with_retry)
        get_version_retry = GET_VERSION_RETRYS;
    return this->publish_json(j, 1);
}

int MachineObject::command_get_access_code() {
    BOOST_LOG_TRIVIAL(info) << "command_get_access_code";
    json j;
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["command"] = "get_access_code";

    return this->publish_json(j);
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
    return this->publish_json(j);
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
        return this->publish_json(j);
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

    return this->publish_json(j);
}

int MachineObject::command_clean_print_error_uiop(int print_error)
{
    json j;
    j["system"]["command"] = "uiop";
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["name"] = "print_error";
    j["system"]["action"] = "close";
    j["system"]["source"] = 1;// 0-Mushu 1-Studio
    j["system"]["type"] = "dialog";

    // the error to be cleaned
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    j["system"]["err"] = std::string(buf);

    return this->publish_json(j);
}

int MachineObject::command_upgrade_confirm()
{
    BOOST_LOG_TRIVIAL(info) << "command_upgrade_confirm";
    json j;
    j["upgrade"]["command"] = "upgrade_confirm";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["src_id"] = 1; // 1 for slicer
    return this->publish_json(j);
}

int MachineObject::command_consistency_upgrade_confirm()
{
    BOOST_LOG_TRIVIAL(info) << "command_consistency_upgrade_confirm";
    json j;
    j["upgrade"]["command"] = "consistency_confirm";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["src_id"] = 1; // 1 for slicer
    return this->publish_json(j);
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

    return this->publish_json(j);
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

    return this->publish_json(j);
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
    if (m_support_mqtt_homing)
    {
        json j;
        j["print"]["command"] = "back_to_center";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return this->publish_json(j);
    }

    // gcode command
    return this->is_in_printing() ? this->publish_gcode("G28 X\n") : this->publish_gcode("G28 \n");
}

int MachineObject::command_task_partskip(std::vector<int> part_ids)
{
    BOOST_LOG_TRIVIAL(trace) << "command_task_partskip: ";
    json j;
    j["print"]["command"] = "skip_objects";
    j["print"]["obj_list"] = part_ids;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_task_abort()
{
    BOOST_LOG_TRIVIAL(trace) << "command_task_abort: ";
    json j;
    j["print"]["command"] = "stop";
    j["print"]["param"] = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_task_cancel(std::string job_id)
{
    BOOST_LOG_TRIVIAL(trace) << "command_task_cancel: " << job_id;
    json j;
    j["print"]["command"] = "stop";
    j["print"]["param"] = "";
    j["print"]["job_id"] = job_id;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_task_pause()
{
    json j;
    j["print"]["command"] = "pause";
    j["print"]["param"] = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_task_resume()
{
    if(check_resume_condition()) return 0;

    json j;
    j["print"]["command"] = "resume";
    j["print"]["param"] = "";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_hms_idle_ignore(const std::string &error_str, int type)
{
    if(check_resume_condition()) return 0;

    json j;
    j["print"]["command"]     = "idle_ignore";
    j["print"]["err"]         = error_str;
    j["print"]["type"]        = type;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    return this->publish_json(j, 1);
}

int MachineObject::command_hms_resume(const std::string& error_str, const std::string& job_id)
{
    if(check_resume_condition()) return 0;

    json j;
    j["print"]["command"] = "resume";
    j["print"]["err"] = error_str;
    j["print"]["param"] = "reserve";
    j["print"]["job_id"] = job_id;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_hms_ignore(const std::string& error_str, const std::string& job_id)
{
    if(check_resume_condition()) return 0;

    json j;
    j["print"]["command"] = "ignore";
    j["print"]["err"] = error_str;
    j["print"]["param"] = "reserve";
    j["print"]["job_id"] = job_id;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_hms_stop(const std::string &error_str, const std::string &job_id) {
    json j;
    j["print"]["command"]     = "stop";
    j["print"]["err"]         = error_str;
    j["print"]["param"]       = "reserve";
    j["print"]["job_id"]      = job_id;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_stop_buzzer()
{
    json j;
    j["print"]["command"] = "buzzer_ctrl";
    j["print"]["mode"] = 0;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    return this->publish_json(j, 1);
}

int MachineObject::command_set_bed(int temp)
{
    if (m_support_mqtt_bet_ctrl)
    {
        json j;
        j["print"]["command"] = "set_bed_temp";
        j["print"]["temp"] = temp;
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return this->publish_json(j);
    }

    std::string gcode_str = (boost::format("M140 S%1%\n") % temp).str();
    return this->publish_gcode(gcode_str);
}

int MachineObject::command_set_nozzle(int temp)
{
    std::string gcode_str = (boost::format("M104 S%1%\n") % temp).str();
    return this->publish_gcode(gcode_str);
}

int MachineObject::command_set_nozzle_new(int nozzle_id, int temp)
{
    BOOST_LOG_TRIVIAL(info) << "set_nozzle_temp";

    json j;
    j["print"]["sequence_id"]    = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"]        = "set_nozzle_temp";
    j["print"]["extruder_index"] = nozzle_id;
    j["print"]["target_temp"]    = temp;

    return this->publish_json(j, 1);
}

int MachineObject::command_refresh_nozzle(){
    json j;
    j["print"]["sequence_id"]    = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"]        = "refresh_nozzle";

    return this->publish_json(j, 1);
}

int MachineObject::command_set_chamber(int temp)
{
    json j;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"] = "set_ctt";
    j["print"]["ctt_val"] = temp;

    return this->publish_json(j, 1);
}

int MachineObject::check_resume_condition()
{
    if (jobState_ > 1) {
        GUI::wxGetApp().show_dialog(_L("To ensure your safety, certain processing tasks (such as laser) can only be resumed on printer."));
        return 1;
    }
    return 0;
}
int MachineObject::command_ams_change_filament(bool load, std::string ams_id, std::string slot_id, int old_temp, int new_temp)
{
    json j;
    try {
        auto tray_id = 0;
        if (atoi(ams_id.c_str()) < 16) {
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
    return this->publish_json(j);
}

int MachineObject::command_ams_user_settings(bool start_read_opt, bool tray_read_opt, bool remain_flag)
{
    json j;
    j["print"]["command"] = "ams_user_setting";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["ams_id"] = -1; // all ams
    j["print"]["startup_read_option"]   = start_read_opt;
    j["print"]["tray_read_option"]      = tray_read_opt;
    j["print"]["calibrate_remain_flag"] = remain_flag;

    m_fila_system->GetAmsSystemSetting().SetDetectOnInsertEnabled(tray_read_opt);
    m_fila_system->GetAmsSystemSetting().SetDetectOnPowerupEnabled(start_read_opt);
    m_fila_system->GetAmsSystemSetting().SetDetectRemainEnabled(remain_flag);
    ams_user_setting_start = time(nullptr);

    return this->publish_json(j);
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

    if (tag_ams_id == VIRTUAL_TRAY_MAIN_ID || tag_ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
        tag_tray_id = VIRTUAL_TRAY_DEPUTY_ID;
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

    return this->publish_json(j);
}

int MachineObject::command_ams_refresh_rfid(std::string tray_id)
{
    std::string gcode_cmd = (boost::format("M620 R%1% \n") % tray_id).str();
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode_cmd;
    return this->publish_gcode(gcode_cmd);
}

int MachineObject::command_ams_refresh_rfid2(int ams_id,  int slot_id)
{
    json j;
    j["print"]["command"]       = "ams_get_rfid";
    j["print"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["ams_id"]        = ams_id;
    j["print"]["slot_id"]       = slot_id;
    return this->publish_json(j);
}


int MachineObject::command_ams_select_tray(std::string tray_id)
{
    std::string gcode_cmd = (boost::format("M620 P%1% \n") % tray_id).str();
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode_cmd;
    return this->publish_gcode(gcode_cmd);
}

int MachineObject::command_ams_control(std::string action)
{
    if (action == "resume" && check_resume_condition()) return 0;

    //valid actions
    if (action == "resume" || action == "reset" || action == "pause" || action == "done" || action == "abort") {
        json j;
        j["print"]["command"] = "ams_control";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["param"] = action;
        return this->publish_json(j);
    }
    return -1;
}

int MachineObject::command_ams_drying_stop()
{
    json j;
    j["print"]["command"] = "auto_stop_ams_dry";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    return this->publish_json(j);
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
    return this->publish_json(j);
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
    return this->publish_json(j);
}


int MachineObject::command_set_printing_speed(DevPrintingSpeedLevel lvl)
{
    json j;
    j["print"]["command"] = "print_speed";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["param"] = std::to_string((int)lvl);

    return this->publish_json(j);
}

int MachineObject::command_set_printing_option(bool auto_recovery)
{
    int print_option = (int)auto_recovery << (int)PRINT_OP_AUTO_RECOVERY;
    json j;
    j["print"]["command"]       = "print_option";
    j["print"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["option"]        = print_option;
    j["print"]["auto_recovery"] = auto_recovery;

    return this->publish_json(j);
}

int MachineObject::command_nozzle_blob_detect(bool nozzle_blob_detect)
{
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_blob_detect"] = nozzle_blob_detect;
    nozzle_blob_detection_enabled = nozzle_blob_detect;
    nozzle_blob_detection_hold_start = time(nullptr);
    return this->publish_json(j);
}

int MachineObject::command_set_prompt_sound(bool prompt_sound){
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["sound_enable"] = prompt_sound;

    return this->publish_json(j);
}

int MachineObject::command_set_filament_tangle_detect(bool filament_tangle_detect) {
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["filament_tangle_detect"] = filament_tangle_detect;

    return this->publish_json(j);
}

int MachineObject::command_ams_switch_filament(bool switch_filament)
{
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["auto_switch_filament"] = switch_filament;

    m_fila_system->GetAmsSystemSetting().SetAutoRefillEnabled(switch_filament);
    BOOST_LOG_TRIVIAL(trace) << "command_ams_filament_settings:" << switch_filament;
    ams_switch_filament_start = time(nullptr);

    return this->publish_json(j);
}

int MachineObject::command_ams_air_print_detect(bool air_print_detect)
{
    json j;
    j["print"]["command"] = "print_option";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["air_print_detect"] = air_print_detect;

    ams_air_print_status = air_print_detect;
    BOOST_LOG_TRIVIAL(trace) << "command_ams_air_print_detect:" << air_print_detect;

    return this->publish_json(j);
}


int MachineObject::command_axis_control(std::string axis, double unit, double input_val, int speed)
{
    if (m_support_mqtt_axis_control)
    {
        json j;
        j["print"]["command"] = "xyz_ctrl";
        j["print"]["axis"] = axis;
        j["print"]["dir"] = input_val > 0 ? 1 : -1;
        j["print"]["mode"] = (std::abs(input_val) >= 10) ? 1 : 0;
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return this->publish_json(j);
    }

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
    }
    else {
        return -1;
    }


    return this->publish_gcode(cmd);
}

int MachineObject::command_extruder_control(int nozzle_id, double val)
{
    json j;
    j["print"]["command"]     = "set_extrusion_length";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["extruder_index"] = nozzle_id;
    j["print"]["length"] = (int)val;
    return this->publish_json(j);
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

int MachineObject::command_start_calibration(bool vibration, bool bed_leveling, bool xcam_cali, bool motor_noise, bool nozzle_cali, bool bed_cali, bool clumppos_cali)
{
    if (!is_support_command_calibration()) {
        // fixed gcode file
        json j;
        j["print"]["command"] = "gcode_file";
        j["print"]["param"] = "/usr/etc/print/auto_cali_for_user.gcode";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return this->publish_json(j);
    } else {
        json j;
        j["print"]["command"] = "calibration";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["option"]  = +   (clumppos_cali ? 1 << 6 : 0)
                                +   (bed_cali     ? 1 << 5 : 0)
                                +   (nozzle_cali  ? 1 << 4 : 0)
                                +   (motor_noise  ? 1 << 3 : 0)
                                +   (vibration    ? 1 << 2 : 0)
                                +   (bed_leveling ? 1 << 1 : 0)
                                +   (xcam_cali    ? 1 << 0 : 0);
        return this->publish_json(j);
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
        j["print"]["filaments"][i]["nozzle_id"]            = _generate_nozzle_id(pa_data.calib_datas[i].nozzle_volume_type,to_string_nozzle_diameter(pa_data.calib_datas[i].nozzle_diameter)).ToStdString();
        j["print"]["filaments"][i]["nozzle_diameter"]      = to_string_nozzle_diameter(pa_data.calib_datas[i].nozzle_diameter);
        j["print"]["filaments"][i]["max_volumetric_speed"] = std::to_string(pa_data.calib_datas[i].max_volumetric_speed);

        if (i > 0) filament_ids += ",";
        filament_ids += pa_data.calib_datas[i].filament_id;
    }

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali: " << j.dump();

    return this->publish_json(j);
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
            j["print"]["filaments"][i]["nozzle_id"]   = _generate_nozzle_id(pa_calib_values[i].nozzle_volume_type, to_string_nozzle_diameter(pa_calib_values[i].nozzle_diameter)).ToStdString();
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
        return this->publish_json(j);
    }

    return -1;
}

int MachineObject::command_delete_pa_calibration(const PACalibIndexInfo& pa_calib)
{
    json j;
    j["print"]["command"]         = "extrusion_cali_del";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["extruder_id"]     = pa_calib.extruder_id;
    j["print"]["nozzle_id"]       = _generate_nozzle_id(pa_calib.nozzle_volume_type, to_string_nozzle_diameter(pa_calib.nozzle_diameter)).ToStdString();
    j["print"]["filament_id"]     = pa_calib.filament_id;
    j["print"]["cali_idx"]        = pa_calib.cali_idx;
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(pa_calib.nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_del: " << j.dump();
    return this->publish_json(j);
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
        j["print"]["nozzle_id"] = _generate_nozzle_id(calib_info.nozzle_volume_type, to_string_nozzle_diameter(calib_info.nozzle_diameter)).ToStdString();
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(calib_info.nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_get: " << j.dump();
    request_tab_from_bbs = true;
    return this->publish_json(j);
}

int MachineObject::command_get_pa_calibration_result(float nozzle_diameter)
{
    json j;
    j["print"]["command"]         = "extrusion_cali_get_result";
    j["print"]["sequence_id"]     = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["nozzle_diameter"] = to_string_nozzle_diameter(nozzle_diameter);

    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_get_result: " << j.dump();
    return this->publish_json(j);
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
    return this->publish_json(j);
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
            j["print"]["filaments"][i]["extruder_id"]          = calib_data.calib_datas[i].extruder_id;
            j["print"]["filaments"][i]["ams_id"]               = calib_data.calib_datas[i].ams_id;
            j["print"]["filaments"][i]["slot_id"]              = calib_data.calib_datas[i].slot_id;

            if (i > 0)
                filament_ids += ",";
            filament_ids += calib_data.calib_datas[i].filament_id;
        }

        BOOST_LOG_TRIVIAL(info) << "flowrate_cali: " << j.dump();
        return this->publish_json(j);
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
    return this->publish_json(j);
}

int MachineObject::command_ipcam_record(bool on_off)
{
    BOOST_LOG_TRIVIAL(info) << "command_ipcam_record = " << on_off;
    json j;
    j["camera"]["command"] = "ipcam_record_set";
    j["camera"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["camera"]["control"] = on_off ? "enable" : "disable";
    camera_recording_ctl_start          = time(nullptr);
    this->camera_recording_when_printing = on_off;
    return this->publish_json(j);
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
    return this->publish_json(j);
}

int MachineObject::command_ipcam_resolution_set(std::string resolution)
{
    BOOST_LOG_TRIVIAL(info) << "command:ipcam_resolution_set" << ", resolution:" << resolution;
    json j;
    j["camera"]["command"] = "ipcam_resolution_set";
    j["camera"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["camera"]["resolution"] = resolution;
    camera_resolution_hold_count = HOLD_COUNT_CAMERA;
    camera_recording_ctl_start = time(nullptr);
    this->camera_resolution = resolution;
    return this->publish_json(j);
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

   // int           cfg = 123;
   // get_flag_bits(cfg, 11, 2);

    BOOST_LOG_TRIVIAL(info) << "command:xcam_control_set" << ", module_name:" << module_name << ", control:" << on_off << ", halt_print_sensitivity:" << lvl;
    return this->publish_json(j);
}

int MachineObject::command_ack_proceed(json& proceed) {
    if (proceed["command"].empty()) return -1;

    proceed["err_code"] = 0;
    if (proceed.contains("err_ignored")) {
        proceed["err_ignored"].push_back(proceed["err_index"]);
    } else {
        proceed["err_ignored"] = std::vector<int>{proceed["err_index"]};
    }
    proceed["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);

    json j;
    j["print"] = proceed;
    return this->publish_json(j);
}

int MachineObject::command_xcam_control_ai_monitoring(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false:true;

    xcam_ai_monitoring = on_off;
    xcam_ai_monitoring_hold_start  = time(nullptr);
    xcam_ai_monitoring_sensitivity = lvl;
    return command_xcam_control("printing_monitor", on_off, lvl);
}

// refine printer function options
int MachineObject::command_xcam_control_spaghetti_detection(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false : true;

    xcam_spaghetti_detection       = on_off;
    xcam_ai_monitoring_hold_start  = time(nullptr);
    xcam_spaghetti_detection_sensitivity = lvl;
    return command_xcam_control("spaghetti_detector", on_off, lvl);
}

int MachineObject::command_xcam_control_purgechutepileup_detection(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false : true;

    xcam_purgechutepileup_detection = on_off;
    xcam_ai_monitoring_hold_start  = time(nullptr);
    xcam_purgechutepileup_detection_sensitivity = lvl;
    return command_xcam_control("pileup_detector", on_off, lvl);
}

int MachineObject::command_xcam_control_nozzleclumping_detection(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false : true;

    xcam_nozzleclumping_detection  = on_off;
    xcam_ai_monitoring_hold_start  = time(nullptr);
    xcam_nozzleclumping_detection_sensitivity = lvl;
    return command_xcam_control("clump_detector", on_off, lvl);
}

int MachineObject::command_xcam_control_airprinting_detection(bool on_off, std::string lvl)
{
    bool print_halt = (lvl == "never_halt") ? false : true;

    xcam_airprinting_detection     = on_off;
    xcam_ai_monitoring_hold_start  = time(nullptr);
    xcam_airprinting_detection_sensitivity = lvl;
    return command_xcam_control("airprint_detector", on_off, lvl);
}

int MachineObject::command_xcam_control_buildplate_marker_detector(bool on_off)
{
    xcam_buildplate_marker_detector = on_off;
    xcam_buildplate_marker_hold_start = time(nullptr);
    return command_xcam_control("buildplate_marker_detector", on_off);
}

int MachineObject::command_xcam_control_first_layer_inspector(bool on_off, bool print_halt)
{
    xcam_first_layer_inspector = on_off;
    xcam_first_layer_hold_start = time(nullptr);
    return command_xcam_control("first_layer_inspector", on_off);
}

int MachineObject::command_xcam_control_auto_recovery_step_loss(bool on_off)
{
    xcam_auto_recovery_step_loss = on_off;
    xcam_auto_recovery_hold_start = time(nullptr);
    return command_set_printing_option(on_off);
}

int MachineObject::command_xcam_control_allow_prompt_sound(bool on_off)
{
    xcam_allow_prompt_sound = on_off;
    xcam_prompt_sound_hold_start = time(nullptr);
    return command_set_prompt_sound(on_off);
}

int MachineObject::command_xcam_control_filament_tangle_detect(bool on_off)
{
    xcam_filament_tangle_detect = on_off;
    xcam_filament_tangle_detect_hold_start = time(nullptr);
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

bool MachineObject::is_in_printing_pause() const
{
    return print_status == "PAUSE";
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
    subscribe_counter = SUBSCRIBE_RETRY_COUNT;
}

void MachineObject::reset()
{
    BOOST_LOG_TRIVIAL(trace) << "reset dev_id=" << dev_id;
    last_update_time = std::chrono::system_clock::now();
    subscribe_counter = SUBSCRIBE_RETRY_COUNT;
    m_push_count = 0;
    m_full_msg_count = 0;
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
    network_wired = false;
    dev_connection_name = "";
    job_id_ = "";
    jobState_ = 0;
    m_plate_index = -1;
    device_cert_installed = false;

    // reset print_json
    json empty_j;
    print_json.diff2all_base_reset(empty_j);

    for (auto i = 0; i < vt_slot.size(); i++) {
        vt_slot[i].reset();

        if (i == 1) {
            vt_slot.erase(vt_slot.begin() + 1);
        }
    }
    subtask_ = nullptr;
    has_extra_flow_type = false;
    m_partskip_ids.clear();
}

void MachineObject::set_print_state(std::string status)
{
    print_status = status;
}

int MachineObject::connect(bool use_openssl)
{
    if (get_dev_ip().empty()) return -1;
    std::string username = "bblp";
    std::string password = get_access_code();

    if (m_agent) {
        try {
            return m_agent->connect_printer(get_dev_id(), get_dev_ip(), username, password, use_openssl);
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

bool MachineObject::is_info_ready(bool check_version) const
{
    if (check_version && module_vers.empty())
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": not ready, failed to check version";
        return false;
    }

    std::chrono::system_clock::time_point curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::microseconds>(last_push_time - curr_time);
    if (m_full_msg_count > 0 && m_push_count > 0 && diff.count() < PUSHINFO_TIMEOUT) {
        return true;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__
        << ": not ready, m_full_msg_count=" << m_full_msg_count
        << ", m_push_count=" << m_push_count
        << ", diff.count()=" << diff.count()
        << ", dev_id=" << dev_id;
    return false;
}


bool MachineObject::is_security_control_ready() const
{
    return device_cert_installed;
}

std::vector<std::string> MachineObject::get_resolution_supported()
{
    return camera_resolution_supported;
}

std::vector<std::string> MachineObject::get_compatible_machine()
{
    return DevPrinterConfigUtil::get_compatible_machine(printer_type);
}

bool MachineObject::is_camera_busy_off()
{
    if (get_printer_series() == PrinterSeries::SERIES_P1P)
        return is_in_prepare() || is_in_upgrading();
    return false;
}

int MachineObject::publish_json(const json& json_item, int qos, int flag)
{
    int rtn = 0;
    if (is_lan_mode_printer()) {
        rtn = local_publish_json(json_item.dump(), qos, flag);
    } else {
        rtn = cloud_publish_json(json_item.dump(), qos, flag);
    }

    if (rtn == 0) {
        BOOST_LOG_TRIVIAL(info) << "publish_json: " << json_item.dump() << " code: " << rtn;
    } else {
        BOOST_LOG_TRIVIAL(error) << "publish_json: " << json_item.dump() << " code: " << rtn;
    }

    return rtn;
}

int MachineObject::cloud_publish_json(std::string json_str, int qos, int flag)
{
    int result = -1;
    if (m_agent)
        result = m_agent->send_message(get_dev_id(), json_str, qos, flag);

    return result;
}

int MachineObject::local_publish_json(std::string json_str, int qos, int flag)
{
    int result = -1;
    if (m_agent) {
        result = m_agent->send_message_to_printer(get_dev_id(), json_str, qos, flag);
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

int MachineObject::parse_json(std::string tunnel, std::string payload, bool key_field_only)
{
#ifdef ORCA_NETWORK_DEBUG
    BOOST_LOG_TRIVIAL(info) << "parse_json: payload = " << payload;
    flush_logs();
#endif

    if (tunnel == "lan") last_lan_msg_time_ = std::chrono::system_clock::now();
    if (tunnel == "cloud") last_cloud_msg_time_ = std::chrono::system_clock::now();

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
                            m_full_msg_count++;

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
                        if (!printer_type.empty()) {
                            m_full_msg_count++;/* all message package is full at LAN mode*/
                            print_json.load_compatible_settings(printer_type, "");
                        }

                        print_json.diff2all_base_reset(j_pre);
                    }

                    if (j_pre["print"].contains("s_obj")){
                        if(j_pre["print"]["s_obj"].is_array()){
                            m_partskip_ids.clear();
                            for(auto it=j_pre["print"]["s_obj"].begin(); it!=j_pre["print"]["s_obj"].end(); it++){
                                m_partskip_ids.push_back(it.value().get<int>());
                            }
                        }
                    }
                }
            }
            if (j_pre["print"].contains("plate_idx")){ // && m_plate_index == -1
                if (j_pre["print"]["plate_idx"].is_number())
                {
                    m_plate_index = j_pre["print"]["plate_idx"].get<int>();
                }
                else if (j_pre["print"]["plate_idx"].is_string())
                {
                    try
                    {
                        m_plate_index = std::stoi(j_pre["print"]["plate_idx"].get<std::string>());
                    }
                    catch (...) { BOOST_LOG_TRIVIAL(error) << "parse_json: failed to convert plate_idx to int"; }
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

                    message_delay.clear();
                    message_delay.shrink_to_fit();
                }
            }
            message_delay.push_back(std::make_tuple(message_type, t_utc, delay));
        }
        else
            last_utc_time = last_update_time;

#if !BBL_RELEASE_TO_PUBLIC
        BOOST_LOG_TRIVIAL(info) << "parse_json: dev_id=" << dev_id << ", tunnel is=" << tunnel << ", merged playload=" << j.dump();
#else
        if (Slic3r::get_logging_level() < level_string_to_boost("trace")) {
            BOOST_LOG_TRIVIAL(info) << "parse_json: dev_id=" << dev_id << ", origin playload=" << j_pre.dump();
        } else {
            BOOST_LOG_TRIVIAL(trace) << "parse_json: dev_id=" << dev_id << ", tunnel is=" << tunnel << ", merged playload=" << j.dump();
        }
#endif

        // Parse version info first, as if version arrive or change, 'print' need parse again with new compatible settings
        try {
            if (j.contains("info")) {
                if (j["info"].contains("command") && j["info"]["command"].get<std::string>() == "get_version") {
                    json j_module = j["info"]["module"];
                    clear_version_info();
                    for (auto it = j_module.begin(); it != j_module.end(); it++) {
                        DevFirmwareVersionInfo ver_info;
                        ver_info.name = (*it)["name"].get<std::string>();
                        if ((*it).contains("product_name"))
                            ver_info.product_name = wxString::FromUTF8((*it)["product_name"].get<string>());
                        if ((*it).contains("sw_ver"))
                            ver_info.sw_ver = (*it)["sw_ver"].get<std::string>();
                        if ((*it).contains("sw_new_ver"))
                            ver_info.sw_new_ver = (*it)["sw_new_ver"].get<std::string>();
                        if ((*it).contains("visible") && (*it).contains("new_ver")) {
                            ver_info.sw_new_ver = (*it)["new_ver"].get<std::string>();
                        }
                        if ((*it).contains("sn"))
                            ver_info.sn = (*it)["sn"].get<std::string>();
                        if ((*it).contains("hw_ver"))
                            ver_info.hw_ver = (*it)["hw_ver"].get<std::string>();
                        if((*it).contains("flag"))
                            ver_info.firmware_flag= (*it)["flag"].get<int>();

                        store_version_info(ver_info);
                        if (ver_info.name == "ota") {
                            NetworkAgent* agent = GUI::wxGetApp().getAgent();
                            if (agent) {
                                std::string dev_ota_str = "dev_ota_ver:" + this->get_dev_id();
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

        try {
            if (auto ptr = m_fila_system->GetAmsFirmwareSwitch().lock()) {
                ptr->ParseFirmwareSwitch(j);
            }
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "parse_json: failed to parse firmware switch info";
        }

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
                if (!m_manager->IsMultiMachineEnabled() && !is_support_agora) {
                    if (jj.contains("support_tunnel_mqtt")) {
                        if (jj["support_tunnel_mqtt"].is_boolean()) {
                            is_support_tunnel_mqtt = jj["support_tunnel_mqtt"].get<bool>();
                        }
                    }
                }

                //nozzle temp range
                if (jj.contains("nozzle_temp_range")) {
                    if (jj["nozzle_temp_range"].is_array()) {
                        nozzle_temp_range.clear();
                        for (auto it = jj["nozzle_temp_range"].begin(); it != jj["nozzle_temp_range"].end(); it++) {
                            nozzle_temp_range.push_back(it.value().get<int>());
                        }
                    }
                }

                // bed temp range
                if (jj.contains("bed_temp_range")) {
                    if (jj["bed_temp_range"].is_array()) {
                        bed_temp_range.clear();
                        for (auto it = jj["bed_temp_range"].begin(); it != jj["bed_temp_range"].end(); it++) {
                            bed_temp_range.push_back(it.value().get<int>());
                        }
                    }
                }

                //supported function
                m_config->ParseConfig(jj);

                if (jj.contains("support_build_plate_marker_detect")) {
                    if (jj["support_build_plate_marker_detect"].is_boolean()) {
                        is_support_build_plate_marker_detect = jj["support_build_plate_marker_detect"].get<bool>();
                    }
                }

                if(jj.contains("support_build_plate_marker_detect_type") && jj["support_build_plate_marker_detect_type"].is_number()) {
                    m_plate_maker_detect_type = (PlateMakerDectect)jj["support_build_plate_marker_detect_type"].get<int>();
                }

                if (jj.contains("support_flow_calibration") && jj["support_flow_calibration"].is_boolean())
                {
                    is_support_pa_calibration = jj["support_flow_calibration"].get<bool>();
                }

                if (jj.contains("support_send_to_sd")) {
                    if (jj["support_send_to_sd"].is_boolean()) {
                        is_support_send_to_sdcard = jj["support_send_to_sd"].get<bool>();
                    }
                }

              m_fan->ParseV2_0(jj);

                if (jj.contains("support_filament_backup")) {
                    if (jj["support_filament_backup"].is_boolean()) {
                        is_support_filament_backup = jj["support_filament_backup"].get<bool>();
                    }
                }

                if (jj.contains("support_update_remain")) {
                    if (jj["support_update_remain"].is_boolean()) {
                        is_support_update_remain = jj["support_update_remain"].get<bool>();
                        if (auto ptr = m_fila_system->GetAmsFirmwareSwitch().lock();
                            ptr->GetCurrentFirmwareIdxRun() == DevAmsSystemFirmwareSwitch::IDX_AMS_AMS2_AMSHT) {
                            is_support_update_remain = true;// special case, for the firmware, remain is supported
                        }
                    }
                }

                if (jj.contains("support_bed_leveling")) {
                    if (jj["support_bed_leveling"].is_number_integer()) {
                        is_support_bed_leveling = jj["support_bed_leveling"].get<int>();
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

                if (jj.contains("bed_temperature_limit")) {
                    if (jj["bed_temperature_limit"].is_number_integer()) {
                        bed_temperature_limit = jj["bed_temperature_limit"].get<int>();
                    }
                }

                if (jj.contains("support_refresh_nozzle")) {
                    if (jj["support_refresh_nozzle"].is_boolean()) {
                        is_support_refresh_nozzle = jj["support_refresh_nozzle"].get<bool>();
                    }
                }
            }


            if (jj.contains("command")) {
                if (jj["command"].get<std::string>() == "ams_change_filament") {
                    if (jj.contains("errno")) {
                        if (jj["errno"].is_number()) {
                            if (jj.contains("soft_temp")) {
                                int soft_temp = jj["soft_temp"].get<int>();
                                if (jj["errno"].get<int>() == -2) {
                                    wxString text = wxString::Format(_L("The chamber temperature is too high, which may cause the filament to soften. Please wait until the chamber temperature drops below %d\u2103. You may open the front door or enable fans to cool down."), soft_temp);
                                    GUI::wxGetApp().push_notification(this, text);
                                } else if (jj["errno"].get<int>() == -4) {
                                    wxString text = wxString::Format(_L("AMS temperature is too high, which may cause the filament to soften. Please wait until the AMS temperature drops below %d\u2103."), soft_temp);
                                    GUI::wxGetApp().push_notification(this, text);
                                }
                            } else {
                                if (jj["errno"].get<int>() == -2) {
                                    wxString text = _L("The current chamber temperature or the target chamber temperature exceeds 45\u2103. In order to avoid extruder clogging, low temperature filament(PLA/PETG/TPU) is not allowed to be loaded.");
                                    GUI::wxGetApp().push_notification(this, text);
                                }
                            }
                        }
                    }
                }

                if (jj["command"].get<std::string>() == "set_ctt") {
                    if (m_agent && is_studio_cmd(sequence_id)) {
                        if (jj["errno"].is_number()) {
                            wxString text;
                            if (jj["errno"].get<int>() == -2) {
                                 text = _L("Low temperature filament(PLA/PETG/TPU) is loaded in the extruder. In order to avoid extruder clogging, it is not allowed to set the chamber temperature.");
                            }
                            else if (jj["errno"].get<int>() == -4) {
                                 text = _L("When you set the chamber temperature below 40\u2103, the chamber temperature control will not be activated, "
                                           "and the target chamber temperature will automatically be set to 0\u2103." /* 0°C */);
                            }
                            if(!text.empty()){
#if __WXOSX__
                            set_ctt_dlg(text);
#else
                            GUI::wxGetApp().push_notification(this, text);
#endif
                            }
                        }
                    }
                }

                if (!key_field_only)
                {
                    if (is_studio_cmd(sequence_id) && jj.contains("command") && jj.contains("err_code"))
                    {
                        if (jj["err_code"].is_number())
                        {
                            /* proceed action*/
                            json action_json = jj.contains("err_index") ? jj : json();

                            add_command_error_code_dlg(jj["err_code"].get<int>(), action_json);
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

                     DevStorage::ParseV1_0(jj, m_storage);

                    if (!key_field_only) {
                        if (jj.contains("home_flag")) { parse_home_flag(jj["home_flag"].get<int>());}

                        /*the param is invalid in np for Yeshu*/
                        if (jj.contains("hw_switch_state")) {
                            hw_switch_state = jj["hw_switch_state"].get<int>();
                            m_extder_system->m_extders[MAIN_EXTRUDER_ID].m_ext_has_filament = hw_switch_state;
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
                            if (jj["net"].contains("info")) {
                                for (auto info_item = jj["net"]["info"].begin(); info_item != jj["net"]["info"].end(); info_item++) {

                                    if (info_item->contains("ip")) {
                                        auto tmp_dev_ip = (*info_item)["ip"].get<int64_t>();
                                        if (tmp_dev_ip == 0)
                                            continue ;
                                        else {
                                           set_dev_ip(DevUtil::convertToIp(tmp_dev_ip));
                                        }
                                    } else {
                                        break;
                                    }
                                }
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
                        this->job_id_ = DevJsonValParser::get_longlong_val(jj["job_id"]);
                    }
                    else {
                        is_support_wait_sending_finish = false;
                    }

                    if (jj.contains("subtask_name")) {
                        subtask_name = jj["subtask_name"].get<std::string>();
                    }

                    if (!key_field_only) {
                        if (jj.contains("printer_type")) {
                            printer_type = _parse_printer_type(jj["printer_type"].get<std::string>());
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

                        if (jj.contains("job_attr")) {
                            int jobAttr = jj["job_attr"].get<int>();
                            jobState_ =  get_flag_bits(jobAttr, 4, 4);
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

                        DevBed::ParseV1_0(jj,m_bed);

                        if (jj.contains("frame_temper")) {
                            if (jj["frame_temper"].is_number()) {
                                frame_temp = jj["frame_temper"].get<float>();
                            }
                        }

                        ExtderSystemParser::ParseV1_0(jj, m_extder_system);

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
                        m_fan->ParseV1_0(jj);

                        /* parse speed */
                        DevPrintOptionsParser::Parse(m_print_options, jj);

                        try {
                            if (jj.contains("spd_mag")) {
                                printing_speed_mag = jj["spd_mag"].get<int>();
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

                        if (jj.contains("stg_cd")) {
                            stage_remaining_seconds = jj["stg_cd"].get<int>();
                        }
                    }
                    catch (...) {
                        ;
                    }

                    if (!key_field_only) {
                        /*get filam_bak*/
                        try {
                            m_extder_system->m_extders[MAIN_EXTRUDER_ID].m_filam_bak.clear();

                            if (jj.contains("filam_bak")) {
                                if (jj["filam_bak"].is_array()) {
                                    for (auto it = jj["filam_bak"].begin(); it != jj["filam_bak"].end(); it++) {
                                        const auto& filam_bak_val = it.value().get<int>();
                                        m_extder_system->m_extders[MAIN_EXTRUDER_ID].m_filam_bak.push_back(filam_bak_val);
                                    }
                                }
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
                                    {
                                        m_lamp->SetChamberLight((*it)["mode"].get<std::string>());
                                    }
                                }
                            }
                        }
                        catch (...) {
                            ;
                        }
                    }
#pragma endregion
                    if (!key_field_only) {

                        if (jj.contains("nozzle_diameter") && jj.contains("nozzle_type"))
                        {
                            std::optional<int> flag_e3d;
                            if (jj.contains("flag3")) {
                                int flag3           = jj["flag3"].get<int>();
                                flag_e3d            = std::make_optional(get_flag_bits(flag3, 10, 3));
                                has_extra_flow_type = true;
                            }

                            DevNozzleSystemParser::ParseV1_0(jj["nozzle_type"], jj["nozzle_diameter"], m_nozzle_system, flag_e3d);
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
                                if ((int)upgrade_display_state != jj["upgrade_state"]["dis_state"].get<int>()
                                    && jj["upgrade_state"]["dis_state"].get<int>() == 3) {
                                    GUI::wxGetApp().CallAfter([this] {
                                        this->command_get_version();
                                        });
                                }
                                if (upgrade_display_hold_count > 0)
                                {
                                    upgrade_display_hold_count--;
                                }
                                else
                                {
                                    upgrade_display_state = (DevFirmwareUpgradingState)jj["upgrade_state"]["dis_state"].get<int>();
                                    if ((upgrade_display_state == DevFirmwareUpgradingState::UpgradingAvaliable) && is_lan_mode_printer())
                                    {
                                        upgrade_display_state = DevFirmwareUpgradingState::UpgradingUnavaliable;
                                    }
                                }
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
                                        upgrade_display_state = DevFirmwareUpgradingState::UpgradingInProgress;
                                    }
                                    else if (upgrade_status == "UPGRADE_SUCCESS"
                                        || upgrade_status == "DOWNLOAD_FAIL"
                                        || upgrade_status == "FLASH_FAIL"
                                        || upgrade_status == "PRE_FLASH_FAIL"
                                        || upgrade_status == "UPGRADE_FAIL") {
                                        upgrade_display_state = DevFirmwareUpgradingState::UpgradingFinished;
                                    }
                                    else {
                                        if (upgrade_new_version) {
                                            upgrade_display_state = DevFirmwareUpgradingState::UpgradingAvaliable;
                                        }
                                        else {
                                            upgrade_display_state = DevFirmwareUpgradingState::UpgradingUnavaliable;
                                        }
                                    }
                                }
                            }
                            // new ver list
                            if (jj["upgrade_state"].contains("new_ver_list")) {
                                m_new_ver_list_exist = true;
                                new_ver_list.clear();
                                for (auto ver_item = jj["upgrade_state"]["new_ver_list"].begin(); ver_item != jj["upgrade_state"]["new_ver_list"].end(); ver_item++) {
                                    DevFirmwareVersionInfo ver_info;
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
                                    if (time(nullptr) - camera_recording_ctl_start > HOLD_TIME_3SEC) {
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
                                if (time(nullptr) - xcam_ai_monitoring_hold_start > HOLD_TIME_3SEC) {

                                    if (jj["xcam"].contains("cfg")) {
                                        xcam_disable_ai_detection_display = true;
                                       //  std::string cfg    = jj["xcam"]["cfg"].get<std::string>();

                                        int cfg                  = jj["xcam"]["cfg"].get<int>();
                                         xcam_spaghetti_detection = get_flag_bits(cfg,7);
                                         switch (get_flag_bits(cfg, 8, 2)) {
                                             case 0: xcam_spaghetti_detection_sensitivity = "low"; break;
                                             case 1: xcam_spaghetti_detection_sensitivity = "medium"; break;
                                             case 2: xcam_spaghetti_detection_sensitivity = "high"; break;
                                             default: break;
                                         }

                                         xcam_purgechutepileup_detection = get_flag_bits(cfg, 10);
                                         switch (get_flag_bits(cfg, 11, 2)) {

                                         case 0: xcam_purgechutepileup_detection_sensitivity = "low"; break;
                                         case 1: xcam_purgechutepileup_detection_sensitivity = "medium"; break;
                                         case 2: xcam_purgechutepileup_detection_sensitivity = "high"; break;
                                         default: break;
                                         }

                                         xcam_nozzleclumping_detection = get_flag_bits(cfg, 13);
                                         switch (get_flag_bits(cfg, 14, 2)) {

                                         case 0: xcam_nozzleclumping_detection_sensitivity = "low"; break;
                                         case 1: xcam_nozzleclumping_detection_sensitivity = "medium"; break;
                                         case 2: xcam_nozzleclumping_detection_sensitivity = "high"; break;
                                         default: break;
                                         }

                                         xcam_airprinting_detection    = get_flag_bits(cfg, 16);
                                         switch (get_flag_bits(cfg, 17, 2)) {

                                         case 0: xcam_airprinting_detection_sensitivity = "low"; break;
                                         case 1: xcam_airprinting_detection_sensitivity = "medium"; break;
                                         case 2: xcam_airprinting_detection_sensitivity = "high"; break;
                                         default: break;
                                         }

                                    }
                                    else if (jj["xcam"].contains("printing_monitor")) {
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

                                if (time(nullptr) - xcam_first_layer_hold_start > HOLD_TIME_3SEC) {
                                    if (jj["xcam"].contains("first_layer_inspector")) {
                                        xcam_first_layer_inspector = jj["xcam"]["first_layer_inspector"].get<bool>();
                                    }
                                }

                                if (time(nullptr) - xcam_buildplate_marker_hold_start > HOLD_TIME_3SEC) {
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
                    if (!key_field_only && jj.contains("hms")) {
                        m_hms_system->ParseHMSItems(jj["hms"]);
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
                        DevFilaSystemParser::ParseV1_0(jj, this, m_fila_system, key_field_only);
                    }

                    /* vitrual tray*/
                    if (!key_field_only) {
                        try {
                            if (jj.contains("vir_slot") && jj["vir_slot"].is_array()) {

                                for (auto it = jj["vir_slot"].begin(); it != jj["vir_slot"].end(); it++) {
                                    auto vslot = parse_vt_tray(it.value().get<json>());

                                    if (vslot.id == std::to_string(VIRTUAL_TRAY_MAIN_ID)) {
                                        auto it = std::next(vt_slot.begin(), 0);
                                        if (it != vt_slot.end()) {
                                            vt_slot[0] = vslot;
                                        }
                                        else {
                                            vt_slot.push_back(vslot);
                                        }
                                    }
                                    else if (vslot.id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                                        auto it = std::next(vt_slot.begin(), 1);
                                        if (it != vt_slot.end()) {
                                            vt_slot[1] = vslot;
                                        }
                                        else {
                                            vt_slot.push_back(vslot);
                                        }
                                    }
                                }

                            }
                            else if (jj.contains("vt_tray")) {
                                auto main_slot = parse_vt_tray(jj["vt_tray"].get<json>());
                                main_slot.id = std::to_string(VIRTUAL_TRAY_MAIN_ID);


                                auto it = std::next(vt_slot.begin(), 0);
                                if (it != vt_slot.end()) {
                                    vt_slot[0] = main_slot;
                                }
                                else {
                                    vt_slot.push_back(main_slot);
                                }
                            }
                            else {
                                ams_support_virtual_tray = false;
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
                            this->job_id_ = DevJsonValParser::get_longlong_val(jj["job_id"]);
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
                            GUI::wxGetApp().push_notification(this, text);
                        }
                    }
                } else if (jj["command"].get<std::string>() == "ams_filament_setting" && !key_field_only) {
                    // BBS trigger ams UI update
                    ams_version = -1;

                    if (jj["ams_id"].is_number()) {
                        int ams_id = jj["ams_id"].get<int>();
                        int tray_id = 0;
                        if (jj.contains("tray_id")) {
                            tray_id = jj["tray_id"].get<int>();
                        }
                        if (ams_id == 255 && tray_id == VIRTUAL_TRAY_MAIN_ID) {
                            BOOST_LOG_TRIVIAL(info) << "ams_filament_setting, parse tray info";
                            vt_slot[0].nozzle_temp_max = std::to_string(jj["nozzle_temp_max"].get<int>());
                            vt_slot[0].nozzle_temp_min = std::to_string(jj["nozzle_temp_min"].get<int>());
                            vt_slot[0].color = jj["tray_color"].get<std::string>();
                            vt_slot[0].setting_id = jj["tray_info_idx"].get<std::string>();
                            //vt_tray.type = jj["tray_type"].get<std::string>();
                            vt_slot[0].m_fila_type = setting_id_to_type(vt_slot[0].setting_id, jj["tray_type"].get<std::string>());
                            // delay update
                            vt_slot[0].set_hold_count();
                        } else {
                            auto ams = m_fila_system->GetAmsById(std::to_string(ams_id));
                            if (ams) {
                                tray_id = jj["tray_id"].get<int>();
                                auto tray_it = ams->GetTrays().find(std::to_string(tray_id));
                                if (tray_it != ams->GetTrays().end()) {
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
                                    tray_it->second->m_fila_type = setting_id_to_type(tray_it->second->setting_id, jj["tray_type"].get<std::string>());
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
                                if (time(nullptr) - xcam_first_layer_hold_start > HOLD_TIME_3SEC) {
                                    xcam_first_layer_inspector = enable;
                                }
                            }
                            else if (jj["module_name"].get<std::string>() == "buildplate_marker_detector") {
                                if (time(nullptr) - xcam_buildplate_marker_hold_start > HOLD_TIME_3SEC) {
                                    xcam_buildplate_marker_detector = enable;
                                }
                            }
                            else if (jj["module_name"].get<std::string>() == "printing_monitor") {
                                if (time(nullptr) - xcam_ai_monitoring_hold_start > HOLD_TIME_3SEC) {
                                    xcam_ai_monitoring = enable;
                                    if (jj.contains("halt_print_sensitivity")) {
                                        xcam_ai_monitoring_sensitivity = jj["halt_print_sensitivity"].get<std::string>();
                                    }
                                }
                            }
                            else if (jj["module_name"].get<std::string>() == "spaghetti_detector") {
                                if (time(nullptr) - xcam_ai_monitoring_hold_start > HOLD_TIME_3SEC) {
                                    // old protocol
                                    xcam_ai_monitoring = enable;
                                    if (jj.contains("print_halt")) {
                                        if (jj["print_halt"].get<bool>()) {
                                            xcam_ai_monitoring_sensitivity = "medium";
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if(jj["command"].get<std::string>() == "print_option") {
                     try {
                          if (jj.contains("option")) {
                              if (jj["option"].is_number()) {
                                  int option = jj["option"].get<int>();
                                  if (time(nullptr) - xcam_auto_recovery_hold_start > HOLD_TIME_3SEC) {
                                      xcam_auto_recovery_step_loss = ((option >> (int)PRINT_OP_AUTO_RECOVERY) & 0x01) != 0;
                                  }
                              }
                          }

                          if (time(nullptr) - xcam_auto_recovery_hold_start > HOLD_TIME_3SEC) {
                              if (jj.contains("auto_recovery")) {
                                  xcam_auto_recovery_step_loss = jj["auto_recovery"].get<bool>();
                              }
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
                            GUI::wxGetApp().push_notification(this, info, _L("Calibration error"), UserNotificationStyle::UNS_WARNING_CONFIRM);
                            BOOST_LOG_TRIVIAL(info) << cali_mode << " result fail, reason = " << reason;
                        }
                    }
                } else if (jj["command"].get<std::string>() == "extrusion_cali_set") {
                    int ams_id = -1;
                    int tray_id = -1;
                    int curr_tray_id = -1;
                    if (jj.contains("tray_id")) {
                        try {
                            curr_tray_id = jj["tray_id"].get<int>();
                            if (curr_tray_id == VIRTUAL_TRAY_MAIN_ID)
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
                    if (tray_id == VIRTUAL_TRAY_MAIN_ID) {
                        if (jj.contains("k_value"))
                            vt_slot[0].k = jj["k_value"].get<float>();
                        if (jj.contains("n_coef"))
                            vt_slot[0].n = jj["n_coef"].get<float>();
                    } else {

                        auto ams_item = m_fila_system->GetAmsById(std::to_string(ams_id));
                        if (ams_item) {
                            auto tray_item = ams_item->GetTrays().find(std::to_string(tray_id));
                            if (tray_item != ams_item->GetTrays().end()) {
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
                    int ams_id       = -1;
                    int slot_id      = -1;
                    int tray_id      = -1;

                    if (jj.contains("ams_id")) {
                        try {
                            ams_id  = jj["ams_id"].get<int>();
                            slot_id = jj["slot_id"].get<int>();
                        } catch (...) {
                            ;
                        }
                    }
                    else {
                        tray_id = jj["tray_id"].get<int>();
                        if(tray_id >= 0 && tray_id < 16)
                        {
                            ams_id  = tray_id / 4;
                            slot_id = tray_id % 4;
                        }
                        else if(tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID){
                            ams_id  = tray_id;
                            slot_id = 0;
                        }
                    }

                    BOOST_LOG_TRIVIAL(trace) << "extrusion_cali_sel: unsupported ams_id = " << ams_id << "slot_id = " << slot_id;

                    if (jj.contains("cali_idx")) {
                        if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID) {

                            if (ams_id == VIRTUAL_TRAY_MAIN_ID && vt_slot.size() > 0) {

                                vt_slot[MAIN_EXTRUDER_ID].cali_idx = jj["cali_idx"].get<int>();
                                vt_slot[MAIN_EXTRUDER_ID].set_hold_count();

                            } else if (ams_id == VIRTUAL_TRAY_DEPUTY_ID && vt_slot.size() > 1) {

                                vt_slot[DEPUTY_EXTRUDER_ID].cali_idx = jj["cali_idx"].get<int>();
                                vt_slot[DEPUTY_EXTRUDER_ID].set_hold_count();

                            }

                        }
                        else {
                            auto tray_item = m_fila_system->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
                            if (tray_item)
                            {
                                tray_item->cali_idx = jj["cali_idx"].get<int>();
                                tray_item->set_hold_count();
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
                                pa_calib_result.filament_id = (*it)["filament_id"].get<std::string>();

                                if ((*it).contains("setting_id")) {
                                    pa_calib_result.setting_id  = (*it)["setting_id"].get<std::string>();
                                }

                                // old
                                if (jj["nozzle_diameter"].is_number_float()) {
                                    pa_calib_result.nozzle_diameter = jj["nozzle_diameter"].get<float>();
                                } else if (jj["nozzle_diameter"].is_string()) {
                                    pa_calib_result.nozzle_diameter = string_to_float(jj["nozzle_diameter"].get<std::string>());
                                }

                                // new: should get nozzle diameter from filament item
                                if ((*it).contains("setting_id")) {
                                    if ((*it)["nozzle_diameter"].is_number_float()) {
                                        pa_calib_result.nozzle_diameter = (*it)["nozzle_diameter"].get<float>();
                                    } else if ((*it)["nozzle_diameter"].is_string()) {
                                        pa_calib_result.nozzle_diameter = string_to_float((*it)["nozzle_diameter"].get<std::string>());
                                    }
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
                                    pa_calib_result.nozzle_volume_type = NozzleVolumeType::nvtStandard;
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

                                if (this->is_support_new_auto_cali_method)
                                    pa_calib_result.tray_id = get_tray_id_by_ams_id_and_slot_id(pa_calib_result.ams_id, pa_calib_result.slot_id);
                                else
                                    pa_calib_result.tray_id = (*it)["tray_id"].get<int>();

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
               m_fan->command_handle_response(jj);
            }
        }

        if (!key_field_only) {
            try {
                if (j.contains("camera")) {
                    if (j["camera"].contains("command")) {
                        if (j["camera"]["command"].get<std::string>() == "ipcam_timelapse") {
                            if (camera_timelapse_hold_count > 0) {
                                camera_timelapse_hold_count--;
                            }
                            else
                            {
                                if (j["camera"]["control"].get<std::string>() == "enable") this->camera_timelapse = true;
                                if (j["camera"]["control"].get<std::string>() == "disable") this->camera_timelapse = false;
                                BOOST_LOG_TRIVIAL(info) << "ack of timelapse = " << camera_timelapse;
                            }
                        } else if (j["camera"]["command"].get<std::string>() == "ipcam_record_set") {
                            if (time(nullptr) - camera_recording_ctl_start > HOLD_TIME_3SEC) {
                                if (j["camera"]["control"].get<std::string>() == "enable") this->camera_recording_when_printing = true;
                                if (j["camera"]["control"].get<std::string>() == "disable") this->camera_recording_when_printing = false;
                                BOOST_LOG_TRIVIAL(info) << "ack of ipcam_record_set " << camera_recording_when_printing;
                            }
                        } else if (j["camera"]["command"].get<std::string>() == "ipcam_resolution_set") {
                            if (camera_resolution_hold_count > 0) {
                                camera_resolution_hold_count--;
                            }
                            else
                            {
                                this->camera_resolution = j["camera"]["resolution"].get<std::string>();
                                BOOST_LOG_TRIVIAL(info) << "ack of resolution = " << camera_resolution;
                            }
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
                            this->upgrade_display_state = DevFirmwareUpgradingState::UpgradingInProgress;
                            upgrade_display_hold_count = HOLD_COUNT_MAX;
                            BOOST_LOG_TRIVIAL(info) << "ack of upgrade_confirm";
                        }

                        bool check_studio_cmd = true;
                        if (j["upgrade"].contains("sequence_id")) {
                            try
                            {
                                std::string str_seq = j["upgrade"]["sequence_id"].get<std::string>();
                                check_studio_cmd = is_studio_cmd(stoi(str_seq));
                            }
                            catch (...) { }
                        }

                        if (check_studio_cmd && j["upgrade"].contains("err_code")) {
                            if (j["upgrade"]["err_code"].is_number()) {
                                add_command_error_code_dlg(j["upgrade"]["err_code"].get<int>());
                            }
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
            BOOST_LOG_TRIVIAL(trace) << "parse_json  m_active_state =" << m_active_state;
            parse_state_changed_event();
        }
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(trace) << "parse_json failed! dev_id=" << this->get_dev_id() <<", payload = " << payload;
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
        auto print_error_dlg = new GUI::SecondaryCheckDialog(nullptr, wxID_ANY, _L("Warning"), GUI::SecondaryCheckDialog::VisibleButtons::ONLY_CONFIRM); // ORCA VisibleButtons instead ButtonStyle 
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

    return publish_json(j);
}

void MachineObject::update_device_cert_state(bool ready)
{
    device_cert_installed = ready;
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
            auto rating_info = new DevPrintTaskRatingInfo();
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

void MachineObject::free_slice_info()
{
    if (get_slice_info_thread)
    {
        if (get_slice_info_thread->joinable())
        {
            get_slice_info_thread->interrupt();
            get_slice_info_thread->join();
        }

        delete get_slice_info_thread;
        get_slice_info_thread = nullptr;
    }

    if (slice_info)
    {
        delete slice_info;
        slice_info = nullptr;
    }
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
            if (!slice_info) return;
            if (!get_slice_info_thread) return;/*STUDIO-12264*/
            if (get_slice_info_thread->interruption_requested()) { return;}

            if (plate_idx >= 0) {
                plate_index = plate_idx;
                this->m_plate_index = plate_idx;
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
            // this->m_plate_index = plate_index;
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
            result = m_agent->get_printer_firmware(get_dev_id(), &http_code, &http_body);
            if (result < 0) {
                // get upgrade list failed
                return;
            }
            try {
                json j = json::parse(http_body);
                if (j.contains("devices") && !j["devices"].is_null()) {
                    firmware_list.clear();
                    for (json::iterator it = j["devices"].begin(); it != j["devices"].end(); it++) {
                        if ((*it)["dev_id"].get<std::string>() == this->get_dev_id()) {
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


DevAmsTray MachineObject::parse_vt_tray(json vtray)
{
    auto vt_tray = DevAmsTray(std::to_string(VIRTUAL_TRAY_MAIN_ID));

    if (vtray.contains("id"))
        vt_tray.id = vtray["id"].get<std::string>();
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - extrusion_cali_set_hold_start);
    if (diff.count() > HOLD_TIMEOUT || diff.count() < 0
        || extrusion_cali_set_tray_id != VIRTUAL_TRAY_MAIN_ID) {
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
                vt_tray.m_fila_type = "PLA-S";
            }
            else if (vt_tray.setting_id == "GFS01") {
                vt_tray.m_fila_type = "PA-S";
            }
            else {
                vt_tray.m_fila_type = type;
            }
        }
        else {
            vt_tray.setting_id = "";
            vt_tray.m_fila_type = "";
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
            vt_tray.UpdateColorFromStr(color);
        }
        else {
            vt_tray.color = "";
        }
        if (vtray.contains("ctype")) {
            vt_tray.ctype = vtray["ctype"].get<int>();
        }
        else {
            vt_tray.ctype = 1;
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
            if (vtray["cols"].is_array()) {
                for (auto it = vtray["cols"].begin(); it != vtray["cols"].end(); it++) {
                    vt_tray.cols.push_back(it.value().get<std::string>());
                }
            }
        } else {
            vt_tray.cols.push_back(vt_tray.color);
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

bool MachineObject::contains_tray(const std::string &ams_id, const std::string &tray_id) const
{
    if (ams_id != VIRTUAL_AMS_MAIN_ID_STR && ams_id != VIRTUAL_AMS_DEPUTY_ID_STR) {

        return m_fila_system->GetAmsTray(ams_id, tray_id) != nullptr;
    } else {
        for (const auto& tray : vt_slot) {
            if (tray.id == ams_id) { return true; }
        }
    }

    return false;
}

DevAmsTray MachineObject::get_tray(const std::string &ams_id, const std::string &tray_id) const
{
    if (ams_id.empty() && tray_id.empty())
    {
        return DevAmsTray(tray_id);
    }

    if (ams_id != VIRTUAL_AMS_MAIN_ID_STR && ams_id != VIRTUAL_AMS_DEPUTY_ID_STR) {
        auto tray = m_fila_system->GetAmsTray(ams_id, tray_id);
        if (tray) { return *tray;};
    }
    else {
        for (const auto &tray : vt_slot) {
            if (tray.id == ams_id) { return tray; }
        }
    }

    assert(0);/*use contains_tray() check first*/
    return DevAmsTray(tray_id);
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
        if (camera_resolution_hold_count > 0) camera_resolution_hold_count--;
        if (camera_timelapse_hold_count > 0) camera_timelapse_hold_count--;


        if (time(nullptr) - ams_user_setting_start > HOLD_COUNT_MAX)
        {
            m_fila_system->GetAmsSystemSetting().SetDetectOnInsertEnabled(get_flag_bits(cfg, 0));
            m_fila_system->GetAmsSystemSetting().SetDetectOnPowerupEnabled(get_flag_bits(cfg, 1));
        }

        upgrade_force_upgrade = get_flag_bits(cfg, 2);

        if (time(nullptr) - camera_recording_ctl_start > HOLD_COUNT_MAX)
        {
            camera_recording_when_printing = get_flag_bits(cfg, 3);
        }

        if (camera_resolution_hold_count > 0)
        {
            camera_resolution_hold_count--;
        }
        else
        {
            camera_resolution = get_flag_bits(cfg, 4) == 0 ? "720p" : "1080p";
        }

        if (camera_timelapse_hold_count > 0)
        {
            camera_timelapse_hold_count--;
        }
        else
        {
            camera_timelapse = get_flag_bits(cfg, 5);
        }

        tutk_state = get_flag_bits(cfg, 6) == 1 ? "disable" : "";
        m_lamp->SetChamberLight(get_flag_bits(cfg, 7) == 1 ? DevLamp::LIGHT_EFFECT_ON : DevLamp::LIGHT_EFFECT_OFF);
        //is_support_build_plate_marker_detect = get_flag_bits(cfg, 12); todo yangcong
        if (time(nullptr) - xcam_first_layer_hold_start > HOLD_TIME_3SEC) { xcam_first_layer_inspector = get_flag_bits(cfg, 12); }

        if (time(nullptr) - xcam_ai_monitoring_hold_start > HOLD_COUNT_MAX) {
            xcam_ai_monitoring = get_flag_bits(cfg, 15);

            switch (get_flag_bits(cfg, 13, 2)) {
            case 0: xcam_ai_monitoring_sensitivity = "never_halt"; break;
            case 1: xcam_ai_monitoring_sensitivity = "low"; break;
            case 2: xcam_ai_monitoring_sensitivity = "medium"; break;
            case 3: xcam_ai_monitoring_sensitivity = "high"; break;
            default: break;
            }
        }

        if (time(nullptr) - xcam_auto_recovery_hold_start > HOLD_COUNT_MAX) {
            xcam_auto_recovery_step_loss = get_flag_bits(cfg, 16);
        }

        if (time(nullptr) - ams_user_setting_start > HOLD_COUNT_MAX){
            m_fila_system->GetAmsSystemSetting().SetDetectRemainEnabled(get_flag_bits(cfg, 17));
        }

        if (time(nullptr) - ams_switch_filament_start > HOLD_TIME_3SEC){
            m_fila_system->GetAmsSystemSetting().SetAutoRefillEnabled(get_flag_bits(cfg, 18));
        }

        if (time(nullptr) - xcam__save_remote_print_file_to_storage_start_time > HOLD_TIME_3SEC){
            xcam__save_remote_print_file_to_storage = get_flag_bits(cfg, 19);
        }

        if (time(nullptr) - xcam_door_open_check_start_time > HOLD_TIME_3SEC){
            xcam_door_open_check = (DoorOpenCheckState) get_flag_bits(cfg, 20, 2);
        }

        if (time(nullptr) - xcam_prompt_sound_hold_start > HOLD_TIME_3SEC) {
            xcam_allow_prompt_sound = get_flag_bits(cfg, 22);
        }

        if (time(nullptr) - xcam_filament_tangle_detect_hold_start > HOLD_TIME_3SEC){
            xcam_filament_tangle_detect = get_flag_bits(cfg, 23);
        }

        if (time(nullptr) - nozzle_blob_detection_hold_start > HOLD_TIME_3SEC) {
            nozzle_blob_detection_enabled = get_flag_bits(cfg, 24);
        }

        installed_upgrade_kit = get_flag_bits(cfg, 25);

        DevPrintOptionsParser::ParseDetectionV2_1(m_print_options, cfg);
    }

    /*fun*/
    std::string fun = print["fun"].get<std::string>();
    BOOST_LOG_TRIVIAL(info) << "new print data fun = " << fun;

    if (!fun.empty()) {

        is_support_agora    = get_flag_bits(fun, 1);
        if (is_support_agora) is_support_tunnel_mqtt = false;

        is_220V_voltage  = get_flag_bits(fun, 2) == 0?false:true;
        is_support_flow_calibration = get_flag_bits(fun, 6);
        if (this->is_series_o()) { is_support_flow_calibration = false; } // todo: Temp modification due to incorrect machine push message for H2D
        is_support_pa_calibration = get_flag_bits(fun, 7);
        if (this->is_series_p()) { is_support_pa_calibration = false; } // todo: Temp modification due to incorrect machine push message for P
        is_support_prompt_sound = get_flag_bits(fun, 8);
        is_support_filament_tangle_detect = get_flag_bits(fun, 9);
        is_support_motor_noise_cali = get_flag_bits(fun, 10);
        is_support_user_preset = get_flag_bits(fun, 11);
        is_support_door_open_check = get_flag_bits(fun, 12);
        is_support_nozzle_blob_detection = get_flag_bits(fun, 13);
        is_support_upgrade_kit = get_flag_bits(fun, 14);
        is_support_internal_timelapse = get_flag_bits(fun, 28);
        m_support_mqtt_homing = get_flag_bits(fun, 32);
        is_support_brtc = get_flag_bits(fun, 31);
        m_support_mqtt_axis_control = get_flag_bits(fun, 38);
        m_support_mqtt_bet_ctrl = get_flag_bits(fun, 39);
        is_support_new_auto_cali_method = get_flag_bits(fun, 40);
        is_support_spaghetti_detection = get_flag_bits(fun, 42);
        is_support_purgechutepileup_detection = get_flag_bits(fun, 43);
        is_support_nozzleclumping_detection = get_flag_bits(fun, 44);
        is_support_airprinting_detection = get_flag_bits(fun, 45);
        m_fan->SetSupportCoolingFilter(get_flag_bits(fun, 46));
        is_support_ext_change_assist = get_flag_bits(fun, 48);
        is_support_partskip = get_flag_bits(fun, 49);
        is_support_idelheadingprotect_detection = get_flag_bits(fun, 62);
    }

    /*fun2*/
    std::string fun2;
    if (print.contains("fun2") && print["fun2"].is_string()) {
        fun2 = print["fun2"].get<std::string>();
        BOOST_LOG_TRIVIAL(info) << "new print data fun2 = " << fun2;
    }

    // fun2 may have infinite length, use get_flag_bits_no_border
    if (!fun2.empty()) {
        is_support_print_with_emmc = get_flag_bits_no_border(fun2, 0) == 1;
    }

    /*aux*/
    std::string aux = print["aux"].get<std::string>();

    BOOST_LOG_TRIVIAL(info) << "new print data aux = " << aux;

    if (!aux.empty()) {
        m_storage->set_sdcard_state(get_flag_bits(aux, 12, 2));
    }

    /*stat*/
    std::string stat = print["stat"].get<std::string>();

    BOOST_LOG_TRIVIAL(info) << "new print data stat = " << stat;

    if (!stat.empty()) {
        camera_recording = get_flag_bits(stat, 7);
        m_lamp->SetLampCloseRecheck((get_flag_bits(stat, 36) == 1));
    }

    /*device*/
    if (print.contains("device")) {
        json const& device = print["device"];


        //new fan data
        m_fan->ParseV3_0(device);

        if (device.contains("type")) {
            m_device_mode = (DeviceMode)device["type"].get<int>();// FDM:1<<0 Laser:1<< Cut:1<<2
        }

        DevBed::ParseV2_0(device,m_bed);

        if (device.contains("nozzle")) {  DevNozzleSystemParser::ParseV2_0(device["nozzle"], m_nozzle_system); }
        if (device.contains("extruder")) { ExtderSystemParser::ParseV2_0(device["extruder"], m_extder_system);}
        if (device.contains("ext_tool")) { DevExtensionToolParser::ParseV2_0(device["ext_tool"], m_extension_tool); }

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

static bool is_hex_digit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

int MachineObject::get_flag_bits(std::string str, int start, int count) const
{
    try {
        unsigned long long decimal_value = std::stoull(str, nullptr, 16);
        unsigned long long mask = (1ULL << count) - 1;
        int flag = (decimal_value >> start) & mask;
        return flag;
    }
    catch (...) {
        return 0;
    }
}

uint32_t MachineObject::get_flag_bits_no_border(std::string str, int start_idx, int count) const
{
    if (start_idx < 0 || count <= 0) return 0;

    try {
        // --- 1) trim ---
        auto ltrim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char ch) { return !std::isspace(ch); }));
            };
        auto rtrim = [](std::string& s) {
            s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
            };
        ltrim(str); rtrim(str);

        // --- 2) remove 0x/0X prefix ---
        if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            str.erase(0, 2);
        }

        // --- 3) keep only hex digits ---
        std::string hex;
        hex.reserve(str.size());
        for (char c : str) {
            if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
        }
        if (hex.empty()) return 0;

        // --- 4) use size_t for all index/bit math ---
        const size_t total_bits = hex.size() * 4ULL;

        const size_t ustart = static_cast<size_t>(start_idx);
        if (ustart >= total_bits) return 0;

        const int int_bits = std::numeric_limits<uint32_t>::digits; // typically 32
        const size_t need_bits = static_cast<size_t>(std::min(count, int_bits));

        // [first_bit, last_bit]
        const size_t first_bit = ustart;
        const size_t last_bit = std::min(ustart + need_bits, total_bits) - 1ULL;
        if (last_bit < first_bit) return 0;


        const size_t right_index = hex.size() - 1ULL;

        const size_t first_nibble = first_bit / 4ULL;
        const size_t last_nibble = last_bit / 4ULL;

        const size_t start_idx = right_index - last_nibble;
        const size_t end_idx = right_index - first_nibble;
        if (end_idx < start_idx) return 0;

        const size_t sub_len = end_idx - start_idx + 1ULL;
        if (end_idx >= hex.size()) return 0;

        const std::string sub_hex = hex.substr(start_idx, sub_len);

        unsigned long long chunk = std::stoull(sub_hex, nullptr, 16);

        const unsigned nibble_offset = static_cast<unsigned>(first_bit % 4ULL);
        const unsigned long long shifted =
            (nibble_offset == 0U) ? chunk : (chunk >> nibble_offset);

        uint32_t mask;
        if (need_bits >= static_cast<size_t>(std::numeric_limits<uint32_t>::digits)) {
            mask = std::numeric_limits<uint32_t>::max();
        }
        else {
            mask = static_cast<uint32_t>((1ULL << need_bits) - 1ULL);
        }

        const uint32_t val = static_cast<uint32_t>(shifted & mask);
        return val;
    }
    catch (const std::invalid_argument&) {
        return 0;
    }
    catch (const std::out_of_range&) {
        return 0;
    }
    catch (...) {
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

        for (auto it = m_filament_list.begin(); it != m_filament_list.end(); it++) { m_checked_filament.insert(it->first); }

        m_filament_list = filament_list;
    }
}

void MachineObject::update_printer_preset_name()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "start update preset_name";
    PresetBundle *     preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) return;
    auto               printer_model = DevPrinterConfigUtil::get_printer_display_name(this->printer_type);
    std::set<std::string> diameter_set;
    for (auto &nozzle : m_extder_system->m_extders) {
        float diameter = nozzle.GetNozzleDiameter();
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
    auto printer_model = DevPrinterConfigUtil::get_printer_display_name(this->printer_type);
    std::map<std::string, std::set<std::string>> need_checked_filament_id;
    for (auto &ams_pair : m_fila_system->GetAmsList()) {
        auto ams_id = ams_pair.first;
        auto &ams = ams_pair.second;
        std::ostringstream stream;
        if (ams->GetExtruderId() < 0 || ams->GetExtruderId() >= m_extder_system->GetTotalExtderCount()) {
            return;
        }
        stream << std::fixed << std::setprecision(1) << m_extder_system->GetNozzleDiameter(ams->GetExtruderId());
        std::string nozzle_diameter_str = stream.str();
        assert(nozzle_diameter_str.size() == 3);
        if (m_nozzle_filament_data.find(nozzle_diameter_str) == m_nozzle_filament_data.end()) {
            //assert(false);
            continue;
        }
        auto &data = m_nozzle_filament_data[nozzle_diameter_str];
        auto &filament_list = data.filament_list;
        auto &checked_filament = data.checked_filament;
        for (const auto &[slot_id, curr_tray] : ams->GetTrays()) {

            if (curr_tray->setting_id.size() == 8 && curr_tray->setting_id[0] == 'P' && filament_list.find(curr_tray->setting_id) == filament_list.end()) {
                if (checked_filament.find(curr_tray->setting_id) != checked_filament.end()) {
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
                if (checked_filament.find(curr_tray->setting_id) != checked_filament.end()) {
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


                            command_ams_filament_settings(std::stoi(ams_id), std::stoi(slot_id), curr_tray->setting_id, preset_setting_id, curr_tray->color, curr_tray->m_fila_type,
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

    for (auto vt_tray : vt_slot) {
        int vt_id = std::stoi(vt_tray.id);
        int index = 255 - vt_id;
        if (index >= m_extder_system->GetTotalExtderCount()) {
            BOOST_LOG_TRIVIAL(error) << " vt_tray id map for nozzle id is not exist, index is: " << index << " nozzle count" << m_extder_system->GetTotalExtderCount();
            continue;
        }
        auto diameter = m_extder_system->GetNozzleDiameter(index);
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
            if (checked_filament.find(vt_tray.setting_id) != checked_filament.end()) {
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
            if (checked_filament.find(vt_tray.setting_id) != checked_filament.end()) {
                need_checked_filament_id[nozzle_diameter_str].insert(vt_tray.setting_id);
                try {
                    std::string        preset_setting_id;
                    PresetBundle *     preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;
                    std::ostringstream stream;
                    stream << std::fixed << std::setprecision(1) << m_extder_system->GetNozzleDiameter(MAIN_EXTRUDER_ID);
                    std::string nozzle_diameter_str = stream.str();
                    bool        is_equation = preset_bundle->check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(DevPrinterConfigUtil::get_printer_display_name(
                                                                                                                               this->printer_type),
                                                                                                                           nozzle_diameter_str, vt_tray.setting_id,
                                                                                                                           vt_tray.tag_uid, vt_tray.nozzle_temp_min,
                                                                                                                           vt_tray.nozzle_temp_max, preset_setting_id);
                    if (!is_equation) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__
                                                << " vt_tray filament is not match min max temp and reset, filament_id: " << vt_tray.setting_id;
                        command_ams_filament_settings(vt_id, 0, vt_tray.setting_id, preset_setting_id, vt_tray.color, vt_tray.m_fila_type, std::stoi(vt_tray.nozzle_temp_min),
                                                          std::stoi(vt_tray.nozzle_temp_max));

                    }
                } catch (...) {
                    BOOST_LOG_TRIVIAL(info) << "check fail and vt_tray.id" << vt_tray.id;
                }
            }
        }
    }

    for (auto &diameter_pair : m_nozzle_filament_data) {
        auto &diameter = diameter_pair.first;
        auto &data     = diameter_pair.second;
        for (auto &filament_id : need_checked_filament_id[diameter]) {
            data.checked_filament.erase(filament_id);
        }
    }
}


void MachineObject::command_set_door_open_check(DoorOpenCheckState state)
{
    json j;
    j["system"]["command"]       = "set_door_stat";
    j["system"]["sequence_id"]   = std::to_string(MachineObject::m_sequence_id++);
    switch (state)
    {
         case Slic3r::MachineObject::DOOR_OPEN_CHECK_DISABLE:            j["system"]["config"] = 0; break;
         case Slic3r::MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING:     j["system"]["config"] = 1; break;
         case Slic3r::MachineObject::DOOR_OPEN_CHECK_ENABLE_PAUSE_PRINT: j["system"]["config"] = 2; break;
         default: assert(0); return;
    }

    if (publish_json(j) == 0)
    {
        xcam_door_open_check = state;
        xcam_door_open_check_start_time = time(nullptr);
    }
}


void MachineObject::command_set_save_remote_print_file_to_storage(bool save)
{
    if (get_save_remote_print_file_to_storage() != save)
    {
        json j;
        j["system"]["command"] = "print_cache_set";
        j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["system"]["config"] = save ? true : false;

        if (publish_json(j) == 0)
        {
            xcam__save_remote_print_file_to_storage = save;
            xcam__save_remote_print_file_to_storage_start_time = time(nullptr);
        }
    }
}

wxString MachineObject::get_nozzle_replace_url() const
{
    const wxString& strLanguage = GUI::wxGetApp().app_config->get("language");
    const wxString& lan_code = strLanguage.BeforeFirst('_');

    const json& link_map = DevPrinterConfigUtil::get_json_from_config(printer_type, "print", "nozzle_replace_wiki");
    if (link_map.contains(lan_code.ToStdString())) {
        return link_map[lan_code.ToStdString()].get<wxString>();
    }

    if (link_map.contains("en")){
        return link_map["en"].get<wxString>();
    }/*retry with en*/

    return "https://wiki.bambulab.com/en/h2/maintenance/replace-hotend";
}

std::string MachineObject::get_error_code_str(int error_code)
{
    if (error_code < 0) { return std::string();}

    char buf[32];
    ::sprintf(buf, "%08X", error_code);
    std::string print_error_str = std::string(buf);
    if (print_error_str.size() > 4) { print_error_str.insert(4, "-"); }
    return print_error_str;
}

void MachineObject::add_command_error_code_dlg(int command_err, json action_json)
{
    if (command_err > 0 && !Slic3r::GUI::wxGetApp().get_hms_query()->is_internal_error(this, command_err))
    {
        GUI::wxGetApp().CallAfter([this, command_err, action_json, token = std::weak_ptr<int>(m_token)]
        {
            if (token.expired()) { return;}
            GUI::DeviceErrorDialog* device_error_dialog = new GUI::DeviceErrorDialog(this, (wxWindow*)GUI::wxGetApp().mainframe);
            device_error_dialog->Bind(wxEVT_DESTROY, [this, token = std::weak_ptr<int>(m_token)](auto& event)
                {
                    if (!token.expired()) { m_command_error_code_dlgs.erase((GUI::DeviceErrorDialog*)event.GetEventObject());}
                    event.Skip();
                });

            if(!action_json.is_null()) device_error_dialog->set_action_json(action_json);
            device_error_dialog->show_error_code(command_err);
            m_command_error_code_dlgs.insert(device_error_dialog);
        });
    };
}

bool MachineObject::is_multi_extruders() const
{
    return m_extder_system->GetTotalExtderCount() > 1;
}

int MachineObject::get_extruder_id_by_ams_id(const std::string& ams_id)
{
    return m_fila_system->GetExtruderIdByAmsId(ams_id);
}

Slic3r::DevPrintingSpeedLevel MachineObject::GetPrintingSpeedLevel() const
{
    return m_print_options->GetPrintingSpeedLevel();
}

bool MachineObject::is_target_slot_unload() const
{
    return m_extder_system->GetTargetSlotId().compare("255") == 0;
}

Slic3r::DevAms* MachineObject::get_curr_Ams()
{
    return m_fila_system->GetAmsById(m_extder_system->GetCurrentAmsId());
}

Slic3r::DevAmsTray* MachineObject::get_ams_tray(std::string ams_id, std::string tray_id)
{
    return m_fila_system->GetAmsTray(ams_id, tray_id);
}

bool MachineObject::HasAms() const
{
    return m_fila_system->HasAms();
}

void change_the_opacity(wxColour& colour)
{
    if (colour.Alpha() == 255) {
        colour = wxColour(colour.Red(), colour.Green(), colour.Blue(), 254);
    }
}

} // namespace Slic3r
