//**********************************************************/
/* File: uiAmsHumidityPopup.cpp
*  Description: The popup with DevAms Humidity
*
* \n class uiAmsHumidityPopup
//**********************************************************/

#include "uiAmsHumidityPopup.h"

#include "slic3r/Utils/WxFontUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"


#include <wx/dcgraph.h>
#include <wx/grid.h>
#include <wx/button.h>
#include <wx/textctrl.h>
#include <cctype>

namespace Slic3r { namespace GUI {

// Conservative, "do-not-exceed" drying profile per filament family. Temps are the
// safe ceiling for the material (heat-sensitive plastics like PLA/TPU must not be
// dried near their glass-transition or they deform). Used to pick a SAFE default
// for the whole AMS = the lowest-temp filament present.
static void filament_safe_drying(const std::string &type_in, int &temp, int &hours)
{
    std::string t = type_in;
    for (auto &c : t) c = (char) ::toupper((unsigned char) c);
    auto has = [&](const char *s) { return t.find(s) != std::string::npos; };
    if (has("TPU"))                 { temp = 45; hours = 12; return; }
    if (has("PVA"))                 { temp = 45; hours = 12; return; }
    if (has("PLA"))                 { temp = 45; hours = 8;  return; }
    if (has("PCTG") || has("PETG")) { temp = 65; hours = 8;  return; }
    if (has("HIPS"))                { temp = 60; hours = 8;  return; }
    if (has("PPS") || has("PPA"))   { temp = 90; hours = 12; return; }
    if (has("ASA"))                 { temp = 80; hours = 8;  return; }
    if (has("ABS"))                 { temp = 80; hours = 8;  return; }
    if (has("PET"))                 { temp = 65; hours = 8;  return; } // after PETG/PCTG
    if (has("PC"))                  { temp = 80; hours = 10; return; } // after PCTG
    if (has("NYLON") || has("PAHT") || has("PA6") || has("PA12") || has("PA"))
                                    { temp = 80; hours = 12; return; } // after PLA/PVA/PPA/PPS
    temp = 45; hours = 8; // unknown -> conservative
}

uiAmsPercentHumidityDryPopup::uiAmsPercentHumidityDryPopup(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "")
{
    Create();
    wxGetApp().UpdateDlgDarkUI(this); // Orca: apply dark-mode theming to the popup
}

void uiAmsPercentHumidityDryPopup::Create()
{
    // create images
    idle_img = ScalableBitmap(this, "ams_drying", 16);
    drying_img = ScalableBitmap(this, "ams_is_drying", 16); // Orca: use the drying-state icon

    // background
    SetBackgroundColour(*wxWHITE);

    // create title sizer
    wxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);

    Label* title = new Label(this, _L("Current AMS humidity"));
    title->SetForegroundColour(*wxBLACK);
    title->SetBackgroundColour(*wxWHITE);
    title->SetFont(Label::Head_18);

    title_sizer->AddStretchSpacer();
    title_sizer->Add(title, 0);
    title_sizer->AddStretchSpacer();

    // create humidity image
    m_humidity_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);

    // create dry state sizer
    wxGridSizer* dry_state_sizer = new wxGridSizer(2, FromDIP(5), FromDIP(5));
    m_dry_state_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
    m_dry_state_img->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_dry_state_img->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_dry_state = new Label(this);
    m_dry_state->SetForegroundColour(*wxBLACK);
    m_dry_state->SetBackgroundColour(*wxWHITE);
    m_dry_state->SetFont(Label::Body_14);
    dry_state_sizer->Add(m_dry_state_img, 1, wxALIGN_RIGHT);
    dry_state_sizer->Add(m_dry_state, 1, wxALIGN_LEFT);

    // create table grid sizer
    wxGridSizer* grid_sizer = new wxGridSizer(2, 3, FromDIP(10), FromDIP(10));
    m_humidity_header = new Label(this, _L("Humidity"));
    m_temperature_header = new Label(this, _L("Temperature"));
    left_dry_time_header = new Label(this, _L("Left Time"));
    m_humidity_label = new Label(this);
    m_temperature_label = new Label(this);
    left_dry_time_label = new Label(this);

    grid_sizer->Add(m_humidity_header, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_temperature_header, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(left_dry_time_header, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_humidity_label, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_temperature_label, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(left_dry_time_label, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);

    // complete main sizer
    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->AddSpacer(FromDIP(10));
    m_sizer->Add(title_sizer, 1, wxEXPAND | wxHORIZONTAL);
    m_sizer->Add(m_humidity_img, 1, wxALIGN_CENTER_HORIZONTAL, 0);
    m_sizer->AddSpacer(FromDIP(10));
    m_sizer->Add(dry_state_sizer, 1 ,wxEXPAND | wxHORIZONTAL);
    m_sizer->Add(grid_sizer, 1, wxEXPAND | wxHORIZONTAL, FromDIP(15));

    // active drying control row (Start/Stop). Shown only when the selected printer
    // supports remote AMS drying (see UpdateContents()).
    m_dry_ctrl_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_dry_temp_input = new wxTextCtrl(this, wxID_ANY, "45", wxDefaultPosition, wxSize(FromDIP(40), -1));
    m_dry_time_input = new wxTextCtrl(this, wxID_ANY, "8",  wxDefaultPosition, wxSize(FromDIP(40), -1));
    m_dry_start_btn  = new wxButton(this, wxID_ANY, _L("Start Drying"));
    m_dry_stop_btn   = new wxButton(this, wxID_ANY, _L("Stop Drying"));
    m_dry_ctrl_sizer->Add(new Label(this, _L("Temp")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    m_dry_ctrl_sizer->Add(m_dry_temp_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    m_dry_ctrl_sizer->Add(new Label(this, wxString::FromUTF8(u8"℃")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_dry_ctrl_sizer->Add(new Label(this, _L("Time")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    m_dry_ctrl_sizer->Add(m_dry_time_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    m_dry_ctrl_sizer->Add(new Label(this, _L("h")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_dry_ctrl_sizer->Add(m_dry_start_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_dry_ctrl_sizer->Add(m_dry_stop_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_dry_start_btn->Bind(wxEVT_BUTTON, &uiAmsPercentHumidityDryPopup::OnStartDrying, this);
    m_dry_stop_btn->Bind(wxEVT_BUTTON, &uiAmsPercentHumidityDryPopup::OnStopDrying, this);
    // each time the popup opens, prefill temp/time with a safe default derived from
    // the lowest-temp filament currently in this AMS (fires only on show, so it
    // never overwrites a value the user just typed while it's open)
    Bind(wxEVT_SHOW, [this](wxShowEvent &e) { e.Skip(); if (e.IsShown()) set_safe_drying_defaults(); });
    m_sizer->Add(m_dry_ctrl_sizer, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(8));

    m_sizer->AddSpacer(FromDIP(10));
    SetSizer(m_sizer);

    SetSize(wxSize(FromDIP(400), FromDIP(330)));
    SetMinSize(wxSize(FromDIP(400), FromDIP(330)));
    SetMaxSize(wxSize(FromDIP(400), FromDIP(330)));

    Fit();
    Layout();
    Refresh();
}

void uiAmsPercentHumidityDryPopup::Update(int humidiy_level, int humidity_percent, int left_dry_time, float current_temperature)
{
    if (m_humidity_level != humidiy_level || m_humidity_percent != humidity_percent ||
        m_left_dry_time != left_dry_time || m_current_temperature != current_temperature)
    {
        m_humidity_level   = humidiy_level;
        m_humidity_percent = humidity_percent;
        m_left_dry_time    = left_dry_time;
        m_current_temperature = current_temperature;

        UpdateContents();
    }
}

void uiAmsPercentHumidityDryPopup::UpdateContents()
{
    // humitidy image
    if (0 < m_humidity_level && m_humidity_level < 6)
    {
        ScalableBitmap humitidy_image;
        if (wxGetApp().dark_mode())
        {
            humitidy_image = ScalableBitmap(this, "hum_level" + std::to_string(m_humidity_level) + "_no_num_light", 64);
        }
        else
        {
            humitidy_image = ScalableBitmap(this, "hum_level" + std::to_string(m_humidity_level) + "_no_num_light", 64);
        }

        m_humidity_img->SetBitmap(humitidy_image.bmp());
    }

    // dry state
    if (m_left_dry_time > 0)
    {
        m_dry_state_img->SetBitmap(drying_img.bmp());
        m_dry_state->SetLabel(_L("Drying"));
        m_dry_state->Fit();
    }
    else
    {
        m_dry_state_img->SetBitmap(idle_img.bmp());
        m_dry_state->SetLabel(_L("Idle"));
        m_dry_state->Fit();
    }

    // table grid
    const wxString& humidity_str = wxString::Format("%d%%", m_humidity_percent);
    m_humidity_label->SetLabel(humidity_str);
    // Orca: format the temperature with an explicit UTF-8 \u2103 (no translation of a units-only format)
    const wxString& temp_str = wxString::Format(wxString::FromUTF8(u8"%d\u2103" /* \u00b0C */), (int)std::round(m_current_temperature));
    m_temperature_label->SetLabel(temp_str);

    if (m_left_dry_time > 0)
    {
        wxString display_hour_str;
        int left_hours = m_left_dry_time / 60;
        if (left_hours < 10) {
            display_hour_str = wxString::Format("0%d", left_hours);
        } else {
            display_hour_str = wxString::Format("%d", left_hours);
        }

        wxString display_min_str;
        int left_minutes = m_left_dry_time % 60;
        if (left_minutes < 10) {
            display_min_str = wxString::Format("0%d", left_minutes);
        } else {
            display_min_str = wxString::Format("%d", left_minutes);
        }

        const wxString& time_str = wxString::Format("%s : %s", display_hour_str, display_min_str);
        left_dry_time_label->SetLabel(time_str);
    }
    else
    {
        left_dry_time_label->SetLabel(_L("Idle"));
    }

    // show the active-drying controls only when the selected printer supports remote AMS drying
    bool support_dry = false;
    if (auto *dev = wxGetApp().getDeviceManager()) {
        if (MachineObject *obj = dev->get_selected_machine())
            support_dry = obj->is_support_remote_dry;
    }
    if (m_dry_ctrl_sizer)
        m_sizer->Show(m_dry_ctrl_sizer, support_dry, true);

    Fit();
    Layout();
    Refresh();
}

void uiAmsPercentHumidityDryPopup::OnStartDrying(wxCommandEvent &e)
{
    auto *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    if (!obj) return;
    long temp = 45, hours = 8;
    if (m_dry_temp_input) m_dry_temp_input->GetValue().ToLong(&temp);
    if (m_dry_time_input) m_dry_time_input->GetValue().ToLong(&hours);
    int ams_id = 0;
    try { ams_id = std::stoi(m_ams_id); } catch (...) {}
    obj->command_ams_drying_start(ams_id, std::string(), (int) temp, (int) hours);
    EndModal(wxID_OK);
}

void uiAmsPercentHumidityDryPopup::OnStopDrying(wxCommandEvent &e)
{
    auto *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    if (!obj) return;
    int ams_id = 0;
    try { ams_id = std::stoi(m_ams_id); } catch (...) {}
    obj->command_ams_drying_stop(ams_id);
    EndModal(wxID_OK);
}

void uiAmsPercentHumidityDryPopup::set_safe_drying_defaults()
{
    int temp = 45, hours = 8; // conservative fallback (PLA-safe)
    auto *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    if (obj && obj->GetFilaSystem()) {
        auto &ams_list = obj->GetFilaSystem()->GetAmsList();
        auto it = ams_list.find(m_ams_id);
        if (it != ams_list.end() && it->second) {
            bool found = false;
            int best_temp = 0, best_hours = 0;
            for (auto &tray : it->second->GetTrays()) {
                if (!tray.second) continue;
                std::string ftype = tray.second->get_filament_type();
                if (ftype.empty()) continue;
                int t = 45, h = 8;
                filament_safe_drying(ftype, t, h);
                // pick the lowest-temp (most heat-sensitive) filament; ties -> longer time
                if (!found || t < best_temp || (t == best_temp && h > best_hours)) {
                    best_temp = t; best_hours = h; found = true;
                }
            }
            if (found) { temp = best_temp; hours = best_hours; }
        }
    }
    if (m_dry_temp_input) m_dry_temp_input->SetValue(wxString::Format("%d", temp));
    if (m_dry_time_input) m_dry_time_input->SetValue(wxString::Format("%d", hours));
}

void uiAmsPercentHumidityDryPopup::msw_rescale()
{
    idle_img.msw_rescale();
    drying_img.msw_rescale();
    UpdateContents();
}

} // namespace GUI

} // namespace Slic3r