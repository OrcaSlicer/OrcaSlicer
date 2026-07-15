#include "LinuxDisplayBackend.hpp"

#if defined(__WXGTK__)

#include <gdk/gdk.h>

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

} // namespace GUI
} // namespace Slic3r

#endif // defined(__WXGTK__)