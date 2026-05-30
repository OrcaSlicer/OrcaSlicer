#include "RenamePrinterModelDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

RenamePrinterModelDialog::RenamePrinterModelDialog(wxWindow* parent, const std::vector<std::string>& models)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Rename printer model"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
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

    m_name_input = new TextInput(this, wxString(), wxEmptyString, wxEmptyString, wxDefaultPosition,
        wxSize(FromDIP(360), -1), wxTE_PROCESS_ENTER);
    w_sizer->Add(m_name_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok_btn = dlg_btns->GetOK();
    dlg_btns->GetOK()->Bind(    wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_ok_btn->IsEnabled()) EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    w_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(8));

    // Prefill the new-name field with the selected model and keep OK state in sync.
    if (!choices.IsEmpty()) m_name_input->GetTextCtrl()->SetValue(choices.front());
    m_model_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        m_name_input->GetTextCtrl()->SetValue(m_model_combo->GetValue());
        update_ok_state();
    });
    m_name_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { update_ok_state(); e.Skip(); });

    SetSizer(w_sizer);
    Layout();
    w_sizer->Fit(this);
    update_ok_state();
    wxGetApp().UpdateDlgDarkUI(this);
}

void RenamePrinterModelDialog::update_ok_state()
{
    const std::string newn = get_new_name();
    const std::string oldn = get_selected_model();
    const bool valid = !newn.empty() && newn != oldn && !oldn.empty();
    if (m_ok_btn) m_ok_btn->Enable(valid);
}

std::string RenamePrinterModelDialog::get_selected_model() const
{
    return into_u8(m_model_combo->GetValue());
}

std::string RenamePrinterModelDialog::get_new_name() const
{
    std::string s = into_u8(m_name_input->GetTextCtrl()->GetValue());
    // trim surrounding whitespace
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

RenamePrinterModelDialog::~RenamePrinterModelDialog() {}

}} // namespace Slic3r::GUI
