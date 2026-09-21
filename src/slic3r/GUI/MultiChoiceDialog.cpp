#include "MultiChoiceDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"

namespace Slic3r { namespace GUI {

MultiChoiceDialog::MultiChoiceDialog(
    wxWindow*            parent,
    const wxString&      message,
    const wxString&      caption,
    const wxArrayString& choices
)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, caption, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    if(!message.IsEmpty()){
        wxStaticText *msg = new wxStaticText(this, wxID_ANY, message);
        msg->SetFont(Label::Body_13);
        msg->Wrap(-1);
        w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(10));
    }

    m_check_list = new CheckList(this, choices);

    w_sizer->Add(m_check_list, 1, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(    wxEVT_BUTTON, [this](wxCommandEvent &e) {EndModal(wxID_OK);});
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {EndModal(wxID_CANCEL);});

    w_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(w_sizer);
    Layout();
    w_sizer->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

wxArrayInt MultiChoiceDialog::GetSelections() const
{
    return m_check_list->GetSelections();
}

void MultiChoiceDialog::SetSelections(wxArrayInt sel_array)
{
    m_check_list->SetSelections(sel_array);
}

MultiChoiceDialog::~MultiChoiceDialog() {}

void MultiChoiceDialog::on_dpi_changed(const wxRect &suggested_rect) {}

}} // namespace Slic3r::GUI