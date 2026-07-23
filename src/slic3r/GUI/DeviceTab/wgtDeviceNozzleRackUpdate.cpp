//**********************************************************/
/* File: wgtDeviceNozzleRackUpdate.cpp
*  Description: The panel with rack updating
*
* \n class wgtDeviceNozzleRackUpdate
//**********************************************************/

#include "wgtDeviceNozzleRackUpdate.h"

#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevUpgrade.h"

#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#define WX_DIP_SIZE(x, y) wxSize(FromDIP(x), FromDIP(y))

static wxColour s_red_clr("#D01B1B");

namespace Slic3r::GUI
{

wxDEFINE_EVENT(wxEVT_NOZZLE_JUMP_UPGRADE, wxCommandEvent);

wgtDeviceNozzleRackUpgradeDlg::wgtDeviceNozzleRackUpgradeDlg(wxWindow* parent, const std::shared_ptr<DevNozzleRack> rack)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    m_rack_upgrade_panel = new wgtDeviceNozzleRackUprade(this);
    m_rack_upgrade_panel->UpdateRackInfo(rack);

    Bind(wxEVT_NOZZLE_JUMP_UPGRADE, [this](wxCommandEvent&) {
        EndModal(wxID_OK);
    });

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_rack_upgrade_panel, 0, wxEXPAND);
    SetSizer(main_sizer);
    Layout();
    Fit();

    wxGetApp().UpdateDlgDarkUI(this);
}

void wgtDeviceNozzleRackUpgradeDlg::UpdateRackInfo(const std::shared_ptr<DevNozzleRack> rack)
{
    m_rack_upgrade_panel->UpdateRackInfo(rack);
}

void wgtDeviceNozzleRackUpgradeDlg::on_dpi_changed(const wxRect& suggested_rect)
{
    m_rack_upgrade_panel->Rescale();
}

wgtDeviceNozzleRackUprade::wgtDeviceNozzleRackUprade(wxWindow* parent,
                                                     wxWindowID id,
                                                     const wxPoint& pos,
                                                     const wxSize& size,
                                                     long style)
    : wxPanel(parent, id, pos, size, style)
{
    CreateGui();
}

void wgtDeviceNozzleRackUprade::CreateGui()
{
    SetBackgroundColour(*wxWHITE);

    // Main vertical sizer
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Header: title + buttons
    auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Title label
    auto* title_label = new Label(this, _L("Hotends Info"));
    title_label->SetFont(Label::Head_14);
    header_sizer->Add(title_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));

    // Spacer
    header_sizer->AddStretchSpacer();
    main_sizer->Add(header_sizer, 0, wxEXPAND | wxTOP | wxRIGHT, FromDIP(10));

    // "Nozzles"
    m_extruder_nozzle_item = new wgtDeviceNozzleRackHotendUpdate(this, "R");
    m_extruder_nozzle_item->UpdateColourStyle(wxColour("#F8F8F8"));
    m_extruder_nozzle_item->SetExtruderNozzleId(MAIN_EXTRUDER_ID);

    main_sizer->Add(m_extruder_nozzle_item, 0, wxEXPAND | wxALL, FromDIP(12));
    for (int id = 0; id < 6; id ++)
    {
        auto item = new wgtDeviceNozzleRackHotendUpdate(this, wxString::Format("%d", id + 1));
        item->SetRackNozzleId(id);
        m_nozzle_items[id] = item;

        main_sizer->Add(item, 0, wxEXPAND | wxALL, FromDIP(12));
        if (id < 5)
        {
            wxPanel* separator = new wxPanel(this);
            separator->SetMaxSize(wxSize(-1, FromDIP(1)));
            separator->SetMinSize(wxSize(-1, FromDIP(1)));
            separator->SetBackgroundColour(wxColour(238, 238, 238)); // Orca: grey300 as a literal, no equivalent colour macro in Orca
            main_sizer->Add(separator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        }
    }

    main_sizer->AddSpacer(FromDIP(20));

    // Set sizer
    this->SetSizer(main_sizer);
    this->Layout();
}

void wgtDeviceNozzleRackUprade::UpdateRackInfo(const std::shared_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    if (!rack) { return;}

    // update the nozzles
    m_extruder_nozzle_item->UpdateExtruderNozzleInfo(rack);
    for (auto iter : m_nozzle_items)
    {
        iter.second->UpdateRackNozzleInfo(rack);
    }

    // update layout
    Layout();
}

void wgtDeviceNozzleRackUprade::OnBtnReadAll(wxCommandEvent& e)
{
    if (auto rack = m_nozzle_rack.lock())
    {
        rack->CtrlRackReadAll(true);
    }
}

void wgtDeviceNozzleRackUprade::Rescale()
{
    m_extruder_nozzle_item->Rescale();
    for (auto& iter : m_nozzle_items)
    {
        iter.second->Rescale();
    }
}

#define WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG *wxWHITE
wgtDeviceNozzleRackHotendUpdate::wgtDeviceNozzleRackHotendUpdate(wxWindow* parent, const wxString& idx_text)
    : StaticBox(parent, wxID_ANY)
{
    CreateGui();

    m_idx_label->SetLabel(idx_text);
}

void wgtDeviceNozzleRackHotendUpdate::CreateGui()
{
    SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    SetBorderColor(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    SetCornerRadius(0);

    //load nozzle hs image
    for (int i = 1; i <= 4; i++)
    {
        auto normalImage = new ScalableBitmap(this, "Nozzle_HS_01_0" + std::to_string(i * 2), 46);
        auto bigImage = new ScalableBitmap(this, "Big_Nozzle_HS_01_0" + std::to_string(i * 2), 216);
        nozzle_hs.push_back({normalImage, bigImage});
    }
    for (int i = 2; i <= 4; i++)
    {
        auto normalImage = new ScalableBitmap(this, "Nozzle_HH_01_0" + std::to_string(i * 2), 46);
        auto bigImage = new ScalableBitmap(this, "Big_Nozzle_HH_01_0" + std::to_string(i * 2), 216);
        nozzle_hh.push_back({normalImage, bigImage});
    }

    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Index
    m_idx_label = new Label(this);
    m_idx_label->SetFont(Label::Head_14);
    m_idx_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    content_sizer->Add(m_idx_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(25));

    // Icon
    wxPanel* imagePanel = new wxPanel(this);
    imagePanel->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    imagePanel->SetMaxSize(WX_DIP_SIZE(46, -1));
    imagePanel->SetMinSize(WX_DIP_SIZE(46, -1));
    imagePanel->SetSize(wxSize(FromDIP(46), FromDIP(-1)));
    wxBoxSizer* panelSizer = new wxBoxSizer(wxVERTICAL);
    imagePanel->SetSizer(panelSizer);

    m_nozzle_empty_image = new ScalableBitmap(imagePanel, "dev_rack_nozzle_empty", 46);
    m_icon_bitmap = new wxStaticBitmap(imagePanel, wxID_ANY, m_nozzle_empty_image->bmp());
    panelSizer->Add(m_icon_bitmap, 0, wxALIGN_CENTER);
    content_sizer->Add(imagePanel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(25));
    m_icon_bitmap->Bind(wxEVT_ENTER_WINDOW, &wgtDeviceNozzleRackHotendUpdate::OnBitmapHoverEnter, this);
    m_icon_bitmap->Bind(wxEVT_LEAVE_WINDOW, &wgtDeviceNozzleRackHotendUpdate::OnBitmapHoverLeave, this);

    // Diameter/type (vertical)
    wxPanel* type_panel = new wxPanel(this);
    type_panel->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    auto* main_type_sizer = new wxBoxSizer(wxVERTICAL);
    auto* type_sizer_row_1 = new wxBoxSizer(wxHORIZONTAL);
    auto* type_sizer_row_2 = new wxBoxSizer(wxHORIZONTAL);

    m_material_label = new Label(type_panel);
    m_material_label->SetFont(Label::Body_12);
    m_material_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_colour_box = new StaticBox(type_panel);
    m_colour_box->SetMaxSize(WX_DIP_SIZE(16, 16));
    m_colour_box->SetMinSize(WX_DIP_SIZE(16, 16));
    m_colour_box->SetCornerRadius(FromDIP(2));
    m_colour_box->SetSize(wxSize(FromDIP(16), FromDIP(16)));
    // m_colour_box->SetBackgroundColour(*wxRED);

    // type_sizer_row_1->AddStretchSpacer();
    type_sizer_row_1->Add(m_colour_box, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);
    type_sizer_row_1->AddStretchSpacer(1);
    type_sizer_row_1->Add(m_material_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
    // type_sizer_row_1->AddStretchSpacer();

    m_diameter_label = new Label(type_panel);
    m_diameter_label->SetFont(Label::Body_12);
    m_diameter_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_flowtype_label = new Label(type_panel);
    m_flowtype_label->SetFont(Label::Body_12);
    m_flowtype_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_type_label = new Label(type_panel);
    m_type_label->SetFont(Label::Body_12);
    m_type_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    type_sizer_row_2->Add(m_diameter_label, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT );
    type_sizer_row_2->Add(m_flowtype_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
    type_sizer_row_2->Add(m_type_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));

    main_type_sizer->Add(type_sizer_row_1, 0, wxALIGN_LEFT);
    main_type_sizer->Add(type_sizer_row_2, 1, wxALIGN_LEFT | wxEXPAND | wxTOP, FromDIP(4));
    type_panel->SetSizer(main_type_sizer);
    type_panel->SetMaxSize(WX_DIP_SIZE(160, 40));
    type_panel->SetMinSize(WX_DIP_SIZE(160, 40));
    type_panel->SetSize(WX_DIP_SIZE(160, 40));

    content_sizer->Add(type_panel, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));

    // SN and version (vertical)
    wxPanel* info_panel = new wxPanel(this);
    info_panel->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    auto* info_sizer = new wxBoxSizer(wxVERTICAL);
    m_sn_label = new Label(info_panel);
    m_sn_label->SetFont(Label::Body_12);
    m_sn_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    auto* version_h_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_version_label = new Label(info_panel);
    m_version_label->SetFont(Label::Body_12);
    m_version_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_version_new_label = new Label(info_panel);
    m_version_new_label->SetFont(Label::Body_12);
    m_version_new_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    m_version_new_label->SetForegroundColour(wxColour(0, 168, 84)); // Green

    version_h_sizer->Add(m_version_label, 0, wxALIGN_CENTER_VERTICAL);
    version_h_sizer->Add(m_version_new_label, 0, wxALIGN_CENTER_VERTICAL);
    info_sizer->Add(m_sn_label, 0, wxALIGN_LEFT);
    info_sizer->Add(version_h_sizer, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));
    info_panel->SetSizer(info_sizer);
    info_panel->SetMaxSize(WX_DIP_SIZE(183, 40));
    info_panel->SetMinSize(WX_DIP_SIZE(183, 40));
    info_panel->SetSize(WX_DIP_SIZE(183, 40));

    //Used Time
    m_used_time = new Label(this);
    m_used_time->SetFont(Label::Body_12);
    m_used_time->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_refresh_icon = new ScalableBitmap(this, "refresh_printer", 12);
    // m_in_refreh_icon = new ScalableBitmap(this, "refresh_nozzle", 12);
    m_error_icon = new ScalableBitmap(this, "error", 14);
    m_status_bitmap = new wxStaticBitmap(this, wxID_ANY, m_refresh_icon->bmp());
    m_status_bitmap->Bind(wxEVT_LEFT_UP, &wgtDeviceNozzleRackHotendUpdate::OnStatusIconClick, this);

    std::vector<std::string> list{"refresh_nozzle_1", "refresh_nozzle_2", "refresh_nozzle_3", "refresh_nozzle_4"};
    // Orca: AnimaIcon has no per-instance size argument (it fixes the size internally), so none is passed.
    m_refreshing_icon = new AnimaIcon(this, wxID_ANY, list, "refresh_nozzle", 100);
    m_refreshing_icon->Show(false);


    m_status_label = new Label(this);
    m_status_label->SetFont(Label::Body_12);
    // m_status_label->SetForegroundColour(wxColour("#00AE42"));

    content_sizer->Add(info_panel, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    content_sizer->Add(m_used_time, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    content_sizer->Add(m_status_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    content_sizer->Add(m_status_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
    content_sizer->Add(m_refreshing_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
    content_sizer->AddSpacer(FromDIP(25));

    auto* main_sizer = new wxBoxSizer(wxHORIZONTAL);
    main_sizer->Add(content_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));

    SetSizer(main_sizer);
    Layout();
}

void wgtDeviceNozzleRackHotendUpdate::OnStatusIconClick(wxMouseEvent& event)
{
    if (m_status_label->GetLabel() == _L("Refresh"))
    {
        m_status_label->SetForegroundColour(wxColour("#A3A3A3"));
        m_status_label->SetLabel(_L("Refreshing"));
        m_status_bitmap->Show(false);
        // m_status_bitmap->Refresh();
        if(!m_refreshing_icon->IsPlaying()) 
        {
            m_refreshing_icon->Play();
            m_refreshing_icon->Show();
        }
        if (auto shared = m_nozzle_rack.lock())
        {
            shared->CrtlRackReadNozzle(m_rack_nozzle_id);
        }
        Layout();
    }

    if (m_status_label->GetLabel() == _L("Error") /*&& m_nozzle_status == NOZZLE_STATUS_ABNORMAL*/)
    {
        if (auto shared = m_nozzle_rack.lock())
        {
            MessageDialog dlg(nullptr, _L("Hotend status abnormal, unavailable at present. Please upgrade the firmware"
            " and try again."), _L("Update"), wxICON_WARNING);
            dlg.AddButton(wxID_CANCEL, _L("Cancel"), false);
            dlg.AddButton(wxID_OK,_L("Jump to the upgrade page"), true);

            if (dlg.ShowModal() == wxID_OK) 
            {
                wxGetApp().mainframe->m_monitor->jump_to_Upgrade();

                wxCommandEvent evt(wxEVT_NOZZLE_JUMP_UPGRADE, GetId());
                evt.SetEventObject(this);
                wxWindow* target = GetParent();
                if (target) 
                {
                    target = target->GetParent();
                    if (target) 
                    {
                        wxPostEvent(target, evt);
                    }
                }
            };
        }
    }
}

void wgtDeviceNozzleRackHotendUpdate::OnBitmapHoverEnter(wxMouseEvent& event)
{
    int scaledW = FromDIP(240);
    int scaledH = FromDIP(240);

    ScalableBitmap* scaledBmp{nullptr};
    if (m_nozzle_status == NOZZLE_STATUS_EMPTY || m_nozzle_status == NOZZLE_STATUS_UNKNOWN || !findNozzleImage)
    {
        if (!m_scaled_nozzle_empty_image) {m_scaled_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 216);}

        scaledBmp = m_scaled_nozzle_empty_image;
    }
    else
    {
        scaledBmp = m_scaled_nozzle_image;
    }

    m_hoverFrame = new wxFrame(nullptr, wxID_ANY, "", 
                                wxDefaultPosition, wxDefaultSize, 
                                wxFRAME_NO_TASKBAR | wxBORDER_NONE | wxTRANSPARENT_WINDOW);
    m_hoverFrame->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    m_hoverFrame->SetSize(scaledW, scaledH);
    wxBoxSizer* frameSizer = new wxBoxSizer(wxVERTICAL);
    m_hoverFrame->SetSizer(frameSizer);

    wxStaticBitmap* hoverBmp = new wxStaticBitmap(m_hoverFrame, wxID_ANY, scaledBmp->bmp());
    frameSizer->Add(hoverBmp, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(12));

    wxPoint mousePos = wxGetMousePosition();
    m_hoverFrame->SetPosition(wxPoint(mousePos.x + 10, mousePos.y));
    m_hoverFrame->Layout();
    m_hoverFrame->Show(true);

    event.Skip();
}

void wgtDeviceNozzleRackHotendUpdate::OnBitmapHoverLeave(wxMouseEvent& event)
{
    if (m_hoverFrame)
    {
        m_hoverFrame->Destroy();
        m_hoverFrame = nullptr;
    }
    event.Skip();
}

void wgtDeviceNozzleRackHotendUpdate::updateNozzleImage(const DevNozzle& nozzle)
{
    if (m_nozzle_status == NOZZLE_STATUS_UNKNOWN || m_nozzle_status == NOZZLE_STATUS_EMPTY)
    {
        if (!m_nozzle_empty_image) {m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);}
        m_icon_bitmap->SetBitmap(m_nozzle_empty_image->bmp());
        m_icon_bitmap->Refresh();
        return;
    }
    findNozzleImage = false;
    int index = -1;
    switch (nozzle.GetNozzleDiameterType()) {
        case NOZZLE_DIAMETER_0_2: index = 0; break;
        case NOZZLE_DIAMETER_0_4: index = 1; break;
        case NOZZLE_DIAMETER_0_6: index = 2; break;
        case NOZZLE_DIAMETER_0_8: index = 3; break;
        default:                  index = -1; break;
    }
    NozzleType nozzleType = nozzle.GetNozzleType();
    if (nozzleType == ntHardenedSteel || nozzleType == ntStainlessSteel)
    {
        if (nozzle.GetNozzleFlowType() == H_FLOW && index >= 1)
        {
            m_nozzle_image = nozzle_hh[index - 1][0];
            m_icon_bitmap->SetBitmap(m_nozzle_image->bmp());
            m_scaled_nozzle_image = nozzle_hh[index - 1][1];
            findNozzleImage = true;
        }
        else if (nozzle.GetNozzleFlowType() == S_FLOW && index >= 0)
        {
            m_nozzle_image = nozzle_hs[index][0];
            m_icon_bitmap->SetBitmap(m_nozzle_image->bmp());
            m_scaled_nozzle_image = nozzle_hs[index][1];
            findNozzleImage = true;
        }
    }
    if (!findNozzleImage)
    {
        if (!m_nozzle_empty_image) {m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);}
        m_icon_bitmap->SetBitmap(m_nozzle_empty_image->bmp());
    }
    m_icon_bitmap->Refresh();
}

void wgtDeviceNozzleRackHotendUpdate::UpdateColourStyle(const wxColour& clr)
{
    SetBackgroundColour(clr);
    SetBorderColor(clr);

    // BFS: Update all children background color
    auto children = GetChildren();
    while (!children.IsEmpty())
    {
        auto win = children.front();
        children.pop_front();
        win->SetBackgroundColour(clr);

        for (auto child : win->GetChildren())
        {
            children.push_back(child);
        }
    }
}

void wgtDeviceNozzleRackHotendUpdate::UpdateExtruderNozzleInfo(const std::shared_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    if (rack)
    {
        DevNozzleSystem* nozzle_system = rack->GetNozzleSystem();
        if (nozzle_system)
        {
            UpdateInfo(nozzle_system->GetExtNozzle(m_ext_nozzle_id));
        }
    }
}

void wgtDeviceNozzleRackHotendUpdate::UpdateRackNozzleInfo(const std::shared_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    if (rack)
    {
        UpdateInfo(rack->GetNozzle(m_rack_nozzle_id));
    }
}

void wgtDeviceNozzleRackHotendUpdate::UpdateInfo(const DevNozzle& nozzle)
{
    /*update nozzle possition and background*/
    if (auto share = m_nozzle_rack.lock())
    {
        // if (share->GetReadingCount() > 0 && m_status_label->IsShown())
        if (share->GetReadingCount() > 0 && m_status_label->IsShown() && m_status_label->GetLabel() == _L("Refreshing"))
        {
            m_refreshing_icon->Play();
            m_refreshing_icon->Show();
            m_nozzle_status = NOZZLE_STATUS_DC;
            return;
        }
        else
        {
            m_refreshing_icon->Stop();
            m_refreshing_icon->Show(false);
        }
    }

    wxString filamentDisplayName{};
    for (auto iter = GUI::wxGetApp().preset_bundle->filaments.begin(); iter != GUI::wxGetApp().preset_bundle->filaments.end(); ++iter) 
    {
        const Preset& filament_preset = *iter;
        // const auto& config = filament_preset.config;
        if (filament_preset.filament_id == nozzle.GetFilamentId()) 
        {
            filamentDisplayName = wxString(filament_preset.alias);
        }
    }

    if (nozzle.IsEmpty() && m_nozzle_status != NOZZLE_STATUS_EMPTY)
    {
        m_nozzle_status = NOZZLE_STATUS_EMPTY;

        m_material_label->Show(false);
        m_colour_box->Show(false);

        m_diameter_label->Show(false);
        m_flowtype_label->Show(false);
        m_type_label->SetLabel(_L("Empty"));
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        m_sn_label->Show(false);
        m_version_label->Show(false);
        m_version_new_label->Show(false);

        updateNozzleImage(nozzle);

        m_used_time->Show(false);
        m_status_label->Show(false);
        m_status_bitmap->Show(false);

    }
    else if (nozzle.IsNormal() && m_nozzle_status != NOZZLE_STATUS_NORMAL)
    {
        m_nozzle_status = NOZZLE_STATUS_NORMAL;

        m_material_label->SetLabel(filamentDisplayName);
        m_colour_box->SetBackgroundColour(wxColour("#" + nozzle.GetFilamentColor()));
        m_colour_box->SetBorderColor(wxColour("#" + nozzle.GetFilamentColor()));
        m_material_label->Show(true);
        m_colour_box->Show(true);

        m_diameter_label->Show(true);
        m_flowtype_label->Show(true);
        m_diameter_label->SetLabel(nozzle.GetNozzleDiameterStr());
        m_flowtype_label->SetLabel(nozzle.GetNozzleFlowTypeStr());
        m_type_label->SetLabel(nozzle.GetNozzleTypeStr());
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        m_sn_label->Show(true);
        m_version_label->Show(true);

        updateNozzleImage(nozzle);

        m_used_time->Show(true);
        m_status_label->Show(false);
        m_status_bitmap->Show(false);
    }
    else if (nozzle.IsAbnormal() && m_nozzle_status != NOZZLE_STATUS_ABNORMAL)
    {
        m_nozzle_status = NOZZLE_STATUS_ABNORMAL;

        m_material_label->SetLabel(filamentDisplayName);
        m_colour_box->SetBackgroundColour(wxColour("#" + nozzle.GetFilamentColor()));
        m_colour_box->SetBorderColor(wxColour("#" + nozzle.GetFilamentColor()));
        m_material_label->Show(true);
        m_colour_box->Show(true);

        m_diameter_label->Show(true);
        m_flowtype_label->Show(true);
        m_diameter_label->SetLabel(nozzle.GetNozzleDiameterStr());
        m_flowtype_label->SetLabel(nozzle.GetNozzleFlowTypeStr());
        m_type_label->SetLabel(nozzle.GetNozzleTypeStr());
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        updateNozzleImage(nozzle);

        m_status_label->Show(true);
        m_status_bitmap->Show(true);
        m_status_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#E14747")));
        m_status_label->SetLabel(_L("Error"));
        m_status_bitmap->SetBitmap(m_error_icon->bmp());
        m_status_bitmap->Refresh();
    }
    else if (nozzle.IsUnknown()  && m_nozzle_status != NOZZLE_STATUS_UNKNOWN)
    {
        m_nozzle_status = NOZZLE_STATUS_UNKNOWN;

        m_colour_box->Show(false);
        m_material_label->SetLabel(wxString("--"));
        m_material_label->Show(true);

        m_diameter_label->Show(false);
        m_flowtype_label->Show(false);
        m_type_label->SetLabel(_L("Unknown"));
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        m_sn_label->Show(true);
        m_version_label->Show(true);

        updateNozzleImage(nozzle);

        m_used_time->Show(true);
        m_status_label->Show(true);
        m_status_bitmap->Show(true);
        m_status_label->SetForegroundColour(wxColour("#009688")); // Orca: accent green
        m_status_label->SetLabel(_L("Refresh"));
        m_status_bitmap->SetBitmap(m_refresh_icon->bmp());
        m_status_bitmap->Refresh();
    }

    // Update firmware info
    const DevFirmwareVersionInfo& firmware = nozzle.GetFirmwareInfo();
    if (nozzle.IsUnknown())
    {
        m_sn_label->SetLabel(wxString::Format("%s: --", _L("SN")));
        m_version_label->SetLabel(wxString::Format("%s: --", _L("Version")));
        m_version_new_label->Show(false);
    }
    if (nozzle.IsNormal() || nozzle.IsAbnormal())
    {
        if (firmware.isValid())
        {
            m_sn_label->SetLabel(wxString::Format("%s: %s", _L("SN"), firmware.sn));

            if (!firmware.sw_new_ver.empty() && firmware.sw_new_ver != firmware.sw_ver)
            {
                m_version_label->SetLabel(wxString::Format("%s: %s > ", _L("Version"), firmware.sw_ver));
                m_version_new_label->SetLabel(wxString::Format("%s", firmware.sw_new_ver));
                m_version_new_label->Show(true);
            }
            else
            {
                m_version_label->SetLabel(wxString::Format("%s:%s", _L("Version"), firmware.sw_ver));
                m_version_new_label->Show(false);
            }
        }
        else
        {
            m_sn_label->SetLabel(wxString::Format("%s: --", _L("SN")));
            m_version_label->SetLabel(wxString::Format("%s: --", _L("Version")));
            m_version_new_label->Show(false);
        }
    }

    if (!nozzle.IsEmpty())
    {
        if (nozzle.IsUnknown())
        {
            m_used_time->SetLabel(wxString::Format(_L("Used Time: %s"), "--h"));
        }
        else
        {
            // Update used time
            int usedSeconds = nozzle.GetNozzlePrintTime();
            if (usedSeconds < 60)
            {
                m_used_time->SetLabel(wxString::Format(_L("Used Time: %s"), "0 h"));
            }
            else
            {
                int printTime = (usedSeconds >= 3600 ? usedSeconds / 3600 : usedSeconds / 60);
                std::string printTimeStr = (usedSeconds >= 3600 ? std::to_string(printTime) + " h" : std::to_string(printTime) + " min");
                m_used_time->SetLabel(wxString::Format(_L("Used Time: %s"), printTimeStr.c_str()));
            }
        }

    }
}

void wgtDeviceNozzleRackHotendUpdate::Rescale()
{
    // update images
    if (m_nozzle_image) { m_nozzle_image->msw_rescale(); }
    if (m_nozzle_empty_image) { m_nozzle_empty_image->msw_rescale(); }
    if (m_nozzle_status == NOZZLE_STATUS_EMPTY && m_nozzle_empty_image)
    {
        m_icon_bitmap->SetBitmap(m_nozzle_empty_image->bmp());
    }
    else if (m_nozzle_image != nullptr)
    {
        m_icon_bitmap->SetBitmap(m_nozzle_image->bmp());
    }
    m_icon_bitmap->Refresh();
}

};// namespace Slic3r::GUI
