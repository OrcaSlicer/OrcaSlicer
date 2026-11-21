#pragma once

#ifdef __APPLE__

class wxWindow;

namespace Slic3r { namespace GUI {

using CameraFullscreenEscapeCallback = void (*)(void *);

void *install_camera_fullscreen_escape_monitor(CameraFullscreenEscapeCallback callback, void *context);
void  uninstall_camera_fullscreen_escape_monitor(void *monitor);
void *enter_camera_fullscreen_presentation(wxWindow *window);
void  apply_camera_fullscreen_frame(wxWindow *window);
void  restore_camera_fullscreen_presentation(void *presentation);

// Temporarily lower the window level so other apps can appear above the
// fullscreen camera view (e.g. when the user Cmd+Tabs away).
void  suspend_camera_fullscreen_topmost(wxWindow *window);
// Restore the elevated window level when the fullscreen view regains focus.
void  resume_camera_fullscreen_topmost(wxWindow *window);

void  attach_camera_fullscreen_overlay(wxWindow *parent, wxWindow *overlay);
void  detach_camera_fullscreen_overlay(wxWindow *parent, wxWindow *overlay);

void  raise_main_window_after_camera_fullscreen(wxWindow *top_level);

}} // namespace Slic3r::GUI

#endif
