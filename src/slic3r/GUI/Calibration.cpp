#include "Calibration.hpp"
#include "I18N.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/Thread.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Preview.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/StaticBox.hpp"

#include "DeviceCore/DevConfig.h"

static wxColour FG_COLOR = wxColour(0x32, 0x3A, 0x3D);
static wxColour BG_COLOR = wxColour(0xF8, 0xF8, 0xF8);

#define CALI_FLOW_CONTENT_WIDTH  FromDIP(200)

namespace Slic3r { namespace GUI {

CalibrationDialog::CalibrationDialog(Plater *plater)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Calibration"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    this->SetDoubleBuffered(true);

    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

    wxBoxSizer *sizer_body = new wxBoxSizer(wxHORIZONTAL);
    auto        body_panel = new wxPanel(this, wxID_ANY);

    body_panel->SetBackgroundColour(*wxWHITE);
    auto cali_left_panel = new StaticBox(body_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(303), -1));
    cali_left_panel->SetBackgroundColor(BG_COLOR);
    cali_left_panel->SetBorderColor(BG_COLOR);

    wxBoxSizer *cali_left_sizer = new wxBoxSizer(wxVERTICAL);
    cali_left_sizer->Add(0, 0, 0, wxTOP, FromDIP(25));

    // calibration step selection
    auto cali_step_select_title = new wxStaticText(cali_left_panel, wxID_ANY, _L("Calibration step selection"), wxDefaultPosition, wxDefaultSize, 0);
    cali_step_select_title->SetFont(::Label::Head_14);
    cali_step_select_title->Wrap(-1);
    cali_step_select_title->SetForegroundColour(FG_COLOR);
    cali_step_select_title->SetBackgroundColour(BG_COLOR);
    cali_left_sizer->Add(cali_step_select_title, 0, wxLEFT, FromDIP(15));

    select_xcam_cali    = create_check_option(_L("Micro lidar calibration"), cali_left_panel, _L("Micro lidar calibration"), "xcam_cali");
    select_bed_leveling = create_check_option(_L("Bed leveling"),            cali_left_panel, _L("Bed leveling"),                       "bed_leveling");
    select_vibration    = create_check_option(_L("Vibration compensation"), cali_left_panel, _L("Vibration compensation"), "vibration");
    select_motor_noise  = create_check_option(_L("Motor noise cancellation"), cali_left_panel, _L("Motor noise cancellation"), "motor_noise");
    select_nozzle_cali  = create_check_option(_L("Nozzle offset calibration"), cali_left_panel, _L("Nozzle offset calibration"), "nozzle_cali");
    select_heatbed_cali  = create_check_option(_L("High-temperature Heatbed Calibration"), cali_left_panel, _L("High-temperature Heatbed Calibration"), "bed_cali");
    select_clumppos_cali = create_check_option(_L("Nozzle clumping detection Calibration"), cali_left_panel, _L("Nozzle clumping detection Calibration"), "clump_pos_cali");

    // STUDIO-10091 the default not checked option
    if(m_checkbox_list.count("bed_cali") != 0)
    {
        m_checkbox_list["bed_cali"]->SetValue(false);
    }

    cali_left_sizer->Add(0, FromDIP(18), 0, wxEXPAND, 0);
    cali_left_sizer->Add(select_xcam_cali, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(select_bed_leveling, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(select_vibration, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(select_motor_noise, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(select_nozzle_cali, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(select_heatbed_cali, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(select_clumppos_cali, 0, wxLEFT, FromDIP(15));
    cali_left_sizer->Add(0, FromDIP(30), 0, wxEXPAND, 0);

    auto cali_left_text_top = new wxStaticText(cali_left_panel, wxID_ANY, _L("Calibration program"), wxDefaultPosition, wxDefaultSize, 0);
    cali_left_text_top->SetFont(::Label::Head_14);
    cali_left_text_top->Wrap(-1);
    cali_left_text_top->SetForegroundColour(FG_COLOR);
    cali_left_text_top->SetBackgroundColour(BG_COLOR);

    cali_left_sizer->Add(cali_left_text_top, 0, wxLEFT, FromDIP(15));

    cali_left_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    auto cali_left_text_body =
        new Label(cali_left_panel, _L("The calibration program detects the status of your device automatically to minimize deviation.\nIt keeps the device performing optimally."));
    cali_left_text_body->Wrap(FromDIP(260));
    cali_left_text_body->SetForegroundColour(wxColour(0x6B, 0x6B, 0x6B));
    cali_left_text_body->SetBackgroundColour(BG_COLOR);
    cali_left_text_body->SetFont(::Label::Body_13);
    cali_left_sizer->Add(cali_left_text_body, 0, wxLEFT, FromDIP(15));

    cali_left_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));

   /* auto cali_left_text_top_prepar = new wxStaticText(cali_left_panel, wxID_ANY, _L("Preparation before calibration"), wxDefaultPosition, wxDefaultSize, 0);
     cali_left_text_top_prepar->SetFont(::Label::Head_14);
     cali_left_text_top_prepar->SetForegroundColour(wxColour(0x32, 0x3A, 0x3D));
     cali_left_text_top_prepar->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));
     cali_left_text_top_prepar->Wrap(-1);
     cali_left_sizer->Add(cali_left_text_top_prepar, 0, wxLEFT, FromDIP(15));

     cali_left_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

     auto cali_left_text_body_prepar =
         new wxStaticText(cali_left_panel, wxID_ANY,
                          _L("Before calibration, please make sure a filament is loaded and its nozzle temperature and bed temperature is set in Feeding lab."),
     wxDefaultPosition, wxSize(FromDIP(260), -1), 0); cali_left_text_body_prepar->Wrap(FromDIP(260)); cali_left_text_body_prepar->SetFont(::Label::Body_13);
     cali_left_text_body_prepar->SetForegroundColour(wxColour(0x6B, 0x6B, 0x6B));
     cali_left_text_body_prepar->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));
     cali_left_sizer->Add(cali_left_text_body_prepar, 0, wxLEFT, FromDIP(15));*/

    cali_left_panel->SetSizer(cali_left_sizer);
    cali_left_panel->Layout();
    sizer_body->Add(cali_left_panel, 0, wxALIGN_CENTER, 0);

    sizer_body->Add(0, 0, 0, wxLEFT, FromDIP(8));

    wxBoxSizer *cali_right_sizer_h = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *cali_right_sizer_v = new wxBoxSizer(wxVERTICAL);

    auto cali_right_panel = new StaticBox(body_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(182), FromDIP(200)));
    cali_right_panel->SetBackgroundColor(BG_COLOR);
    cali_right_panel->SetBorderColor(BG_COLOR);

    auto cali_text_right_top = new wxStaticText(cali_right_panel, wxID_ANY, _L("Calibration Flow"), wxDefaultPosition, wxDefaultSize, 0);
    cali_text_right_top->Wrap(-1);
    cali_text_right_top->SetFont(::Label::Head_14);
    cali_text_right_top->SetForegroundColour(AMS_CONTROL_BRAND_COLOUR);
    cali_text_right_top->SetBackgroundColour(BG_COLOR);

    auto staticline = new ::StaticLine(cali_right_panel);
    staticline->SetLineColour(AMS_CONTROL_BRAND_COLOUR);
    auto calibration_sizer = new wxBoxSizer(wxVERTICAL);

    m_calibration_flow = new StepIndicator(cali_right_panel, wxID_ANY);
    StateColor bg_color(std::pair<wxColour, int>(BG_COLOR, StateColor::Normal));
    m_calibration_flow->SetBackgroundColor(bg_color);
    m_calibration_flow->SetFont(Label::Body_12);
    m_calibration_flow->SetMinSize(wxSize(CALI_FLOW_CONTENT_WIDTH, FromDIP(160)));
    m_calibration_flow->SetSize(wxSize(CALI_FLOW_CONTENT_WIDTH, FromDIP(160)));

    m_calibration_btn = new Button(cali_right_panel, _L("Start Calibration"));
    m_calibration_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

    cali_right_sizer_v->Add(cali_text_right_top, 0, wxALIGN_CENTER, 0);
    cali_right_sizer_v->Add(0, 0, 0, wxTOP, FromDIP(7));
    cali_right_sizer_v->Add(staticline, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    cali_right_sizer_v->Add(0, 0, 0, wxTOP, FromDIP(3));
    cali_right_sizer_v->Add(m_calibration_flow, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(6));
    cali_right_sizer_v->Add(0, 0, 1, wxEXPAND, 5);
    cali_right_sizer_v->Add(m_calibration_btn, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    cali_right_sizer_h->Add(cali_right_sizer_v, 0, wxALIGN_CENTER, 0);
    cali_right_panel->SetSizer(cali_right_sizer_h);
    cali_right_panel->Layout();

    sizer_body->Add(cali_right_panel, 0, wxEXPAND, 0);

    body_panel->SetSizer(sizer_body);
    body_panel->Layout();

    m_sizer_main->Add(body_panel, 0, wxEXPAND | wxALL, FromDIP(25));
    SetSizer(m_sizer_main);
    Layout();
    Fit();

    m_calibration_btn->Bind(wxEVT_LEFT_DOWN, &CalibrationDialog::on_start_calibration, this);
}

CalibrationDialog::~CalibrationDialog() {}

void CalibrationDialog::on_dpi_changed(const wxRect &suggested_rect) {}

wxWindow* CalibrationDialog::create_check_option(wxString title, wxWindow* parent, wxString tooltip, std::string param)
{
    auto checkbox = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    checkbox->SetBackgroundColour(BG_COLOR);

    wxBoxSizer* sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* sizer_check = new wxBoxSizer(wxVERTICAL);

    auto check = new ::CheckBox(checkbox);

    sizer_check->Add(check, 0, wxBOTTOM | wxEXPAND | wxTOP, FromDIP(5));

    sizer_checkbox->Add(sizer_check, 0, wxEXPAND, FromDIP(5));
    sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(11));

    auto text = new wxStaticText(checkbox, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    text->SetFont(::Label::Body_13);
    text->SetForegroundColour(wxColour(107, 107, 107));
    text->Wrap(-1);
    sizer_checkbox->Add(text, 0, wxBOTTOM | wxEXPAND | wxTOP, FromDIP(5));

    checkbox->SetSizer(sizer_checkbox);
    checkbox->Layout();
    sizer_checkbox->Fit(checkbox);

    checkbox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);

    text->Bind(wxEVT_LEFT_DOWN, [this, check](wxMouseEvent&) { check->SetValue(check->GetValue() ? false : true); });
    m_checkbox_list[param] = check;
    m_checkbox_list[param]->SetValue(true);
    return checkbox;
}

void CalibrationDialog::update_cali(MachineObject *obj)
{
    if (!obj) return;
    if (obj->GetConfig()->SupportAIMonitor() && obj->GetConfig()->SupportCalibrationLidar())
    {
        select_xcam_cali->Show();
    } else {
        select_xcam_cali->Hide();
        m_checkbox_list["xcam_cali"]->SetValue(false);
    }

    if(obj->is_support_bed_leveling != 0){
        select_bed_leveling->Show();
    }else{
        select_bed_leveling->Hide();
        m_checkbox_list["bed_leveling"]->SetValue(false);
    }

    if (obj->is_support_motor_noise_cali) {
        select_motor_noise->Show();
    } else {
        select_motor_noise->Hide();
        m_checkbox_list["motor_noise"]->SetValue(false);
    }

    if (obj->GetConfig()->SupportCalibrationNozzleOffset()) {
        select_nozzle_cali->Show();
    } else {
        select_nozzle_cali->Hide();
        m_checkbox_list["nozzle_cali"]->SetValue(false);
    }

    if (obj->GetConfig()->SupportCalibrationHighTempBed()) {
        select_heatbed_cali->Show();
    } else {
        select_heatbed_cali->Hide();
        m_checkbox_list["bed_cali"]->SetValue(false);
    }

    if (obj->GetConfig()->SupportCaliClumpPos()) {
        select_clumppos_cali->Show();
    } else {
        select_clumppos_cali->Hide();
        m_checkbox_list["clump_pos_cali"]->SetValue(false);
    }

    if (obj->is_calibration_running() || obj->is_calibration_done()) {
        if (obj->is_calibration_done()) {
            m_calibration_btn->Enable();
            m_calibration_btn->SetLabel(_L("Completed"));

        } else {
            // RUNNING && IDLE
            m_calibration_btn->Disable();
            m_calibration_btn->SetLabel(_L("Calibrating"));

        }
        auto size = wxSize(CALI_FLOW_CONTENT_WIDTH, obj->stage_list_info.size() * FromDIP(35));
        if (m_calibration_flow->GetSize().y != size.y) {
            m_calibration_flow->SetSize(size);
            m_calibration_flow->SetMinSize(size);
            m_calibration_flow->SetMaxSize(size);
            m_calibration_flow->Refresh();

            Layout();

        }
        if (is_stage_list_info_changed(obj)) {
            // change items if stage_list_info changed
            m_calibration_flow->DeleteAllItems();
            for (int i = 0; i < obj->stage_list_info.size(); i++) {
                m_calibration_flow->AppendItem(Slic3r::get_stage_string(obj->stage_list_info[i]));
            }

            last_stage_list_info = obj->stage_list_info;
        }
        int index = obj->get_curr_stage_idx();
        m_calibration_flow->SelectItem(index);
    } else {
        // IDLE
        if (obj->is_in_printing()) {
            m_calibration_btn->Disable();
        }
        else {
            m_calibration_btn->Enable();
        }
        m_calibration_flow->DeleteAllItems();
        m_calibration_btn->SetLabel(_L("Start Calibration"));
        if (!m_checkbox_list["vibration"]->GetValue() && !m_checkbox_list["bed_leveling"]->GetValue() &&
            !m_checkbox_list["xcam_cali"]->GetValue() && !m_checkbox_list["motor_noise"]->GetValue() &&
            !m_checkbox_list["nozzle_cali"]->GetValue() && !m_checkbox_list["bed_cali"]->GetValue())
        {
            m_calibration_btn->Disable();
            m_calibration_btn->SetLabel(_L("No step selected"));
        }
        else {
            m_calibration_btn->Enable();
        }
    }
}

bool CalibrationDialog::is_stage_list_info_changed(MachineObject *obj)
{
    if (!obj) return true;

    if (last_stage_list_info.size() != obj->stage_list_info.size()) return true;

    for (int i = 0; i < last_stage_list_info.size(); i++) {
        if (last_stage_list_info[i] != obj->stage_list_info[i]) return true;
    }
    last_stage_list_info = obj->stage_list_info;
    return false;
}

void CalibrationDialog::on_start_calibration(wxMouseEvent &event)
{
    if (m_obj) {
        if (m_obj->is_calibration_done()) {
            m_obj->calibration_done = false;
            EndModal(wxID_CANCEL);
            Close();
        } else {
            BOOST_LOG_TRIVIAL(info) << "on_start_calibration";
            m_obj->command_start_calibration(
                m_checkbox_list["vibration"]->GetValue(),
                m_checkbox_list["bed_leveling"]->GetValue(),
                m_checkbox_list["xcam_cali"]->GetValue(),
                m_checkbox_list["motor_noise"]->GetValue(),
                m_checkbox_list["nozzle_cali"]->GetValue(),
                m_checkbox_list["bed_cali"]->GetValue(),
                m_checkbox_list["clump_pos_cali"]->GetValue()
                );
        }
    }
}

void CalibrationDialog::update_machine_obj(MachineObject *obj) { m_obj = obj; }

bool CalibrationDialog::Show(bool show)
{
    if (show) {
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();
    }
    return DPIDialog::Show(show);
}

}} // namespace Slic3r::GUI
