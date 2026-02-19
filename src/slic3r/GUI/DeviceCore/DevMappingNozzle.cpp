#include "DevMapping.h"

#include "DevNozzleRack.h"
#include "DevNozzleSystem.h"

#include "DevUtil.h"
#include "DevUtilBackend.h"

#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/Print.hpp"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"

#include "slic3r/GUI/GUI_App.hpp"

#include <boost/lexical_cast.hpp>
#include <nlohmann/json.hpp>
using namespace nlohmann;

namespace Slic3r {

static int s_get_physical_extruder_id(int total_ext_count, int logical_extruder_id)
{
    if (total_ext_count == 2) {
        return logical_extruder_id == 1 ? 1 : 0;
    } else if (total_ext_count == 1) {
        return 0;
    }

    return logical_extruder_id - 1;
}

static std::string  s_get_diameter_str(float diameter)
{
    return (boost::format("%.2f") % diameter).str();
}

static std::string s_get_diameter_str(const std::string& diameter)
{
    try {
        float dia = boost::lexical_cast<float>(diameter);
        return s_get_diameter_str(dia);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to boost::lexical_cast: " << diameter;
        return diameter;
    }

    try {
        float dia = std::stof(diameter);
        return s_get_diameter_str(dia);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " std::stof: " << diameter;
        return diameter;
    }
}


// get auto nozzle mapping through AP
// warnings:
// (1) the fila_id here is 1 based index
// (2) the AP wants the diameter string with 2 decimal places
int MachineObject::ctrl_get_auto_nozzle_mapping(Slic3r::GUI::Plater* plater, const std::vector<FilamentInfo>& ams_mapping, int flow_cali_opt, int pa_value)
{
    m_auto_nozzle_mapping.Clear();
    if (!plater) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": plater is nullptr";
        return -1;
    }

    GCodeProcessorResult* gcode_result = plater->background_process().get_current_gcode_result();
    if (!gcode_result) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": gcode_result is nullptr";
        return -1;
    }

    const auto& result = gcode_result->nozzle_group_result;
    if (!result) {
        assert(false && "gcode_result->nozzle_group_result");
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": gcode_result->nozzle_group_result is NULL";
        return -1;
    }

    if (ams_mapping.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": the ams mapping is empty";
        return -1;// the ams mapping is empty
    }

    json command_jj;
    command_jj["print"]["command"] = "get_auto_nozzle_mapping";
    m_auto_nozzle_mapping.m_sequence_id = std::to_string(m_sequence_id++);
    command_jj["print"]["sequence_id"] = m_auto_nozzle_mapping.m_sequence_id;
    command_jj["print"]["calibration"] = flow_cali_opt;
    command_jj["print"]["extrude_cali_manual_mode"] = pa_value;


    // filament seq
    json filament_seq_jj;
    int max_fila_id = 0;
    std::unordered_set<int> filaid_set;
    for (int idx = 0; idx < gcode_result->filament_change_sequence.size(); idx++) {
        int fila_id = gcode_result->filament_change_sequence[idx];
        if (filaid_set.count(fila_id) == 0) {
            filaid_set.insert(fila_id);
            filament_seq_jj[fila_id + 1] = idx;
            max_fila_id = std::max(max_fila_id, fila_id + 1);
        }
    }

    for (int fila_id = 0; fila_id <= max_fila_id; fila_id++) {
        if (filament_seq_jj[fila_id].is_null()) {
            filament_seq_jj[fila_id] = -1;// fill the used fila_id with -1
        }
    }

    command_jj["print"]["filament_seq"] = filament_seq_jj;

    // ams mapping
    std::vector<int> ams_mapping_vec(33, 0xFFFF);/* AP ask to fill them*/
    for (auto item :  ams_mapping) {
        try {
            int ams_id = stoi(item.ams_id);
            int slot_id = !item.slot_id.empty() ? stoi(item.slot_id) : 0;
            ams_mapping_vec[item.id + 1] = (ams_id << 8) | slot_id;/*using ams_id << 8 | slot_id*/
        } catch (std::exception& e) {
            assert(false && "invalid ams_id or slot_id");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception: " << e.what();
            ams_mapping_vec[item.id + 1] = item.tray_id;
        }
    }
    command_jj["print"]["ams_mapping"] = ams_mapping_vec;

    // filament info
    json filament_info_jj;
    for (int fila_id = 0; fila_id < ams_mapping.size(); fila_id++) {
        const auto& fila = ams_mapping[fila_id];
        const auto& nozzle_info = result->get_nozzle_for_filament(fila.id);
        if (!nozzle_info) {
            assert(false && "nozzle_info should not be nullptr");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "nozzle_info should not be nullptr";
            continue;
        }

        json fila_item_jj;
        fila_item_jj["id"] = (fila.id + 1);
        fila_item_jj["direction"] = (nozzle_info->extruder_id == LOGIC_L_EXTRUDER_ID ? 1 : 2);
        fila_item_jj["group"] = nozzle_info->group_id;
        fila_item_jj["nozzle_d"] = s_get_diameter_str(nozzle_info->diameter);
        fila_item_jj["nozzle_v"] = (nozzle_info->volume_type == NozzleVolumeType::nvtHighFlow) ? "High Flow" : "Standard";
        fila_item_jj["cate"] = fila.filament_id;
        fila_item_jj["color"] = fila.color;
        filament_info_jj.push_back(fila_item_jj);
    }

    command_jj["print"]["fila_info"] = filament_info_jj;

    // nozzle info
    json nozzle_info_jj;
    const auto& extruder_nozzles = m_nozzle_system->GetExtNozzles();
    for (const auto& nozzle : extruder_nozzles) {
        if (nozzle.second.IsNormal()) {
            json nozzle_item_jj;
            nozzle_item_jj["pos"] = nozzle.second.GetNozzleId();
            if (m_nozzle_system->GetReplaceNozzleTar().has_value() && nozzle_item_jj["pos"] == MAIN_EXTRUDER_ID) {
                nozzle_item_jj["pos"] = m_nozzle_system->GetReplaceNozzleTar().value();// special case of tar_id. see protocol definition
            }

            nozzle_item_jj["nozzle_d"] = s_get_diameter_str(nozzle.second.GetNozzleDiameter());
            nozzle_item_jj["nozzle_v"] = DevNozzle::ToNozzleFlowString(nozzle.second.GetNozzleFlowType());
            nozzle_item_jj["wear"] = nozzle.second.GetNozzleWear();
            nozzle_item_jj["cate"] = nozzle.second.GetFilamentId();
            nozzle_item_jj["color"] = nozzle.second.GetFilamentColor();
            nozzle_info_jj.push_back(nozzle_item_jj);
        }
    }

    const auto& rack_nozzles = m_nozzle_system->GetNozzleRack()->GetRackNozzles();
    for (const auto& nozzle : rack_nozzles) {
        if (nozzle.second.IsNormal()) {
            json nozzle_item_jj;
            nozzle_item_jj["pos"] = (nozzle.second.GetNozzleId() + 0x10);
            if (m_nozzle_system->GetReplaceNozzleTar().has_value() && nozzle_item_jj["pos"] == MAIN_EXTRUDER_ID) {
                nozzle_item_jj["pos"] = m_nozzle_system->GetReplaceNozzleTar().value();// special case of tar_id. see protocol definition
            }

            nozzle_item_jj["nozzle_d"] = s_get_diameter_str(nozzle.second.GetNozzleDiameter());
            nozzle_item_jj["nozzle_v"] = DevNozzle::ToNozzleFlowString(nozzle.second.GetNozzleFlowType());
            nozzle_item_jj["wear"] = nozzle.second.GetNozzleWear();
            nozzle_item_jj["cate"] = nozzle.second.GetFilamentId();
            nozzle_item_jj["color"] = nozzle.second.GetFilamentColor();
            nozzle_info_jj.push_back(nozzle_item_jj);
        }
    }

    command_jj["print"]["nozzle_info"] = nozzle_info_jj;
    return publish_json(command_jj);
}


void DevNozzleMappingResult::ParseAutoNozzleMapping(Slic3r::MachineObject* obj, const json& print_jj)
{
    if (print_jj.contains("command") && print_jj["command"].get<string>() == "get_auto_nozzle_mapping") {
        if (print_jj.contains("sequence_id") && print_jj["sequence_id"] == m_sequence_id) {
            Clear();
            DevJsonValParser::ParseVal(print_jj, "result", m_result);
            DevJsonValParser::ParseVal(print_jj, "reason", m_mqtt_reason);
            DevJsonValParser::ParseVal(print_jj, "errno", m_errno);
            DevJsonValParser::ParseVal(print_jj, "detail", m_detail_json);
            DevJsonValParser::ParseVal(print_jj, "type", m_type);

            if (print_jj.contains("mapping")) {
                m_nozzle_mapping_json = print_jj["mapping"];
                const auto& mapping = print_jj["mapping"].get<std::vector<int>>();
                for (int fila_id = 0; fila_id < mapping.size(); ++fila_id) {
                    m_nozzle_mapping[fila_id] = mapping[fila_id];
                }
            }

            m_flush_weight_base = GetFlushWeight(obj);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": get_auto_nozzle_mapping: " << m_result;
        }
    }
}

void DevNozzleMappingResult::Clear()
{
    m_sequence_id.clear();
    m_result.clear();
    m_mqtt_reason.clear();
    m_type.clear();
    m_errno = 0;
    m_detail_msg.clear();
    m_detail_json.clear();
    m_nozzle_mapping.clear();
    m_nozzle_mapping_json.clear();

    m_flush_weight_base = -1;
    m_flush_weight_current = -1;
}


void DevNozzleMappingResult::SetManualNozzleMapping(Slic3r::MachineObject* obj, int fila_id, int nozzle_pos_id)
{
    if (nozzle_pos_id == MAIN_EXTRUDER_ID && obj->GetNozzleSystem()->GetReplaceNozzleTar().has_value()){
        nozzle_pos_id = obj->GetNozzleSystem()->GetReplaceNozzleTar().value();// special case of tar_id. see protocol definition
    }

    if (m_nozzle_mapping[fila_id] != nozzle_pos_id) {
        m_nozzle_mapping[fila_id] = nozzle_pos_id;
        m_flush_weight_current = GetFlushWeight(obj);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": fila_id=" << fila_id << ", nozzle_pos_id=" << nozzle_pos_id;
    }

    try {
        auto mapping = m_nozzle_mapping_json.get<std::vector<int>>();
        if (mapping.at(fila_id) != nozzle_pos_id) {
            mapping[fila_id] = nozzle_pos_id;
            m_nozzle_mapping_json = mapping;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": updated to " << m_nozzle_mapping_json.dump();
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": exception: " << e.what();
    };
}


int DevNozzleMappingResult::GetMappedNozzlePosIdByFilaId(MachineObject* obj, int fila_id) const
{
    auto iter = m_nozzle_mapping.find(fila_id);
    if (iter == m_nozzle_mapping.end()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": not mapped for " << fila_id;
        return -1;// the filament is not mapped by the machine
    }

    if (obj->GetNozzleSystem()->GetReplaceNozzleTar().has_value() && iter->second == obj->GetNozzleSystem()->GetReplaceNozzleTar().value()) {
        return 0;// 0 means right extueder
    }

    return iter->second;
}

float DevNozzleMappingResult::GetFlushWeight(Slic3r::MachineObject* obj) const
{
    auto plater = Slic3r::GUI::wxGetApp().plater();
    if (!plater) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": plater is nullptr";
        return -1;
    }

    GCodeProcessorResult* gcode_result = plater->background_process().get_current_gcode_result();
    if (!gcode_result) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": gcode_result is nullptr";
        return -1;
    }

    if (gcode_result->filament_change_sequence.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": filament_change_sequence is empty";
        return -1;
    }

    if (!Slic3r::GUI::wxGetApp().preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": preset_bundle is nullptr";
        return -1;
    };

    const std::vector<std::vector<std::vector<float>>>& flush_matrix = Slic3r::GUI::wxGetApp().preset_bundle->get_full_flush_matrix();

    float total_flush_volume = 0;
    MultiNozzleUtils::NozzleStatusRecorder recorder;
    for (auto filament : gcode_result->filament_change_sequence) {
        auto nozzle_pos = GetMappedNozzlePosIdByFilaId(obj, filament);
        if (nozzle_pos == -1){
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": nozzle_pos is -1 for fila_id=" << filament;
            continue;
        }

        auto nozzle_info = obj->GetNozzleSystem()->GetNozzleByPosId(nozzle_pos);
        if (nozzle_info.IsEmpty()) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": nozzle_info IsEmpty for fila_id=" << filament << ", nozzle_pos=" << nozzle_pos;
            continue;
        }

        int extruder_id = nozzle_info.GetLogicExtruderId();
        int nozzle_id = nozzle_info.GetNozzlePosId();
        int last_filament = recorder.get_filament_in_nozzle(nozzle_id);

        if (last_filament != -1 && last_filament != filament) {
            if (flush_matrix.size() > extruder_id &&
                flush_matrix[extruder_id].size() > last_filament &&
                flush_matrix[extruder_id][last_filament].size() > filament){
                total_flush_volume += flush_matrix[extruder_id][last_filament][filament];
            }
            else{
                assert(false && "missing flush volume");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": missing flush volume for extruder_id=" << extruder_id
                                           << ", last_filament=" << last_filament << ", filament=" << filament;
            }
        }

        recorder.set_nozzle_status(nozzle_id, filament);
    }

    // estimate the flush weight: total_flush_volume * 1.26 * 0.001
    return total_flush_volume * 1.26 * 0.001;
}

} // namespace Slic3r