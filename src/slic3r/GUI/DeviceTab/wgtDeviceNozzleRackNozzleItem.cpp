//**********************************************************/
/* File: wgtDeviceNozzleRackNozzleItem.cpp
*  Description: One nozzle cell of the H2C hotend rack view. Holds the
*  wgtDeviceNozzleRackNozzleItem widget, the SetNozzleBmpColor helper and the layout
*  constants it needs.
//**********************************************************/

#include "wgtDeviceNozzleRackNozzleItem.h"

#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include "slic3r/GUI/Widgets/Label.hpp"

#define WX_DIP_SIZE_46 wxSize(FromDIP(46), FromDIP(46))
#define WX_DIP_SIZE(x, y) wxSize(FromDIP(x), FromDIP(y))

#define WGT_RACK_NOZZLE_SIZE WX_DIP_SIZE(88, 100)

static wxColour s_gray_clr("#B0B0B0");
static wxColour s_hgreen_clr("#009688");
static wxColour s_red_clr("#D01B1B");

wxDEFINE_EVENT(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, wxCommandEvent);

namespace Slic3r::GUI
{

static wxBitmap SetNozzleBmpColor(const wxBitmap& bmp, const std::string& color_str) {
    if(color_str.empty()) return bmp;

    wxImage img = bmp.ConvertToImage();
    wxColour color("#" + color_str);

    for (int y = 0; y < img.GetHeight(); ++y) {
        for (int x = 0; x < img.GetWidth(); ++x) {
            unsigned char r = img.GetRed(x, y);
            unsigned char g = img.GetGreen(x, y);
            unsigned char b = img.GetBlue(x, y);

            /*replace yellow with color*/
            if ( r >= 180 && g >= 180 && b <= 150) {
                img.SetRGB(x, y, color.Red(), color.Green(), color.Blue());
            }
        }
    }

    return wxBitmap(img, -1, bmp.GetScaleFactor());
}

wgtDeviceNozzleRackNozzleItem::wgtDeviceNozzleRackNozzleItem(wxWindow* parent, int nozzle_id)
    : StaticBox(parent, wxID_ANY), m_nozzle_id(nozzle_id)
{
    CreateGui();
}

void wgtDeviceNozzleRackNozzleItem::CreateGui()
{
    // Background
    SetCornerRadius(FromDIP(5));
    SetBackgroundColor(*wxWHITE);

    // Top H
    wxSizer *top_h_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_nozzle_label_id = new Label(this);
    m_nozzle_label_id->SetFont(Label::Body_12);
    m_nozzle_label_id->SetBackgroundColour(*wxWHITE);
    m_nozzle_label_id->SetLabel(wxString::Format("%d", m_nozzle_id + 1));

    m_status             = NOZZLE_STATUS::NOZZLE_EMPTY;
    m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);
    m_nozzle_icon = new wxStaticBitmap(this, wxID_ANY, m_nozzle_empty_image->bmp(), wxDefaultPosition, WX_DIP_SIZE_46);
    m_nozzle_icon->SetBackgroundColour(*wxWHITE);

    m_nozzle_selected_bitmap = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, WX_DIP_SIZE(20, 20));
    m_nozzle_selected_bitmap->SetBackgroundColour(*wxWHITE);

    top_h_sizer->Add(m_nozzle_label_id, 0, wxTOP | wxLEFT, FromDIP(6));
    top_h_sizer->AddStretchSpacer(1);
    top_h_sizer->Add(m_nozzle_icon, 0, wxTOP, FromDIP(10));
    top_h_sizer->AddStretchSpacer(1);
    top_h_sizer->Add(m_nozzle_selected_bitmap, 0, wxTOP | wxRIGHT, FromDIP(2));

    // Bottom V
    wxBoxSizer* bottom_v = new wxBoxSizer(wxVERTICAL);

    wxSizer* label_h_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_nozzle_label_1 = new Label(this);
    m_nozzle_label_1->SetFont(Label::Body_12);
    m_nozzle_label_1->SetBackgroundColour(*wxWHITE);
    m_nozzle_label_1->SetLabel(_L("Empty"));

    label_h_sizer->Add(m_nozzle_label_1, 0, wxALIGN_LEFT);

    auto status_icon = create_scaled_bitmap("dev_rack_nozzle_error_icon", this, 14);
    m_nozzle_status_icon = new wxStaticBitmap(this, wxID_ANY, status_icon, wxDefaultPosition, WX_DIP_SIZE(14, 14));
    m_nozzle_status_icon->Bind(wxEVT_LEFT_DOWN, &wgtDeviceNozzleRackNozzleItem::OnBtnNozzleStatus, this);
    m_nozzle_status_icon->Bind(wxEVT_ENTER_WINDOW, [this](auto&) { SetCursor(wxCURSOR_HAND); });
    m_nozzle_status_icon->Bind(wxEVT_LEAVE_WINDOW, [this](auto&) { SetCursor(wxCURSOR_ARROW); });
    m_nozzle_status_icon->SetBackgroundColour(*wxWHITE);
    m_nozzle_status_icon->Show(false);

    label_h_sizer->Add(m_nozzle_status_icon, 0, wxALIGN_CENTER | wxLEFT, FromDIP(2));
    bottom_v->Add(label_h_sizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));

    m_nozzle_label_2 = new Label(this);
    m_nozzle_label_2->SetFont(Label::Body_12);
    m_nozzle_label_2->SetBackgroundColour(*wxWHITE);
    bottom_v->Add(m_nozzle_label_2, 0, wxALIGN_CENTER_HORIZONTAL);

    // Main sizer
    wxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(top_h_sizer, 0, wxEXPAND);
    main_sizer->Add(bottom_v, 0, wxALIGN_CENTER_HORIZONTAL);
    SetSizer(main_sizer);

    SetMinSize(WGT_RACK_NOZZLE_SIZE);
    SetMaxSize(WGT_RACK_NOZZLE_SIZE);
    SetSize(WGT_RACK_NOZZLE_SIZE);
    Layout();
};

void wgtDeviceNozzleRackNozzleItem::SetSelected(bool selected)
{
    if (!m_enable_select){
        assert(false && "not support select");
        return;
    }

    if (m_is_selected != selected) {
        m_is_selected = selected;
        if (selected) {
            if (!m_nozzle_selected_image) {
                m_nozzle_selected_image = new ScalableBitmap(this, "dev_rack_nozzle_selected", 20);
            }

            m_nozzle_selected_bitmap->SetBitmap(m_nozzle_selected_image->bmp());
            SetBorderColor(StateColor::darkModeColorFor(s_hgreen_clr));
        } else {
            m_nozzle_selected_bitmap->SetBitmap(wxNullBitmap);
            SetBorderColor(StateColor::darkModeColorFor(s_gray_clr));
        }

        Refresh();
    }
}

void wgtDeviceNozzleRackNozzleItem::Update(const std::shared_ptr<DevNozzleRack> rack, bool on_rack /*= true*/)
{
    m_rack = rack;

    if (rack) {
        const auto        &nozzle_info  = on_rack ? rack->GetNozzle(m_nozzle_id) : rack->GetNozzleSystem()->GetExtNozzle(m_nozzle_id);
        const wxString    &diameter_str = nozzle_info.GetNozzleDiameterStr();
        const wxString    &flowtype_str = nozzle_info.GetNozzleFlowTypeStr();
        const std::string &color        = nozzle_info.GetFilamentColor();

        /*check empty first*/
        if (nozzle_info.IsEmpty()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_EMPTY, _L("Empty"), wxEmptyString, color);
        } else if (nozzle_info.IsNormal()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_NORMAL, diameter_str, flowtype_str, color);
        } else if (nozzle_info.IsAbnormal()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_ERROR, _L("Error"), wxEmptyString, color);
        } else if (nozzle_info.IsUnknown()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_UNKNOWN, _L("Unknown"), wxEmptyString, color);
        }
    }
}

void wgtDeviceNozzleRackNozzleItem::SetNozzleStatus(NOZZLE_STATUS status, const wxString& str1, const wxString& str2, const std::string& color)
{
    if (m_status != status || m_filament_color != color)
    {
        m_status = status;
        m_filament_color = color;
        switch (status)
        {
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_EMPTY:
        {
            if (!m_nozzle_empty_image) { m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);}
            m_nozzle_icon->SetBitmap(m_nozzle_empty_image->bmp());
            break;
        }
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_NORMAL:
        {
            if (!m_nozzle_normal_image) { m_nozzle_normal_image = new ScalableBitmap(this, "dev_rack_nozzle_normal", 46);}
            m_nozzle_icon->SetBitmap(SetNozzleBmpColor(m_nozzle_normal_image->bmp(), m_filament_color));
            break;
        }
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_UNKNOWN:
        {
            if (!m_nozzle_unknown_image) { m_nozzle_unknown_image = new ScalableBitmap(this, "dev_rack_nozzle_unknown", 46);}
            m_nozzle_icon->SetBitmap(m_nozzle_unknown_image->bmp());
            break;
        }
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR:
        {
            if (!m_nozzle_error_image) { m_nozzle_error_image = new ScalableBitmap(this, "dev_rack_nozzle_error", 46);}
            m_nozzle_icon->SetBitmap(m_nozzle_error_image->bmp());
            break;
        }
        default:
        {
            break;
        }
        }

        if (status == wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR)
        {
            m_nozzle_label_1->SetForegroundColour(StateColor::darkModeColorFor(s_red_clr));
            m_nozzle_status_icon->Show(true);
        }
        else
        {
            m_nozzle_label_1->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));
            m_nozzle_status_icon->Show(false);
        }
    }

    bool update_layout = (m_nozzle_label_1->GetLabel() != str1 || m_nozzle_label_2->GetLabel() != str2);
    m_nozzle_label_1->SetLabel(str1);
    m_nozzle_label_2->SetLabel(str2);

    if (update_layout) {
        Layout();
    }
}

void wgtDeviceNozzleRackNozzleItem::OnBtnNozzleStatus(wxMouseEvent& evt)
{
    if (m_is_disabled) {
        return;
    }

    auto rack = m_rack.lock();
    if (rack && m_status == wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR)
    {
        // Orca: show the abnormal-hotend warning as an informational dialog only, with no
        // "Jump to the upgrade page" button, since Orca's MessageDialog and device Upgrade UI
        // have no such entry point. Fires when tapping an error-state rack nozzle's status icon.
        MessageDialog dlg(nullptr, _L("The hotend is in an abnormal state and currently unavailable. "
            "Please go to 'Device -> Upgrade' to upgrade firmware."), _L("Abnormal Hotend"), wxICON_WARNING | wxOK);
        dlg.ShowModal();
    }
}

void wgtDeviceNozzleRackNozzleItem::Rescale()
{
    if (m_nozzle_normal_image) { m_nozzle_normal_image->msw_rescale(); }
    if (m_nozzle_empty_image) { m_nozzle_empty_image->msw_rescale(); }
    if (m_nozzle_unknown_image) { m_nozzle_unknown_image->msw_rescale(); }
    if (m_nozzle_error_image) { m_nozzle_error_image->msw_rescale(); }

    auto status_icon = create_scaled_bitmap("dev_rack_nozzle_error_icon", this, 14);
    m_nozzle_status_icon->SetBitmap(status_icon);
    m_nozzle_status_icon->Refresh();

    if (m_nozzle_selected_image) {
        m_nozzle_selected_image->msw_rescale();
        if (m_is_selected) {
            m_nozzle_selected_bitmap->SetBitmap(m_nozzle_selected_image->bmp());
        }
    };

    switch (m_status)
    {
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_EMPTY:
    {
        m_nozzle_icon->SetBitmap(m_nozzle_empty_image->bmp());
        break;
    }
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_NORMAL:
    {
        m_nozzle_icon->SetBitmap(SetNozzleBmpColor(m_nozzle_normal_image->bmp(), m_filament_color));
        break;
    }
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_UNKNOWN:
    {
        m_nozzle_icon->SetBitmap(m_nozzle_unknown_image->bmp());
        break;
    }
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR:
    {
        m_nozzle_icon->SetBitmap(m_nozzle_error_image->bmp());
        break;
    }
    default:
    {
        break;
    }
    };
};

void wgtDeviceNozzleRackNozzleItem::EnableSelect()
{
    if (m_enable_select == true) {
        return;
    };

    m_enable_select = true;
    m_nozzle_icon->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    m_nozzle_label_id->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    m_nozzle_label_1->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    m_nozzle_label_2->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
}

void wgtDeviceNozzleRackNozzleItem::OnItemSelected(wxMouseEvent& evt)
{
    if (m_enable_select && !m_is_disabled){
        SetSelected(true);
        wxCommandEvent command_evt(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, GetId());
        command_evt.SetEventObject(this);
        ProcessEvent(command_evt);
    }

    evt.Skip();
}


void wgtDeviceNozzleRackNozzleItem::SetDisable(bool disabled)
{
    if (m_is_disabled == disabled) {
        return;
    }

    m_is_disabled = disabled;

    auto bg_clr = disabled ? StateColor::darkModeColorFor("#E5E7EB") : StateColor::darkModeColorFor(*wxWHITE);
    m_nozzle_icon->SetBackgroundColour(bg_clr);
    m_nozzle_label_id->SetBackgroundColour(bg_clr);
    m_nozzle_label_1->SetBackgroundColour(bg_clr);
    m_nozzle_status_icon->SetBackgroundColour(bg_clr);
    m_nozzle_label_2->SetBackgroundColour(bg_clr);
    m_nozzle_selected_bitmap->SetBackgroundColour(bg_clr);

    SetBackgroundColor(bg_clr);
    Refresh();
};

};// end of namespace Slic3r::GUI
