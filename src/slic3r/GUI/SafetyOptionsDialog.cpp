#include "SafetyOptionsDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include "Widgets/SwitchButton.hpp"
#include "MsgDialog.hpp"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevNozzleSystem.h"

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
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
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

    open_door_switch_board->Bind(wxCUSTOMEVT_SWITCH_POS, [this](wxCommandEvent &evt)
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

    UpdateOptionOpenDoorCheck(obj_);

    this->Freeze();
    this->Thaw();
    Layout();
}

void SafetyOptionsDialog::UpdateOptionOpenDoorCheck(MachineObject *obj) {
    if (!obj || !obj->support_door_open_check()) {
        m_cb_open_door->Hide();
        text_open_door->Hide();
        open_door_switch_board->Hide();
        return;
    }

    if (obj->get_door_open_check_state() != MachineObject::DOOR_OPEN_CHECK_DISABLE) {
        m_cb_open_door->SetValue(true);
        open_door_switch_board->Enable();

        if (obj->get_door_open_check_state() == MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING) {
            open_door_switch_board->updateState("left");
            open_door_switch_board->Refresh();
        } else if (obj->get_door_open_check_state() == MachineObject::DOOR_OPEN_CHECK_ENABLE_PAUSE_PRINT) {
            open_door_switch_board->updateState("right");
            open_door_switch_board->Refresh();
        }

    } else {
        m_cb_open_door->SetValue(false);
        open_door_switch_board->Disable();
    }

    m_cb_open_door->Show();
    text_open_door->Show();
    open_door_switch_board->Show();
}

wxBoxSizer* SafetyOptionsDialog::create_settings_group(wxWindow* parent)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    //Open Door Detection
    wxBoxSizer* line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_open_door = new CheckBox(parent);
    text_open_door = new Label(parent, _L("Open Door Detection"));
    text_open_door->SetFont(Label::Body_14);
    open_door_switch_board = new SwitchBoard(parent, _L("Notification"), _L("Pause printing"), wxSize(FromDIP(200), FromDIP(26)));
    open_door_switch_board->Disable();
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_open_door, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_open_door, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    sizer->Add(open_door_switch_board, 0, wxLEFT, FromDIP(58));
    line_sizer->Add(FromDIP(10), 0, 0, 0);
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

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

}} // namespace Slic3r::GUI