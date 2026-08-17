#include "LinuxDisplayBackend.hpp"

#if defined(__WXGTK__)

#include <gdk/gdk.h>

#ifdef wxHAVE_GDK_X11
#include <gdk/gdkx.h>
#endif

// gdkx.h pulls in Xlib.h, which leaks these macros into the rest of the
// translation unit. Undefine them so sibling files in a unity build are not
// corrupted. This file only uses GDK macros, never the Xlib ones.
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif
#ifdef Always
#undef Always
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#ifdef Convex
#undef Convex
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