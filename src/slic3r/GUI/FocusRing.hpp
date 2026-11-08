#pragma once

#include <array>
#include <wx/colour.h>
#include <wx/timer.h>
#include <wx/weakref.h>

class wxWindow;
class wxPopupWindow;

namespace Slic3r { namespace GUI {

// A visible keyboard-focus outline, like a browser's focus ring: four thin strips floated
// around whatever window has keyboard focus. Shown only after keyboard navigation (Tab) and
// hidden again by any pointer press, so mouse users never see it. It follows the focused
// window on a timer, since parents scroll and resize without telling it.
class FocusRing
{
public:
    static FocusRing& get();

    void on_key_navigation();        // Tab was pressed: arm the ring
    void on_pointer();               // a mouse button: disarm and hide
    void on_focus(wxWindow* win);    // keyboard focus moved
    void hide();
    void set_color(const wxColour& color);

private:
    FocusRing();
    void show_for(wxWindow* win);
    void layout();
    void ensure_strips(wxWindow* top);

    // Weak: the strips are children of the current top-level window and die with it (a
    // dialog closing), so plain pointers dangled and the next focus change crashed.
    std::array<wxWeakRef<wxPopupWindow>, 4> m_strips;
    wxWeakRef<wxWindow>           m_top;
    wxWeakRef<wxWindow>           m_target;
    wxTimer                       m_timer;
    wxColour                      m_color{ 59, 139, 245 };
    bool                          m_armed{ false };
};

}} // namespace Slic3r::GUI
