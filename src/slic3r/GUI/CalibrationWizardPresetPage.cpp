#include <regex>
#include "CalibrationWizardPresetPage.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r { namespace GUI {
static int PA_LINE = 0;
static int PA_PATTERN = 1;

CaliPresetCaliStagePanel::CaliPresetCaliStagePanel(
    wxWindow* parent,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliPresetCaliStagePanel::create_panel(wxWindow* parent)
{
    auto title = new Label(parent, _L("Calibration Type"));
    title->SetFont(Label::Head_14);
    m_top_sizer->Add(title);
    m_top_sizer->AddSpacer(FromDIP(15));

    m_complete_radioBox = new wxRadioButton(parent, wxID_ANY, _L("Complete Calibration"));
    m_complete_radioBox->SetForegroundColour(*wxBLACK);
    
    m_complete_radioBox->SetValue(true);
    m_stage = CALI_MANUAL_STAGE_1;
    m_top_sizer->Add(m_complete_radioBox);
    m_top_sizer->AddSpacer(FromDIP(10));
    m_fine_radioBox = new wxRadioButton(parent, wxID_ANY, _L("Fine Calibration based on flow ratio"));
    m_fine_radioBox->SetForegroundColour(*wxBLACK);
    m_top_sizer->Add(m_fine_radioBox);

    input_panel = new wxPanel(parent);
    input_panel->Hide();
    auto input_sizer = new wxBoxSizer(wxHORIZONTAL);
    input_panel->SetSizer(input_sizer);
    flow_ratio_input = new TextInput(input_panel, wxEmptyString, "", "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE);
    flow_ratio_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    float default_flow_ratio = 1.0f;
    auto flow_ratio_str = wxString::Format("%.2f", default_flow_ratio);
    flow_ratio_input->GetTextCtrl()->SetValue(flow_ratio_str);
    input_sizer->AddSpacer(FromDIP(18));
    input_sizer->Add(flow_ratio_input, 0, wxTOP, FromDIP(10));
    m_top_sizer->Add(input_panel);

    m_top_sizer->AddSpacer(PRESET_GAP);
    // events
    m_complete_radioBox->Bind(wxEVT_RADIOBUTTON, [this](auto& e) {
        m_stage_panel_parent->get_current_object()->flow_ratio_calibration_type = COMPLETE_CALIBRATION;
        input_panel->Show(false);
        m_stage = CALI_MANUAL_STAGE_1;
        GetParent()->Layout();
        GetParent()->Fit();
        });
    m_fine_radioBox->Bind(wxEVT_RADIOBUTTON, [this](auto& e) {
        m_stage_panel_parent->get_current_object()->flow_ratio_calibration_type = FINE_CALIBRATION;
        input_panel->Show();
        m_stage = CALI_MANUAL_STAGE_2;
        GetParent()->Layout();
        GetParent()->Fit();
        });
    flow_ratio_input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](auto& e) {
        float flow_ratio = 0.0f;
        if (!CalibUtils::validate_input_flow_ratio(flow_ratio_input->GetTextCtrl()->GetValue(), &flow_ratio)) {
            MessageDialog msg_dlg(nullptr, _L("Please input a valid value (0.0 < flow ratio < 2.0)"), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
        }
        auto flow_ratio_str = wxString::Format("%.3f", flow_ratio);
        flow_ratio_input->GetTextCtrl()->SetValue(flow_ratio_str);
        m_flow_ratio_value = flow_ratio;
        });
    flow_ratio_input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this](auto& e) {
        float flow_ratio = 0.0f;
        if (!CalibUtils::validate_input_flow_ratio(flow_ratio_input->GetTextCtrl()->GetValue(), &flow_ratio)) {
            MessageDialog msg_dlg(nullptr, _L("Please input a valid value (0.0 < flow ratio < 2.0)"), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
        }
        auto flow_ratio_str = wxString::Format("%.3f", flow_ratio);
        flow_ratio_input->GetTextCtrl()->SetValue(flow_ratio_str);
        m_flow_ratio_value = flow_ratio;
        e.Skip();
        });
    Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        SetFocusIgnoringChildren();
        });
}

void CaliPresetCaliStagePanel::set_cali_stage(CaliPresetStage stage, float value)
{
    if (stage == CaliPresetStage::CALI_MANUAL_STAGE_1) {
        wxCommandEvent radioBox_evt(wxEVT_RADIOBUTTON);
        radioBox_evt.SetEventObject(m_complete_radioBox);
        wxPostEvent(m_complete_radioBox, radioBox_evt);
        m_stage = stage;
    }
    else if(stage == CaliPresetStage::CALI_MANUAL_STAGE_2){
        wxCommandEvent radioBox_evt(wxEVT_RADIOBUTTON);
        radioBox_evt.SetEventObject(m_fine_radioBox);
        wxPostEvent(m_fine_radioBox, radioBox_evt);
        m_stage = stage;
        m_flow_ratio_value = value;
    }
}

void CaliPresetCaliStagePanel::get_cali_stage(CaliPresetStage& stage, float& value)
{
    stage = m_stage;
    value = (m_stage == CALI_MANUAL_STAGE_2) ? m_flow_ratio_value : value;
}

void CaliPresetCaliStagePanel::set_flow_ratio_value(float flow_ratio)
{
    flow_ratio_input->GetTextCtrl()->SetValue(wxString::Format("%.2f", flow_ratio));
    m_flow_ratio_value = flow_ratio;
}

void CaliPresetCaliStagePanel::set_flow_ratio_calibration_type(FlowRatioCalibrationType type) {
    if (type == COMPLETE_CALIBRATION) {
        m_complete_radioBox->SetValue(true);
        m_stage = CaliPresetStage::CALI_MANUAL_STAGE_1;
        input_panel->Hide();
    }
    else if (type == FINE_CALIBRATION) {
        m_fine_radioBox->SetValue(true);
        m_stage = CaliPresetStage::CALI_MANUAL_STAGE_2;
        input_panel->Show();
    }
    GetParent()->Layout();
    GetParent()->Fit();
}

CaliComboBox::CaliComboBox(wxWindow* parent,
    wxString                              title,
    wxArrayString                         values,
    int                                   default_index, // default delected id
    std::function<void(wxCommandEvent&)>  on_value_change,
    wxWindowID                            id,
    const wxPoint&                        pos,
    const wxSize&                         size,
    long                                  style)
    : wxPanel(parent, id, pos, size, style)
    , m_title(title)
    , m_on_value_change_call_back(on_value_change)
{
    SetBackgroundColour(*wxWHITE);
    m_top_sizer = new wxBoxSizer(wxVERTICAL);
    m_top_sizer->AddSpacer(PRESET_GAP);
    auto combo_title = new Label(this, title);
    combo_title->SetFont(Label::Head_14);
    combo_title->Wrap(-1);
    m_top_sizer->Add(combo_title, 0, wxALL, 0);
    m_top_sizer->AddSpacer(FromDIP(10));
    m_combo_box = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, CALIBRATION_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
    m_top_sizer->Add(m_combo_box, 0, wxALL, 0);
    m_top_sizer->AddSpacer(PRESET_GAP);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);

    // set values
    for (int i = 0; i < values.size(); ++i) {
        m_combo_box->AppendString(values[i]);
    }
    m_combo_box->SetSelection(default_index);

    // bind call back function
    if (m_on_value_change_call_back)
        m_combo_box->Bind(wxEVT_COMBOBOX, m_on_value_change_call_back);
}

int CaliComboBox::get_selection() const
{
    if (m_combo_box)
        return m_combo_box->GetSelection();

    return 0;
}

wxString CaliComboBox::get_value() const
{
    if (m_combo_box)
        return m_combo_box->GetValue();

    return wxString();
}

void CaliComboBox::set_values(const wxArrayString &values)
{
    if (m_combo_box) {
        for (int i = 0; i < values.size(); ++i) {
            m_combo_box->AppendString(values[i]);
        }
        m_combo_box->SetSelection(0);
    }
}

CaliPresetWarningPanel::CaliPresetWarningPanel(
    wxWindow* parent,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliPresetWarningPanel::create_panel(wxWindow* parent)
{
    m_warning_text = new Label(parent, wxEmptyString);
    m_warning_text->SetFont(Label::Body_13);
    m_warning_text->SetForegroundColour(wxColour(230, 92, 92));
    m_warning_text->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    m_top_sizer->Add(m_warning_text, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
}

void CaliPresetWarningPanel::set_warning(wxString text)
{
    m_warning_text->SetLabel(text);
}

CaliPresetCustomRangePanel::CaliPresetCustomRangePanel(
    wxWindow* parent,
    int input_value_nums,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
    , m_input_value_nums(input_value_nums)
{
    SetBackgroundColour(*wxWHITE);

    m_title_texts.resize(input_value_nums);
    m_value_inputs.resize(input_value_nums);

    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliPresetCustomRangePanel::set_unit(wxString unit)
{
    for (size_t i = 0; i < m_input_value_nums; ++i) {
        m_value_inputs[i]->SetLabel(unit);
    }
}

void CaliPresetCustomRangePanel::set_titles(wxArrayString titles)
{
    if (titles.size() != m_input_value_nums)
        return;

    for (size_t i = 0; i < m_input_value_nums; ++i) {
        m_title_texts[i]->SetLabel(titles[i]);
    }
}

void CaliPresetCustomRangePanel::set_values(wxArrayString values) {
    if (values.size() != m_input_value_nums)
        return;

    for (size_t i = 0; i < m_input_value_nums; ++i) {
        m_value_inputs[i]->GetTextCtrl()->SetValue(values[i]);
    }
}

wxArrayString CaliPresetCustomRangePanel::get_values()
{
    wxArrayString result;
    for (size_t i = 0; i < m_input_value_nums; ++i) {
        result.push_back(m_value_inputs[i]->GetTextCtrl()->GetValue());
    }
    return result;
}

void CaliPresetCustomRangePanel::create_panel(wxWindow* parent)
{
    wxBoxSizer* horiz_sizer;
    horiz_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (size_t i = 0; i < m_input_value_nums; ++i) {
        if (i > 0) {
            horiz_sizer->Add(FromDIP(10), 0, 0, wxEXPAND, 0);
        }

        wxBoxSizer *item_sizer;
        item_sizer = new wxBoxSizer(wxVERTICAL);
        m_title_texts[i] = new Label(parent, _L("Title"));
        m_title_texts[i]->Wrap(-1);
        m_title_texts[i]->SetFont(::Label::Body_14);
        item_sizer->Add(m_title_texts[i], 0, wxALL, 0);
        m_value_inputs[i] = new TextInput(parent, wxEmptyString, wxString::FromUTF8("\u2103" /* °C */), "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE, 0);
        m_value_inputs[i]->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        m_value_inputs[i]->GetTextCtrl()->Bind(wxEVT_TEXT, [this, i](wxCommandEvent& event) {
            std::string number = m_value_inputs[i]->GetTextCtrl()->GetValue().ToStdString();
            std::string decimal_point;
            std::string expression = "^[-+]?[0-9]+([,.][0-9]+)?$";
            std::regex decimalRegex(expression);
            int decimal_number = 0;
            if (std::regex_match(number, decimalRegex)) {
                std::smatch match;
                if (std::regex_search(number, match, decimalRegex)) {
                    std::string decimalPart = match[1].str();
                    if (decimalPart != "")
                        decimal_number = decimalPart.length() - 1;
                    else
                        decimal_number = 0;
                }
                int max_decimal_length;
                if (i <= 1)
                    max_decimal_length = 3;
                else if (i >= 2)
                    max_decimal_length = 4;
                if (decimal_number > max_decimal_length) {
                    int allowed_length = number.length() - decimal_number + max_decimal_length;
                    number = number.substr(0, allowed_length);
                    m_value_inputs[i]->GetTextCtrl()->SetValue(number);
                    m_value_inputs[i]->GetTextCtrl()->SetInsertionPointEnd();
                }
            }
            // input is not a number, invalid.
            else
                BOOST_LOG_TRIVIAL(trace) << "The K input string is not a valid number when calibrating. ";

            });
        item_sizer->Add(m_value_inputs[i], 0, wxALL, 0);
        horiz_sizer->Add(item_sizer, 0, wxEXPAND, 0);
    }

    m_top_sizer->Add(horiz_sizer, 0, wxEXPAND, 0);
}


CaliPresetTipsPanel::CaliPresetTipsPanel(
    wxWindow* parent,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    this->SetBackgroundColour(wxColour(238, 238, 238));
    this->SetMinSize(wxSize(MIN_CALIBRATION_PAGE_WIDTH, -1));
    
    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliPresetTipsPanel::create_panel(wxWindow* parent)
{
    m_top_sizer->AddSpacer(FromDIP(10));

    auto preset_panel_tips = new Label(parent, _L("A test model will be printed. Please clear the build plate and place it back to the hot bed before calibration."));
    preset_panel_tips->SetFont(Label::Body_14);
    preset_panel_tips->Wrap(CALIBRATION_TEXT_MAX_LENGTH * 1.5f);
    m_top_sizer->Add(preset_panel_tips, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    m_top_sizer->AddSpacer(FromDIP(10));

    auto info_sizer = new wxFlexGridSizer(0, 3, 0, FromDIP(10));
    info_sizer->SetFlexibleDirection(wxBOTH);
    info_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    auto nozzle_temp_sizer = new wxBoxSizer(wxVERTICAL);
    auto nozzle_temp_text = new Label(parent, _L("Nozzle temperature"));
    nozzle_temp_text->SetFont(Label::Body_12);
    m_nozzle_temp = new TextInput(parent, wxEmptyString, wxString::FromUTF8("\u2103" /* °C */), "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE, wxTE_READONLY);
    m_nozzle_temp->SetBorderWidth(0);
    nozzle_temp_sizer->Add(nozzle_temp_text, 0, wxALIGN_LEFT);
    nozzle_temp_sizer->Add(m_nozzle_temp, 0, wxEXPAND);
    nozzle_temp_text->Hide();
    m_nozzle_temp->Hide();

    auto bed_temp_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto printing_param_text = new Label(parent, _L("Printing Parameters"));
    printing_param_text->SetFont(Label::Head_12);
    printing_param_text->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    bed_temp_sizer->Add(printing_param_text, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));
    auto bed_temp_text = new Label(parent, _L("Bed temperature"));
    bed_temp_text->SetFont(Label::Body_12);

    m_bed_temp = new Label(parent, wxString::FromUTF8("- °C"));
    m_bed_temp->SetFont(Label::Body_12);
    bed_temp_sizer->Add(bed_temp_text, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(10));
    bed_temp_sizer->Add(m_bed_temp, 0, wxALIGN_CENTER);

    auto max_flow_sizer = new wxBoxSizer(wxVERTICAL);
    auto max_flow_text = new Label(parent, _L("Max volumetric speed"));
    max_flow_text->SetFont(Label::Body_12);
    m_max_volumetric_speed = new TextInput(parent, wxEmptyString, wxString::FromUTF8("mm³"), "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE, wxTE_READONLY);
    m_max_volumetric_speed->SetBorderWidth(0);
    max_flow_sizer->Add(max_flow_text, 0, wxALIGN_LEFT);
    max_flow_sizer->Add(m_max_volumetric_speed, 0, wxEXPAND);
    max_flow_text->Hide();
    m_max_volumetric_speed->Hide();

    m_nozzle_temp->GetTextCtrl()->Bind(wxEVT_SET_FOCUS, [](auto&) {});
    m_max_volumetric_speed->GetTextCtrl()->Bind(wxEVT_SET_FOCUS, [](auto&) {});

    info_sizer->Add(nozzle_temp_sizer);
    info_sizer->Add(bed_temp_sizer);
    info_sizer->Add(max_flow_sizer);
    m_top_sizer->Add(info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    m_top_sizer->AddSpacer(FromDIP(10));
}

void CaliPresetTipsPanel::set_params(int nozzle_temp, int bed_temp, float max_volumetric)
{
    wxString text_nozzle_temp = wxString::Format("%d", nozzle_temp);
    m_nozzle_temp->GetTextCtrl()->SetValue(text_nozzle_temp);

    std::string bed_temp_text = bed_temp==0 ? "-": std::to_string(bed_temp);
    m_bed_temp->SetLabel(wxString::FromUTF8(bed_temp_text + "\u2103" /* °C */));

    wxString flow_val_text = wxString::Format("%0.2f", max_volumetric);
    m_max_volumetric_speed->GetTextCtrl()->SetValue(flow_val_text);
}

void CaliPresetTipsPanel::get_params(int& nozzle_temp, int& bed_temp, float& max_volumetric)
{
    try {
        nozzle_temp = stoi(m_nozzle_temp->GetTextCtrl()->GetValue().ToStdString());
    }
    catch (...) {
        nozzle_temp = 0;
    }
    try {
        bed_temp = stoi(m_bed_temp->GetLabel().ToStdString());
    }
    catch (...) {
        bed_temp = 0;
    }
    try {
        max_volumetric = stof(m_max_volumetric_speed->GetTextCtrl()->GetValue().ToStdString());
    }
    catch (...) {
        max_volumetric = 0.0f;
    }
}

CalibrationPresetPage::CalibrationPresetPage(
    wxWindow* parent,
    CalibMode cali_mode,
    bool custom_range,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : CalibrationWizardPage(parent, id, pos, size, style)
    , m_show_custom_range(custom_range)
{
    SetBackgroundColour(*wxWHITE);

    m_cali_mode = cali_mode;
    m_page_type = CaliPageType::CALI_PAGE_PRESET;
    m_cali_filament_mode = CalibrationFilamentMode::CALI_MODEL_SINGLE;
    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationPresetPage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_ams_sync_button->msw_rescale();
    m_virtual_tray_comboBox->msw_rescale();
    for (auto& comboBox : m_filament_comboBox_list) {
        comboBox->msw_rescale();
    }
}

void CalibrationPresetPage::on_sys_color_changed()
{
    CalibrationWizardPage::on_sys_color_changed();
    m_ams_sync_button->msw_rescale();
}

void CalibrationPresetPage::create_selection_panel(wxWindow* parent)
{
    auto panel_sizer = new wxBoxSizer(wxVERTICAL);

    auto nozzle_combo_text = new Label(parent, _L("Nozzle Diameter"));
    nozzle_combo_text->SetFont(Label::Head_14);
    nozzle_combo_text->Wrap(-1);
    panel_sizer->Add(nozzle_combo_text, 0, wxALL, 0);
    panel_sizer->AddSpacer(FromDIP(10));
    m_comboBox_nozzle_dia = new ComboBox(parent, wxID_ANY, "", wxDefaultPosition, CALIBRATION_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
    panel_sizer->Add(m_comboBox_nozzle_dia, 0, wxALL, 0);

    panel_sizer->AddSpacer(PRESET_GAP);

    auto plate_type_combo_text = new Label(parent, _L("Plate Type"));
    plate_type_combo_text->SetFont(Label::Head_14);
    plate_type_combo_text->Wrap(-1);
    panel_sizer->Add(plate_type_combo_text, 0, wxALL, 0);
    panel_sizer->AddSpacer(FromDIP(10));
    m_comboBox_bed_type = new ComboBox(parent, wxID_ANY, "", wxDefaultPosition, CALIBRATION_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
    panel_sizer->Add(m_comboBox_bed_type, 0, wxALL, 0);

    panel_sizer->AddSpacer(PRESET_GAP);

    m_filament_from_panel = new wxPanel(parent);
    m_filament_from_panel->Hide();
    auto filament_from_sizer = new wxBoxSizer(wxVERTICAL);
    auto filament_from_text = new Label(m_filament_from_panel, _L("filament position"));
    filament_from_text->SetFont(Label::Head_14);
    filament_from_sizer->Add(filament_from_text, 0);
    auto raioBox_sizer = new wxFlexGridSizer(2, 1, 0, FromDIP(10));
    m_ams_radiobox = new wxRadioButton(m_filament_from_panel, wxID_ANY, _L("AMS"));
    m_ams_radiobox->SetValue(true);

    raioBox_sizer->Add(m_ams_radiobox, 0);
    m_ext_spool_radiobox = new wxRadioButton(m_filament_from_panel, wxID_ANY, _L("External Spool"));
    raioBox_sizer->Add(m_ext_spool_radiobox, 0);
    filament_from_sizer->Add(raioBox_sizer, 0);
    m_filament_from_panel->SetSizer(filament_from_sizer);
    panel_sizer->Add(m_filament_from_panel, 0, wxBOTTOM, PRESET_GAP);

    auto filament_for_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto filament_for_text = new Label(parent, _L("Filament For Calibration"));
    filament_for_text->SetFont(Label::Head_14);
    filament_for_title_sizer->Add(filament_for_text, 0, wxALIGN_CENTER);
    filament_for_title_sizer->AddSpacer(FromDIP(25));
    m_ams_sync_button = new ScalableButton(parent, wxID_ANY, "ams_fila_sync", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 18);
    m_ams_sync_button->SetBackgroundColour(*wxWHITE);
    m_ams_sync_button->SetToolTip(_L("Synchronize filament list from AMS"));
    filament_for_title_sizer->Add(m_ams_sync_button, 0, wxALIGN_CENTER);
    panel_sizer->Add(filament_for_title_sizer);
    panel_sizer->AddSpacer(FromDIP(6));

    parent->SetSizer(panel_sizer);
    panel_sizer->Fit(parent);

    m_ams_radiobox->Bind(wxEVT_RADIOBUTTON, &CalibrationPresetPage::on_choose_ams, this);
    m_ext_spool_radiobox->Bind(wxEVT_RADIOBUTTON, &CalibrationPresetPage::on_choose_ext_spool, this);
    m_ams_sync_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        sync_ams_info(curr_obj);
    });

    m_comboBox_nozzle_dia->Bind(wxEVT_COMBOBOX, &CalibrationPresetPage::on_select_nozzle, this);

    m_comboBox_bed_type->Bind(wxEVT_COMBOBOX, &CalibrationPresetPage::on_select_plate_type, this);
}

#define NOZZLE_LIST_COUNT       4
#define NOZZLE_LIST_DEFAULT     1
float nozzle_diameter_list[NOZZLE_LIST_COUNT] = {0.2, 0.4, 0.6, 0.8 };

void CalibrationPresetPage::init_selection_values()
{
    // init nozzle diameter
    for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
        m_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f mm", nozzle_diameter_list[i]));
    }
    m_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);

    // init plate type
    int curr_selection = 0;
    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    if (bed_type_def && bed_type_def->enum_keys_map) {
        for (auto item : bed_type_def->enum_labels) {
            m_comboBox_bed_type->AppendString(_L(item));
        }
        m_comboBox_bed_type->SetSelection(curr_selection);
    }
}

void CalibrationPresetPage::create_filament_list_panel(wxWindow* parent)
{
    auto panel_sizer = new wxBoxSizer(wxVERTICAL);

    m_filament_list_tips = new Label(parent, _L("Tips for calibration material: \n- Materials that can share same hot bed temperature\n- Different filament brand and family (Brand = Bambu, Family = Basic, Matte)"));
    m_filament_list_tips->Hide();
    m_filament_list_tips->SetFont(Label::Body_13);
    m_filament_list_tips->SetForegroundColour(wxColour(145, 145, 145));
    m_filament_list_tips->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    panel_sizer->Add(m_filament_list_tips, 0, wxBOTTOM, FromDIP(10));

    // ams panel
    m_multi_ams_panel = new wxPanel(parent);
    auto multi_ams_sizer = new wxBoxSizer(wxVERTICAL);
    auto ams_items_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < 4; i++) {
        AMSinfo temp_info = AMSinfo{ std::to_string(i), std::vector<Caninfo>{} };
        auto amsitem = new AMSPreview(m_multi_ams_panel, wxID_ANY, temp_info);
        amsitem->Bind(wxEVT_LEFT_DOWN, [this, amsitem](wxMouseEvent& e) {
            on_switch_ams(amsitem->get_ams_id());
            e.Skip();
            });
        m_ams_preview_list.push_back(amsitem);
        ams_items_sizer->Add(amsitem, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(6));
    }
    multi_ams_sizer->Add(ams_items_sizer, 0);
    multi_ams_sizer->AddSpacer(FromDIP(10));
    m_multi_ams_panel->SetSizer(multi_ams_sizer);

    panel_sizer->Add(m_multi_ams_panel);
    m_multi_ams_panel->Hide();

    auto filament_fgSizer = new wxFlexGridSizer(2, 2, FromDIP(10), CALIBRATION_FGSIZER_HGAP);
    for (int i = 0; i < 4; i++) {
        auto filament_comboBox_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxRadioButton* radio_btn = new wxRadioButton(m_filament_list_panel, wxID_ANY, "");
        CheckBox* check_box = new CheckBox(m_filament_list_panel);
        check_box->SetBackgroundColour(*wxWHITE);
        FilamentComboBox* fcb = new FilamentComboBox(m_filament_list_panel);
        fcb->SetRadioBox(radio_btn);
        fcb->SetCheckBox(check_box);
        fcb->set_select_mode(CalibrationFilamentMode::CALI_MODEL_SINGLE);
        filament_comboBox_sizer->Add(radio_btn, 0, wxALIGN_CENTER);
        filament_comboBox_sizer->Add(check_box, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
        filament_comboBox_sizer->Add(fcb, 0, wxALIGN_CENTER);
        filament_fgSizer->Add(filament_comboBox_sizer, 0);

        fcb->Bind(EVT_CALI_TRAY_CHANGED, &CalibrationPresetPage::on_select_tray, this);

        radio_btn->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent& evt) {
            wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
            event.SetEventObject(this);
            wxPostEvent(this, event);
            });
        check_box->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
            wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
            event.SetEventObject(this);
            wxPostEvent(this, event);
            evt.Skip();
            });
        m_filament_comboBox_list.push_back(fcb);

        if (i == 0)
            radio_btn->SetValue(true);
    }
    panel_sizer->Add(filament_fgSizer, 0);

    parent->SetSizer(panel_sizer);
    panel_sizer->Fit(parent);
}

void CalibrationPresetPage::create_ext_spool_panel(wxWindow* parent)
{
    auto panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer->AddSpacer(FromDIP(10));
    wxRadioButton* radio_btn = new wxRadioButton(parent, wxID_ANY, "");
    CheckBox* check_box = new CheckBox(parent);
    m_virtual_tray_comboBox = new FilamentComboBox(parent);
    m_virtual_tray_comboBox->SetRadioBox(radio_btn);
    m_virtual_tray_comboBox->SetCheckBox(check_box);
    m_virtual_tray_comboBox->set_select_mode(CalibrationFilamentMode::CALI_MODEL_SINGLE);
    radio_btn->SetValue(true);

    m_virtual_tray_comboBox->Bind(EVT_CALI_TRAY_CHANGED, &CalibrationPresetPage::on_select_tray, this);

    panel_sizer->Add(radio_btn, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
    panel_sizer->Add(check_box, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
    panel_sizer->Add(m_virtual_tray_comboBox, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
    parent->SetSizer(panel_sizer);
    panel_sizer->Fit(parent);

    radio_btn->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent& evt) {
        wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
        event.SetEventObject(this);
        wxPostEvent(this, event);
        });
}

void CalibrationPresetPage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    if (m_cali_mode == CalibMode::Calib_Flow_Rate) {
        wxArrayString steps;
        steps.Add(_L("Preset"));
        steps.Add(_L("Calibration1"));
        steps.Add(_L("Calibration2"));
        steps.Add(_L("Record Factor"));
        m_step_panel = new CaliPageStepGuide(parent, steps);
        m_step_panel->set_steps(0);
    }
    else {
        wxArrayString steps;
        steps.Add(_L("Preset"));
        steps.Add(_L("Calibration"));
        steps.Add(_L("Record Factor"));
        m_step_panel = new CaliPageStepGuide(parent, steps);
        m_step_panel->set_steps(0);
    }

    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    m_cali_stage_panel = new CaliPresetCaliStagePanel(parent);
    m_cali_stage_panel->set_parent(this);
    m_top_sizer->Add(m_cali_stage_panel, 0);

    m_selection_panel = new wxPanel(parent);
    m_selection_panel->SetBackgroundColour(*wxWHITE);
    create_selection_panel(m_selection_panel);
    init_selection_values();

    m_filament_list_panel = new wxPanel(parent);
    m_filament_list_panel->SetBackgroundColour(*wxWHITE);
    create_filament_list_panel(m_filament_list_panel);

    if (m_cali_mode == CalibMode::Calib_PA_Line || m_cali_mode == CalibMode::Calib_PA_Pattern) {
        wxArrayString pa_cali_modes;
        pa_cali_modes.push_back(_L("Line"));
        pa_cali_modes.push_back(_L("Pattern"));
        m_pa_cali_method_combox = new CaliComboBox(parent, _L("Method"), pa_cali_modes);
    }
    
    m_ext_spool_panel = new wxPanel(parent);
    create_ext_spool_panel(m_ext_spool_panel);
    m_ext_spool_panel->Hide();

    m_warning_panel = new CaliPresetWarningPanel(parent);

    m_tips_panel = new CaliPresetTipsPanel(parent);

    m_sending_panel = new CaliPageSendingPanel(parent);
    m_sending_panel->get_sending_progress_bar()->set_cancel_callback_fina([this]() {
        on_cali_cancel_job();
        });
    m_sending_panel->Hide();

    m_custom_range_panel = new CaliPresetCustomRangePanel(parent);

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_PRESET);

    m_statictext_printer_msg = new Label(this, wxEmptyString, wxALIGN_CENTER_HORIZONTAL);
    m_statictext_printer_msg->SetFont(::Label::Body_13);
    m_statictext_printer_msg->Hide();

    m_top_sizer->Add(m_selection_panel, 0);
    m_top_sizer->Add(m_filament_list_panel, 0);
    m_top_sizer->Add(m_ext_spool_panel, 0);
    if (m_pa_cali_method_combox)
        m_top_sizer->Add(m_pa_cali_method_combox, 0);
    m_top_sizer->Add(m_custom_range_panel, 0);
    m_top_sizer->AddSpacer(FromDIP(15));
    m_top_sizer->Add(m_warning_panel, 0);
    m_top_sizer->Add(m_tips_panel, 0);
    m_top_sizer->AddSpacer(PRESET_GAP);
    m_top_sizer->Add(m_sending_panel, 0, wxALIGN_CENTER);
    m_top_sizer->Add(m_statictext_printer_msg, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);

    Bind(EVT_CALI_TRAY_CHANGED, &CalibrationPresetPage::on_select_tray, this);
}

void CalibrationPresetPage::update_print_status_msg(wxString msg, bool is_warning)
{
    update_priner_status_msg(msg, is_warning);
}

wxString CalibrationPresetPage::format_text(wxString& m_msg)
{
    if (wxGetApp().app_config->get("language") != "zh_CN") { return m_msg; }

    wxString out_txt = m_msg;
    wxString count_txt = "";
    int      new_line_pos = 0;

    for (int i = 0; i < m_msg.length(); i++) {
        auto text_size = m_statictext_printer_msg->GetTextExtent(count_txt);
        if (text_size.x < (FromDIP(600))) {
            count_txt += m_msg[i];
        }
        else {
            out_txt.insert(i - 1, '\n');
            count_txt = "";
        }
    }
    return out_txt;
}

void CalibrationPresetPage::stripWhiteSpace(std::string& str)
{
    if (str == "") { return; }

    string::iterator cur_it;
    cur_it = str.begin();

    while (cur_it != str.end()) {
        if ((*cur_it) == '\n' || (*cur_it) == ' ') {
            cur_it = str.erase(cur_it);
        }
        else {
            cur_it++;
        }
    }
}

void CalibrationPresetPage::update_priner_status_msg(wxString msg, bool is_warning)
{
    auto colour = is_warning ? wxColour(0xFF, 0x6F, 0x00) : wxColour(0x6B, 0x6B, 0x6B);
    m_statictext_printer_msg->SetForegroundColour(colour);

    if (msg.empty()) {
        if (!m_statictext_printer_msg->GetLabel().empty()) {
            m_statictext_printer_msg->SetLabel(wxEmptyString);
            m_statictext_printer_msg->Hide();
            Layout();
            Fit();
        }
    }
    else {
        msg = format_text(msg);

        auto str_new = msg.ToStdString();
        stripWhiteSpace(str_new);

        auto str_old = m_statictext_printer_msg->GetLabel().ToStdString();
        stripWhiteSpace(str_old);

        if (str_new != str_old) {
            if (m_statictext_printer_msg->GetLabel() != msg) {
                m_statictext_printer_msg->SetLabel(msg);
                m_statictext_printer_msg->SetMinSize(wxSize(FromDIP(600), -1));
                m_statictext_printer_msg->SetMaxSize(wxSize(FromDIP(600), -1));
                m_statictext_printer_msg->Wrap(FromDIP(600));
                m_statictext_printer_msg->Show();
                Layout();
                Fit();
            }
        }
    }
}

void CalibrationPresetPage::on_select_nozzle(wxCommandEvent& evt)
{
    update_combobox_filaments(curr_obj);
}

void CalibrationPresetPage::on_select_plate_type(wxCommandEvent& evt)
{
    select_default_compatible_filament();
    check_filament_compatible();
}

void CalibrationPresetPage::on_choose_ams(wxCommandEvent& event)
{
    select_default_compatible_filament();

    m_filament_list_panel->Show();
    m_ams_sync_button->Show();
    m_ext_spool_panel->Hide();
    Layout();
}

void CalibrationPresetPage::on_choose_ext_spool(wxCommandEvent& event)
{
    m_filament_list_panel->Hide();
    m_ams_sync_button->Hide();
    m_ext_spool_panel->Show();
    Layout();
}

void CalibrationPresetPage::on_select_tray(wxCommandEvent& event)
{
    check_filament_compatible();

    on_recommend_input_value();
}

void CalibrationPresetPage::on_switch_ams(std::string ams_id)
{
    for (auto i = 0; i < m_ams_preview_list.size(); i++) {
        AMSPreview *item = m_ams_preview_list[i];
        if (item->get_ams_id() == ams_id) {
            item->OnSelected();
        } else {
            item->UnSelected();
        }
    }

    update_filament_combobox(ams_id);

    select_default_compatible_filament();

    Layout();
}

void CalibrationPresetPage::on_recommend_input_value()
{
    //TODO fix this
    std::map<int, Preset *> selected_filaments = get_selected_filaments();
    if (selected_filaments.empty())
        return;

    if (m_cali_mode == CalibMode::Calib_PA_Line) {

    }
    else if (m_cali_mode == CalibMode::Calib_Flow_Rate && m_cali_stage_panel) {
        Preset *selected_filament_preset = selected_filaments.begin()->second;
        if (selected_filament_preset) {
            const ConfigOptionFloats* flow_ratio_opt = selected_filament_preset->config.option<ConfigOptionFloats>("filament_flow_ratio");
            if (flow_ratio_opt) {
                m_cali_stage_panel->set_flow_ratio_value(flow_ratio_opt->get_at(0));
            }
        }
    }
    else if (m_cali_mode == CalibMode::Calib_Vol_speed_Tower) {
        Preset* selected_filament_preset = selected_filaments.begin()->second;
        if (selected_filament_preset) {
            if (m_custom_range_panel) {
                const ConfigOptionFloats* speed_opt = selected_filament_preset->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
                if (speed_opt) {
                    double max_volumetric_speed = speed_opt->get_at(0);
                    wxArrayString values;
                    values.push_back(wxString::Format("%.2f", max_volumetric_speed - 5));
                    values.push_back(wxString::Format("%.2f", max_volumetric_speed + 5));
                    values.push_back(wxString::Format("%.2f", 0.5f));
                    m_custom_range_panel->set_values(values);
                }
            }
        }
    }
}

void CalibrationPresetPage::check_filament_compatible()
{
    std::map<int, Preset*> selected_filaments = get_selected_filaments();
    std::string incompatiable_filament_name;
    std::string error_tips;
    int bed_temp = 0;

    std::vector<Preset*> selected_filaments_list;
    for (auto& item: selected_filaments)
        selected_filaments_list.push_back(item.second);

    if (!is_filaments_compatiable(selected_filaments_list, bed_temp, incompatiable_filament_name, error_tips)) {
        m_tips_panel->set_params(0, 0, 0.0f);
        if (!error_tips.empty()) {
            wxString tips = from_u8(error_tips);
            m_warning_panel->set_warning(tips);
        } else {
            wxString tips = wxString::Format(_L("%s is not compatible with %s"), m_comboBox_bed_type->GetValue(), incompatiable_filament_name);
            m_warning_panel->set_warning(tips);
        }
        m_has_filament_incompatible = true;
        update_show_status();
    } else {
        m_tips_panel->set_params(0, bed_temp, 0);
        m_warning_panel->set_warning("");
        m_has_filament_incompatible = false;
        update_show_status();
    }

    Layout();
}

bool CalibrationPresetPage::is_filaments_compatiable(const std::vector<Preset*>& prests)
{
    std::string incompatiable_filament_name;
    std::string error_tips;
    int bed_temp = 0;
    return is_filaments_compatiable(prests, bed_temp, incompatiable_filament_name, error_tips);
}

bool CalibrationPresetPage::is_filament_in_blacklist(Preset* preset, std::string& error_tips)
{
    if (m_ams_radiobox->GetValue() && wxGetApp().app_config->get("skip_ams_blacklist_check") != "true") {
        bool in_blacklist = false;
        std::string action;
        std::string info;
        std::string filamnt_type;
        preset->get_filament_type(filamnt_type);

        auto vendor = dynamic_cast<ConfigOptionStrings*> (preset->config.option("filament_vendor"));
        if (vendor && (vendor->values.size() > 0)) {
            std::string vendor_name = vendor->values[0];
            DeviceManager::check_filaments_in_blacklist(vendor_name, filamnt_type, in_blacklist, action, info);
        }

        if (in_blacklist) {
            error_tips = info;
            if (action == "prohibition") {
                return false;
            }
            else if (action == "warning") {
                return true;
            }
        }
        else {
            error_tips = "";
            return true;
        }
    }
    if (m_ext_spool_radiobox->GetValue()) {
        if (m_cali_mode == CalibMode::Calib_PA_Line && m_cali_method == CalibrationMethod::CALI_METHOD_AUTO) {
            std::string filamnt_type;
            preset->get_filament_type(filamnt_type);
            if (filamnt_type == "TPU") {
                error_tips = _u8L("TPU is not supported for Flow Dynamics Auto-Calibration.");
                return false;
            }
        }
    }
    return true;
}

bool CalibrationPresetPage::is_filaments_compatiable(const std::vector<Preset*> &prests,
    int& bed_temp,
    std::string& incompatiable_filament_name,
    std::string& error_tips)
{
    if (prests.empty()) return true;

    bed_temp = 0;
    std::vector<std::string> filament_types;
    for (auto &item : prests) {
        if (!item)
            continue;

        // update bed temperature
        BedType curr_bed_type = BedType(m_comboBox_bed_type->GetSelection() + btDefault + 1);
        const ConfigOptionInts *opt_bed_temp_ints = item->config.option<ConfigOptionInts>(get_bed_temp_key(curr_bed_type));
        int bed_temp_int = 0;
        if (opt_bed_temp_ints) {
            bed_temp_int = opt_bed_temp_ints->get_at(0);
        }

        if (bed_temp_int <= 0) {
            if (!item->alias.empty())
                incompatiable_filament_name = item->alias;
            else
                incompatiable_filament_name = item->name;

            return false;
        } else {
            // set for first preset
            if (bed_temp == 0)
                bed_temp = bed_temp_int;
        }
        std::string display_filament_type;
        filament_types.push_back(item->config.get_filament_type(display_filament_type, 0));

        // check is it in the filament blacklist
        if (!is_filament_in_blacklist(item, error_tips))
            return false;
    }

    if (!Print::check_multi_filaments_compatibility(filament_types)) {
        error_tips = _u8L("Cannot print multiple filaments which have large difference of temperature together. Otherwise, the extruder and nozzle may be blocked or damaged during printing");
        return false;
    }

    return true;
}

void CalibrationPresetPage::update_plate_type_collection(CalibrationMethod method)
{
    m_comboBox_bed_type->Clear();
    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    if (bed_type_def && bed_type_def->enum_keys_map) {
        for (int i = 0; i < bed_type_def->enum_labels.size(); i++) {
            m_comboBox_bed_type->AppendString(_L(bed_type_def->enum_labels[i]));
        }
        m_comboBox_bed_type->SetSelection(0);
    }
}

void CalibrationPresetPage::update_combobox_filaments(MachineObject* obj)
{
    if (!obj) return;

    if (!obj->is_info_ready())
        return;

    //step 1: update combobox filament list
    float nozzle_value = get_nozzle_value();
    obj->cali_selected_nozzle_dia = nozzle_value;
    if (nozzle_value < 1e-3) {
        return;
    }

    Preset* printer_preset = get_printer_preset(obj, nozzle_value);

    // sync ams filaments list info
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle && printer_preset) {
        preset_bundle->set_calibrate_printer(printer_preset->name);
    }

    //step 2: sync ams info from object by default
    sync_ams_info(obj);

    //step 3: select the default compatible filament to calibration
    select_default_compatible_filament();
}

bool CalibrationPresetPage::is_blocking_printing()
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;

    MachineObject* obj_ = dev->get_selected_machine();
    if (obj_ == nullptr) return true;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    auto source_model = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
    auto target_model = obj_->printer_type;

    if (source_model != target_model) {
        std::vector<std::string> compatible_machine = dev->get_compatible_machine(target_model);
        vector<std::string>::iterator it = find(compatible_machine.begin(), compatible_machine.end(), source_model);
        if (it == compatible_machine.end()) {
            return true;
        }
    }

    return false;
}

void CalibrationPresetPage::update_show_status()
{
    NetworkAgent* agent = Slic3r::GUI::wxGetApp().getAgent();
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!agent) {return;}
    if (!dev) return;
    dev->check_pushing();

    MachineObject* obj_ = dev->get_selected_machine();
    if (!obj_) {
        if (agent->is_user_login()) {
            show_status(CaliPresetPageStatus::CaliPresetStatusInvalidPrinter);
        }
        else {
            show_status(CaliPresetPageStatus::CaliPresetStatusNoUserLogin);
        }
        return;
    }

    if (!obj_->is_lan_mode_printer()) {
        if (!agent->is_server_connected()) {
            agent->refresh_connection();
            show_status(CaliPresetPageStatus::CaliPresetStatusConnectingServer);
            return;
        }
    }

    if (wxGetApp().app_config && wxGetApp().app_config->get("internal_debug").empty()) {
        if (obj_->upgrade_force_upgrade) {
            show_status(CaliPresetPageStatus::CaliPresetStatusNeedForceUpgrading);
            return;
        }

        if (obj_->upgrade_consistency_request) {
            show_status(CaliPresetStatusNeedConsistencyUpgrading);
            return;
        }
    }

    //if (is_blocking_printing()) {
    //    show_status(CaliPresetPageStatus::CaliPresetStatusUnsupportedPrinter);
    //    return;
    //}
    //else
    if (obj_->is_connecting() || !obj_->is_connected()) {
        show_status(CaliPresetPageStatus::CaliPresetStatusInConnecting);
        return;
    }
    else if (obj_->is_in_upgrading()) {
        show_status(CaliPresetPageStatus::CaliPresetStatusInUpgrading);
        return;
    }
    else if (obj_->is_system_printing()) {
        show_status(CaliPresetPageStatus::CaliPresetStatusInSystemPrinting);
        return;
    }
    else if (obj_->is_in_printing()) {
        show_status(CaliPresetPageStatus::CaliPresetStatusInPrinting);
        return;
    }
    else if (!obj_->is_support_print_without_sd && (obj_->get_sdcard_state() == MachineObject::SdcardState::NO_SDCARD)) {
        show_status(CaliPresetPageStatus::CaliPresetStatusNoSdcard);
        return;
    }

    // check sdcard when if lan mode printer
    if (obj_->is_lan_mode_printer()) {
        if (obj_->get_sdcard_state() == MachineObject::SdcardState::NO_SDCARD) {
            show_status(CaliPresetPageStatus::CaliPresetStatusLanModeNoSdcard);
            return;
        }
    }

    if (m_has_filament_incompatible) {
        show_status(CaliPresetPageStatus::CaliPresetStatusFilamentIncompatible);
        return;
    }

    show_status(CaliPresetPageStatus::CaliPresetStatusNormal);
}


bool CalibrationPresetPage::need_check_sdcard(MachineObject* obj)
{
    if (!obj) return false;

    bool need_check = false;
    if (obj->get_printer_series() == PrinterSeries::SERIES_X1) {
        if (m_cali_mode == CalibMode::Calib_Flow_Rate && m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            need_check = true;
        }
        else if (m_cali_mode == CalibMode::Calib_Vol_speed_Tower && m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL)
        {
            need_check =  true;
        }
    }
    else if (obj->get_printer_series() == PrinterSeries::SERIES_P1P) {
        if (m_cali_mode == CalibMode::Calib_Flow_Rate && m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            need_check =  true;
        }
        else if (m_cali_mode == CalibMode::Calib_Vol_speed_Tower && m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            need_check =  true;
        }
    }
    else {
        assert(false);
        return false;
    }

    return need_check;
}

void CalibrationPresetPage::show_status(CaliPresetPageStatus status)
{
    if (m_stop_update_page_status)
        return;

    if (m_page_status != status)
        //BOOST_LOG_TRIVIAL(info) << "CalibrationPresetPage: show_status = " << status << "(" << get_print_status_info(status) << ")";
    m_page_status = status;

    // other
    if (status == CaliPresetPageStatus::CaliPresetStatusInit) {
        update_print_status_msg(wxEmptyString, false);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusNormal) {
        update_print_status_msg(wxEmptyString, false);
        Enable_Send_Button(true);
        Layout();
        Fit();
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusNoUserLogin) {
        wxString msg_text = _L("No login account, only printers in LAN mode are displayed");
        update_print_status_msg(msg_text, false);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInvalidPrinter) {
        update_print_status_msg(wxEmptyString, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusConnectingServer) {
        wxString msg_text = _L("Connecting to server");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInUpgrading) {
        wxString msg_text = _L("Cannot send the print job when the printer is updating firmware");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInSystemPrinting) {
        wxString msg_text = _L("The printer is executing instructions. Please restart printing after it ends");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInPrinting) {
        wxString msg_text = _L("The printer is busy on other print job");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusLanModeNoSdcard) {
        wxString msg_text = _L("An SD card needs to be inserted before printing via LAN.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(true);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusNoSdcard) {
        wxString msg_text = _L("An SD card needs to be inserted before printing.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusNeedForceUpgrading) {
        wxString msg_text = _L("Cannot send the print job to a printer whose firmware is required to get updated.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusNeedConsistencyUpgrading) {
        wxString msg_text = _L("Cannot send the print job to a printer whose firmware is required to get updated.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusFilamentIncompatible) {
        update_print_status_msg(wxEmptyString, false);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInConnecting) {
        wxString msg_text = _L("Connecting to printer");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }

    Layout();
}

void CalibrationPresetPage::Enable_Send_Button(bool enable)
{
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_CALI, enable);
}

float CalibrationPresetPage::get_nozzle_value()
{
    double nozzle_value = 0.0;
    wxString nozzle_value_str = m_comboBox_nozzle_dia->GetValue();
    try {
        nozzle_value_str.ToDouble(&nozzle_value);
    }
    catch (...) {
        ;
    }

    return nozzle_value;
}

void CalibrationPresetPage::update(MachineObject* obj)
{
    curr_obj = obj;
    
    //update printer status
    update_show_status();

}

void CalibrationPresetPage::on_device_connected(MachineObject* obj)
{   
    init_with_machine(obj);
    update_combobox_filaments(obj);
}

void CalibrationPresetPage::set_cali_filament_mode(CalibrationFilamentMode mode)
{
    CalibrationWizardPage::set_cali_filament_mode(mode);

    for (int i = 0; i < m_filament_comboBox_list.size(); i++) {
        m_filament_comboBox_list[i]->set_select_mode(mode);
    }

    if (mode == CALI_MODEL_MULITI) {
        m_filament_list_tips->Show();
    }
    else {
        m_filament_list_tips->Hide();
    }
}

void CalibrationPresetPage::set_cali_method(CalibrationMethod method)
{
    CalibrationWizardPage::set_cali_method(method);
    if (method == CalibrationMethod::CALI_METHOD_MANUAL) {
        if (m_cali_mode == CalibMode::Calib_Flow_Rate) {
            wxArrayString steps;
            steps.Add(_L("Preset"));
            steps.Add(_L("Calibration1"));
            steps.Add(_L("Calibration2"));
            steps.Add(_L("Record Factor"));
            m_step_panel->set_steps_string(steps);
            m_step_panel->set_steps(0);
            if (m_cali_stage_panel)
                m_cali_stage_panel->Show();

            if (m_pa_cali_method_combox)
                m_pa_cali_method_combox->Show(false);

            if (m_custom_range_panel)
                m_custom_range_panel->Show(false);
        }
        else if (m_cali_mode == CalibMode::Calib_PA_Line || m_cali_mode == CalibMode::Calib_PA_Pattern) {
            if (m_cali_stage_panel)
                m_cali_stage_panel->Show(false);

            if (m_pa_cali_method_combox)
                m_pa_cali_method_combox->Show();

            if (m_custom_range_panel) {
                wxArrayString titles;
                titles.push_back(_L("From k Value"));
                titles.push_back(_L("To k Value"));
                titles.push_back(_L("Step value"));
                m_custom_range_panel->set_titles(titles);

                wxArrayString values;
                values.push_back(wxString::Format(wxT("%.0f"), 0));
                values.push_back(wxString::Format(wxT("%.2f"), 0.05));
                values.push_back(wxString::Format(wxT("%.3f"), 0.005));
                m_custom_range_panel->set_values(values);

                m_custom_range_panel->set_unit("");
                m_custom_range_panel->Show();
            }
        }
    }
    else {
        wxArrayString steps;
        steps.Add(_L("Preset"));
        steps.Add(_L("Calibration"));
        steps.Add(_L("Record Factor"));
        m_step_panel->set_steps_string(steps);
        m_step_panel->set_steps(0);
        if (m_cali_stage_panel)
            m_cali_stage_panel->Show(false);
        if (m_custom_range_panel)
            m_custom_range_panel->Show(false);
        if (m_pa_cali_method_combox)
            m_pa_cali_method_combox->Show(false);
    }
}

void CalibrationPresetPage::on_cali_start_job()
{
    m_sending_panel->reset();
    m_sending_panel->Show();
    Enable_Send_Button(false);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_CALI, false);
    Layout();
    Fit();

    m_stop_update_page_status = true;
}

void CalibrationPresetPage::on_cali_finished_job()
{
    m_sending_panel->reset();
    m_sending_panel->Show(false);
    update_print_status_msg(wxEmptyString, false);
    Enable_Send_Button(true);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_CALI, true);
    Layout();
    Fit();

    m_stop_update_page_status = false;
}

void CalibrationPresetPage::on_cali_cancel_job()
{
    BOOST_LOG_TRIVIAL(info) << "CalibrationWizard::print_job: enter canceled";
    if (CalibUtils::print_worker) {
            BOOST_LOG_TRIVIAL(info) << "calibration_print_job: canceled";
        CalibUtils::print_worker->cancel_all();
        CalibUtils::print_worker->wait_for_idle();
    }

    m_sending_panel->reset();
    m_sending_panel->Show(false);
    update_print_status_msg(wxEmptyString, false);
    Enable_Send_Button(true);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_CALI, true);
    Layout();
    Fit();

    m_stop_update_page_status = false;
}

void CalibrationPresetPage::init_with_machine(MachineObject* obj)
{
    if (!obj) return;

    //set flow ratio calibration type
    m_cali_stage_panel->set_flow_ratio_calibration_type(obj->flow_ratio_calibration_type);
    // set nozzle value from machine
    bool nozzle_is_set = false;
    for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
        if (abs(obj->m_extder_data.extders[0].current_nozzle_diameter - nozzle_diameter_list[i]) < 1e-3) {
            if (m_comboBox_nozzle_dia->GetCount() > i) {
                m_comboBox_nozzle_dia->SetSelection(i);
                nozzle_is_set = true;
            }
        }
    }

    if (nozzle_is_set) {
        wxCommandEvent event(wxEVT_COMBOBOX);
        event.SetEventObject(this);
        wxPostEvent(m_comboBox_nozzle_dia, event);
        m_comboBox_nozzle_dia->SetToolTip(_L("The nozzle diameter has been synchronized from the printer Settings"));
    } else {
        m_comboBox_nozzle_dia->SetToolTip(wxEmptyString);
        // set default to 0.4
        if (m_comboBox_nozzle_dia->GetCount() > NOZZLE_LIST_DEFAULT)
            m_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);
    }

    // set bed type collection from machine
    //if (m_cali_mode == CalibMode::Calib_PA_Line)
    //    update_plate_type_collection(m_cali_method);

    // init default for filament source
    // TODO if user change ams/ext, need to update
    if ( !obj->has_ams() || (obj->m_tray_now == std::to_string(VIRTUAL_TRAY_ID)) )
    {
        m_ext_spool_radiobox->SetValue(true);
        m_ams_radiobox->SetValue(false);
        wxCommandEvent event(wxEVT_RADIOBUTTON);
        event.SetEventObject(this);
        wxPostEvent(this->m_ext_spool_radiobox, event);
    }
    else {
        m_ams_radiobox->SetValue(true);
        m_ext_spool_radiobox->SetValue(false);
        wxCommandEvent event(wxEVT_RADIOBUTTON);
        event.SetEventObject(this);
        wxPostEvent(this->m_ams_radiobox, event);
    }
    Layout();

    // init filaments for calibration
    sync_ams_info(obj);
}

void CalibrationPresetPage::sync_ams_info(MachineObject* obj)
{
    if (!obj) return;

    std::map<int, DynamicPrintConfig> old_full_filament_ams_list = wxGetApp().sidebar().build_filament_ams_list(obj);
    std::map<int, DynamicPrintConfig> full_filament_ams_list;
    for (auto ams_item : old_full_filament_ams_list) {
        int key = ams_item.first & 0x0FFFF;
        full_filament_ams_list[key] = std::move(ams_item.second);
    }

    // sync filament_ams_list from obj ams list
    filament_ams_list.clear();
    for (auto& ams_item : obj->amsList) {
        for (auto& tray_item: ams_item.second->trayList) {
            int tray_id = -1;
            if (!tray_item.second->id.empty()) {
                try {
                    tray_id = stoi(tray_item.second->id) + stoi(ams_item.second->id) * 4;
                }
                catch (...) {
                    ;
                }
            }
            auto filament_ams = full_filament_ams_list.find(tray_id);
            if (filament_ams != full_filament_ams_list.end()) {
                filament_ams_list[tray_id] = filament_ams->second;
            }
        }
    }

    // init virtual tray info
    if (full_filament_ams_list.find(VIRTUAL_TRAY_ID) != full_filament_ams_list.end()) {
        filament_ams_list[VIRTUAL_TRAY_ID] = full_filament_ams_list[VIRTUAL_TRAY_ID];
    }


    // update filament from panel, display only obj has ams
    // update multi ams panel, display only obj has multi ams
    if (obj->has_ams()) {
        if (obj->amsList.size() > 1) {
            m_multi_ams_panel->Show();
            on_switch_ams(obj->amsList.begin()->first);
        } else {
            m_multi_ams_panel->Hide();
            if (!obj->amsList.empty())
                update_filament_combobox(obj->amsList.begin()->first);
        }
    }
    else {
        update_filament_combobox();
        m_multi_ams_panel->Hide();
    }

    std::vector<AMSinfo> ams_info;
    for (auto ams = obj->amsList.begin(); ams != obj->amsList.end(); ams++) {
        AMSinfo info;
        info.ams_id = ams->first;
        if (ams->second->is_exists 
            && info.parse_ams_info(obj, ams->second, obj->ams_calibrate_remain_flag, obj->is_support_ams_humidity)) {
            ams_info.push_back(info);
        }
    }
    
    for (auto i = 0; i < m_ams_preview_list.size(); i++) {
        AMSPreview* item = m_ams_preview_list[i];
        if (ams_info.size() > 1) {
            if (i < ams_info.size()) {
                item->Update(ams_info[i]);
                item->Open();
            } else {
                item->Close();
            }
        } else {
            item->Close();
        }
    }

    Layout();
}

void CalibrationPresetPage::select_default_compatible_filament()
{
    if (!curr_obj)
        return;

    if (m_ams_radiobox->GetValue()) {
        std::vector<Preset*> multi_select_filaments;
        for (auto &fcb : m_filament_comboBox_list) {
            if (!fcb->GetRadioBox()->IsEnabled())
                continue;

            Preset* preset = const_cast<Preset *>(fcb->GetComboBox()->get_selected_preset());
            if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_SINGLE) {
                if (preset && is_filaments_compatiable({preset})) {
                    fcb->GetRadioBox()->SetValue(true);
                    wxCommandEvent event(wxEVT_RADIOBUTTON);
                    event.SetEventObject(this);
                    wxPostEvent(fcb->GetRadioBox(), event);
                    Layout();
                    break;
                } else
                    fcb->GetRadioBox()->SetValue(false);
            } else if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_MULITI) {
                if (!preset) {
                    fcb->GetCheckBox()->SetValue(false);
                    continue;
                }
                multi_select_filaments.push_back(preset);
                if (!is_filaments_compatiable(multi_select_filaments)) {
                    multi_select_filaments.pop_back();
                    fcb->GetCheckBox()->SetValue(false);
                }
                else
                    fcb->GetCheckBox()->SetValue(true);

                wxCommandEvent event(wxEVT_CHECKBOX);
                event.SetEventObject(this);
                wxPostEvent(fcb->GetCheckBox(), event);
                Layout();
            }
        }
    }
    else if (m_ext_spool_radiobox->GetValue()){
        Preset *preset = const_cast<Preset *>(m_virtual_tray_comboBox->GetComboBox()->get_selected_preset());
        if (preset && is_filaments_compatiable({preset})) {
            m_virtual_tray_comboBox->GetRadioBox()->SetValue(true);
        } else
            m_virtual_tray_comboBox->GetRadioBox()->SetValue(false);

        wxCommandEvent event(wxEVT_RADIOBUTTON);
        event.SetEventObject(this);
        wxPostEvent(m_virtual_tray_comboBox->GetRadioBox(), event);
        Layout();
    }
    else {
        assert(false);
    }

    check_filament_compatible();
}

std::vector<FilamentComboBox*> CalibrationPresetPage::get_selected_filament_combobox()
{
    std::vector<FilamentComboBox*> fcb_list;

    if (m_ext_spool_radiobox->GetValue()) {
        if (m_ext_spool_panel) {
            if (m_virtual_tray_comboBox->GetRadioBox()->GetValue())
                fcb_list.push_back(m_virtual_tray_comboBox);
        }
    } else if (m_ams_radiobox->GetValue()) {
        if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_MULITI) {
            for (auto& fcb : m_filament_comboBox_list) {
                if (fcb->GetCheckBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
        }
        else if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_SINGLE) {
            for (auto& fcb : m_filament_comboBox_list) {
                if (fcb->GetRadioBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
        }
    } else {
        assert(false);
    }

    return fcb_list;
}

std::map<int, Preset*> CalibrationPresetPage::get_selected_filaments()
{
    std::map<int, Preset*> out;
    std::vector<FilamentComboBox*> fcb_list = get_selected_filament_combobox();

    for (int i = 0; i < fcb_list.size(); i++) {
        Preset* preset = const_cast<Preset*>(fcb_list[i]->GetComboBox()->get_selected_preset());
        // valid tray id
        if (fcb_list[i]->get_tray_id() >= 0) {
            out.emplace(std::make_pair(fcb_list[i]->get_tray_id(), preset));
        }
    }
    

    return out;
}

void CalibrationPresetPage::get_preset_info(float& nozzle_dia, BedType& plate_type)
{
    if (m_comboBox_nozzle_dia->GetSelection() >=0 && m_comboBox_nozzle_dia->GetSelection() < NOZZLE_LIST_COUNT) {
        nozzle_dia = nozzle_diameter_list[m_comboBox_nozzle_dia->GetSelection()];
    } else {
        nozzle_dia = -1.0f;
    }

    if (m_comboBox_bed_type->GetSelection() >= 0)
        plate_type = static_cast<BedType>(m_comboBox_bed_type->GetSelection() + 1);
}

void CalibrationPresetPage::get_cali_stage(CaliPresetStage& stage, float& value)
{
    m_cali_stage_panel->get_cali_stage(stage, value);

    if (stage != CaliPresetStage::CALI_MANUAL_STAGE_2) {
        std::map<int, Preset*> selected_filaments = get_selected_filaments();
        if (!selected_filaments.empty()) {
            const ConfigOptionFloats* flow_ratio_opt = selected_filaments.begin()->second->config.option<ConfigOptionFloats>("filament_flow_ratio");
            if (flow_ratio_opt) {
                m_cali_stage_panel->set_flow_ratio_value(flow_ratio_opt->get_at(0));
                value = flow_ratio_opt->get_at(0);
            }
        }
    }
}

void CalibrationPresetPage::update_filament_combobox(std::string ams_id)
{
    for (auto& fcb : m_filament_comboBox_list) {
        fcb->update_from_preset();
        fcb->set_select_mode(m_cali_filament_mode);
    }

    DynamicPrintConfig empty_config;
    empty_config.set_key_value("filament_id", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("tag_uid", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("filament_type", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("tray_name", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("filament_colour", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("filament_exist", new ConfigOptionBools{ false });

    // update virtual tray combo box
    m_virtual_tray_comboBox->update_from_preset();
    auto it = std::find_if(filament_ams_list.begin(), filament_ams_list.end(), [](auto& entry) {
        return entry.first == VIRTUAL_TRAY_ID;
        });

    if (it != filament_ams_list.end()) {
        m_virtual_tray_comboBox->load_tray_from_ams(VIRTUAL_TRAY_ID, it->second);
    }
    else {
        m_virtual_tray_comboBox->load_tray_from_ams(VIRTUAL_TRAY_ID, empty_config);
    }

    if (filament_ams_list.empty())
        return;

    int ams_id_int = 0;
    try {
        if (!ams_id.empty())
            ams_id_int = stoi(ams_id.c_str());

    } catch (...) {}

    for (int i = 0; i < 4; i++) {
        int tray_index = ams_id_int * 4 + i;

        auto it = std::find_if(filament_ams_list.begin(), filament_ams_list.end(), [tray_index](auto& entry) {
            return entry.first == tray_index;
            });

        if (it != filament_ams_list.end()) {
            m_filament_comboBox_list[i]->load_tray_from_ams(tray_index, it->second);
        }
        else {
            m_filament_comboBox_list[i]->load_tray_from_ams(tray_index, empty_config);
        }
    }
}

Preset* CalibrationPresetPage::get_printer_preset(MachineObject* obj, float nozzle_value)
{
    if (!obj) return nullptr;

    Preset* printer_preset = nullptr;
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    for (auto printer_it = preset_bundle->printers.begin(); printer_it != preset_bundle->printers.end(); printer_it++) {
        // only use system printer preset
        if (!printer_it->is_system) continue;

        ConfigOption* printer_nozzle_opt = printer_it->config.option("nozzle_diameter");
        ConfigOptionFloats* printer_nozzle_vals = nullptr;
        if (printer_nozzle_opt)
            printer_nozzle_vals = dynamic_cast<ConfigOptionFloats*>(printer_nozzle_opt);
        std::string model_id = printer_it->get_current_printer_type(preset_bundle);

        std::string printer_type = obj->printer_type;
        if (obj->is_support_upgrade_kit && obj->installed_upgrade_kit) { printer_type = "C12"; }
        if (model_id.compare(printer_type) == 0
            && printer_nozzle_vals
            && abs(printer_nozzle_vals->get_at(0) - nozzle_value) < 1e-3) {
            printer_preset = &(*printer_it);
        }
    }

    return printer_preset;
}

Preset* CalibrationPresetPage::get_print_preset()
{
    Preset* printer_preset = get_printer_preset(curr_obj, get_nozzle_value());

    Preset* print_preset = nullptr;
    wxArrayString print_items;

    // get default print profile
    std::string default_print_profile_name;
    if (printer_preset && printer_preset->config.has("default_print_profile")) {
        default_print_profile_name = printer_preset->config.opt_string("default_print_profile");
    }

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        for (auto print_it = preset_bundle->prints.begin(); print_it != preset_bundle->prints.end(); print_it++) {
            if (print_it->name == default_print_profile_name) {
                print_preset = &(*print_it);
                BOOST_LOG_TRIVIAL(trace) << "CaliPresetPage: get_print_preset = " << print_preset->name;
            }
        }
    }

    return print_preset;
}

std::string CalibrationPresetPage::get_print_preset_name()
{
    Preset* print_preset = get_print_preset();
    if (print_preset)
        return print_preset->name;
    return "";
}

wxArrayString CalibrationPresetPage::get_custom_range_values()
{
    if (m_custom_range_panel) {
        return m_custom_range_panel->get_values();
    }
    return wxArrayString();
}

CalibMode CalibrationPresetPage::get_pa_cali_method()
{
    if (m_pa_cali_method_combox) {
        int selected_mode = m_pa_cali_method_combox->get_selection();
        if (selected_mode == PA_LINE) {
            return CalibMode::Calib_PA_Line;
        }
        else if (selected_mode == PA_PATTERN) {
            return CalibMode::Calib_PA_Pattern;
        }
    }
    return CalibMode::Calib_PA_Line;
}

MaxVolumetricSpeedPresetPage::MaxVolumetricSpeedPresetPage(
    wxWindow *parent, CalibMode cali_mode, bool custom_range, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : CalibrationPresetPage(parent, cali_mode, custom_range, id, pos, size, style)
{
    if (custom_range && m_custom_range_panel) {
        wxArrayString titles;
        titles.push_back(_L("From Volumetric Speed"));
        titles.push_back(_L("To Volumetric Speed"));
        titles.push_back(_L("Step"));
        m_custom_range_panel->set_titles(titles);

        m_custom_range_panel->set_unit(wxString::FromUTF8("mm³/s"));
    }
}
}}
