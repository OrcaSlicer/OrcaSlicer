#include "PopupWindow.hpp"
#include <wx/popupwin.h>
#include <wx/event.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#endif

static wxWindow *GetTopParent(wxWindow *pWindow)
{
    wxWindow *pWin = pWindow;
    while (pWin->GetParent()) {
        pWin = pWin->GetParent();
        if (auto top = dynamic_cast<wxNonOwnedWindow*>(pWin))
            return top;
    }
    return pWin;
}

bool PopupWindow::Create(wxWindow *parent, int style)
{
    if (!wxPopupTransientWindow::Create(parent, style))
        return false;
#ifdef __WXGTK__
    GetTopParent(parent)->Bind(wxEVT_ACTIVATE, &PopupWindow::topWindowActiavate, this);
#endif
#ifdef __WXOSX__
    if (style & wxPU_CONTAINS_CONTROLS)
    for (auto evt : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_LEFT_DCLICK, wxEVT_MOTION, wxEVT_MOUSEWHEEL})
        Bind(evt, &PopupWindow::OnMouseEvent2, this);
#endif
    return true;
}

PopupWindow::~PopupWindow()
{
#ifdef __WXGTK__
    GetTopParent(this)->Unbind(wxEVT_ACTIVATE, &PopupWindow::topWindowActiavate, this);
#endif
#ifdef __WXMSW__
    GetTopParent(this)->Unbind(wxEVT_ACTIVATE, &PopupWindow::topWindowActivate, this);
    GetTopParent(this)->Unbind(wxEVT_ICONIZE, &PopupWindow::topWindowIconize, this);
    GetTopParent(this)->Unbind(wxEVT_SHOW, &PopupWindow::topWindowShow, this);
#endif
}

#ifdef __WXOSX__

static wxEvtHandler * HitTest(wxWindow * parent, wxMouseEvent &evt)
{
    auto pt = evt.GetPosition();
    const wxWindowList &children = parent->GetChildren();
    for (auto w : children) {
        wxRect rc { w->GetPosition(), w->GetSize() };
        if (rc.Contains(pt)) {
            evt.SetPosition(pt - rc.GetTopLeft());
            if (auto child = HitTest(w, evt))
                return child;
            return w;
        }
    }
    return nullptr;
}

void PopupWindow::OnMouseEvent2(wxMouseEvent &evt)
{
    auto child = ::HitTest(this, evt);
    if (evt.GetEventType() == wxEVT_MOTION) {
        auto h = child ? child : this;
        if (hovered != h) {
            wxMouseEvent leave(wxEVT_LEAVE_WINDOW);
            leave.SetEventObject(hovered);
            leave.SetId(static_cast<wxWindow*>(hovered)->GetId());
            hovered->ProcessEventLocally(leave);
            hovered = h;
            wxMouseEvent enter(wxEVT_ENTER_WINDOW);
            enter.SetEventObject(hovered);
            enter.SetId(static_cast<wxWindow*>(hovered)->GetId());
            hovered->ProcessEventLocally(enter);
        }
    }
    if (child) {
        child->ProcessEventLocally(evt);
    } else {
        evt.Skip();
    }
}

#endif

#ifdef __WXGTK__
template<class Fn> static void for_each_tooltip_window(Fn fn)
{
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next)
        if (gtk_window_get_type_hint(GTK_WINDOW(l->data)) == GDK_WINDOW_TYPE_HINT_TOOLTIP)
            fn(GTK_WINDOW(l->data));
    g_list_free(toplevels);
}

bool PopupWindow::Show(bool show)
{
    // On Wayland a shown GTK tooltip is itself an xdg_popup, and GDK maps a new popup only as a child of
    // the topmost one ("Tried to map a popup with a non-top most parent"). GTK hides the tooltip on a
    // click or key press only after the widgets have handled it, so a popup opened from that click would
    // never appear. GtkMenu hides the tooltip first for the same reason, through a private call; hiding the
    // tooltip window is the public equivalent, and GtkTooltip then treats it as closed and shows it again
    // on a later hover.
    if (show && !IsShown() && Slic3r::GUI::is_running_on_wayland())
        for_each_tooltip_window([](GtkWindow *win) { gtk_widget_hide(GTK_WIDGET(win)); });
    return wxPopupTransientWindow::Show(show);
}

// Whether a tooltip of a widget in this popup, or in a popup opened from it, is on screen. Such a tooltip is
// transient for the window of the popup holding the widget, and that popup is transient for the one it opened from.
static bool own_tooltip_mapped(GtkWindow *popup)
{
    bool mapped = false;
    for_each_tooltip_window([popup, &mapped](GtkWindow *win) {
        // The compositor can dismiss a tooltip without GTK hiding the widget, so ask GDK whether it is mapped.
        GdkWindow *gdk_win = gtk_widget_get_window(GTK_WIDGET(win));
        if (!gdk_win || !gdk_window_is_visible(gdk_win))
            return;
        for (GtkWindow *w = gtk_window_get_transient_for(win); w && !mapped; w = gtk_window_get_transient_for(w))
            mapped = w == popup;
    });
    return mapped;
}

// On Wayland, a GTK tooltip of a widget in a shown popup takes the keyboard focus from the toplevel, which then
// reports a deactivation while the user is still on the popup: GTK makes the tooltip transient for the popup's
// window, which holds the seat grab, so GDK maps the tooltip as an xdg_popup with a grab of its own, and the
// compositor gives the keyboard to the topmost grabbing popup. The toplevel is activated again when the tooltip
// hides, so that deactivation is ignored. wxGTK reports no second deactivation until the next activation, so if
// the app loses the focus while such a tooltip holds it (another app's window taking the focus, a gesture), the
// popup stays open until the user returns. X11 tooltips never take the focus.
void PopupWindow::topWindowActiavate(wxActivateEvent &event)
{
    event.Skip();
    if (!event.GetActive() && IsShown() && ShouldDismissOnTopWindowDeactivate() &&
        !(Slic3r::GUI::is_running_on_wayland() && own_tooltip_mapped(GTK_WINDOW(m_widget))))
        DismissAndNotify();
}
#endif

#ifdef __WXMSW__
void PopupWindow::BindUnfocusEvent()
{
    GetTopParent(this)->Bind(wxEVT_ACTIVATE, &PopupWindow::topWindowActivate, this);
    GetTopParent(this)->Bind(wxEVT_ICONIZE, &PopupWindow::topWindowIconize, this);
    GetTopParent(this)->Bind(wxEVT_SHOW, &PopupWindow::topWindowShow, this);
}

void PopupWindow::topWindowActivate(wxActivateEvent &event)
{
    if (!event.GetActive())
        Dismiss();
}

void PopupWindow::topWindowIconize(wxIconizeEvent &event)
{
    event.Skip();
    if (event.IsIconized())
        Dismiss();
}

void PopupWindow::topWindowShow(wxShowEvent &event)
{
    event.Skip();
    if (!event.IsShown())
        Dismiss();
}
#endif
