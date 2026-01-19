#include <regex>
#include "CalibrationWizardPresetPage.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/Print.hpp"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaBlackList.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevStorage.h"

#define CALIBRATION_LABEL_SIZE wxSize(FromDIP(150), FromDIP(24))
#define SYNC_BUTTON_SIZE (wxSize(FromDIP(50), FromDIP(50)))
#define CALIBRATION_TEXT_INPUT_Y_SIZE FromDIP(20)

#define LEFT_EXTRUDER_ID  1
#define RIGHT_EXTRUDER_ID 0

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

void CaliPresetCaliStagePanel::msw_rescale()
{
    flow_ratio_input->GetTextCtrl()->SetSize(wxSize(-1, CALIBRATION_TEXT_INPUT_Y_SIZE));
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

void CaliPresetCustomRangePanel::msw_rescale()
{
    for (TextInput *value_input : m_value_inputs)
        value_input->GetTextCtrl()->SetSize(wxSize(-1, CALIBRATION_TEXT_INPUT_Y_SIZE));
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
        m_value_inputs[i] = new TextInput(parent, wxEmptyString, _L("\u2103" /* °C */), "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE, 0);
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
    m_nozzle_temp = new TextInput(parent, wxEmptyString, _L("\u2103" /* °C */), "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE, wxTE_READONLY);
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

    m_bed_temp = new Label(parent, _L("- \u2103" /* °C */));
    m_bed_temp->SetFont(Label::Body_12);
    bed_temp_sizer->Add(bed_temp_text, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(10));
    bed_temp_sizer->Add(m_bed_temp, 0, wxALIGN_CENTER);

    auto max_flow_sizer = new wxBoxSizer(wxVERTICAL);
    auto max_flow_text = new Label(parent, _L("Max volumetric speed"));
    max_flow_text->SetFont(Label::Body_12);
    m_max_volumetric_speed = new TextInput(parent, wxEmptyString, _L("mm³"), "", wxDefaultPosition, CALIBRATION_FROM_TO_INPUT_SIZE, wxTE_READONLY);
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
    m_extrder_types.resize(2, ExtruderType::etDirectDrive);
    m_extruder_nozzle_types.resize(2, NozzleVolumeType::nvtHighFlow);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationPresetPage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_ams_sync_button->msw_rescale();
    for (auto& comboBox : m_filament_comboBox_list) {
        comboBox->msw_rescale();
    }

    for (AMSPreview *ams_item : m_main_ams_preview_list)
        ams_item->msw_rescale();
    for (AMSPreview *ams_item : m_deputy_ams_preview_list)
        ams_item->msw_rescale();
    for (AMSPreview *ams_item : m_ams_preview_list)
        ams_item->msw_rescale();

    m_cali_stage_panel->msw_rescale();
    m_custom_range_panel->msw_rescale();
}

void CalibrationPresetPage::on_sys_color_changed()
{
    CalibrationWizardPage::on_sys_color_changed();
    m_ams_sync_button->msw_rescale();
}

int CalibrationPresetPage::get_extruder_id(int ams_id)
{
    if (m_ams_id_to_extruder_id_map.find(ams_id) != m_ams_id_to_extruder_id_map.end()) {
        return m_ams_id_to_extruder_id_map[ams_id];
    }

    return 0;
}

void CalibrationPresetPage::create_selection_panel(wxWindow* parent)
{
    auto panel_sizer = new wxBoxSizer(wxVERTICAL);

    auto sync_button_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_sync = new Button(parent, "", "ams_nozzle_sync");
    m_btn_sync->SetToolTip(_L("Synchronize nozzle and AMS information"));
    m_btn_sync->SetCornerRadius(8);
    StateColor btn_sync_bg_col(std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour("#F8F8F8"), StateColor::Hovered),
                               std::pair<wxColour, int>(wxColour("#F8F8F8"), StateColor::Normal));
    StateColor btn_sync_bd_col(std::pair<wxColour, int>(wxColour("#009688"), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour("#009688"), StateColor::Hovered),
                               std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Normal));
    m_btn_sync->SetBackgroundColor(btn_sync_bg_col);
    m_btn_sync->SetBorderColor(btn_sync_bd_col);
    m_btn_sync->SetCanFocus(false);
    m_btn_sync->SetPaddingSize({FromDIP(6), FromDIP(12)});
    m_btn_sync->SetMinSize(SYNC_BUTTON_SIZE);
    m_btn_sync->SetMaxSize(SYNC_BUTTON_SIZE);
    m_btn_sync->SetVertical();
    m_btn_sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (!curr_obj) {
            MessageDialog msg_dlg(nullptr, _L("Please connect the printer first before synchronizing."), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "CalibrationPresetPage: sync_nozzle_info - machine object status:"
                                << " dev_id = " << curr_obj->get_dev_id()
                                << ", print_type = " << curr_obj->printer_type
                                << ", printer_status = " << curr_obj->print_status
                                << ", cali_finished = " << curr_obj->cali_finished
                                << ", cali_version = " << curr_obj->cali_version
                                << ", cache_flow_ratio = " << curr_obj->cache_flow_ratio
                                << ", sub_task_name = " << curr_obj->subtask_name
                                << ", gcode_file_name = " << curr_obj->m_gcode_file;

        for (const CaliPresetInfo &preset_info : curr_obj->selected_cali_preset) {
            BOOST_LOG_TRIVIAL(info) << "CalibrationPresetPage: sync_nozzle_info - selected preset: "
                                    << "tray_id = " << preset_info.tray_id
                                    << ", nozzle_diameter = " << preset_info.nozzle_diameter
                                    << ", filament_id = " << preset_info.filament_id
                                    << ", settring_id = " << preset_info.setting_id
                                    << ", name = " << preset_info.name;
        }

        for (const auto& extruder : curr_obj->GetExtderSystem()->GetExtruders()) {
            if (extruder.GetNozzleType() == NozzleType::ntUndefine) {
                wxString name = _L("left");
                if (extruder.GetExtId() == 0) { name = _L("right"); }
                wxString msg = wxString::Format(_L("Printer %s nozzle information has not been set. Please configure it before proceeding with the calibration."), name);
                MessageDialog msg_dlg(nullptr, msg, wxEmptyString, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
                break;
            }
        }
        on_device_connected(curr_obj);
    });

    m_sync_button_text = new Label(parent, _L("AMS and nozzle information are synced"));
    m_sync_button_text->SetFont(Label::Head_14);
    m_sync_button_text->Wrap(-1);
    sync_button_sizer->Add(m_btn_sync);
    sync_button_sizer->AddSpacer(FromDIP(20));
    sync_button_sizer->Add(m_sync_button_text, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);
    panel_sizer->Add(sync_button_sizer);
    panel_sizer->AddSpacer(PRESET_GAP);

    // single extruder
    {
        m_single_nozzle_info_panel = new wxPanel(parent);
        m_single_nozzle_info_panel->SetBackgroundColour(*wxWHITE);
        auto single_nozzle_sizer = new wxBoxSizer(wxVERTICAL);
        auto nozzle_combo_text = new Label(m_single_nozzle_info_panel, _L("Nozzle Diameter"));
        nozzle_combo_text->SetFont(Label::Head_14);
        nozzle_combo_text->Wrap(-1);

        single_nozzle_sizer->Add(nozzle_combo_text, 0, wxALL, 0);
        single_nozzle_sizer->AddSpacer(FromDIP(10));
        m_comboBox_nozzle_dia = new ComboBox(m_single_nozzle_info_panel, wxID_ANY, "", wxDefaultPosition, CALIBRATION_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
        single_nozzle_sizer->Add(m_comboBox_nozzle_dia, 0, wxALL, 0);

        m_nozzle_diameter_tips = new Label(m_single_nozzle_info_panel, "");
        m_nozzle_diameter_tips->Hide();
        m_nozzle_diameter_tips->SetFont(Label::Body_13);
        // m_nozzle_diameter_tips->SetForegroundColour(wxColour(100, 100, 100));
        m_nozzle_diameter_tips->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
        single_nozzle_sizer->Add(m_nozzle_diameter_tips, 0, wxALL, 0);
        single_nozzle_sizer->AddSpacer(PRESET_GAP);

        auto nozzle_volume_type_text = new Label(m_single_nozzle_info_panel, _L("Nozzle Flow"));
        nozzle_volume_type_text->SetFont(Label::Head_14);
        nozzle_volume_type_text->Wrap(-1);

        m_comboBox_nozzle_volume = new ComboBox(m_single_nozzle_info_panel, wxID_ANY, "", wxDefaultPosition, CALIBRATION_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
        m_comboBox_nozzle_volume->Disable();

        single_nozzle_sizer->Add(nozzle_volume_type_text, 0, wxALL, 0);
        single_nozzle_sizer->AddSpacer(FromDIP(5));
        single_nozzle_sizer->Add(m_comboBox_nozzle_volume, 0, wxALL, 0);
        single_nozzle_sizer->AddSpacer(FromDIP(5));
        single_nozzle_sizer->AddSpacer(PRESET_GAP);

        m_single_nozzle_info_panel->SetSizer(single_nozzle_sizer);
        panel_sizer->Add(m_single_nozzle_info_panel);
    }

    // multi extruder
    {
        m_multi_nozzle_info_panel = new wxPanel(parent);
        m_multi_nozzle_info_panel->SetBackgroundColour(*wxWHITE);
        auto nozzle_volume_sizer = new wxBoxSizer(wxVERTICAL);
        auto nozzle_info_text = new Label(m_multi_nozzle_info_panel, _L("Nozzle Info"));
        nozzle_info_text->SetFont(Label::Head_14);
        nozzle_info_text->Wrap(-1);
        nozzle_volume_sizer->Add(nozzle_info_text, 0, wxALL, 0);
        //nozzle_volume_sizer->AddSpacer(FromDIP(10));

        wxBoxSizer *      type_sizer  = new wxBoxSizer(wxHORIZONTAL);
        m_left_nozzle_volume_type_sizer  = new wxStaticBoxSizer(wxVERTICAL, m_multi_nozzle_info_panel, _L("Left Nozzle"));
        {
            //wxBoxSizer *nozzle_diameter_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto        nozzle_diameter_text  = new Label(m_multi_nozzle_info_panel, _L("Nozzle Diameter"));
            nozzle_diameter_text->SetFont(Label::Head_14);
            nozzle_diameter_text->Wrap(-1);

            m_left_comboBox_nozzle_dia = new ComboBox(m_multi_nozzle_info_panel, wxID_ANY, "", wxDefaultPosition, CALIBRATION_FILAMENT_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
            m_left_comboBox_nozzle_dia->Disable();

            m_left_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            m_left_nozzle_volume_type_sizer->Add(nozzle_diameter_text, 0, wxLEFT | wxRIGHT, 10);
            m_left_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            m_left_nozzle_volume_type_sizer->Add(m_left_comboBox_nozzle_dia, 0, wxLEFT | wxRIGHT, 10);
            m_left_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));

            //wxBoxSizer *nozzle_volume_sizer   = new wxBoxSizer(wxHORIZONTAL);
            auto        nozzle_volume_type_text = new Label(m_multi_nozzle_info_panel, _L("Nozzle Flow"));
            nozzle_volume_type_text->SetFont(Label::Head_14);
            nozzle_volume_type_text->Wrap(-1);

            m_left_comboBox_nozzle_volume = new ComboBox(m_multi_nozzle_info_panel, wxID_ANY, "", wxDefaultPosition, CALIBRATION_FILAMENT_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
            m_left_comboBox_nozzle_volume->Disable();

            m_left_nozzle_volume_type_sizer->Add(nozzle_volume_type_text, 0, wxLEFT | wxRIGHT, 10);
            m_left_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            m_left_nozzle_volume_type_sizer->Add(m_left_comboBox_nozzle_volume, 0, wxLEFT | wxRIGHT, 10);
            m_left_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));

            //m_left_nozzle_volume_type_sizer->Add(nozzle_diameter_sizer);
            //m_left_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            //m_left_nozzle_volume_type_sizer->Add(nozzle_volume_sizer);
        }

        m_right_nozzle_volume_type_sizer = new wxStaticBoxSizer(wxVERTICAL, m_multi_nozzle_info_panel, _L("Right Nozzle"));
        {
            //wxBoxSizer *nozzle_diameter_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto        nozzle_diameter_text  = new Label(m_multi_nozzle_info_panel, _L("Nozzle Diameter"));
            nozzle_diameter_text->SetFont(Label::Head_14);
            nozzle_diameter_text->Wrap(-1);

            m_right_comboBox_nozzle_dia = new ComboBox(m_multi_nozzle_info_panel, wxID_ANY, "", wxDefaultPosition, CALIBRATION_FILAMENT_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
            m_right_comboBox_nozzle_dia->Disable();

            m_right_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            m_right_nozzle_volume_type_sizer->Add(nozzle_diameter_text, 0, wxLEFT | wxRIGHT, 10);
            m_right_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            m_right_nozzle_volume_type_sizer->Add(m_right_comboBox_nozzle_dia, 0, wxLEFT | wxRIGHT, 10);
            m_right_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));

            //wxBoxSizer *nozzle_volume_sizer     = new wxBoxSizer(wxHORIZONTAL);
            auto        nozzle_volume_type_text = new Label(m_multi_nozzle_info_panel, _L("Nozzle Flow"));
            nozzle_volume_type_text->SetFont(Label::Head_14);
            nozzle_volume_type_text->Wrap(-1);

            m_right_comboBox_nozzle_volume = new ComboBox(m_multi_nozzle_info_panel, wxID_ANY, "", wxDefaultPosition, CALIBRATION_FILAMENT_COMBOX_SIZE, 0, nullptr, wxCB_READONLY);
            m_right_comboBox_nozzle_volume->Disable();

            m_right_nozzle_volume_type_sizer->Add(nozzle_volume_type_text, 0, wxLEFT | wxRIGHT, 10);
            m_right_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            m_right_nozzle_volume_type_sizer->Add(m_right_comboBox_nozzle_volume, 0, wxLEFT | wxRIGHT, 10);
            m_right_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));

            //m_right_nozzle_volume_type_sizer->Add(nozzle_diameter_sizer);
            //m_right_nozzle_volume_type_sizer->AddSpacer(FromDIP(5));
            //m_right_nozzle_volume_type_sizer->Add(nozzle_volume_sizer);
        }

        type_sizer->Add(m_left_nozzle_volume_type_sizer, 1, wxEXPAND | wxRIGHT, 10);
        type_sizer->Add(m_right_nozzle_volume_type_sizer, 1, wxEXPAND, 0);

        nozzle_volume_sizer->Add(type_sizer);
        nozzle_volume_sizer->AddSpacer(PRESET_GAP);

        m_multi_nozzle_info_panel->SetSizer(nozzle_volume_sizer);
        panel_sizer->Add(m_multi_nozzle_info_panel);

        m_multi_nozzle_info_panel->Hide();
    }

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
    // init nozzle diameter and nozzle volume
    {
        m_comboBox_nozzle_dia->Clear();
        for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
            m_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f mm", nozzle_diameter_list[i]));
        }
        m_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);

        m_comboBox_nozzle_volume->Clear();
        const ConfigOptionDef *nozzle_volume_type_def = print_config_def.get("nozzle_volume_type");
        if (nozzle_volume_type_def && nozzle_volume_type_def->enum_keys_map) {
            for (auto item : nozzle_volume_type_def->enum_labels) {
                m_comboBox_nozzle_volume->AppendString(_L(item));
            }
        }

        m_comboBox_nozzle_volume->SetSelection(int(NozzleVolumeType::nvtStandard));
    }

    Preset* cur_printer_preset = get_printer_preset(curr_obj, 0.4);

    m_comboBox_bed_type->Clear();
    m_displayed_bed_types.clear();

    // init plate type
    int curr_selection = 0;
    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    if (bed_type_def && bed_type_def->enum_keys_map) {
        int select_index = 0;
        int bed_type_value = 0;
        for (auto item : bed_type_def->enum_labels) {
            bed_type_value++;
            if (cur_printer_preset) {
                const VendorProfile::PrinterModel *pm = PresetUtils::system_printer_model(*cur_printer_preset);
                if (pm) {
                    bool find = std::find(pm->not_support_bed_types.begin(), pm->not_support_bed_types.end(), item) != pm->not_support_bed_types.end();
                    if (find) {
                        continue;
                    }
                }
            }

            if (item == "Textured PEI Plate") {
                curr_selection = select_index;
            }

            m_comboBox_bed_type->AppendString(_L(item));
            m_displayed_bed_types.emplace_back(BedType(bed_type_value));
            select_index++;
        }
        m_comboBox_bed_type->SetSelection(curr_selection);
    }

    // left
    {
        m_left_comboBox_nozzle_dia->Clear();
        for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
            m_left_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f mm", nozzle_diameter_list[i]));
        }
        m_left_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);

        m_left_comboBox_nozzle_volume->Clear();
        const ConfigOptionDef *nozzle_volume_type_def = print_config_def.get("nozzle_volume_type");
        if (nozzle_volume_type_def && nozzle_volume_type_def->enum_keys_map) {
            for (auto item : nozzle_volume_type_def->enum_labels) {
                m_left_comboBox_nozzle_volume->AppendString(_L(item));
            }
        }

        m_left_comboBox_nozzle_volume->SetSelection(int(NozzleVolumeType::nvtStandard));
    }

    // right
    {
        m_right_comboBox_nozzle_dia->Clear();
        for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
            m_right_comboBox_nozzle_dia->AppendString(wxString::Format("%1.1f mm", nozzle_diameter_list[i]));
        }
        m_right_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);

        m_right_comboBox_nozzle_volume->Clear();
        const ConfigOptionDef *nozzle_volume_type_def = print_config_def.get("nozzle_volume_type");
        if (nozzle_volume_type_def && nozzle_volume_type_def->enum_keys_map) {
            for (auto item : nozzle_volume_type_def->enum_labels) {
                m_right_comboBox_nozzle_volume->AppendString(_L(item));
            }
        }

        m_right_comboBox_nozzle_volume->SetSelection(int(NozzleVolumeType::nvtStandard));
    }
}

void CalibrationPresetPage::create_filament_list_panel(wxWindow* parent)
{
    auto panel_sizer = new wxBoxSizer(wxVERTICAL);

    m_filament_list_tips = new Label(parent, _L("Tips for calibration material:\n- Materials that can share same hot bed temperature\n- Different filament brand and family (Brand = Bambu, Family = Basic, Matte)"));
    m_filament_list_tips->Hide();
    m_filament_list_tips->SetFont(Label::Body_13);
    m_filament_list_tips->SetForegroundColour(wxColour(145, 145, 145));
    m_filament_list_tips->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    panel_sizer->Add(m_filament_list_tips, 0, wxBOTTOM, FromDIP(10));

    // Preview item
    m_multi_ams_panel = new wxPanel(parent);
    panel_sizer->Add(m_multi_ams_panel);

    auto filament_fgSizer = new wxFlexGridSizer(2, 2, FromDIP(10), CALIBRATION_FGSIZER_HGAP);
    for (int i = 0; i < 4; i++) {
        auto filament_comboBox_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxRadioButton* radio_btn = new wxRadioButton(m_filament_list_panel, wxID_ANY, "");
        CheckBox* check_box = new CheckBox(m_filament_list_panel);
        check_box->SetBackgroundColour(*wxWHITE);
        FilamentComboBox* fcb = new FilamentComboBox(m_filament_list_panel, i);
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

float CalibrationPresetPage::get_nozzle_diameter(int extruder_id) const
{
    float nozzle_dia = -1.f;
    if (!curr_obj)
        return nozzle_dia;

    if (curr_obj->is_multi_extruders()) {
        if (extruder_id == LEFT_EXTRUDER_ID) {
            if (m_left_comboBox_nozzle_dia->GetSelection() >= 0 && m_left_comboBox_nozzle_dia->GetSelection() < NOZZLE_LIST_COUNT) {
                nozzle_dia = nozzle_diameter_list[m_left_comboBox_nozzle_dia->GetSelection()];
            }
        } else if (extruder_id == RIGHT_EXTRUDER_ID) {
            if (m_right_comboBox_nozzle_dia->GetSelection() >= 0 && m_right_comboBox_nozzle_dia->GetSelection() < NOZZLE_LIST_COUNT) {
                nozzle_dia = nozzle_diameter_list[m_right_comboBox_nozzle_dia->GetSelection()];
            }
        }
    }
    else if (m_comboBox_nozzle_dia->GetSelection() >= 0 && m_comboBox_nozzle_dia->GetSelection() < NOZZLE_LIST_COUNT) {
        nozzle_dia = nozzle_diameter_list[m_comboBox_nozzle_dia->GetSelection()];
    }

    return nozzle_dia;
}

NozzleVolumeType CalibrationPresetPage::get_nozzle_volume_type(int extruder_id) const
{
    if (curr_obj) {
        if (curr_obj->is_multi_extruders()) {
            if (extruder_id == LEFT_EXTRUDER_ID) {
                return NozzleVolumeType(m_left_comboBox_nozzle_volume->GetSelection());
            } else if (extruder_id == RIGHT_EXTRUDER_ID) {
                return NozzleVolumeType(m_right_comboBox_nozzle_volume->GetSelection());
            }
        }
        else
            return NozzleVolumeType(m_comboBox_nozzle_volume->GetSelection());
    }
    return NozzleVolumeType::nvtStandard;
}

ExtruderType CalibrationPresetPage::get_extruder_type(int extruder_id) const
{
    if (curr_obj) {
        int extruder_idx = 0;
        if (curr_obj->is_multi_extruders()) {
            if (extruder_id == RIGHT_EXTRUDER_ID) {
                extruder_idx = 1;
            }
        }
        if (m_extrder_types.size() > extruder_idx)
            return m_extrder_types[extruder_idx];
    }
    return ExtruderType::etDirectDrive;
}

wxBoxSizer* CalibrationPresetPage::create_ams_items_sizer(MachineObject* obj, wxPanel* ams_preview_panel, std::vector<AMSPreview*> &ams_preview_list, std::vector<AMSinfo> &ams_info, int nozzle_id){
    /* clear ams_preview_list */
    for (auto &item : ams_preview_list) {
        delete item;
    }
    ams_preview_list.clear();

    /* create ams_preview_list */
    auto ams_items_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (auto &info : ams_info) {
        auto preview_ams_item = new AMSPreview(ams_preview_panel, wxID_ANY, info, info.ams_type);
        preview_ams_item->Update(info);
        preview_ams_item->Open();
        ams_preview_list.push_back(preview_ams_item);
        std::string ams_id = preview_ams_item->get_ams_id();
        bool is_single = !obj->is_multi_extruders();
        preview_ams_item->Bind(wxEVT_LEFT_DOWN, [this, ams_id, nozzle_id, is_single](wxMouseEvent &e) {
            if(is_single){
                update_filament_combobox(ams_id);
            } else{
                update_multi_extruder_filament_combobox(ams_id, nozzle_id);
            }
            e.Skip();
        });
        ams_items_sizer->Add(preview_ams_item, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(6));
    }
    return ams_items_sizer;
}

void CalibrationPresetPage::create_multi_extruder_filament_list_panel(wxWindow *parent)
{
    m_multi_extruder_ams_panel_sizer = new wxBoxSizer(wxVERTICAL);

    m_filament_list_tips = new Label(
        parent,
        _L("Tips for calibration material:\n- Materials that can share same hot bed temperature\n- Different filament brand and family (Brand = Bambu, Family = Basic, Matte)"));
    m_filament_list_tips->Hide();
    m_filament_list_tips->SetFont(Label::Body_13);
    m_filament_list_tips->SetForegroundColour(wxColour(145, 145, 145));
    m_filament_list_tips->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    m_multi_extruder_ams_panel_sizer->Add(m_filament_list_tips, 0, wxBOTTOM, FromDIP(10));

    {
        // 1. Preview item
        m_main_sizer              = new wxStaticBoxSizer(wxVERTICAL, parent, "Main");
        m_main_ams_preview_panel  = new wxPanel(parent);
        m_main_sizer->Add(m_main_ams_preview_panel);

        // 2. AMS item
        m_main_ams_items_sizer = new wxBoxSizer(wxVERTICAL);
        for (int i = 0; i < 4; i++) { // 4 slots
            auto           filament_comboBox_sizer = new wxBoxSizer(wxHORIZONTAL);
            wxRadioButton *radio_btn               = new wxRadioButton(m_multi_exutrder_filament_list_panel, wxID_ANY, "");
            CheckBox *     check_box               = new CheckBox(m_multi_exutrder_filament_list_panel);
            check_box->SetBackgroundColour(*wxWHITE);
            FilamentComboBox *fcb = new FilamentComboBox(m_multi_exutrder_filament_list_panel, i + 4);
            fcb->SetRadioBox(radio_btn);
            fcb->SetCheckBox(check_box);
            fcb->set_select_mode(CalibrationFilamentMode::CALI_MODEL_SINGLE);
            filament_comboBox_sizer->Add(radio_btn, 0, wxALIGN_CENTER);
            filament_comboBox_sizer->Add(check_box, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
            filament_comboBox_sizer->Add(fcb, 0, wxALIGN_CENTER);
            m_main_ams_items_sizer->Add(filament_comboBox_sizer, 0);

            fcb->Bind(EVT_CALI_TRAY_CHANGED, &CalibrationPresetPage::on_select_tray, this);
            m_main_filament_comboBox_list.emplace_back(fcb);

            radio_btn->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &evt) {
                wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
                event.SetEventObject(this);
                wxPostEvent(this, event);
            });
            check_box->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
                wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
                event.SetEventObject(this);
                wxPostEvent(this, event);
                evt.Skip();
            });

            if (i == 0)
                radio_btn->SetValue(true);
        }

        m_main_sizer->Add(m_main_ams_items_sizer, 1, wxEXPAND | wxALL, 10);
    }

    {
        // 1. Preview item
        m_deputy_sizer             = new wxStaticBoxSizer(wxVERTICAL, parent, "Deputy");
        m_deputy_ams_preview_panel = new wxPanel(parent);
        m_deputy_sizer->Add(m_deputy_ams_preview_panel);

        // 2. AMS item
        m_deputy_ams_items_sizer = new wxBoxSizer(wxVERTICAL);
        for (int i = 0; i < 4; ++i) {  // 4 slots
            auto           filament_comboBox_sizer = new wxBoxSizer(wxHORIZONTAL);
            wxRadioButton *radio_btn               = new wxRadioButton(m_multi_exutrder_filament_list_panel, wxID_ANY, "");
            CheckBox *     check_box               = new CheckBox(m_multi_exutrder_filament_list_panel);
            check_box->SetBackgroundColour(*wxWHITE);
            FilamentComboBox *fcb = new FilamentComboBox(m_multi_exutrder_filament_list_panel, i);
            fcb->SetRadioBox(radio_btn);
            fcb->SetCheckBox(check_box);
            fcb->set_select_mode(CalibrationFilamentMode::CALI_MODEL_SINGLE);
            filament_comboBox_sizer->Add(radio_btn, 0, wxALIGN_CENTER);
            filament_comboBox_sizer->Add(check_box, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
            filament_comboBox_sizer->Add(fcb, 0, wxALIGN_CENTER);
            m_deputy_ams_items_sizer->Add(filament_comboBox_sizer, 0);

            fcb->Bind(EVT_CALI_TRAY_CHANGED, &CalibrationPresetPage::on_select_tray, this);
            m_deputy_filament_comboBox_list.emplace_back(fcb);

            radio_btn->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &evt) {
                wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
                event.SetEventObject(this);
                wxPostEvent(this, event);
            });
            check_box->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
                wxCommandEvent event(EVT_CALI_TRAY_CHANGED);
                event.SetEventObject(this);
                wxPostEvent(this, event);
                evt.Skip();
            });
        }

        m_deputy_sizer->Add(m_deputy_ams_items_sizer, 1, wxEXPAND | wxALL, 10);
    }

    m_multi_exturder_ams_sizer = new wxBoxSizer(wxHORIZONTAL);
    if (m_main_extruder_on_left) {
        m_main_sizer->GetStaticBox()->SetLabel(_L("Left Nozzle"));
        m_deputy_sizer->GetStaticBox()->SetLabel(_L("Right Nozzle"));
        m_multi_exturder_ams_sizer->Add(m_main_sizer, 1, wxALL | wxALIGN_BOTTOM, 10);
        m_multi_exturder_ams_sizer->Add(m_deputy_sizer, 1, wxALL | wxALIGN_BOTTOM, 10);
    }
    else {
        m_main_sizer->GetStaticBox()->SetLabel(_L("Right Nozzle"));
        m_deputy_sizer->GetStaticBox()->SetLabel(_L("Left Nozzle"));
        m_multi_exturder_ams_sizer->Add(m_deputy_sizer, 1, wxEXPAND | wxALL | wxALIGN_BOTTOM, 10);
        m_multi_exturder_ams_sizer->Add(m_main_sizer, 1, wxEXPAND | wxALL | wxALIGN_BOTTOM, 10);
    }
    m_multi_extruder_ams_panel_sizer->Add(m_multi_exturder_ams_sizer);

    parent->SetSizer(m_multi_extruder_ams_panel_sizer);
    m_multi_extruder_ams_panel_sizer->Fit(parent);
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

    m_multi_exutrder_filament_list_panel = new wxPanel(parent);
    m_multi_exutrder_filament_list_panel->SetBackgroundColour(*wxWHITE);
    create_multi_extruder_filament_list_panel(m_multi_exutrder_filament_list_panel);

    if (m_cali_mode == CalibMode::Calib_PA_Line || m_cali_mode == CalibMode::Calib_PA_Pattern) {
        wxArrayString pa_cali_modes;
        pa_cali_modes.push_back(_L("Line"));
        pa_cali_modes.push_back(_L("Pattern"));
        m_pa_cali_method_combox = new CaliComboBox(parent, _L("Method"), pa_cali_modes);
    }
    
    m_warning_panel = new CaliPresetWarningPanel(parent);

    m_tips_panel = new CaliPresetTipsPanel(parent);

    m_sending_panel = new CaliPageSendingPanel(parent);
    m_sending_panel->get_sending_progress_bar()->set_cancel_callback_fina([this]() {
        on_cali_cancel_job();
        });
    m_sending_panel->Bind(EVT_SHOW_ERROR_FAIL_SEND, [this](auto &event){on_cali_cancel_job();});
    m_sending_panel->Hide();

    m_custom_range_panel = new CaliPresetCustomRangePanel(parent);

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_PRESET);

    m_statictext_printer_msg = new Label(this, wxEmptyString, wxALIGN_CENTER_HORIZONTAL);
    m_statictext_printer_msg->SetFont(::Label::Body_13);
    m_statictext_printer_msg->Hide();

    m_top_sizer->Add(m_selection_panel, 0);
    m_top_sizer->Add(m_filament_list_panel, 0);
    m_top_sizer->Add(m_multi_exutrder_filament_list_panel, 0);
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

void CalibrationPresetPage::on_select_nozzle_volume_type(wxCommandEvent &evt, size_t extruder_id)
{
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
    Layout();
}

void CalibrationPresetPage::on_choose_ext_spool(wxCommandEvent& event)
{
    m_filament_list_panel->Hide();
    m_ams_sync_button->Hide();
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
            const ConfigOptionFloatsNullable* flow_ratio_opt = selected_filament_preset->config.option<ConfigOptionFloatsNullable>("filament_flow_ratio");
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

    std::vector<Preset *> selected_filaments_list;
    for (auto &item : selected_filaments)
        selected_filaments_list.push_back(item.second);

    if (!is_filaments_compatiable(selected_filaments, bed_temp, incompatiable_filament_name, error_tips)) {
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

bool CalibrationPresetPage::is_filaments_compatiable(const std::map<int, Preset*>& prests)
{
    std::string incompatiable_filament_name;
    std::string error_tips;
    int bed_temp = 0;
    return is_filaments_compatiable(prests, bed_temp, incompatiable_filament_name, error_tips);
}

bool CalibrationPresetPage::is_filament_in_blacklist(int tray_id, Preset* preset, std::string& error_tips)
{
    if (!curr_obj)
        return true;

    int ams_id;
    int slot_id;
    int out_tray_id;
    get_tray_ams_and_slot_id(curr_obj, tray_id, ams_id, slot_id, out_tray_id);

    if (wxGetApp().app_config->get("skip_ams_blacklist_check") != "true") {
        bool in_blacklist = false;
        std::string action;
        wxString info;
        std::string filamnt_type;
        preset->get_filament_type(filamnt_type);

        auto vendor = dynamic_cast<ConfigOptionStrings*> (preset->config.option("filament_vendor"));
        if (vendor && (vendor->values.size() > 0)) {
            std::string vendor_name = vendor->values[0];
            DevFilaBlacklist::check_filaments_in_blacklist(curr_obj->printer_type, vendor_name, filamnt_type, preset->filament_id, ams_id, slot_id, "", in_blacklist, action, info);
        }

        if (in_blacklist) {
            error_tips = info.ToUTF8().data();
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
    if (devPrinterUtil::IsVirtualSlot(ams_id)) {
        if (m_cali_mode == CalibMode::Calib_PA_Line && (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO || m_cali_method == CalibrationMethod::CALI_METHOD_NEW_AUTO)) {
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

bool CalibrationPresetPage::is_filaments_compatiable(const std::map<int, Preset*> &prests,
    int& bed_temp,
    std::string& incompatiable_filament_name,
    std::string& error_tips)
{
    if (prests.empty()) return true;

    bed_temp = 0;
    std::vector<std::string> filament_types;
    for (auto &item : prests) {
        const auto& item_preset = item.second;
        if (!item_preset)
            continue;

        // update bed temperature
        BedType curr_bed_type = BedType(m_displayed_bed_types[m_comboBox_bed_type->GetSelection()]);
        const ConfigOptionInts *opt_bed_temp_ints = item_preset->config.option<ConfigOptionInts>(get_bed_temp_key(curr_bed_type));
        int bed_temp_int = 0;
        if (opt_bed_temp_ints) {
            bed_temp_int = opt_bed_temp_ints->get_at(0);
        }

        if (bed_temp_int <= 0) {
            if (!item_preset->alias.empty())
                incompatiable_filament_name = item_preset->alias;
            else
                incompatiable_filament_name = item_preset->name;

            return false;
        } else {
            // set for first preset
            if (bed_temp == 0)
                bed_temp = bed_temp_int;
        }
        std::string display_filament_type;
        filament_types.push_back(item_preset->config.get_filament_type(display_filament_type, 0));

        // check is it in the filament blacklist
        if (!is_filament_in_blacklist(item.first, item_preset, error_tips))
            return false;
    }

    if (Print::check_multi_filaments_compatibility(filament_types) == FilamentCompatibilityType::HighLowMixed) {
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
    if (!printer_preset)
        return;

    auto opt_extruder_type = printer_preset->config.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (opt_extruder_type) {
        assert(opt_extruder_type->values.size() <= 2);
        for (size_t i = 0; i < opt_extruder_type->values.size(); ++i) {
            m_extrder_types[i] = (ExtruderType)(opt_extruder_type->values[i]);
        }
    }

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
        std::vector<std::string> compatible_machine = obj_->get_compatible_machine();
        vector<std::string>::iterator it = find(compatible_machine.begin(), compatible_machine.end(), source_model);
        if (it == compatible_machine.end()) {
            return true;
        }
    }

    return false;
}

void CalibrationPresetPage::update_sync_button_status()
{
    auto set_status = [this](bool synced) {
        StateColor synced_colour(std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Normal));
        StateColor not_synced_colour(std::pair<wxColour, int>(wxColour("#009688"), StateColor::Normal));
        if (synced) {
            m_btn_sync->SetBorderColor(synced_colour);
            m_btn_sync->SetIcon("ams_nozzle_sync");
            m_sync_button_text->SetLabel(_L("AMS and nozzle information are synced"));
        } else {
            m_btn_sync->SetBorderColor(not_synced_colour);
            m_btn_sync->SetIcon("printer_sync");
            m_sync_button_text->SetLabel(_L("Sync AMS and nozzle information"));
        }
    };

    if (!curr_obj || !curr_obj->is_info_ready()) {
        set_status(false);
        return;
    }

    struct CaliNozzleInfo
    {
        float nozzle_diameter{0.4f};
        int   nozzle_volume_type{0};

        bool operator==(const CaliNozzleInfo &other) const
        {
            return abs(nozzle_diameter - other.nozzle_diameter) < EPSILON
                && nozzle_volume_type == other.nozzle_volume_type;
        }
    };

    if (curr_obj->is_multi_extruders()) {
        std::vector<CaliNozzleInfo> machine_obj_nozzle_infos;
        machine_obj_nozzle_infos.resize(2);
        for (const DevExtder& extruder : curr_obj->GetExtderSystem()->GetExtruders()) {
            machine_obj_nozzle_infos[extruder.GetExtId()].nozzle_diameter = extruder.GetNozzleDiameter();
            machine_obj_nozzle_infos[extruder.GetExtId()].nozzle_volume_type = int(extruder.GetNozzleFlowType()) - 1;
        }

        std::vector<CaliNozzleInfo> cali_nozzle_infos;
        cali_nozzle_infos.resize(2);
        for (size_t extruder_id = 0; extruder_id < 2; ++extruder_id) {
            cali_nozzle_infos[extruder_id].nozzle_diameter = get_nozzle_diameter(extruder_id);
            cali_nozzle_infos[extruder_id].nozzle_volume_type = int(get_nozzle_volume_type(extruder_id));
        }

        if (machine_obj_nozzle_infos == cali_nozzle_infos) {
            set_status(true);
        }
        else {
            set_status(false);
        }
    }
    else {
        if (abs(curr_obj->GetExtderSystem()->GetNozzleDiameter(0) - get_nozzle_diameter(0)) < EPSILON) {
            set_status(true);
        }
        else {
            set_status(false);
        }
    }
}

void CalibrationPresetPage::update_show_status()
{
    NetworkAgent* agent = Slic3r::GUI::wxGetApp().getAgent();
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!agent) {return;}
    if (!dev) return;

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
            show_status(CaliPresetPageStatus::CaliPresetStatusConnectingServer);
            return;
        }
    }

    if (wxGetApp().app_config) {
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

    //if (obj_->is_multi_extruders()) {
    //    float diameter = obj_->m_extder_data.extders[0].current_nozzle_diameter;
    //    bool  is_same_diameter = std::all_of(obj_->m_extder_data.extders.begin(), obj_->m_extder_data.extders.end(),
    //       [diameter](const Extder& extruder) {
    //            return std::fabs(extruder.current_nozzle_diameter - diameter) < EPSILON;
    //       });
    //    if (!is_same_diameter) {
    //        show_status(CaliPresetPageStatus::CaliPresetStatusDifferentNozzleDiameters);
    //        return;
    //    }
    //}

    // check sdcard when if lan mode printer
    if (obj_->is_lan_mode_printer()) {
        if (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD) {
            show_status(CaliPresetPageStatus::CaliPresetStatusLanModeNoSdcard);
            return;
        } else if (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL ||
                   obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_READONLY) {
            show_status(CaliPresetPageStatus::CaliPresetStatusLanModeSDcardNotAvailable);
            return;
        }
    }
    else if (!obj_->GetConfig()->SupportPrintWithoutSD() && (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD))
    {
        show_status(CaliPresetPageStatus::CaliPresetStatusNoSdcard);
        return;
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
        wxString msg_text = _L("No login account, only printers in LAN mode are displayed.");
        update_print_status_msg(msg_text, false);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInvalidPrinter) {
        update_print_status_msg(wxEmptyString, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusConnectingServer) {
        wxString msg_text = _L("Connecting to server...");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInUpgrading) {
        wxString msg_text = _L("Cannot send a print job while the printer is updating firmware.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInSystemPrinting) {
        wxString msg_text = _L("The printer is executing instructions. Please restart printing after it ends.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusInPrinting) {
        wxString msg_text = _L("The printer is busy with another print job.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusLanModeNoSdcard) {
        wxString msg_text = _L("Storage needs to be inserted before printing via LAN.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusLanModeSDcardNotAvailable) {
        wxString msg_text = _L("Storage is not available or is in read-only mode.");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusNoSdcard) {
        wxString msg_text = _L("Storage needs to be inserted before printing.");
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
        wxString msg_text = _L("Connecting to printer...");
        update_print_status_msg(msg_text, true);
        Enable_Send_Button(false);
    }
    else if (status == CaliPresetPageStatus::CaliPresetStatusDifferentNozzleDiameters) {
        wxString msg_text = _L("Calibration only supports cases where the left and right nozzle diameters are identical.");
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

    update_sync_button_status();
}

void CalibrationPresetPage::on_device_connected(MachineObject* obj)
{
    init_selection_values();
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

    auto get_nozzle_diameter_list_index = [&obj](int extruder_id) -> int {
        for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
            if (abs(obj->GetExtderSystem()->GetNozzleDiameter(extruder_id) - nozzle_diameter_list[i]) < 1e-3) {
                return i;
            }
        }
        return -1;
    };

    //set flow ratio calibration type
    m_cali_stage_panel->set_flow_ratio_calibration_type(obj->flow_ratio_calibration_type);
    // set nozzle value from machine
    bool nozzle_is_set = false;
    for (int i = 0; i < NOZZLE_LIST_COUNT; i++) {
        if (abs(obj->GetExtderSystem()->GetNozzleDiameter(0) - nozzle_diameter_list[i]) < 1e-3) {
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

    if (obj->is_multi_extruders()) {
        for (size_t i = 0; i < obj->GetExtderSystem()->GetTotalExtderCount(); ++i) {
            if (i == LEFT_EXTRUDER_ID) {
                int index = get_nozzle_diameter_list_index(LEFT_EXTRUDER_ID);
                if ((index != -1) && m_left_comboBox_nozzle_dia->GetCount() > index) {
                    m_left_comboBox_nozzle_dia->SetSelection(index);
                    wxCommandEvent event(wxEVT_COMBOBOX);
                    event.SetEventObject(this);
                    wxPostEvent(m_left_comboBox_nozzle_dia, event);
                    m_left_comboBox_nozzle_dia->SetToolTip(_L("The nozzle diameter has been synchronized from the printer Settings"));
                }
                else {
                    m_left_comboBox_nozzle_dia->SetToolTip(wxEmptyString);
                    if (m_left_comboBox_nozzle_dia->GetCount() > NOZZLE_LIST_DEFAULT)
                        m_left_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);
                }

                if (obj->GetExtderSystem()->GetNozzleFlowType(i) != NozzleFlowType::NONE_FLOWTYPE) {
                    m_left_comboBox_nozzle_volume->SetSelection(obj->GetExtderSystem()->GetNozzleFlowType(i) - 1);
                } else {
                    m_left_comboBox_nozzle_volume->SetSelection(0);
                }
            }
            else if (i == RIGHT_EXTRUDER_ID) {
                int index = get_nozzle_diameter_list_index(RIGHT_EXTRUDER_ID);
                if ((index != -1) && m_right_comboBox_nozzle_dia->GetCount() > index) {
                    m_right_comboBox_nozzle_dia->SetSelection(index);
                    wxCommandEvent event(wxEVT_COMBOBOX);
                    event.SetEventObject(this);
                    wxPostEvent(m_right_comboBox_nozzle_dia, event);
                    m_right_comboBox_nozzle_dia->SetToolTip(_L("The nozzle diameter has been synchronized from the printer Settings"));
                } else {
                    m_right_comboBox_nozzle_dia->SetToolTip(wxEmptyString);
                    if (m_right_comboBox_nozzle_dia->GetCount() > NOZZLE_LIST_DEFAULT)
                        m_right_comboBox_nozzle_dia->SetSelection(NOZZLE_LIST_DEFAULT);
                }

                if (obj->GetExtderSystem()->GetNozzleFlowType(i) != NozzleFlowType::NONE_FLOWTYPE) {
                    m_right_comboBox_nozzle_volume->SetSelection(obj->GetExtderSystem()->GetNozzleFlowType(i) - 1);
                } else {
                    m_right_comboBox_nozzle_volume->SetSelection(0);
                }
            }
        }

        if (!obj->is_main_extruder_on_left() && m_main_extruder_on_left) {
            m_multi_exturder_ams_sizer->Detach(m_main_sizer);
            m_multi_exturder_ams_sizer->Detach(m_deputy_sizer);

            m_main_sizer->GetStaticBox()->SetLabel(_L("Right Nozzle"));
            m_deputy_sizer->GetStaticBox()->SetLabel(_L("Left Nozzle"));
            m_multi_exturder_ams_sizer->Add(m_deputy_sizer, 1, wxEXPAND | wxALL | wxALIGN_BOTTOM, 10);
            m_multi_exturder_ams_sizer->Add(m_main_sizer, 1, wxEXPAND | wxALL | wxALIGN_BOTTOM, 10);

            m_main_extruder_on_left = false;
        }
        else if (obj->is_main_extruder_on_left() && !m_main_extruder_on_left) {
            m_multi_exturder_ams_sizer->Detach(m_main_sizer);
            m_multi_exturder_ams_sizer->Detach(m_deputy_sizer);

            m_main_sizer->GetStaticBox()->SetLabel(_L("Left Nozzle"));
            m_deputy_sizer->GetStaticBox()->SetLabel(_L("Right Nozzle"));
            m_multi_exturder_ams_sizer->Add(m_main_sizer, 1, wxEXPAND | wxALL | wxALIGN_BOTTOM, 10);
            m_multi_exturder_ams_sizer->Add(m_deputy_sizer, 1, wxEXPAND | wxALL | wxALIGN_BOTTOM, 10);

            m_main_extruder_on_left = true;
        }

        m_single_nozzle_info_panel->Hide();
        m_multi_nozzle_info_panel->Show();
        m_multi_exutrder_filament_list_panel->Show();
        m_filament_list_panel->Hide();
    }
    else {
        if ((obj->GetExtderSystem()->GetTotalExtderCount() > 0) && (obj->GetExtderSystem()->GetNozzleFlowType(0) != NozzleFlowType::NONE_FLOWTYPE))
        {
            m_comboBox_nozzle_volume->SetSelection(obj->GetExtderSystem()->GetNozzleFlowType(0) - 1);
        } else {
            m_comboBox_nozzle_volume->SetSelection(0);
        }

        m_single_nozzle_info_panel->Show();
        m_multi_nozzle_info_panel->Hide();
        m_multi_exutrder_filament_list_panel->Hide();
        m_filament_list_panel->Show();
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
        if (key == VIRTUAL_TRAY_MAIN_ID || key == VIRTUAL_TRAY_DEPUTY_ID) {
            ams_item.second.set_key_value("filament_exist", new ConfigOptionBools{true});
        }
        full_filament_ams_list[key] = std::move(ams_item.second);
    }

    // sync filament_ams_list from obj ams list
    filament_ams_list.clear();
    for (auto& ams_item : obj->GetFilaSystem()->GetAmsList()) {
        for (auto& tray_item: ams_item.second->GetTrays()) {
            int tray_id = -1;
            if (!tray_item.second->id.empty()) {
                try {
                    tray_id = stoi(tray_item.second->id) + stoi(ams_item.second->GetAmsId()) * 4;
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
    if (full_filament_ams_list.find(VIRTUAL_TRAY_MAIN_ID) != full_filament_ams_list.end()) {
        filament_ams_list[VIRTUAL_TRAY_MAIN_ID] = full_filament_ams_list[VIRTUAL_TRAY_MAIN_ID];
    }

    if (full_filament_ams_list.find(VIRTUAL_TRAY_DEPUTY_ID) != full_filament_ams_list.end()) {
        filament_ams_list[VIRTUAL_TRAY_DEPUTY_ID] = full_filament_ams_list[VIRTUAL_TRAY_DEPUTY_ID];
    }

    // update filament from panel, display only obj has ams
    // update multi ams panel, display only obj has multi ams
    if (obj->HasAms()) {
        if (obj->is_multi_extruders()) {
            bool main_done   = false;
            bool deputy_done = false;
            for (auto &ams_item : obj->GetFilaSystem()->GetAmsList()) {
                if (ams_item.second->GetExtruderId() == 0 && !main_done) {
                    update_multi_extruder_filament_combobox(ams_item.second->GetAmsId(), ams_item.second->GetExtruderId());
                    main_done = true;
                } else if (ams_item.second->GetExtruderId() == 1 && !deputy_done) {
                    update_multi_extruder_filament_combobox(ams_item.second->GetAmsId(), ams_item.second->GetExtruderId());
                    deputy_done = true;
                }
            }

            if (!main_done)
                update_multi_extruder_filament_combobox(std::to_string(VIRTUAL_TRAY_MAIN_ID), 0);

            if (!deputy_done)
                update_multi_extruder_filament_combobox(std::to_string(VIRTUAL_TRAY_DEPUTY_ID), 1);
        }
        else {
            if (obj->GetFilaSystem()->GetAmsList().size() > 1) {
                on_switch_ams(obj->GetFilaSystem()->GetAmsList().begin()->first);
            } else {
                if (!obj->GetFilaSystem()->GetAmsList().empty())
                    update_filament_combobox(obj->GetFilaSystem()->GetAmsList().begin()->first);
            }
        }
    }
    else {
        if (obj->is_multi_extruders()) {
            update_multi_extruder_filament_combobox(std::to_string(VIRTUAL_TRAY_MAIN_ID), 0);
            update_multi_extruder_filament_combobox(std::to_string(VIRTUAL_TRAY_DEPUTY_ID), 1);
        } else {
            update_filament_combobox(std::to_string(VIRTUAL_TRAY_MAIN_ID));
        }
    }

    m_ams_id_to_extruder_id_map.clear();
    std::vector<AMSinfo> ams_info;
    std::vector<AMSinfo> main_ams_info;
    std::vector<AMSinfo> deputy_ams_info;

    const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
    for (auto ams = ams_list.begin(); ams != ams_list.end(); ams++) {
        AMSinfo info;
        info.ams_id = ams->first;
        if (ams->second->IsExist()
            && info.parse_ams_info(obj, ams->second, obj->GetFilaSystem()->IsDetectRemainEnabled(), obj->is_support_ams_humidity)) {
            ams_info.push_back(info);
            if (info.nozzle_id == 0) {
                main_ams_info.push_back(info);
            } else {
                deputy_ams_info.push_back(info);
            }
        }
        m_ams_id_to_extruder_id_map[stoi(info.ams_id)] = info.nozzle_id;
    }
    if (obj->is_multi_extruders()) {
        m_ams_id_to_extruder_id_map[VIRTUAL_TRAY_MAIN_ID] = 0;
        m_ams_id_to_extruder_id_map[VIRTUAL_TRAY_DEPUTY_ID] = 1;
    }

    /* add vt_ams info to ams info list*/
    for (const DevAmsTray& vt_tray : obj->vt_slot) {
        AMSinfo     info;
        info.parse_ext_info(obj, vt_tray);
        info.ams_type = AMSModel::EXT_AMS;

        if (vt_tray.id == std::to_string(VIRTUAL_TRAY_MAIN_ID)) {
            ams_info.push_back(info);
            main_ams_info.push_back(info);
        } else if (vt_tray.id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
            deputy_ams_info.push_back(info);
        } else {
            assert(false);
        }
    }

    /* update ams preview */
    auto ams_items_sizer = create_ams_items_sizer(obj, m_multi_ams_panel, m_ams_preview_list, ams_info, MAIN_EXTRUDER_ID);
    auto multi_ams_sizer = new wxBoxSizer(wxVERTICAL);
    multi_ams_sizer->Add(ams_items_sizer, 0);
    multi_ams_sizer->AddSpacer(FromDIP(10));
    m_multi_ams_panel->SetSizer(multi_ams_sizer);

    m_main_ams_preview_panel->SetSizer(create_ams_items_sizer(obj, m_main_ams_preview_panel, m_main_ams_preview_list, main_ams_info, MAIN_EXTRUDER_ID));
    m_deputy_ams_preview_panel->SetSizer(create_ams_items_sizer(obj, m_deputy_ams_preview_panel, m_deputy_ams_preview_list, deputy_ams_info, DEPUTY_EXTRUDER_ID));

    Layout();
}

void CalibrationPresetPage::select_default_compatible_filament()
{
    if (!curr_obj)
        return;

    std::string ams_id;
    for (AMSPreview* ams_perview : m_ams_preview_list) {
        if (ams_perview->IsSelected()) {
            ams_id = ams_perview->get_ams_id();
            break;
        }
    }

    if (ams_id.empty())
        return;

    if (!devPrinterUtil::IsVirtualSlot(ams_id)) {
        std::map<int, Preset *> selected_filament;
        for (size_t i = 0; i < 4; ++i) {
            auto &fcb = m_filament_comboBox_list[i];
            if (!fcb->GetRadioBox()->IsEnabled())
                continue;
            int tray_id = fcb->get_tray_id();
            Preset* preset = const_cast<Preset *>(fcb->GetComboBox()->get_selected_preset());
            if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_SINGLE) {
                selected_filament.clear();
                selected_filament[tray_id] = preset;
                if (preset && is_filaments_compatiable(selected_filament)) {
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
                selected_filament.insert(std::make_pair(tray_id, preset));
                if (!is_filaments_compatiable(selected_filament)) {
                    selected_filament.erase(tray_id);
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
    else {
        std::map<int, Preset *> selected_filament;
        Preset  *preset  = const_cast<Preset *>(m_filament_comboBox_list[0]->GetComboBox()->get_selected_preset());
        selected_filament[m_filament_comboBox_list[0]->get_tray_id()] = preset;
        if (preset && is_filaments_compatiable(selected_filament)) {
            m_filament_comboBox_list[0]->GetRadioBox()->SetValue(true);
        } else
            m_filament_comboBox_list[0]->GetRadioBox()->SetValue(false);

        wxCommandEvent event(wxEVT_RADIOBUTTON);
        event.SetEventObject(this);
        wxPostEvent(m_filament_comboBox_list[0]->GetRadioBox(), event);
        Layout();
    }

    check_filament_compatible();
}

int CalibrationPresetPage::get_index_by_tray_id(int tray_id)
{
    std::vector<FilamentComboBox*> fcb_list = get_selected_filament_combobox();
    for (auto fcb : fcb_list) {
        if (fcb->get_tray_id() == tray_id) {
            return fcb->get_index();
        }
    }
    return -1;
}

std::vector<FilamentComboBox*> CalibrationPresetPage::get_selected_filament_combobox()
{
    std::vector<FilamentComboBox*> fcb_list;

    if (curr_obj && curr_obj->is_multi_extruders()) {
        if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_MULITI) {
            for (auto &fcb : m_main_filament_comboBox_list) {
                if (fcb->GetCheckBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
            for (auto &fcb : m_deputy_filament_comboBox_list) {
                if (fcb->GetCheckBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
        } else if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_SINGLE) {
            for (auto &fcb : m_main_filament_comboBox_list) {
                if (fcb->GetRadioBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
            for (auto &fcb : m_deputy_filament_comboBox_list) {
                if (fcb->GetRadioBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
        }
    }
    else {
        if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_MULITI) {
            for (auto &fcb : m_filament_comboBox_list) {
                if (fcb->GetCheckBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
        } else if (m_cali_filament_mode == CalibrationFilamentMode::CALI_MODEL_SINGLE) {
            for (auto &fcb : m_filament_comboBox_list) {
                if (fcb->GetRadioBox()->GetValue()) {
                    fcb_list.push_back(fcb);
                }
            }
        }
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
        plate_type = static_cast<BedType>(m_displayed_bed_types[m_comboBox_bed_type->GetSelection()]);
}

void CalibrationPresetPage::get_cali_stage(CaliPresetStage& stage, float& value)
{
    m_cali_stage_panel->get_cali_stage(stage, value);

    if (stage != CaliPresetStage::CALI_MANUAL_STAGE_2) {
        std::map<int, Preset*> selected_filaments = get_selected_filaments();
        if (!selected_filaments.empty()) {
            const ConfigOptionFloatsNullable* flow_ratio_opt = selected_filaments.begin()->second->config.option<ConfigOptionFloatsNullable>("filament_flow_ratio");
            if (flow_ratio_opt) {
                m_cali_stage_panel->set_flow_ratio_value(flow_ratio_opt->get_at(0));
                value = flow_ratio_opt->get_at(0);
            }
        }
    }
}

void CalibrationPresetPage::update_multi_extruder_filament_combobox(const std::string &ams_id, int nozzle_id)
{
    if (nozzle_id == 0) {
        for (auto &fcb : m_main_filament_comboBox_list) {
            fcb->update_from_preset();
            fcb->set_select_mode(m_cali_filament_mode);
        }
        for (auto i = 0; i < m_main_ams_preview_list.size(); i++) {
            AMSPreview *item = m_main_ams_preview_list[i];
            if (item->get_ams_id() == ams_id) {
                item->OnSelected();
            } else {
                item->UnSelected();
            }
        }
    }
    else {
        for (auto &fcb : m_deputy_filament_comboBox_list) {
            fcb->update_from_preset();
            fcb->set_select_mode(m_cali_filament_mode);
        }
        for (auto i = 0; i < m_deputy_ams_preview_list.size(); i++) {
            AMSPreview *item = m_deputy_ams_preview_list[i];
            if (item->get_ams_id() == ams_id) {
                item->OnSelected();
            } else {
                item->UnSelected();
            }
        }
    }

    DynamicPrintConfig empty_config;
    empty_config.set_key_value("filament_id", new ConfigOptionStrings{""});
    empty_config.set_key_value("tag_uid", new ConfigOptionStrings{""});
    empty_config.set_key_value("filament_type", new ConfigOptionStrings{""});
    empty_config.set_key_value("tray_name", new ConfigOptionStrings{""});
    empty_config.set_key_value("filament_colour", new ConfigOptionStrings{""});
    empty_config.set_key_value("filament_exist", new ConfigOptionBools{false});

    if (filament_ams_list.empty()) return;

    int ams_id_int = 0;
    try {
        if (!ams_id.empty())
            ams_id_int = stoi(ams_id.c_str());

    } catch (...) {}

    int item_size = 4;
    if (ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
        item_size = 1;
    }

    if (ams_id_int >= 128 && ams_id_int < 153) {  // N3S
        item_size = 1;
    }

    for (int i = 0; i < 4; i++) {
        if (i < item_size) {
            if (nozzle_id == 0)
                m_main_filament_comboBox_list[i]->ShowPanel();
            else
                m_deputy_filament_comboBox_list[i]->ShowPanel();
        }
        else {
            if (nozzle_id == 0)
                m_main_filament_comboBox_list[i]->HidePanel();
            else
                m_deputy_filament_comboBox_list[i]->HidePanel();
        }

        int tray_index = ams_id_int * 4 + i;
        if (ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
            tray_index = stoi(ams_id);
        }

        auto it = std::find_if(filament_ams_list.begin(), filament_ams_list.end(), [tray_index](auto &entry) {
            return entry.first == tray_index;
        });

        if (nozzle_id == 0) {
            if (m_main_filament_comboBox_list.empty())
                continue;
            if (it != filament_ams_list.end()) {
                m_main_filament_comboBox_list[i]->load_tray_from_ams(tray_index, it->second);
            } else {
                m_main_filament_comboBox_list[i]->load_tray_from_ams(tray_index, empty_config);
            }
        }
        else{
            if (m_deputy_filament_comboBox_list.empty())
                continue;
            if (it != filament_ams_list.end()) {
                m_deputy_filament_comboBox_list[i]->load_tray_from_ams(tray_index, it->second);
            } else {
                m_deputy_filament_comboBox_list[i]->load_tray_from_ams(tray_index, empty_config);
            }
        }
    }
    Layout();
}

void CalibrationPresetPage::update_filament_combobox(std::string ams_id)
{
    for (auto& fcb : m_filament_comboBox_list) {
        fcb->update_from_preset();
        fcb->set_select_mode(m_cali_filament_mode);
    }

    for (auto i = 0; i < m_ams_preview_list.size(); i++) {
        AMSPreview *item = m_ams_preview_list[i];
        if (item->get_ams_id() == ams_id) {
            item->OnSelected();
        } else {
            item->UnSelected();
        }
    }

    DynamicPrintConfig empty_config;
    empty_config.set_key_value("filament_id", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("tag_uid", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("filament_type", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("tray_name", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("filament_colour", new ConfigOptionStrings{ "" });
    empty_config.set_key_value("filament_exist", new ConfigOptionBools{ false });

    if (filament_ams_list.empty())
        return;

    int ams_id_int = 0;
    try {
        if (!ams_id.empty())
            ams_id_int = stoi(ams_id.c_str());

    } catch (...) {}

    int item_size = 4;
    if (ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
        item_size = 1;
    }

    if (ams_id_int >= 128 && ams_id_int < 153) { // N3S
        item_size = 1;
    }

    for (int i = 0; i < 4; i++) {
        if (i < item_size)
            m_filament_comboBox_list[i]->ShowPanel();
        else
            m_filament_comboBox_list[i]->HidePanel();

        int tray_index = ams_id_int * 4 + i;
        if (ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
            tray_index = stoi(ams_id);
        }

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
    Layout();
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
        ConfigOptionFloats *printer_nozzle_vals = nullptr;
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

        m_custom_range_panel->set_unit(_L("mm³/s"));
    }
}
}}
