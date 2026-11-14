#include "LinuxDisplayBackend.hpp"

#if defined(__WXGTK__)

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <wx/window.h>

#ifdef wxHAVE_GDK_X11
#include <gdk/gdkx.h>
#endif

#ifdef wxHAVE_GDK_WAYLAND
#include <gdk/gdkwayland.h>
#endif

namespace Slic3r {
namespace GUI {

LinuxDisplayBackend get_linux_display_backend()
{
    static const LinuxDisplayBackend backend = []() -> LinuxDisplayBackend {
        GdkDisplay *display = gdk_display_get_default();
        if (!display)
            return LinuxDisplayBackend::Unknown;

#ifdef wxHAVE_GDK_WAYLAND
        if (GDK_IS_WAYLAND_DISPLAY(display))
            return LinuxDisplayBackend::Wayland;
#endif

#ifdef wxHAVE_GDK_X11
        if (GDK_IS_X11_DISPLAY(display))
            return LinuxDisplayBackend::X11;
#endif

        return LinuxDisplayBackend::Unknown;
    }();
    return backend;
}

bool is_running_on_wayland()
{
    return get_linux_display_backend() == LinuxDisplayBackend::Wayland;
}

bool is_running_on_x11()
{
    return get_linux_display_backend() == LinuxDisplayBackend::X11;
}

void remove_wayland_titlebar(wxWindow* window)
{
    if (!is_running_on_wayland())
        return;
    // With a title bar of its own, an empty one, GTK tells the compositor that the window draws its decorations
    // itself; undecorated, the window then draws none.
    GtkWindow* const gtk_window = GTK_WINDOW(window->GetHandle());
    GtkWidget* const empty      = gtk_fixed_new();
    gtk_widget_set_size_request(empty, 0, 0);
    gtk_window_set_titlebar(gtk_window, empty);
    gtk_window_set_decorated(gtk_window, false);
}

} // namespace GUI
} // namespace Slic3r

#endif // defined(__WXGTK__)