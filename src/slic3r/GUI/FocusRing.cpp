#include "FocusRing.hpp"

#include <wx/popupwin.h>
#include <wx/window.h>
#include <wx/toplevel.h>
#include <wx/glcanvas.h>

namespace Slic3r { namespace GUI {

namespace {
constexpr int  THICKNESS = 2;   // logical px, scaled by the target's DPI
constexpr int  GAP       = 1;   // between the control and the ring
constexpr int  PERIOD_MS = 80;
} // namespace

FocusRing& FocusRing::get()
{
    static FocusRing ring;
    return ring;
}

FocusRing::FocusRing()
{
    m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { layout(); });
}

void FocusRing::on_key_navigation() { m_armed = true; }

void FocusRing::on_pointer()
{
    m_armed = false;
    hide();
}

void FocusRing::on_focus(wxWindow* win)
{
    if (m_armed && win != nullptr)
        show_for(win);
    else
        hide();
}

void FocusRing::set_color(const wxColour& color)
{
    m_color = color;
    for (wxWeakRef<wxPopupWindow>& strip : m_strips)
        if (strip != nullptr) {
            strip->SetBackgroundColour(m_color);
            strip->Refresh();
        }
}

void FocusRing::ensure_strips(wxWindow* top)
{
    if (m_top == top && m_strips[0])
        return;
    for (wxWeakRef<wxPopupWindow>& strip : m_strips) {
        if (strip)
            strip->Destroy();
        // A popup: no decoration, no focus of its own, floats above its parent's windows.
        strip = new wxPopupWindow(top, wxBORDER_NONE);
        strip->SetBackgroundColour(m_color);
    }
    m_top = top;
}

void FocusRing::show_for(wxWindow* win)
{
    wxWindow* top = wxGetTopLevelParent(win);
    if (top == nullptr) {
        hide();
        return;
    }
    ensure_strips(top);
    m_target = win;
    layout();
    if (!m_timer.IsRunning())
        m_timer.Start(PERIOD_MS);
}

void FocusRing::layout()
{
    wxWindow* win = m_target.get();
    if (win == nullptr || !win->IsShownOnScreen() || wxWindow::FindFocus() != win) {
        hide();
        return;
    }
    const int t = win->FromDIP(THICKNESS);
    const int g = win->FromDIP(GAP);
    wxRect r = win->GetScreenRect();
    r.Inflate(g + t, g + t);
    const wxRect rects[4] = {
        { r.x, r.y, r.width, t },                       // top
        { r.x, r.y + r.height - t, r.width, t },        // bottom
        { r.x, r.y, t, r.height },                      // left
        { r.x + r.width - t, r.y, t, r.height },        // right
    };
    for (size_t i = 0; i < 4; ++i) {
        wxPopupWindow* strip = m_strips[i].get();
        if (strip == nullptr)
            return;
        strip->SetSize(rects[i]);
        if (!strip->IsShown())
            strip->Show();
    }
}

void FocusRing::hide()
{
    m_timer.Stop();
    m_target = nullptr;
    for (wxWeakRef<wxPopupWindow>& strip : m_strips)
        if (strip && strip->IsShown())
            strip->Hide();
}

}} // namespace Slic3r::GUI
