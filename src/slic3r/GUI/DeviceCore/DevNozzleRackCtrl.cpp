#include "DevNozzleRack.h"
#include "DevUtil.h"
#include "DevExtruderSystem.h"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <wx/thread.h>


namespace Slic3r
{

void DevNozzleRack::CtrlRackPosMove(RackPos new_pos) const
{
    if (!CheckRackMoveWarningDlg())
    {
        return;
    }

    int action_id = -1;
    if (new_pos == RACK_POS_A_TOP)
    {
        action_id = 1;
    }
    else if(new_pos == RACK_POS_B_TOP)
    {
        action_id = 2;
    }

    if (action_id != -1)
    {
        json j;
        j["print"]["command"] = "nozzle_holder_ctrl";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["action"] = action_id;
        m_nozzle_system->GetOwner()->publish_json(j);
    }
}


void DevNozzleRack::CtrlRackPosGoHome() const
{
    if (!CheckRackMoveWarningDlg())
    {
        return;
    }

    json j;
    j["print"]["command"] = "nozzle_holder_ctrl";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["action"] = 0;
    m_nozzle_system->GetOwner()->publish_json(j);
}

bool DevNozzleRack::CheckRackMoveWarningDlg() const
{
    if (wxThread::IsMain())
    {
        static bool s_show_move_warning = true;
        if (s_show_move_warning)
        {
            Slic3r::GUI::MessageDialog dlg(nullptr, _L("The toolhead and hotend rack may move. Please keep your hands away from the chamber."),
                _L("Warning"), wxICON_WARNING | wxOK);
            dlg.show_dsa_button();
            if (dlg.ShowModal() != wxID_OK)
            {
                s_show_move_warning = !dlg.get_checkbox_state();
                return false;
            }

            s_show_move_warning = !dlg.get_checkbox_state();
        }
    }

    return true;
}

void DevNozzleRack::CtrlRackConfirmNozzle(int rack_nozzle_id) const
{
    json j;
    j["print"]["command"] = "nozzle_info_confirm";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["id"] = (rack_nozzle_id + 16); // from 0x16
    m_nozzle_system->GetOwner()->publish_json(j);
}

void DevNozzleRack::CtrlRackConfirmAll() const
{
    json j;
    j["print"]["command"] = "nozzle_info_confirm";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["id"] = 0xff;
    m_nozzle_system->GetOwner()->publish_json(j);
}

void DevNozzleRack::CrtlRackReadNozzle(int rack_nozzle_id) const
{
    json j;
    j["print"]["command"] = "holder_nozzle_refresh";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["id"] = (rack_nozzle_id + 16); // from 0x16
    m_nozzle_system->GetOwner()->publish_json(j);
}


bool DevNozzleRack::CtrlCanReadAll() const
{
    //is in print
    if (m_nozzle_system->GetOwner()->is_in_printing())
    {
        return false;
    }

    if (m_nozzle_system->GetOwner()->is_in_upgrading()) {
        return false;
    }

    //if have nozzle
    bool has_nozzle_on_rack = false;
    for (const auto &nozzle_pair : m_rack_nozzles)
    {
        if (!nozzle_pair.second.IsEmpty())
        {
            has_nozzle_on_rack = true;
            break;
        }
    }

    bool has_nozzle_on_ext = false;
    if (m_nozzle_system->ContainsExtNozzle(MAIN_EXTRUDER_ID)) {
        has_nozzle_on_ext = true;
    }

    if (!has_nozzle_on_rack && !has_nozzle_on_ext) {
        return false;
    }

    //if is in loading
    if (m_nozzle_system->GetOwner()->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE)
    {
        return false;
    }

    auto ext = m_nozzle_system->GetOwner()->GetExtderSystem();
    if (ext && ext->IsBusyLoading()) return false;

    if (GetReadingCount() > 0)
    {
        return false;
    }

    return m_status == RACK_STATUS_IDLE;
}

void DevNozzleRack::CtrlRackReadAll(bool gui_check) const
{
    if (gui_check && wxThread::IsMain())
    {
#if 0
        if (!HasUnknownNozzles()) {
            Slic3r::GUI::MessageDialog dlg(nullptr, _L("Hotend information may be inaccurate. "
                "Would you like to re-read the hotend? (Hotend information may change during power-off)."),
                _L("Warning"), wxICON_WARNING | wxOK | wxYES);
            dlg.SetButtonLabel(wxID_OK, _L("I confirm all"));
            dlg.SetButtonLabel(wxID_YES, _L("Re-read all"));

            int rtn = dlg.ShowModal();
            if (rtn == wxID_OK) {
                CtrlRackConfirmAll();
            } else if (rtn == wxID_YES) {
                if (CheckRackMoveWarningDlg()) {
                    CtrlRackReadAll(false);
                }
            }

            return;
        }
#endif

        if (CheckRackMoveWarningDlg()) {
            CtrlRackReadAll(false);
        }

        return;
    }

    json j;
    j["print"]["command"] = "holder_nozzle_refresh";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["id"] = 0xff;
    m_nozzle_system->GetOwner()->publish_json(j);
}


bool DevNozzleRack::CtrlCanUpdateAll() const
{
    if (m_nozzle_system->GetOwner()->is_in_printing())
    {
        return false;
    }

    auto ext_nozzle = m_nozzle_system->GetExtNozzle(MAIN_EXTRUDER_ID);
    if (ext_nozzle.IsAbnormal())
    {
        return true;
    }

    auto ext_nozzle_firmware = m_nozzle_system->GetExtruderNozzleFirmware();
    if (!ext_nozzle_firmware.sw_new_ver.empty() &&
        ext_nozzle_firmware.sw_new_ver != ext_nozzle_firmware.sw_ver)
    {
        return true;
    }

    for (auto val : m_rack_nozzles_firmware)
    {
        const auto& firmware_info = val.second;
        if (!firmware_info.sw_new_ver.empty() && firmware_info.sw_new_ver != firmware_info.sw_ver)
        {
            return true;
        }
        else if (GetNozzle(val.first).IsAbnormal())
        {
            return true;
        }
    }

    return false;
}

int DevNozzleRack::CtrlRackUpgradeExtruderNozzle() const
{
    return CtrlRackUpgrade("wtm");
}

int DevNozzleRack::CtrlRackUpgradeRackNozzle(int rack_nozzle_id) const
{
    return CtrlRackUpgrade("wtm/" + std::to_string(0x10 + rack_nozzle_id));
}

int DevNozzleRack::CtrlRackUpgradeAll() const
{
    return CtrlRackUpgrade("wtm_all");
}

int DevNozzleRack::CtrlRackUpgrade(const std::string& module_str) const
{
    if (wxThread::IsMain())
    {
        if (GetReadingCount() > 0)
        {
            Slic3r::GUI::MessageDialog dlg(nullptr, _L("Reading the hotends, please wait."), _L("Warning"), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return -1;
        }

        static bool s_show_upgrade_warning = true;
        if (s_show_upgrade_warning)
        {
            Slic3r::GUI::MessageDialog dlg(nullptr, _L("During the hotend upgrade, the toolhead will move. Don't reach into the chamber."),
                _L("Warning"), wxICON_WARNING | wxOK | wxCANCEL);
            dlg.show_dsa_button();
            dlg.SetButtonLabel(wxID_OK, _L("Update"));
            int rtn = dlg.ShowModal();
            s_show_upgrade_warning = !dlg.get_checkbox_state();
            if (rtn != wxID_OK)
            {
                return -1;
            }
        }
    }

    json j;
    j["upgrade"]["command"] = "wtm_upgrade";
    j["upgrade"]["module"] = module_str;
    j["upgrade"]["src_id"] = 1;
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    return m_nozzle_system->GetOwner()->publish_json(j);
}

};