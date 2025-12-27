#include "SingleChoiceDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"

#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

SingleChoiceDialog::SingleChoiceDialog(const wxString &message, const wxString &caption, const wxArrayString &choices, int initialSelection, wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, caption, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);

    const int   dlg_width = 200;
    wxBoxSizer *bSizer    = new wxBoxSizer(wxVERTICAL);
    bSizer->SetMinSize(wxSize(FromDIP(dlg_width), -1));

    wxStaticText *message_text = new wxStaticText(this, wxID_ANY, message, wxDefaultPosition, wxDefaultSize, 0);
    message_text->Wrap(-1);
    bSizer->Add(message_text, 0, wxALL, 5);

    type_comboBox = new ComboBox(this, wxID_ANY, choices[0], wxDefaultPosition, wxSize(FromDIP(dlg_width - 10), -1), 0, NULL, wxCB_READONLY);
    for (const wxString &type_name : choices) { type_comboBox->Append(type_name); }
    bSizer->Add(type_comboBox, 0, wxALL | wxALIGN_CENTER, 5);
    bSizer->AddSpacer(FromDIP(10));
    type_comboBox->SetSelection(initialSelection);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_OK); });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    bSizer->Add(dlg_btns, 0, wxEXPAND);

    this->SetSizer(bSizer);
    this->Layout();
    bSizer->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}
SingleChoiceDialog::~SingleChoiceDialog() {}
int SingleChoiceDialog::GetSingleChoiceIndex() { return this->ShowModal() == wxID_OK ? GetTypeComboBox()->GetSelection() : -1; }

void SingleChoiceDialog::on_dpi_changed(const wxRect &suggested_rect) {}
}} // namespace Slic3r::GUI