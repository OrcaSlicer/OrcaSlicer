#include "CheckList.hpp"

#include "slic3r/GUI/GUI_App.hpp"

/*
TODO
option to hiding top toolbar
*/

CheckList::CheckList(
    wxWindow* parent,
    const wxArrayString& choices,
    long scroll_style
)
    : wxWindow(parent, wxID_ANY)
    , m_search(this, "search", 16)
    , m_menu(this, "filter", 16)
    , m_first_load(true)
{
    Freeze();
    w_sizer = new wxBoxSizer(wxVERTICAL);

    f_sizer = new wxBoxSizer(wxHORIZONTAL);
    f_bar   = new wxPanel(this, wxID_ANY);
    f_bar->SetBackgroundColour(parent->GetBackgroundColour());
    m_filter_box = new TextInput(f_bar, "", "", "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_filter_box->SetIcon(m_search.bmp());
    m_filter_box->SetMinSize(FromDIP(wxSize(200,24)));
    m_filter_box->SetSize(FromDIP(wxSize(-1,24)));
    m_filter_box->SetFocus();
    m_filter_ctrl = m_filter_box->GetTextCtrl();
    m_filter_ctrl->SetFont(Label::Body_13);
    m_filter_ctrl->SetSize(wxSize(-1, FromDIP(16))); // Centers text vertically
    m_filter_ctrl->SetHint(_L("Type to filter..."));
    m_filter_ctrl->Bind(wxEVT_TEXT,       [this](auto &e) {Filter(m_filter_ctrl->GetValue());});
    m_filter_ctrl->Bind(wxEVT_TEXT_ENTER, [this](auto &e) {Filter(m_filter_ctrl->GetValue());});
    m_filter_ctrl->Bind(wxEVT_SET_FOCUS,  [this](auto &e) {Filter(m_filter_ctrl->GetValue());e.Skip();});
    m_filter_ctrl->Bind(wxEVT_KILL_FOCUS, [this](auto &e) {Filter(m_filter_ctrl->GetValue());e.Skip();});
    f_sizer->Add(m_filter_box, 1, wxEXPAND);
    Bind(wxEVT_SET_FOCUS,  [this](auto &e) {m_filter_box->SetFocus();});

    fb_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto create_btn = [this] (wxString title, bool select){
        auto btn = new wxStaticText(f_bar, wxID_ANY, title);
        btn->SetForegroundColour("#009687");
        btn->SetCursor(wxCURSOR_HAND);
        btn->SetFont(Label::Body_13);
        btn->Bind(wxEVT_LEFT_DOWN, [this, select](wxMouseEvent &e) {SelectAll(select);});
        fb_sizer->Add(btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    };
    f_sizer->Add(fb_sizer,0 ,wxALIGN_CENTER_VERTICAL);
    create_btn(_L("All") , true);
    create_btn(_L("None"), false);

    m_menu_button = new wxStaticBitmap(f_bar, wxID_ANY, m_menu.bmp());
    m_menu_button->SetCursor(wxCURSOR_HAND);
    m_menu_button->Bind(wxEVT_LEFT_DOWN, &CheckList::ShowMenu, this);
    f_sizer->Add(m_menu_button,0 ,wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    
    f_bar->SetSizerAndFit(f_sizer);
    w_sizer->Add(f_bar, 0, wxEXPAND);

    auto spacer = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(3)));
    spacer->SetBackgroundColour(parent->GetBackgroundColour());
    w_sizer->Add(spacer, 0, wxEXPAND);

    s_sizer       = new wxBoxSizer(wxVERTICAL);
    m_scroll_area = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, scroll_style);
    m_scroll_area->SetScrollRate(0, 10);
    m_scroll_area->SetSizer(s_sizer);
    m_scroll_area->SetBackgroundColour(parent->GetBackgroundColour());
    m_scroll_area->Bind(wxEVT_RIGHT_DOWN, &CheckList::ShowMenu, this);
    m_scroll_area->DisableFocusFromKeyboard();

    m_info = new wxStaticText(m_scroll_area, wxID_ANY, "");
    m_info->SetFont(Label::Body_13);
    s_sizer->Add(m_info, 1, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(10));
    m_info->Hide();

    m_info_nonsel = _L("No selected items...");
    m_info_allsel = _L("All items selected...");
    m_info_empty  = _L("No matching items...");

    SetBackgroundColour(StateColor::darkModeColorFor("#DBDBDB")); // draws border on wxScrolledWindow

    w_sizer->Add(m_scroll_area, 1, wxEXPAND | wxALL, FromDIP(1)); // 1 for border
    s_sizer->Layout();

    m_list_size = choices.size();

    m_checks.reserve(m_list_size);

    auto margin = FromDIP(2);
    wxCheckBox* cb;

    for (size_t i = 0; i < m_list_size; ++i){
        cb = new wxCheckBox(m_scroll_area, wxID_ANY, choices[i]);
        m_checks.emplace_back(cb);
        s_sizer->Add(cb, 0, wxALL, margin);
    }

    m_scroll_area->FitInside();
    s_sizer->Layout();

    SetSizer(w_sizer);
    Layout();
    Thaw();
}

void CheckList::SetSelections(wxArrayInt sel_array){
    if(!m_first_load){
        for (size_t i = 0; i < m_list_size; ++i)
            Check(i, false);
        m_first_load = false;
    }

    for (int i : sel_array)
        Check(i, true);
}

wxArrayInt CheckList::GetSelections()
{
    wxArrayInt checks;
    for (size_t i = 0; i < m_list_size; ++i)
        if (m_checks[i]->GetValue())
            checks.push_back(i);
    return checks;
}

void CheckList::Check(int i, bool checked)
{
    if (i > -1 && i < m_list_size)
        m_checks[i]->SetValue(checked);
}

void CheckList::SelectAll(bool value)
{
    for (size_t i = 0; i < m_list_size; ++i)
        Check(i, value);
}

void CheckList::SelectVisible(bool value)
{
    auto filter = m_filter_ctrl->GetValue().Lower();
    if((!value && filter == "::unsel") || (value && filter == "::sel"))
        m_filter_ctrl->SetValue("");
    for (size_t i = 0; i < m_list_size; ++i)
        if(m_checks[i]->IsShown())
            Check(i, value);
}

bool CheckList::IsChecked(int i)
{
    if (i > -1 && i < m_list_size)
        return false;
    return m_checks[i]->GetValue();
}

void CheckList::Filter(const wxString& filterText)
{
    Freeze();
    auto filter = filterText.Lower();
    auto c_text = m_filter_ctrl->GetValue().Lower();

    if(filter == "::sel" || filter == "::nonsel"){
        if(c_text != filter){ // not text input
            m_filter_ctrl->SetValue(filter);
            m_filter_ctrl->SetSelection(0,-1);
        }
        fb_sizer->Show(false);
        if (filter == "::sel")
            for (auto& cb : m_checks) cb->Show(cb->GetValue());
        else
            for (auto& cb : m_checks) cb->Show(!cb->GetValue());
    }
    else{
        bool clear = filterText.IsEmpty();
        fb_sizer->Show(clear);
        for (auto& cb : m_checks)
            cb->Show(clear || cb->GetLabel().Lower().Contains(filter));
    }

    m_info->Show();
    for (size_t i = 0; i < m_list_size; ++i) {
        if (m_checks[i]->IsShown()){
            m_info->Hide();
            break;
        }
    }
    if (m_info->IsShown())
        m_info->SetLabel(filter == "::sel" ? m_info_nonsel : filter == "::nonsel" ? m_info_allsel : m_info_empty);

    m_scroll_area->FitInside();
    f_sizer->Layout();
    Thaw();
}

void CheckList::ShowMenu(wxMouseEvent &evt)
{
    bool filtering  = !m_filter_ctrl->GetValue().IsEmpty();
    bool list_empty = m_info->IsShown();

    wxMenu m;
    m.Append(wxID_FILE1, _L("Select All"  ))->Enable(!filtering);
    m.Append(wxID_FILE2, _L("Deselect All"))->Enable(!filtering);
    m.AppendSeparator();
    m.Append(wxID_FILE3, _L("Select visible"  ))->Enable(!list_empty && filtering);
    m.Append(wxID_FILE4, _L("Deselect visible"))->Enable(!list_empty && filtering);
    m.AppendSeparator();
    m.Append(wxID_FILE5, _L("Filter selected"   ));
    m.Append(wxID_FILE6, _L("Filter nonSelected"));

    m.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        switch (e.GetId()){
            case wxID_FILE1: SelectAll(true)     ; break;
            case wxID_FILE2: SelectAll(false)    ; break;
            case wxID_FILE3: SelectVisible(true) ; break;
            case wxID_FILE4: SelectVisible(false); break;
            case wxID_FILE5: Filter("::sel")     ; break;
            case wxID_FILE6: Filter("::nonsel")  ; break;
            default: break;
        }
    },wxID_FILE1, wxID_FILE6);

    wxWindow* p = dynamic_cast<wxWindow*>(evt.GetEventObject());
    if     (p->GetId() == m_scroll_area->GetId())
        p->PopupMenu(&m, evt.GetPosition());
    else if(p->GetId() == m_menu_button->GetId())
        p->PopupMenu(&m, evt.GetPosition());
}