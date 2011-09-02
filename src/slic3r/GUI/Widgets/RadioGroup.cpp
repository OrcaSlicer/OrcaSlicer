#include "RadioGroup.hpp"
#include "Label.hpp"
#include "StateColor.hpp"

RadioGroup::RadioGroup(
    wxWindow* parent,
    const std::vector<wxString>& labels,
    long  direction,
    int row_col_limit
)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER) // ensure wxTAB_TRAVERSAL not applied by default
    , m_on(       this, "radio_on"       , 18)
    , m_off(      this, "radio_off"      , 18)
    , m_on_hover( this, "radio_on_hover" , 18)
    , m_off_hover(this, "radio_off_hover", 18)
    , m_disabled( this, "radio_disabled" , 18)
    , m_selectedIndex(0)
    , m_focused(false)
    , m_enabled(true)
{
    Create(parent, labels, direction, row_col_limit);
}

void RadioGroup::Create(
    wxWindow* parent,
    const std::vector<wxString>& labels,
    long  direction,   /* wxHORIZONTAL / wxVERTICAL */
    int row_col_limit  /* sets column/row count depends on direction. creates new row if wxHORIZONTAL used after limit reached */
)
{
    m_labels = labels;
    m_item_count = m_labels.size();

    auto bg = parent->GetBackgroundColour();
    this->SetBackgroundColour(bg);

    m_text_color = StateColor(
        std::pair(wxColour("#6B6A6A"), (int)StateColor::Disabled),
        std::pair(wxColour("#363636"), (int)StateColor::Enabled)
    );

    m_focus_color = StateColor(
        std::pair(bg                 , (int)StateColor::NotFocused),
        std::pair(wxColour("#009688"), (int)StateColor::Focused)
    );

    auto bmp_size   = m_on.GetBmpSize();
    int  item_limit = row_col_limit < 0 ? 1 : row_col_limit > m_item_count ? m_item_count : row_col_limit;
    int  count      = (int(m_item_count / item_limit) + (m_item_count % item_limit));
    int  rows       = (direction & wxHORIZONTAL) ? item_limit : count;
    int  cols       = (direction & wxHORIZONTAL) ? count : item_limit;
    wxFlexGridSizer* f_sizer = new wxFlexGridSizer(rows, cols, 0, 0);

    SetDoubleBuffered(true);

    for (int i = 0; i < m_item_count; ++i){
        auto rb = new wxStaticBitmap(this, wxID_ANY, m_off.bmp(), wxDefaultPosition, wxDefaultSize, wxBU_LEFT | wxNO_BORDER);
        m_radioButtons.push_back(rb);
        rb->Bind(wxEVT_LEFT_DOWN   ,([this, i](wxMouseEvent e) {SetSelection(i, true) ; e.Skip();}));
        rb->Bind(wxEVT_ENTER_WINDOW,([this, i](wxMouseEvent e) {SetRadioIcon(i, true) ; e.Skip();}));
        rb->Bind(wxEVT_LEAVE_WINDOW,([this, i](wxMouseEvent e) {
            // prevent removing hover effect while switching between button and its text
            auto win = wxFindWindowAtPoint(wxGetMousePosition());
            if(!win || win->GetId() != m_labelButtons[i]->GetId())
                SetRadioIcon(i, false);
            e.Skip();
        }));

        auto tx = new Button(this, m_labels[i]);
        if(i != 0) // one focusable control must exist. wxPanel starts taking focus if there is no focusable control
            tx->SetCanFocus(false);
        tx->SetPaddingSize(FromDIP(wxSize(5,2)));
        tx->SetBackgroundColor(bg);
        tx->SetCornerRadius(0);
        tx->SetBorderWidth(FromDIP(1));
        tx->SetBorderColor(m_focus_color);
        tx->SetFont(Label::Body_14);
        tx->SetTextColor(m_text_color);
        tx->Bind(wxEVT_BUTTON      ,([this, i](wxCommandEvent e) {SetSelection(i, true) ; e.Skip();}));
        tx->Bind(wxEVT_ENTER_WINDOW,([this, i](wxMouseEvent   e) {SetRadioIcon(i, true) ; e.Skip();}));
        tx->Bind(wxEVT_LEAVE_WINDOW,([this, i](wxMouseEvent   e) {
            // prevent removing hover effect while switching between button and its text
            auto win = wxFindWindowAtPoint(wxGetMousePosition());
            if(!win || win->GetId() != m_radioButtons[i]->GetId())
                SetRadioIcon(i, false);
            e.Skip();
        }));
        tx->Bind(wxEVT_CHAR_HOOK, ([this, tx](wxKeyEvent&e){
            if(tx->HasFocus()){
                int  k = e.GetKeyCode();
                bool is_next = (k == WXK_DOWN || k == WXK_RIGHT);
                bool is_prev = (k == WXK_LEFT || k == WXK_UP);
                if      (is_next) SelectNext();
                else if (is_prev) SelectPrevious();
                e.Skip(!(is_next || is_prev));
            }else{
                e.Skip();
            }
        }));
        m_labelButtons.push_back(tx);

        wxBoxSizer* radio_sizer = new wxBoxSizer(wxHORIZONTAL);
        radio_sizer->Add(rb, 0, wxALIGN_CENTER_VERTICAL);
        radio_sizer->Add(tx, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, this->FromDIP(10));
        f_sizer->Add(radio_sizer, 0, wxTOP | wxBOTTOM, this->FromDIP(4));
    }
    SetSelection(m_selectedIndex);
    SetSizer(f_sizer);
}

void RadioGroup::SetSelection(int index, bool focus)
{
    if (index >= 0 && index < m_item_count){
        int prev_index = m_selectedIndex;
        if(index != prev_index){ // prevent no focusable item on first creation. wxPanel starts taking focus if there is no focusable control
            m_labelButtons[index]->SetCanFocus(true);
            m_labelButtons[prev_index]->SetCanFocus(false);
        }
        if(focus)
            m_labelButtons[index]->SetFocus();
        m_selectedIndex = index;
        for (size_t i = 0; i < m_item_count; ++i)
            SetRadioIcon(i, m_labelButtons[index]->HasFocus() && i == m_selectedIndex);

        wxCommandEvent evt(wxEVT_COMMAND_RADIOBOX_SELECTED, GetId());
        evt.SetInt(index);
        evt.SetString(m_labels[index]);
        GetEventHandler()->ProcessEvent(evt);
    }
}

int RadioGroup::GetSelection()
{ 
    return m_selectedIndex; 
}

void RadioGroup::SelectNext(bool focus)
{
    SetSelection(m_selectedIndex + 1 > (m_radioButtons.size() - 1) ? 0 : m_selectedIndex + 1, focus);
}

void RadioGroup::SelectPrevious(bool focus)
{
    SetSelection(m_selectedIndex - 1 < 0 ? (m_radioButtons.size() - 1) : m_selectedIndex - 1, focus);
}

void RadioGroup::SetRadioIcon(int i, bool hover)
{
    auto icon = !m_enabled ? m_disabled : m_selectedIndex == i ? (hover ? m_on_hover : m_on) : (hover ? m_off_hover : m_off);
    m_radioButtons[i]->SetBitmap(icon.bmp());
}

bool RadioGroup::Enable(bool enable)
{
    m_enabled = enable;
    bool result = wxPanel::Enable(enable);

    if (result) {
        for (size_t i = 0; i < m_item_count; ++i){
            SetRadioIcon(i, false);
            m_labelButtons[i]->Enable(enable); // normally disabling parent should do this but not
        }

        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
    }

    return result;
};

// is focused

bool RadioGroup::Disable() {return RadioGroup::Enable(false);};

bool RadioGroup::IsEnabled(){return m_enabled;};

void RadioGroup::SetRadioTooltip(int i, wxString tooltip)
{
    m_radioButtons[i]->SetToolTip(tooltip);
    m_labelButtons[i]->SetToolTip(tooltip);
}