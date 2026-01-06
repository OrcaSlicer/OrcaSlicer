#include "Tabbook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "TabButton.hpp"

//BBS set font size
#include "Widgets/Label.hpp"

#include <wx/button.h>
#include <wx/sizer.h>

wxDEFINE_EVENT(wxCUSTOMEVT_TABBOOK_SEL_CHANGED, wxCommandEvent);

const static wxColour TAB_BUTTON_BG  = wxColour("#FEFFFF");
const static wxColour TAB_BUTTON_SEL = wxColour("#BFE1DE"); // ORCA

static const wxFont& TAB_BUTTON_FONT     = Label::Body_14;
static const wxFont& TAB_BUTTON_FONT_SEL = Label::Head_14;


static const int BUTTON_DEF_HEIGHT = 46;
static const int BUTTON_DEF_WIDTH  = 220;


TabButtonsListCtrl::TabButtonsListCtrl(wxWindow *parent, wxBoxSizer *side_tools) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    SetBackgroundColour(TAB_BUTTON_BG);

    int em = em_unit(this);
    // BBS: no gap
    m_btn_margin = 0;
    m_line_margin = std::lround(0.1 * em);

    m_arrow_img = ScalableBitmap(this, "monitor_arrow", 14);

    m_sizer = new wxBoxSizer(wxVERTICAL);
    this->SetSizer(m_sizer);
    if (side_tools != NULL) {
        for (size_t idx = 0; idx < side_tools->GetItemCount(); idx++) {
            wxSizerItem *item     = side_tools->GetItem(idx);
            wxWindow *   item_win = item->GetWindow();
            if (item_win) { item_win->Reparent(this); }
        }
        m_sizer->Add(side_tools, 0, wxEXPAND | wxLEFT | wxTOP, m_btn_margin);
    }

    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);
    m_sizer->Add(m_buttons_sizer, 0, wxLEFT | wxTOP, m_btn_margin);
    m_sizer->AddStretchSpacer(1);
}

void TabButtonsListCtrl::OnPaint(wxPaintEvent &)
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this);
    const wxSize sz = GetSize();
    wxPaintDC dc(this);

    if (m_selection < 0 || m_selection >= (int)m_pageButtons.size())
        return;

    const wxColour& btn_marker_color = Slic3r::GUI::wxGetApp().get_color_hovered_btn_label();

    // highlight selected notebook button

    for (int idx = 0; idx < int(m_pageButtons.size()); idx++) {
        TabButton *btn = m_pageButtons[idx];
        btn->SetBackgroundColor(idx == m_selection ? TAB_BUTTON_SEL : TAB_BUTTON_BG);
        
        wxPoint pos = btn->GetPosition();
        wxSize size = btn->GetSize();
        const wxColour &clr  = StateColor::darkModeColorFor(idx == m_selection ? btn_marker_color : TAB_BUTTON_BG);
        dc.SetPen(clr);
        dc.SetBrush(clr);
        dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
    }
    dc.SetPen(btn_marker_color);
    dc.SetBrush(btn_marker_color);
    dc.DrawRectangle(1, sz.y - m_line_margin, sz.x, m_line_margin);
}

void TabButtonsListCtrl::Rescale()
{
    m_arrow_img = ScalableBitmap(this, "monitor_arrow", 14);

    int em = em_unit(this);
    for (TabButton *btn : m_pageButtons) {
        btn->SetMinSize({BUTTON_DEF_WIDTH * em / 10, BUTTON_DEF_HEIGHT * em / 10});
        btn->SetBitmap(m_arrow_img);
        btn->Rescale();
    }

    m_sizer->Layout();
}

void TabButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel)
        return;
    if (m_selection >= 0) {
        m_pageButtons[m_selection]->SetBackgroundColor(TAB_BUTTON_BG);
        m_pageButtons[m_selection]->SetFont(TAB_BUTTON_FONT);
    }
    m_selection = sel;
    m_pageButtons[m_selection]->SetBackgroundColor(TAB_BUTTON_SEL);
    m_pageButtons[m_selection]->SetFont(TAB_BUTTON_FONT_SEL);
    Refresh();
}

void TabButtonsListCtrl::showNewTag(int sel, bool tag)
{
    if (m_pageButtons[sel]->GetShowNewTag() == tag)
    {
        return;
    }

    m_pageButtons[sel]->ShowNewTag(tag);
    Refresh();
}

bool TabButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/)
{
    TabButton *btn = new TabButton(this, text, m_arrow_img, wxNO_BORDER);
    btn->SetCornerRadius(0);

    int em = em_unit(this);
    btn->SetMinSize({BUTTON_DEF_WIDTH * em / 10, BUTTON_DEF_HEIGHT * em / 10});

    btn->SetBackgroundColor(TAB_BUTTON_BG);
    btn->SetTextColor(*wxBLACK);
    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            auto sel = it - m_pageButtons.begin();
            SetSelection(sel);
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_TABBOOK_SEL_CHANGED);
            evt.SetId(sel);
            wxPostEvent(this->GetParent(), evt);
        }
    });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    m_buttons_sizer->Insert(n, new wxSizerItem(btn));
    m_buttons_sizer->SetRows(m_pageButtons.size() + 1);
    m_sizer->Layout();
    return true;
}

void TabButtonsListCtrl::RemovePage(size_t n)
{
    if (n >= m_pageButtons.size()) return;
    TabButton *btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    m_buttons_sizer->Remove(n);
    btn->Reparent(nullptr);
    btn->Destroy();
    m_sizer->Layout();
}

bool TabButtonsListCtrl::SetPageImage(size_t n, const std::string &bmp_name)
{
    if (n >= m_pageButtons.size())
        return false;

    ScalableBitmap bitmap;
    if (!bmp_name.empty())
        bitmap = ScalableBitmap(this, bmp_name, 14);
    m_pageButtons[n]->SetBitmap(bitmap);

    return true;
}

void TabButtonsListCtrl::SetPageText(size_t n, const wxString &strText)
{
    TabButton *btn = m_pageButtons[n];
    btn->SetLabel(strText);
}

wxString TabButtonsListCtrl::GetPageText(size_t n) const
{
    TabButton *btn = m_pageButtons[n];
    return btn->GetLabel();
}

const wxSize& TabButtonsListCtrl::GetPaddingSize(size_t n) {
    return m_pageButtons[n]->GetPaddingSize();
}

void TabButtonsListCtrl::SetPaddingSize(const wxSize& size) {
    for (auto& btn : m_pageButtons) {
        btn->SetPaddingSize(size);
    }
}

void TabButtonsListCtrl::SetFooterText(const wxString& text)
{
    if (!m_footer_text) {
        m_footer_text = new wxStaticText(this, wxID_ANY, text);
        m_footer_text->SetForegroundColour(wxColour(128, 128, 128));
        m_footer_text->SetFont(Label::Body_10);
        int em = em_unit(this);
        m_sizer->Add(m_footer_text, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, em / 2);
    } else {
        m_footer_text->SetLabel(text);
    }
    m_sizer->Layout();
}

//#endif // _WIN32


