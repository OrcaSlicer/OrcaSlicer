#include <nlohmann/json.hpp>
#include "DevCtrl.h"

// TODO: remove this include
#include "DevUtil.h"
#include "slic3r/GUI/DeviceManager.hpp"

using namespace nlohmann;

namespace Slic3r
{

DevCtrlInfo::DevCtrlInfo(MachineObject* obj, int sequence_id, const json& req_json,
                         int interval_max, int interval_min)
{
    m_request_dev_id = obj->get_dev_id();
    m_request_seq = sequence_id;
    m_request_time = time(nullptr);
    m_request_json = req_json;
    m_request_interval_max = interval_max;
    m_request_interval_min = interval_min;
}

bool DevCtrlInfo::CheckCanUpdateData(const nlohmann::json& jj)
{
    if (m_request_json.empty()) {
        return true;
    }

    if (m_time_out) {
        return true;
    }
    if (time(nullptr) - m_request_time > m_request_interval_max) {
        OnTimeOut();
        return true;
    }
    if (time(nullptr) - m_request_time < m_request_interval_min) {
        return false;
    }

    if (m_received) {
        return true;
    }
    try {
        if (jj.contains("sequence_id") && jj["sequence_id"].is_string()) {
            int sequence_id = stoi(jj["sequence_id"].get<std::string>());
            if (sequence_id >= m_request_seq) {
                OnReceived();
                return true;
            }
        }
    } catch (...) {
        ;
    }

    return false;
}

    int DevCtrl::command_select_extruder(int id)
    {
        json j;
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["command"] = "select_extruder";
        j["print"]["extruder_index"] = id;
        int rtn = m_obj->publish_json(j, 1);
        if (rtn == 0)
        {
            m_obj->targ_nozzle_id_from_pc = id;
        }

        return rtn;
    }
}