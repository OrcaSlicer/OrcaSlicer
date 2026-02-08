#include "DevExtruderSystem.h"
#include "DevNozzleSystem.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r
{

DevNozzle DevNozzleSystem::GetNozzle(int id) const
{
    if (m_nozzles.find(id) != m_nozzles.end())
    {
        return m_nozzles.at(id);
    }

    return DevNozzle();
}

void DevNozzleSystem::Reset()
{
    m_nozzles.clear();
    m_extder_exist = 0;
    m_state = 0; // idle state
}


static unordered_map<string, NozzleFlowType> _str2_nozzle_flow_type = {
    {"S", NozzleFlowType::S_FLOW},
    {"H", NozzleFlowType::H_FLOW},
    {"A", NozzleFlowType::S_FLOW},
    {"X", NozzleFlowType::S_FLOW}
};

static unordered_map<string, NozzleType> _str2_nozzle_type = {
    {"00", NozzleType::ntStainlessSteel},
    {"01", NozzleType::ntHardenedSteel},
    {"05", NozzleType::ntTungstenCarbide}
};

static void s_parse_nozzle_type(const std::string& nozzle_type_str, DevNozzle& nozzle)
{
    if (NozzleTypeStrToEumn.count(nozzle_type_str) != 0)
    {
        nozzle.m_nozzle_type = NozzleTypeStrToEumn[nozzle_type_str];
    }
    else if (nozzle_type_str.length() >= 4)
    {
        const std::string& flow_type_str = nozzle_type_str.substr(1, 1);
        if (_str2_nozzle_flow_type.count(flow_type_str) != 0)
        {
            nozzle.m_nozzle_flow = _str2_nozzle_flow_type[flow_type_str];
        }
        const std::string& type_str = nozzle_type_str.substr(2, 2);
        if (_str2_nozzle_type.count(type_str) != 0)
        {
            nozzle.m_nozzle_type = _str2_nozzle_type[type_str];
        }
    }
}


void DevNozzleSystemParser::ParseV1_0(const nlohmann::json& nozzletype_json,
                                      const nlohmann::json& diameter_json,
                                      DevNozzleSystem* system,
                                      std::optional<int> flag_e3d)
{
    //Since both the old and new protocols push data.
   // assert(system->m_nozzles.size() < 2);
    DevNozzle nozzle;
    nozzle.m_nozzle_id = 0;
    nozzle.m_nozzle_flow = NozzleFlowType::S_FLOW; // default flow type

    {
        float nozzle_diameter = 0.0f;
        if (diameter_json.is_number_float())
        {
            nozzle_diameter = diameter_json.get<float>();
        }
        else if (diameter_json.is_string())
        {
            nozzle_diameter = DevUtil::string_to_float(diameter_json.get<std::string>());
        }

        if (nozzle_diameter == 0.0f)
        {
            nozzle.m_diameter = 0.0f;
        }
        else
        {
            nozzle.m_diameter = round(nozzle_diameter * 10) / 10;
        }
    }

    {
        if (nozzletype_json.is_string())
        {
            s_parse_nozzle_type(nozzletype_json.get<std::string>(), nozzle);
        }
    }

    {
        if (flag_e3d.has_value()) {
            // 0: BBL S_FLOW; 1:E3D H_FLOW (only P)
            if (flag_e3d.value() == 1) {
                // note: E3D = E3D nozzle type + High Flow
                nozzle.m_nozzle_flow = NozzleFlowType::H_FLOW;
                nozzle.m_nozzle_type = NozzleType::ntE3D;
            } else {
                nozzle.m_nozzle_flow = NozzleFlowType::S_FLOW;
            }
        }
    }

    system->m_nozzles[nozzle.m_nozzle_id] = nozzle;
}


void DevNozzleSystemParser::ParseV2_0(const json& nozzle_json, DevNozzleSystem* system)
{
    system->Reset();

    if (nozzle_json.contains("exist"))
    {
        system->m_extder_exist = DevUtil::get_flag_bits(nozzle_json["exist"].get<int>(), 0, 16);
    }

    if (nozzle_json.contains("state"))
    {
        system->m_state = DevUtil::get_flag_bits(nozzle_json["state"].get<int>(), 0, 4);
    }

    for (auto it = nozzle_json["info"].begin(); it != nozzle_json["info"].end(); it++)
    {
        DevNozzle nozzle_obj;
        const auto& njon = it.value();
        nozzle_obj.m_nozzle_id = njon["id"].get<int>();
        nozzle_obj.m_diameter = njon["diameter"].get<float>();
        s_parse_nozzle_type(njon["type"].get<std::string>(), nozzle_obj);
        system->m_nozzles[nozzle_obj.m_nozzle_id] = nozzle_obj;
    }
}
}