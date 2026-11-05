#include "PrimitiveSizeDialog.hpp"

#include "Field.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>
#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/valtext.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {

static constexpr int input_width = 120;

// The number in the input, with either decimal separator; false when it is not a finite number.
static bool parse_input(const ::TextInput* input, double& value)
{
    const char dec_sep     = is_decimal_separator_point() ? '.' : ',';
    const char dec_sep_alt = dec_sep == '.' ? ',' : '.';
    wxString   str         = input->GetTextCtrl()->GetValue();
    str.Replace(dec_sep_alt, dec_sep, false);
    return str.ToDouble(&value) && std::isfinite(value);
}

PrimitiveSizeDialog::PrimitiveSizeDialog(wxWindow* parent, const wxString& title, const std::vector<Field>& fields)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(20));
    for (size_t i = 0; i < fields.size(); ++i) {
        auto input = new ::TextInput(this, double_to_string(fields[i].value, 2), _L("mm"), "", wxDefaultPosition,
                                     FromDIP(wxSize(input_width, -1)));
        input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        input->Bind(wxEVT_TEXT, [this, i](wxCommandEvent& e) {
            e.Skip();
            on_input_changed(i);
        });
        input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            if (read_values())
                EndModal(wxID_OK);
        });
        grid->Add(new wxStaticText(this, wxID_ANY, fields[i].label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(input, 0, wxALIGN_CENTER_VERTICAL);
        m_inputs.push_back(input);
        m_last_values.push_back(fields[i].value);
    }

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

    if (fields.size() > 1)
        m_uniform_scale = add_checkbox(sizer, _L("Uniform scale"), _L("Changing one dimension changes the others in proportion."));
    m_save_default = add_checkbox(sizer, _L("Save these settings as default"),
                                  _L("If enabled, these dimensions are used for this shape from now on, also when this dialog is not shown."));
    m_dont_show    = add_checkbox(sizer, _L("Don't show again"),
                                  _L("If enabled, shapes are added at their default size without asking. The dialog can be turned back on in Preferences."));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (read_values())
            EndModal(wxID_OK);
    });
    sizer->Add(dlg_btns, 0, wxEXPAND);
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    if (!m_inputs.empty()) {
        m_inputs.front()->GetTextCtrl()->SetFocus();
        m_inputs.front()->GetTextCtrl()->SelectAll();
    }
}

bool PrimitiveSizeDialog::save_as_default() const { return m_save_default->GetValue(); }

bool PrimitiveSizeDialog::dont_show_again() const { return m_dont_show->GetValue(); }

::CheckBox* PrimitiveSizeDialog::add_checkbox(wxSizer* sizer, const wxString& label, const wxString& tooltip)
{
    auto checkbox = new ::CheckBox(this);
    auto text     = new wxStaticText(this, wxID_ANY, label);
    checkbox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    auto toggle = [checkbox](wxMouseEvent&) { checkbox->SetValue(!checkbox->GetValue()); };
    text->Bind(wxEVT_LEFT_DOWN, toggle);
    text->Bind(wxEVT_LEFT_DCLICK, toggle);

    auto row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    return checkbox;
}

void PrimitiveSizeDialog::on_input_changed(size_t idx)
{
    double value = 0.;
    if (!parse_input(m_inputs[idx], value) || value <= 0.)
        return;
    if (m_uniform_scale && m_uniform_scale->GetValue()) {
        const double ratio = value / m_last_values[idx];
        for (size_t i = 0; i < m_inputs.size(); ++i)
            if (i != idx) {
                m_last_values[i] *= ratio;
                m_inputs[i]->GetTextCtrl()->ChangeValue(double_to_string(m_last_values[i], 2));
            }
    }
    m_last_values[idx] = value;
}

bool PrimitiveSizeDialog::read_values()
{
    std::vector<double> values;
    for (::TextInput* input : m_inputs) {
        double     value     = 0.;
        const bool is_number = parse_input(input, value);
        if (!is_number || value <= 0.) {
            MessageDialog msg_dlg(this, is_number ? _L("The size must be greater than zero") : _L("Invalid numeric input."),
                                  wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            input->GetTextCtrl()->SetFocus();
            input->GetTextCtrl()->SelectAll();
            return false;
        }
        values.push_back(value);
    }
    m_values = std::move(values);
    return true;
}

void PrimitiveSizeDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    for (::TextInput* input : m_inputs) {
        input->SetMinSize(FromDIP(wxSize(input_width, -1)));
        input->Rescale();
    }
    if (m_uniform_scale)
        m_uniform_scale->Rescale();
    m_save_default->Rescale();
    m_dont_show->Rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

}} // namespace Slic3r::GUI
