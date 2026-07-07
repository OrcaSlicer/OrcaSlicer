#include "AddNozzleSizeDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

AddNozzleSizeDialog::AddNozzleSizeDialog(wxWindow* parent, const std::string& user_model, const std::vector<std::string>& addable_sizes)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Add nozzle size"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_sizes(addable_sizes)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Select the nozzle sizes to add to \"%s\":"), from_u8(user_model)));
    msg->SetFont(Label::Body_13);
    msg->Wrap(FromDIP(360));
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(12));

    wxArrayString choices;
    for (const std::string& s : m_sizes) choices.Add(from_u8(s) + " mm");
    m_check_list = new CheckList(this, choices);
    w_sizer->Add(m_check_list, 1, wxEXPAND | wxRIGHT | wxLEFT | wxTOP, FromDIP(12));

    // Custom-size entry for diameters the system model does not ship.
    wxStaticText* custom_label = new wxStaticText(this, wxID_ANY, _L("Can't find your nozzle size? Enter a custom size (mm):"));
    custom_label->SetFont(Label::Body_13);
    custom_label->Wrap(FromDIP(360));
    w_sizer->Add(custom_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_custom_input = new ::TextInput(this, wxString(), wxEmptyString, wxEmptyString, wxDefaultPosition,
        wxSize(FromDIP(120), -1), wxTE_PROCESS_ENTER);
    w_sizer->Add(m_custom_input, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(    wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    w_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(8));

    SetSizer(w_sizer);
    Layout();
    w_sizer->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

std::vector<std::string> AddNozzleSizeDialog::get_checked_sizes() const
{
    std::vector<std::string> out;
    for (int idx : m_check_list->GetSelections())
        if (idx >= 0 && idx < (int) m_sizes.size())
            out.push_back(m_sizes[idx]);
    return out;
}

std::string AddNozzleSizeDialog::get_custom_text() const
{
    std::string s = into_u8(m_custom_input->GetTextCtrl()->GetValue());
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

AddNozzleSizeDialog::~AddNozzleSizeDialog() {}

}} // namespace Slic3r::GUI
