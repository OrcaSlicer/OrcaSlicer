#include "RenamePrinterModelDialog.hpp"

#include <algorithm>
#include <cstring>

#include "libslic3r/PresetBundle.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

RenamePrinterModelDialog::RenamePrinterModelDialog(wxWindow* parent, const std::vector<std::string>& models)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Rename printer model"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_models(models)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY,
        _L("Rename a custom printer model. The new name is applied to every nozzle variant of that model."));
    msg->SetFont(Label::Body_13);
    msg->Wrap(FromDIP(360));
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(12));

    wxStaticText* model_label = new wxStaticText(this, wxID_ANY, _L("Printer model:"));
    model_label->SetFont(Label::Body_13);
    w_sizer->Add(model_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    wxArrayString choices;
    for (const std::string& m : models) choices.Add(from_u8(m));
    m_model_combo = new wxComboBox(this, wxID_ANY, choices.IsEmpty() ? wxString() : choices.front(),
        wxDefaultPosition, wxSize(FromDIP(360), -1), choices, wxCB_READONLY);
    if (!choices.IsEmpty()) m_model_combo->SetSelection(0);
    w_sizer->Add(m_model_combo, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    wxStaticText* name_label = new wxStaticText(this, wxID_ANY, _L("New name:"));
    name_label->SetFont(Label::Body_13);
    w_sizer->Add(name_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_name_input = new ::TextInput(this, wxString(), wxEmptyString, wxEmptyString, wxDefaultPosition,
        wxSize(FromDIP(360), -1), 0); // no wxTE_PROCESS_ENTER: let Enter trigger the dialog's OK button
    w_sizer->Add(m_name_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // Inline validation message, orange like SavePresetDialog.
    m_valid_label = new wxStaticText(this, wxID_ANY, "");
    m_valid_label->SetForegroundColour(wxColour(255, 111, 0));
    w_sizer->Add(m_valid_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok_btn = dlg_btns->GetOK();
    dlg_btns->GetOK()->Bind(    wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_ok_btn->IsEnabled()) EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    w_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(8));

    // Prefill the new-name field with the selected model and validate on every change.
    if (!choices.IsEmpty()) m_name_input->GetTextCtrl()->SetValue(choices.front());
    m_model_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        m_name_input->GetTextCtrl()->SetValue(m_model_combo->GetValue());
        update_valid();
    });
    m_name_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { update_valid(); e.Skip(); });

    SetSizer(w_sizer);
    Layout();
    w_sizer->Fit(this);
    update_valid();
    wxGetApp().UpdateDlgDarkUI(this);
}

void RenamePrinterModelDialog::update_valid()
{
    // Validate the RAW field text (so leading/trailing space is rejected, not silently trimmed),
    // mirroring SavePresetDialog::Item::update and reusing the same localized messages.
    const std::string name = into_u8(m_name_input->GetTextCtrl()->GetValue());
    const std::string old  = get_selected_model();
    wxString info;
    bool     ok = true; // false => invalid (OK disabled)

    const char *unusable_symbols = "<>[]:/\\|?*\"";
    // (SavePresetDialog also guards the "(modified)" suffix, but that check is dead at runtime: the
    // dirty marker is set to "* " in GUI_App.cpp and "*" is already an unusable symbol above, so it
    // can never be reached. Omitted here.)

    if (name.empty()) {
        info = _L("The name is not allowed to be empty.");
        ok = false;
    } else if (name.find_first_of(' ') == 0) {
        info = _L("The name is not allowed to start with space character.");
        ok = false;
    } else if (name.find_last_of(' ') == name.length() - 1) {
        info = _L("The name is not allowed to end with space character.");
        ok = false;
    }
    if (ok)
        for (size_t i = 0; i < std::strlen(unusable_symbols); ++i)
            if (name.find_first_of(unusable_symbols[i]) != std::string::npos) {
                info = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
                ok = false;
                break;
            }
    if (ok && name == "Default Printer") {
        info = _L("Name is unavailable.");
        ok = false;
    }
    if (ok) {
        const std::vector<std::string> sys = wxGetApp().preset_bundle->printers.system_printer_models();
        if (std::find(sys.begin(), sys.end(), name) != sys.end()) {
            info = _L("Overwriting a system profile is not allowed.");
            ok = false;
        }
    }
    // Renaming onto ANOTHER existing user model would merge their variants (and risk (model,variant)
    // collisions), so reject it.
    if (ok && name != old && std::find(m_models.begin(), m_models.end(), name) != m_models.end()) {
        info = _L("A printer model with this name already exists.");
        ok = false;
    }
    // Same as the current name: nothing to do; disable OK without flagging an error.
    if (ok && name == old)
        ok = false;

    m_valid_label->SetLabel(info);
    m_valid_label->Show(!info.IsEmpty());
    if (m_ok_btn) m_ok_btn->Enable(ok);
    Layout();
    Fit();
}

std::string RenamePrinterModelDialog::get_selected_model() const
{
    return into_u8(m_model_combo->GetValue());
}

std::string RenamePrinterModelDialog::get_new_name() const
{
    // OK is only enabled for a validated name (no leading/trailing space), so the raw value is clean.
    return into_u8(m_name_input->GetTextCtrl()->GetValue());
}

RenamePrinterModelDialog::~RenamePrinterModelDialog() {}

}} // namespace Slic3r::GUI
