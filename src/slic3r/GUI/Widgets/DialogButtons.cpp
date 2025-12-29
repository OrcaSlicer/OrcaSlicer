#include "DialogButtons.hpp"

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI {

// ORCA standardize dialog buttons
DialogButtons::DialogButtons(wxWindow* parent, std::vector<wxString> non_translated_labels, const wxString& primary_btn_translated_label, const int left_aligned_buttons_count)
    : wxPanel(parent, wxID_ANY)
{
    m_parent  = parent;
    m_sizer   = new wxBoxSizer(wxHORIZONTAL);
    m_primary = primary_btn_translated_label;
    m_alert   = wxEmptyString;
    m_left_aligned_buttons_count = left_aligned_buttons_count;
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    // Add all to array
    for (wxString label : non_translated_labels) {
        Button* btn = new Button(this, _L(label));
        wxString l = label;
        l.LowerCase();
        auto f = m_standardIDs.find(l);
        if (f != m_standardIDs.end())
            btn->SetId(f->second);
        m_buttons.push_back(btn);
    }

    m_parent->Bind(wxEVT_DPI_CHANGED, &DialogButtons::on_dpi_changed, this);

    SetSizer(m_sizer);

    UpdateButtons();
}

DialogButtons::~DialogButtons() {
    m_parent->Unbind(wxEVT_DPI_CHANGED, &DialogButtons::on_dpi_changed, this);
}

void DialogButtons::on_dpi_changed(wxDPIChangedEvent& event) {
    UpdateButtons();
    event.Skip();
}

Button* DialogButtons::GetButtonFromID(wxStandardID id) {
    for (Button* btn : m_buttons)
        if (btn->GetId() == id)
            return btn;
    return nullptr;
}

Button* DialogButtons::GetButtonFromLabel(wxString translated_label) {
    for (Button* btn : m_buttons)
        if (btn->GetLabel() == translated_label)
            return btn;
    return nullptr;
}

Button* DialogButtons::GetButtonFromIndex(int index) {
    if(index >= 0 && index < m_buttons.size())
        return m_buttons[index];
    else
        return nullptr;
}

Button* DialogButtons::PickFromList(std::set<wxStandardID> ID_list) {
    // Picks first button from given list
    Button* b;
    for (auto itr : ID_list) {
        b = GetButtonFromID(itr);
        if (b != nullptr)
            return b;
    }
    return nullptr;
}

// shorthands for common buttons
Button* DialogButtons::GetOK()     {return GetButtonFromID(wxID_OK)      ;}
Button* DialogButtons::GetYES()    {return GetButtonFromID(wxID_YES)     ;}
Button* DialogButtons::GetAPPLY()  {return GetButtonFromID(wxID_APPLY)   ;}
Button* DialogButtons::GetCONFIRM(){return GetButtonFromID(wxID_APPLY)   ;}
Button* DialogButtons::GetNO()     {return GetButtonFromID(wxID_NO)      ;}
Button* DialogButtons::GetCANCEL() {return GetButtonFromID(wxID_CANCEL)  ;}
Button* DialogButtons::GetRETURN() {return GetButtonFromID(wxID_BACKWARD);} // gets Return button
Button* DialogButtons::GetNEXT()   {return GetButtonFromID(wxID_FORWARD) ;}
Button* DialogButtons::GetFIRST()  {return GetButtonFromIndex(0)         ;}
Button* DialogButtons::GetLAST()   {return GetButtonFromIndex(m_buttons.size() - 1);}

void DialogButtons::SetPrimaryButton(wxString translated_label) {
    // use _L("Create") translated text for custom buttons
    // use non existing button name to disable primary styled button
    Button* btn;
    if(translated_label.IsEmpty()){ // prefer standard primary buttons if label empty
        if(m_buttons.size() == 1)
            btn = m_buttons.front();
        else
            btn = PickFromList(m_primaryIDs);
    }else
        btn = GetButtonFromLabel(translated_label);

    if(btn == nullptr) return;

    m_primary = translated_label;

    // apply focus only if there is no focused element exist. this prevents stealing focus from input boxes
    if(m_parent->FindFocus() == nullptr)
        btn->SetFocus();
 
    btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
}

void DialogButtons::SetAlertButton(wxString translated_label) {
    // use _L("Create") translated text for custom buttons
    // use non existing button name to disable alert styled button
    if(m_buttons.size() == 1)
        return;
    Button* btn;
    if(translated_label.IsEmpty()){ // prefer standard alert buttons if label empty
        btn = PickFromList(m_alertIDs);
    }else
        btn = GetButtonFromLabel(translated_label);

    if(btn == nullptr) return;

    m_alert = translated_label;

    btn->SetStyle(ButtonStyle::Alert, ButtonType::Choice);
}

void DialogButtons::UpdateButtons() {
    m_sizer->Clear();
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    // Apply standard style to all
    for (Button* btn : m_buttons) {
        btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        btn->Bind(wxEVT_KEY_DOWN, &DialogButtons::on_keydown, this);
    }

    int btn_gap = FromDIP(ButtonProps::ChoiceButtonGap());

    if(m_left_aligned_buttons_count == 0)
        m_sizer->AddSpacer(btn_gap);

    for (int i = 0; i < m_buttons.size(); i++) {
        if(m_left_aligned_buttons_count == i)
            m_sizer->AddStretchSpacer();

        auto margin_to = (i >= m_left_aligned_buttons_count ? wxRIGHT : wxLEFT);
        m_sizer->Add(m_buttons[i], 0, margin_to | wxTOP | wxBOTTOM | wxALIGN_CENTER_VERTICAL, btn_gap);
    }

    SetPrimaryButton(m_primary);
    SetAlertButton(m_alert);

    Layout();
    Fit();
}

int DialogButtons::FromDIP(int d) {
    return m_parent->FromDIP(d);
}

void DialogButtons::SetLeftAlignedButtonsCount(int left_aligned_buttons_count){
    m_left_aligned_buttons_count = left_aligned_buttons_count;
    UpdateButtons();
}

// This might be helpful for future use

// Append Button

// Prepend Button

void DialogButtons::on_keydown(wxKeyEvent& e) {
    wxObject* current = e.GetEventObject();
    int key = e.GetKeyCode();
    int cnt = m_buttons.size();
    if(cnt > 1){
        int i = -1;
        for (Button* btn : m_buttons){
            i++;
            if(btn->HasFocus())
                break;
        }
        // possible issue if button hidden
        if      (key == WXK_LEFT)  {m_buttons[i - 1 < 0 ? (cnt - 1) : i - 1]->SetFocus();}
        else if (key == WXK_RIGHT) {m_buttons[i + 1 > (cnt - 1) ? 0 : i + 1]->SetFocus();}
    }
    e.Skip();
}

}} // namespace Slic3r::GUI