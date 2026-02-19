//**********************************************************/
/* File: wgtDeviceNozzleRackUpdate.cpp
*  Description: The panel with rack updating
*
* \n class wgtDeviceNozzleRackUpdate
//**********************************************************/

#include "wgtDeviceNozzleRackUpdate.h"

#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"

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

wgtDeviceNozzleRackUpgradeDlg::wgtDeviceNozzleRackUpgradeDlg(wxWindow* parent, const std::shared_ptr<DevNozzleRack> rack)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    m_rack_upgrade_panel = new wgtDeviceNozzleRackUprade(this);
    m_rack_upgrade_panel->UpdateRackInfo(rack);

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

        main_sizer->Add(item, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        if (id < 5)
        {
            wxPanel* separator = new wxPanel(this);
            separator->SetMaxSize(wxSize(-1, FromDIP(1)));
            separator->SetMinSize(wxSize(-1, FromDIP(1)));
            separator->SetBackgroundColour(WXCOLOUR_GREY300);
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

    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Index
    m_idx_label = new Label(this);
    m_idx_label->SetFont(Label::Head_14);
    m_idx_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    content_sizer->Add(m_idx_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(25));

    // Icon
    m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);
    m_icon_bitmap = new wxStaticBitmap(this, wxID_ANY, m_nozzle_empty_image->bmp());
    content_sizer->Add(m_icon_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(25));

    // Diameter/type (vertical)
    wxPanel* type_panel = new wxPanel(this);
    auto* type_sizer = new wxBoxSizer(wxVERTICAL);
    m_diameter_label = new Label(type_panel);
    m_diameter_label->SetFont(Label::Body_12);
    m_diameter_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_flowtype_label = new Label(type_panel);
    m_flowtype_label->SetFont(Label::Body_12);
    m_flowtype_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_type_label = new Label(type_panel);
    m_type_label->SetFont(Label::Body_12);
    m_type_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    type_sizer->AddStretchSpacer();
    type_sizer->Add(m_diameter_label, 0, wxALIGN_LEFT);
    type_sizer->Add(m_flowtype_label, 0, wxALIGN_LEFT);
    type_sizer->Add(m_type_label, 0, wxALIGN_LEFT);
    type_sizer->AddStretchSpacer();
    type_panel->SetSizer(type_sizer);
    type_panel->SetMaxSize(WX_DIP_SIZE(100, -1));
    type_panel->SetMinSize(WX_DIP_SIZE(100, -1));
    type_panel->SetSize(WX_DIP_SIZE(100, -1));
    content_sizer->Add(type_panel, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(12));

    // SN and version (vertical)
    auto* info_sizer = new wxBoxSizer(wxVERTICAL);
    m_sn_label = new Label(this);
    m_sn_label->SetFont(Label::Body_12);
    m_sn_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    auto* version_h_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_version_label = new Label(this);
    m_version_label->SetFont(Label::Body_12);
    m_version_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);

    m_version_new_label = new Label(this);
    m_version_new_label->SetFont(Label::Body_12);
    m_version_new_label->SetBackgroundColour(WGT_DEVICE_NOZZLE_RACK_HOTEND_UPDATE_DEFAULT_BG);
    m_version_new_label->SetForegroundColour(wxColour(0, 168, 84)); // Green

    version_h_sizer->Add(m_version_label, 0, wxALIGN_CENTER_VERTICAL);
    version_h_sizer->Add(m_version_new_label, 0, wxALIGN_CENTER_VERTICAL);
    info_sizer->Add(m_sn_label, 0, wxALIGN_LEFT);
    info_sizer->Add(version_h_sizer, 0, wxALIGN_LEFT);
    content_sizer->Add(info_sizer, 1, wxALIGN_CENTER_VERTICAL | wxLEFT);
    content_sizer->AddSpacer(FromDIP(25));

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(content_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));

    SetSizer(main_sizer);
    Layout();
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
    if (nozzle.IsEmpty() && m_nozzle_status != NOZZLE_STATUS_EMPTY)
    {
        m_nozzle_status = NOZZLE_STATUS_EMPTY;
        m_diameter_label->Show(false);
        m_flowtype_label->Show(false);
        m_type_label->SetLabel(_L("Empty"));
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        if (!m_nozzle_empty_image) {m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);}
        m_icon_bitmap->SetBitmap(m_nozzle_empty_image->bmp());
        m_icon_bitmap->Refresh();
    }
    else if (nozzle.IsNormal() && m_nozzle_status != NOZZLE_STATUS_NORMAL)
    {
        m_nozzle_status = NOZZLE_STATUS_NORMAL;
        m_diameter_label->Show(true);
        m_flowtype_label->Show(true);
        m_diameter_label->SetLabel(nozzle.GetNozzleDiameterStr());
        m_flowtype_label->SetLabel(nozzle.GetNozzleFlowTypeStr());
        m_type_label->SetLabel(nozzle.GetNozzleTypeStr());
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        if (!m_nozzle_image) {m_nozzle_image = new ScalableBitmap(this, "dev_rack_nozzle_normal", 46);}
        m_icon_bitmap->SetBitmap(m_nozzle_image->bmp());
        m_icon_bitmap->Refresh();
    }
    else if (nozzle.IsAbnormal() && m_nozzle_status != NOZZLE_STATUS_ABNORMAL)
    {
        m_nozzle_status = NOZZLE_STATUS_ABNORMAL;
        m_diameter_label->Show(false);
        m_flowtype_label->Show(false);
        m_type_label->SetLabel(_L("Error"));
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(s_red_clr));

        if (!m_nozzle_image) { m_nozzle_image = new ScalableBitmap(this, "dev_rack_nozzle_normal", 46); }
        m_icon_bitmap->SetBitmap(m_nozzle_image->bmp());
        m_icon_bitmap->Refresh();
    }
    else if (nozzle.IsUnknown()  && m_nozzle_status != NOZZLE_STATUS_UNKNOWN)
    {
        m_nozzle_status = NOZZLE_STATUS_UNKNOWN;
        m_diameter_label->Show(false);
        m_flowtype_label->Show(false);
        m_type_label->SetLabel(_L("Unknown"));
        m_type_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

        if (!m_nozzle_image) { m_nozzle_image = new ScalableBitmap(this, "dev_rack_nozzle_normal", 46); }
        m_icon_bitmap->SetBitmap(m_nozzle_image->bmp());
        m_icon_bitmap->Refresh();
    }

    // Update firmware info
    const DevFirmwareVersionInfo& firmware = nozzle.GetFirmwareInfo();
    if (!nozzle.IsAbnormal())
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
            m_sn_label->SetLabel(wxString::Format("%s: N/A", _L("SN")));
            m_version_label->SetLabel(wxString::Format("%s: N/A", _L("Version")));
            m_version_new_label->Show(false);
        }
    }
    else /*default to show update icon if IsAbnormal*/
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
            m_sn_label->SetLabel(wxString::Format("%s: N/A", _L("SN")));
            m_version_label->SetLabel(wxString::Format("%s: N/A", _L("Version")));
            m_version_new_label->Show(false);
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
