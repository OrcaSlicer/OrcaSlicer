#pragma once

#include <wx/gdicmn.h>
#include <wx/image.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    // A 3D canvas came back entirely black. The view is never drawn black, so its surface was not
    // ready yet, as on Wayland for a moment after the canvas is first shown; capture again later.
    bool        blank_canvas = false;
    // Where the native capture drew one of our own windows that covers the target (X11 only, which reads
    // the screen), in image pixels; the 3D canvases are not composited there.
    std::vector<wxRect> covered;
};

// How a capture gets a 3D canvas's frame: render one and read it back, unless the caller keeps frames.
using CanvasFrameProvider = std::function<bool(GLCanvas3D& canvas, wxImage& out, std::string& err)>;

// Window captures kept open from one capture to the next, as a recording's frames are. On Windows a window
// is read through Windows Graphics Capture, which takes what the screen shows without making the window
// repaint, as PrintWindow does, visibly. Opening one waits for the next composited frame, and an open one has
// the newest at hand.
class CaptureSessions;

// Null where there is nothing to keep open: other platforms, and Windows without Windows Graphics Capture.
// UI thread, which must also release it.
std::shared_ptr<CaptureSessions> open_capture_sessions();

struct CaptureOptions
{
    // X11: hide our hidden-from-automation windows over the target for the moment of the capture. A
    // recording keeps them shown, since hiding them at every frame flickers them.
    bool                hide_covering = true;
    // Empty: GLCanvas3D::render_and_capture().
    CanvasFrameProvider canvas_frame;
    // Null: the capture opens and closes its own.
    CaptureSessions*    sessions = nullptr;
};

// Whether a window capture reads the screen, and so shows our own windows over the target (X11).
bool capture_reads_screen();
// Whether the app knows where its top-level windows are on the screen (not on Wayland).
bool window_positions_known();

// True when all but a stray few pixels are black.
bool is_blank(const wxImage& image);

// The canvas exactly as presented: scene, toolpaths, toolbars and ImGui overlays. All platforms.
std::optional<Capture> capture_canvas(GLCanvas3D& canvas, std::string& err, const CaptureOptions& options = {});

// A top-level window's own pixels (never other windows on the screen), with every visible 3D
// canvas composited in from its canvas capture, since window capture APIs miss or mangle OpenGL
// content on several platforms. Minimized windows fail.
std::optional<Capture> capture_window(wxTopLevelWindow& window, std::string& err, const CaptureOptions& options = {});

// The OS part of capture_window(): Windows Graphics Capture (or PrintWindow without it), macOS
// cacheDisplayInRect, Linux X11 XGetImage or (Wayland) gtk_widget_draw. Implemented per platform. `covered`
// gets the rectangles where it drew another of our windows (X11).
bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* sessions, wxImage& out, bool& approximate, std::string& note,
                           std::vector<wxRect>& covered, std::string& err);

// Physical-pixel offset of `child`'s client area inside `top`'s client area.
wxPoint client_offset_in(wxWindow& child, wxTopLevelWindow& top);

// The largest size within `max_dim` on its long edge that keeps the aspect ratio; sizes already
// small enough are unchanged.
wxSize fit_within(const wxSize& size, int max_dim);

// Copies `src` into `dst` at `at`, clipped to `dst`, leaving the `excluded` rectangles of `dst` as they are.
void composite(wxImage& dst, const wxImage& src, const wxPoint& at, const std::vector<wxRect>& excluded = {});

// PNG bytes of an image.
std::string encode_png(const wxImage& image);

} // namespace Mcp
}} // namespace Slic3r::GUI
