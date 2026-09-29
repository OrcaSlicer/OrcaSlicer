#include "DevExtruderSystem.h"
#include "DevFilaSystem.h"
#include "DevFilaSwitch.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r {

DevFilaSwitch::DevFilaSwitch(MachineObject *owner) { m_owner = owner; }

void DevFilaSwitch::Reset()
{
    m_is_installed = false;
    m_in_a_has_filament.reset();
    m_in_b_has_filament.reset();
    m_in_a_slot.reset();
    m_in_b_slot.reset();
    m_out_a_extruder_id.reset();
    m_out_b_extruder_id.reset();
    m_cali_status = CaliStatus::CALI_IDLE;
}

bool DevFilaSwitch::IsReady() const
{
    if (!m_is_installed) {
        BOOST_LOG_TRIVIAL(debug) << "[FilaSwitch] IsReady: not installed";
        return false;
    }

    const auto& ams_list = m_owner->GetFilaSystem()->GetAmsList();
    for (const auto& ams_item : ams_list) {
        if (ams_item.second->GetBindedExtruderSet().size() == 0) { return false; }
        if (!ams_item.second->GetSwitcherPos().has_value()) {
            return false;
        }
    }

    return true;
}

std::optional<DevAmsTray> DevFilaSwitch::GetInA_Slot() const
{
    auto ams_id_opt = GetInA_SlotId();
    if (m_owner && ams_id_opt.has_value()) { return m_owner->get_tray(std::to_string(ams_id_opt->first), std::to_string(ams_id_opt->first)); }

    return std::nullopt;
}

std::optional<DevAmsTray> DevFilaSwitch::GetInB_Slot() const
{
    auto ams_id_opt = GetInB_SlotId();
    if (m_owner && ams_id_opt.has_value()) { return m_owner->get_tray(std::to_string(ams_id_opt->first), std::to_string(ams_id_opt->first)); }

    return std::nullopt;
}

std::optional<DevExtder> DevFilaSwitch::GetOutA_Extruder() const
{
    auto ext_id_opt = GetOutA_ExtruderId();
    if (m_owner && ext_id_opt.has_value()) { return m_owner->GetExtderSystem()->GetExtderById(ext_id_opt.value()); }

    return std::nullopt;
}

std::optional<DevExtder> DevFilaSwitch::GetOutB_Extruder() const
{
    auto ext_id_opt = GetOutB_ExtruderId();
    if (m_owner && ext_id_opt.has_value()) { return m_owner->GetExtderSystem()->GetExtderById(ext_id_opt.value()); }

    return std::nullopt;
}

void DevFilaSwitch::ParseFilaSwitchInfo(const nlohmann::json &print_jj)
{
    if (print_jj.contains("aux")) {
        const auto &info_bits = DevJsonValParser::GetVal<std::string>(print_jj, "aux");
        if (m_is_installed != (DevUtil::get_flag_bits(info_bits, 29, 1) == 1)) {
            m_is_installed = (DevUtil::get_flag_bits(info_bits, 29, 1) == 1);
            if (!m_is_installed) { Reset(); }
        };
    }

    if (print_jj.contains("device") && print_jj["device"].contains("fila_switch")) {
        const auto &fila_switch_jj = print_jj["device"]["fila_switch"];
        if (fila_switch_jj.contains("in")) {
            const auto &in_vec = DevJsonValParser::GetVal<std::vector<int>>(fila_switch_jj, "in");
            if (in_vec.size() == 2) {
                if (in_vec[0] != -1) {
                    DevAmsSlotId slot_id;
                    slot_id.first  = DevUtil::get_flag_bits(in_vec[0], 8, 8);
                    slot_id.second = DevUtil::get_flag_bits(in_vec[0], 0, 8);
                    m_in_b_slot    = slot_id;
                } else {
                    m_in_b_slot = std::nullopt;
                };

                if (in_vec[1] != -1) {
                    DevAmsSlotId slot_id;
                    slot_id.first  = DevUtil::get_flag_bits(in_vec[1], 8, 8);
                    slot_id.second = DevUtil::get_flag_bits(in_vec[1], 0, 8);
                    m_in_a_slot    = slot_id;
                } else {
                    m_in_a_slot.reset();
                };
            }
        }

        if (fila_switch_jj.contains("out")) {
            const auto &out_vec = DevJsonValParser::GetVal<std::vector<int>>(fila_switch_jj, "out");
            if (out_vec.size() == 2) {
                if (out_vec[0] != 0xE) {
                    m_out_b_extruder_id = out_vec[0];
                } else {
                    m_out_b_extruder_id.reset();
                }

                if (out_vec[1] != 0xE) {
                    m_out_a_extruder_id = out_vec[1];
                } else {
                    m_out_a_extruder_id.reset();
                }
            }
        }

        if (fila_switch_jj.contains("stat")) { m_cali_status = DevJsonValParser::GetVal<CaliStatus>(fila_switch_jj, "stat"); }

        if (fila_switch_jj.contains("info")) {
            const auto &info_bits = DevJsonValParser::GetVal<int>(fila_switch_jj, "info");
            m_in_b_has_filament   = DevUtil::get_flag_bits(info_bits, 0, 1);
            m_in_a_has_filament   = DevUtil::get_flag_bits(info_bits, 0, 1);
        }
    }
}

}; // namespace Slic3r