#include <boost/log/trivial.hpp>
#include "slic3r/GUI/GUI_App.hpp"

#include "slic3r/GUI/UserNotification.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/dir.h>
#include "fast_float/fast_float.h"

#include "DevCalib.h"
#include "DevDefs.h"
#include "DevFilaSystem.h"
#include "DevConfig.h"

namespace Slic3r {

static float string_to_float(const std::string &str_value)
{
    float value = 0.0;
    fast_float::from_chars(str_value.c_str(), str_value.c_str() + str_value.size(), value);
    return value;
}

static NozzleVolumeType convert_to_nozzle_type(const std::string &str)
{
    if (str.size() < 8) {
        assert(false && "invalid nozzle info");
        return NozzleVolumeType::nvtStandard;
    }

    if (str[1] == 'S')
        return NozzleVolumeType::nvtStandard;
    else if (str[1] == 'H')
        return NozzleVolumeType::nvtHighFlow;
    else if (str[1] == 'U')
        return NozzleVolumeType::nvtTPUHighFlow;
    // Orca: no nvtE3DHighFlow in Orca's NozzleVolumeType; map 'B' to Standard
    else
        return NozzleVolumeType::nvtStandard;
}

static float get_number_flexible(const json& j, const std::string& key, float def = 0.0f)
{
    if (!j.contains(key)) return def;

    const auto& v = j[key];

    if (v.is_number_float())    return v.get<float>();
    if (v.is_number_integer())  return static_cast<float>(v.get<int>());
    if (v.is_string())          return string_to_float(v.get<std::string>());

    return def;
}

void from_json(const json& j, PACalibResult& cali) {
    cali.extruder_id        = j.value("extruder_id", 0);
    cali.nozzle_volume_type = convert_to_nozzle_type(j.value("nozzle_id", "HS00-0.4"));
    cali.tray_id            = j.value("tray_id",0);
    cali.ams_id             = j.value("ams_id",0);
    cali.slot_id            = j.value("slot_id",0);
    cali.cali_idx           = j.value("cali_idx",-1);
    cali.nozzle_pos_id      = j.value("nozzle_pos",-1);
    cali.nozzle_diameter    = get_number_flexible(j, "nozzle_diameter", 0.4f);
    cali.nozzle_sn          = j.value("nozzle_sn","");
    cali.filament_id        = j.value("filament_id","");
    cali.setting_id         = j.value("setting_id","");
    cali.name               = j.value("name","");
    cali.k_value            = get_number_flexible(j, "k_value", 0.0f);
    cali.n_coef             = get_number_flexible(j, "n_coef", 0.0f);
    cali.confidence         = j.value("confidence", 0);
}

void from_json(const json& j, FlowRatioCalibResult& cali)
{
    cali.tray_id         = j.value("tray_id", 0);
    cali.nozzle_diameter = string_to_float(j.value("nozzle_diameter", ""));
    cali.filament_id     = j.value("filament_id", "");
    cali.setting_id      = j.value("setting_id", "");
    cali.flow_ratio      = string_to_float(j.value("flow_ratio", ""));
    cali.confidence      = j.value("confidence", 0);
}

void DevCalib::ParseCalibVersion(const json& j, DevCalib* system)
{
    if(system) system->m_calib_version = j.value("cali_version", -1);
}

bool DevCalib::IsVersionExpired() const
{
    if (m_last_calib_version.has_value())
        return m_last_calib_version.value() != m_calib_version;
    else
        return true;
}

void DevCalib::ParseSupportNewAutoCalib(int flag, DevCalib* system)
{
    if(system) system->m_support_new_auto_cali = flag;
}

void DevCalib::RequestPAResult()
{
    m_pa_results_status = CalibStatus::REQUEST;
}

void DevCalib::ResetPAResult()
{
    m_pa_calib_results.clear();
    m_pa_results_status = CalibStatus::IDLE;
}

int DevCalib::RequestPAHistory(const PACalibExtruderInfo &calib_info)
{
    m_pa_calib_tab.clear();
    m_pa_table_status = CalibStatus::REQUEST;

    return GetOwner()->command_get_pa_calibration_tab(calib_info);
}

void DevCalib::ResetPAHistory()
{
    m_pa_calib_tab.clear();
    m_pa_table_status = CalibStatus::IDLE;
}

void DevCalib::RequestFlowRateResult()
{
    m_flow_results_status = CalibStatus::REQUEST;
}

void DevCalib::ResetFlowRateResult()
{
    m_flow_ratio_results.clear();
    m_flow_results_status = CalibStatus::IDLE;
}

void calib_fail_message(MachineObject* obj, std::string cali_mode, std::string reason){
    wxString    info;
    if (reason == "invalid nozzle_diameter" || reason == "nozzle_diameter is not supported") {
        info = _L("This calibration does not support the currently selected nozzle diameter");
    } else if (reason == "invalid handle_flowrate_cali param") {
        info = _L("Current flowrate cali param is invalid");
    } else if (reason == "nozzle_diameter is not matched") {
        info = _L("Selected diameter and machine diameter do not match");
    } else if (reason == "generate auto filament cali gcode failure") {
        info = _L("Failed to generate cali gcode");
    } else {
        info = wxString(reason);
    }

    GUI::wxGetApp().push_notification(obj, info, _L("Calibration error"), UserNotificationStyle::UNS_WARNING_CONFIRM);
    BOOST_LOG_TRIVIAL(info) << cali_mode << " result fail, reason = " << reason;
}

void DevCalib::ExtrusionCalibSetParse(const json & jj){
    int tray_id = jj.value("tray_id", -1);

    auto tray_ams_slot_map = GetOwner()->GetFilaSystem()->GetTrayIndexMap();
    int ams_id  = tray_ams_slot_map.find(tray_id) != tray_ams_slot_map.end() ? tray_ams_slot_map[tray_id].first : -1;
    int slot_id = tray_ams_slot_map.find(tray_id) != tray_ams_slot_map.end() ? tray_ams_slot_map[tray_id].second : -1;

    if(tray_id == VIRTUAL_TRAY_MAIN_ID) {
        GetOwner()->vt_slot[MAIN_EXTRUDER_ID].k = jj.value("k_value", GetOwner()->vt_slot[MAIN_EXTRUDER_ID].k);
        GetOwner()->vt_slot[MAIN_EXTRUDER_ID].n = jj.value("n_value", GetOwner()->vt_slot[MAIN_EXTRUDER_ID].n);
    }else{
        auto tray_item = GetOwner()->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
        if (tray_item) {
            tray_item->k = jj.value("k_value", tray_item->k);
            tray_item->n = jj.value("n_coef", tray_item->n);
        }
    }

    GetOwner()->extrusion_cali_set_tray_id = tray_id;
    GetOwner()->extrusion_cali_set_hold_start = std::chrono::system_clock::now();
}

/* calib select ack parse */
void DevCalib::ExtrusionCalibSelectParse(const json &jj){
    try{
        int tray_id = jj.value("tray_id", -1);

        auto tray_ams_slot_map = GetOwner()->GetFilaSystem()->GetTrayIndexMap();
        int default_ams_id = tray_ams_slot_map.find(tray_id) != tray_ams_slot_map.end() ? tray_ams_slot_map[tray_id].first : -1;
        int default_slot_id = tray_ams_slot_map.find(tray_id) != tray_ams_slot_map.end() ? tray_ams_slot_map[tray_id].second : -1;

        int ams_id = jj.value("ams_id", default_ams_id);
        int slot_id = jj.value("slot_id", default_slot_id);

        BOOST_LOG_TRIVIAL(trace) << "extrusion_cali_sel: illegal ams_id = " << ams_id << "slot_id = " << slot_id;

        std::vector<DevAmsTray> &vt_slot = GetOwner()->vt_slot;
        if (ams_id == VIRTUAL_TRAY_MAIN_ID && vt_slot.size() > 0) {
            vt_slot[MAIN_EXTRUDER_ID].cali_idx = jj.value("cali_idx", vt_slot[MAIN_EXTRUDER_ID].cali_idx);
            vt_slot[MAIN_EXTRUDER_ID].set_hold_count();
        } else if (ams_id == VIRTUAL_TRAY_DEPUTY_ID && vt_slot.size() > 1) {
            vt_slot[DEPUTY_EXTRUDER_ID].cali_idx = jj.value("cali_idx", vt_slot[DEPUTY_EXTRUDER_ID].cali_idx);
            vt_slot[DEPUTY_EXTRUDER_ID].set_hold_count();
        } else {
            auto tray_item = GetOwner()->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
            if (tray_item) {
                tray_item->cali_idx = jj.value("cali_idx", tray_item->cali_idx);
                tray_item->set_hold_count();
            }
        }
    } catch(...){

    }
}

void DevCalib::ExtrusionCalibGetTableParse(const json &jj){
    if (GetPAHistoryStatus() == CalibStatus::REQUEST ) {
        m_pa_table_status = CalibStatus::WAITING;

        /* request success */
        if (!(jj.contains("result") && jj.contains("reason") && jj["result"].get<std::string>() == "fail")) {
            SyncCalibVersion();
            m_pa_table_status = CalibStatus::FINISHED;
        }

        try{
            json filaments_json;
            if(jj.contains("filaments"))
            {
                /* fill item->nozzle_diameter with command->nozzle_diameter */
                filaments_json = jj["filaments"];
                for (auto &f : filaments_json) {
                    if (!f.contains("nozzle_diameter") && jj.contains("nozzle_diameter")) {
                        f["nozzle_diameter"] = jj["nozzle_diameter"];
                    }
                }
            }

            m_pa_calib_tab = filaments_json.get<std::vector<PACalibResult>>();

            /* filter invalid pa_calib_tab */
            m_pa_calib_tab.erase(std::remove_if(m_pa_calib_tab.begin(), m_pa_calib_tab.end(), [](auto &res) { return res.k_value < 0.0f || res.k_value > 10.0f; }), m_pa_calib_tab.end());

            if (m_pa_calib_tab.empty()) { BOOST_LOG_TRIVIAL(info) << "empty pa calib history"; }
        }catch(...){
            m_pa_calib_tab.clear();
            BOOST_LOG_TRIVIAL(error) << "pa calib history missing fields, current json:\n "<< jj.dump();
        }
        // notify cali history to update
    }
}

void DevCalib::ExtrusionCalibGetResultParse(const json &jj)
{
    m_pa_results_status = CalibStatus::WAITING;

    if (!(jj.contains("result") && jj.contains("reason") && jj["result"].get<std::string>() == "fail" && jj.contains("err_code"))) {
        m_pa_results_status = CalibStatus::FINISHED;
    }

    try {
        json filaments_json;
        if(jj.contains("filaments"))
        {
            /* fill item->nozzle_diameter with command->nozzle_diameter */
            filaments_json = jj["filaments"];
            for (auto &f : filaments_json) {
                if (!f.contains("nozzle_diameter") && jj.contains("nozzle_diameter") ) {
                    f["nozzle_diameter"] = jj["nozzle_diameter"];
                }

                if (IsSupportNewAutoCali()) {
                    auto ams_id = f.value("ams_id", 0);
                    auto slot_id = f.value("slot_id", 0);
                    if(f.contains("tray_id")){
                        // Orca: Orca's DevFilaSystem has no GetTrayIdByAmsSlotId; mirror its
                        // semantics via a reverse lookup over GetTrayIndexMap() (correct for
                        // non-4-slot AMS layouts, unlike a fixed ams_id*4+slot_id formula).
                        int mapped_tray_id = -1;
                        const auto tray_ams_slot_map = GetOwner()->GetFilaSystem()->GetTrayIndexMap();
                        for (const auto& item : tray_ams_slot_map) {
                            if (item.second.first == ams_id && item.second.second == slot_id) {
                                mapped_tray_id = item.first;
                                break;
                            }
                        }
                        f["tray_id"] = mapped_tray_id;
                    }
                }
            }
        }

        m_pa_calib_results = filaments_json.get<std::vector<PACalibResult>>();

        m_pa_calib_results.erase(std::remove_if(m_pa_calib_results.begin(), m_pa_calib_results.end(), [](auto &res) { return res.k_value < 0.0f || res.k_value > 10.0f; }), m_pa_calib_results.end());

        if (m_pa_calib_results.empty()) { BOOST_LOG_TRIVIAL(info) << "empty pa calib result"; }
    } catch (...) {
        m_pa_calib_results.clear();
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "pa calibration results missing fileds, current json: \n"<<jj.dump();
    }
}

void DevCalib::FlowrateGetResultParse(const json &jj){
    m_flow_results_status = CalibStatus::FINISHED;
    m_flow_ratio_results.clear();

    if(!jj.contains("filaments")) return;

    try {
        m_flow_ratio_results = jj["filaments"].get<std::vector<FlowRatioCalibResult>>();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "flow ratio calibration results missing fileds, current json:\n"<<jj.dump();
    }
}

void DevCalib::ParseV1_0(const json &jj, DevCalib *system, bool key_field_only)
{
    if(!jj.contains("command")) return;

    if (jj["command"].get<std::string>() == "extrusion_cali" || jj["command"].get<std::string>() == "flowrate_cali") {
        if (jj.contains("result")) {
            if (jj["result"].get<std::string>() == "success") {
            } else if (jj["result"].get<std::string>() == "fail") {
                std::string cali_mode = jj["command"].get<std::string>();
                std::string reason    = jj["reason"].get<std::string>();
                calib_fail_message(system->GetOwner(), cali_mode, reason);
            }
        }
    } else if (jj["command"].get<std::string>() == "extrusion_cali_set") {
        system->ExtrusionCalibSetParse(jj);
    } else if (jj["command"].get<std::string>() == "extrusion_cali_sel") {
        system->ExtrusionCalibSelectParse(jj);
    } else if (jj["command"].get<std::string>() == "extrusion_cali_get") {
        system->ExtrusionCalibGetTableParse(jj);
    } else if (jj["command"].get<std::string>() == "extrusion_cali_get_result") {
        system->ExtrusionCalibGetResultParse(jj);
    } else if (jj["command"].get<std::string>() == "flowrate_get_result" && !key_field_only) {
        system->FlowrateGetResultParse(jj);
    }
}

} // namespace Slic3r