#include "Notebook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"

//BBS set font size

#include <wx/bookctrl.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <string>
#include <algorithm>
#include <vector>
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/dcclient.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/window.h>
#include <wx/cursor.h>
#include <wx/toplevel.h>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_PAGES_REORDERED, wxCommandEvent);

ButtonsListCtrl::ButtonsListCtrl(wxWindow *parent, wxBoxSizer* side_tools) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    wxColour default_btn_bg;
#ifdef __APPLE__
    default_btn_bg = wxColour("#3B4446"); // Gradient #414B4E
#else
    default_btn_bg = wxColour("#2D2D30"); // Gradient #414B4E
#endif

   
    SetBackgroundColour(default_btn_bg);

    int em = em_unit(this);// Slic3r::GUI::wxGetApp().em_unit();
    // BBS: no gap
    m_btn_margin = 0; // std::lround(0.3 * em);
    m_line_margin = std::lround(0.1 * em);

    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    this->SetSizer(m_sizer);

    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);
    m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    m_side_tools = side_tools;
    if (side_tools != NULL) {
        m_sizer->AddStretchSpacer(1);
        for (size_t idx = 0; idx < side_tools->GetItemCount(); idx++) {
            wxSizerItem* item = side_tools->GetItem(idx);
            wxWindow* item_win = item->GetWindow();
            if (item_win) {
                item_win->Reparent(this);
            }
        }
        m_sizer->Add(side_tools, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, m_btn_margin);
    }

    // BBS: disable custom paint
    //this->Bind(wxEVT_PAINT, &ButtonsListCtrl::OnPaint, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED, [](auto& e){
    });

    // ORCA: tab dragging. The mouse is captured by this control while a tab is dragged.
    Bind(wxEVT_MOTION, &ButtonsListCtrl::OnDragMotion, this);
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& evt) {
        if (m_drag_from >= 0)
            EndDrag(true);
        else
            evt.Skip();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { EndDrag(false); });
}

void ButtonsListCtrl::OnPaint(wxPaintEvent&)
{
    //Slic3r::GUI::wxGetApp().UpdateDarkUI(this);
    const wxSize sz = GetSize();
    wxPaintDC dc(this);

    if (m_selection < 0 || m_selection >= (int)m_pageButtons.size())
        return;

    wxColour selected_btn_bg("#1F8EEA");
    wxColour default_btn_bg("#3B4446"); // Gradient #414B4E
    const wxColour& btn_marker_color = Slic3r::GUI::wxGetApp().get_color_hovered_btn_label();

    // highlight selected notebook button

    for (int idx = 0; idx < int(m_pageButtons.size()); idx++) {
        Button* btn = m_pageButtons[idx];

        btn->SetBackgroundColor(idx == m_selection ? selected_btn_bg : default_btn_bg);

        wxPoint pos = btn->GetPosition();
        wxSize size = btn->GetSize();
        const wxColour& clr = idx == m_selection ? btn_marker_color : default_btn_bg;
        dc.SetPen(clr);
        dc.SetBrush(clr);
        dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
    }
    // Draw orange bottom line

    dc.SetPen(btn_marker_color);
    dc.SetBrush(btn_marker_color);
    dc.DrawRectangle(1, sz.y - m_line_margin, sz.x, m_line_margin);
}

void ButtonsListCtrl::UpdateMode()
{
    //m_mode_sizer->SetMode(Slic3r::GUI::wxGetApp().get_mode());
}

void ButtonsListCtrl::Rescale()
{
    //m_mode_sizer->msw_rescale();
    int em = em_unit(this);
    for (Button* btn : m_pageButtons) {
        //BBS
        btn->SetMinSize({(btn->GetLabel().empty() ? 40 : 132) * em / 10, 36 * em / 10});
        btn->Rescale();
    }

    // BBS: no gap
    //m_btn_margin = std::lround(0.3 * em);
    //m_line_margin = std::lround(0.1 * em);
    //m_buttons_sizer->SetVGap(m_btn_margin);
    //m_buttons_sizer->SetHGap(m_btn_margin);

    m_sizer->Layout();
}

void ButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel && sel >= 0 && sel < static_cast<int>(m_pageButtons.size()))
        return;
    // BBS: change button color
    wxColour selected_btn_bg("#009688");    // Gradient #009688
    if (m_selection >= 0 && m_selection < static_cast<int>(m_pageButtons.size())) {
        StateColor bg_color = StateColor(
        std::pair{wxColour(107, 107, 107), (int) StateColor::Hovered},
        std::pair{wxColour(59, 68, 70), (int) StateColor::Normal});
        m_pageButtons[m_selection]->SetBackgroundColor(bg_color);
        StateColor text_color = StateColor(
        std::pair{wxColour(254,254, 254), (int) StateColor::Normal}
        );
        m_pageButtons[m_selection]->SetTextColor(text_color);
    }

    if (sel < 0 || sel >= static_cast<int>(m_pageButtons.size())) {
        m_selection = -1;
        Refresh();
        return;
    }

    m_selection = sel;

    StateColor bg_color = StateColor(
        std::pair{wxColour(0, 150, 136), (int) StateColor::Hovered},
        std::pair{wxColour(0,150, 136), (int) StateColor::Normal});
    m_pageButtons[m_selection]->SetBackgroundColor(bg_color);

    StateColor text_color = StateColor(
        std::pair{wxColour(254, 254, 254), (int) StateColor::Normal}
        );
    m_pageButtons[m_selection]->SetTextColor(text_color);
    
    Refresh();
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/, const wxBitmap &bmp /* = wxNullBitmap */)
{
    Button * btn = new Button(this, text, bmp_name, wxNO_BORDER);
    btn->SetCornerRadius(0);

    if (bmp_name.empty() && bmp.IsOk())
        btn->SetIcon(bmp);

    // The label no longer carries a leading space, so widen the icon<->text gap to keep the
    // original spacing between a tab's icon and its caption.
    {
        wxClientDC dc(btn);
        dc.SetFont(btn->GetFont());
        int space_w = 0;
        dc.GetTextExtent(" ", &space_w, nullptr);
        btn->SetIconSpacing(5 + space_w);
    }

    //BBS set size for button
    btn->SetMinSize({GetTabWidth(text.empty()), 36 * em_unit(this) / 10});

    StateColor bg_color = StateColor(
        std::pair{wxColour(107, 107, 107), (int) StateColor::Hovered},
        std::pair{wxColour(59, 68, 70), (int) StateColor::Normal});

    btn->SetBackgroundColor(bg_color);
    StateColor text_color = StateColor(
        std::pair{wxColour(254,254, 254), (int) StateColor::Normal});
    btn->SetTextColor(text_color);
    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            auto sel = it - m_pageButtons.begin();
            //do it later
            //SetSelection(sel);
            
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
            evt.SetId(sel);
            wxPostEvent(this->GetParent(), evt);
        }
    });
    btn->Bind(wxEVT_LEFT_DOWN, [this, btn](wxMouseEvent& evt) { OnTabLeftDown(evt, btn); });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    // ORCA: keep the selection on the same button, or SetSelection() later resets the wrong one and
    // the selected colour stays on a tab that is no longer selected.
    if (m_selection >= static_cast<int>(n))
        ++m_selection;
    m_pageLabels.insert(m_pageLabels.begin() + n, text); // ORCA
    m_pageIcons.insert(m_pageIcons.begin() + n, bmp_name);
    m_buttons_sizer->Insert(n, new wxSizerItem(btn));
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    if (n >= m_pageButtons.size())
        return;

    if (m_selection == static_cast<int>(n))
        m_selection = -1;
    else if (m_selection > static_cast<int>(n))
        --m_selection;

    Button* btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    m_pageLabels.erase(m_pageLabels.begin() + n); // ORCA
    m_pageIcons.erase(m_pageIcons.begin() + n);
    m_buttons_sizer->Remove(n);
#if __WXOSX__
    RemoveChild(btn);
#else
    btn->Reparent(nullptr);
#endif
    btn->Destroy();
    m_sizer->Layout();
}

bool ButtonsListCtrl::SetPageImage(size_t n, const std::string& bmp_name) const
{
    if (n >= m_pageButtons.size())
        return false;
     
    // BBS
    //return m_pageButtons[n]->SetBitmap_(bmp_name);
    ScalableBitmap bitmap(NULL, bmp_name);
    //m_pageButtons[n]->SetBitmap_(bitmap);
    return true;
}

void ButtonsListCtrl::SetPageText(size_t n, const wxString& strText)
{
    Button* btn = m_pageButtons[n];
    btn->SetLabel(strText);
    if(!strText.empty())  // ORCA
        m_pageLabels[n] = strText;
}

// ORCA
void ButtonsListCtrl::SetCompact(size_t n, bool compact)
{
    Button* btn = m_pageButtons[n];
    btn->SetMinSize({GetTabWidth(compact), 36 * em_unit(this) / 10});
    btn->SetLabel(compact ? "" : m_pageLabels[n]);
}

wxString ButtonsListCtrl::GetPageText(size_t n) const
{
    Button* btn = m_pageButtons[n];
    return btn->GetLabel();
}

// ORCA
wxString ButtonsListCtrl::GetPageLabel(size_t n) const
{
    return n < m_pageLabels.size() ? m_pageLabels[n] : wxString();
}

// ORCA
void ButtonsListCtrl::SetOverflowButton(wxWindow* button)
{
    if (m_overflow_button == button)
        return;

    if (m_overflow_button != nullptr)
        m_sizer->Detach(m_overflow_button);

    m_overflow_button = button;

    if (m_overflow_button != nullptr)
        // Right after the tab buttons (index 0), ahead of any stretch spacer / side_tools.
        m_sizer->Insert(1, m_overflow_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    m_sizer->Layout();
}

// ORCA
int ButtonsListCtrl::GetTabWidth(bool compact)
{
    return (compact ? 40 : 136) * em_unit(this) / 10;
}

// ORCA: width of the shown contents of a horizontal sizer. Measured from the widgets rather than
// the sizer's cached minimum, which lags behind a Show()/Hide() until the next layout.
static int shown_width(wxSizer* sizer)
{
    int width = 0;
    for (wxSizerItem* item : sizer->GetChildren()) {
        if (item->IsSizer())
            width += shown_width(item->GetSizer());
        else if (wxWindow* win = item->GetWindow()) {
            if (!win->IsShown())
                continue;
            width += win->GetSizer() != nullptr ? shown_width(win->GetSizer()) : win->GetBestSize().GetWidth();
        } else
            width += item->GetSpacer().GetWidth();
        width += item->GetBorder() * (((item->GetFlag() & wxLEFT) ? 1 : 0) + ((item->GetFlag() & wxRIGHT) ? 1 : 0));
    }
    return width;
}

// ORCA: the side tools (Slice/Print) only show on Prepare and Preview, and only take room there.
// The margin is the one MainFrame::fit_tab_labels() keeps in front of them.
int ButtonsListCtrl::GetTabsAreaWidth() const
{
    const int side_tools = m_side_tools != nullptr ? shown_width(m_side_tools) : 0;
    return GetClientSize().GetWidth() - side_tools - FromDIP(15);
}

// ORCA: Button::messureSize() with the label shown: an icon-only tab's width (icon and padding)
// plus the text and the icon gap set in InsertPage().
int ButtonsListCtrl::GetFullTabWidth(const wxString& label)
{
    if (label.empty())
        return GetTabWidth(true);
    // In the tab buttons' font, which can differ from the bar's.
    const wxWindow* text = m_pageButtons.empty() ? static_cast<const wxWindow*>(this) : m_pageButtons.front();
    return std::max(GetTabWidth(false), GetTabWidth(true) + text->GetTextExtent(label).GetWidth() + 5 + text->GetTextExtent(" ").GetWidth());
}

// ORCA
void ButtonsListCtrl::MovePage(size_t from, size_t to)
{
    if (from == to || from >= m_pageButtons.size() || to >= m_pageButtons.size())
        return;

    auto move = [from, to](auto& items) {
        auto item = std::move(items[from]);
        items.erase(items.begin() + from);
        items.insert(items.begin() + to, std::move(item));
    };
    Button* btn = m_pageButtons[from];
    move(m_pageButtons);
    move(m_pageLabels);
    move(m_pageIcons);

    const int f = static_cast<int>(from), t = static_cast<int>(to);
    if (m_selection == f)
        m_selection = t;
    else if (f < m_selection && m_selection <= t)
        --m_selection;
    else if (t <= m_selection && m_selection < f)
        ++m_selection;

    m_buttons_sizer->Detach(btn);
    m_buttons_sizer->Insert(to, btn);
    m_sizer->Layout();
}

// ORCA: Cmd (macOS) / Ctrl + press starts a drag; a plain press goes to the button as before.
void ButtonsListCtrl::OnTabLeftDown(wxMouseEvent& evt, Button* btn)
{
    const auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn);
    if (!m_reorder_enabled || !evt.CmdDown() || m_drag_from >= 0 || it == m_pageButtons.end()) {
        evt.Skip();
        return;
    }

    // Not skipped, so the button never shows as pressed and sends no click.
    m_drag_from = m_drag_at = static_cast<int>(it - m_pageButtons.begin());
    SetCursor(wxCursor(wxCURSOR_SIZEWE));
    CaptureMouse();
    wxGetTopLevelParent(this)->Bind(wxEVT_CHAR_HOOK, &ButtonsListCtrl::OnDragKey, this);
}

void ButtonsListCtrl::OnDragMotion(wxMouseEvent& evt)
{
    evt.Skip();
    if (m_drag_from < 0 || m_drag_at >= static_cast<int>(m_pageButtons.size()))
        return;

    const int x     = evt.GetPosition().x;
    const int width = m_pageButtons[m_drag_at]->GetSize().GetWidth();
    for (size_t i = 0; i < m_pageButtons.size(); ++i) {
        const wxRect rect = m_pageButtons[i]->GetRect();
        if (static_cast<int>(i) == m_drag_at || x < rect.GetLeft() || x > rect.GetRight())
            continue;
        // Move once the pointer would be over the dragged tab after the move. Tabs differ in
        // width (icon-only ones), and moving earlier would swap them back and forth.
        const bool past = static_cast<int>(i) > m_drag_at ? x > rect.GetRight() - width : x < rect.GetLeft() + width;
        if (past) {
            static_cast<Notebook*>(GetParent())->MovePage(m_drag_at, i);
            m_drag_at = static_cast<int>(i);
        }
        break;
    }
}

void ButtonsListCtrl::OnDragKey(wxKeyEvent& evt)
{
    if (m_drag_from >= 0 && evt.GetKeyCode() == WXK_ESCAPE)
        EndDrag(false);
    else
        evt.Skip();
}

void ButtonsListCtrl::EndDrag(bool commit)
{
    if (m_drag_from < 0)
        return;

    const int from = m_drag_from, at = m_drag_at;
    m_drag_from = m_drag_at = -1;
    if (HasCapture())
        ReleaseMouse();
    SetCursor(wxNullCursor);
    wxGetTopLevelParent(this)->Unbind(wxEVT_CHAR_HOOK, &ButtonsListCtrl::OnDragKey, this);

    auto* book = static_cast<Notebook*>(GetParent());
    if (!commit) {
        book->MovePage(at, from);
    } else if (at == from) {
        // Pressed with the modifier but not moved: still a click.
        wxCommandEvent evt(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
        evt.SetId(from);
        wxPostEvent(book, evt);
    } else {
        book->RememberPageOrder();
        wxPostEvent(book, wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_PAGES_REORDERED));
    }
}

//#endif // _WIN32

// ORCA
void Notebook::MovePage(size_t from, size_t to)
{
    const size_t count = GetPageCount();
    if (from == to || from >= count || to >= count)
        return;

    auto move = [from, to](auto& items) {
        auto item = items[from];
        items.erase(items.begin() + from);
        items.insert(items.begin() + to, item);
    };
    move(m_pages);
    move(m_pageNames);

    const int f = static_cast<int>(from), t = static_cast<int>(to);
    if (m_selection == f)
        m_selection = t;
    else if (f < m_selection && m_selection <= t)
        --m_selection;
    else if (t <= m_selection && m_selection < f)
        ++m_selection;

    GetBtnsListCtrl()->MovePage(from, to);
}

// ORCA
std::vector<wxString> Notebook::MergePageOrder(const std::vector<wxString>& old_order, const std::vector<wxString>& page_names)
{
    std::vector<wxString> order;
    for (const wxString& name : page_names)
        if (!name.empty())
            order.push_back(name);

    // Back to front, so the page that came next in the old order is already placed.
    for (size_t k = old_order.size(); k-- > 0;)
        if (std::find(order.begin(), order.end(), old_order[k]) == order.end())
            order.insert(k + 1 < old_order.size() ? std::find(order.begin(), order.end(), old_order[k + 1]) : order.end(),
                         old_order[k]);
    return order;
}

// ORCA: pages without a place in `order` go where they were asked for.
size_t Notebook::OrderedPosition(const std::vector<wxString>& order, const std::vector<wxString>& page_names,
                                 const wxString& id, size_t n)
{
    const size_t rank = PageRank(order, id);
    if (rank == SIZE_MAX)
        return n;

    size_t lo = 0, hi = page_names.size();
    for (size_t i = 0; i < page_names.size(); ++i) {
        const size_t other = PageRank(order, page_names[i]);
        if (other == SIZE_MAX)
            continue;
        if (other < rank)
            lo = i + 1;
        else if (hi == page_names.size())
            hi = i;
    }
    return std::clamp(n, lo, std::max(lo, hi));
}

void Notebook::Init()
{
    // We don't need any border as we don't have anything to separate the
    // page contents from.
    SetInternalBorder(0);

    // No effects by default.
    m_showEffect = m_hideEffect = wxSHOW_EFFECT_NONE;

    m_showTimeout = m_hideTimeout = 0;

    m_pageNames.clear();

    /* On Linux, Gstreamer wxMediaCtrl does not seem to get along well with
     * 32-bit X11 visuals (the overlay does not work).  Is this a wxWindows
     * bug?  Is this a Gstreamer bug?  No idea, but it is our problem ... 
     * and anyway, this transparency thing just isn't all that interesting,
     * so we just don't do it on Linux. 
     */
#ifndef __WXGTK__
    SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
#endif
}
