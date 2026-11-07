#include "Notebook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StateHandler.hpp"

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
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/window.h>

#include <algorithm>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);

// ORCA: a tab button's fill and its keyboard focus ring, set together. The border is drawn in
// every state, in the fill's own colour except while focused, so only the ring shows focus: the
// fill ignores it. StateHandler repaints on focus changes. The ring takes Button::SetStyle's focus
// colours, #00FFD4 in light mode and the Orca teal in dark mode. The teal is #009689 because the
// dark mode map turns #009688 into #00675B, the selected tab's own fill, which would hide the ring.
static void set_tab_colors(Button* btn, const wxColour& normal, const wxColour& hovered)
{
    StateColor background(std::pair{hovered, (int) StateColor::Hovered},
                          std::pair{normal, (int) StateColor::Normal});
    background.setTakeFocusedAsHovered(false);
    btn->SetBackgroundColor(background);
    const bool is_dark = StateColor::darkModeColorFor("#FFFFFF") != wxColour("#FFFFFF");
    StateColor border(std::pair{is_dark ? wxColour("#009689") : wxColour("#00FFD4"), (int) StateColor::Focused},
                      std::pair{hovered, (int) StateColor::Hovered},
                      std::pair{normal, (int) StateColor::Normal});
    border.setTakeFocusedAsHovered(false);
    btn->SetBorderColor(border);
}

// ORCA: a tab button that draws its focus ring only for focus a key press brought, as a
// browser's :focus-visible does. A click still focuses the tab (Button::mouseDown), but the ring
// stays off: StateHandler, which runs first, marks every focus Focused, so focus that did not
// come through SetFocusFromKbd() has the state cleared again. Tab traversal, the arrow keys, F6
// and Enter/Space activation all move focus through SetFocusFromKbd(), and any other key press
// on a clicked tab, short of a Ctrl/Alt shortcut, brings the ring back too.
namespace {
class NotebookTabButton : public Button
{
public:
    NotebookTabButton(wxWindow* parent, const wxString& text, const wxString& icon, long style)
        : Button(parent, text, icon, style)
    {
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
            m_focus_from_kbd = false;
            state_handler.set_state(0, StateHandler::Focused); // a click on the already focused tab
            evt.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& evt) {
            if (!m_focus_from_kbd)
                state_handler.set_state(0, StateHandler::Focused);
            evt.Skip();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& evt) {
            const int key = evt.GetKeyCode();
            const bool modifier_key = key == WXK_SHIFT || key == WXK_CONTROL || key == WXK_ALT || key == WXK_RAW_CONTROL ||
                                      key == WXK_WINDOWS_LEFT || key == WXK_WINDOWS_RIGHT || key == WXK_WINDOWS_MENU;
            if (!modifier_key && !(evt.GetModifiers() & (wxMOD_CONTROL | wxMOD_ALT | wxMOD_META)))
                show_focus_ring();
            evt.Skip();
        });
    }

    void SetFocusFromKbd() override
    {
        m_focus_from_kbd = true;
        Button::SetFocusFromKbd();
        show_focus_ring();
    }

private:
    // A tab that already has the focus gets no focus event to set the state from.
    void show_focus_ring()
    {
        m_focus_from_kbd = true;
        if (HasFocus())
            state_handler.set_state(StateHandler::Focused, StateHandler::Focused);
    }

    bool m_focus_from_kbd = false;
};
} // namespace

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
    // ORCA: consumed here, on whichever selection change the activation produced, so an
    // Enter/Space that ends up changing nothing cannot leak into a later programmatic one.
    const bool from_keyboard = m_activating_from_keyboard;
    m_activating_from_keyboard = false;

    if (m_selection == sel && sel >= 0 && sel < static_cast<int>(m_pageButtons.size()))
        return;
    // BBS: change button color
    wxColour selected_btn_bg("#009688");    // Gradient #009688
    if (m_selection >= 0 && m_selection < static_cast<int>(m_pageButtons.size())) {
        set_tab_colors(m_pageButtons[m_selection], wxColour(59, 68, 70), wxColour(107, 107, 107));
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

    set_tab_colors(m_pageButtons[m_selection], wxColour(0, 150, 136), wxColour(0, 150, 136));

    StateColor text_color = StateColor(
        std::pair{wxColour(254, 254, 254), (int) StateColor::Normal}
        );
    m_pageButtons[m_selection]->SetTextColor(text_color);

    // ORCA: keyboard activation keeps focus on the tab. Enter/Space reaches the page
    // switch as a synthetic click (Button::keyDownUp), so it shares the mouse path and
    // inherits its hand-off of focus to the page: Notebook::SetSelection runs
    // DoSetSelection before this call, and the page-changed handler focuses the page,
    // which the plater then does again from a posted event - hence CallAfter rather than
    // a direct SetFocus here. Only the keyboard sets the flag, so a click still lands the
    // user on the page they clicked to.
    if (from_keyboard) {
        Button* btn = m_pageButtons[m_selection];
        CallAfter([this, btn]() {
            if (std::find(m_pageButtons.begin(), m_pageButtons.end(), btn) != m_pageButtons.end() &&
                btn->IsShownOnScreen() && btn->IsEnabled())
                btn->SetFocusFromKbd();
        });
    }

    Refresh();
}

wxWindow* ButtonsListCtrl::GetSelectedButton() const
{
    if (m_selection >= 0 && m_selection < int(m_pageButtons.size()))
        return m_pageButtons[m_selection];
    return nullptr;
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/, const wxBitmap &bmp /* = wxNullBitmap */)
{
    Button * btn = new NotebookTabButton(this, text, bmp_name, wxNO_BORDER);
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

    // ORCA: wxNO_BORDER zeroed the width; the ring needs a border in every state (see set_tab_colors).
    btn->SetBorderWidth(btn->FromDIP(2));
    set_tab_colors(btn, wxColour(59, 68, 70), wxColour(107, 107, 107));
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
    // ORCA: WAI-ARIA tablist keyboard model. Arrow keys were dead on these
    // buttons: Button::keyDownUp hands them to HandleAsNavigationKey, which
    // acts only on Tab, and never calls Skip(). This handler gives them
    // meaning - Left/Right move focus among the tab buttons and wrap at the
    // ends (stock Tab traversal walks on into the side tools sharing this
    // strip), Home/End jump to the first/last tab, and Up/Down stay swallowed,
    // as a horizontal tab list should. Enter/Space still activates via
    // Button::keyDownUp's click simulation. This dynamic handler runs before
    // Button's static event table, so it wins for the keys it takes.
    btn->Bind(wxEVT_KEY_DOWN, [this, btn](wxKeyEvent& evt) {
        auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn);
        if (it == m_pageButtons.end()) {
            evt.Skip();
            return;
        }
        // ORCA: Enter/Space is left to Button::keyDownUp, which activates the tab as a
        // click; flag it so SetSelection can take focus back afterwards. Every other key
        // clears the flag rather than leaving it set for an unrelated selection change.
        m_activating_from_keyboard = evt.GetKeyCode() == WXK_RETURN || evt.GetKeyCode() == WXK_SPACE;
        const int key = evt.GetKeyCode();
        if (key == WXK_UP || key == WXK_DOWN)
            return;
        const int next = row_focus_target(int(it - m_pageButtons.begin()), int(m_pageButtons.size()), key);
        if (next < 0) {
            evt.Skip();
            return;
        }
        m_pageButtons[next]->SetFocusFromKbd();
    });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
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
