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

PrimitiveSizeDialog::PrimitiveSizeDialog(wxWindow* parent, const wxString& title, const std::vector<Field>& fields)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(20));
    for (const Field& field : fields) {
        auto input = new ::TextInput(this, double_to_string(field.value, 2), _L("mm"), "", wxDefaultPosition,
                                     FromDIP(wxSize(input_width, -1)));
        input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            if (read_values())
                EndModal(wxID_OK);
        });
        grid->Add(new wxStaticText(this, wxID_ANY, field.label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(input, 0, wxALIGN_CENTER_VERTICAL);
        m_inputs.push_back(input);
    }

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

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

bool PrimitiveSizeDialog::read_values()
{
    const char dec_sep     = is_decimal_separator_point() ? '.' : ',';
    const char dec_sep_alt = dec_sep == '.' ? ',' : '.';

    std::vector<double> values;
    for (::TextInput* input : m_inputs) {
        wxString str = input->GetTextCtrl()->GetValue();
        str.Replace(dec_sep_alt, dec_sep, false);
        double     value     = 0.;
        const bool is_number = str.ToDouble(&value) && std::isfinite(value);
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
    m_save_default->Rescale();
    m_dont_show->Rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

}} // namespace Slic3r::GUI
