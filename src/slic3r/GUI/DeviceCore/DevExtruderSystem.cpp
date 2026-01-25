#include <nlohmann/json.hpp>
#include "DevExtruderSystem.h"
#include "DevNozzleSystem.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "DevUtil.h"

using namespace nlohmann;

namespace Slic3r
{
    wxString DevExtder::GetDisplayLoc() const
    {
        if (system->GetTotalExtderCount() == 2)
        {
            if (m_ext_id == MAIN_EXTRUDER_ID)
            {
                return _L("right");
            }
            else
            {
                return _L("left");
            }
        }

        return wxEmptyString;
    }

    wxString DevExtder::GetDisplayName() const
    {
        if (system->GetTotalExtderCount() == 2)
        {
            if (m_ext_id == MAIN_EXTRUDER_ID)
            {
                return _L("right extruder");
            }
            else
            {
                return _L("left extruder");
            }
        }

        return _L("extruder");
    }

    NozzleType DevExtder::GetNozzleType() const
    {
        return system->Owner()->GetNozzleSystem()->GetNozzle(m_current_nozzle_id).m_nozzle_type;
    }

    NozzleFlowType DevExtder::GetNozzleFlowType() const
    {
        return system->Owner()->GetNozzleSystem()->GetNozzle(m_current_nozzle_id).m_nozzle_flow;
    }

    float DevExtder::GetNozzleDiameter() const
    {
        return system->Owner()->GetNozzleSystem()->GetNozzle(m_current_nozzle_id).m_diameter;
    }

    DevExtderSystem::DevExtderSystem(MachineObject* obj)
        : m_owner(obj)
    {
        // default to have one extruder
        m_total_extder_count = 1;
        DevExtder ext(this, MAIN_EXTRUDER_ID);
        m_extders.emplace_back(ext);
    }

    bool DevExtderSystem::CanQuitSwitching() const
    {
        if (!IsSwitchingFailed())
        {
            return false;
        }
        return !m_owner->is_in_printing() && !m_owner->is_in_printing_pause();
    }

    std::optional<DevExtder> DevExtderSystem::GetCurrentExtder() const
    {
        return GetExtderById(m_current_extder_id);
    }

    std::optional<DevExtder> DevExtderSystem::GetLoadingExtder() const
    {
        if (m_current_loading_extder_id != INVALID_EXTRUDER_ID)
        {
            return GetExtderById(m_current_loading_extder_id);
        }

        return std::nullopt;
    }

    std::optional<DevExtder> DevExtderSystem::GetExtderById(int extder_id) const
    {
        if (extder_id >= m_extders.size())
        {
            assert(false && "Invalid extruder ID");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "No extruder found for " << extder_id;
            return std::nullopt;
        }

        return m_extders[extder_id];
    }

    std::string DevExtderSystem::GetCurrentAmsId() const
    {
        auto cur_extder = GetCurrentExtder();
        return cur_extder ? cur_extder->GetSlotNow().ams_id : DevAmsSlotInfo().ams_id;
    }

    std::string DevExtderSystem::GetCurrentSlotId() const
    {
        auto cur_extder = GetCurrentExtder();
        return cur_extder ? GetCurrentExtder()->GetSlotNow().slot_id : DevAmsSlotInfo().slot_id;
    }

    std::string DevExtderSystem::GetTargetAmsId() const
    {
        auto cur_extder = GetCurrentExtder();
        return cur_extder ? GetCurrentExtder()->GetSlotTarget().ams_id : DevAmsSlotInfo().ams_id;
    }

    std::string DevExtderSystem::GetTargetSlotId() const
    {
        auto cur_extder = GetCurrentExtder();
        return cur_extder ? GetCurrentExtder()->GetSlotTarget().slot_id : DevAmsSlotInfo().slot_id;
    }

    bool DevExtderSystem::HasFilamentBackup() const
    {
        for (const auto& ext : m_extders)
        {
            if (ext.HasFilamBackup())
            {
                return true;
            }
        }

        return false;
    }

    void ExtderSystemParser::ParseV1_0(const nlohmann::json& print_json, DevExtderSystem* system)
    {
        if (system->GetTotalExtderCount() != 1)
        {
            return;
        }

        if (print_json.contains("nozzle_temper") && print_json["nozzle_temper"].is_number())
        {
            system->m_extders[MAIN_EXTRUDER_ID].m_cur_temp = print_json["nozzle_temper"].get<float>();
        }

        if (print_json.contains("nozzle_target_temper") && print_json["nozzle_target_temper"].is_number())
        {
            system->m_extders[MAIN_EXTRUDER_ID].m_target_temp = print_json["nozzle_target_temper"].get<float>();
        }

        if (print_json.contains("ams") && print_json["ams"].contains("tray_tar"))
        {
            const std::string& tray_tar = print_json["ams"]["tray_tar"].get<std::string>();
            if (!tray_tar.empty())
            {
                int tray_tar_int = atoi(tray_tar.c_str());
                if (tray_tar_int == VIRTUAL_TRAY_MAIN_ID) /*255 means unloading*/ {
                    system->m_extders[MAIN_EXTRUDER_ID].m_star.ams_id = "";
                    system->m_extders[MAIN_EXTRUDER_ID].m_star.slot_id = std::to_string(VIRTUAL_TRAY_MAIN_ID);
                }
                else if (tray_tar_int == VIRTUAL_TRAY_DEPUTY_ID) /*254 means loading ext spool*/ {
                    system->m_extders[MAIN_EXTRUDER_ID].m_star.ams_id = std::to_string(VIRTUAL_TRAY_MAIN_ID);
                    system->m_extders[MAIN_EXTRUDER_ID].m_star.slot_id = "0";
                }
                else
                {
                    if (tray_tar_int >= 0x80 && tray_tar_int <= 0x87)
                    {
                        system->m_extders[MAIN_EXTRUDER_ID].m_star.ams_id = std::to_string(tray_tar_int);
                    }
                    else
                    {
                        system->m_extders[MAIN_EXTRUDER_ID].m_star.ams_id = std::to_string(tray_tar_int >> 2);
                    }

                    system->m_extders[MAIN_EXTRUDER_ID].m_star.slot_id = std::to_string(tray_tar_int & 0x3);
                }
            }
        }

        if (print_json.contains("ams") && print_json["ams"].contains("tray_now"))
        {
            const std::string& tray_now = print_json["ams"]["tray_now"].get<std::string>();
            if (!tray_now.empty())
            {
                int tray_now_int = atoi(tray_now.c_str());
                if (tray_now_int == VIRTUAL_TRAY_MAIN_ID)
                {
                    system->m_extders[MAIN_EXTRUDER_ID].m_snow.ams_id = "";
                    system->m_extders[MAIN_EXTRUDER_ID].m_snow.slot_id = "";
                }
                else if (tray_now_int == VIRTUAL_TRAY_DEPUTY_ID)
                {
                    system->m_extders[MAIN_EXTRUDER_ID].m_snow.ams_id = std::to_string(VIRTUAL_TRAY_MAIN_ID);
                    system->m_extders[MAIN_EXTRUDER_ID].m_snow.slot_id = "0";
                }
                else
                {
                    if (tray_now_int >= 0x80 && tray_now_int <= 0x87)
                    {
                        system->m_extders[MAIN_EXTRUDER_ID].m_snow.ams_id = std::to_string(tray_now_int);
                    }
                    else
                    {
                        system->m_extders[MAIN_EXTRUDER_ID].m_snow.ams_id = std::to_string(tray_now_int >> 2);
                    }

                    system->m_extders[MAIN_EXTRUDER_ID].m_snow.slot_id = std::to_string(tray_now_int & 0x3);
                }
            }
        }

        system->m_current_busy_for_loading = false;
        system->m_current_loading_extder_id = INVALID_EXTRUDER_ID;
        if (!system->m_extders[MAIN_EXTRUDER_ID].m_star.ams_id.empty())
        {
            system->m_current_busy_for_loading = (system->m_extders[MAIN_EXTRUDER_ID].m_snow == system->m_extders[MAIN_EXTRUDER_ID].m_star);
            if (system->m_current_busy_for_loading)
            {
                system->m_current_loading_extder_id = MAIN_EXTRUDER_ID;
            }
        }
    }

    void ExtderSystemParser::ParseV2_0(const nlohmann::json& extruder_json, DevExtderSystem* system)
    {
        system->m_total_extder_count = DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 0, 4);
        if (system->m_current_extder_id != DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 4, 4))
        {
            system->Owner()->targ_nozzle_id_from_pc = INVALID_EXTRUDER_ID;
            system->m_current_extder_id = DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 4, 4);
        }
        else if (system->m_switch_extder_state == ES_SWITCHING_FAILED)
        {
            system->Owner()->targ_nozzle_id_from_pc = INVALID_EXTRUDER_ID;
        }

        system->m_target_extder_id = DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 8, 4);
        system->m_switch_extder_state = (DevExtderSwitchState)DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 12, 3);
        system->m_current_loading_extder_id = DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 15, 4);
        system->m_current_busy_for_loading = DevUtil::get_flag_bits(extruder_json["state"].get<int>(), 19);

        system->m_extders.clear();
        for (auto it = extruder_json["info"].begin(); it != extruder_json["info"].cend(); it++)
        {

            DevExtder extder_obj(system);

            const auto&  njon = it.value();
            extder_obj.SetExtId(njon["id"].get<int>());

            extder_obj.m_filam_bak.clear();
            const json& filam_bak_items = njon["filam_bak"];
            for (const auto& filam_bak_item : filam_bak_items)
            {
                const auto& filam_bak_val = filam_bak_item.get<int>();
                extder_obj.m_filam_bak.emplace_back(filam_bak_val);
            }

            extder_obj.m_ext_has_filament = (DevUtil::get_flag_bits(njon["info"].get<int>(), 1) != 0);
            extder_obj.m_buffer_has_filament = (DevUtil::get_flag_bits(njon["info"].get<int>(), 2) != 0);
            extder_obj.m_has_nozzle = (DevUtil::get_flag_bits(njon["info"].get<int>(), 3) != 0);
            extder_obj.m_cur_temp = DevUtil::get_flag_bits(njon["temp"].get<int>(), 0, 16);
            extder_obj.m_target_temp = DevUtil::get_flag_bits(njon["temp"].get<int>(), 16, 16);

            extder_obj.m_spre.slot_id = std::to_string(DevUtil::get_flag_bits(njon["spre"].get<int>(), 0, 8));
            extder_obj.m_spre.ams_id = std::to_string(DevUtil::get_flag_bits(njon["spre"].get<int>(), 8, 8));

            extder_obj.m_snow.slot_id = std::to_string(DevUtil::get_flag_bits(njon["snow"].get<int>(), 0, 8));
            extder_obj.m_snow.ams_id = std::to_string(DevUtil::get_flag_bits(njon["snow"].get<int>(), 8, 8));

            extder_obj.m_star.slot_id = std::to_string(DevUtil::get_flag_bits(njon["star"].get<int>(), 0, 8));
            extder_obj.m_star.ams_id = std::to_string(DevUtil::get_flag_bits(njon["star"].get<int>(), 8, 8));

            extder_obj.m_ams_stat = DevUtil::get_flag_bits(njon["stat"].get<int>(), 0, 16);
            extder_obj.m_rfid_stat = DevUtil::get_flag_bits(njon["stat"].get<int>(), 16, 16);

            //current nozzle info
            extder_obj.m_current_nozzle_id = njon["hnow"].get<int>();
            system->m_extders.push_back(extder_obj);
        }
    }
}