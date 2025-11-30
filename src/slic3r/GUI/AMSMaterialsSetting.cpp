#include "AMSMaterialsSetting.hpp"
#include "ExtrusionCalibration.hpp"
#include "MsgDialog.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Preset.hpp"
#include "I18N.hpp"
#include <boost/log/trivial.hpp>
#include <wx/colordlg.h>
#include <wx/dcgraph.h>
#include "CalibUtils.hpp"
#include "../Utils/ColorSpaceConvert.hpp"
#include "EncodedFilament.hpp"


#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaBlackList.h"
#include "DeviceCore/DevFilaSystem.h"

#define FILAMENT_MAX_TEMP       300
#define FILAMENT_MIN_TEMP       120

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_SELECTED_COLOR, wxCommandEvent);

static std::string float_to_string_with_precision(float value, int precision = 3)
{
    if (value < 0)
        return std::string();

    std::stringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

AMSMaterialsSetting::AMSMaterialsSetting(wxWindow *parent, wxWindowID id)
    : DPIDialog(parent, id, _L("AMS Materials Setting"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_color_picker_popup(ColorPickerPopup(this))
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void AMSMaterialsSetting::create()
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_panel_normal = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    create_panel_normal(m_panel_normal);
    m_panel_kn = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    create_panel_kn(m_panel_kn);

    wxBoxSizer *m_sizer_button = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_button->Add(0, 0, 1, wxEXPAND, 0);

    m_button_confirm = new Button(this, _L("Confirm"));
    m_button_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_confirm->Bind(wxEVT_BUTTON, &AMSMaterialsSetting::on_select_ok, this);

    m_button_reset = new Button(this, _L("Reset"));
    m_button_reset->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_button_reset->Bind(wxEVT_BUTTON, &AMSMaterialsSetting::on_select_reset, this);

    m_button_close = new Button(this, _L("Close"));
    m_button_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_button_close->Bind(wxEVT_BUTTON, &AMSMaterialsSetting::on_select_close, this);

    m_sizer_button->Add(m_button_confirm, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));
    m_sizer_button->Add(m_button_reset, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));
    m_sizer_button->Add(m_button_close, 0, wxALIGN_CENTER, 0);

    m_sizer_main->Add(m_panel_normal, 0, wxALL, FromDIP(2));

    m_sizer_main->Add(m_panel_kn, 0, wxALL, FromDIP(2));

    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(24));
    m_sizer_main->Add(m_sizer_button, 0,  wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    m_sizer_main->Add(0, 0, 0,  wxTOP, FromDIP(16));

    SetSizer(m_sizer_main);
    Layout();
    Fit();

    m_input_nozzle_min->GetTextCtrl()->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        warning_text->Hide();
        Layout();
        Fit();
        e.Skip();
        });
    m_input_nozzle_min->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) {
        input_min_finish();
        e.Skip();
        });
    m_input_nozzle_min->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        input_min_finish();
        e.Skip();
        });

    m_input_nozzle_max->GetTextCtrl()->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        warning_text->Hide();
        Layout();
        Fit();
        e.Skip();
        });
    m_input_nozzle_max->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) {
        input_max_finish();
        e.Skip();
        });
    m_input_nozzle_max->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        input_max_finish();
        e.Skip();
        });

    Bind(wxEVT_PAINT, &AMSMaterialsSetting::paintEvent, this);
    Bind(EVT_SELECTED_COLOR, &AMSMaterialsSetting::on_picker_color, this);
     m_comboBox_filament->Connect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(AMSMaterialsSetting::on_select_filament), NULL, this);

    m_comboBox_cali_result->Connect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(AMSMaterialsSetting::on_select_cali_result), NULL, this);
}

void AMSMaterialsSetting::create_panel_normal(wxWindow* parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* m_sizer_filament = new wxBoxSizer(wxHORIZONTAL);

    m_title_filament = new wxStaticText(parent, wxID_ANY, _L("Filament"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_filament->SetFont(::Label::Body_13);
    m_title_filament->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_filament->Wrap(-1);
    m_sizer_filament->Add(m_title_filament, 0, wxALIGN_CENTER, 0);

    m_sizer_filament->Add(0, 0, 0, wxEXPAND, 0);

#ifdef __APPLE__
    m_comboBox_filament = new wxComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, 0, nullptr, wxCB_READONLY);
#else
    m_comboBox_filament = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, 0, nullptr, wxCB_READONLY);
#endif

    m_sizer_filament->Add(m_comboBox_filament, 1, wxALIGN_CENTER, 0);

    // make the style the same with disable m_input_k_val, FIXME
    m_readonly_filament = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_readonly_filament->SetBorderColor(StateColor(std::make_pair(0xDBDBDB, (int)StateColor::Focused), std::make_pair(0x009688, (int)StateColor::Hovered),
        std::make_pair(0xDBDBDB, (int)StateColor::Normal)));
    m_readonly_filament->SetFont(::Label::Body_14);
    m_readonly_filament->SetLabelColor(AMS_MATERIALS_SETTING_GREY800);
    m_readonly_filament->GetTextCtrl()->Bind(wxEVT_SET_FOCUS, [](auto& e) {});
    m_readonly_filament->GetTextCtrl()->Hide();
    m_readonly_filament->Disable();
    m_sizer_filament->Add(m_readonly_filament, 1, wxALIGN_CENTER, 0);
    m_readonly_filament->Hide();

    wxBoxSizer* m_sizer_colour = new wxBoxSizer(wxHORIZONTAL);

    m_title_colour = new wxStaticText(parent, wxID_ANY, _L("Color"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_colour->SetFont(::Label::Body_13);
    m_title_colour->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_colour->Wrap(-1);
    m_sizer_colour->Add(m_title_colour, 0, wxALIGN_CENTER, 0);

    m_sizer_colour->Add(0, 0, 0, wxEXPAND, 0);

    m_clr_picker = new ColorPicker(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_clr_picker->set_show_full(true);
    m_clr_picker->SetBackgroundColour(*wxWHITE);


    m_clr_picker->Bind(wxEVT_LEFT_DOWN, &AMSMaterialsSetting::on_clr_picker, this);
    m_sizer_colour->Add(m_clr_picker, 0, 0, 0);
    m_clr_name = new Label(parent, wxEmptyString);
    m_clr_name->SetForegroundColour(*wxBLACK);
    m_clr_name->SetBackgroundColour(*wxWHITE);
    m_clr_name->SetFont(Label::Body_13);
    m_sizer_colour->Add(m_clr_name, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));

    wxBoxSizer* m_sizer_temperature = new wxBoxSizer(wxHORIZONTAL);
    m_title_temperature = new wxStaticText(parent, wxID_ANY, _L("Nozzle\nTemperature"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_temperature->SetFont(::Label::Body_13);
    m_title_temperature->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_temperature->Wrap(-1);
    m_sizer_temperature->Add(m_title_temperature, 0, wxALIGN_CENTER, 0);

    m_sizer_temperature->Add(0, 0, 0, wxEXPAND, 0);

    wxBoxSizer* sizer_other = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* sizer_tempinput = new wxBoxSizer(wxHORIZONTAL);

    m_input_nozzle_max = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_INPUT_SIZE, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_input_nozzle_min = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_INPUT_SIZE, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_input_nozzle_max->Enable(false);
    m_input_nozzle_min->Enable(false);

    m_input_nozzle_max->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_input_nozzle_max->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
    m_input_nozzle_min->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_input_nozzle_min->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));

    degree            = new ScalableBitmap(parent, "degree", 16);
    bitmap_max_degree = new wxStaticBitmap(parent, -1, degree->bmp(), wxDefaultPosition, wxDefaultSize);
    bitmap_min_degree = new wxStaticBitmap(parent, -1, degree->bmp(), wxDefaultPosition, wxDefaultSize);

    sizer_tempinput->Add(m_input_nozzle_max, 1, wxALIGN_CENTER, 0);
    sizer_tempinput->Add(bitmap_min_degree, 0, wxALIGN_CENTER, 0);
    sizer_tempinput->Add(FromDIP(10), 0, 0, 0);
    sizer_tempinput->Add(m_input_nozzle_min, 1, wxALIGN_CENTER, 0);
    sizer_tempinput->Add(bitmap_max_degree, 0, wxALIGN_CENTER, 0);

    wxBoxSizer* sizer_temp_txt = new wxBoxSizer(wxHORIZONTAL);
    auto m_title_max = new wxStaticText(parent, wxID_ANY, _L("max"), wxDefaultPosition, AMS_MATERIALS_SETTING_INPUT_SIZE);
    m_title_max->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_max->SetFont(::Label::Body_13);
    auto m_title_min = new wxStaticText(parent, wxID_ANY, _L("min"), wxDefaultPosition, AMS_MATERIALS_SETTING_INPUT_SIZE);
    m_title_min->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_min->SetFont(::Label::Body_13);
    sizer_temp_txt->Add(m_title_max, 1, wxALIGN_CENTER, 0);
    sizer_temp_txt->Add(FromDIP(10), 0, 0, 0);
    sizer_temp_txt->Add(m_title_min, 1, wxALIGN_CENTER | wxRIGHT, FromDIP(16));


    sizer_other->Add(sizer_temp_txt, 0, wxALIGN_CENTER, 0);
    sizer_other->Add(sizer_tempinput, 0, wxALIGN_CENTER, 0);

    m_sizer_temperature->Add(sizer_other, 0, wxALL | wxALIGN_CENTER, 0);
    m_sizer_temperature->AddStretchSpacer();

    wxString warning_string = wxString::FromUTF8(
        (boost::format(_u8L("The input value should be greater than %1% and less than %2%")) % FILAMENT_MIN_TEMP % FILAMENT_MAX_TEMP).str());
    warning_text = new wxStaticText(parent, wxID_ANY, warning_string, wxDefaultPosition, wxDefaultSize, 0);
    warning_text->SetFont(::Label::Body_13);
    warning_text->SetForegroundColour(wxColour(255, 111, 0));

    warning_text->Wrap(AMS_MATERIALS_SETTING_BODY_WIDTH);
    warning_text->SetMinSize(wxSize(AMS_MATERIALS_SETTING_BODY_WIDTH, -1));
    warning_text->Hide();

    m_panel_SN = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    wxBoxSizer* m_sizer_SN = new wxBoxSizer(wxVERTICAL);
    m_sizer_SN->AddSpacer(FromDIP(16));

    wxBoxSizer* m_sizer_SN_inside = new wxBoxSizer(wxHORIZONTAL);

    auto m_title_SN = new wxStaticText(m_panel_SN, wxID_ANY, _L("SN"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_SN->SetFont(::Label::Body_13);
    m_title_SN->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_SN->Wrap(-1);
    m_sizer_SN_inside->Add(m_title_SN, 0, wxALIGN_CENTER, 0);

    m_sizer_SN_inside->Add(0, 0, 0, wxEXPAND, 0);

    m_sn_number = new wxStaticText(m_panel_SN, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    m_sn_number->SetForegroundColour(*wxBLACK);
    m_sizer_SN_inside->Add(m_sn_number, 0, wxALIGN_CENTER, 0);
    m_sizer_SN->Add(m_sizer_SN_inside);

    m_panel_SN->SetSizer(m_sizer_SN);
    m_panel_SN->Layout();
    m_panel_SN->Fit();

    wxBoxSizer* m_tip_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_tip_readonly = new Label(parent, "");
    m_tip_readonly->SetForegroundColour(*wxBLACK);
    m_tip_readonly->SetBackgroundColour(*wxWHITE);
    m_tip_readonly->SetMinSize(wxSize(FromDIP(380), -1));
    m_tip_readonly->SetMaxSize(wxSize(FromDIP(380), -1));
    m_tip_readonly->Hide();
    m_tip_sizer->Add(m_tip_readonly, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(20));

    sizer->Add(0, 0, 0, wxTOP, FromDIP(16));
    sizer->Add(m_sizer_filament, 0, wxLEFT | wxRIGHT, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(16));
    sizer->Add(m_sizer_colour, 0, wxLEFT | wxRIGHT, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(16));
    sizer->Add(m_sizer_temperature, 0, wxLEFT | wxRIGHT, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(5));
    sizer->Add(warning_text, 0, wxLEFT | wxRIGHT, FromDIP(20));
    sizer->Add(m_panel_SN, 0, wxLEFT, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(24));
    sizer->Add(m_tip_sizer, 0, wxLEFT, FromDIP(20));
    parent->SetSizer(sizer);
}

void AMSMaterialsSetting::create_panel_kn(wxWindow* parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);
    auto cali_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    // title
    m_ratio_text   = new wxStaticText(parent, wxID_ANY, _L("Factors of Flow Dynamics Calibration"));
    m_ratio_text->SetForegroundColour(wxColour(50, 58, 61));
    m_ratio_text->SetFont(Label::Head_14);

    std::string language = wxGetApp().app_config->get("language");
    wxString    region   = "en";
    if (language.find("zh") == 0)
        region = "zh";
    wxString link_url = wxString::Format("https://wiki.bambulab.com/%s/software/bambu-studio/calibration_pa", region);
    m_wiki_ctrl = new wxHyperlinkCtrl(parent, wxID_ANY, "Wiki", link_url);
    m_wiki_ctrl->SetNormalColour(*wxBLUE);
    m_wiki_ctrl->SetHoverColour(wxColour(0, 0, 200));
    m_wiki_ctrl->SetVisitedColour(*wxBLUE);
    m_wiki_ctrl->SetFont(Label::Head_14);
    cali_title_sizer->Add(m_ratio_text, 0, wxALIGN_CENTER_VERTICAL);
    cali_title_sizer->Add(m_wiki_ctrl, 0, wxALIGN_CENTER_VERTICAL);

    wxBoxSizer *m_sizer_cali_resutl = new wxBoxSizer(wxHORIZONTAL);
    // pa profile
    m_title_pa_profile = new wxStaticText(parent, wxID_ANY, _L("PA Profile"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    m_title_pa_profile->SetMinSize(wxSize(FromDIP(80), -1));
    m_title_pa_profile->SetMaxSize(wxSize(FromDIP(80), -1));
    m_title_pa_profile->SetFont(::Label::Body_13);
    m_title_pa_profile->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_title_pa_profile->Wrap(-1);
    m_sizer_cali_resutl->Add(m_title_pa_profile, 0, wxALIGN_CENTER, 0);
    m_sizer_cali_resutl->Add(0, 0, 0, wxEXPAND, 0);

    m_comboBox_cali_result = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, AMS_MATERIALS_SETTING_COMBOX_WIDTH, 0, nullptr, wxCB_READONLY);
    m_sizer_cali_resutl->Add(m_comboBox_cali_result, 1, wxALIGN_CENTER, 0);

    auto kn_val_sizer = new wxFlexGridSizer(0, 2, 0, 0);
    kn_val_sizer->SetFlexibleDirection(wxBOTH);
    kn_val_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
    kn_val_sizer->AddGrowableCol(1);

    // k params input
    m_k_param = new wxStaticText(parent, wxID_ANY, _L("Factor K"), wxDefaultPosition, wxDefaultSize, 0);
    m_k_param->SetMinSize(wxSize(FromDIP(80), -1));
    m_k_param->SetMaxSize(wxSize(FromDIP(80), -1));
    m_k_param->SetFont(::Label::Body_13);
    m_k_param->SetForegroundColour(wxColour(50, 58, 61));
    m_k_param->Wrap(-1);
    kn_val_sizer->Add(m_k_param, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));

    m_input_k_val = new TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_input_k_val->SetMinSize(wxSize(FromDIP(245), -1));
    m_input_k_val->SetMaxSize(wxSize(FromDIP(245), -1));
    m_input_k_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    kn_val_sizer->Add(m_input_k_val, 0, wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(0));

    // n params input
    wxBoxSizer* n_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_n_param = new wxStaticText(parent, wxID_ANY, _L("Factor N"), wxDefaultPosition, wxDefaultSize, 0);
    m_n_param->SetFont(::Label::Body_13);
    m_n_param->SetForegroundColour(wxColour(50, 58, 61));
    m_n_param->Wrap(-1);
    kn_val_sizer->Add(m_n_param, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_input_n_val = new TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_input_n_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    kn_val_sizer->Add(m_input_n_val, 0, wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_n_param->Hide();
    m_input_n_val->Hide();

    sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer->Add(cali_title_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(12));
    sizer->Add(m_sizer_cali_resutl, 0, wxLEFT | wxRIGHT, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer->Add(kn_val_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    parent->SetSizer(sizer);
}

void AMSMaterialsSetting::paintEvent(wxPaintEvent &evt)
{
    auto      size = GetSize();
    wxPaintDC dc(this);
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#000000")), 1, wxPENSTYLE_SOLID));
    dc.SetBrush(wxBrush(*wxTRANSPARENT_BRUSH));
    dc.DrawRectangle(0, 0, size.x, size.y);
}

AMSMaterialsSetting::~AMSMaterialsSetting()
{
    m_comboBox_filament->Disconnect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(AMSMaterialsSetting::on_select_filament), NULL, this);
    m_comboBox_cali_result->Disconnect(wxEVT_COMMAND_COMBOBOX_SELECTED, wxCommandEventHandler(AMSMaterialsSetting::on_select_cali_result), NULL, this);
}

void AMSMaterialsSetting::input_min_finish()
{
    if (m_input_nozzle_min->GetTextCtrl()->GetValue().empty()) return;

    auto val = std::atoi(m_input_nozzle_min->GetTextCtrl()->GetValue().c_str());

    if (val < FILAMENT_MIN_TEMP || val > FILAMENT_MAX_TEMP) {
        warning_text->Show();
    } else {
        warning_text->Hide();
    }
    Layout();
    Fit();
}

void AMSMaterialsSetting::input_max_finish()
{
    if (m_input_nozzle_max->GetTextCtrl()->GetValue().empty()) return;

    auto val = std::atoi(m_input_nozzle_max->GetTextCtrl()->GetValue().c_str());

    if (val < FILAMENT_MIN_TEMP || val > FILAMENT_MAX_TEMP) {
        warning_text->Show();
    }
    else {
        warning_text->Hide();
    }
    Layout();
    Fit();
}

void AMSMaterialsSetting::update()
{
    if (obj) {
        update_widgets();
        update_filament_editing(obj->is_in_printing() || obj->can_resume());
    }
}

void AMSMaterialsSetting::update_filament_editing(bool is_printing)
{
    if (is_printing) {
        m_comboBox_filament->Enable(obj->is_support_filament_setting_inprinting);
        m_comboBox_cali_result->Enable(obj->is_support_filament_setting_inprinting);
        m_button_confirm->Show(obj->is_support_filament_setting_inprinting);
        m_button_reset->Show(obj->is_support_filament_setting_inprinting);
    }
    else {
        m_comboBox_filament->Enable(true);
        m_comboBox_cali_result->Enable(true);
        m_button_reset->Show(true);
        m_button_confirm->Show(true);
    }

    if (!m_is_third) {
        m_tip_readonly->SetLabelText(wxEmptyString);
        m_tip_readonly->Hide();
    }
    else {
        if (!obj->is_support_filament_setting_inprinting) {
            if (!is_virtual_tray()) {
                m_tip_readonly->SetLabelText(_L("Setting AMS slot information while printing is not supported"));
            } else {
                m_tip_readonly->SetLabelText(_L("Setting Virtual slot information while printing is not supported"));
            }
        } else {
            m_tip_readonly->SetLabelText(wxEmptyString);
        }

        m_tip_readonly->Wrap(FromDIP(380));
        m_tip_readonly->Show(is_printing);
    }
}

void AMSMaterialsSetting::on_select_reset(wxCommandEvent& event) {
    MessageDialog msg_dlg(nullptr, _L("Are you sure you want to clear the filament information?"), wxEmptyString, wxICON_WARNING | wxOK | wxCANCEL);
    auto result = msg_dlg.ShowModal();
    if (result != wxID_OK)
        return;

    m_input_nozzle_min->GetTextCtrl()->SetValue("");
    m_input_nozzle_max->GetTextCtrl()->SetValue("");
    ams_filament_id = "";
    ams_setting_id = "";
    m_filament_selection = -1;
    wxString k_text = "0.000";
    wxString n_text = "0.000";
    m_filament_type = "";
    long nozzle_temp_min_int = 0;
    long nozzle_temp_max_int = 0;
    wxColour color = *wxWHITE;
    char col_buf[10];
    sprintf(col_buf, "%02X%02X%02X00", (int)color.Red(), (int)color.Green(), (int)color.Blue());
    std::string color_str;  // reset use empty string

    std::string   selected_ams_id;
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
            auto        filament_item = map_filament_items[m_comboBox_filament->GetValue().ToStdString()];
            std::string filament_id   = filament_item.filament_id;
            if (it->filament_id.compare(filament_id) == 0) {
                selected_ams_id = it->filament_id;
                break;
            }
        }
    }

    if (obj) {
        if(m_is_third){
            obj->command_ams_filament_settings(ams_id, slot_id, ams_filament_id, ams_setting_id, std::string(col_buf), m_filament_type, nozzle_temp_min_int,
                                               nozzle_temp_max_int);
        }

        // set k / n value
        if (obj->cali_version <= -1 && obj->get_printer_series() == PrinterSeries::SERIES_P1P) {
            // set extrusion cali ratio
            int cali_tray_id = ams_id * 4 + slot_id;

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
            obj->command_extrusion_cali_set(cali_tray_id, "", "", k, n);
        }
        else {
            PACalibIndexInfo select_index_info;
            int tray_id = ams_id * 4 + slot_id;
            if (is_virtual_tray()) {
                tray_id = ams_id;
                if (!obj->is_enable_np) {
                    tray_id = VIRTUAL_TRAY_DEPUTY_ID;
                }
            }
            select_index_info.tray_id = tray_id;
            select_index_info.ams_id = ams_id;
            select_index_info.slot_id = slot_id;
            select_index_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);
            select_index_info.cali_idx = -1;
            select_index_info.filament_id     = selected_ams_id;
            CalibUtils::select_PA_calib_result(select_index_info);
        }
    }
    Close();
}

void AMSMaterialsSetting::on_select_ok(wxCommandEvent &event)
{
    //get filament id
    ams_filament_id = "";
    ams_setting_id = "";

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {

            auto filament_item = map_filament_items[m_comboBox_filament->GetValue().ToStdString()];
            std::string filament_id = filament_item.filament_id;
            if (it->filament_id.compare(filament_id) == 0) {


                //check is it in the filament blacklist
                if (wxGetApp().app_config->get("skip_ams_blacklist_check") != "true") {
                    bool in_blacklist = false;
                    std::string action;
                    wxString info;
                    std::string filamnt_type;
                    std::string filamnt_name;
                    it->get_filament_type(filamnt_type);

                    auto vendor = dynamic_cast<ConfigOptionStrings *>(it->config.option("filament_vendor"));

                    if (vendor && (vendor->values.size() > 0)) {
                        std::string vendor_name = vendor->values[0];
                        DevFilaBlacklist::check_filaments_in_blacklist(obj->printer_type, vendor_name, filamnt_type, it->filament_id, ams_id, slot_id, it->name, in_blacklist, action, info);
                    }

                    if (in_blacklist) {
                        if (action == "prohibition") {
                            MessageDialog msg_wingow(nullptr, info, _L("Error"), wxICON_WARNING | wxOK);
                            msg_wingow.ShowModal();
                            //m_comboBox_filament->SetSelection(m_filament_selection);
                            return;
                        }
                        else if (action == "warning") {
                            MessageDialog msg_wingow(nullptr, info, _L("Warning"), wxICON_INFORMATION | wxOK);
                            msg_wingow.ShowModal();
                        }
                    }
                }

                ams_filament_id = it->filament_id;
                ams_setting_id = it->setting_id;
                break;
            }
        }
    }

    wxString nozzle_temp_min = m_input_nozzle_min->GetTextCtrl()->GetValue();
    auto     filament = m_comboBox_filament->GetValue();

    wxString nozzle_temp_max = m_input_nozzle_max->GetTextCtrl()->GetValue();

    long nozzle_temp_min_int, nozzle_temp_max_int;
    nozzle_temp_min.ToLong(&nozzle_temp_min_int);
    nozzle_temp_max.ToLong(&nozzle_temp_max_int);
    wxColour color = m_clr_picker->m_colour;
    char col_buf[10];
    sprintf(col_buf, "%02X%02X%02X%02X", (int)color.Red(), (int)color.Green(), (int)color.Blue(), (int)color.Alpha());

    if (ams_filament_id.empty() || nozzle_temp_min.empty() || nozzle_temp_max.empty() || m_filament_type.empty()) {
        BOOST_LOG_TRIVIAL(trace) << "Invalid Setting id";
        MessageDialog msg_dlg(nullptr, _L("You need to select the material type and color first."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }


    // set filament
    if (m_is_third) {
        obj->command_ams_filament_settings(ams_id, slot_id, ams_filament_id, ams_setting_id, std::string(col_buf), m_filament_type, nozzle_temp_min_int, nozzle_temp_max_int);
    }

    //reset param
    wxString k_text = m_input_k_val->GetTextCtrl()->GetValue();
    wxString n_text = m_input_n_val->GetTextCtrl()->GetValue();

    if (obj->cali_version <= -1 && (obj->get_printer_series() != PrinterSeries::SERIES_X1) && !ExtrusionCalibration::check_k_validation(k_text)) {
        wxString k_tips = wxString::Format(_L("Please input a valid value (K in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE);
        wxString kn_tips = wxString::Format(_L("Please input a valid value (K in %.1f~%.1f, N in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE, 0.6, 2.0);
        MessageDialog msg_dlg(nullptr, k_tips, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    // set k / n value
    if (is_virtual_tray()) {
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

        auto vt_tray = ams_id;
        if (!obj->is_enable_np) {
            vt_tray = VIRTUAL_TRAY_DEPUTY_ID;
        }

        if (obj->cali_version >= 0) {
            PACalibIndexInfo select_index_info;
            select_index_info.tray_id = vt_tray;
            select_index_info.ams_id = ams_id;
            select_index_info.slot_id = 0;
            select_index_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);

            auto cali_select_id = m_comboBox_cali_result->GetSelection();
            if (m_pa_profile_items.size() > 0 && cali_select_id >= 0) {
                select_index_info.cali_idx = m_pa_profile_items[cali_select_id].cali_idx;
                select_index_info.filament_id = m_pa_profile_items[cali_select_id].filament_id;
            }
            else { // default item
                select_index_info.cali_idx = -1;
                select_index_info.filament_id = ams_filament_id;
            }

            CalibUtils::select_PA_calib_result(select_index_info);
        }
        else {
            obj->command_extrusion_cali_set(vt_tray, "", "", k, n);
        }
    }
    else {
        int cali_tray_id = ams_id * 4 + slot_id;
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

        if (obj->cali_version >= 0) {
            PACalibIndexInfo select_index_info;
            select_index_info.tray_id = cali_tray_id;
            select_index_info.ams_id = ams_id;
            select_index_info.slot_id = slot_id;
            select_index_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);

            auto cali_select_id = m_comboBox_cali_result->GetSelection();
            if (m_pa_profile_items.size() > 0 && cali_select_id > 0) {
                select_index_info.cali_idx = m_pa_profile_items[cali_select_id].cali_idx;
                select_index_info.filament_id = m_pa_profile_items[cali_select_id].filament_id;
            }
            else { // default item
                select_index_info.cali_idx    = -1;
                select_index_info.filament_id = ams_filament_id;
            }

            CalibUtils::select_PA_calib_result(select_index_info);
        }
        else {
            obj->command_extrusion_cali_set(cali_tray_id, "", "", k, n);
        }
    }
    Close();
}

void AMSMaterialsSetting::on_select_close(wxCommandEvent &event)
{
    Close();
}

void AMSMaterialsSetting::set_color(wxColour color)
{
    //m_clrData->SetColour(color);
    m_clr_picker->is_empty(false);
    m_clr_picker->set_color(color);

    FilamentColor fila_color;
    fila_color.m_colors.insert(color);
    fila_color.EndSet(m_clr_picker->ctype);
    auto clr_query = GUI::wxGetApp().get_filament_color_code_query();
    m_clr_name->SetLabelText(clr_query->GetFilaColorName(ams_filament_id, fila_color));
}

void AMSMaterialsSetting::set_empty_color(wxColour color)
{
    m_clr_picker->is_empty(true);
    m_clr_picker->set_color(color);
    m_clr_name->SetLabelText(wxEmptyString);
}

void AMSMaterialsSetting::set_colors(std::vector<wxColour> colors)
{
    //m_clrData->SetColour(color);
    m_clr_picker->set_colors(colors);

    if (!colors.empty())
    {
        FilamentColor fila_color;
        for (const auto& clr : colors) { fila_color.m_colors.insert(clr); }
        fila_color.EndSet(m_clr_picker->ctype);
        auto clr_query = GUI::wxGetApp().get_filament_color_code_query();
        m_clr_name->SetLabelText(clr_query->GetFilaColorName(ams_filament_id, fila_color));
    }
}

void AMSMaterialsSetting::set_ctype(int ctype)
{
    m_clr_picker->ctype = ctype;
}

void AMSMaterialsSetting::on_picker_color(wxCommandEvent& event)
{
    unsigned int color_num  = event.GetInt();
    set_color(wxColour(color_num>>24&0xFF, color_num>>16&0xFF, color_num>>8&0xFF, color_num&0xFF));
}

void AMSMaterialsSetting::on_clr_picker(wxMouseEvent &event)
{
    if(!m_is_third)
        return;

    if (obj->is_in_printing() || obj->can_resume()) {
        if (!obj->is_support_filament_setting_inprinting) {
            return;
        }
    }

    std::vector<wxColour> ams_colors;
    obj->GetFilaSystem()->CollectAmsColors(ams_colors);

    wxPoint img_pos = m_clr_picker->ClientToScreen(wxPoint(0, 0));
    wxPoint popup_pos(img_pos.x - m_color_picker_popup.GetSize().x - FromDIP(95), img_pos.y - FromDIP(65));
    m_color_picker_popup.Position(popup_pos, wxSize(0, 0));
    m_color_picker_popup.set_ams_colours(ams_colors);
    m_color_picker_popup.set_def_colour(m_clr_picker->m_colour);
    m_color_picker_popup.Popup();
}

bool AMSMaterialsSetting::is_virtual_tray()
{
    if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID)
        return true;
    return false;
}

void AMSMaterialsSetting::update_widgets()
{
    if (obj && obj->get_printer_series() == PrinterSeries::SERIES_X1 && obj->cali_version <= -1) {
        // Low version firmware does not display k value
        m_panel_kn->Hide();
    }
    else if(is_virtual_tray()) // virtual tray
    {
        if (obj)
            m_panel_normal->Show();
        else
            m_panel_normal->Hide();
        m_panel_kn->Show();
    } else if (obj && (obj->ams_support_virtual_tray || obj->cali_version >= 0)) {
        m_panel_normal->Show();
        m_panel_kn->Show();
    } else {
        m_panel_normal->Show();
        m_panel_kn->Hide();
    }
    Layout();
}

bool AMSMaterialsSetting::Show(bool show)
{
    if (show) {
        m_button_confirm->Rescale(); // ORCA re applies size
        m_input_nozzle_max->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
        m_input_nozzle_min->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
        //m_clr_picker->set_color(m_clr_picker->GetParent()->GetBackgroundColour());

        m_ratio_text->Show();
        m_wiki_ctrl->Show();
        m_k_param->Show();
        m_input_k_val->Show();
        Layout();
        Fit();
        wxGetApp().UpdateDlgDarkUI(this);
    }
    return DPIDialog::Show(show);
}

static void _collect_filament_info(const wxString& shown_name,
                                   const Preset& filament,
                                   unordered_map<wxString, wxString>& query_filament_vendors,
                                   unordered_map<wxString, wxString>& query_filament_types)
{
    query_filament_vendors[shown_name] = filament.config.get_filament_vendor();
    query_filament_types[shown_name] = filament.config.get_filament_type();
}

void AMSMaterialsSetting::Popup(wxString filament, wxString sn, wxString temp_min, wxString temp_max, wxString k, wxString n)
{
    if (!obj) return;
    update_widgets();
    // set default value
    if (k.IsEmpty())
        k = "0.000";
    if (n.IsEmpty())
        n = "0.000";

    m_input_k_val->GetTextCtrl()->SetValue(k);
    m_input_n_val->GetTextCtrl()->SetValue(n);

    int idx = 0;
    wxArrayString filament_items;
    wxString bambu_filament_name;
    wxString hint_filament_name; // the hint type to be selected
    std::unordered_map<wxString, wxString> query_filament_vendors;// some information for sort
    std::unordered_map<wxString, wxString> query_filament_types;  //

    std::set<std::string> filament_id_set;
    PresetBundle *        preset_bundle = wxGetApp().preset_bundle;
    std::ostringstream    stream;
    stream << std::fixed << std::setprecision(1) << obj->GetExtderSystem()->GetNozzleDiameter(0);
    std::string nozzle_diameter_str = stream.str();
    std::set<std::string> printer_names = preset_bundle->get_printer_names_by_printer_type_and_nozzle(DevPrinterConfigUtil::get_printer_display_name(obj->printer_type), nozzle_diameter_str);

    if (preset_bundle) {
        BOOST_LOG_TRIVIAL(trace) << "system_preset_bundle filament number=" << preset_bundle->filaments.size();
        for (auto filament_it = preset_bundle->filaments.begin(); filament_it != preset_bundle->filaments.end(); filament_it++) {
            //filter by system preset
            Preset& preset = *filament_it;
            /*The situation where the user preset is not displayed is as follows:
                1. Not a root preset
                2. Not system preset and the printer firmware does not support user preset */
            if (preset_bundle->filaments.get_preset_base(*filament_it) != &preset || (!filament_it->is_system && !obj->is_support_user_preset)) {
                continue;
            }

            ConfigOption *       printer_opt  = filament_it->config.option("compatible_printers");
            ConfigOptionStrings *printer_strs = dynamic_cast<ConfigOptionStrings *>(printer_opt);
            for (auto printer_str : printer_strs->values) {
                if (printer_names.find(printer_str) != printer_names.end()) {
                    if (filament_id_set.find(filament_it->filament_id) != filament_id_set.end()) {
                        continue;
                    } else {
                        filament_id_set.insert(filament_it->filament_id);
                        // name matched
                        if (filament_it->is_system) {
                            filament_items.push_back(filament_it->alias);
                            _collect_filament_info(filament_it->alias, preset, query_filament_vendors, query_filament_types);

                            FilamentInfos filament_infos;
                            filament_infos.filament_id             = filament_it->filament_id;
                            filament_infos.setting_id              = filament_it->setting_id;
                            map_filament_items[filament_it->alias] = filament_infos;
                        } else {
                            char   target = '@';
                            size_t pos    = filament_it->name.find(target);
                            if (pos != std::string::npos) {
                                std::string user_preset_alias    = filament_it->name.substr(0, pos - 1);
                                wxString    wx_user_preset_alias = wxString(user_preset_alias.c_str(), wxConvUTF8);
                                user_preset_alias                = wx_user_preset_alias.ToStdString();

                                filament_items.push_back(user_preset_alias);
                                _collect_filament_info(user_preset_alias, preset, query_filament_vendors, query_filament_types);

                                FilamentInfos filament_infos;
                                filament_infos.filament_id            = filament_it->filament_id;
                                filament_infos.setting_id             = filament_it->setting_id;
                                map_filament_items[user_preset_alias] = filament_infos;
                            }
                        }

                        if (filament_it->filament_id == ams_filament_id) {
                            hint_filament_name = from_u8(filament_it->alias);
                            bambu_filament_name = from_u8(filament_it->alias);


                            // update if nozzle_temperature_range is found
                            ConfigOption *opt_min = filament_it->config.option("nozzle_temperature_range_low");
                            if (opt_min) {
                                ConfigOptionInts *opt_min_ints = dynamic_cast<ConfigOptionInts *>(opt_min);
                                if (opt_min_ints) {
                                    wxString text_nozzle_temp_min = wxString::Format("%d", opt_min_ints->get_at(0));
                                    m_input_nozzle_min->GetTextCtrl()->SetValue(text_nozzle_temp_min);
                                }
                            }
                            ConfigOption *opt_max = filament_it->config.option("nozzle_temperature_range_high");
                            if (opt_max) {
                                ConfigOptionInts *opt_max_ints = dynamic_cast<ConfigOptionInts *>(opt_max);
                                if (opt_max_ints) {
                                    wxString text_nozzle_temp_max = wxString::Format("%d", opt_max_ints->get_at(0));
                                    m_input_nozzle_max->GetTextCtrl()->SetValue(text_nozzle_temp_max);
                                }
                            }
                        }
                        idx++;
                    }
                }
            }

        }
    }

    if (!sn.empty()) {
        m_sn_number->SetLabel(sn);
        m_panel_SN->Show();
    }
    else {
        m_panel_SN->Hide();
    }

    if (obj) {
        if (!m_is_third) {
            m_comboBox_filament->Hide();
            m_readonly_filament->Show();
            if (bambu_filament_name.empty()) {
                m_readonly_filament->SetLabel("Bambu " + filament);
            }
            else {
                m_readonly_filament->SetLabel(bambu_filament_name);
            }

            m_input_nozzle_min->GetTextCtrl()->SetValue(temp_min);
            m_input_nozzle_max->GetTextCtrl()->SetValue(temp_max);
        }
        else {
            m_comboBox_filament->Show();
            m_readonly_filament->Hide();
        }

        if (obj->cali_version >= 0) {
            m_title_pa_profile->Show();
            m_comboBox_cali_result->Show();
            m_input_k_val->Disable();
        }
        else {
            m_title_pa_profile->Hide();
            m_comboBox_cali_result->Hide();
            m_input_k_val->Enable();
        }

        m_button_reset->Show();
        //m_button_confirm->Show();
    }

    // Sort the filaments
    {
        static std::unordered_map<wxString, int> sorted_names
        {   {"Bambu PLA Basic",        0},
            {"Bambu PLA Matte",        1},
            {"Bambu PETG HF",          2},
            {"Bambu ABS",              3},
            {"Bambu PLA Silk",         4},
            {"Bambu PLA-CF" ,          5},
            {"Bambu PLA Galaxy",       6},
            {"Bambu PLA Metal",        7},
            {"Bambu PLA Marble",       8},
            {"Bambu PETG-CF",          9},
            {"Bambu PETG Translucent", 10},
            {"Bambu ABS-GF",           11}
        };

        static std::vector<wxString> sorted_vendors { "Bambu Lab", "Generic" };
        static std::vector<wxString> sorted_types { "PLA", "PETG", "ABS", "TPU" };
        auto _filament_sorter = [&query_filament_vendors, &query_filament_types](const wxString& left, const wxString& right) -> bool
        {
            { // Compare name order
                const auto& iter1 = sorted_names.find(left);
                int name_order1 = (iter1 != sorted_names.end()) ? iter1->second : INT_MAX;

                const auto& iter2 = sorted_names.find(right);
                int name_order2 = (iter2 != sorted_names.end()) ? iter2->second : INT_MAX;
                if (name_order1 != name_order2)
                {
                    return name_order1 < name_order2;
                }
            }
            { // Compare vendor
                auto iter1 = std::find(sorted_vendors.begin(), sorted_vendors.end(), query_filament_vendors[left]);
                auto iter2 = std::find(sorted_vendors.begin(), sorted_vendors.end(), query_filament_vendors[right]);
                if (iter1 != iter2)
                {
                    return iter1 < iter2;
                };
            }
            { // Compare type
                auto iter1 = std::find(sorted_types.begin(), sorted_types.end(), query_filament_types[left]);
                auto iter2 = std::find(sorted_types.begin(), sorted_types.end(), query_filament_types[right]);
                if (iter1 != iter2)
                {
                    return iter1 < iter2;
                }
            }

            return left < right;
        };

        std::sort(filament_items.begin(), filament_items.end(), _filament_sorter);
    }

    // traverse the hint selection idx
    int selection_idx = -1;
    {
        for(int i = 0; i < filament_items.size(); i++)
        {
            if (hint_filament_name == filament_items[i])
            {
                selection_idx = i;
                break;
            }
        }
    }

    m_comboBox_filament->Set(filament_items);
    m_comboBox_filament->SetSelection(selection_idx);
    post_select_event(selection_idx);

    if (selection_idx < 0) {
        m_comboBox_filament->SetValue(wxEmptyString);
    }

    // Set the flag whether to open the filament setting dialog from the device page
    m_comboBox_filament->SetClientData(new int(1));

    update();
    Layout();
    Fit();
    ShowModal();
}

void AMSMaterialsSetting::post_select_event(int index) {
    wxCommandEvent event(wxEVT_COMBOBOX);
    event.SetInt(index);
    event.SetEventObject(m_comboBox_filament);
    wxPostEvent(m_comboBox_filament, event);
}

void AMSMaterialsSetting::on_select_cali_result(wxCommandEvent &evt)
{
    m_pa_cali_select_id = evt.GetSelection();
    if (m_pa_cali_select_id >= 0) {
        m_input_k_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[m_pa_cali_select_id].k_value));
        m_input_n_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[m_pa_cali_select_id].n_coef));
    }
    else{
        m_input_k_val->GetTextCtrl()->SetValue(std::to_string(0.00));
        m_input_n_val->GetTextCtrl()->SetValue(std::to_string(0.00));
    }
}

void AMSMaterialsSetting::on_select_filament(wxCommandEvent &evt)
{
    // Get the flag whether to open the filament setting dialog from the device page
    int* from_printer = static_cast<int*>(m_comboBox_filament->GetClientData());

    m_filament_type = "";
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle) {
        std::ostringstream stream;
        if (obj)
            stream << std::fixed << std::setprecision(1) << obj->GetExtderSystem()->GetNozzleDiameter(0);
        std::string nozzle_diameter_str = stream.str();
        std::set<std::string> printer_names = preset_bundle->get_printer_names_by_printer_type_and_nozzle(DevPrinterConfigUtil::get_printer_display_name(obj->printer_type),
                                                                                                          nozzle_diameter_str);
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
            if (!m_comboBox_filament->GetValue().IsEmpty()) {
                auto filament_item = map_filament_items[m_comboBox_filament->GetValue().ToStdString()];
                std::string filament_id   = filament_item.filament_id;
                if (it->filament_id.compare(filament_id) == 0) {
                    ConfigOption *       printer_opt  = it->config.option("compatible_printers");
                    ConfigOptionStrings *printer_strs = dynamic_cast<ConfigOptionStrings *>(printer_opt);
                    bool has_compatible_printer = false;
                    for (auto printer_str : printer_strs->values) {
                        if (printer_names.find(printer_str) != printer_names.end()) {
                            has_compatible_printer = true;
                            break;
                        }
                    }
                    if (!it->is_system && !has_compatible_printer) continue;
                    // ) if nozzle_temperature_range is found
                    ConfigOption* opt_min = it->config.option("nozzle_temperature_range_low");
                    if (opt_min) {
                        ConfigOptionInts* opt_min_ints = dynamic_cast<ConfigOptionInts*>(opt_min);
                        if (opt_min_ints) {
                            wxString text_nozzle_temp_min = wxString::Format("%d", opt_min_ints->get_at(0));
                            m_input_nozzle_min->GetTextCtrl()->SetValue(text_nozzle_temp_min);
                        }
                    }
                    ConfigOption* opt_max = it->config.option("nozzle_temperature_range_high");
                    if (opt_max) {
                        ConfigOptionInts* opt_max_ints = dynamic_cast<ConfigOptionInts*>(opt_max);
                        if (opt_max_ints) {
                            wxString text_nozzle_temp_max = wxString::Format("%d", opt_max_ints->get_at(0));
                            m_input_nozzle_max->GetTextCtrl()->SetValue(text_nozzle_temp_max);
                        }
                    }
                    ConfigOption* opt_type = it->config.option("filament_type");
                    bool found_filament_type = false;
                    if (opt_type) {
                        ConfigOptionStrings* opt_type_strs = dynamic_cast<ConfigOptionStrings*>(opt_type);
                        if (opt_type_strs) {
                            found_filament_type = true;
                            //m_filament_type = opt_type_strs->get_at(0);
                            std::string display_filament_type;
                            m_filament_type = it->config.get_filament_type(display_filament_type);
                        }
                    }
                    if (!found_filament_type)
                        m_filament_type = "";

                    break;
                }
            }
        }
    }
    if (m_input_nozzle_min->GetTextCtrl()->GetValue().IsEmpty()) {
         m_input_nozzle_min->GetTextCtrl()->SetValue("0");
    }
    if (m_input_nozzle_max->GetTextCtrl()->GetValue().IsEmpty()) {
         m_input_nozzle_max->GetTextCtrl()->SetValue("0");
    }

    m_filament_selection = evt.GetSelection();

    //reset cali
    int cali_select_idx = -1;

    if ( !this->obj || m_filament_selection < 0) {
        m_input_k_val->Enable(false);
        m_input_n_val->Enable(false);
        m_button_confirm->Disable(); // ORCA No need to change style
        m_comboBox_cali_result->Clear();
        m_comboBox_cali_result->SetValue(wxEmptyString);
        m_input_k_val->GetTextCtrl()->SetValue(wxEmptyString);
        m_input_n_val->GetTextCtrl()->SetValue(wxEmptyString);
        m_comboBox_filament->SetClientData(new int(0));
        return;
    }
    else {
        m_button_confirm->Enable(true);  // ORCA No need to change style
    }

    //filament id
    ams_filament_id = "";
    ams_setting_id = "";

    if (preset_bundle) {
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
            auto itor = map_filament_items.find(m_comboBox_filament->GetValue().ToStdString());
            if ( itor != map_filament_items.end()) {
                ams_filament_id = itor->second.filament_id;
                ams_setting_id  = itor->second.setting_id;
                break;
            }

            if (it->alias.compare(m_comboBox_filament->GetValue().ToStdString()) == 0) {
                ams_filament_id = it->filament_id;
                ams_setting_id = it->setting_id;
                break;
            }
        }
    }

    wxArrayString items;
    m_pa_profile_items.clear();
    m_comboBox_cali_result->SetValue(wxEmptyString);

    auto get_cali_index = [this](const std::string& str) -> int{
        for (int i = 0; i < int(m_pa_profile_items.size()); ++i) {
            if (m_pa_profile_items[i].name == str)
                return i;
        }
        return 0;
    };

    int extruder_id = obj->get_extruder_id_by_ams_id(std::to_string(ams_id));
    if (obj->is_nozzle_flow_type_supported() && (obj->GetExtderSystem()->GetNozzleFlowType(extruder_id) == NozzleFlowType::NONE_FLOWTYPE))
    {
        MessageDialog dlg(nullptr, _L("The nozzle flow is not set. Please set the nozzle flow rate before editing the filament.\n'Device -> Print parts'"), _L("Warning"), wxICON_WARNING | wxOK);
        dlg.ShowModal();
    }

    NozzleFlowType nozzle_flow_type = obj->GetExtderSystem()->GetNozzleFlowType(extruder_id);
    NozzleVolumeType nozzle_volume_type = NozzleVolumeType::nvtStandard;
    if (nozzle_flow_type != NozzleFlowType::NONE_FLOWTYPE)
    {
        nozzle_volume_type = NozzleVolumeType(nozzle_flow_type - 1);
    }

    if (obj->cali_version >= 0) {
        // add default item
        PACalibResult default_item;
        default_item.cali_idx = -1;
        default_item.filament_id = ams_filament_id;
        if (obj->GetConfig()->SupportCalibrationPA_FlowAuto()) {
            default_item.k_value = -1;
            default_item.n_coef  = -1;
        }
        else {
            get_default_k_n_value(ams_filament_id, default_item.k_value, default_item.n_coef);
        }
        m_pa_profile_items.emplace_back(default_item);
        items.push_back(_L("Default"));

        m_input_k_val->GetTextCtrl()->SetValue(wxEmptyString);
        std::vector<PACalibResult> cali_history = this->obj->pa_calib_tab;
        for (auto cali_item : cali_history) {
            if (cali_item.filament_id == ams_filament_id) {
                if (obj->is_multi_extruders() && (cali_item.extruder_id != extruder_id || cali_item.nozzle_volume_type != nozzle_volume_type)) {
                    continue;
                }
                items.push_back(from_u8(cali_item.name));
                m_pa_profile_items.push_back(cali_item);
            }
        }

        m_comboBox_cali_result->Set(items);
        if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
            if (from_printer && (*from_printer == 1)) {
                for (auto slot : obj->vt_slot) {
                    if (slot.id == std::to_string(ams_id))
                        cali_select_idx = CalibUtils::get_selected_calib_idx(m_pa_profile_items, slot.cali_idx);
                }

                if (cali_select_idx >= 0)
                    m_comboBox_cali_result->SetSelection(cali_select_idx);
                else
                    m_comboBox_cali_result->SetSelection(0);
            }
            else {
                int index = get_cali_index(m_comboBox_filament->GetLabel().ToStdString());
                m_comboBox_cali_result->SetSelection(index);
            }
        }
        else {
            if (from_printer && (*from_printer == 1)) {
                DevAmsTray* selected_tray = this->obj->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
                if (!selected_tray)
                {
                    return;
                }

                cali_select_idx = CalibUtils::get_selected_calib_idx(m_pa_profile_items, selected_tray->cali_idx);
                if (cali_select_idx < 0)
                {
                    BOOST_LOG_TRIVIAL(info) << "extrusion_cali_status_error: cannot find pa profile, ams_id = " << ams_id
                        << ", slot_id = " << slot_id << ", cali_idx = " << selected_tray->cali_idx;
                    cali_select_idx = 0;
                }
                m_comboBox_cali_result->SetSelection(cali_select_idx);
            }
            else {
                int index = get_cali_index(m_comboBox_filament->GetLabel().ToStdString());
                m_comboBox_cali_result->SetSelection(index);
            }
        }

        if (cali_select_idx >= 0) {
            m_input_k_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[cali_select_idx].k_value));
            m_input_n_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[cali_select_idx].n_coef));
        }
        else {
            m_input_k_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[0].k_value));
            m_input_n_val->GetTextCtrl()->SetValue(float_to_string_with_precision(m_pa_profile_items[0].n_coef));
        }
    }
    else {
        if (!ams_filament_id.empty()) {
            //m_input_k_val->GetTextCtrl()->SetValue("0.00");
            m_input_k_val->Enable(true);
        }
        else {
            //m_input_k_val->GetTextCtrl()->SetValue("0.00");
            m_input_k_val->Disable();
        }
    }

    m_comboBox_filament->SetClientData(new int(0));
}

void AMSMaterialsSetting::on_dpi_changed(const wxRect &suggested_rect)
{
    m_input_nozzle_max->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
    m_input_nozzle_min->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
    m_input_k_val->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(20)));
    m_clr_picker->msw_rescale();
    degree->msw_rescale();
    bitmap_max_degree->SetBitmap(degree->bmp());
    bitmap_min_degree->SetBitmap(degree->bmp());
    m_button_reset->Rescale(); // ORCA
    m_button_confirm->Rescale(); // ORCA
    m_button_close->Rescale(); // ORCA
    this->Refresh();
}

ColorPicker::ColorPicker(wxWindow* parent, wxWindowID id, const wxPoint& pos /*= wxDefaultPosition*/, const wxSize& size /*= wxDefaultSize*/)
{
    wxWindow::Create(parent, id, pos, size);

    SetSize(wxSize(FromDIP(25), FromDIP(25)));
    SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));

    Bind(wxEVT_PAINT, &ColorPicker::paintEvent, this);

    m_bitmap_border = create_scaled_bitmap("color_picker_border", nullptr, 25);
    m_bitmap_border_dark = create_scaled_bitmap("color_picker_border_dark", nullptr, 25);
    m_bitmap_transparent_def = ScalableBitmap(this, "transparent_color_picker", 25);
    m_bitmap_transparent = create_scaled_bitmap("transparent_color_picker", nullptr, 25);
}

ColorPicker::~ColorPicker(){}

void ColorPicker::msw_rescale()
{
    m_bitmap_border = create_scaled_bitmap("color_picker_border", nullptr, 25);
    m_bitmap_border_dark = create_scaled_bitmap("color_picker_border_dark", nullptr, 25);
    m_bitmap_transparent_def.msw_rescale();

    Refresh();
}

void ColorPicker::set_color(wxColour col)
{
    if (m_colour != col && col.Alpha() != 0 && col.Alpha() != 255 && col.Alpha() != 254) {
        transparent_changed = true;
    }
    m_colour = col;
    Refresh();
}

void ColorPicker::set_colors(std::vector<wxColour> cols)
{
    m_cols = cols;
    Refresh();
}

void ColorPicker::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void ColorPicker::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void ColorPicker::doRender(wxDC& dc)
{
    wxSize     size = GetSize();
    auto alpha = m_colour.Alpha();
    auto radius = m_show_full ? size.x / 2 - FromDIP(1) : size.x / 2;
    if (m_selected) radius -= FromDIP(1);

    if (alpha == 0) {
        wxSize bmp_size = m_bitmap_transparent_def.GetBmpSize();
        int center_x = (size.x - bmp_size.x) / 2;
        int center_y = (size.y - bmp_size.y) / 2;
        dc.DrawBitmap(m_bitmap_transparent_def.bmp(), center_x, center_y);
    }
    else if (alpha != 254 && alpha != 255) {
        if (transparent_changed) {
            std::string rgb = (m_colour.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
            if (rgb.size() == 9) {
                //delete alpha value
                rgb = rgb.substr(0, rgb.size() - 2);
            }
            float alpha_f = 0.7 * m_colour.Alpha() / 255.0;
            std::vector<std::string> replace;
            replace.push_back(rgb);
            std::string fill_replace = "fill-opacity=\"" + std::to_string(alpha_f);
            replace.push_back(fill_replace);
            m_bitmap_transparent = ScalableBitmap(this, "transparent_color_picker", 25, false, false, true, replace).bmp();
            transparent_changed = false;
        }
            wxSize bmp_size = m_bitmap_transparent.GetSize();
            int center_x = (size.x - bmp_size.x) / 2;
            int center_y = (size.y - bmp_size.y) / 2;
            dc.DrawBitmap(m_bitmap_transparent, center_x, center_y);
    }
    else {
        dc.SetPen(wxPen(m_colour));
        dc.SetBrush(wxBrush(m_colour));
        dc.DrawCircle(size.x / 2, size.y / 2, radius);
    }

    if (m_selected) {
        dc.SetPen(wxPen(m_colour));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawCircle(size.x / 2, size.y / 2, size.x / 2);
    }

    if (m_show_full) {
        dc.SetPen(wxPen(wxColour("#6B6B6B")));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawCircle(size.x / 2, size.y / 2, radius);

        if (m_cols.size() > 1) {
            if (ctype == 0) {
                int left = FromDIP(0);
                float total_width = size.x;
                int gwidth = std::round(total_width / (m_cols.size() - 1));

                for (int i = 0; i < m_cols.size() - 1; i++) {

                    if ((left + gwidth) > (size.x)) {
                        gwidth = size.x - left;
                    }

                    auto rect = wxRect(left, 0, gwidth, size.y);
                    dc.GradientFillLinear(rect, m_cols[i], m_cols[i + 1], wxEAST);
                    left += gwidth;
                }
                if (wxGetApp().dark_mode()) {
                    dc.DrawBitmap(m_bitmap_border_dark, wxPoint(0, 0));
                }
                else {
                    dc.DrawBitmap(m_bitmap_border, wxPoint(0, 0));
                }
            }
            else {
                float ev_angle = 360.0 / m_cols.size();
                float startAngle = 270.0;
                float endAngle = 270.0;
                dc.SetPen(*wxTRANSPARENT_PEN);
                for (int i = 0; i < m_cols.size(); i++) {
                    dc.SetBrush(m_cols[i]);
                    endAngle += ev_angle;
                    endAngle = endAngle > 360.0 ? endAngle - 360.0 : endAngle;
                    wxPoint center(size.x / 2, size.y / 2);
                    dc.DrawEllipticArc(center.x - radius, center.y - radius, 2 * radius, 2 * radius, startAngle, endAngle);
                    startAngle += ev_angle;
                    startAngle = startAngle > 360.0 ? startAngle - 360.0 : startAngle;
                }
                if (wxGetApp().dark_mode()) {
                    dc.DrawBitmap(m_bitmap_border_dark, wxPoint(0, 0));
                }
                else {
                    dc.DrawBitmap(m_bitmap_border, wxPoint(0, 0));
                }
            }
        }
    }

    if (m_is_empty) {
        dc.SetTextForeground(*wxBLACK);
        auto tsize = dc.GetTextExtent("?");
        auto pot = wxPoint((size.x - tsize.x) / 2, (size.y - tsize.y) / 2);
        dc.DrawText("?", pot);
    }
}

ColorPickerPopup::ColorPickerPopup(wxWindow* parent)
    :PopupWindow(parent, wxBORDER_NONE)
{
    m_def_colors.clear();
    m_def_colors.push_back(wxColour("#FFFFFF"));
    m_def_colors.push_back(wxColour("#fff144"));
    m_def_colors.push_back(wxColour("#DCF478"));
    m_def_colors.push_back(wxColour("#0ACC38"));
    m_def_colors.push_back(wxColour("#057748"));
    m_def_colors.push_back(wxColour("#0d6284"));
    m_def_colors.push_back(wxColour("#0EE2A0"));
    m_def_colors.push_back(wxColour("#76D9F4"));
    m_def_colors.push_back(wxColour("#46a8f9"));
    m_def_colors.push_back(wxColour("#2850E0"));
    m_def_colors.push_back(wxColour("#443089"));
    m_def_colors.push_back(wxColour("#A03CF7"));
    m_def_colors.push_back(wxColour("#F330F9"));
    m_def_colors.push_back(wxColour("#D4B1DD"));
    m_def_colors.push_back(wxColour("#f95d73"));
    m_def_colors.push_back(wxColour("#f72323"));
    m_def_colors.push_back(wxColour("#7c4b00"));
    m_def_colors.push_back(wxColour("#f98c36"));
    m_def_colors.push_back(wxColour("#fcecd6"));
    m_def_colors.push_back(wxColour("#D3C5A3"));
    m_def_colors.push_back(wxColour("#AF7933"));
    m_def_colors.push_back(wxColour("#898989"));
    m_def_colors.push_back(wxColour("#BCBCBC"));
    m_def_colors.push_back(wxColour("#161616"));


    SetBackgroundColour(wxColour(*wxWHITE));

    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* m_sizer_box = new wxBoxSizer(wxVERTICAL);

    m_def_color_box = new StaticBox(this);
    wxBoxSizer* m_sizer_ams = new wxBoxSizer(wxHORIZONTAL);
    auto m_title_ams = new wxStaticText(m_def_color_box, wxID_ANY, _L("AMS"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_ams->SetFont(::Label::Body_14);
    m_title_ams->SetBackgroundColour(wxColour(238, 238, 238));
    m_sizer_ams->Add(m_title_ams, 0, wxALL, 5);
    auto ams_line = new wxPanel(m_def_color_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    ams_line->SetBackgroundColour(wxColour("#CECECE"));
    ams_line->SetMinSize(wxSize(-1, 1));
    ams_line->SetMaxSize(wxSize(-1, 1));
    m_sizer_ams->Add(ams_line, 1, wxALIGN_CENTER, 0);


    m_def_color_box->SetCornerRadius(FromDIP(10));
    m_def_color_box->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Normal)));
    m_def_color_box->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Normal)));

    //ams
    m_ams_fg_sizer = new wxFlexGridSizer(0, 8, 0, 0);
    m_ams_fg_sizer->SetFlexibleDirection(wxBOTH);
    m_ams_fg_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    //other
    wxFlexGridSizer* fg_sizer;
    fg_sizer = new wxFlexGridSizer(0, 8, 0, 0);
    fg_sizer->SetFlexibleDirection(wxBOTH);
    fg_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);


    for (wxColour col : m_def_colors) {
        auto cp = new ColorPicker(m_def_color_box, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        cp->set_color(col);
        cp->set_selected(false);
        cp->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(238,238,238)));
        m_color_pickers.push_back(cp);
        fg_sizer->Add(cp, 0, wxALL, FromDIP(3));
        cp->Bind(wxEVT_LEFT_DOWN, [this, cp](auto& e) {
            set_def_colour(cp->m_colour);

            wxCommandEvent evt(EVT_SELECTED_COLOR);
            unsigned long g_col = ((cp->m_colour.Red() & 0xff) << 24) + ((cp->m_colour.Green() & 0xff) << 16) + ((cp->m_colour.Blue() & 0xff) << 8) + (cp->m_colour.Alpha() & 0xff);
            evt.SetInt(g_col);
            wxPostEvent(GetParent(), evt);
        });
    }

    wxBoxSizer* m_sizer_other = new wxBoxSizer(wxHORIZONTAL);
    auto m_title_other = new wxStaticText(m_def_color_box, wxID_ANY, _L("Other Color"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_other->SetFont(::Label::Body_14);
    m_title_other->SetBackgroundColour(wxColour(238, 238, 238));
    m_sizer_other->Add(m_title_other, 0, wxALL, 5);
    auto other_line = new wxPanel(m_def_color_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    other_line->SetMinSize(wxSize(-1, 1));
    other_line->SetMaxSize(wxSize(-1, 1));
    other_line->SetBackgroundColour(wxColour("#CECECE"));
    m_sizer_other->Add(other_line, 1, wxALIGN_CENTER, 0);

    //custom color
    wxBoxSizer* m_sizer_custom = new wxBoxSizer(wxHORIZONTAL);
    auto m_title_custom = new wxStaticText(m_def_color_box, wxID_ANY, _L("Custom Color"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_custom->SetFont(::Label::Body_14);
    m_title_custom->SetBackgroundColour(wxColour(238, 238, 238));
    auto custom_line = new wxPanel(m_def_color_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    custom_line->SetBackgroundColour(wxColour("#CECECE"));
    custom_line->SetMinSize(wxSize(-1, 1));
    custom_line->SetMaxSize(wxSize(-1, 1));
    m_sizer_custom->Add(m_title_custom, 0, wxALL, 5);
    m_sizer_custom->Add(custom_line, 1, wxALIGN_CENTER, 0);

    m_custom_cp =  new StaticBox(m_def_color_box);
    m_custom_cp->SetSize(FromDIP(60), FromDIP(25));
    m_custom_cp->SetMinSize(wxSize(FromDIP(60), FromDIP(25)));
    m_custom_cp->SetMaxSize(wxSize(FromDIP(60), FromDIP(25)));
    m_custom_cp->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Normal)));
    m_custom_cp->Bind(wxEVT_LEFT_DOWN, &ColorPickerPopup::on_custom_clr_picker, this);
    m_custom_cp->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_HAND);
    });
    m_custom_cp->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_ARROW);
    });

    m_ts_bitmap_custom = ScalableBitmap(this, "ts_custom_color_picker", 25);
    m_ts_stbitmap_custom = new wxStaticBitmap(m_custom_cp, wxID_ANY, m_ts_bitmap_custom.bmp());

    m_ts_stbitmap_custom->Bind(wxEVT_LEFT_DOWN, &ColorPickerPopup::on_custom_clr_picker, this);
    m_ts_stbitmap_custom->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_HAND);
        });
    m_ts_stbitmap_custom->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {
        SetCursor(wxCURSOR_ARROW);
        });

    auto sizer_custom = new wxBoxSizer(wxVERTICAL);
    m_custom_cp->SetSizer(sizer_custom);
    sizer_custom->Add(m_ts_stbitmap_custom, 0, wxEXPAND, 0);
    m_custom_cp->Layout();

    m_clrData = new wxColourData();
    m_clrData->SetChooseFull(true);
    m_clrData->SetChooseAlpha(false);


    m_sizer_box->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_sizer_box->Add(m_sizer_ams, 1, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(10));
    m_sizer_box->Add(m_ams_fg_sizer, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(10));
    m_sizer_box->Add(m_sizer_other, 1, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(10));
    m_sizer_box->Add(fg_sizer, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(10));
    m_sizer_box->Add(m_sizer_custom, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(10));
    m_sizer_box->Add(m_custom_cp, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(16));
    m_sizer_box->Add(0, 0, 0, wxTOP, FromDIP(10));


    m_def_color_box->SetSizer(m_sizer_box);
    m_def_color_box->Layout();
    m_def_color_box->Fit();

    m_sizer_main->Add(m_def_color_box, 0, wxALL | wxEXPAND, 10);
    SetSizer(m_sizer_main);
    Layout();
    Fit();

    Bind(wxEVT_PAINT, &ColorPickerPopup::paintEvent, this);
    wxGetApp().UpdateDarkUIWin(this);
}

void ColorPickerPopup::on_custom_clr_picker(wxMouseEvent& event)
{
    std::vector<std::string> colors = wxGetApp().app_config->get_custom_color_from_config();
    for (int i = 0; i < colors.size(); i++) {
        m_clrData->SetCustomColour(i, string_to_wxColor(colors[i]));
    }
    auto clr_dialog = new wxColourDialog(nullptr, m_clrData);
    wxColour picker_color;

    if (clr_dialog->ShowModal() == wxID_OK) {
        m_clrData = &(clr_dialog->GetColourData());
        if (colors.size() != CUSTOM_COLOR_COUNT) {
            colors.resize(CUSTOM_COLOR_COUNT);
        }
        for (int i = 0; i < CUSTOM_COLOR_COUNT; i++) {
            colors[i] = color_to_string(m_clrData->GetCustomColour(i));
        }
        wxGetApp().app_config->save_custom_color_to_config(colors);

        picker_color = wxColour(
            m_clrData->GetColour().Red(),
            m_clrData->GetColour().Green(),
            m_clrData->GetColour().Blue(),
            255
        );

        if (picker_color.Alpha() == 0) {
             m_ts_stbitmap_custom->Show();
        }
        else {
            m_ts_stbitmap_custom->Hide();
            m_custom_cp->SetBackgroundColor(picker_color);
        }

        set_def_colour(picker_color);
        wxCommandEvent evt(EVT_SELECTED_COLOR);
        unsigned long g_col = ((picker_color.Red() & 0xff) << 24) + ((picker_color.Green() & 0xff) << 16) + ((picker_color.Blue() & 0xff) << 8) + (picker_color.Alpha() & 0xff);
        evt.SetInt(g_col);
        wxPostEvent(GetParent(), evt);
    }
}

void ColorPickerPopup::set_ams_colours(std::vector<wxColour> ams)
{
    if (m_ams_color_pickers.size() > 0) {
        for (ColorPicker* col_pick:m_ams_color_pickers) {

            std::vector<ColorPicker*>::iterator iter = find(m_color_pickers.begin(), m_color_pickers.end(), col_pick);
            if (iter != m_color_pickers.end()) {
                col_pick->Destroy();
                m_color_pickers.erase(iter);
            }
        }

        m_ams_color_pickers.clear();
    }


    m_ams_colors = ams;
    for (wxColour col : m_ams_colors) {
        auto cp = new ColorPicker(m_def_color_box, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        cp->set_color(col);
        cp->set_selected(false);
        cp->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(238,238,238)));
        m_color_pickers.push_back(cp);
        m_ams_color_pickers.push_back(cp);
        m_ams_fg_sizer->Add(cp, 0, wxALL, FromDIP(3));
        cp->Bind(wxEVT_LEFT_DOWN, [this, cp](auto& e) {
            set_def_colour(cp->m_colour);

            wxCommandEvent evt(EVT_SELECTED_COLOR);
            unsigned long g_col = ((cp->m_colour.Red() & 0xff) << 24) + ((cp->m_colour.Green() & 0xff) << 16) + ((cp->m_colour.Blue() & 0xff) << 8) + (cp->m_colour.Alpha() & 0xff);
            evt.SetInt(g_col);
            wxPostEvent(GetParent(), evt);
        });
    }
    m_ams_fg_sizer->Layout();
    Layout();
    Fit();
}

void ColorPickerPopup::set_def_colour(wxColour col)
{
    m_def_col = col;

    for (ColorPicker* cp : m_color_pickers) {
        if (cp->m_selected) {
            cp->set_selected(false);
        }
    }

    for (ColorPicker* cp : m_color_pickers) {
        if (cp->m_colour == m_def_col) {
            cp->set_selected(true);
            break;
        }
    }

    if (m_def_col.Alpha() == 0) {
        m_ts_stbitmap_custom->Show();
    }
    else {
        m_ts_stbitmap_custom->Hide();
        m_custom_cp->SetBackgroundColor(m_def_col);
    }

    Dismiss();
}

void ColorPickerPopup::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}

void ColorPickerPopup::OnDismiss() {}

void ColorPickerPopup::Popup()
{
    PopupWindow::Popup();
}

bool ColorPickerPopup::ProcessLeftDown(wxMouseEvent& event) {
    return PopupWindow::ProcessLeftDown(event);
}

}} // namespace Slic3r::GUI
