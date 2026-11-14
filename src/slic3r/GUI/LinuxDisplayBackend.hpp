#pragma once

class wxWindow;

namespace Slic3r {
namespace GUI {
#if defined(__WXGTK__)

enum class LinuxDisplayBackend { X11, Wayland, Unknown };

// Detect which display backend is in use at runtime.
// Must be called after gtk_init() / wxWidgets initialization.
LinuxDisplayBackend get_linux_display_backend();

// Convenience predicates.
bool is_running_on_wayland();
bool is_running_on_x11();

// Keeps a window made without a border free of a title bar on Wayland, where KWin draws one on a window
// that does not draw its own decorations, borderless or not. Call it before the window is first shown.
// No-op on X11.
void remove_wayland_titlebar(wxWindow* window);

#endif // defined(__WXGTK__)
} // namespace GUI
} // namespace Slic3r
