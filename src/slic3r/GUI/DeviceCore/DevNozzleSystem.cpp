#include "DevExtruderSystem.h"

#include "DevNozzleRack.h"
#include "DevNozzleSystem.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

namespace Slic3r
{

// ---- DevNozzle: flow/volume conversions (Standard / High Flow / TPU High Flow) ----------------------
// Device-reported U_FLOW nozzles map to nvtTPUHighFlow so a synced TPU-HF rack activates the H2C
// change_filament_gcode TPU-kit branch. nvtHybrid is a slicer-only sentinel with no device
// representation, so ToNozzleFlowType(nvtHybrid) falls through to NONE_FLOWTYPE.

NozzleFlowType DevNozzle::ToNozzleFlowType(const NozzleVolumeType& type)
{
    switch (type) {
        case NozzleVolumeType::nvtStandard:    return NozzleFlowType::S_FLOW;
        case NozzleVolumeType::nvtHighFlow:    return NozzleFlowType::H_FLOW;
        case NozzleVolumeType::nvtTPUHighFlow: return NozzleFlowType::U_FLOW;
        default: return NozzleFlowType::NONE_FLOWTYPE;
    }
}

NozzleVolumeType DevNozzle::ToNozzleVolumeType(const NozzleFlowType& type)
{
    switch (type) {
        case NozzleFlowType::S_FLOW: return NozzleVolumeType::nvtStandard;
        case NozzleFlowType::H_FLOW: return NozzleVolumeType::nvtHighFlow;
        case NozzleFlowType::U_FLOW: return NozzleVolumeType::nvtTPUHighFlow;
        default: {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << "nozzle flow type None convert to nozzle volume type Standard";
            return NozzleVolumeType::nvtStandard;
        }
    }
}

wxString DevNozzle::GetNozzleFlowTypeStr(NozzleFlowType type)
{
    switch (type) {
    case NozzleFlowType::H_FLOW: return _L("High Flow");
    case NozzleFlowType::S_FLOW: return _L("Standard");
    case NozzleFlowType::U_FLOW: return _L("TPU High Flow");
    default: break;
    }

    return _L("Unknown");
}

// Untranslated flow-type literal used for filaments_blacklist.json nozzle_flows matching.
// Keep the raw English strings; the translated GetNozzleFlowTypeStr must not be used for JSON matching.
std::string DevNozzle::GetNozzleFlowTypeString(NozzleFlowType type)
{
    switch (type) {
        case NozzleFlowType::H_FLOW: return "High Flow";
        case NozzleFlowType::S_FLOW: return "Standard";
        case NozzleFlowType::U_FLOW: return "TPU High Flow";
        default: return "Unknown";
    }
}

// Untranslated flow-type literal serialized into the get_auto_nozzle_mapping payload.
// Differs from GetNozzleFlowTypeString only in the fallback: "" (empty) rather than "Unknown".
std::string DevNozzle::ToNozzleFlowString(const NozzleFlowType& type)
{
    switch (type) {
        case NozzleFlowType::S_FLOW: return "Standard";
        case NozzleFlowType::H_FLOW: return "High Flow";
        case NozzleFlowType::U_FLOW: return "TPU High Flow";
        default: return std::string();
    }
}

wxString DevNozzle::GetNozzleTypeStr(NozzleType type)
{
    switch (type) {
    case Slic3r::ntHardenedSteel:   return _L("Hardened Steel");
    case Slic3r::ntStainlessSteel:  return _L("Stainless Steel");
    case Slic3r::ntTungstenCarbide: return _L("Tungsten Carbide");
    default: break;
    }

    return _L("Unknown");
}

// ---- DevNozzle: status ------------------------------------------------------------------------------

bool DevNozzle::IsInfoReliable() const
{
    if (IsEmpty()) { return false; }
    return DevUtil::get_flag_bits(m_stat, 0, 1) == 0;
}

bool DevNozzle::IsNormal() const
{
    if (IsEmpty()) { return false; }
    return DevUtil::get_flag_bits(m_stat, 1, 2) == 0;
}

bool DevNozzle::IsAbnormal() const
{
    return DevUtil::get_flag_bits(m_stat, 1, 2) == (1 << 0);
}

bool DevNozzle::IsUnknown() const
{
    return DevUtil::get_flag_bits(m_stat, 1, 2) == (1 << 1);
}

int DevNozzle::GetNozzlePosId() const
{
    return IsOnRack() ? (m_nozzle_id + 0x10) : m_nozzle_id;
}

NozzleDiameterType DevNozzle::GetNozzleDiameterType() const
{
    if (is_approx(m_diameter, 0.2f))
        return NozzleDiameterType::NOZZLE_DIAMETER_0_2;
    else if (is_approx(m_diameter, 0.4f))
        return NozzleDiameterType::NOZZLE_DIAMETER_0_4;
    else if (is_approx(m_diameter, 0.6f))
        return NozzleDiameterType::NOZZLE_DIAMETER_0_6;
    else if (is_approx(m_diameter, 0.8f))
        return NozzleDiameterType::NOZZLE_DIAMETER_0_8;
    else
        return NozzleDiameterType::NONE_DIAMETER_TYPE;
}

DevFirmwareVersionInfo DevNozzle::GetFirmwareInfo() const
{
    const auto& nozzle_rack = m_nozzle_rack.lock();
    if (nozzle_rack) {
        if (IsOnRack()) {
            return nozzle_rack->GetNozzleFirmwareInfo(m_nozzle_id);
        } else if (m_nozzle_id == 0) {
            return nozzle_rack->GetNozzleSystem()->GetExtruderNozzleFirmware();
        }
    }

    return DevFirmwareVersionInfo();
}

// ---- DevNozzle: location ----------------------------------------------------------------------------

int DevNozzle::GetLogicExtruderId() const
{
    int total_ext_count = GetTotalExtruderCount();
    if (total_ext_count == 1) {
        return LOGIC_UNIQUE_EXTRUDER_ID;
    } else if (total_ext_count == 2) {
        if (AtLeftExtruder()) {
            return LOGIC_L_EXTRUDER_ID;
        } else if (AtRightExtruder()) {
            return LOGIC_R_EXTRUDER_ID;
        }
    }

    assert(0);
    return LOGIC_UNIQUE_EXTRUDER_ID;
}

int DevNozzle::GetExtruderId() const
{
    int total_ext_count = GetTotalExtruderCount();
    if (total_ext_count == 1) {
        return MAIN_EXTRUDER_ID;
    } else if (total_ext_count == 2) {
        if (AtRightExtruder()) {
            return MAIN_EXTRUDER_ID;
        } else if (AtLeftExtruder()) {
            return DEPUTY_EXTRUDER_ID;
        }
    }

    return MAIN_EXTRUDER_ID;
}

bool DevNozzle::AtLeftExtruder() const
{
    assert(GetTotalExtruderCount() == 2);
    if (IsOnRack()) { return false; }
    return m_nozzle_id == DEPUTY_EXTRUDER_ID;
}

bool DevNozzle::AtRightExtruder() const
{
    assert(GetTotalExtruderCount() == 2);
    if (IsOnRack()) { return true; }
    return m_nozzle_id == MAIN_EXTRUDER_ID;
}

int DevNozzle::GetTotalExtruderCount() const
{
    auto rack = m_nozzle_rack.lock();
    if (rack) {
        MachineObject* obj = rack->GetNozzleSystem()->GetOwner();
        return obj->GetExtderSystem()->GetTotalExtderCount();
    }
    return 1;
}

// ---- DevNozzleSystem --------------------------------------------------------------------------------

DevNozzleSystem::DevNozzleSystem(MachineObject* owner)
    : m_owner(owner), m_nozzle_rack(std::make_shared<DevNozzleRack>(this))
{
}

DevNozzle DevNozzleSystem::GetExtNozzle(int id) const
{
    if (m_ext_nozzles.find(id) != m_ext_nozzles.end())
    {
        return m_ext_nozzles.at(id);
    }

    return DevNozzle();
}

DevNozzle DevNozzleSystem::GetRackNozzle(int idx) const
{
    return m_nozzle_rack->GetNozzle(idx);
}

const std::map<int, DevNozzle>& DevNozzleSystem::GetRackNozzles() const
{
    return m_nozzle_rack->GetRackNozzles();
}

void DevNozzleSystem::SetSupportNozzleRack(bool supported)
{
    m_nozzle_rack->SetSupported(supported);
}

const std::vector<DevNozzle> DevNozzleSystem::CollectNozzles(int ext_loc, NozzleFlowType flow_type, float diameter) const
{
    auto s_match = [&](const DevNozzle& nozzle) -> bool {
        if (nozzle.IsEmpty() || nozzle.IsAbnormal()) {
            return false;
        }

        if (diameter >= 0.0f && nozzle.m_diameter != diameter) {
            return false;
        }

        if (m_owner->is_nozzle_flow_type_supported() && flow_type != nozzle.GetNozzleFlowType()) {
            return false;
        }

        return true;
    };

    std::vector<DevNozzle> result;

    auto ext_nozzle = GetExtNozzle(ext_loc);
    if (s_match(ext_nozzle)) {
        result.push_back(ext_nozzle);
    }

    if (ext_loc == MAIN_EXTRUDER_ID) {
        auto rack_nozzles = m_nozzle_rack->GetRackNozzles();
        for (auto rack_nozzle : rack_nozzles) {
            if (s_match(rack_nozzle.second)) {
                result.push_back(rack_nozzle.second);
            }
        }
    }

    return result;
}

std::vector<MultiNozzleUtils::NozzleGroupInfo> DevNozzleSystem::GetNozzleGroups() const
{
    std::vector<MultiNozzleUtils::NozzleGroupInfo> nozzle_groups;

    auto nozzle_in_extruder = this->GetExtNozzles();
    for (auto& elem : nozzle_in_extruder) {
        auto& nozzle = elem.second;
        MultiNozzleUtils::NozzleGroupInfo info;
        info.extruder_id = nozzle.GetLogicExtruderId();
        info.diameter = format_diameter_to_str(nozzle.m_diameter);
        info.volume_type = DevNozzle::ToNozzleVolumeType(nozzle.m_nozzle_flow);
        info.nozzle_count = 1;
        nozzle_groups.emplace_back(std::move(info));
    }

    auto nozzle_rack = this->GetNozzleRack();
    if (!nozzle_rack)
        return nozzle_groups;

    auto nozzle_in_rack = nozzle_rack->GetRackNozzles(); // nozzles in rack
    for (auto& elem : nozzle_in_rack) {
        auto& nozzle = elem.second;
        if (nozzle.IsUnknown() || nozzle.IsAbnormal())
            continue;
        int extruder_id = nozzle.GetLogicExtruderId();
        std::string diameter = format_diameter_to_str(nozzle.m_diameter);
        NozzleVolumeType volume_type = DevNozzle::ToNozzleVolumeType(nozzle.m_nozzle_flow);

        bool found = false;
        for (auto& group : nozzle_groups) {
            if (group.extruder_id == extruder_id &&
                group.volume_type == volume_type &&
                group.diameter == diameter) {
                found = true;
                group.nozzle_count += 1;
                break;
            }
        }
        if (!found)
            nozzle_groups.emplace_back(diameter, volume_type, extruder_id, 1);
    }
    return nozzle_groups;
}

bool DevNozzleSystem::IsRackMaximumInstalled() const
{
    auto ext_nozzle = GetExtNozzle(MAIN_EXTRUDER_ID);
    if (ext_nozzle.IsEmpty()) {
        return false;
    }

    const auto& rack_nozzles = m_nozzle_rack->GetRackNozzles();
    if (rack_nozzles.size() < 6) {
        return false;
    }

    for (auto item : rack_nozzles) {
        if (item.second.IsEmpty()) {
            return false;
        }
    }

    return true;
}

bool DevNozzleSystem::HasUnreliableNozzles() const
{
    for (auto nozzle : m_ext_nozzles) {
        if (!nozzle.second.IsInfoReliable()) {
            return true;
        }
    }

    if (m_nozzle_rack->HasUnreliableNozzles()) {
        return true;
    }

    return false;
}

bool DevNozzleSystem::HasUnknownNozzles() const
{
    for (auto nozzle : m_ext_nozzles) {
        if (nozzle.second.IsUnknown()) {
            return true;
        }
    }

    if (m_nozzle_rack->HasUnknownNozzles()) {
        return true;
    }

    return false;
}

void DevNozzleSystem::AddFirmwareInfoWTM(const DevFirmwareVersionInfo& info)
{
    static const std::string s_wtm_prefix = "wtm/";
    auto pos = info.name.find(s_wtm_prefix);
    if (pos == std::string::npos) {
        m_ext_nozzle_firmware_info = info;
    } else {
        try {
            auto str = info.name.substr(s_wtm_prefix.size()); // remove "wtm/" prefix
            int rack_nozzle_id = std::stoi(str) - 0x10; // rack nozzle IDs start from 0x10
            m_nozzle_rack->AddNozzleFirmwareInfo(rack_nozzle_id, info);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Invalid nozzle ID in firmware name:  "
                                     << info.name << ", error: " << e.what();
        }
    }
}

void DevNozzleSystem::ClearFirmwareInfoWTM()
{
    m_ext_nozzle_firmware_info = DevFirmwareVersionInfo();
    m_nozzle_rack->ClearNozzleFirmwareInfo();
}

void DevNozzleSystem::ClearNozzles()
{
    m_ext_nozzles.clear();
    m_nozzle_rack->ClearRackNozzles();
}

// ---- parsing ----------------------------------------------------------------------------------------

static unordered_map<string, NozzleFlowType> _str2_nozzle_flow_type = {
    {"S", NozzleFlowType::S_FLOW},
    {"H", NozzleFlowType::H_FLOW},
    {"A", NozzleFlowType::S_FLOW},
    {"X", NozzleFlowType::S_FLOW},
    {"E", NozzleFlowType::H_FLOW}, // E3D high-flow
    {"U", NozzleFlowType::U_FLOW}, // TPU 1.75 high-flow -> nvtTPUHighFlow
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
    else if (nozzle_type_str == "N/A")
    {
        nozzle.m_nozzle_type = NozzleType::ntUndefine;
        nozzle.m_nozzle_flow = NozzleFlowType::NONE_FLOWTYPE;
    }
}


void DevNozzleSystemParser::ParseV1_0(const nlohmann::json& nozzletype_json,
                                      const nlohmann::json& diameter_json,
                                      DevNozzleSystem* system,
                                      std::optional<int> flag_e3d)
{
    //Since both the old and new protocols push data.
   // assert(system->m_ext_nozzles.size() < 2);
    DevNozzle nozzle;
    nozzle.SetRack(system->GetNozzleRack());
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

    system->m_ext_nozzles[nozzle.m_nozzle_id] = nozzle;
}


void DevNozzleSystemParser::ParseV2_0(const json& device_json, DevNozzleSystem* system)
{
    if (device_json.contains("nozzle"))
    {
        const json& nozzle_json = device_json["nozzle"];
        if (nozzle_json.contains("exist"))
        {
            system->m_extder_exist = DevUtil::get_flag_bits(nozzle_json["exist"].get<int>(), 0, 16);
        }

        if (nozzle_json.contains("state"))
        {
            system->m_state_0_4 = DevUtil::get_flag_bits(nozzle_json["state"].get<int>(), 0, 4);
            int new_reading_idx = DevUtil::get_flag_bits(nozzle_json["state"].get<int>(), 8, 4);
            int new_reading_count = DevUtil::get_flag_bits(nozzle_json["state"].get<int>(), 4, 4);
            if (system->m_reading_count != new_reading_count || system->m_reading_idx != new_reading_idx)
            {
                system->m_reading_idx = new_reading_idx;
                system->m_reading_count = new_reading_count;
                if (system->m_reading_count == 0)
                {
                    system->m_nozzle_rack->SendReadingFinished();
                }
            }
        }

        // replace-nozzle state: a rack nozzle standing in for the toolhead position.
        // Absent for every non-rack printer's "nozzle" block => both stay std::nullopt (inert).
        if (nozzle_json.contains("src_id"))
        {
            system->m_replace_nozzle_src = std::make_optional(nozzle_json["src_id"].get<int>());
        }

        if (nozzle_json.contains("tar_id"))
        {
            system->m_replace_nozzle_tar = std::make_optional(nozzle_json["tar_id"].get<int>());
        }

        system->ClearNozzles();
        for (auto it = nozzle_json["info"].begin(); it != nozzle_json["info"].end(); it++)
        {
            DevNozzle nozzle_obj;
            nozzle_obj.SetRack(system->GetNozzleRack());

            const auto& njon = it.value();
            nozzle_obj.m_nozzle_id = DevUtil::get_hex_bits(njon["id"].get<int>(), 0);
            nozzle_obj.m_diameter = njon["diameter"].get<float>();
            s_parse_nozzle_type(njon["type"].get<std::string>(), nozzle_obj);
            if (njon.contains("stat"))/*maybe not contains*/
            {
                nozzle_obj.SetStatus(njon["stat"].get<int>());
            }

            DevJsonValParser::ParseVal(njon, "fila_id", nozzle_obj.m_fila_id);
            DevJsonValParser::ParseVal(njon, "wear", nozzle_obj.m_wear);
            if (njon.contains("p_t"))/*maybe not contains*/
            {
                nozzle_obj.m_nozzle_print_time = njon["p_t"].get<int>();
            }
            if (njon.contains("color_m"))/*maybe not contains*/
            {
                nozzle_obj.m_filament_clr = njon["color_m"].get<std::string>();
            }

            if (DevUtil::get_hex_bits(njon["id"].get<int>(), 1) == 1)
            {
                system->m_nozzle_rack->AddRackNozzle(nozzle_obj);
            }
            else
            {
                system->m_ext_nozzles[nozzle_obj.m_nozzle_id] = nozzle_obj;
            }
        }
    }

    if (device_json.contains("holder"))
    {
        system->m_nozzle_rack->ParseRackInfo(device_json["holder"]);
    }
}
};
