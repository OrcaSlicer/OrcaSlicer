#include "WindowControl.hpp"

#include <wx/defs.h>
#include <wx/display.h>
#include <wx/gdicmn.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <chrono>
#include <string>

#if defined(__WXGTK__)
#include "slic3r/GUI/LinuxDisplayBackend.hpp"

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef wxHAVE_GDK_X11
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>

#include <vector>
#endif
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

#if defined(__WXGTK__)

GtkWidget* gtk_window_of(const wxTopLevelWindow& window)
{
    auto* widget = static_cast<GtkWidget*>(window.GetHandle());
    return widget != nullptr && GTK_IS_WINDOW(widget) ? widget : nullptr;
}

// What a Wayland compositor says of a window, which GTK passes on as a visibility notify and wx ignores:
// it hides the window (xdg_toplevel's suspended state). KWin hides a minimized window, but also one on
// another virtual desktop and every window while the screens are off; Mutter hides any window it has not
// shown for 3 s. So a hidden window reads minimized only while set_window's minimize of it stands, which
// ends once the window shows, takes the focus or closes.
constexpr const char* kSuspendedKey = "orca-mcp-suspended";
constexpr const char* kMinimizedKey = "orca-mcp-minimized";

void set_flag(GObject* window, const char* key, bool on) { g_object_set_data(window, key, on ? GINT_TO_POINTER(1) : nullptr); }

bool window_flag(const wxTopLevelWindow& window, const char* key)
{
    GtkWidget* widget = gtk_window_of(window);
    return widget != nullptr && g_object_get_data(G_OBJECT(widget), key) != nullptr;
}

// The GtkWindow an emission hook's signal was emitted on, or null for any other widget.
GObject* hooked_window(guint count, const GValue* values)
{
    return count >= 1 && G_VALUE_HOLDS_OBJECT(&values[0]) && GTK_IS_WINDOW(g_value_get_object(&values[0])) ?
               G_OBJECT(g_value_get_object(&values[0])) :
               nullptr;
}

gboolean on_visibility_notify(GSignalInvocationHint*, guint count, const GValue* values, gpointer)
{
    if (GObject* window = hooked_window(count, values); window != nullptr && count >= 2) {
        if (const auto* event = static_cast<const GdkEventVisibility*>(g_value_get_boxed(&values[1])); event != nullptr) {
            const bool hidden = event->state == GDK_VISIBILITY_FULLY_OBSCURED;
            set_flag(window, kSuspendedKey, hidden);
            if (!hidden)
                set_flag(window, kMinimizedKey, false);
        }
    }
    return TRUE; // stay installed
}

// A minimized window never has the focus, though one shown while the screens are off is never shown.
gboolean on_focus_in(GSignalInvocationHint*, guint count, const GValue* values, gpointer)
{
    if (GObject* window = hooked_window(count, values))
        set_flag(window, kMinimizedKey, false);
    return TRUE;
}

// A window shown again gets a new surface, which starts out shown.
gboolean on_unmap(GSignalInvocationHint*, guint count, const GValue* values, gpointer)
{
    if (GObject* window = hooked_window(count, values)) {
        set_flag(window, kSuspendedKey, false);
        set_flag(window, kMinimizedKey, false);
    }
    return TRUE;
}

// How GTK lays out a top-level window on Wayland: its whole allocation, the content wx puts in it, and a
// title bar GTK shows above the content. Unset off Wayland and before the window has a surface.
struct WaylandFrame
{
    bool   realized = false;
    wxSize outer, inner;
    int    title = 0;

    // The margins GTK draws the shadow and resize border of a window it decorates in: the compositor
    // shows the window without them, and a window capture draws only its content.
    wxSize margins() const { return {std::max(0, outer.x - inner.x), std::max(0, outer.y - inner.y - title)}; }
};

WaylandFrame wayland_frame(const wxTopLevelWindow& window)
{
    WaylandFrame frame;
    GtkWidget*   widget  = gtk_window_of(window);
    GtkWidget*   content = widget != nullptr ? gtk_bin_get_child(GTK_BIN(widget)) : nullptr;
    if (content == nullptr || !is_running_on_wayland() || !gtk_widget_get_realized(widget))
        return frame;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    frame.outer = {a.width, a.height};
    gtk_widget_get_allocation(content, &a);
    frame.inner = {a.width, a.height};
    if (GtkWidget* bar = gtk_window_get_titlebar(GTK_WINDOW(widget)); bar != nullptr && gtk_widget_get_visible(bar)) {
        gtk_widget_get_allocation(bar, &a);
        frame.title = a.height;
    }
    frame.realized = true;
    return frame;
}

// The decorations wx takes off a window's size before it gives the size to GTK, which wx keeps to itself.
// On Wayland wx measures them, as the whole allocation less the content, only for a window GTK decorates
// while the compositor would rather not (gdk_wayland_display_prefers_ssd() false, as on Mutter); for every
// other window it counts none, even one GTK decorates on KWin, as OrcaSlicer's main window with its own
// title bar.
wxSize wx_decorations(const wxTopLevelWindow& window, const WaylandFrame& frame)
{
#ifdef GDK_WINDOWING_WAYLAND
    GtkWidget*  widget  = gtk_window_of(window);
    GdkDisplay* display = widget != nullptr ? gtk_widget_get_display(widget) : nullptr;
    if (display != nullptr && GDK_IS_WAYLAND_DISPLAY(display) && gtk_window_get_decorated(GTK_WINDOW(widget)) &&
        !gdk_wayland_display_prefers_ssd(display))
        return frame.outer - frame.inner;
#endif
    return {0, 0};
}

#ifdef wxHAVE_GDK_X11
// The window's X11 window while no window manager runs, as on a bare Xvfb, else null. Every EWMH window
// manager lists _NET_WM_STATE among the hints it takes; with none, nothing maximizes, minimizes or stacks a
// window unless the app does it.
GdkWindow* unmanaged_x11_window(const wxTopLevelWindow& window)
{
    GtkWidget* widget = gtk_window_of(window);
    GdkWindow* gdk    = widget != nullptr ? gtk_widget_get_window(widget) : nullptr;
    if (gdk == nullptr || !GDK_IS_X11_WINDOW(gdk))
        return nullptr;
    return gdk_x11_screen_supports_net_wm_hint(gdk_window_get_screen(gdk), gdk_atom_intern_static_string("_NET_WM_STATE")) ? nullptr : gdk;
}

// Raises the shown windows GTK has as transient for `window`, and theirs in turn, in the order they were
// created, as a window manager keeps a dialog above the window it belongs to.
void raise_transients(const wxTopLevelWindow& window)
{
    std::vector<GtkWindow*> owners{GTK_WINDOW(gtk_window_of(window))};
    for (wxWindow* w : wxTopLevelWindows) {
        auto*      tlw    = dynamic_cast<wxTopLevelWindow*>(w);
        GtkWidget* widget = tlw != nullptr && tlw->IsShown() ? gtk_window_of(*tlw) : nullptr;
        if (widget == nullptr || !gtk_widget_get_realized(widget))
            continue;
        GtkWindow* parent = gtk_window_get_transient_for(GTK_WINDOW(widget));
        if (parent != nullptr && std::find(owners.begin(), owners.end(), parent) != owners.end()) {
            gdk_window_raise(gtk_widget_get_window(widget));
            owners.push_back(GTK_WINDOW(widget));
        }
    }
}

// The window's normal rect, kept on the GtkWindow while set_window keeps it maximized itself.
constexpr const char* kNormalRectKey = "orca-mcp-normal-rect";

// Adds or removes the maximized atoms in the window's _NET_WM_STATE, keeping the others GDK set. With no
// window manager to do it, GDK reads the property back when it changes and reports the window maximized
// or not, so wx's IsMaximized() follows.
void set_maximized_hint(GdkWindow* gdk, bool maximized)
{
    GdkDisplay*  display = gdk_window_get_display(gdk);
    Display*     dpy     = GDK_DISPLAY_XDISPLAY(display);
    const Window xid     = GDK_WINDOW_XID(gdk);
    const Atom   state   = gdk_x11_get_xatom_by_name_for_display(display, "_NET_WM_STATE");
    const Atom   vert    = gdk_x11_get_xatom_by_name_for_display(display, "_NET_WM_STATE_MAXIMIZED_VERT");
    const Atom   horz    = gdk_x11_get_xatom_by_name_for_display(display, "_NET_WM_STATE_MAXIMIZED_HORZ");

    std::vector<Atom> atoms;
    Atom              type   = None;
    int               format = 0;
    unsigned long     count = 0, after = 0;
    unsigned char*    data = nullptr;
    if (XGetWindowProperty(dpy, xid, state, 0, 1024, False, XA_ATOM, &type, &format, &count, &after, &data) == Success &&
        data != nullptr) {
        if (type == XA_ATOM && format == 32) {
            const auto* got = reinterpret_cast<const long*>(data); // 32-bit properties come back as longs
            for (unsigned long i = 0; i < count; ++i)
                if (Atom(got[i]) != vert && Atom(got[i]) != horz)
                    atoms.push_back(Atom(got[i]));
        }
        XFree(data);
    }
    if (maximized) {
        atoms.push_back(vert);
        atoms.push_back(horz);
    }
    XChangeProperty(dpy, xid, state, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(atoms.data()),
                    int(atoms.size()));
    XFlush(dpy);
}
#endif // wxHAVE_GDK_X11

#endif // __WXGTK__

} // namespace

void track_window_states()
{
#if defined(__WXGTK__)
    if (!is_running_on_wayland())
        return;
    g_signal_add_emission_hook(g_signal_lookup("visibility-notify-event", GTK_TYPE_WIDGET), 0, on_visibility_notify, nullptr, nullptr);
    g_signal_add_emission_hook(g_signal_lookup("focus-in-event", GTK_TYPE_WIDGET), 0, on_focus_in, nullptr, nullptr);
    g_signal_add_emission_hook(g_signal_lookup("unmap", GTK_TYPE_WIDGET), 0, on_unmap, nullptr, nullptr);
#endif
}

const char* window_state(const wxTopLevelWindow& window)
{
#if defined(__WXGTK__)
    if (window_flag(window, kSuspendedKey) && window_flag(window, kMinimizedKey))
        return "minimized";
#endif
    return window.IsIconized() ? "minimized" : window.IsMaximized() ? "maximized" : "normal";
}

wxSize window_size(wxTopLevelWindow& window)
{
#if defined(__WXGTK__)
    // On Wayland, what GTK allocated less the shadow: wx's own size counts the shadow after some
    // allocations and not after others.
    if (const WaylandFrame frame = wayland_frame(window); frame.realized)
        return frame.outer - frame.margins();
#endif
    return window.GetSize();
}

wxSize frame_size_for(const wxSize& asked, const wxSize& extra, const wxSize& min, const wxSize& max, bool& limited)
{
    const wxSize wanted = asked + extra;
    wxSize       size   = wanted;
    size.IncTo(min);
    size.DecToIfSpecified(max);
    limited = size != wanted;
    return size;
}

bool resize_window(wxTopLevelWindow& window, const wxSize& size)
{
    // wx gives GTK its size less the decorations it counts, and GTK adds a title bar it shows: the wx size
    // for `size` is `size` plus those decorations less that title bar. Nothing but on Wayland.
    wxSize extra(0, 0);
#if defined(__WXGTK__)
    if (const WaylandFrame frame = wayland_frame(window); frame.realized)
        extra = wx_decorations(window, frame) - wxSize(0, frame.title);
#endif
    bool limited = false;
    window.SetSize(frame_size_for(size, extra, window.GetMinSize(), window.GetMaxSize(), limited));
    return limited;
}

void maximize_window(wxTopLevelWindow& window)
{
#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
    // With no window manager the app is its own: it remembers the normal rect, marks the window
    // maximized and fills the work area of its display.
    if (GdkWindow* gdk = unmanaged_x11_window(window); gdk != nullptr) {
        if (window.IsMaximized())
            return;
        GtkWidget* widget = gtk_window_of(window);
        g_object_set_data_full(G_OBJECT(widget), kNormalRectKey, new wxRect(window.GetPosition(), window.GetSize()),
                               [](gpointer rect) { delete static_cast<wxRect*>(rect); });
        set_maximized_hint(gdk, true);
        const int index = wxDisplay::GetFromWindow(&window);
        window.SetSize(wxDisplay(unsigned(index == wxNOT_FOUND ? 0 : index)).GetClientArea());
        return;
    }
#endif
    window.Maximize();
}

void minimize_window(wxTopLevelWindow& window)
{
#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
    // An iconify asks the window manager; with none, the app unmaps the window itself, which GDK then
    // reports as minimized, and wx's Iconize(false) maps it again.
    if (GdkWindow* gdk = unmanaged_x11_window(window); gdk != nullptr) {
        XUnmapWindow(GDK_WINDOW_XDISPLAY(gdk), GDK_WINDOW_XID(gdk));
        XFlush(GDK_WINDOW_XDISPLAY(gdk));
        return;
    }
#endif
#if defined(__WXGTK__)
    if (GtkWidget* widget = gtk_window_of(window); widget != nullptr && is_running_on_wayland())
        set_flag(G_OBJECT(widget), kMinimizedKey, true);
#endif
    window.Iconize();
}

bool restore_window(wxTopLevelWindow& window)
{
    // Restore() won't do: on wxGTK it presents the window, which raises it, and on Windows a window
    // minimized while maximized comes back maximized. Un-minimizing goes out only for a minimized window,
    // as GTK raises a mapped X11 window it un-minimizes. Un-maximizing goes out only for a maximized
    // window: on Windows it restores, so activates, a normal window too, and on Wayland the compositor
    // answers it with the window's current size, which replaces a size asked for right after.
    if (std::string(window_state(window)) == "minimized") {
        // Wayland gives an app no request to un-minimize a window, only to activate it, which brings it
        // back when the compositor allows the app the focus.
        if (restore_needs_activation()) {
            window.Raise();
        } else {
            window.Iconize(false);
#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
            if (unmanaged_x11_window(window) != nullptr)
                raise_transients(window); // mapping it again raised it
#endif
        }
    }
    if (window.IsMaximized()) {
#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
        if (GdkWindow* gdk = unmanaged_x11_window(window); gdk != nullptr) {
            GtkWidget*   widget = gtk_window_of(window);
            const auto*  normal = static_cast<const wxRect*>(g_object_get_data(G_OBJECT(widget), kNormalRectKey));
            const wxRect rect   = normal != nullptr ? *normal : window.GetRect();
            set_maximized_hint(gdk, false);
            window.SetSize(rect);
            g_object_set_data(G_OBJECT(widget), kNormalRectKey, nullptr);
        } else
#endif
            window.Maximize(false);
    }
#if defined(__WXGTK__)
    // GTK reports the state once the window manager or the compositor has applied it.
    return std::string(window_state(window)) != "normal";
#else
    return false;
#endif
}

void raise_window(wxTopLevelWindow& window)
{
    window.Raise();
#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
    if (unmanaged_x11_window(window) != nullptr)
        raise_transients(window);
#endif
}

bool restore_needs_activation()
{
#if defined(__WXGTK__)
    return is_running_on_wayland();
#else
    return false;
#endif
}

std::chrono::seconds state_change_timeout(const std::string& state)
{
#if defined(__WXGTK__)
    if (state == "minimized" && is_running_on_wayland())
        return std::chrono::seconds(5);
#endif
    return std::chrono::seconds(3);
}

}}} // namespace Slic3r::GUI::Mcp
