#include "ExtrusionCalibration.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/Preset.hpp"
#include "I18N.hpp"
#include <boost/log/trivial.hpp>
#include <wx/dcgraph.h>
#include "CalibUtils.hpp"

namespace Slic3r { namespace GUI {


ExtrusionCalibration::ExtrusionCalibration(wxWindow *parent, wxWindowID id)
    : DPIDialog(parent, id, _L("Dynamic flow calibration"), wxDefaultPosition, wxDefaultSize, (wxSYSTEM_MENU |
        wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX | wxCAPTION |wxCLIP_CHILDREN))
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void ExtrusionCalibration::init_bitmaps()
{
    auto lan = wxGetApp().app_config->get_language_code();
    if (lan == "zh-cn") {
        m_is_zh = true;
        m_calibration_tips_bmp_zh = create_scaled_bitmap("extrusion_calibration_tips_zh", nullptr, 256);
    }
    else{
        m_is_zh = false;
        m_calibration_tips_bmp_en = create_scaled_bitmap("extrusion_calibration_tips_en", nullptr, 256);
    }
    m_calibration_tips_open_btn_bmp = create_scaled_bitmap("extrusion_calibrati_open_button", nullptr, 16);
}

void ExtrusionCalibration::create()
{
    init_bitmaps();
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer* sizer_main = new wxBoxSizer(wxVERTICAL);
    m_step_1_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_step_1_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* step_1_sizer = new wxBoxSizer(wxVERTICAL);

    m_step_1_panel->SetSizer(step_1_sizer);

    step_1_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    // filament title
    wxString intro_text = _L("The nozzle temp and max volumetric speed will affect the calibration results. Please fill in the same values as the actual printing. They can be auto-filled by selecting a filament preset.");
    m_filament_preset_title = new Label(m_step_1_panel, intro_text);
    m_filament_preset_title->SetFont(Label::Body_12);
    m_filament_preset_title->SetForegroundColour(EXTRUSION_CALIBRATION_GREY800);
    m_filament_preset_title->Wrap(this->GetSize().x);
    step_1_sizer->Add(m_filament_preset_title, 0, wxEXPAND);

    step_1_sizer->AddSpacer(FromDIP(12));

    auto select_sizer = new wxBoxSizer(wxVERTICAL);

    auto nozzle_dia_sel_text = new wxStaticText(m_step_1_panel, wxID_ANY, _L("Nozzle Diameter"), wxDefaultPosition, wxDefaultSize, 0);
    select_sizer->Add(nozzle_dia_sel_text, 0, wxALIGN_LEFT);
    select_sizer->AddSpacer(FromDIP(4));


#ifdef __APPLE__
    m_comboBox_nozzle_dia = new wxComboBox(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, EXTRUSION_CALIBRATION_BED_COMBOX, 0, nullptr, wxCB_READONLY);
#else
    m_comboBox_nozzle_dia = new ComboBox(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, EXTRUSION_CALIBRATION_BED_COMBOX, 0, nullptr, wxCB_READONLY);
#endif
    m_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f", 0.2));
    m_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f", 0.4));
    m_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f", 0.6));
    m_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f", 0.8));

    select_sizer->Add(m_comboBox_nozzle_dia, 0, wxEXPAND);
    select_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    auto filament_sel_text = new wxStaticText(m_step_1_panel, wxID_ANY, _L("Filament"), wxDefaultPosition, wxDefaultSize, 0);
    select_sizer->Add(filament_sel_text, 0, wxALIGN_LEFT);
    select_sizer->AddSpacer(FromDIP(4));
#ifdef __APPLE__
    m_comboBox_filament = new wxComboBox(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, EXTRUSION_CALIBRATION_BED_COMBOX, 0, nullptr, wxCB_READONLY);
#else
    m_comboBox_filament = new ComboBox(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, EXTRUSION_CALIBRATION_BED_COMBOX, 0, nullptr, wxCB_READONLY);
#endif
    select_sizer->Add(m_comboBox_filament, 0, wxEXPAND);
    select_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    auto bed_type_sel_text = new wxStaticText(m_step_1_panel, wxID_ANY, _L("Bed Type"), wxDefaultPosition, wxDefaultSize, 0);
    select_sizer->Add(bed_type_sel_text, 0, wxALIGN_LEFT);
    select_sizer->AddSpacer(FromDIP(4));

#ifdef __APPLE__
    m_comboBox_bed_type = new wxComboBox(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, EXTRUSION_CALIBRATION_BED_COMBOX, 0, nullptr, wxCB_READONLY);
#else
    m_comboBox_bed_type = new ComboBox(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, EXTRUSION_CALIBRATION_BED_COMBOX, 0, nullptr, wxCB_READONLY);
#endif
    select_sizer->Add(m_comboBox_bed_type, 0, wxEXPAND);

    // get bed type
    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    if (bed_type_def && bed_type_def->enum_keys_map) {
        for (auto item : *bed_type_def->enum_keys_map) {
            if (item.first == "Default Plate")
                continue;
            m_comboBox_bed_type->AppendString(_L(item.first));
        }
    }

    step_1_sizer->Add(select_sizer, 0, wxEXPAND);

    // static line
    step_1_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);
    wxPanel* static_line = new wxPanel(m_step_1_panel, wxID_ANY, wxDefaultPosition, { -1, FromDIP(1) });
    static_line->SetBackgroundColour(EXTRUSION_CALIBRATION_GREY300);
    step_1_sizer->Add(static_line, 0, wxEXPAND);
    step_1_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    // filament info
    auto info_sizer = new wxFlexGridSizer(0, 3, 0, FromDIP(16));
    info_sizer->SetFlexibleDirection(wxBOTH);
    info_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    auto nozzle_temp_sizer = new wxBoxSizer(wxVERTICAL);
    auto nozzle_temp_text = new wxStaticText(m_step_1_panel, wxID_ANY, _L("Nozzle temperature"));
    auto max_input_width = std::max(std::max(std::max(wxWindow::GetTextExtent(_L("Nozzle temperature")).x,
        wxWindow::GetTextExtent(_L("Bed Temperature")).x),
        wxWindow::GetTextExtent(_L("Max volumetric speed")).x),
        EXTRUSION_CALIBRATION_INPUT_SIZE.x);
    m_nozzle_temp = new TextInput(m_step_1_panel, wxEmptyString, _L("\u2103" /* °C */), "", wxDefaultPosition, { max_input_width, EXTRUSION_CALIBRATION_INPUT_SIZE.y }, wxTE_READONLY);
    nozzle_temp_sizer->Add(nozzle_temp_text, 0, wxALIGN_LEFT);
    nozzle_temp_sizer->AddSpacer(FromDIP(4));
    nozzle_temp_sizer->Add(m_nozzle_temp, 0, wxEXPAND);

    auto bed_temp_sizer = new wxBoxSizer(wxVERTICAL);
    auto bed_temp_text = new wxStaticText(m_step_1_panel, wxID_ANY, _L("Bed temperature"));
    m_bed_temp = new TextInput(m_step_1_panel, wxEmptyString, _L("\u2103" /* °C */), "", wxDefaultPosition, { max_input_width, EXTRUSION_CALIBRATION_INPUT_SIZE.y }, wxTE_READONLY);
    bed_temp_sizer->Add(bed_temp_text, 0, wxALIGN_LEFT);
    bed_temp_sizer->AddSpacer(FromDIP(4));
    bed_temp_sizer->Add(m_bed_temp, 0, wxEXPAND);

    auto max_flow_sizer = new wxBoxSizer(wxVERTICAL);
    auto max_flow_text = new wxStaticText(m_step_1_panel, wxID_ANY, _L("Max volumetric speed"));
    m_max_flow_ratio = new TextInput(m_step_1_panel, wxEmptyString, _L("mm³"), "", wxDefaultPosition, { max_input_width, EXTRUSION_CALIBRATION_INPUT_SIZE.y }, wxTE_READONLY);
    max_flow_sizer->Add(max_flow_text, 0, wxALIGN_LEFT);
    max_flow_sizer->AddSpacer(FromDIP(4));
    max_flow_sizer->Add(m_max_flow_ratio, 0, wxEXPAND);

    info_sizer->Add(nozzle_temp_sizer);
    info_sizer->Add(bed_temp_sizer);
    info_sizer->Add(max_flow_sizer);

    step_1_sizer->Add(info_sizer, 0, wxEXPAND);

    // static line
    step_1_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);
    wxPanel* static_line2 = new wxPanel(m_step_1_panel, wxID_ANY, wxDefaultPosition, { -1, FromDIP(1) });
    static_line2->SetBackgroundColour(EXTRUSION_CALIBRATION_GREY300);
    step_1_sizer->Add(static_line2, 0, wxEXPAND);
    step_1_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    auto cali_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_info_text = new wxStaticText(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_info_text->SetFont(Label::Body_12);
    m_info_text->Hide();

    m_error_text = new wxStaticText(m_step_1_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_error_text->SetFont(Label::Body_12);
    m_error_text->SetForegroundColour(wxColour(208, 27, 27));
    m_error_text->Hide();

    m_button_cali = new Button(m_step_1_panel, _L("Start calibration"));
    m_button_cali->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_cali->Bind(wxEVT_BUTTON, &ExtrusionCalibration::on_click_cali, this);

    m_cali_cancel = new Button(m_step_1_panel, _L("Cancel"));
    m_cali_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_cali_cancel->Hide();
    m_cali_cancel->Bind(wxEVT_BUTTON, &ExtrusionCalibration::on_click_cancel, this);

    m_button_next_step = new Button(m_step_1_panel, _L("Next"));
    m_button_next_step->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_button_next_step->Bind(wxEVT_BUTTON, &ExtrusionCalibration::on_click_next, this);
    m_button_next_step->Hide();

    cali_sizer->Add(m_info_text, 10, wxALIGN_LEFT | wxRIGHT | wxALIGN_CENTRE_VERTICAL, FromDIP(10));
    cali_sizer->Add(m_error_text, 10, wxALIGN_LEFT | wxRIGHT | wxALIGN_CENTRE_VERTICAL, FromDIP(10));
    cali_sizer->AddStretchSpacer();
    cali_sizer->Add(m_button_cali, 0, wxRIGHT | wxALIGN_CENTRE_VERTICAL, FromDIP(10));
    cali_sizer->Add(m_cali_cancel, 0, wxRIGHT | wxALIGN_CENTRE_VERTICAL, FromDIP(10));
    cali_sizer->Add(m_button_next_step, 0, wxRIGHT | wxALIGN_CENTRE_VERTICAL, FromDIP(10));

    step_1_sizer->Add(cali_sizer, 0, wxEXPAND);
    step_1_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);


    m_step_2_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_step_2_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* step_2_sizer = new wxBoxSizer(wxVERTICAL);
    m_step_2_panel->SetSizer(step_2_sizer);

    step_2_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    // save result title and tips
    wxBoxSizer* save_result_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxString fill_intro_text = _L("Calibration completed. Please find the most uniform extrusion line on your hot bed like the picture below, and fill the value on its left side into the factor K input box.");
    m_save_cali_result_title = new Label(m_step_2_panel, fill_intro_text);
    m_save_cali_result_title->SetFont(::Label::Body_12);
    m_save_cali_result_title->SetForegroundColour(EXTRUSION_CALIBRATION_GREY800);
    m_save_cali_result_title->Wrap(this->GetSize().x);
    save_result_sizer->Add(m_save_cali_result_title, 0, wxEXPAND);
    step_2_sizer->Add(save_result_sizer, 0, wxEXPAND);
    step_2_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    auto content_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_calibration_tips_static_bmp = new wxStaticBitmap(m_step_2_panel, wxID_ANY, wxNullBitmap, wxDefaultPosition, EXTRUSION_CALIBRATION_BMP_SIZE, 0);
    m_calibration_tips_static_bmp->SetMinSize(EXTRUSION_CALIBRATION_BMP_SIZE);
    content_sizer->Add(m_calibration_tips_static_bmp, 1, wxEXPAND | wxSHAPED);
    content_sizer->Add(EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0, 0);
    // k/n input value
    auto kn_sizer = new wxBoxSizer(wxVERTICAL);
    auto k_val_text = new wxStaticText(m_step_2_panel, wxID_ANY, _L("Factor K"), wxDefaultPosition, wxDefaultSize, 0);
    m_k_val = new TextInput(m_step_2_panel, wxEmptyString, "", "", wxDefaultPosition, wxDefaultSize);
    auto n_val_text = new wxStaticText(m_step_2_panel, wxID_ANY, _L("Factor N"), wxDefaultPosition, wxDefaultSize, 0);
    m_n_val = new TextInput(m_step_2_panel, wxEmptyString, "", "", wxDefaultPosition, wxDefaultSize);

    // hide n
    n_val_text->Hide();
    m_n_val->Hide();
    kn_sizer->Add(k_val_text, 0, wxALIGN_CENTER_VERTICAL);
    kn_sizer->Add(m_k_val, 0, wxEXPAND);
    kn_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);
    kn_sizer->Add(n_val_text, 0, wxALIGN_CENTER_VERTICAL);
    kn_sizer->Add(m_n_val, 0, wxEXPAND);

    // save button
    m_button_save_result = new Button(m_step_2_panel, _L("Save"));
    m_button_save_result->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_save_result->Bind(wxEVT_BUTTON, &ExtrusionCalibration::on_click_save, this);

    m_button_last_step = new Button(m_step_2_panel, _L("Last Step")); // Back for english
    m_button_last_step->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_button_last_step->Bind(wxEVT_BUTTON, &ExtrusionCalibration::on_click_last, this);


    kn_sizer->AddStretchSpacer();
    kn_sizer->Add(m_button_last_step, 0);
    kn_sizer->AddSpacer(FromDIP(10));
    kn_sizer->Add(m_button_save_result, 0);

    content_sizer->Add(kn_sizer, 0, wxEXPAND);

    step_2_sizer->Add(content_sizer, 0, wxEXPAND);
    step_2_sizer->Add(0, EXTRUSION_CALIBRATION_WIDGET_GAP, 0, 0);

    sizer_main->Add(m_step_1_panel, 1, wxEXPAND);
    sizer_main->Add(m_step_2_panel, 1, wxEXPAND);

    wxBoxSizer* top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->Add(FromDIP(24), 0);
    top_sizer->Add(sizer_main, 1, wxEXPAND);
    top_sizer->Add(FromDIP(24), 0);
    SetSizer(top_sizer);

    // set default nozzle
    m_comboBox_nozzle_dia->SetSelection(1);
    // set a default bed type
    m_comboBox_bed_type->SetSelection(0);
    // set to step 1
    set_step(1);

    Layout();
    Fit();

    m_k_val->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) {
        input_value_finish();
        e.Skip();
        });

    m_n_val->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) {
        input_value_finish();
        e.Skip();
        });


    m_calibration_tips_static_bmp->Bind(wxEVT_PAINT, &ExtrusionCalibration::paint, this);

    m_calibration_tips_static_bmp->Bind(wxEVT_LEFT_UP, &ExtrusionCalibration::open_bitmap, this);

    m_comboBox_filament->Connect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(ExtrusionCalibration::on_select_filament), NULL, this);
    m_comboBox_bed_type->Connect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(ExtrusionCalibration::on_select_bed_type), NULL, this);
    m_comboBox_nozzle_dia->Connect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(ExtrusionCalibration::on_select_nozzle_dia), NULL, this);
}

ExtrusionCalibration::~ExtrusionCalibration()
{
    m_comboBox_filament->Disconnect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(ExtrusionCalibration::on_select_filament), NULL, this);
    m_comboBox_bed_type->Disconnect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(ExtrusionCalibration::on_select_bed_type), NULL, this);
    m_comboBox_nozzle_dia->Disconnect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(ExtrusionCalibration::on_select_nozzle_dia), NULL, this);
}

void ExtrusionCalibration::paint(wxPaintEvent&) {
    auto      size = m_calibration_tips_static_bmp->GetSize();
    wxPaintDC dc(m_calibration_tips_static_bmp);
    wxGCDC gcdc(dc);

    dc.DrawBitmap(m_is_zh ? m_calibration_tips_bmp_zh : m_calibration_tips_bmp_en, wxPoint(0, 0));

    gcdc.SetPen(wxColour(0, 0, 0, 61));
    gcdc.SetBrush(wxColour(0, 0, 0, 61));
    gcdc.DrawRectangle(wxPoint(0, 0), EXTRUSION_CALIBRATION_BMP_TIP_BAR);

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    int pos_offset = (EXTRUSION_CALIBRATION_BMP_TIP_BAR.y - EXTRUSION_CALIBRATION_BMP_BTN_SIZE.y) / 2;
    wxPoint open_btn_pos = wxPoint(size.x - pos_offset - EXTRUSION_CALIBRATION_BMP_BTN_SIZE.x, pos_offset);
    dc.DrawBitmap(m_calibration_tips_open_btn_bmp, open_btn_pos);

    gcdc.SetFont(Label::Head_14);
    gcdc.SetTextForeground(wxColour(255, 255, 255, 224));
    wxSize text_size = wxWindow::GetTextExtent(_L("Example"));
    gcdc.DrawText(_L("Example"), { (EXTRUSION_CALIBRATION_BMP_TIP_BAR.x - text_size.x) / 2, (EXTRUSION_CALIBRATION_BMP_TIP_BAR.y - text_size.y) / 2});

    return;
}

void ExtrusionCalibration::open_bitmap(wxMouseEvent& event) {
    auto pos = event.GetPosition();
    auto size = m_calibration_tips_static_bmp->GetSize();
    if (pos.x > size.x - EXTRUSION_CALIBRATION_BMP_TIP_BAR.y && pos.y > 0 &&
        pos.x < size.x && pos.y < EXTRUSION_CALIBRATION_BMP_TIP_BAR.y) {
        auto* popup = new wxDialog(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize);
        auto bmp_sizer = new wxBoxSizer(wxVERTICAL);
        wxStaticBitmap* zoomed_bitmap =  new wxStaticBitmap(popup, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);
        zoomed_bitmap->SetBitmap(create_scaled_bitmap(m_is_zh ? "extrusion_calibration_tips_zh" : "extrusion_calibration_tips_en", nullptr, 720));
        bmp_sizer->Add(zoomed_bitmap, 1, wxEXPAND);
        popup->SetSizer(bmp_sizer);
        popup->Layout();
        popup->Fit();
        popup->CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(popup);
        popup->ShowModal();
    }
    return;
}

void ExtrusionCalibration::input_value_finish()
{
    ;
}

void ExtrusionCalibration::show_info(bool show, bool is_error, wxString text)
{
    if (show && is_error) {
        m_error_text->Show();
        if (m_error_text->GetLabelText().compare(text) != 0)
            m_error_text->SetLabelText(text);
        m_info_text->Hide();
    }
    else if (show && !is_error) {
        m_info_text->Show();
        if (m_info_text->GetLabelText().compare(text) != 0)
            m_info_text->SetLabelText(text);
        m_error_text->Hide();
    }
    else {
        if (is_error) {
            m_info_text->Hide();
            m_error_text->Show();
            if (m_error_text->GetLabelText().compare(text) != 0)
                m_error_text->SetLabelText(text);
        } else {
            m_info_text->Show();
            if (m_info_text->GetLabelText().compare(text) != 0)
                m_info_text->SetLabelText(text);
            m_error_text->Hide();
        }
    }
}

void ExtrusionCalibration::update()
{
    if (obj) {
        if (obj->is_in_extrusion_cali()) {
            show_info(true, false, wxString::Format(_L("Calibrating... %d%%"), obj->mc_print_percent));
            m_cali_cancel->Show();
            m_cali_cancel->Enable();
            m_button_cali->Hide();
            m_button_next_step->Hide();
        } else if (obj->is_extrusion_cali_finished()) {
            if (m_bed_temp->GetTextCtrl()->GetValue().compare("0") == 0) {
                wxString tips = get_bed_type_incompatible(false);
                show_info(true, true, tips);
            }
            else {
                get_bed_type_incompatible(true);
                show_info(true, false, _L("Calibration completed"));
            }
            m_cali_cancel->Hide();
            m_button_cali->Show();
            m_button_next_step->Show();
        } else {
            if (m_bed_temp->GetTextCtrl()->GetValue().compare("0") == 0) {
                wxString tips = get_bed_type_incompatible(false);
                show_info(true, true, tips);
            } else {
                get_bed_type_incompatible(true);
                show_info(true, false, wxEmptyString);
            }
            m_cali_cancel->Hide();
            m_button_cali->Show();
            m_button_next_step->Hide();
        }
        Layout();
    }
}

void ExtrusionCalibration::on_click_cali(wxCommandEvent& event)
{
    if (obj) {
        int nozzle_temp = -1;
        int bed_temp = -1;
        float max_volumetric_speed = -1;

        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (preset_bundle) {
            for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
                wxString filament_name = wxString::FromUTF8(it->name);
                if (filament_name.compare(m_comboBox_filament->GetValue()) == 0) {
                    try {
                        bed_temp = get_bed_temp(&it->config);
                        const ConfigOptionInts* nozzle_temp_opt = it->config.option<ConfigOptionInts>("nozzle_temperature");
                        const ConfigOptionFloats* speed_opt = it->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
                        if (nozzle_temp_opt && speed_opt) {
                            nozzle_temp = nozzle_temp_opt->get_at(0);
                            max_volumetric_speed = speed_opt->get_at(0);
                            if (bed_temp >= 0 && nozzle_temp >= 0 && max_volumetric_speed >= 0) {
                                int curr_tray_id = ams_id * 4 + tray_id;
                                if (tray_id == VIRTUAL_TRAY_MAIN_ID)
                                    curr_tray_id = tray_id;
                                obj->command_start_extrusion_cali(curr_tray_id, nozzle_temp, bed_temp, max_volumetric_speed, it->setting_id);
                                return;
                            }
                        } else {
                            BOOST_LOG_TRIVIAL(error) << "cali parameters is invalid";
                        }
                    } catch(...) {
                        ;
                    }
                }
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << "extrusion_cali: preset_bundle is nullptr";
        }
    } else {
        BOOST_LOG_TRIVIAL(error) << "cali obj parameters is invalid";
    }
}

void ExtrusionCalibration::on_click_cancel(wxCommandEvent& event)
{
    if (obj) {
        BOOST_LOG_TRIVIAL(info) << "extrusion_cali: stop";
        obj->command_stop_extrusion_cali();
    }
}

bool ExtrusionCalibration::check_k_validation(wxString k_text)
{
    if (k_text.IsEmpty())
        return false;
    double k = 0.0;
    try {
        k_text.ToDouble(&k);
    }
    catch (...) {
        ;
    }

    if (k <= MIN_PA_K_VALUE || k >= MAX_PA_K_VALUE)
        return false;
    return true;
}

bool ExtrusionCalibration::check_k_n_validation(wxString k_text, wxString n_text)
{
    if (k_text.IsEmpty() || n_text.IsEmpty())
        return false;
    double k = 0.0;
    try {
        k_text.ToDouble(&k);
    }
    catch (...) {
        ;
    }

    double n = 0.0;
    try {
        n_text.ToDouble(&n);
    }
    catch (...) {
        ;
    }
    if (k <= MIN_PA_K_VALUE || k >= MAX_PA_K_VALUE)
        return false;
    if (n < 0.6 || n > 2.0)
        return false;
    return true;
}

void ExtrusionCalibration::on_click_save(wxCommandEvent &event)
{
    wxString k_text = m_k_val->GetTextCtrl()->GetValue();
    wxString n_text = m_n_val->GetTextCtrl()->GetValue();
    if (!ExtrusionCalibration::check_k_validation(k_text)) {
        wxString k_tips = wxString::Format(_L("Please input a valid value (K in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE);
        wxString kn_tips = wxString::Format(_L("Please input a valid value (K in %.1f~%.1f, N in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE, 0.6, 2.0);
        MessageDialog msg_dlg(nullptr, k_tips, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    double k = 0.0;
    try {
        k_text.ToDouble(&k);
    }
    catch (...) {
        ;
    }

    double n = 0.0;
    try {
        n_text.ToDouble(&n);
    }
    catch (...) {
        ;
    }

    // set values
    int nozzle_temp = -1;
    int bed_temp = -1;
    float max_volumetric_speed = -1;
    std::string setting_id;
    std::string name;
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
            wxString filament_name = wxString::FromUTF8(it->name);
            if (filament_name.compare(m_comboBox_filament->GetValue()) == 0) {
                if (obj) {
                    bed_temp    = get_bed_temp(&it->config);
                    const ConfigOptionInts* nozzle_temp_opt = it->config.option<ConfigOptionInts>("nozzle_temperature");
                    const ConfigOptionFloats* speed_opt = it->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
                    if (nozzle_temp_opt && speed_opt) {
                        nozzle_temp = nozzle_temp_opt->get_at(0);
                        max_volumetric_speed = speed_opt->get_at(0);
                    }
                    setting_id = it->setting_id;
                    name = it->name;
                }
            }
        }
    }

    // send command
    int curr_tray_id = ams_id * 4 + tray_id;
    if (tray_id == VIRTUAL_TRAY_MAIN_ID)
        curr_tray_id = tray_id;
    obj->command_extrusion_cali_set(curr_tray_id, setting_id, name, k, n, bed_temp, nozzle_temp, max_volumetric_speed);
    Close();
}

void ExtrusionCalibration::on_click_last(wxCommandEvent &event)
{
    set_step(1);
}

void ExtrusionCalibration::on_click_next(wxCommandEvent& event)
{
    set_step(2);
}

bool ExtrusionCalibration::Show(bool show)
{
    if (show) {
        m_k_val->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
        m_n_val->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
    }
    return DPIDialog::Show(show);
}

void ExtrusionCalibration::update_combobox_filaments()
{
    m_comboBox_filament->SetValue(wxEmptyString);
    user_filaments.clear();
    int selection_idx = -1;
    int filament_index = -1;
    int curr_selection = -1;
    wxArrayString filament_items;
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle && obj) {
        BOOST_LOG_TRIVIAL(trace) << "system_preset_bundle filament number=" << preset_bundle->filaments.size();
        std::string printer_type = obj->printer_type;
        std::set<std::string> printer_preset_list;
        for (auto printer_it = preset_bundle->printers.begin(); printer_it != preset_bundle->printers.end(); printer_it++) {
            // only use system printer preset
            if (!printer_it->is_system) continue;

            std::string model_id = printer_it->get_current_printer_type(preset_bundle);
            ConfigOption* printer_nozzle_opt = printer_it->config.option("nozzle_diameter");
            ConfigOptionFloats* printer_nozzle_vals = nullptr;
            if (printer_nozzle_opt)
                printer_nozzle_vals = dynamic_cast<ConfigOptionFloats*>(printer_nozzle_opt);
            double nozzle_value = 0.4;
            wxString nozzle_value_str = m_comboBox_nozzle_dia->GetValue();
            try {
                nozzle_value_str.ToDouble(&nozzle_value);
            } catch(...) {
                ;
            }
            if (!model_id.empty() && model_id.compare(obj->printer_type) == 0
                && printer_nozzle_vals
                && abs(printer_nozzle_vals->get_at(0) - nozzle_value) < 1e-3) {
                    printer_preset_list.insert(printer_it->name);
                    BOOST_LOG_TRIVIAL(trace) << "extrusion_cali: printer_model = " << model_id;
            } else {
                BOOST_LOG_TRIVIAL(error) << "extrusion_cali: printer_model = " << model_id;
            }
        }

        for (auto filament_it = preset_bundle->filaments.begin(); filament_it != preset_bundle->filaments.end(); filament_it++) {
            ConfigOption* printer_opt = filament_it->config.option("compatible_printers");
            ConfigOptionStrings* printer_strs = dynamic_cast<ConfigOptionStrings*>(printer_opt);
            for (auto printer_str : printer_strs->values) {
                if (printer_preset_list.find(printer_str) != printer_preset_list.end()) {
                    user_filaments.push_back(&(*filament_it));

                    // set default filament id
                    filament_index++;
                    if (filament_it->is_system
                        && !ams_filament_id.empty()
                        && filament_it->filament_id == ams_filament_id
                        ) {
                        curr_selection = filament_index;
                    }

                    if (filament_it->name == obj->extrusion_cali_filament_name && !obj->extrusion_cali_filament_name.empty())
                    {
                        curr_selection = filament_index;
                    }

                    wxString filament_name = wxString::FromUTF8(filament_it->name);
                    filament_items.Add(filament_name);
                    break;
                }
            }
        }
        m_comboBox_filament->Set(filament_items);
        m_comboBox_filament->SetSelection(curr_selection);
        post_select_event();
    }

    if (m_comboBox_filament->GetValue().IsEmpty())
        m_button_cali->Disable();
    else
        m_button_cali->Enable();
}

wxString ExtrusionCalibration::get_bed_type_incompatible(bool incompatible)
{
    if (incompatible) {
        m_button_cali->Enable();
        return wxEmptyString;
    }
    else {
        m_button_cali->Disable();
        std::string filament_alias = "";
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (preset_bundle) {
            for (auto filament_it = preset_bundle->filaments.begin(); filament_it != preset_bundle->filaments.end(); filament_it++) {
                wxString filament_name = wxString::FromUTF8(filament_it->name);
                if (filament_name.compare(m_comboBox_filament->GetValue()) == 0) {
                    filament_alias = filament_it->alias;
                }
            }
        }
        wxString tips = wxString::Format(_L("%s does not support %s"), m_comboBox_bed_type->GetValue(), filament_alias);
        return tips;
    }
}

void ExtrusionCalibration::Popup()
{
    this->SetSize(EXTRUSION_CALIBRATION_DIALOG_SIZE);

    update_combobox_filaments();

    set_step(1);

    update();
    Layout();
    Fit();
    wxGetApp().UpdateDlgDarkUI(this);
    ShowModal();
}
void ExtrusionCalibration::post_select_event() {
    wxCommandEvent event(wxEVT_COMBOBOX);
    event.SetEventObject(m_comboBox_filament);
    wxPostEvent(m_comboBox_filament, event);
}

void ExtrusionCalibration::set_step(int step_index)
{
    if (step_index == 2) {
        wxString title_text = wxString::Format("%s - %s 2/2", _L("Dynamic flow Calibration"), _L("Step"));
        SetTitle(title_text);
        m_step_1_panel->Hide();
        m_step_2_panel->Show();
    } else {
        wxString title_text = wxString::Format("%s - %s 1/2", _L("Dynamic flow Calibration"), _L("Step"));
        SetTitle(title_text);
        m_step_1_panel->Show();
        m_step_2_panel->Hide();
    }
    this->SetMinSize(EXTRUSION_CALIBRATION_DIALOG_SIZE);
    Layout();
    Fit();
}

void ExtrusionCalibration::on_select_filament(wxCommandEvent &evt)
{
    m_filament_type = "";
    update_filament_info();

    // set a default value for input values
    if (m_k_val->GetTextCtrl()->GetValue().IsEmpty()) {
        m_k_val->GetTextCtrl()->SetValue("0");
    }
    if (m_n_val->GetTextCtrl()->GetValue().IsEmpty()) {
        m_n_val->GetTextCtrl()->SetValue("0");
    }
}

void ExtrusionCalibration::update_filament_info()
{
    if (m_comboBox_filament->GetValue().IsEmpty()) {
        m_nozzle_temp->GetTextCtrl()->SetValue(wxEmptyString);
        m_bed_temp->GetTextCtrl()->SetValue(wxEmptyString);
        m_max_flow_ratio->GetTextCtrl()->SetValue(wxEmptyString);
        return;
    }

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    int bed_temp_int = -1;
    if (preset_bundle) {
        for (auto filament_it = preset_bundle->filaments.begin(); filament_it != preset_bundle->filaments.end(); filament_it++) {
            wxString filament_name = wxString::FromUTF8(filament_it->name);
            if (filament_name.compare(m_comboBox_filament->GetValue()) == 0) {
                m_filament_type = filament_it->name;

                if (obj) {
                    obj->extrusion_cali_filament_name = filament_it->name;
                    BOOST_LOG_TRIVIAL(info) << "set extrusion cali filament name = " << obj->extrusion_cali_filament_name;
                }

                // update nozzle temperature
                ConfigOption* opt_nozzle_temp = filament_it->config.option("nozzle_temperature");
                if (opt_nozzle_temp) {
                    ConfigOptionInts* opt_min_ints = dynamic_cast<ConfigOptionInts*>(opt_nozzle_temp);
                    if (opt_min_ints) {
                        wxString text_nozzle_temp = wxString::Format("%d", opt_min_ints->get_at(0));
                        m_nozzle_temp->GetTextCtrl()->SetValue(text_nozzle_temp);
                    }
                }
                // update bed temperature
                bed_temp_int = get_bed_temp(&filament_it->config);
                wxString bed_temp_text = wxString::Format("%d", bed_temp_int);
                m_bed_temp->GetTextCtrl()->SetValue(bed_temp_text);

                // update max flow speed
                ConfigOption* opt_flow_speed = filament_it->config.option("filament_max_volumetric_speed");
                if (opt_flow_speed) {
                    ConfigOptionFloats* opt_flow_floats = dynamic_cast<ConfigOptionFloats*>(opt_flow_speed);
                    if (opt_flow_floats) {
                        wxString flow_val_text = wxString::Format("%0.2f", opt_flow_floats->get_at(0));
                        m_max_flow_ratio->GetTextCtrl()->SetValue(flow_val_text);
                    }
                }
            }
        }
    }
}

int ExtrusionCalibration::get_bed_temp(DynamicPrintConfig* config)
{
    BedType curr_bed_type = BedType(m_comboBox_bed_type->GetSelection() + btDefault + 1);
    const ConfigOptionInts* opt_bed_temp_ints = config->option<ConfigOptionInts>(get_bed_temp_key(curr_bed_type));
    if (opt_bed_temp_ints) {
        return opt_bed_temp_ints->get_at(0);
    }
    return -1;
}

void ExtrusionCalibration::on_select_bed_type(wxCommandEvent &evt)
{
    update_filament_info();
}

void ExtrusionCalibration::on_select_nozzle_dia(wxCommandEvent &evt)
{
    update_combobox_filaments();
}

void ExtrusionCalibration::on_dpi_changed(const wxRect &suggested_rect) { this->Refresh(); }

}} // namespace Slic3r::GUI
