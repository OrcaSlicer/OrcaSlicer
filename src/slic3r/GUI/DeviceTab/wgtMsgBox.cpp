#include "wgtMsgBox.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <wx/sizer.h>

namespace Slic3r::GUI
{

wgtMsgBox::wgtMsgBox(wxWindow* parent)
    : StaticBox(parent, wxID_ANY)
{
    CreateGUI();
}

void wgtMsgBox::CreateGUI()
{
    wxBoxSizer* mainSizer = new wxBoxSizer(wxHORIZONTAL);

    m_txt_label = new Label(this);
    m_txt_label->SetFont(::Label::Body_13);
    m_txt_label->SetBackgroundColour(*wxWHITE);

    auto padding_sizer = new wxBoxSizer(wxHORIZONTAL);
    padding_sizer->Add(m_txt_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    mainSizer->Add(padding_sizer, 0, wxALIGN_CENTER_VERTICAL | wxALL, 1);

    //Button* btnClose = CreateCloseButton();
    //wxButton* btnClose = CreateCloseButton();
    //mainSizer->Add(btnClose, 0, wxALIGN_CENTER_VERTICAL);

    SetSizer(mainSizer);
    mainSizer->Fit(this);
}

void wgtMsgBox::OnCloseClicked(wxCommandEvent& evt)
{
    this->Hide();
}

} // namespace Slic3r::GUI
