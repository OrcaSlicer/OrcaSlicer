#include "SafetyOptionsDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include "Widgets/SwitchButton.hpp"
#include "MsgDialog.hpp"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevNozzleSystem.h"
#include "DeviceCore/DevPrintOptions.h"

static const wxColour STATIC_BOX_LINE_COL = wxColour(238, 238, 238);
static const wxColour STATIC_TEXT_CAPTION_COL = wxColour(100, 100, 100);
static const wxColour STATIC_TEXT_EXPLAIN_COL = wxColour(100, 100, 100);

namespace Slic3r { namespace GUI {

static StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(194, 194, 194), StateColor::Disabled),
                               std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                               std::pair<wxColour, int>(wxColour(0, 177, 66), StateColor::Normal));

SafetyOptionsDialog::SafetyOptionsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Safety Options"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    this->SetDoubleBuffered(true);
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    SetBackgroundColour(*wxWHITE);
    SetSize(FromDIP(480),FromDIP(320));

    m_scrollwindow = new wxScrolledWindow(this, wxID_ANY);
    m_scrollwindow->SetScrollRate(0, FromDIP(10));
    m_scrollwindow->SetBackgroundColour(*wxWHITE);
    m_scrollwindow->SetMinSize(wxSize(FromDIP(480), wxDefaultCoord));
    m_scrollwindow->SetMaxSize(wxSize(FromDIP(480), wxDefaultCoord));

    auto m_options_sizer = create_settings_group(m_scrollwindow);
    m_options_sizer->SetMinSize(wxSize(FromDIP(460), wxDefaultCoord));

    m_scrollwindow->SetSizer(m_options_sizer);

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(m_scrollwindow, 1, wxEXPAND);
    this->SetSizer(mainSizer);

    m_options_sizer->Fit(m_scrollwindow);
    m_scrollwindow->FitInside();

    this->Layout();

    m_cb_open_door->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
        if (m_cb_open_door->GetValue()) {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING); }
        } else {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_DISABLE); }
        }
        evt.Skip();
    });

    m_open_door_switch_board->Bind(wxCUSTOMEVT_SWITCH_POS, [this](wxCommandEvent &evt)
    {
        if (evt.GetInt() == 0)
        {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_PAUSE_PRINT); }
        }
        else if (evt.GetInt() == 1)
        {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING); }
        }
        evt.Skip();
    });

    m_cb_idel_heating_protection->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
        if (obj)
        {
            obj->GetPrintOptions()->command_xcam_control_idelheatingprotect_detector(m_cb_idel_heating_protection->GetValue());
        }
        else
        {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << "obj is empty";
        }
            evt.Skip();
    });

    auto toast_click = [this](wxMouseEvent &e) {
        if (m_idel_protect_unavailable) {
            show_idel_heating_toast(_L("Unavailable while heating maintenance function is on."));
        }
        e.Skip();
    };
    m_text_idel_heating_protection->Bind(wxEVT_LEFT_DOWN, toast_click);
    m_text_idel_heating_protection_caption->Bind(wxEVT_LEFT_DOWN, toast_click);
    m_idel_heating_container->Bind(wxEVT_LEFT_DOWN, toast_click);

    m_idel_heating_toast_timer.SetOwner(this);
    Bind( wxEVT_TIMER, [this](wxTimerEvent &e) {
             if (m_idel_heating_toast) {
                 m_idel_heating_toast->Destroy();
                 m_idel_heating_toast = nullptr;
             }}, m_idel_heating_toast_timer.GetId());

    wxGetApp().UpdateDlgDarkUI(this);
}

SafetyOptionsDialog::~SafetyOptionsDialog()
{
}

void SafetyOptionsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
}

void SafetyOptionsDialog::update_options(MachineObject* obj_)
{
    if (!obj_) return;

    updateOpenDoorCheck(obj_);
    updateIdelHeatingProtect(obj_);

    this->Freeze();
    this->Thaw();
    Layout();
}

void SafetyOptionsDialog::updateOpenDoorCheck(MachineObject *obj) {
    if (!obj || !obj->support_door_open_check()) {
        m_cb_open_door->Hide();
        m_text_open_door->Hide();
        m_open_door_switch_board->Hide();
        return;
    }

    if (obj->get_door_open_check_state() != MachineObject::DOOR_OPEN_CHECK_DISABLE) {
        m_cb_open_door->SetValue(true);
        m_open_door_switch_board->Enable();

        if (obj->get_door_open_check_state() == MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING) {
            m_open_door_switch_board->updateState("left");
            m_open_door_switch_board->Refresh();
        } else if (obj->get_door_open_check_state() == MachineObject::DOOR_OPEN_CHECK_ENABLE_PAUSE_PRINT) {
            m_open_door_switch_board->updateState("right");
            m_open_door_switch_board->Refresh();
        }

    } else {
        m_cb_open_door->SetValue(false);
        m_open_door_switch_board->Disable();
    }

    m_cb_open_door->Show();
    m_text_open_door->Show();
    m_open_door_switch_board->Show();
}

void SafetyOptionsDialog::updateIdelHeatingProtect(MachineObject *obj)
{
    if (obj->is_support_idelheadingprotect_detection) {
        m_idel_heating_container->Show();
    } else {
        m_idel_heating_container->Hide();
        m_idel_protect_unavailable = false;
        return;
    }

    if (obj->GetPrintOptions()->GetIdelHeatingProtectEenabled() == 2)
    {
        m_cb_idel_heating_protection->SetValue(false);
        m_cb_idel_heating_protection->Enable(false);
        m_text_idel_heating_protection->SetForegroundColour(wxColour(170, 170, 170));
        m_text_idel_heating_protection_caption->SetForegroundColour(wxColour(170, 170, 170));
        m_cb_idel_heating_protection->SetBackgroundColour(wxColour(255, 255, 255));
        m_idel_protect_unavailable = true;
    } else {
        m_cb_idel_heating_protection->Enable(true);
        m_cb_idel_heating_protection->SetValue(obj->GetPrintOptions()->GetIdelHeatingProtectEenabled());
        m_text_idel_heating_protection->SetForegroundColour(*wxBLACK);
        m_text_idel_heating_protection_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
        m_cb_idel_heating_protection->SetForegroundColour(*wxBLACK);
        m_idel_protect_unavailable = false;
    }
}

wxBoxSizer* SafetyOptionsDialog::create_settings_group(wxWindow* parent)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    //Open Door Detection
    wxBoxSizer* line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_open_door = new CheckBox(parent);
    m_text_open_door = new Label(parent, _L("Open Door Detection"));
    m_text_open_door->SetFont(Label::Body_14);
    m_open_door_switch_board = new SwitchBoard(parent, _L("Notification"), _L("Pause printing"), wxSize(FromDIP(200), FromDIP(26)));
    m_open_door_switch_board->Disable();
    line_sizer->AddSpacer(FromDIP(5));
    line_sizer->Add(m_cb_open_door, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(m_text_open_door, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    sizer->Add(m_open_door_switch_board, 0, wxLEFT, FromDIP(65));
    line_sizer->Add(FromDIP(10), 0, 0, 0);
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    //Idel Heating Protect
    m_idel_heating_container = new wxPanel(parent, wxID_ANY);
    m_idel_heating_container->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* idel_container_sizer = new wxBoxSizer(wxVERTICAL);

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_idel_heating_protection = new CheckBox(m_idel_heating_container);
    m_text_idel_heating_protection = new Label(m_idel_heating_container, _L("Idel Heating Protection"));
    m_text_idel_heating_protection->SetFont(Label::Body_14);
    line_sizer->AddSpacer(FromDIP(5));
    line_sizer->Add(m_cb_idel_heating_protection, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(m_text_idel_heating_protection, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    idel_container_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_text_idel_heating_protection_caption = new Label(m_idel_heating_container, _L("Stops heating automatically after 5 mins of idle to ensure safety."));
    m_text_idel_heating_protection_caption->SetFont(Label::Body_12);
    m_text_idel_heating_protection_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->AddSpacer(FromDIP(20));
    line_sizer->Add(m_text_idel_heating_protection_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    idel_container_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

    m_idel_heating_container->SetSizer(idel_container_sizer);
    sizer->Add(m_idel_heating_container, 0, wxEXPAND);

    return sizer;
}

void SafetyOptionsDialog::update_machine_obj(MachineObject *obj_)
{
    obj = obj_;
}

bool SafetyOptionsDialog::Show(bool show)
{
    if (show) {
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();
    }
    return DPIDialog::Show(show);
}

void SafetyOptionsDialog::show_idel_heating_toast(const wxString &text)
{
    if (m_idel_heating_toast)
    {
        m_idel_heating_toast->Destroy();
        m_idel_heating_toast = nullptr;
    }

    m_idel_heating_toast = new wxPopupWindow(this);
    m_idel_heating_toast->SetBackgroundColour(wxColour(0, 0, 0));
    wxStaticText *textCtrl = new wxStaticText(m_idel_heating_toast, wxID_ANY, text, wxPoint(10, 10));
    textCtrl->SetForegroundColour(*wxWHITE);
    // Start a one-shot timer for 3 seconds to dismiss
    wxSize textSize = textCtrl->GetBestSize();
    m_idel_heating_toast->SetSize(textSize + wxSize(20, 20));

    wxRect  anchor = m_text_idel_heating_protection->GetScreenRect();
    wxPoint pos    = anchor.GetBottomLeft();
    pos.y += FromDIP(40);

    m_idel_heating_toast->Move(pos);
    m_idel_heating_toast->Show(true);

    m_idel_heating_toast_timer.Stop();
    m_idel_heating_toast_timer.StartOnce(3000);
}

}} // namespace Slic3r::GUI