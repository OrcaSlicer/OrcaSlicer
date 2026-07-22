/*****************************************************************//**
 * \file   AmsMappingPopupUpdate.cpp
 * \brief  do update for AmsMappingPopup
 * \note   moved from AmsMappingPopup.cpp to here to reduce the code size of AmsMappingPopup.cpp
 *
 * \author xin.zhang
 * \date   February 2026
 *********************************************************************/
#include "AmsMappingPopup.hpp"
#include "I18N.hpp"
#include "DeviceCore/DevConfigUtil.h"

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Preview.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/StaticBox.hpp"

#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <miniz.h>
#include <algorithm>
#include <optional>
#include "Plater.hpp"
#include "BitmapCache.hpp"

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevFilaSwitch.h"
#include "DeviceCore/DevMappingNozzle.h"
#include "DeviceCore/DevNozzleRack.h" // Orca: full type for GetNozzleSystem()->GetNozzleRack()->IsSupported()

#include "DeviceTab/wgtDeviceNozzleSelect.h"
#include "DeviceTab/wgtMsgPanel.h"

namespace Slic3r::GUI {


static void _add_containers(const AmsMapingPopup* win,
                            std::list<MappingContainer*>& one_slot_containers,
                            const std::vector<MappingContainer*>& four_slots_containers,
                            wxBoxSizer* target_sizer)
{
    for (auto container : four_slots_containers) { target_sizer->Add(container, 0, wxTOP, win->FromDIP(5)); }

    while (!one_slot_containers.empty()) {
        wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
        for (int i = 0; i < 3; i++) {
            if (one_slot_containers.empty()) { break; }

            sizer->Add(one_slot_containers.front(), 0, wxLEFT, (i == 0) ? 0 : win->FromDIP(5));
            one_slot_containers.pop_front();
        }

        target_sizer->Add(sizer, 0, wxTOP, win->FromDIP(5));
    }
}

void AmsMapingPopup::update(MachineObject* obj,
                            const std::vector<FilamentInfo>& ams_mapping_result,
                            bool use_dynamic_switch,
                            std::optional<PrintFromType> print_type)
{
    if (!obj) { return; } // Orca: guard before use (REF logs before its null check)

    BOOST_LOG_TRIVIAL(info) << "ams_mapping total count " << obj->GetFilaSystem()->GetAmsCount();

    for (auto& ams_container : m_amsmapping_container_list) {
        ams_container->Destroy();
    }

    m_amsmapping_container_list.clear();
    m_amsmapping_container_sizer_list.clear();
    m_mapping_item_list.clear();

    /*title*/
    update_title(obj);

    /*ams mapping*/
    update_ams_tips(obj);
    update_mapping_items(obj, ams_mapping_result, use_dynamic_switch);

    /*rack*/
    update_rack_select(obj, use_dynamic_switch, print_type);
    update_flush_waste(obj);

    if (wxGetApp().dark_mode() && m_reset_btn->GetName() != "erase_dark") {
        m_reset_btn->SetName("erase_dark");
        m_reset_btn->SetBitmap(ScalableBitmap(m_right_first_text_panel, "erase_dark", 14).bmp());
    } else if (!wxGetApp().dark_mode() && m_reset_btn->GetName() != "erase") {
        m_reset_btn->SetName("erase");
        m_reset_btn->SetBitmap(ScalableBitmap(m_right_first_text_panel, "erase", 14).bmp());
    }

    size_t extruder_num = obj->GetExtderSystem()->GetTotalExtderCount();
    if (extruder_num == 1) {
        m_left_marea_panel->Hide();
        m_left_extra_slot->Hide();
        m_split_line_panel->Hide();
        m_right_marea_panel->Show();
        m_right_marea_panel->Enable(true);
        m_right_extra_slot->Show();
        m_right_extra_slot->Enable(true);
        m_right_split_ext_sizer->Show(true);
        set_sizer_title(m_right_split_ams_sizer, _L("AMS"));
    } else if (extruder_num > 1) {
        if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
            m_left_marea_panel->Show(false);
            m_left_extra_slot->Show(false);
            m_right_extra_slot->Show(false);
            m_left_split_ams_sizer->Show(false);
            m_right_split_ams_sizer->Show(false);
            m_right_split_ext_sizer->Show(false);
            m_split_line_panel->Hide();

            m_right_marea_panel->Show();
            m_right_marea_panel->Enable(true);
        } else {

            m_left_marea_panel->Show();
            m_left_extra_slot->Show();
            m_split_line_panel->Show();
            m_right_marea_panel->Show();
            m_right_extra_slot->Show();
            m_right_split_ext_sizer->Show(true);
            m_left_tips->SetLabel(m_left_tip_text);
            m_right_tips->SetLabel(m_right_tip_text);
            set_sizer_title(m_left_split_ams_sizer, _L("Left AMS"));
            set_sizer_title(m_right_split_ams_sizer, _L("Right AMS"));
            if (m_show_type == ShowType::LEFT) {
                m_left_marea_panel->Enable(true);
                m_left_extra_slot->Enable(true);
                m_right_marea_panel->Enable(false);
                m_right_extra_slot->Enable(false);
                if (m_use_in_sync_dialog) {
                    m_left_tips->SetLabel(m_single_tip_text);
                    m_right_tips->SetLabel("");
                }
            } else if (m_show_type == ShowType::RIGHT) {
                m_left_marea_panel->Enable(false);
                m_left_extra_slot->Enable(false);
                m_right_marea_panel->Enable(true);
                m_right_extra_slot->Enable(true);
                if (m_use_in_sync_dialog) {
                    // m_right_tips->SetLabel(m_single_tip_text);
                    m_left_tips->SetLabel("");
                }
            } else if (m_show_type == ShowType::LEFT_AND_RIGHT) {
                m_left_marea_panel->Enable(true);
                m_left_extra_slot->Enable(true);
                m_right_marea_panel->Enable(true);
                m_right_extra_slot->Enable(true);
                if (m_use_in_sync_dialog) {
                    m_left_tips->SetLabel(m_single_tip_text);
                    m_right_tips->SetLabel("");
                }
            } else if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
                m_left_marea_panel->Show(false);
                m_left_extra_slot->Show(false);
                m_right_extra_slot->Show(false);
                m_left_split_ams_sizer->Show(false);
                m_right_split_ams_sizer->Show(false);
                m_right_split_ext_sizer->Show(false);
                m_split_line_panel->Hide();

                m_right_marea_panel->Show();
                m_right_marea_panel->Enable(true);
            }
        }
    }

    Layout();
    Fit();
    Refresh();
}

// Orca: the kept fila model has no tray-level ams_type; the owning AMS type is passed in
// (DevAms::GetAmsType() for AMS trays, DevAmsType::EXT_SPOOL for the virtual/ext slots).
static std::optional<TrayData> sGetTrayData(DevAmsTray* tray,
                                            const std::string& ams_id_str,
                                            DevAmsType ams_type)
{
    int ams_id = 0;
    int tray_id = 0;
    try {
        ams_id = std::stoi(ams_id_str);
        tray_id = atoi(tray->id.c_str());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":[error] invalid ams_id or tray_id: " << e.what();
        return std::nullopt;
    }

    TrayData td;
    td.ams_id = ams_id;
    if (ams_type != DevAmsType::EXT_SPOOL) {
        td.slot_id = tray_id;
    } else {
        td.slot_id = 0;
    }

    if (ams_type == DevAmsType::AMS ||
        ams_type == DevAmsType::AMS_LITE ||
        ams_type == DevAmsType::N3F) {
        td.id = ams_id * 4 + tray_id;
    } else if (ams_type == DevAmsType::N3S) {
        td.id = ams_id + tray_id;
    } else if(ams_type == DevAmsType::EXT_SPOOL){
        td.id = tray_id;
    }

    if (ams_type != EXT_SPOOL && !tray->is_exists) {
        td.type = EMPTY;
    } else {
        if (!tray->is_tray_info_ready()) {
            td.type = THIRD;
        } else {
            td.type = NORMAL;
            td.remain = tray->remain;
            td.colour = DevAmsTray::decode_color(tray->color);
            td.name = tray->get_display_filament_type();
            td.filament_type = tray->get_filament_type();
            td.ctype = tray->ctype;
            for (const auto& col : tray->cols) {
                td.material_cols.push_back(DevAmsTray::decode_color(col));
            }
        }
    }

    return td;
}

void AmsMapingPopup::update_mapping_items(MachineObject* obj, const std::vector<FilamentInfo>& ams_mapping_result, bool use_dynamic_switch)
{
    std::list<MappingContainer*>   left_one_slot_containers;
    std::list<MappingContainer*>   right_one_slot_containers;
    std::vector<MappingContainer*> left_four_slots_containers;
    std::vector<MappingContainer*> right_four_slot_containers;

    const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
    for (auto ams_iter = ams_list.begin(); ams_iter != ams_list.end(); ams_iter++) {
        const auto& extruder_set = ams_iter->second->GetBindedExtruderSet();
        if (extruder_set.size() <= 0) {
            continue;
        }

        wxPanel* target_panel = nullptr;
        if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
            target_panel = m_right_marea_panel;
        } else {
            int extruder_id = *extruder_set.begin();
            if (extruder_id == MAIN_EXTRUDER_ID) {
                target_panel = m_right_marea_panel;
            } else if (extruder_id == DEPUTY_EXTRUDER_ID) {
                target_panel = m_left_marea_panel;
            }
        }

        if (!target_panel) {
            continue;
        }

        auto sizer_mapping_list = new wxBoxSizer(wxHORIZONTAL);
        auto ams_mapping_item_container = new MappingContainer(target_panel, ams_iter->second->GetDisplayName(), ams_iter->second->GetSlotCount());
        ams_mapping_item_container->SetName(target_panel->GetName());
        ams_mapping_item_container->SetSizer(sizer_mapping_list);
        ams_mapping_item_container->Layout();

        BOOST_LOG_TRIVIAL(trace) << "ams_mapping ams id " << ams_iter->first.c_str();

        DevAms* ams_group = ams_iter->second;
        auto ams_type = ams_group->GetAmsType();
        std::vector<TrayData>                      tray_datas;
        std::map<std::string, DevAmsTray*>::const_iterator tray_iter;
        for (tray_iter = ams_group->GetTrays().cbegin(); tray_iter != ams_group->GetTrays().cend(); tray_iter++) {
            const auto& td_opt = sGetTrayData(tray_iter->second, ams_group->GetAmsId(), ams_type);
            if (td_opt.has_value()) {
                tray_datas.push_back(td_opt.value());
            }
        }

        ams_mapping_item_container->Show();
        add_ams_mapping(tray_datas, obj->GetFilaSystem()->IsDetectRemainEnabled(), ams_mapping_item_container, sizer_mapping_list);
        m_amsmapping_container_sizer_list.push_back(sizer_mapping_list);
        m_amsmapping_container_list.push_back(ams_mapping_item_container);

        if (target_panel == m_right_marea_panel) {
            if (ams_mapping_item_container->get_slots_num() == 1) {
                right_one_slot_containers.push_back(ams_mapping_item_container);
            } else {
                right_four_slot_containers.push_back(ams_mapping_item_container);
            }
        } else if (target_panel == m_left_marea_panel) {
            if (ams_mapping_item_container->get_slots_num() == 1) {
                left_one_slot_containers.push_back(ams_mapping_item_container);
            } else {
                left_four_slots_containers.push_back(ams_mapping_item_container);
            }
        }
    }

    for (int i = obj->vt_slot.size() - 1; i >= 0; i--) {
        DevAmsTray* tray_data = &obj->vt_slot[i];
        const auto& td_opt = sGetTrayData(tray_data, tray_data->id, DevAmsType::EXT_SPOOL);
        if (!td_opt.has_value()) {
            continue;
        }

        if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
            auto sizer_mapping_list = new wxBoxSizer(wxHORIZONTAL);
            const auto& shown_name = td_opt->ams_id == VIRTUAL_TRAY_MAIN_ID ? "Ext-R" : "Ext-L";
            auto ams_mapping_item_container = new MappingContainer(m_right_marea_panel, shown_name, 1);
            ams_mapping_item_container->SetName(m_right_marea_panel->GetName());
            ams_mapping_item_container->SetSizer(sizer_mapping_list);
            ams_mapping_item_container->Layout();

            std::vector<TrayData>  tray_datas;
            tray_datas.push_back(td_opt.value());
            add_ams_mapping(tray_datas, false, ams_mapping_item_container, sizer_mapping_list);
            m_amsmapping_container_sizer_list.push_back(sizer_mapping_list);
            m_amsmapping_container_list.push_back(ams_mapping_item_container);
            right_one_slot_containers.push_back(ams_mapping_item_container);
        } else {
            if (td_opt->ams_id == VIRTUAL_TRAY_MAIN_ID) {
                m_right_extra_slot->send_win = send_win;
                add_ext_ams_mapping(td_opt.value(), m_right_extra_slot);
            } else if (td_opt->ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
                m_left_extra_slot->send_win = send_win;
                add_ext_ams_mapping(td_opt.value(), m_left_extra_slot);
            }
        }
    }

    _add_containers(this, left_one_slot_containers, left_four_slots_containers, m_sizer_ams_basket_left);
    _add_containers(this, right_one_slot_containers, right_four_slot_containers, m_sizer_ams_basket_right);
    if (m_show_type != ShowType::LEFT_AND_RIGHT_DYNAMIC){
        m_left_split_ams_sizer->Show(!left_one_slot_containers.empty() || !left_four_slots_containers.empty());
        m_right_split_ams_sizer->Show(!right_one_slot_containers.empty() || !right_four_slot_containers.empty());
    } else {
        m_left_split_ams_sizer->Show(false);
        m_right_split_ams_sizer->Show(false);
    }

    update_items_check_state(ams_mapping_result);
}

void AmsMapingPopup::update_ams_data_multi_machines()
{
    m_mapping_from_multi_machines = true;

    std::vector<TrayData> tray_datas;
    for (int i = 0; i < 4; ++i) {
        TrayData td;
        td.id = i;
        td.type = EMPTY;
        td.colour = wxColour(166, 169, 170);
        td.name = "";
        td.filament_type = "";
        td.ctype = 0;
        tray_datas.push_back(td);
    }

    for (auto& ams_container : m_amsmapping_container_list) {
        ams_container->Destroy();
    }

    m_amsmapping_container_list.clear();
    m_amsmapping_container_sizer_list.clear();
    m_mapping_item_list.clear();

    if (wxGetApp().dark_mode() && m_reset_btn->GetName() != "erase_dark") {
        m_reset_btn->SetName("erase_dark");
        m_reset_btn->SetBitmap(ScalableBitmap(m_right_first_text_panel, "erase_dark", 14).bmp());
    } else if (!wxGetApp().dark_mode() && m_reset_btn->GetName() != "erase") {
        m_reset_btn->SetName("erase");
        m_reset_btn->SetBitmap(ScalableBitmap(m_right_first_text_panel, "erase", 14).bmp());
    }

    size_t nozzle_nums = 1;
    m_show_type = ShowType::RIGHT;

    m_left_marea_panel->Hide();
    m_left_extra_slot->Hide();
    // m_left_marea_panel->Show();
    m_right_marea_panel->Show();
    set_sizer_title(m_right_split_ams_sizer, _L("AMS"));
    // m_right_tips->SetLabel(m_single_tip_text);
    m_right_extra_slot->Hide();
    m_left_extra_slot->Hide();



    /*ams*/
    bool                            has_left_ams = false, has_right_ams = false;
    std::list<MappingContainer*>   left_one_slot_containers;
    std::list<MappingContainer*>   right_one_slot_containers;
    std::vector<MappingContainer*> left_four_slots_containers;
    std::vector<MappingContainer*> right_four_slot_containers;
    for (int i = 0; i < 1; i++) {
        int ams_indx = 0;
        int ams_type = 1;
        int nozzle_id = 0;

        if (ams_type >= 1 || ams_type <= 3) { // 1:ams 2:ams-lite 3:n3f

            auto sizer_mapping_list = new wxBoxSizer(wxHORIZONTAL);
            auto ams_mapping_item_container = new MappingContainer(nozzle_id == 0 ? m_right_marea_panel : m_left_marea_panel, "AMS-1", 4);
            ams_mapping_item_container->SetName(nozzle_id == 0 ? m_right_marea_panel->GetName() : m_left_marea_panel->GetName());
            ams_mapping_item_container->SetSizer(sizer_mapping_list);
            ams_mapping_item_container->Layout();

            m_has_unmatch_filament = false;
            ams_mapping_item_container->Show();
            add_ams_mapping(tray_datas, false, ams_mapping_item_container, sizer_mapping_list);
            m_amsmapping_container_sizer_list.push_back(sizer_mapping_list);
            m_amsmapping_container_list.push_back(ams_mapping_item_container);

            if (nozzle_id == 0) {
                has_right_ams = true;
                if (ams_mapping_item_container->get_slots_num() == 1) {
                    right_one_slot_containers.push_back(ams_mapping_item_container);
                } else {
                    right_four_slot_containers.push_back(ams_mapping_item_container);
                }
            } else if (nozzle_id == 1) {
                has_left_ams = true;
                if (ams_mapping_item_container->get_slots_num() == 1) {
                    left_one_slot_containers.push_back(ams_mapping_item_container);
                } else {
                    left_four_slots_containers.push_back(ams_mapping_item_container);
                }
            }
        } else if (ams_type == 4) { // 4:n3s
        }
    }

    _add_containers(this, left_one_slot_containers, left_four_slots_containers, m_sizer_ams_basket_left);
    _add_containers(this, right_one_slot_containers, right_four_slot_containers, m_sizer_ams_basket_right);
    m_left_split_ams_sizer->Show(has_left_ams);
    m_right_split_ams_sizer->Show(has_right_ams);
    //update_items_check_state(ams_mapping_result);

    Layout();
    Fit();
}


void AmsMapingPopup::update_title(MachineObject* obj)
{
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    size_t nozzle_nums = full_config.option<ConfigOptionFloatsNullable>("nozzle_diameter")->values.size();
    if (nozzle_nums > 1) {
        m_split_line_panel->Show();
        if (m_show_type == ShowType::LEFT) {
            wxString nozzle_name = obj ? _L(DevPrinterConfigUtil::get_toolhead_display_name(
                obj->printer_type, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase)) : _L("left nozzle");
            m_title_text->SetLabelText(wxString::Format(_L("Please select the filament installed on the %s."), nozzle_name));
            return;
        } else if (m_show_type == ShowType::RIGHT) {
            wxString nozzle_name = obj ? _L(DevPrinterConfigUtil::get_toolhead_display_name(
                obj->printer_type, MAIN_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase)) : _L("right nozzle");
            m_title_text->SetLabelText(wxString::Format(_L("Please select the filament installed on the %s."), nozzle_name));
            return;
        } else if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
            m_title_text->SetLabelText(_L("Please select the filament."));
            return;
        }
    } else if (nozzle_nums == 1) {
        m_split_line_panel->Hide();
    }

    m_title_text->SetLabelText(_L("Nozzle"));
}


void AmsMapingPopup::update_ams_tips(MachineObject* obj)
{
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    size_t nozzle_nums = full_config.option<ConfigOptionFloatsNullable>("nozzle_diameter")->values.size();
    if (m_ams_tips_msg_panel) {
        m_ams_tips_msg_panel->Clear();
        if (nozzle_nums == 2 && m_show_type != ShowType::LEFT_AND_RIGHT) {
            m_ams_tips_msg_panel->AddMessage(_L("To learn about the filaments matching rules."),
                                             "#FF6F00", ""); // Orca: dropped vendor help link
        }

        if (obj && obj->GetFilaSwitch()->IsInstalled()) {
            const auto& msg = _L("External spools is not supported since Filament Track Switch has been installed. If you want to use external spool, please uninstall it.");
            m_ams_tips_msg_panel->AddMessage(msg, "#FF6F00", "");
        }

        m_ams_tips_msg_panel->Layout();
        m_ams_tips_msg_panel->Fit();
        m_ams_tips_msg_panel->Show(m_ams_tips_msg_panel->GetMessageCount() > 0);
    }
}

void AmsMapingPopup::update_rack_select(MachineObject* obj, bool use_dynamic_switch, std::optional<PrintFromType> print_from_type)
{
    // Orca: MachineObject has no GetNozzleRack() convenience; route through the nozzle system instead.
    m_rack = obj ? obj->GetNozzleSystem()->GetNozzleRack() : nullptr;

    bool show_rack_select_area = false;
    if (!m_mapping_from_multi_machines && !m_use_in_sync_dialog &&
        obj && obj->GetNozzleSystem()->GetNozzleRack()->IsSupported() &&
        print_from_type.has_value() && print_from_type.value() == PrintFromType::FROM_NORMAL) {
        const auto& nozzle_pos_vec = obj->get_nozzle_mapping_result()->GetMappedNozzlePosVecByFilaId(m_current_filament_id);
        m_rack_nozzle_select->UpdatSelectedNozzles(obj->GetNozzleSystem()->GetNozzleRack(), nozzle_pos_vec, use_dynamic_switch, print_from_type);
        show_rack_select_area = true;
    }

    if (show_rack_select_area != m_rack_nozzle_select->IsShown()) {
        m_right_tip_text = show_rack_select_area ? _L("Select Filament && Hotends") : _L("Select Filament");
        m_right_tips->SetLabel(m_right_tip_text);
        m_rack_nozzle_select->Show(show_rack_select_area);
        Layout();
        Fit();
    }
}

void AmsMapingPopup::update_items_check_state(const std::vector<FilamentInfo>& ams_mapping_result)
{
    /*update check states*/
    if (m_parent_item) {
        auto update_item_check_state = [&ams_mapping_result, this](MappingItem* item) {
            if (item) {
                for (const auto& mapping_res : ams_mapping_result) {
                    if (mapping_res.id == this->m_current_filament_id) {
                        if (mapping_res.ams_id == std::to_string(item->m_ams_id) &&
                            mapping_res.slot_id == std::to_string(item->m_slot_id)) {
                            item->set_checked(true);
                        } else {
                            item->set_checked(false);
                        }

                        return;
                    }
                }

                item->set_checked(false);
            }
        };

        update_item_check_state(m_left_extra_slot);
        update_item_check_state(m_right_extra_slot);
        for (auto mapping_item : m_mapping_item_list) {
            update_item_check_state(mapping_item);
        }
    }
}

void AmsMapingPopup::add_ams_mapping(std::vector<TrayData> tray_data,
                                     bool remain_detect_flag,
                                     wxWindow* container,
                                     wxBoxSizer* sizer)
{
    sizer->Add(0, 0, 0, wxLEFT, FromDIP(6));

    for (auto i = 0; i < tray_data.size(); i++) {

        // set button
        MappingItem* m_mapping_item = new MappingItem(container);
        m_mapping_item->send_win = send_win;
        m_mapping_item->m_ams_id = tray_data[i].ams_id;
        m_mapping_item->m_slot_id = tray_data[i].slot_id;
        // Orca: transition_tridid here has no total-extruder-count overload; label the virtual (ext)
        // slots Ext-R / Ext-L inline in the two-nozzle left+right views (matches that overload's result).
        if ((m_show_type == ShowType::LEFT_AND_RIGHT || m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC)
            && (tray_data[i].id == VIRTUAL_TRAY_MAIN_ID || tray_data[i].id == VIRTUAL_TRAY_DEPUTY_ID)) {
            m_mapping_item->set_tray_index(tray_data[i].id == VIRTUAL_TRAY_MAIN_ID ? wxString("Ext-R") : wxString("Ext-L"));
        } else {
            m_mapping_item->set_tray_index(wxGetApp().transition_tridid(tray_data[i].id));
        }

        m_mapping_item->SetSize(wxSize(FromDIP(48), FromDIP(60)));
        m_mapping_item->SetMinSize(wxSize(FromDIP(48), FromDIP(60)));
        m_mapping_item->SetMaxSize(wxSize(FromDIP(48), FromDIP(60)));

        // traversal if can pick the item or not
        bool can_pick_the_item = true;
        std::optional<wxString> item_tooltip_msg;
        // check filament area
        if (m_use_in_sync_dialog) {
            if (can_pick_the_item) {
                if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
                    can_pick_the_item = !devPrinterUtil::IsVirtualSlot(m_mapping_item->m_ams_id);
                    if (!can_pick_the_item) {
                        item_tooltip_msg = _L(
                            "External spools is not supported since Filament Track Switch has been installed. If you want to use external spool, please uninstall it.");
                    }
                }
            }
        }
        else {
            if (can_pick_the_item) {
                // LEFT_AND_RIGHT — the fallback when a filament has no entry in the
                // nozzle map — keeps both panels pickable.
                auto parent = container->GetParent();
                if (parent == m_left_marea_panel) {
                    can_pick_the_item = (m_show_type == ShowType::LEFT || m_show_type == ShowType::LEFT_AND_RIGHT);
                } else if (parent == m_right_marea_panel) {
                    if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
                        can_pick_the_item = !devPrinterUtil::IsVirtualSlot(m_mapping_item->m_ams_id);
                        if (!can_pick_the_item) {
                            item_tooltip_msg = _L(
                                "External spools is not supported since Filament Track Switch has been installed. If you want to use external spool, please uninstall it.");
                        }
                    } else if (m_show_type != ShowType::RIGHT && m_show_type != ShowType::LEFT_AND_RIGHT) {
                        can_pick_the_item = false;
                    }
                }
            }
        }

        // check filament type
        if (can_pick_the_item) {
            if (tray_data[i].type == NORMAL && !is_match_material(tray_data[i].filament_type)){
                can_pick_the_item = false;
            } else if(tray_data[i].type == EMPTY){
                // Mapping to an empty slot is only allowed from the multi-machine send page,
                // which maps onto EMPTY placeholder trays.
                can_pick_the_item = m_mapping_from_multi_machines;
            }
        }

        wxColour display_color;
        if (can_pick_the_item) {
            display_color = tray_data[i].type == NORMAL ? tray_data[i].colour : wxColour(0xCE, 0xCE, 0xCE);
        } else {
            display_color = wxColour(0xEE, 0xEE, 0xEE);
        }

        wxString display_name;
        if (tray_data[i].type == THIRD) {
            display_name = "?";
        } else if (tray_data[i].type == EMPTY) {
            display_name = "-";
        } else {
            display_name = tray_data[i].name;
        }

        m_mapping_item->set_data(m_tag_material, display_color, display_name, remain_detect_flag, tray_data[i], !can_pick_the_item, item_tooltip_msg);
        m_mapping_item->Bind(wxEVT_LEFT_DOWN, [this, can_pick_the_item, m_mapping_item](wxMouseEvent& e) {
            if (can_pick_the_item) {
                m_mapping_item->send_event(m_current_filament_id);
                Dismiss();
            }
        });

        sizer->Add(0, 0, 0, wxRIGHT, FromDIP(6));
        sizer->Add(m_mapping_item, 0, wxTOP, FromDIP(1));
        m_mapping_item_list.push_back(m_mapping_item);
    }
}

void AmsMapingPopup::add_ext_ams_mapping(TrayData tray_data, MappingItem* item)
{
#ifdef __APPLE__
    m_mapping_item_list.push_back(item);
#endif
    // set button
    if (tray_data.type == NORMAL) {
        if (is_match_material(tray_data.filament_type)) {
            item->set_data(m_tag_material, tray_data.colour, tray_data.name, false, tray_data);
        } else {
            item->set_data(m_tag_material, m_ext_mapping_filatype_check ? wxColour(0xEE, 0xEE, 0xEE) : tray_data.colour, tray_data.name, false, tray_data, true);
            m_has_unmatch_filament = true;
        }

        item->Bind(wxEVT_LEFT_DOWN, [this, tray_data, item](wxMouseEvent& e) {
            if (!item->GetParent() || !item->GetParent()->IsEnabled()) return;
            if (m_ext_mapping_filatype_check && !is_match_material(tray_data.filament_type)) return;
            item->send_event(m_current_filament_id);
            Dismiss();
        });
    }


    // temp
    if (tray_data.type == EMPTY) {
        item->set_data(m_tag_material, wxColour(0xCE, 0xCE, 0xCE), "-", false, tray_data);
        item->Bind(wxEVT_LEFT_DOWN, [this, tray_data, item](wxMouseEvent& e) {
            if (!item->GetParent() || !item->GetParent()->IsEnabled()) return;
            item->send_event(m_current_filament_id);
            Dismiss();
        });
    }

    // third party
    if (tray_data.type == THIRD) {
        item->set_data(m_tag_material, tray_data.colour, "?", false, tray_data);
        //item->set_data(wxColour(0xCE, 0xCE, 0xCE), "?", tray_data);
        item->Bind(wxEVT_LEFT_DOWN, [this, tray_data, item](wxMouseEvent& e) {
            if (!item->GetParent() || !item->GetParent()->IsEnabled()) return;
            item->send_event(m_current_filament_id);
            Dismiss();
        });
    }

    item->set_tray_index("Ext");
}

} // namespace Slic3r::GUI