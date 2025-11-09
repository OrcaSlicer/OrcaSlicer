#include "AMSSetting.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

namespace Slic3r { namespace GUI {

AMSSetting::AMSSetting(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, wxEmptyString, pos, size, style)
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}
AMSSetting::~AMSSetting() {}

void AMSSetting::create()
{
    wxBoxSizer *m_sizer_main;
    m_sizer_main = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(*wxWHITE);

    auto m_static_ams_settings = new wxStaticText(this, wxID_ANY, _L("AMS Settings"), wxDefaultPosition, wxDefaultSize, 0);
    m_static_ams_settings->SetFont(::Label::Head_14);
    m_static_ams_settings->SetForegroundColour(AMS_SETTING_GREY800);


    m_panel_body = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, -1), wxTAB_TRAVERSAL);
    m_panel_body->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizerl_body = new wxBoxSizer(wxVERTICAL);

    m_panel_Insert_material = new wxPanel(m_panel_body, wxID_ANY, wxDefaultPosition, wxSize(-1, -1), wxTAB_TRAVERSAL);
    m_panel_Insert_material->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* m_sizer_main_Insert_material = new wxBoxSizer(wxVERTICAL);

    // checkbox area 1
    wxBoxSizer *m_sizer_Insert_material  = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_Insert_material_auto_read = new ::CheckBox(m_panel_Insert_material);
    m_checkbox_Insert_material_auto_read->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_insert_material_read, this);
    m_sizer_Insert_material->Add(m_checkbox_Insert_material_auto_read, 0, wxTOP, 1);

    m_sizer_Insert_material->Add(0, 0, 0, wxLEFT, 12);

    m_title_Insert_material_auto_read = new wxStaticText(m_panel_Insert_material, wxID_ANY, _L("Insertion update"),
                                                         wxDefaultPosition, wxDefaultSize, 0);

    m_title_Insert_material_auto_read->SetFont(::Label::Head_13);
    m_title_Insert_material_auto_read->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_Insert_material_auto_read->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_Insert_material->Add(m_title_Insert_material_auto_read, 0, wxALL | wxEXPAND, 0);



    wxBoxSizer *m_sizer_Insert_material_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_Insert_material_tip_inline      = new wxBoxSizer(wxVERTICAL);

    m_sizer_Insert_material_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line1
    m_tip_Insert_material_line1 = new Label(m_panel_Insert_material,
        _L("The AMS will automatically read the filament information when inserting a new Bambu Lab filament. This takes about 20 seconds.")
    );
    m_tip_Insert_material_line1->SetFont(::Label::Body_13);
    m_tip_Insert_material_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_Insert_material_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_Insert_material_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_tip_Insert_material_line1->Hide();
    m_sizer_Insert_material_tip_inline->Add(m_tip_Insert_material_line1, 0, wxEXPAND, 0);

    // tip line2
    m_tip_Insert_material_line2 = new Label(m_panel_Insert_material,
        _L("Note: if a new filament is inserted during printing, the AMS will not automatically read any information until printing is completed.")
    );
    m_tip_Insert_material_line2->SetFont(::Label::Body_13);
    m_tip_Insert_material_line2->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_Insert_material_line2->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_Insert_material_line2->Wrap(AMS_SETTING_BODY_WIDTH);
    m_tip_Insert_material_line2->Hide();
    m_sizer_Insert_material_tip_inline->Add(m_tip_Insert_material_line2, 0, wxEXPAND | wxTOP, 8);

    // tip line3
    m_tip_Insert_material_line3 = new Label(m_panel_Insert_material,
        _L("When inserting a new filament, the AMS will not automatically read its information, leaving it blank for you to enter manually.")
    );
    m_tip_Insert_material_line3->SetFont(::Label::Body_13);
    m_tip_Insert_material_line3->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_Insert_material_line3->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_Insert_material_line3->Wrap(AMS_SETTING_BODY_WIDTH);
    m_tip_Insert_material_line3->Hide();
    m_sizer_Insert_material_tip_inline->Add(m_tip_Insert_material_line3, 0, wxEXPAND, 0);

    m_sizer_Insert_material_tip->Add(m_sizer_Insert_material_tip_inline, 1, wxALIGN_CENTER, 0);

    m_sizer_main_Insert_material->Add(m_sizer_Insert_material, 0, wxEXPAND | wxTOP, FromDIP(4));
    m_sizer_main_Insert_material->Add(m_sizer_Insert_material_tip, 0, wxEXPAND | wxLEFT | wxTOP, 18);
    m_panel_Insert_material->SetSizer(m_sizer_main_Insert_material);

    // checkbox area 2
    wxBoxSizer *m_sizer_starting = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_starting_auto_read = new ::CheckBox(m_panel_body);
    m_checkbox_starting_auto_read->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_starting_read, this);
    m_sizer_starting->Add(m_checkbox_starting_auto_read, 0, wxTOP, 1);
    m_sizer_starting->Add(0, 0, 0, wxLEFT, 12);
    m_title_starting_auto_read = new wxStaticText(m_panel_body, wxID_ANY, _L("Power on update"), wxDefaultPosition,wxDefaultSize, 0);
    m_title_starting_auto_read->SetFont(::Label::Head_13);
    m_title_starting_auto_read->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_starting_auto_read->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_starting->Add(m_title_starting_auto_read, 1, wxEXPAND, 0);



    wxBoxSizer *m_sizer_starting_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_starting_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    m_sizer_starting_tip_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_starting_line1 = new Label(m_panel_body,
        _L("The AMS will automatically read the information of inserted filament on start-up. It will take about 1 minute. The reading process will roll the filament spools.")
    );
    m_tip_starting_line1->SetFont(::Label::Body_13);
    m_tip_starting_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_starting_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_starting_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_starting_tip_inline->Add(m_tip_starting_line1, 0, wxEXPAND, 0);

    m_tip_starting_line2 = new Label(m_panel_body,
        _L("The AMS will not automatically read information from inserted filament during startup and will continue to use the information recorded before the last shutdown.")
    );
    m_tip_starting_line2->SetFont(::Label::Body_13);
    m_tip_starting_line2->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_starting_line2->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_starting_line2->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_starting_tip_inline->Add(m_tip_starting_line2, 0, wxEXPAND,0);
    m_sizer_starting_tip->Add(m_sizer_starting_tip_inline, 1, wxALIGN_CENTER, 0);

    // checkbox area 3
    wxBoxSizer* m_sizer_remain = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_remain = new ::CheckBox(m_panel_body);
    m_checkbox_remain->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_remain, this);
    m_sizer_remain->Add(m_checkbox_remain, 0, wxTOP, 1);
    m_sizer_remain->Add(0, 0, 0, wxLEFT, 12);
    m_title_remain = new wxStaticText(m_panel_body, wxID_ANY, _L("Update remaining capacity"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_remain->SetFont(::Label::Head_13);
    m_title_remain->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_remain->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_remain->Add(m_title_remain, 1, wxEXPAND, 0);



    wxBoxSizer* m_sizer_remain_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_remain_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    m_sizer_remain_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_remain_line1 = new Label(m_panel_body, _L("AMS will attempt to estimate the remaining capacity of the Bambu Lab filaments."));
    m_tip_remain_line1->SetFont(::Label::Body_13);
    m_tip_remain_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_remain_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_remain_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_remain_inline->Add(m_tip_remain_line1, 0, wxEXPAND, 0);
    m_sizer_remain_tip->Add(m_sizer_remain_inline, 1, wxALIGN_CENTER, 0);

    // checkbox area 4
    wxBoxSizer* m_sizer_switch_filament = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_switch_filament = new ::CheckBox(m_panel_body);
    m_checkbox_switch_filament->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_switch_filament, this);
    m_sizer_switch_filament->Add(m_checkbox_switch_filament, 0, wxTOP, 1);
    m_sizer_switch_filament->Add(0, 0, 0, wxLEFT, 12);
    m_title_switch_filament = new wxStaticText(m_panel_body, wxID_ANY, _L("AMS filament backup"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_switch_filament->SetFont(::Label::Head_13);
    m_title_switch_filament->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_switch_filament->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_switch_filament->Add(m_title_switch_filament, 1, wxEXPAND, 0);



    wxBoxSizer* m_sizer_switch_filament_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_switch_filament_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    m_sizer_switch_filament_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_switch_filament_line1 = new Label(m_panel_body,
        _L("AMS will continue to another spool with matching filament properties automatically when current filament runs out.")
    );
    m_tip_switch_filament_line1->SetFont(::Label::Body_13);
    m_tip_switch_filament_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_switch_filament_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_switch_filament_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_switch_filament_inline->Add(m_tip_switch_filament_line1, 0, wxEXPAND, 0);
    m_sizer_switch_filament_tip->Add(m_sizer_switch_filament_inline, 1, wxALIGN_CENTER, 0);



    // checkbox area 5
    wxBoxSizer* m_sizer_air_print = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_air_print = new ::CheckBox(m_panel_body);
    m_checkbox_air_print->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_air_print_detect, this);
    m_sizer_air_print->Add(m_checkbox_air_print, 0, wxTOP, 1);
    m_sizer_air_print->Add(0, 0, 0, wxLEFT, 12);
    m_title_air_print = new wxStaticText(m_panel_body, wxID_ANY, _L("Air Printing Detection"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_air_print->SetFont(::Label::Head_13);
    m_title_air_print->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_air_print->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_air_print->Add(m_title_air_print, 1, wxEXPAND, 0);

    wxBoxSizer* m_sizer_air_print_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_air_print_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    auto m_sizer_air_print_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_air_print_line = new Label(m_panel_body,
        _L("Detects clogging and filament grinding, halting printing immediately to conserve time and filament.")
    );
    m_tip_air_print_line->SetFont(::Label::Body_13);
    m_tip_air_print_line->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_air_print_line->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_air_print_line->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_air_print_inline->Add(m_tip_air_print_line, 0, wxEXPAND, 0);
    m_sizer_air_print_tip->Add(m_sizer_air_print_inline, 1, wxALIGN_CENTER, 0);

    m_checkbox_air_print->Hide();
    m_title_air_print->Hide();
    m_tip_air_print_line->Hide();


    // panel img
    wxPanel* m_panel_img = new wxPanel(m_panel_body, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_panel_img->SetBackgroundColour(AMS_SETTING_GREY200);
    wxBoxSizer *m_sizer_img = new wxBoxSizer(wxVERTICAL);
    m_am_img = new wxStaticBitmap(m_panel_img, wxID_ANY, create_scaled_bitmap("ams_icon", nullptr, 126), wxDefaultPosition, wxDefaultSize);
    m_sizer_img->Add(m_am_img, 0, wxALIGN_CENTER | wxTOP, 26);
    m_sizer_img->Add(0, 0, 0, wxTOP, 18);
    m_panel_img->SetSizer(m_sizer_img);
    m_panel_img->Layout();
    m_sizer_img->Fit(m_panel_img);

    m_sizer_remain_block = new wxBoxSizer(wxVERTICAL);
    m_sizer_remain_block->Add(m_sizer_remain, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_sizer_remain_block->Add(0, 0, 0, wxTOP, 8);
    m_sizer_remain_block->Add(m_sizer_remain_tip, 0, wxLEFT, 18);
    m_sizer_remain_block->Add(0, 0, 0, wxTOP, 15);

    m_sizerl_body->Add(m_panel_Insert_material, 0, 0, 0);
    m_sizerl_body->Add(m_sizer_starting, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_sizerl_body->Add(0, 0, 0, wxTOP, 8);
    m_sizerl_body->Add(m_sizer_starting_tip, 0, wxLEFT, 18);
    m_sizerl_body->Add(0, 0, 0, wxTOP, 15);
    m_sizerl_body->Add(m_sizer_remain_block, 0, wxEXPAND, 0);
    m_sizerl_body->Add(m_sizer_switch_filament, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_sizerl_body->Add(0, 0, 0, wxTOP, 8);
    m_sizerl_body->Add(m_sizer_switch_filament_tip, 0, wxLEFT, 18);
    m_sizerl_body->Add(0, 0, 0, wxTOP, 6);
    m_sizerl_body->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_sizerl_body->Add(m_sizer_air_print, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_sizerl_body->Add(0, 0, 0, wxTOP, 8);
    m_sizerl_body->Add(m_sizer_air_print_tip, 0, wxLEFT, 18);
    m_sizerl_body->Add(0, 0, 0, wxTOP, 6);
    m_sizerl_body->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_sizerl_body->Add(m_panel_img, 1, wxEXPAND | wxALL, FromDIP(5));

    m_panel_body->SetSizer(m_sizerl_body);
    m_panel_body->Layout();
    m_sizerl_body->Fit(m_panel_body);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_sizer_main->Add(m_static_ams_settings, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_sizer_main->Add(m_panel_body, 1, wxBottom | wxLEFT | wxRIGHT | wxEXPAND, FromDIP(24));

    this->SetSizer(m_sizer_main);
    this->Layout();
    m_sizer_main->Fit(this);

    this->Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);

    Bind(wxEVT_SHOW, [this](auto& e) {
        if (this->IsShown()) {
            if (ams_support_remain) {
                m_sizer_remain_block->Show(true);
            }
            else {
                m_sizer_remain_block->Show(false);
            }
        }
    });
}

void AMSSetting::update_insert_material_read_mode(bool selected, std::string version)
{
    if (!version.empty() && version >= AMS_F1_SUPPORT_INSERTION_UPDATE_DEFAULT) {
        m_checkbox_Insert_material_auto_read->SetValue(true);
        m_checkbox_Insert_material_auto_read->Hide();
        m_title_Insert_material_auto_read->Hide();
        m_tip_Insert_material_line1->Hide();
        m_tip_Insert_material_line2->Hide();
        m_tip_Insert_material_line3->Hide();
        m_panel_Insert_material->Hide();
    }
    else {
        m_panel_Insert_material->Show();
        m_checkbox_Insert_material_auto_read->SetValue(selected);
        m_checkbox_Insert_material_auto_read->Show();
        m_title_Insert_material_auto_read->Show();
        if (selected) {
            m_tip_Insert_material_line1->Show();
            m_tip_Insert_material_line2->Show();
            m_tip_Insert_material_line3->Hide();
        }
        else {
            m_tip_Insert_material_line1->Hide();
            m_tip_Insert_material_line2->Hide();
            m_tip_Insert_material_line3->Show();
        }
    }
    m_panel_Insert_material->Layout();
    m_sizer_Insert_material_tip_inline->Layout();
    Layout();
    Fit();
}

void AMSSetting::update_ams_img(std::string ams_icon_str)
{
    if (wxGetApp().dark_mode()&& ams_icon_str=="extra_icon") {
        ams_icon_str += "_dark";
    }
    m_am_img->SetBitmap(create_scaled_bitmap(ams_icon_str, nullptr, 126));
}

void AMSSetting::update_starting_read_mode(bool selected)
{
    m_checkbox_starting_auto_read->SetValue(selected);
    if (selected) { // selected
        m_tip_starting_line1->Show();
        m_tip_starting_line2->Hide();
    } else { // unselected
        m_tip_starting_line1->Hide();
        m_tip_starting_line2->Show();
    }
    m_sizer_starting_tip_inline->Layout();
    Layout();
    Fit();
}

void AMSSetting::update_remain_mode(bool selected)
{
    if (obj->is_support_update_remain) {
        m_checkbox_remain->Show();
        m_title_remain->Show();
        m_tip_remain_line1->Show();
        Layout();
    }
    else {
        m_checkbox_remain->Hide();
        m_title_remain->Hide();
        m_tip_remain_line1->Hide();
        Layout();
    }
    m_checkbox_remain->SetValue(selected);
}

void AMSSetting::update_switch_filament(bool selected)
{
    if (obj->is_support_filament_backup) {
        m_checkbox_switch_filament->Show();
        m_title_switch_filament->Show();
        m_tip_switch_filament_line1->Show();
        Layout();
    } else {
        m_checkbox_switch_filament->Hide();
        m_title_switch_filament->Hide();
        m_tip_switch_filament_line1->Hide();
        Layout();
    }
    m_checkbox_switch_filament->SetValue(selected);
}

void AMSSetting::update_air_printing_detection(bool selected)
{
    if (false/*obj->is_support_air_print_detection*/) {
        m_checkbox_air_print->Show();
        m_title_air_print->Show();
        m_tip_air_print_line->Show();
    }
    else {
        m_checkbox_air_print->Hide();
        m_title_air_print->Hide();
        m_tip_air_print_line->Hide();
    }
    Layout();
    m_checkbox_air_print->SetValue(selected);
}


void AMSSetting::on_select_ok(wxMouseEvent &event)
{
    if (obj) {
        obj->command_ams_calibrate(ams_id);
    }
}

void AMSSetting::on_insert_material_read(wxCommandEvent &event)
{
    // send command
    if (m_checkbox_Insert_material_auto_read->GetValue()) {
        // checked
        m_tip_Insert_material_line1->Show();
        m_tip_Insert_material_line2->Show();
        m_tip_Insert_material_line3->Hide();
    } else {
        // unchecked
        m_tip_Insert_material_line1->Hide();
        m_tip_Insert_material_line2->Hide();
        m_tip_Insert_material_line3->Show();
    }
    m_checkbox_Insert_material_auto_read->SetValue(event.GetInt());

    bool start_read_opt = m_checkbox_starting_auto_read->GetValue();
    bool tray_read_opt = m_checkbox_Insert_material_auto_read->GetValue();
    bool remain_opt = m_checkbox_remain->GetValue();

    obj->command_ams_user_settings(ams_id, start_read_opt, tray_read_opt, remain_opt);

    m_sizer_Insert_material_tip_inline->Layout();
    Layout();
    Fit();

    event.Skip();
}

void AMSSetting::on_starting_read(wxCommandEvent &event)
{
    if (m_checkbox_starting_auto_read->GetValue()) {
        // checked
        m_tip_starting_line1->Show();
        m_tip_starting_line2->Hide();
    } else {
        // unchecked
        m_tip_starting_line1->Hide();
        m_tip_starting_line2->Show();
    }
    m_checkbox_starting_auto_read->SetValue(event.GetInt());

    bool start_read_opt = m_checkbox_starting_auto_read->GetValue();
    bool tray_read_opt  = m_checkbox_Insert_material_auto_read->GetValue();
    bool remain_opt = m_checkbox_remain->GetValue();

    obj->command_ams_user_settings(ams_id, start_read_opt, tray_read_opt, remain_opt);

    m_sizer_starting_tip_inline->Layout();
    Layout();
    Fit();

    event.Skip();
}

void AMSSetting::on_remain(wxCommandEvent& event)
{
    bool start_read_opt = m_checkbox_starting_auto_read->GetValue();
    bool tray_read_opt = m_checkbox_Insert_material_auto_read->GetValue();
    bool remain_opt = m_checkbox_remain->GetValue();
    obj->command_ams_user_settings(ams_id, start_read_opt, tray_read_opt, remain_opt);
    event.Skip();
}

void AMSSetting::on_switch_filament(wxCommandEvent& event)
{
    bool switch_filament = m_checkbox_switch_filament->GetValue();
    obj->command_ams_switch_filament(switch_filament);
    event.Skip();
}

void AMSSetting::on_air_print_detect(wxCommandEvent& event)
{
    bool air_print_detect = m_checkbox_air_print->GetValue();
    obj->command_ams_air_print_detect(air_print_detect);
    event.Skip();
}

wxString AMSSetting::append_title(wxString text)
{
    wxString lab;
    auto *   widget = new wxStaticText(m_panel_body, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    widget->SetForegroundColour(*wxBLACK);
    widget->Wrap(AMS_SETTING_BODY_WIDTH);
    widget->SetMinSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    lab = widget->GetLabel();
    widget->Destroy();
    return lab;
}

wxStaticText *AMSSetting::append_text(wxString text)
{
    auto *widget = new wxStaticText(m_panel_body, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    widget->Wrap(250);
    widget->SetMinSize(wxSize(250, -1));
    return widget;
}

void AMSSetting::on_dpi_changed(const wxRect &suggested_rect)
{
    //m_button_auto_demarcate->SetMinSize(AMS_SETTING_BUTTON_SIZE);
}

}} // namespace Slic3r::GUI