#include "calib_dlg.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "I18N.hpp"
#include <algorithm>
#include <sstream>
#include <iterator>
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include <wx/anybutton.h>
#include <wx/checklst.h>
#include "libslic3r/Config.hpp"
#include <cstddef>
#include "slic3r/GUI/GUI_Utils.hpp"
#include <wx/colour.h>
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include <slic3r/GUI/Widgets/LabeledCheckBox.hpp>
#include "slic3r/GUI/Widgets/LabeledStaticBox.hpp"
#include "slic3r/GUI/Widgets/RadioGroup.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include "slic3r/GUI/wxExtensions.hpp"
#include <wx/dcgraph.h>
#include "MainFrame.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "format.hpp"
#include "PresetComboBoxes.hpp"
#include "Tab.hpp"
#include "Widgets/DialogButtons.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/dialog.h>
#include <wx/valtext.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <wx/event.h>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Utils.hpp"
#include "Plater.hpp"

namespace Slic3r { namespace GUI {

std::vector<InputShaperType> input_shaper_types_for_flavor(GCodeFlavor flavor);

namespace {

void ParseStringValues(std::string str, std::vector<double> &vec)
{
    vec.clear();
    std::replace(str.begin(), str.end(), ',', ' ');
    std::istringstream inss(str);
    std::copy_if(std::istream_iterator<int>(inss), std::istream_iterator<int>(), std::back_inserter(vec),
                 [](int x){ return x > 0; });
}

int GetTextMax(wxWindow* parent, const std::vector<wxString>& labels)
{
    wxSize text_size;
    for (wxString label : labels)
        text_size.IncTo(parent->GetTextExtent(label));
    return text_size.x + parent->FromDIP(10);
}

LabeledCheckBox* add_scale_checkbox(wxWindow* parent, wxSizer* settings_sizer)
{
    auto cb   = new LabeledCheckBox(parent, _L("Auto-scale for nozzle"));
    cb->SetValue(true);
    cb->SetToolTip(_L("This model is designed around a 0.4 mm nozzle with a 0.2 mm layer height.\n"
                      "When the scaling option is enabled (recommended), it dynamically resizes to match your current nozzle diameter"
                      " and an appropriate layer height, making the test both accurate and easy to read.\n"
                      "Turn scaling off only if you wish to print the reference model exactly as-is."));
    settings_sizer->Add(cb, 0, wxLEFT | wxTOP, parent->FromDIP(3));
    return cb;
}

// The printer the tests run on: the edited printer preset, or an empty config before the presets load.
const DynamicPrintConfig& edited_printer_config()
{
    static const DynamicPrintConfig none;
    const auto* preset_bundle = wxGetApp().preset_bundle;
    return preset_bundle != nullptr ? preset_bundle->printers.get_edited_preset().config : none;
}

std::vector<wxString> make_shaper_type_labels()
{
    auto values = shaper_type_values(edited_printer_config());
    if (values.empty())
        values.emplace_back("");

    std::vector<wxString> labels;
    labels.reserve(values.size());
    for (const auto& label : values)
        labels.emplace_back(wxString::FromUTF8(label.c_str()));
    return labels;
}

// ORCA-Belt: PA Line / PA Pattern have belt plumbing in place (drawn on the
// belt surface via BeltKinematics world-coordinates mode) but are not
// validated yet — belt printers are restricted to the PA Tower for now.
bool is_belt_printer_selected()
{
    if (auto* preset_bundle = wxGetApp().preset_bundle) {
        const auto& cfg = preset_bundle->printers.get_edited_preset().config;
        const auto* opt = cfg.option<ConfigOptionBool>("belt_printer");
        return opt != nullptr && opt->value;
    }
    return false;
}

}

int calib_filament_extruder(size_t filament_id)
{
    PresetBundle&       bundle  = *wxGetApp().preset_bundle;
    const Plater&       plater  = *wxGetApp().plater();
    const auto*         nozzles = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return bed_exclusion_extruder_for_filament(filament_id, plater.get_global_filament_map(), plater.get_global_filament_map_mode(),
                                               bundle.is_bbl_vendor(), false, nozzles != nullptr ? nozzles->size() : 1);
}

CalibFilamentSelector::CalibFilamentSelector(wxWindow* parent) : m_parent(parent)
{
    m_sizer     = new wxBoxSizer(wxHORIZONTAL);
    auto* label = new wxStaticText(parent, wxID_ANY, _L("Filament: "));
    m_combo     = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, parent->FromDIP(wxSize(260, -1)), 0, nullptr,
                                 wxCB_READONLY);
    m_sizer->Add(label, 0, wxALL | wxALIGN_CENTER_VERTICAL, parent->FromDIP(2));
    m_sizer->Add(m_combo, 1, wxALL | wxALIGN_CENTER_VERTICAL, parent->FromDIP(2));
    refresh();
    m_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& event) {
        if (m_on_change)
            m_on_change();
        event.Skip();
    });
    // The dialogs are reused, so the list follows the project's filaments each time one opens.
    parent->Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown()) {
            refresh();
            if (m_on_change)
                m_on_change();
            m_parent->GetSizer()->SetSizeHints(m_parent);
        }
        event.Skip();
    });
}

void CalibFilamentSelector::refresh()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const size_t count = std::min(bundle.num_physical_filaments(), bundle.filament_presets.size());
    const auto*  nozzles = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    const bool   multi_extruder = nozzles != nullptr && nozzles->size() > 1;

    int selection = m_combo->GetSelection();
    if (selection < 0 || size_t(selection) >= count) {
        // Start on the filament the filament tab has open.
        Tab* tab  = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
        selection = tab != nullptr ? tab->get_combo_box()->get_filament_idx() : 0;
    }

    m_combo->Clear();
    const std::vector<wxBitmap*> icons = get_extruder_color_icons(true);
    for (size_t i = 0; i < count; ++i) {
        const Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[i]);
        wxString      name   = wxString::FromUTF8(preset != nullptr ? preset->label(false) : bundle.filament_presets[i]);
        if (const int extruder = calib_filament_extruder(i); multi_extruder && extruder >= 0)
            // TRN %1% is a filament preset name, %2% the number of the extruder that prints it.
            name = format_wxstr(_L("%1% (Extruder %2%)"), name, extruder + 1);
        m_combo->Append(name, i < icons.size() && icons[i] != nullptr ? *icons[i] : wxNullBitmap);
    }
    if (count > 0)
        m_combo->SetSelection(std::clamp(selection, 0, int(count) - 1));
    m_sizer->ShowItems(count > 1);
}

int CalibFilamentSelector::filament_id() const { return std::max(m_combo->GetSelection(), 0); }

int CalibFilamentSelector::extruder_id() const { return std::max(calib_filament_extruder(size_t(filament_id())), 0); }

void CalibFilamentSelector::apply_to(Calib_Params& params) const
{
    params.filament_id = filament_id();
    params.extruder_id = extruder_id();
}

const Preset* CalibFilamentSelector::preset() const
{
    // find_preset() returns the edited copy of the preset the filament tab has open.
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const size_t        id     = size_t(filament_id());
    return id < bundle.filament_presets.size() ? bundle.filaments.find_preset(bundle.filament_presets[id], true) : nullptr;
}

std::vector<std::string> shaper_type_values(const DynamicPrintConfig& printer)
{
    const ConfigOptionDef* def = printer.def()->get("input_shaping_type");
    if (const auto* gcode_flavor_option = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")) {
        auto types = input_shaper_types_for_flavor(gcode_flavor_option->value);
        if (!types.empty()) {
            std::vector<std::string> values;
            values.reserve(types.size());
            for (InputShaperType type : types) {
                if (type == InputShaperType::Disable)
                    continue;
                const size_t idx = static_cast<size_t>(type);
                if (def && idx < def->enum_values.size())
                    values.push_back(def->enum_values[idx]);
                else
                    values.push_back(std::to_string(static_cast<int>(type)));
            }
            if (!values.empty())
                return values;
        }
    }
    if (def && !def->enum_values.empty())
        return {def->enum_values.front()};
    return {"Default"};
}

PA_Calibration_Dlg::PA_Calibration_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("PA Calibration"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Extruder type Radio Group
    auto labeled_box_type = new LabeledStaticBox(this, _L("Extruder type"));
    auto type_box = new wxStaticBoxSizer(labeled_box_type, wxHORIZONTAL);

    m_rbExtruderType = new RadioGroup(this, {_L("DDE"), _L("Bowden")}, wxHORIZONTAL);
    type_box->Add(m_rbExtruderType, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(type_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Method Radio Group
    auto labeled_box_method = new LabeledStaticBox(this, _L("Method"));
    auto method_box = new wxStaticBoxSizer(labeled_box_method, wxHORIZONTAL);

	m_rbMethod = new RadioGroup(this, { _L("PA Tower"), _L("PA Line"), _L("PA Pattern") }, wxHORIZONTAL);
    method_box->Add(m_rbMethod, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(method_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString start_pa_str    = _L("Start PA: ");
    wxString end_pa_str      = _L("End PA: ");
    wxString PA_step_str     = _L("PA step: ");
    wxString sp_accel_str    = _L("Accelerations: ");
    wxString sp_speed_str    = _L("Speeds: ");
    wxString cb_print_no_str = _L("Print numbers");

    int text_max = GetTextMax(this, std::vector<wxString>{start_pa_str, end_pa_str, PA_step_str, sp_accel_str, sp_speed_str, cb_print_no_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // start PA
    auto start_PA_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_pa_text = new wxStaticText(this, wxID_ANY, start_pa_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStartPA = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_PROCESS_ENTER);
    m_tiStartPA->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
	start_PA_sizer->Add(start_pa_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_PA_sizer->Add(m_tiStartPA  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_PA_sizer, 0, wxLEFT, FromDIP(3));

    // end PA
    auto end_PA_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_pa_text = new wxStaticText(this, wxID_ANY, end_pa_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEndPA = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_PROCESS_ENTER);
    m_tiEndPA->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_PA_sizer->Add(end_pa_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_PA_sizer->Add(m_tiEndPA  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_PA_sizer, 0, wxLEFT, FromDIP(3));

    // PA step
    auto PA_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto PA_step_text = new wxStaticText(this, wxID_ANY, PA_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiPAStep = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_PROCESS_ENTER);
    m_tiPAStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    PA_step_sizer->Add(PA_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    PA_step_sizer->Add(m_tiPAStep  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(PA_step_sizer, 0, wxLEFT, FromDIP(3));

    // Print Numbers
    wxBoxSizer* cb_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto cb_title = new wxStaticText(this, wxID_ANY, cb_print_no_str, wxDefaultPosition, st_size, 0);
    m_cbPrintNum = new ::CheckBox(this);
    m_cbPrintNum->SetValue(false);
    m_cbPrintNum->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        (m_params.print_numbers) = (m_params.print_numbers) ? false : true;
        e.Skip();
    });
    cb_sizer->Add(cb_title      , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    cb_sizer->Add(m_cbPrintNum  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(cb_sizer, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(3));

    wxTextValidator val_list_validator(wxFILTER_INCLUDE_CHAR_LIST);
    val_list_validator.SetCharIncludes(wxString("0123456789,"));

    auto sp_accel_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto sp_accel_text = new wxStaticText(this, wxID_ANY, sp_accel_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiBMAccels = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_PROCESS_ENTER);
    m_tiBMAccels->SetToolTip(_L("Comma-separated list of printing accelerations"));
    m_tiBMAccels->GetTextCtrl()->SetValidator(val_list_validator);
    sp_accel_sizer->Add(sp_accel_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    sp_accel_sizer->Add(m_tiBMAccels , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(sp_accel_sizer, 0, wxLEFT, FromDIP(3));

    auto sp_speed_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto sp_speed_text = new wxStaticText(this, wxID_ANY, sp_speed_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiBMSpeeds = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_PROCESS_ENTER);
    m_tiBMSpeeds->SetToolTip(_L("Comma-separated list of printing speeds"));
    m_tiBMSpeeds->GetTextCtrl()->SetValidator(val_list_validator);
    sp_speed_sizer->Add(sp_speed_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    sp_speed_sizer->Add(m_tiBMSpeeds , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(sp_speed_sizer, 0, wxLEFT, FromDIP(3));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/pressure_advance_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &PA_Calibration_Dlg::on_start, this);

    PA_Calibration_Dlg::reset_params();

    // Connect Events
    m_rbExtruderType->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_extruder_type_changed), NULL, this);
    m_rbMethod->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_method_changed), NULL, this);
    this->Connect(wxEVT_SHOW, wxShowEventHandler(PA_Calibration_Dlg::on_show));
    
    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

PA_Calibration_Dlg::~PA_Calibration_Dlg() {
    // Disconnect Events
    m_rbExtruderType->Disconnect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_extruder_type_changed), NULL, this);
    m_rbMethod->Disconnect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_method_changed), NULL, this);
}

void PA_Calibration_Dlg::reset_params() {
    bool isDDE = m_rbExtruderType->GetSelection() == 0 ? true : false;
    int method = m_rbMethod->GetSelection();

    m_params.mode = method == 1 ? CalibMode::Calib_PA_Line : method == 2 ? CalibMode::Calib_PA_Pattern : CalibMode::Calib_PA_Tower;
    const Calib_Params defaults = calib_default_params(m_params.mode, edited_printer_config(), !isDDE);
    m_tiStartPA->GetTextCtrl()->SetValue(wxString::FromDouble(defaults.start));
    m_tiEndPA->GetTextCtrl()->SetValue(wxString::FromDouble(defaults.end));
    m_tiPAStep->GetTextCtrl()->SetValue(wxString::FromDouble(defaults.step));
    m_cbPrintNum->SetValue(defaults.print_numbers);
    m_cbPrintNum->Enable(m_params.mode == CalibMode::Calib_PA_Line);
    m_tiBMAccels->Enable(m_params.mode == CalibMode::Calib_PA_Pattern);
    m_tiBMSpeeds->Enable(m_params.mode == CalibMode::Calib_PA_Pattern);
}

void PA_Calibration_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStartPA->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEndPA->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiPAStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    switch (m_rbMethod->GetSelection()) {
        case 1:
            m_params.mode = CalibMode::Calib_PA_Line;
            break;
        case 2:
            m_params.mode = CalibMode::Calib_PA_Pattern;
            break;
        default:
            m_params.mode = CalibMode::Calib_PA_Tower;
    }
    m_params.print_numbers = m_cbPrintNum->GetValue();
    ParseStringValues(m_tiBMAccels->GetTextCtrl()->GetValue().ToStdString(), m_params.accelerations);
    ParseStringValues(m_tiBMSpeeds->GetTextCtrl()->GetValue().ToStdString(), m_params.speeds);

    const CalibParamsError error = read_double ? check_calib_params(m_params, edited_printer_config()) : CalibParamsError::Range;
    if (error == CalibParamsError::Range) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nStart PA: >= 0.0\nEnd PA: > Start PA\nPA step: >= 0.001"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    // ORCA-Belt: backstop in case the selection slipped past the UI guards.
    if (error == CalibParamsError::BeltMethod) {
        MessageDialog msg_dlg(nullptr, _L("PA Line and PA Pattern tests are not enabled yet on belt printers.\nPlease use the PA Tower method instead."),
                              wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    // Guards against swapped inputs: accelerations must exceed speeds.
    if (error == CalibParamsError::AccelBelowSpeed) {
        MessageDialog msg_dlg(nullptr,
            _L("Acceleration values must be greater than speed values.\nPlease verify the inputs."),
            wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_filament->apply_to(m_params);
    m_plater->calib_pa(m_params);
    EndModal(wxID_OK);

}
void PA_Calibration_Dlg::on_extruder_type_changed(wxCommandEvent& event) {
    PA_Calibration_Dlg::reset_params();
    event.Skip();
}
void PA_Calibration_Dlg::on_method_changed(wxCommandEvent& event) {
    // ORCA-Belt: only the PA Tower method is enabled on belt printers so far.
    if (is_belt_printer_selected() && m_rbMethod->GetSelection() != 0)
        m_rbMethod->SetSelection(0, true);
    PA_Calibration_Dlg::reset_params();
    event.Skip();
}

void PA_Calibration_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

void PA_Calibration_Dlg::on_show(wxShowEvent& event) {
    // ORCA-Belt: the dialog is cached across printer switches, so refresh the
    // belt restriction on every show.
    if (is_belt_printer_selected()) {
        m_rbMethod->SetSelection(0);
        const wxString tip = _L("Not enabled yet on belt printers — use the PA Tower method instead.");
        m_rbMethod->SetRadioTooltip(1, tip);
        m_rbMethod->SetRadioTooltip(2, tip);
    } else {
        m_rbMethod->SetRadioTooltip(1, wxEmptyString);
        m_rbMethod->SetRadioTooltip(2, wxEmptyString);
    }
    PA_Calibration_Dlg::reset_params();
    event.Skip();
}

// Temp calib dlg
//
Temp_Calibration_Dlg::Temp_Calibration_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Temperature calibration"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Belt temperature-tower model: Standard (sectioned tower) vs Overhang (engraved
    // inverted-L provini that stress overhang quality per temperature). Only affects
    // belt printers; the upright tower ignores it, so the picker is only shown on
    // belts. The dialog is cached across printer switches, so visibility is toggled
    // per-show in on_show() rather than gated here at construction time.
    auto labeled_box_model = new LabeledStaticBox(this, _L("Test model"));
    m_model_box = new wxStaticBoxSizer(labeled_box_model, wxHORIZONTAL);
    m_rbModel = new RadioGroup(this, { _L_CONTEXT("Standard", "Test model"), _L_CONTEXT("Overhang", "Test model") }, wxVERTICAL);
    m_model_box->Add(m_rbModel, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(m_model_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString start_temp_str = _L("Start temp: ");
    wxString end_temp_str   = _L("End temp: ");
    wxString temp_step_str  = _L("Temp step: ");
    int text_max = GetTextMax(this, std::vector<wxString>{start_temp_str, end_temp_str, temp_step_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));
    const Calib_Params defaults = calib_default_params(CalibMode::Calib_Temp_Tower, edited_printer_config());

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // start temp
    auto start_temp_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_temp_text = new wxStaticText(this, wxID_ANY, start_temp_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, wxString::FromDouble(defaults.start), _L("\u2103" /* °C */), "", wxDefaultPosition, ti_size);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    start_temp_sizer->Add(start_temp_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_temp_sizer->Add(m_tiStart      , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_temp_sizer, 0, wxLEFT, FromDIP(3));

    // end temp
    auto end_temp_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_temp_text = new wxStaticText(this, wxID_ANY, end_temp_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, wxString::FromDouble(defaults.end), _L("\u2103" /* °C */), "", wxDefaultPosition, ti_size);
    m_tiEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_temp_sizer->Add(end_temp_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_temp_sizer->Add(m_tiEnd      , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_temp_sizer, 0, wxLEFT, FromDIP(3));

    // temp step
    auto temp_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto temp_step_text = new wxStaticText(this, wxID_ANY, temp_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(5), _L("\u2103" /* °C */), "", wxDefaultPosition, ti_size);
    m_tiStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_tiStep->Enable(false);
    temp_step_sizer->Add(temp_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    temp_step_sizer->Add(m_tiStep      , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(temp_step_sizer, 0, wxLEFT, FromDIP(3));

    // Resize the model to the nozzle diameter (recommended)
    m_cbResize = add_scale_checkbox(this, settings_sizer);

    settings_sizer->AddSpacer(FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/temp_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Temp_Calibration_Dlg::on_start, this);

    // Start from the recommended range of the filament being calibrated; keep typed temperatures until it changes.
    m_filament->set_on_change([this, last_filament = std::string()]() mutable {
        const Preset*     preset   = m_filament->preset();
        const std::string filament = preset != nullptr ? preset->name : std::string();
        if (filament == last_filament)
            return;
        last_filament = filament;
        // In the tower's 5 °C steps, or PLA's range when the filament's leaves no room for a test.
        int         start_temp = 230;
        int         end_temp   = 190;
        const auto* low        = preset != nullptr ? preset->config.option<ConfigOptionInts>("nozzle_temperature_range_low") : nullptr;
        const auto* high       = preset != nullptr ? preset->config.option<ConfigOptionInts>("nozzle_temperature_range_high") : nullptr;
        if (low != nullptr && high != nullptr && !low->values.empty() && !high->values.empty()) {
            const int lo = std::max((low->values.front() + 4) / 5 * 5, 155);
            const int hi = std::min(high->values.front() / 5 * 5, 500);
            if (hi >= lo + 5) {
                start_temp = hi;
                end_temp   = lo;
            }
        }
        m_tiStart->GetTextCtrl()->SetValue(std::to_string(start_temp));
        m_tiEnd->GetTextCtrl()->SetValue(std::to_string(end_temp));
    });

    // Refresh the belt-only model picker on every show — the dialog is cached and
    // reused across printer switches.
    this->Connect(wxEVT_SHOW, wxShowEventHandler(Temp_Calibration_Dlg::on_show));
    m_model_box->ShowItems(is_belt_printer_selected());

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);

    auto validate_text = [](TextInput* ti){
        unsigned long t = 0;
        if(!ti->GetTextCtrl()->GetValue().ToULong(&t))
            return;
        if (const unsigned long clamped = (unsigned long) clamp_calib_temp(double(t)); clamped != t) {
            // TRN %s is the temperature unit
            MessageDialog msg_dlg(nullptr, wxString::Format(_L("Supported range: 170%s - 500%s"),
                _L("\u2103" /* °C */), _L("\u2103" /* °C */)),
                wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            t = clamped;
        }
        t = (unsigned long) round_calib_temp(double(t));
        ti->GetTextCtrl()->SetValue(std::to_string(t));
    };

    m_tiStart->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [&](wxFocusEvent &e) {
        validate_text(this->m_tiStart);
        e.Skip();
        });

    m_tiEnd->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [&](wxFocusEvent &e) {
        validate_text(this->m_tiEnd);
        e.Skip();
        });


}

void Temp_Calibration_Dlg::on_start(wxCommandEvent& event) {
    bool read_long = false;
    unsigned long start=0,end=0;
    read_long = m_tiStart->GetTextCtrl()->GetValue().ToULong(&start);
    read_long = read_long && m_tiEnd->GetTextCtrl()->GetValue().ToULong(&end);

    m_params.start = start;
    m_params.end = end;
    m_params.mode = CalibMode::Calib_Temp_Tower;
    if (!read_long || check_calib_params(m_params, edited_printer_config()) != CalibParamsError::None) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nStart temp: <= 500\nEnd temp: >= 155\nStart temp >= End temp + 5"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }
    m_params.nozzle_based_resize = m_cbResize->GetValue();
    // Picker only exists on belt printers; default non-belt to the Standard model.
    m_params.test_model = m_rbModel ? m_rbModel->GetSelection() : 0;
    m_filament->apply_to(m_params);
    m_plater->calib_temp(m_params);
    EndModal(wxID_OK);

}

void Temp_Calibration_Dlg::on_show(wxShowEvent& event) {
    // ORCA-Belt: the dialog is cached across printer switches, so refresh the
    // belt-only "Test model" picker on every show. The Overhang model only
    // applies to belt printers; hide it (and resize the dialog) otherwise.
    const bool belt = is_belt_printer_selected();
    if (m_model_box->AreAnyItemsShown() != belt) {
        m_model_box->ShowItems(belt);
        Layout();
        Fit();
        GetSizer()->SetSizeHints(this);
    }
    event.Skip();
}

void Temp_Calibration_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();

}


// MaxVolumetricSpeed_Test_Dlg
//

MaxVolumetricSpeed_Test_Dlg::MaxVolumetricSpeed_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Max volumetric speed test"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString start_vol_str = _L("Start volumetric speed: ");
    wxString end_vol_str   = _L("End volumetric speed: ");
    wxString vol_step_str  = _L_CONTEXT("Step", "Increment") + ": ";
    int text_max = GetTextMax(this, std::vector<wxString>{start_vol_str, end_vol_str, vol_step_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // start vol
    auto start_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_vol_text = new wxStaticText(this, wxID_ANY, start_vol_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    const Calib_Params defaults = calib_default_params(CalibMode::Calib_Vol_speed_Tower, edited_printer_config());
    m_tiStart = new TextInput(this, wxString::FromDouble(defaults.start), _L(u8"mm³/s"), "", wxDefaultPosition, ti_size);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_vol_sizer->Add(start_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_vol_sizer->Add(m_tiStart     , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_vol_sizer, 0, wxLEFT, FromDIP(3));

    // end vol
    auto end_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_vol_text = new wxStaticText(this, wxID_ANY, end_vol_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, wxString::FromDouble(defaults.end), _L(u8"mm³/s"), "", wxDefaultPosition, ti_size);
    m_tiEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_vol_sizer->Add(end_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_vol_sizer->Add(m_tiEnd     , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_vol_sizer, 0, wxLEFT, FromDIP(3));

    // vol step
    auto vol_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto vol_step_text = new wxStaticText(this, wxID_ANY, vol_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(defaults.step), _L(u8"mm³/s"), "", wxDefaultPosition, ti_size);
    m_tiStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    vol_step_sizer->Add(vol_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    vol_step_sizer->Add(m_tiStep     , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(vol_step_sizer, 0, wxLEFT, FromDIP(3));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/volumetric_speed_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &MaxVolumetricSpeed_Test_Dlg::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

MaxVolumetricSpeed_Test_Dlg::~MaxVolumetricSpeed_Test_Dlg() {
    // Disconnect Events
}

void MaxVolumetricSpeed_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    m_params.mode = CalibMode::Calib_Vol_speed_Tower;
    if (!read_double || check_calib_params(m_params, edited_printer_config()) != CalibParamsError::None) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstart > 0\nstep >= 0\nend > start + step"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_filament->apply_to(m_params);
    m_plater->calib_max_vol_speed(m_params);
    EndModal(wxID_OK);

}

void MaxVolumetricSpeed_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();

}


// VFA_Test_Dlg
//

VFA_Test_Dlg::VFA_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("VFA test"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString start_str    = _L("Start speed: ");
    wxString end_vol_str  = _L("End speed: ");
    wxString vol_step_str = _L_CONTEXT("Step", "Increment") + ": ";
    int text_max = GetTextMax(this, std::vector<wxString>{start_str, end_vol_str, vol_step_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // start vol
    auto start_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_vol_text = new wxStaticText(this, wxID_ANY, start_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    const Calib_Params defaults = calib_default_params(CalibMode::Calib_VFA_Tower, edited_printer_config());
    m_tiStart = new TextInput(this, wxString::FromDouble(defaults.start), _L("mm/s"), "", wxDefaultPosition, ti_size);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_vol_sizer->Add(start_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_vol_sizer->Add(m_tiStart     , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_vol_sizer, 0, wxLEFT, FromDIP(3));

    // end vol
    auto end_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_vol_text = new wxStaticText(this, wxID_ANY, end_vol_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, wxString::FromDouble(defaults.end), _L("mm/s"), "", wxDefaultPosition, ti_size);
    m_tiEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_vol_sizer->Add(end_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_vol_sizer->Add(m_tiEnd     , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_vol_sizer, 0, wxLEFT, FromDIP(3));

    // vol step
    auto vol_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto vol_step_text = new wxStaticText(this, wxID_ANY, vol_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(defaults.step), _L("mm/s"), "", wxDefaultPosition, ti_size);
    m_tiStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    vol_step_sizer->Add(vol_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    vol_step_sizer->Add(m_tiStep     , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(vol_step_sizer, 0, wxLEFT, FromDIP(3));

    // Resize the model to the nozzle diameter (recommended)
    m_cbResize = add_scale_checkbox(this, settings_sizer);

    // Auto-adjust parameters to the filament's max volumetric speed
    auto auto_adjust_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cbAutoAdjust = new LabeledCheckBox(this, _L("Auto-adjust to max volumetric speed"));
    m_cbAutoAdjust->SetValue(true);
    m_cbAutoAdjust->SetToolTip(_L("If the end speed would exceed the filament's maximum volumetric speed, automatically lower the layer "
                                  "height (keeping standard values and staying within the machine's limits) to reach it. If even the "
                                  "minimum layer height is not enough, lower the end speed instead."));
    settings_sizer->Add(m_cbAutoAdjust, 0, wxLEFT | wxTOP, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/vfa_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &VFA_Test_Dlg::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

VFA_Test_Dlg::~VFA_Test_Dlg()
{
    // Disconnect Events
}

void VFA_Test_Dlg::on_start(wxCommandEvent& event)
{
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    m_params.mode = CalibMode::Calib_VFA_Tower;
    if (!read_double || check_calib_params(m_params, edited_printer_config()) != CalibParamsError::None) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstart > 10\nstep >= 0\nend > start + step"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    // If the requested end speed would exceed the filament's maximum volumetric speed, the slicer clamps the
    // outer wall speed, so the upper blocks of the tower would all print at the same (clamped) speed instead of
    // the requested one. Depending on the "Auto-adjust" option, either fix it automatically or just warn.
    m_filament->apply_to(m_params);
    m_params.vfa_layer_height    = 0.0; // 0 = auto (nozzle/2); overridden below when auto-adjusting
    m_params.nozzle_based_resize = m_cbResize->GetValue();
    if (const auto* preset_bundle = wxGetApp().preset_bundle) {
        const auto& printer_config  = preset_bundle->printers.get_edited_preset().config;
        const bool    auto_adjust     = m_cbAutoAdjust->GetValue();
        const Preset* filament        = m_filament->preset();
        const auto&   filament_config = (filament != nullptr ? *filament : preset_bundle->filaments.get_edited_preset()).config;
        const VfaSpeedFit fit = fit_vfa_speed(m_params, printer_config, preset_bundle->prints.get_edited_preset().config, filament_config,
                                              auto_adjust,
                                              vfa_candidate_layer_heights(preset_bundle->prints, printer_config, m_params.extruder_id));

        auto confirm_clamp = [&](const wxString& question) -> bool {
            MessageDialog msg_dlg(nullptr,
                wxString::Format(_L("The end speed (%.0f mm/s) exceeds the filament's maximum volumetric speed "
                                    "(%.1f mm³/s), which limits the outer wall to about %.0f mm/s at this line width and "
                                    "layer height.\nSpeeds above this will be clamped, so the upper blocks of the tower "
                                    "will not print at the requested speed.\n\n%s"),
                                 m_params.end, fit.max_vol_speed, fit.speed_limit, question),
                _L("VFA test"), wxICON_WARNING | wxYES_NO | wxNO_DEFAULT);
            return msg_dlg.ShowModal() == wxID_YES;
        };

        switch (fit.kind) {
        case VfaSpeedFit::ReducedLayerHeight: {
            // Reducing the layer height is enough to reach the requested end speed.
            m_params.vfa_layer_height = fit.layer_height;
            MessageDialog msg_dlg(nullptr,
                wxString::Format(_L("The end speed (%.0f mm/s) exceeds the filament's maximum volumetric speed "
                                    "(%.1f mm³/s) at the default layer height (%.2f mm).\n\n"
                                    "The layer height has been reduced to %.2f mm (a value used by this printer's "
                                    "profiles) so the tower can reach the requested speed."),
                                 m_params.end, fit.max_vol_speed, fit.default_lh, fit.layer_height),
                _L("VFA test"), wxICON_INFORMATION | wxOK);
            msg_dlg.ShowModal();
            break;
        }
        case VfaSpeedFit::LoweredEnd: {
            // Even the smallest available layer height cannot reach the end speed: propose a lower end speed.
            MessageDialog msg_dlg(nullptr,
                wxString::Format(_L("Even at the smallest layer height used by this printer's profiles (%.2f mm) the "
                                    "end speed (%.0f mm/s) exceeds the filament's maximum volumetric speed "
                                    "(%.1f mm³/s).\n\n"
                                    "The layer height will be set to %.2f mm and the end speed lowered to %.0f mm/s.\n\n"
                                    "Continue?"),
                                 fit.layer_height, m_params.end, fit.max_vol_speed, fit.layer_height, fit.end),
                _L("VFA test"), wxICON_WARNING | wxYES_NO | wxNO_DEFAULT);
            if (msg_dlg.ShowModal() != wxID_YES)
                return;
            m_params.end              = fit.end;
            m_params.vfa_layer_height = fit.layer_height;
            break;
        }
        case VfaSpeedFit::Clamped:
            // No compatible process profiles to draw layer heights from, auto-adjust off, or resizing disabled
            // (which forbids changing the layer height): warn and let the user decide.
            if (!confirm_clamp(auto_adjust && m_params.nozzle_based_resize ? _L("Continue anyway?")
                               : m_params.nozzle_based_resize
                                   ? _L("Enable \"Auto-adjust\" to fix this automatically, or continue anyway?")
                                   : _L("Enable \"Auto-scale for nozzle\" and \"Auto-adjust\" to fix this automatically, or continue anyway?")))
                return;
            break;
        case VfaSpeedFit::Fits: break;
        }
    }

    m_plater->calib_VFA(m_params);
    EndModal(wxID_OK);
}

void VFA_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect)
{
    this->Refresh();
    Fit();
}



// Retraction_Test_Dlg
//

Retraction_Test_Dlg::Retraction_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Retraction"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString start_length_str = _L("Start retraction length: ");
    wxString end_length_str   = _L("End retraction length: ");
    wxString length_step_str  = _L_CONTEXT("Step", "Increment") + ": ";
    int text_max = GetTextMax(this, std::vector<wxString>{start_length_str, end_length_str, length_step_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // start length
    auto start_length_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_length_text = new wxStaticText(this, wxID_ANY, start_length_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    const Calib_Params defaults = calib_default_params(CalibMode::Calib_Retraction_tower, edited_printer_config());
    m_tiStart = new TextInput(this, wxString::FromDouble(defaults.start), _L("mm"), "", wxDefaultPosition, ti_size);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_length_sizer->Add(start_length_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_length_sizer->Add(m_tiStart        , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_length_sizer, 0, wxLEFT, FromDIP(3));

    // end length
    auto end_length_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_length_text = new wxStaticText(this, wxID_ANY, end_length_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, wxString::FromDouble(defaults.end), _L("mm"), "", wxDefaultPosition, ti_size);
    m_tiEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_length_sizer->Add(end_length_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_length_sizer->Add(m_tiEnd        , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_length_sizer, 0, wxLEFT, FromDIP(3));

    // length step
    auto length_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto length_step_text = new wxStaticText(this, wxID_ANY, length_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(defaults.step), _L("mm"), "", wxDefaultPosition, ti_size);
    m_tiStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    length_step_sizer->Add(length_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    length_step_sizer->Add(m_tiStep        , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(length_step_sizer, 0, wxLEFT, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/retraction_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Retraction_Test_Dlg::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

Retraction_Test_Dlg::~Retraction_Test_Dlg() {
    // Disconnect Events
}

void Retraction_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    m_params.mode = CalibMode::Calib_Retraction_tower;
    if (!read_double || check_calib_params(m_params, edited_printer_config()) != CalibParamsError::None) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstart > 0\nstep >= 0\nend > start + step"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_filament->apply_to(m_params);
    m_plater->calib_retraction(m_params);
    EndModal(wxID_OK);

}

void Retraction_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();

}

// IroningFlow_Test_Dlg
//

IroningFlow_Test_Dlg::IroningFlow_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Ironing flow test"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);

    // Settings
    wxString start_str  = _L("Minimum ironing flow: ");
    wxString end_str    = _L("Maximum ironing flow: ");
    wxString length_str = _L("Length: ");
    int text_max = GetTextMax(this, std::vector<wxString>{start_str, end_str, length_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // minimum flow
    auto start_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_text = new wxStaticText(this, wxID_ANY, start_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, std::to_string(0), _L("%"), "", wxDefaultPosition, ti_size);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    start_sizer->Add(start_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_sizer->Add(m_tiStart , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_sizer, 0, wxLEFT, FromDIP(3));

    // maximum flow
    auto end_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_text = new wxStaticText(this, wxID_ANY, end_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, std::to_string(70), _L("%"), "", wxDefaultPosition, ti_size);
    m_tiEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_sizer->Add(end_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_sizer->Add(m_tiEnd , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_sizer, 0, wxLEFT, FromDIP(3));

    // length of the ironed band
    auto length_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto length_text = new wxStaticText(this, wxID_ANY, length_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiLength = new TextInput(this, std::to_string(140), _L("mm"), "", wxDefaultPosition, ti_size);
    m_tiLength->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    length_sizer->Add(length_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    length_sizer->Add(m_tiLength , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(length_sizer, 0, wxLEFT, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    auto note_text = new wxStaticText(this, wxID_ANY,
        _L("The ironing flow grows from the minimum at the left end of the band to the maximum at its right end. The longer "
           "the band, the easier it is to read. Check the result by looking at light reflected on the test piece. The "
           "optimum value is high enough to hide the lines of the surface, but low enough to avoid roughness by "
           "overextrusion. In between these two values, the lower ones are matte, the higher ones are glossier."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    note_text->SetForegroundColour(wxColour(128, 128, 128));
    note_text->Wrap(FromDIP(300));
    settings_sizer->Add(note_text, 0, wxALL, FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/ironing_flow_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &IroningFlow_Test_Dlg::on_start, this);

    SetSizerAndFit(v_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void IroningFlow_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    // step holds the length of the ironed band
    read_double = read_double && m_tiLength->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    if (!read_double || m_params.start < 0 || m_params.end <= m_params.start || m_params.end > 100 || m_params.step < 10) {
        MessageDialog msg_dlg(this, _L("Please input valid values:\n0 <= minimum < maximum <= 100\nlength >= 10 mm"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }
    if (ironing_flow_calib_mark_step(m_params.start, m_params.end, m_params.step) == 0.) {
        // TRN: %1% is the minimum length of the ironed band, in mm.
        MessageDialog msg_dlg(this, format_wxstr(_L("Marks every 20%% would be less than 10 mm apart: use a length of at least %1% mm."),
                                                 int(std::ceil((m_params.end - m_params.start) / 2.))),
                              wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }
    // The band plus 5 mm at each end.
    const double       length = m_params.step + 10.;
    const BoundingBoxf bed    = get_extents(wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("printable_area")->values);
    if (length > bed.size().x() - 10.) {
        // TRN: The placeholders are the length of the ironed band, the length of the whole test and the longest band that fits, in mm.
        MessageDialog msg_dlg(this, format_wxstr(_L("A band %1% mm long makes a test %2% mm long, which does not fit on the bed. Use at most %3% mm."),
                                                 int(m_params.step), int(length), int(bed.size().x() - 20.)),
                              wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_params.mode = CalibMode::Calib_Ironing_Flow;
    m_plater->calib_ironing_flow(m_params);
    EndModal(wxID_OK);
}

void IroningFlow_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    m_tiStart->Rescale();
    m_tiEnd->Rescale();
    m_tiLength->Rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

// Input_Shaping_Freq_Test_Dlg
//

Input_Shaping_Freq_Test_Dlg::Input_Shaping_Freq_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Input shaping Frequency test"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    const auto* preset_bundle = wxGetApp().preset_bundle;
    const auto* gcode_flavor_option = (preset_bundle != nullptr)
        ? preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")
        : nullptr;
    const bool reprap_firmware = gcode_flavor_option && gcode_flavor_option->value == GCodeFlavor::gcfRepRapFirmware;

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Model selection
    auto labeled_box_model = new LabeledStaticBox(this, _L("Test model"));
    auto model_box = new wxStaticBoxSizer(labeled_box_model, wxHORIZONTAL);

    m_rbModel = new RadioGroup(this, { _L("Ringing Tower"), _L("Fast Tower") }, wxVERTICAL);
    model_box->Add(m_rbModel, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(model_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Input shaper type selection
    auto labeled_box_type = new LabeledStaticBox(this, _L("Input shaper type"));
    auto type_box = new wxStaticBoxSizer(labeled_box_type, wxVERTICAL);
    auto type_labels = make_shaper_type_labels();
    m_rbType = new RadioGroup(this, type_labels, wxVERTICAL, 3);
    type_box->Add(m_rbType, 0, wxALL | wxEXPAND, FromDIP(4));
    m_rbType->SetSelection(0);

    // Determine firmware-specific note
    wxString firmware_note = _L("Please ensure the selected type is compatible with your firmware version.");
    if (gcode_flavor_option) {
        switch (gcode_flavor_option->value) {
        case GCodeFlavor::gcfMarlinFirmware:
        case GCodeFlavor::gcfMarlinLegacy:
            firmware_note = _L("Marlin version => 2.1.2\nFixed-Time motion not yet implemented.");
            break;
        case GCodeFlavor::gcfKlipper:
            firmware_note = _L("Klipper version => 0.9.0");
            break;
        case GCodeFlavor::gcfRepRapFirmware:
            firmware_note = _L("RepRapFirmware version => 3.4.0\nCheck your firmware documentation for supported shaper types.");
            break;
        default:
            break;
        }
    }

    auto type_note = new wxStaticText(this, wxID_ANY, firmware_note, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    type_note->SetForegroundColour(wxColour(128, 128, 128));
    type_box->Add(type_note, 0, wxALL, FromDIP(5));

    v_sizer->Add(type_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString x_axis_str = reprap_firmware ? _L("Frequency (Start / End): ") : _L("X Start / End: ");
    wxString y_axis_str = _L("Y Start / End: ");
    int text_max = GetTextMax(this, std::vector<wxString>{x_axis_str, y_axis_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Frequency settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    const Calib_Params defaults = calib_default_params(CalibMode::Calib_Input_shaping_freq, edited_printer_config());

    // X axis frequencies
    auto x_freq_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_x_text = new wxStaticText(this, wxID_ANY, x_axis_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiFreqStartX = new TextInput(this, wxString::FromDouble(defaults.freqStartX), _L("Hz"), "", wxDefaultPosition, ti_size);
    m_tiFreqStartX->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_tiFreqEndX   = new TextInput(this, wxString::FromDouble(defaults.freqEndX), _L("Hz"), "", wxDefaultPosition, ti_size);
    m_tiFreqEndX->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    
    x_freq_sizer->Add(start_x_text  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    x_freq_sizer->Add(m_tiFreqStartX, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    x_freq_sizer->Add(m_tiFreqEndX  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(x_freq_sizer, 0, wxLEFT, FromDIP(3));

    // Y axis frequencies
    auto y_freq_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_y_text = new wxStaticText(this, wxID_ANY, y_axis_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiFreqStartY = new TextInput(this, wxString::FromDouble(defaults.freqStartY), _L("Hz"), "", wxDefaultPosition, ti_size);
    m_tiFreqStartY->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_tiFreqEndY =   new TextInput(this, wxString::FromDouble(defaults.freqEndY), _L("Hz"), "", wxDefaultPosition, ti_size);
    m_tiFreqEndY->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    y_freq_sizer->Add(start_y_text  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    y_freq_sizer->Add(m_tiFreqStartY, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    y_freq_sizer->Add(m_tiFreqEndY  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(y_freq_sizer, 0, wxLEFT, FromDIP(3));

    if (reprap_firmware) {
        m_tiFreqStartY->GetTextCtrl()->SetValue(m_tiFreqStartX->GetTextCtrl()->GetValue());
        m_tiFreqEndY->GetTextCtrl()->SetValue(m_tiFreqEndX->GetTextCtrl()->GetValue());
        start_y_text->Hide();
        m_tiFreqStartY->Hide();
        m_tiFreqEndY->Hide();
        settings_sizer->Hide(y_freq_sizer);
        start_x_text->SetLabel(_L("Frequency (Start / End): "));
        m_tiFreqStartX->GetTextCtrl()->SetToolTip(_L("RepRapFirmware uses the same frequency range for both axes."));
        m_tiFreqEndX->GetTextCtrl()->SetToolTip(_L("RepRapFirmware uses the same frequency range for both axes."));
    }

    // Damping Factor
    wxString damping_factor_str = _L("Damp: ");
    auto damping_factor_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto damping_factor_text = new wxStaticText(this, wxID_ANY, damping_factor_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiDampingFactor = new TextInput(this, wxString::Format("%.3f", defaults.start), "", "", wxDefaultPosition, ti_size);
    m_tiDampingFactor->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    
    damping_factor_sizer->Add(damping_factor_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    damping_factor_sizer->Add(m_tiDampingFactor  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(damping_factor_sizer, 0, wxLEFT, FromDIP(3));
    
    settings_sizer->AddSpacer(FromDIP(5));

    auto note_text = new wxStaticText(this, wxID_ANY, _L("Recommended: Set Damp to 0.\nThis will use the printer's default or saved value."), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    note_text->SetForegroundColour(wxColour(128, 128, 128));
    note_text->Wrap(FromDIP(350));
    settings_sizer->Add(note_text, 0, wxALL, FromDIP(5));

    settings_sizer->AddSpacer(FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/input_shaping_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Input_Shaping_Freq_Test_Dlg::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

Input_Shaping_Freq_Test_Dlg::~Input_Shaping_Freq_Test_Dlg() {
    // Disconnect Events
}

void Input_Shaping_Freq_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiFreqStartX->GetTextCtrl()->GetValue().ToDouble(&m_params.freqStartX);
    read_double = read_double && m_tiFreqEndX->GetTextCtrl()->GetValue().ToDouble(&m_params.freqEndX);

    const auto* preset_bundle = wxGetApp().preset_bundle;
    const auto* gcode_flavor_option = (preset_bundle != nullptr)
        ? preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")
        : nullptr;
    const bool reprap_firmware = gcode_flavor_option && gcode_flavor_option->value == GCodeFlavor::gcfRepRapFirmware;

    if (!reprap_firmware) {
        read_double = read_double && m_tiFreqStartY->GetTextCtrl()->GetValue().ToDouble(&m_params.freqStartY);
        read_double = read_double && m_tiFreqEndY->GetTextCtrl()->GetValue().ToDouble(&m_params.freqEndY);
    } else {
        m_params.freqStartY = m_params.freqStartX;
        m_params.freqEndY   = m_params.freqEndX;
        m_tiFreqStartY->GetTextCtrl()->SetValue(m_tiFreqStartX->GetTextCtrl()->GetValue());
        m_tiFreqEndY->GetTextCtrl()->SetValue(m_tiFreqEndX->GetTextCtrl()->GetValue());
    }
    read_double = read_double && m_tiDampingFactor->GetTextCtrl()->GetValue().ToDouble(&m_params.start);

    m_params.mode = CalibMode::Calib_Input_shaping_freq;
    const CalibParamsError error = read_double ? check_calib_params(m_params, edited_printer_config()) : CalibParamsError::Range;
    if (error == CalibParamsError::Range) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\n(0 < FreqStart < FreqEnd < 500)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    if (error == CalibParamsError::Damping) {
        MessageDialog msg_dlg(nullptr, _L("Please input a valid damping factor (0 < Damping/zeta factor <= 1)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    auto shaper_values = shaper_type_values(edited_printer_config());
    int type_selection = m_rbType->GetSelection();
    if (shaper_values.empty()) {
        m_params.shaper_type.clear();
    } else {
        if (type_selection < 0 || type_selection >= static_cast<int>(shaper_values.size()))
            type_selection = 0;
        m_params.shaper_type = shaper_values[static_cast<size_t>(type_selection)];
    }

    // Set model type based on selection
    m_params.test_model = m_rbModel->GetSelection() == 0 ? 0 : 1; // 0 = Ringing Tower, 1 = Fast Tower
    
    m_filament->apply_to(m_params);
    m_plater->calib_input_shaping_freq(m_params);
    EndModal(wxID_OK);
}

void Input_Shaping_Freq_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

// Input_Shaping_Damp_Test_Dlg
//

Input_Shaping_Damp_Test_Dlg::Input_Shaping_Damp_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Input shaping Damp test"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    const auto* preset_bundle = wxGetApp().preset_bundle;
    const auto* gcode_flavor_option = (preset_bundle != nullptr)
        ? preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")
        : nullptr;
    const bool reprap_firmware = gcode_flavor_option && gcode_flavor_option->value == GCodeFlavor::gcfRepRapFirmware;

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Model selection
    auto labeled_box_model = new LabeledStaticBox(this, _L("Test model"));
    auto model_box = new wxStaticBoxSizer(labeled_box_model, wxHORIZONTAL);

    m_rbModel = new RadioGroup(this, { _L("Ringing Tower"), _L("Fast Tower") }, wxVERTICAL);
    model_box->Add(m_rbModel, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(model_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Input shaper type selection
    auto labeled_box_type = new LabeledStaticBox(this, _L("Input shaper type"));
    auto type_box = new wxStaticBoxSizer(labeled_box_type, wxVERTICAL);
    auto type_labels = make_shaper_type_labels();
    m_rbType = new RadioGroup(this, type_labels, wxVERTICAL, 3);
    type_box->Add(m_rbType, 0, wxALL | wxEXPAND, FromDIP(4));
    m_rbType->SetSelection(0);

    // Determine firmware-specific note
    wxString firmware_note = _L("Check firmware compatibility.");
    if (gcode_flavor_option) {
        switch (gcode_flavor_option->value) {
        case GCodeFlavor::gcfMarlinFirmware:
        case GCodeFlavor::gcfMarlinLegacy:
            firmware_note = _L("Marlin version => 2.1.2\nFixed-Time motion not yet implemented.");
            break;
        case GCodeFlavor::gcfKlipper:
            firmware_note = _L("Klipper version => 0.9.0");
            break;
        case GCodeFlavor::gcfRepRapFirmware:
            firmware_note = _L("RepRapFirmware version => 3.4.0\nCheck your firmware documentation for supported shaper types.");
            break;
        default:
            break;
        }
    }

    auto type_note = new wxStaticText(this, wxID_ANY, firmware_note, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    type_note->SetForegroundColour(wxColour(128, 128, 128));
    type_box->Add(type_note, 0, wxALL, FromDIP(5));

    v_sizer->Add(type_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString freq_str = reprap_firmware ? _L("Frequency: ") : _L("Frequency X / Y: ");
    wxString damp_str = _L("Damp (Start / End): ");
    int text_max = GetTextMax(this, std::vector<wxString>{freq_str, damp_str});

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Frequency settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    const Calib_Params defaults = calib_default_params(CalibMode::Calib_Input_shaping_damp, edited_printer_config());

    auto freq_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto freq_text = new wxStaticText(this, wxID_ANY, freq_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiFreqX = new TextInput(this, wxString::FromDouble(defaults.freqStartX), _L("Hz"), "", wxDefaultPosition, ti_size);
    m_tiFreqX->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_tiFreqY = new TextInput(this, wxString::FromDouble(defaults.freqStartY), _L("Hz"), "", wxDefaultPosition, ti_size);
    m_tiFreqY->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    freq_sizer->Add(freq_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    freq_sizer->Add(m_tiFreqX, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    freq_sizer->Add(m_tiFreqY, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(freq_sizer, 0, wxLEFT, FromDIP(3));

    if (reprap_firmware) {
        m_tiFreqY->GetTextCtrl()->SetValue(m_tiFreqX->GetTextCtrl()->GetValue());
        m_tiFreqY->Hide();
        freq_text->SetLabel(freq_str);
        m_tiFreqX->GetTextCtrl()->SetToolTip(_L("RepRapFirmware uses the same frequency for both axes."));
    }

    // Damping Factor Start and End
    auto damp_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto damp_text = new wxStaticText(this, wxID_ANY, damp_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiDampingFactorStart = new TextInput(this, wxString::Format("%.3f", defaults.start), "", "", wxDefaultPosition, ti_size);
    m_tiDampingFactorStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_tiDampingFactorEnd   = new TextInput(this, wxString::Format("%.3f", defaults.end), "", "", wxDefaultPosition, ti_size);
    m_tiDampingFactorEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    damp_sizer->Add(damp_text             , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    damp_sizer->Add(m_tiDampingFactorStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    damp_sizer->Add(m_tiDampingFactorEnd  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(damp_sizer, 0, wxLEFT, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    // Add a note to explain users to use their previously calculated frequency
    auto note_text = new wxStaticText(this, wxID_ANY, _L("Note: Use previously calculated frequencies."), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    note_text->SetForegroundColour(wxColour(128, 128, 128));
    settings_sizer->Add(note_text, 0, wxALL, FromDIP(5));

    settings_sizer->AddSpacer(FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/input_shaping_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Input_Shaping_Damp_Test_Dlg::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

Input_Shaping_Damp_Test_Dlg::~Input_Shaping_Damp_Test_Dlg() {
    // Disconnect Events
}

void Input_Shaping_Damp_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiFreqX->GetTextCtrl()->GetValue().ToDouble(&m_params.freqStartX);
    const auto* preset_bundle = wxGetApp().preset_bundle;
    const auto* gcode_flavor_option = (preset_bundle != nullptr)
        ? preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")
        : nullptr;
    const bool reprap_firmware = gcode_flavor_option && gcode_flavor_option->value == GCodeFlavor::gcfRepRapFirmware;

    if (!reprap_firmware) {
        read_double = read_double && m_tiFreqY->GetTextCtrl()->GetValue().ToDouble(&m_params.freqStartY);
    } else {
        m_params.freqStartY = m_params.freqStartX;
        m_tiFreqY->GetTextCtrl()->SetValue(m_tiFreqX->GetTextCtrl()->GetValue());
    }
    read_double = read_double && m_tiDampingFactorStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiDampingFactorEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);

    m_params.mode = CalibMode::Calib_Input_shaping_damp;
    const CalibParamsError error = read_double ? check_calib_params(m_params, edited_printer_config()) : CalibParamsError::Range;
    if (error == CalibParamsError::Range) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\n(0 < Freq < 500)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    if (error == CalibParamsError::Damping) {
        MessageDialog msg_dlg(nullptr, _L("Please input a valid damping factor (0 <= DampingStart < DampingEnd <= 1)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    auto shaper_values = shaper_type_values(edited_printer_config());
    int type_selection = m_rbType->GetSelection();
    if (shaper_values.empty()) {
        m_params.shaper_type.clear();
    } else {
        if (type_selection < 0 || type_selection >= static_cast<int>(shaper_values.size()))
            type_selection = 0;
        m_params.shaper_type = shaper_values[static_cast<size_t>(type_selection)];
    }

    // Set model type based on selection
    m_params.test_model = m_rbModel->GetSelection() == 0 ? 0 : 1; // 0 = Ringing Tower, 1 = Fast Tower
    
    m_filament->apply_to(m_params);
    m_plater->calib_input_shaping_damp(m_params);
    EndModal(wxID_OK);
}

void Input_Shaping_Damp_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

// Cornering_Test_Dlg
//

Cornering_Test_Dlg::Cornering_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Cornering test"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Model selection
    auto labeled_box_model = new LabeledStaticBox(this, _L("Test model"));
    auto model_box = new wxStaticBoxSizer(labeled_box_model, wxHORIZONTAL);

    m_rbModel = new RadioGroup(this, { _L("Ringing Tower"), _L("Fast Tower"), _L("SCV-V2") }, wxVERTICAL);
    model_box->Add(m_rbModel, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(model_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    wxString start_jd_str = _L("Start: ");
    wxString end_jd_str   = _L("End: ");
    int text_max = GetTextMax(this, std::vector<wxString>{start_jd_str, end_jd_str});

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Cornering settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // Detect GCode Flavor and set appropriate values and units
    const auto* preset_bundle = wxGetApp().preset_bundle;
    const auto* gcode_flavor_option = (preset_bundle != nullptr)
        ? preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")
        : nullptr;

    // Junction Deviation in mm, or Classic Jerk in mm/s.
    const bool         junction_deviation = has_junction_deviation(edited_printer_config());
    const Calib_Params defaults           = calib_default_params(CalibMode::Calib_Cornering, edited_printer_config());
    const wxString     start_value_str    = wxString::Format("%.3f", defaults.start);
    const wxString     end_value_str      = wxString::Format("%.3f", defaults.end);
    const wxString     units_str          = junction_deviation ? "mm" : "mm/s";

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    // Start cornering
    auto start_jd_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_jd_text = new wxStaticText(this, wxID_ANY, start_jd_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiJDStart = new TextInput(this, start_value_str, units_str, "", wxDefaultPosition, ti_size);
    m_tiJDStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    start_jd_sizer->Add(start_jd_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_jd_sizer->Add(m_tiJDStart  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_jd_sizer, 0, wxLEFT, FromDIP(3));

    // End cornering
    auto end_jd_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_jd_text = new wxStaticText(this, wxID_ANY, end_jd_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiJDEnd = new TextInput(this, end_value_str, units_str, "", wxDefaultPosition, ti_size);
    m_tiJDEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_jd_sizer->Add(end_jd_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
       end_jd_sizer->Add(m_tiJDEnd  , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_jd_sizer, 0, wxLEFT, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    // Add note about cornering based on GCode Flavor
    wxString note_msg = _L("Note: Lower values = sharper corners but slower speeds.");
    auto note_text = new wxStaticText(this, wxID_ANY, note_msg, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    note_text->SetForegroundColour(wxColour(128, 128, 128));
    note_text->Wrap(FromDIP(300));
    settings_sizer->Add(note_text, 0, wxALL, FromDIP(5));

    if (gcode_flavor_option) {
        wxString note_msg_2;
        switch (gcode_flavor_option->value) {
            case GCodeFlavor::gcfMarlinFirmware: {
                if (junction_deviation) {
                    note_msg_2 += _L("Marlin 2 Junction Deviation detected:\nTo test Classic Jerk, set 'Maximum Junction Deviation' in Motion ability to 0.");
                } else {
                    note_msg_2 += _L("Marlin 2 Classic Jerk detected:\nTo test Junction Deviation, set 'Maximum Junction Deviation' in Motion ability to a value > 0.");
                }
                break;
            }
            case GCodeFlavor::gcfRepRapFirmware:
                note_msg_2 += _L("RepRap detected: Jerk in mm/s.\nOrcaSlicer will convert the values to mm/min when necessary.");
                break;
            default:
                break;
        }
        if(!note_msg_2.empty()){
            auto note_text_2 = new wxStaticText(this, wxID_ANY, note_msg_2, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
            note_text_2->SetForegroundColour(wxColour(128, 128, 128));
            note_text_2->Wrap(FromDIP(300));
            settings_sizer->Add(note_text_2, 0, wxALL, FromDIP(5));
        }
    }

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/cornering_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Cornering_Test_Dlg::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

Cornering_Test_Dlg::~Cornering_Test_Dlg() {
    // Disconnect Events
}

void Cornering_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiJDStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiJDEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);

    // Junction Deviation (mm) or Classic Jerk (mm/s) limits.
    const auto [max_end_value, warning_threshold] = cornering_end_limits(edited_printer_config());

    m_params.mode = CalibMode::Calib_Cornering;
    if (!read_double || check_calib_params(m_params, edited_printer_config()) != CalibParamsError::None) {
        wxString error_msg = wxString::Format(_L("Please input valid values:\n(0 <= Cornering <= %s)"), wxString::Format("%.3f", max_end_value));
        MessageDialog msg_dlg(nullptr, error_msg, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    } else if (m_params.end > warning_threshold) {
        wxString warning_msg = wxString::Format(_L("NOTE: High values may cause Layer shift (>%s)"), wxString::Format("%.3f", warning_threshold));
        MessageDialog msg_dlg(nullptr, warning_msg, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
    }

    // Set model type based on selection
    m_params.test_model = m_rbModel->GetSelection();

    m_filament->apply_to(m_params);
    m_plater->Calib_Cornering(m_params);
    EndModal(wxID_OK);
}

void Cornering_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

// FlowRateCalibrationDialog
//
FlowRateCalibrationDialog::FlowRateCalibrationDialog(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Flow Ratio Calibration"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 280)), wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE); // make sure background color set for dialog
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    m_filament = std::make_unique<CalibFilamentSelector>(this);
    v_sizer->Add(m_filament->sizer(), 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Type selection
    auto labeled_box_type = new LabeledStaticBox(this, _L("Calibration Test Type"));
    auto type_box = new wxStaticBoxSizer(labeled_box_type, wxVERTICAL);

    m_rbType = new RadioGroup(this, { _L("Pass 1 (Coarse)"), _L("Pass 2 (Fine)"), _L("YOLO (Recommended)"), _L("YOLO (Perfectionist)") }, wxVERTICAL, 1);
    m_rbType->SetSelection(2); // Default to YOLO Recommended
    type_box->Add(m_rbType, 0, wxALL | wxEXPAND, FromDIP(4));
    v_sizer->Add(type_box, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    // Settings
    auto stb = new LabeledStaticBox(this, _L("Settings"));
    auto settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    wxString pattern_str = _L("Top Surface Pattern");
    int text_max = GetTextMax(this, std::vector<wxString>{pattern_str});

    settings_sizer->AddSpacer(FromDIP(5));

    auto st_size = wxSize(text_max, -1);
    auto ti_size = FromDIP(wxSize(120, -1));

    // Top surface pattern
    auto pattern_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto pattern_text  = new wxStaticText(this, wxID_ANY, pattern_str, wxDefaultPosition, st_size, wxALIGN_LEFT);

    // ORCA: Use ComboBox with icons instead of RadioGroup
    m_rbPattern = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, ti_size, 0, nullptr, wxCB_READONLY);
    
    boost::filesystem::path image_path(Slic3r::resources_dir());
    image_path /= "images";

    auto add_pattern_item = [&](const std::string& name, const wxString& label) {
        auto icon_name = "param_" + name;
        if (boost::filesystem::exists(image_path / (icon_name + ".svg"))) {
            // Using 24px icon size to match other settings (Field.cpp uses 24)
            ScalableBitmap bm(this, icon_name, 24);
            m_rbPattern->Append(label, bm.bmp());
        } else {
            m_rbPattern->Append(label);
        }
    };

    add_pattern_item("archimedeanchords", _L("Archimedean Chords"));
    add_pattern_item("monotonic", _L("Monotonic"));
    m_rbPattern->SetSelection(0); // Default to Archimedean Chords
    // ORCA: explicit set value to ensure display on Windows
    m_rbPattern->SetValue(m_rbPattern->GetString(0));
    m_rbPattern->GetDropDown().SetUseContentWidth(true);

    pattern_sizer->Add(pattern_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    pattern_sizer->Add(m_rbPattern , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(pattern_sizer, 0, wxLEFT, FromDIP(3));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));

    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/flow_ratio_calib";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    bottom_sizer->Add(wiki_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &FlowRateCalibrationDialog::on_start, this);

    wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
    v_sizer->SetSizeHints(this);
}

FlowRateCalibrationDialog::~FlowRateCalibrationDialog() {
    // Disconnect Events
}

void FlowRateCalibrationDialog::on_start(wxCommandEvent& event) {
    int type = m_rbType->GetSelection();
    int patternIdx = m_rbPattern->GetSelection();
    
    InfillPattern pattern = ipArchimedeanChords;
    if (patternIdx == 1) pattern = ipMonotonic;
    
    bool is_linear = (type >= 2);
    int pass = (type % 2) + 1;

    m_plater->calib_flowrate(is_linear, pass, pattern, m_filament->filament_id(), m_filament->extruder_id());
    EndModal(wxID_OK);
}

void FlowRateCalibrationDialog::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
}

}} // namespace Slic3r::GUI
