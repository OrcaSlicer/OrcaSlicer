#include "Notebook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"

//BBS set font size

#include <wx/bookctrl.h>
#include <cmath>
#include <cstddef>
#include <utility>
#include <string>
#include <algorithm>
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/dcclient.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/cursor.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/weakref.h>
#include <wx/window.h>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_TAB_REORDER, wxCommandEvent);

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
}

ButtonsListCtrl::~ButtonsListCtrl()
{
    CancelDrag();
}

void ButtonsListCtrl::CancelDrag()
{
    Button* button = std::exchange(m_drag_button, nullptr);
    m_dragging = false;
    if (m_drop_indicator != nullptr)
        m_drop_indicator->Hide();
    if (button != nullptr) {
        button->SetCursor(wxNullCursor);
        if (button->HasCapture())
            button->ReleaseMouse();
    }
}

int ButtonsListCtrl::DraggablePageAt(const wxPoint& position) const
{
    for (size_t i = 0; i < m_pageButtons.size(); ++i)
        if (m_pageDraggable[i] && m_pageButtons[i]->GetRect().Contains(position))
            return static_cast<int>(i);
    return wxNOT_FOUND;
}

void ButtonsListCtrl::OnTabMouseDown(Button* button, wxMouseEvent& event)
{
    CancelDrag();
    m_drag_button = button;
    m_drag_start = event.GetPosition();
    button->SetFocus();
    button->CaptureMouse();
    // Do not enter Button's click handling: losing capture there synthesises a click.
}

void ButtonsListCtrl::OnTabMouseMotion(Button* button, wxMouseEvent& event)
{
    if (m_drag_button != button) {
        event.Skip();
        return;
    }
    if (!event.LeftIsDown()) {
        CancelDrag();
        return;
    }
    const wxPoint delta = event.GetPosition() - m_drag_start;
    if (!m_dragging) {
        const int threshold_x = std::max(FromDIP(4), wxSystemSettings::GetMetric(wxSYS_DRAG_X, this));
        const int threshold_y = std::max(FromDIP(4), wxSystemSettings::GetMetric(wxSYS_DRAG_Y, this));
        if (std::abs(delta.x) < threshold_x && std::abs(delta.y) < threshold_y)
            return;
        m_dragging = true;
    }

    const wxPoint position = ScreenToClient(button->ClientToScreen(event.GetPosition()));
    const int target = DraggablePageAt(position);
    button->SetCursor(wxCursor(target == wxNOT_FOUND ? wxCURSOR_NO_ENTRY : wxCURSOR_HAND));
    if (target == wxNOT_FOUND || m_pageButtons[target] == button) {
        if (m_drop_indicator != nullptr)
            m_drop_indicator->Hide();
        return;
    }
    const wxRect rect = m_pageButtons[target]->GetRect();
    const bool before = position.x < rect.x + rect.width / 2;
    if (m_drop_indicator == nullptr)
        m_drop_indicator = new wxPanel(this, wxID_ANY);
    const int width = FromDIP(3);
    const int edge = before ? rect.x : rect.x + rect.width;
    m_drop_indicator->SetBackgroundColour(Slic3r::GUI::wxGetApp().get_color_hovered_btn_label());
    m_drop_indicator->SetSize(std::max(0, edge - width / 2), rect.y, width, rect.height);
    m_drop_indicator->Show();
    m_drop_indicator->Raise();
    m_drop_indicator->Refresh();
}

void ButtonsListCtrl::OnTabMouseUp(Button* button, wxMouseEvent& event)
{
    if (m_drag_button != button)
        return;

    const auto source_it = std::find(m_pageButtons.begin(), m_pageButtons.end(), button);
    const int source = source_it == m_pageButtons.end() ? wxNOT_FOUND : static_cast<int>(source_it - m_pageButtons.begin());
    const wxPoint position = ScreenToClient(button->ClientToScreen(event.GetPosition()));
    const int target = DraggablePageAt(position);
    const bool dragged = m_dragging;
    const bool clicked = button->GetRect().Contains(position);
    const bool before = target != wxNOT_FOUND &&
                        position.x < m_pageButtons[target]->GetRect().x + m_pageButtons[target]->GetRect().width / 2;
    CancelDrag();

    if (source == wxNOT_FOUND)
        return;
    if (dragged) {
        if (target == wxNOT_FOUND || source == target)
            return;
        auto* book = static_cast<Notebook*>(GetParent());
        const wxWeakRef<wxWindow> source_page(book->GetPage(source));
        const wxWeakRef<wxWindow> target_page(book->GetPage(target));
        // Overflow can replace buttons. Finish this mouse event before relayout and resolve
        // page pointers again so an intervening unload cannot reorder different pages.
        book->CallAfter([book, source_page, target_page, before] {
            if (!source_page || !target_page)
                return;
            const int from = book->FindPage(source_page.get());
            const int to = book->FindPage(target_page.get());
            if (from == wxNOT_FOUND || to == wxNOT_FOUND)
                return;
            wxCommandEvent reorder(wxCUSTOMEVT_NOTEBOOK_TAB_REORDER);
            reorder.SetEventObject(book);
            reorder.SetInt(from);
            reorder.SetExtraLong(to);
            reorder.SetString(before ? "before" : "after");
            book->GetEventHandler()->ProcessEvent(reorder);
        });
    } else if (clicked) {
        wxCommandEvent selected(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
        selected.SetId(source);
        wxPostEvent(GetParent(), selected);
    }
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

bool ButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/, const wxBitmap &bmp /* = wxNullBitmap */,
                                 bool draggable /* = false */)
{
    CancelDrag();
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

    int em = em_unit(this);
    //BBS set size for button
    btn->SetMinSize({(text.empty() ? 40 : 136) * em / 10, 36 * em / 10});

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
    if (draggable) {
        btn->Bind(wxEVT_LEFT_DOWN, [this, btn](wxMouseEvent& event) { OnTabMouseDown(btn, event); });
        btn->Bind(wxEVT_MOTION, [this, btn](wxMouseEvent& event) { OnTabMouseMotion(btn, event); });
        btn->Bind(wxEVT_LEFT_UP, [this, btn](wxMouseEvent& event) { OnTabMouseUp(btn, event); });
        btn->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { CancelDrag(); });
        btn->Bind(wxEVT_KEY_DOWN, [this, btn](wxKeyEvent& event) {
            if (m_drag_button == btn && event.GetKeyCode() == WXK_ESCAPE)
                CancelDrag();
            else
                event.Skip();
        });
        btn->Bind(wxEVT_KILL_FOCUS, [this, btn](wxFocusEvent& event) {
            if (m_drag_button == btn)
                CancelDrag();
            event.Skip();
        });
    }
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    m_pageLabels.insert(m_pageLabels.begin() + n, text); // ORCA
    m_pageIcons.insert(m_pageIcons.begin() + n, bmp_name);
    m_pageDraggable.insert(m_pageDraggable.begin() + n, draggable);
    m_buttons_sizer->Insert(n, new wxSizerItem(btn));
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    if (n >= m_pageButtons.size())
        return;

    CancelDrag();
    if (m_selection == static_cast<int>(n))
        m_selection = -1;
    else if (m_selection > static_cast<int>(n))
        --m_selection;

    Button* btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    m_pageLabels.erase(m_pageLabels.begin() + n); // ORCA
    m_pageIcons.erase(m_pageIcons.begin() + n);
    m_pageDraggable.erase(m_pageDraggable.begin() + n);
    m_buttons_sizer->Remove(n);
#if __WXOSX__
    RemoveChild(btn);
#else
    btn->Reparent(nullptr);
#endif
    btn->Destroy();
    m_sizer->Layout();
}

bool ButtonsListCtrl::MovePage(size_t from, size_t to)
{
    if (from >= m_pageButtons.size() || to >= m_pageButtons.size())
        return false;
    if (from == to)
        return true;

    CancelDrag();
    Button* const selected = m_selection >= 0 && static_cast<size_t>(m_selection) < m_pageButtons.size()
                                 ? m_pageButtons[m_selection] : nullptr;
    Button* const moved = m_pageButtons[from];
    const auto move = [from, to](auto& values) {
        if (from < to)
            std::rotate(values.begin() + from, values.begin() + from + 1, values.begin() + to + 1);
        else
            std::rotate(values.begin() + to, values.begin() + from, values.begin() + from + 1);
    };
    move(m_pageButtons);
    move(m_pageLabels);
    move(m_pageIcons);
    move(m_pageDraggable);
    m_buttons_sizer->Detach(moved);
    m_buttons_sizer->Insert(to, new wxSizerItem(moved));
    m_selection = selected == nullptr ? -1 : static_cast<int>(std::find(m_pageButtons.begin(), m_pageButtons.end(), selected) - m_pageButtons.begin());
    m_sizer->Layout();
    return true;
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
    int em = em_unit(this);
    Button* btn = m_pageButtons[n];
    btn->SetMinSize({(compact ? 40 : 136) * em / 10, 36 * em / 10});
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

//#endif // _WIN32

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
