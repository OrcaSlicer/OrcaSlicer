#pragma once

namespace Slic3r {
namespace GUI {

enum class LinuxDisplayBackend { X11, Wayland, Unknown };

// Detect which display backend is in use at runtime.
// Must be called after gtk_init() / wxWidgets initialization.
// On non-Linux platforms, always returns Unknown.
LinuxDisplayBackend get_linux_display_backend();

// Convenience predicates.
bool is_running_on_wayland();
bool is_running_on_x11();

} // namespace GUI
} // namespace Slic3r
