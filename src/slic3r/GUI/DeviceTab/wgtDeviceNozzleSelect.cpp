//**********************************************************/
/* File: wgtDeviceNozzleSelect.cpp
*  Description: The panel to select nozzle
*
*  \n class wgtDeviceNozzleSelect;
//**********************************************************/

#include "wgtDeviceNozzleSelect.h"
#include "wgtDeviceNozzleRack.h"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/DeviceTab/wgtMsgBox.h"

static wxColour s_gray_clr("#B0B0B0");
static wxColour s_hgreen_clr("#00AE42");
static wxColour s_red_clr("#D01B1B");

static std::vector<int> a_nozzle_seq = {16, 18, 20, 17, 19, 21};

wxDEFINE_EVENT(EVT_NOZZLE_SELECT_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_NOZZLE_SELECT_CLICKED, wxCommandEvent);

namespace Slic3r::GUI {

wgtDeviceNozzleRackSelect::wgtDeviceNozzleRackSelect(wxWindow *parent) : wxPanel(parent, wxID_ANY) { CreateGui(); }

static wxPanel* s_create_title(wxWindow *parent, const wxString& text)
{
    wxPanel *panel = new wxPanel(parent, wxID_ANY);

    auto title  = new Label(panel, text);
    title->SetFont(::Label::Body_13);
    title->SetBackgroundColour(*wxWHITE);
    title->SetForegroundColour(0x909090);

    auto split_line = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    split_line->SetBackgroundColour(0xeeeeee);
    split_line->SetMinSize(wxSize(-1, 1));
    split_line->SetMaxSize(wxSize(-1, 1));

    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(0, 0, 0, wxEXPAND, 0);
    sizer->Add(title, 0, wxALIGN_CENTER, 0);
    sizer->Add(split_line, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND, 0);
    panel->SetSizer(sizer);
    panel->Layout();
    return panel;
}

void wgtDeviceNozzleRackSelect::CreateGui()
{
    wxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    wxColour tip_bg_clr("#FFF0E0");
    m_title_tips_dynamic = new wgtMsgBox(this);
    m_title_tips_dynamic->SetBackgroundColour(tip_bg_clr);
    m_title_tips_dynamic->SetBorderColor(wxColour("#FF6F00"));
    m_title_tips_dynamic->SetCornerRadius(1);
    m_title_tips_dynamic->SetBorderWidth(1);
    auto text_label = m_title_tips_dynamic->GetTextLabel();
    text_label->SetFont(::Label::Body_12);
    text_label->SetForegroundColour("#FF6F00");
    text_label->SetBackgroundColour(tip_bg_clr);
    text_label->SetLabel(_L("Dynamic nozzles are allocated on the current plate. Picking hotend is not supported."));
    text_label->Wrap(FromDIP(300));
    text_label->Fit();
    m_title_tips_dynamic->Layout();
    m_title_tips_dynamic->Refresh();

    // nozzles
    wxGridSizer *nozzle_sizer = new wxGridSizer(2, 3, FromDIP(10), FromDIP(10));
    for (auto idx : a_nozzle_seq) {
        wgtDeviceNozzleRackNozzleItem *nozzle_item = new wgtDeviceNozzleRackNozzleItem(this, idx - 16);
        nozzle_item->EnableSelect();
        nozzle_item->Bind(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, &wgtDeviceNozzleRackSelect::OnNozzleItemSelected, this);
        m_nozzle_items[idx]                        = nozzle_item;
        nozzle_sizer->Add(nozzle_item, 0);
    }

    // toolhead area
    m_toolhead_nozzle_l = new wgtDeviceNozzleRackNozzleItem(this, 1);
    m_toolhead_nozzle_l->EnableSelect();
    m_toolhead_nozzle_l->SetDisplayIdText("L");
    m_toolhead_nozzle_l->Bind(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, &wgtDeviceNozzleRackSelect::OnNozzleItemSelected, this);

    m_toolhead_nozzle_r = new wgtDeviceNozzleRackNozzleItem(this, 0);
    m_toolhead_nozzle_r->EnableSelect();
    m_toolhead_nozzle_r->SetDisplayIdText("R");
    m_toolhead_nozzle_r->Bind(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, &wgtDeviceNozzleRackSelect::OnNozzleItemSelected, this);

    wxSizer* toolhead_sizer = new wxBoxSizer(wxHORIZONTAL);
    toolhead_sizer->Add(m_toolhead_nozzle_l, 0, wxRIGHT, FromDIP(5));
    toolhead_sizer->Add(m_toolhead_nozzle_r, 0, wxLEFT, FromDIP(5));

    main_sizer->Add(m_title_tips_dynamic, 0, wxALIGN_CENTRE_VERTICAL | wxBOTTOM, FromDIP(10));
    main_sizer->Add(s_create_title(this, _L("Hotend Rack")), 0, wxEXPAND);
    main_sizer->Add(nozzle_sizer, 0, wxTOP | wxBOTTOM | wxALIGN_LEFT, FromDIP(10));
    main_sizer->Add(s_create_title(this, _L("ToolHead")), 0, wxEXPAND);
    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(toolhead_sizer, 0, wxALIGN_LEFT);

    SetBackgroundColour(*wxWHITE);
    SetSizer(main_sizer);
    Layout();
    Fit();
}

static void s_update_nozzle_info(wgtDeviceNozzleRackNozzleItem* item,
                                 std::shared_ptr<DevNozzleRack> rack,
                                 const DevNozzle& nozzle_info)
{
    item->Update(rack, nozzle_info.IsOnRack());
    if (nozzle_info.IsUnknown()) {
        if (item->GetToolTipText() != _L("Nozzle information needs to be read")) {
            item->SetToolTip(_L("Nozzle information needs to be read"));
        }
    } else {
        item->SetToolTip(wxEmptyString);
    }
}

void wgtDeviceNozzleRackSelect::UpdateNozzleInfos(std::shared_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    if (rack) {
        s_update_nozzle_info(m_toolhead_nozzle_l, rack, rack->GetNozzleSystem()->GetNozzleByPosId(DEPUTY_EXTRUDER_ID));
        s_update_nozzle_info(m_toolhead_nozzle_r, rack, rack->GetNozzleSystem()->GetNozzleByPosId(MAIN_EXTRUDER_ID));
        for (const auto& item : m_nozzle_items) {
            s_update_nozzle_info(item.second, rack, rack->GetNozzleSystem()->GetNozzleByPosId(item.first));
        }
    }
}

static void s_enable_item_if_match(wgtDeviceNozzleRackNozzleItem* item, 
                                   const DevNozzle& nozzle_info,
                                   const DevNozzle& selected_nozzle)
{
    if (item) {
        if (nozzle_info.GetLogicExtruderId() != selected_nozzle.GetLogicExtruderId()) {
            item->SetDisable(true);
            return;
        }

        if (!nozzle_info.IsEmpty() && !nozzle_info.IsAbnormal() && !nozzle_info.IsUnknown() &&
            nozzle_info.GetNozzleType() == selected_nozzle.GetNozzleType() &&
            nozzle_info.GetNozzleDiameter() == selected_nozzle.GetNozzleDiameter() &&
            nozzle_info.GetNozzleFlowType() == selected_nozzle.GetNozzleFlowType()) {
            item->SetDisable(false);
        } else {
            item->SetDisable(true);
        }
    }
}

void wgtDeviceNozzleRackSelect::UpdatSelectedNozzle(std::shared_ptr<DevNozzleRack> rack, int selected_nozzle_pos_id)
{
    m_nozzle_rack = rack;
    if (rack) {
        SetSelectedNozzle(rack->GetNozzleSystem()->GetNozzleByPosId(selected_nozzle_pos_id));
        if (m_enable_manual_nozzle_pick) {
            s_enable_item_if_match(m_toolhead_nozzle_r, rack->GetNozzleSystem()->GetNozzleByPosId(MAIN_EXTRUDER_ID), m_selected_nozzle);
            s_enable_item_if_match(m_toolhead_nozzle_l, rack->GetNozzleSystem()->GetNozzleByPosId(DEPUTY_EXTRUDER_ID), m_selected_nozzle);
            for (auto& item : m_nozzle_items) {
                s_enable_item_if_match(item.second, rack->GetNozzleSystem()->GetNozzleByPosId(item.first), m_selected_nozzle);
            }
        }
    }
}

void wgtDeviceNozzleRackSelect::UpdatSelectedNozzles(std::shared_ptr<DevNozzleRack> rack,
                                                     std::vector<int> selected_nozzle_pos_vec,
                                                     bool use_dynamic_switch,
                                                     std::optional<PrintFromType> /*print_from_type*/)
{
    m_nozzle_rack = rack;
    m_enable_manual_nozzle_pick = !use_dynamic_switch;
    m_title_tips_dynamic->Show(use_dynamic_switch);

    UpdateNozzleInfos(rack);
    if (!use_dynamic_switch) {
        if (selected_nozzle_pos_vec.size() > 0) {
            return UpdatSelectedNozzle(rack, selected_nozzle_pos_vec.at(0));
        } else {
            return ClearSelection();
        }
    }

    if (rack) {
        ClearSelection();
        for (const auto& pos_id : selected_nozzle_pos_vec) {
            if (pos_id == MAIN_EXTRUDER_ID) {
                m_toolhead_nozzle_r->SetSelected(true);
            } else if (pos_id == DEPUTY_EXTRUDER_ID) {
                m_toolhead_nozzle_l->SetSelected(true);
            } else if (auto it = m_nozzle_items.find(pos_id); it != m_nozzle_items.end()) {
                it->second->SetSelected(true);
            }
        }

        if (!m_toolhead_nozzle_l->IsSelected()) {
            m_toolhead_nozzle_l->SetDisable(true);
        }

        if (!m_toolhead_nozzle_r->IsSelected()) {
            m_toolhead_nozzle_r->SetDisable(true);
        }

        for (const auto& item : m_nozzle_items) {
            if (!item.second->IsSelected()) {
                item.second->SetDisable(true);
            }
        }
    }
}

void wgtDeviceNozzleRackSelect::ClearSelection() 
{
    m_selected_nozzle = DevNozzle();
    m_toolhead_nozzle_l->SetSelected(false);
    m_toolhead_nozzle_r->SetSelected(false);
    for (auto &item : m_nozzle_items) { item.second->SetSelected(false); }
}

void wgtDeviceNozzleRackSelect::SetSelectedNozzle(const DevNozzle &nozzle)
{
    int new_selected_pos_id = nozzle.GetNozzlePosId();
    if (m_selected_nozzle.GetNozzlePosId() != new_selected_pos_id) {
        ClearSelection();

        m_selected_nozzle = nozzle;
        if (new_selected_pos_id == MAIN_EXTRUDER_ID) {
            m_toolhead_nozzle_r->SetSelected(true);
        } else if (new_selected_pos_id == DEPUTY_EXTRUDER_ID) {
            m_toolhead_nozzle_l->SetSelected(true);
        } else if (auto it = m_nozzle_items.find(m_selected_nozzle.GetNozzlePosId()); it != m_nozzle_items.end()) {
            it->second->SetSelected(true);
        }
    }
}

volatile int sGetNozzlePosId(wgtDeviceNozzleRackNozzleItem* item,
                             wgtDeviceNozzleRackNozzleItem* l_item,
                             wgtDeviceNozzleRackNozzleItem* r_item)
{
    int to_select_pos_id = -1;
    if (item == l_item) {
        to_select_pos_id = DEPUTY_EXTRUDER_ID;
    } else if (item == r_item) {
        to_select_pos_id = MAIN_EXTRUDER_ID;
    } else {
        to_select_pos_id = item->GetNozzleId() + 0x10;
    }

    return to_select_pos_id;
}

void wgtDeviceNozzleRackSelect::OnNozzleItemSelected(wxCommandEvent &evt)
{
    if (!m_enable_manual_nozzle_pick) {
        return;
    }

    auto *item = dynamic_cast<wgtDeviceNozzleRackNozzleItem *>(evt.GetEventObject());
    if (item; auto ptr = m_nozzle_rack.lock()) {
        int to_select_pos_id = sGetNozzlePosId(item, m_toolhead_nozzle_l, m_toolhead_nozzle_r);
        if (to_select_pos_id > -1 && to_select_pos_id != GetSelectedNozzlePosID()) {
            SetSelectedNozzle(ptr->GetNozzleSystem()->GetNozzleByPosId(to_select_pos_id));
            wxCommandEvent change_evt(EVT_NOZZLE_SELECT_CHANGED, GetId());
            change_evt.SetEventObject(this);
            ProcessEvent(change_evt);
            evt.Skip();
        } else {
            wxCommandEvent change_evt(EVT_NOZZLE_SELECT_CLICKED, GetId());
            change_evt.SetEventObject(this);
            ProcessEvent(change_evt);
            evt.Skip();
        }
    }
}

void wgtDeviceNozzleRackSelect::Rescale()
{
    m_toolhead_nozzle_l->Rescale();
    m_toolhead_nozzle_r->Rescale();
    for (auto &item : m_nozzle_items) { item.second->Rescale(); }
}

}; // namespace Slic3r::GUI
