#pragma once

#include <wx/gdicmn.h>
#include <wx/image.h>

#include <optional>
#include <string>

class wxTopLevelWindow;
class wxWindow;

namespace Slic3r { namespace GUI {

class GLCanvas3D;

namespace Mcp {

struct Capture
{
    wxImage     image;
    // Some regions may not show what is on screen (a web view under Wayland, a part of the window
    // off-screen, a canvas that was mid-frame); `note` says which.
    bool        approximate = false;
    std::string note;
};

// The canvas exactly as presented: scene, toolpaths, toolbars and ImGui overlays. All platforms.
std::optional<Capture> capture_canvas(GLCanvas3D& canvas, std::string& err);

// A top-level window's own pixels (never other windows on the screen), with every visible 3D
// canvas composited in from its canvas capture, since window capture APIs miss or mangle OpenGL
// content on several platforms. Minimized windows fail.
std::optional<Capture> capture_window(wxTopLevelWindow& window, std::string& err);

// The OS part of capture_window(): Windows PrintWindow, macOS cacheDisplayInRect, Linux X11
// XGetImage or (Wayland) gtk_widget_draw. Implemented per platform.
bool capture_native_window(wxTopLevelWindow& window, wxImage& out, bool& approximate, std::string& note, std::string& err);

// Physical-pixel offset of `child`'s client area inside `top`'s client area.
wxPoint client_offset_in(wxWindow& child, wxTopLevelWindow& top);

// The largest size within `max_dim` on its long edge that keeps the aspect ratio; sizes already
// small enough are unchanged.
wxSize fit_within(const wxSize& size, int max_dim);

// Copies `src` into `dst` at `at`, clipped to `dst`.
void composite(wxImage& dst, const wxImage& src, const wxPoint& at);

// PNG bytes of an image.
std::string encode_png(const wxImage& image);

} // namespace Mcp
}} // namespace Slic3r::GUI
