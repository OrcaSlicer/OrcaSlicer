#include "ModifierGCodeDialog.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include "GUI.hpp"
#include "GUI_App.hpp"

#include "Widgets/DialogButtons.hpp"

namespace Slic3r {
namespace GUI {

static wxTextCtrl *add_gcode_editor(wxWindow *parent, wxSizer *sizer, const wxString &label, const std::string &value, int border)
{
    wxStaticText *label_ctrl = new wxStaticText(parent, wxID_ANY, label);
    sizer->Add(label_ctrl, 0, wxLEFT | wxTOP | wxRIGHT, border);

    wxTextCtrl *editor = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(value), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE
#ifdef _WIN32
        | wxBORDER_SIMPLE
#endif
    );
    editor->SetFont(wxGetApp().code_font());
    editor->SetInsertionPointEnd();
    wxGetApp().UpdateDarkUI(editor);
    sizer->Add(editor, 1, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);

    return editor;
}

ModifierGCodeDialog::ModifierGCodeDialog(wxWindow *parent, const std::string &enter_gcode, const std::string &exit_gcode,
                                          const ModifierGCodeFeatureToggles &feature_toggles) :
    DPIDialog(parent, wxID_ANY, _L("Modifier Custom G-code"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDlgDarkUI(this);

    int border = 10;
    int em = em_unit();

    wxBoxSizer *topSizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *hint = new wxStaticText(this, wxID_ANY,
        _L("These G-code snippets are inserted whenever the toolpath starts or stops printing "
           "extrusions that belong to this modifier's region. Placeholders such as "
           "{layer_num} and {layer_z} are supported."));
    hint->Wrap(60 * em);
    topSizer->Add(hint, 0, wxEXPAND | wxALL, border);

    m_enter_gcode_editor = add_gcode_editor(this, topSizer, _L("Enter G-code") + ":", enter_gcode, border);
    m_exit_gcode_editor  = add_gcode_editor(this, topSizer, _L("Exit G-code") + " (" + _L("optional") + "):", exit_gcode, border);

    wxStaticText *feature_label = new wxStaticText(this, wxID_ANY, _L("Apply to:"));
    topSizer->Add(feature_label, 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);

    wxBoxSizer *feature_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto add_feature_checkbox = [this, feature_sizer](const wxString &label, bool checked) {
        wxCheckBox *checkbox = new wxCheckBox(this, wxID_ANY, label);
        checkbox->SetValue(checked);
        wxGetApp().UpdateDarkUI(checkbox);
        feature_sizer->Add(checkbox, 0, wxRIGHT, FromDIP(15));
        return checkbox;
    };
    m_walls_checkbox      = add_feature_checkbox(_L("Walls"), feature_toggles.walls);
    m_infill_checkbox     = add_feature_checkbox(_L("Infill"), feature_toggles.infill);
    m_support_checkbox    = add_feature_checkbox(_L("Support"), feature_toggles.support);
    m_skirt_brim_checkbox = add_feature_checkbox(_L("Skirt/Brim"), feature_toggles.skirt_brim);
    topSizer->Add(feature_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);

    topSizer->AddSpacer(border);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    topSizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);

    this->Fit();
    fit_in_display(*this, {70 * em, 55 * em});
    this->Layout();
    this->CenterOnScreen();
}

std::string ModifierGCodeDialog::get_enter_gcode() const
{
    return std::string(m_enter_gcode_editor->GetValue().ToUTF8());
}

std::string ModifierGCodeDialog::get_exit_gcode() const
{
    return std::string(m_exit_gcode_editor->GetValue().ToUTF8());
}

ModifierGCodeFeatureToggles ModifierGCodeDialog::get_feature_toggles() const
{
    ModifierGCodeFeatureToggles toggles;
    toggles.walls      = m_walls_checkbox->GetValue();
    toggles.infill     = m_infill_checkbox->GetValue();
    toggles.support    = m_support_checkbox->GetValue();
    toggles.skirt_brim = m_skirt_brim_checkbox->GetValue();
    return toggles;
}

void ModifierGCodeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int &em = em_unit();
    SetMinSize(wxSize(50 * em, 35 * em));
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
