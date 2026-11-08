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
    // The native capture is the screen's composition of our windows (X11, and macOS's window-server capture):
    // our dialogs, popups, menus and tooltips over the target show in it as they are on screen, so none is
    // composited in.
    bool                from_screen = false;
    // Where such a capture drew one of our own windows that covers the target, in image pixels; the 3D
    // canvases are not composited there.
    std::vector<wxRect> covered;
    // The native capture shows the 3D views as they were last presented (macOS's window-server capture), so
    // a caller that keeps them (CaptureOptions::keep_shown_canvases) need not draw them.
    bool                canvases_shown = false;
};

// How a capture gets a 3D canvas's frame: render one and read it back, unless the caller keeps frames.
using CanvasFrameProvider = std::function<bool(GLCanvas3D& canvas, wxImage& out, std::string& err)>;

// Window captures kept open from one capture to the next, as a recording's frames are. On Windows a window
// is read through Windows Graphics Capture, which takes what the screen shows without making the window
// repaint, as PrintWindow does, visibly. Opening one waits for the next composited frame, and an open one has
// the newest at hand. On macOS they keep what ScreenCaptureKit lists of our windows and displays, so that a
// recording does not fetch the list again at every frame.
class CaptureSessions;

// Null where there is nothing to keep open: Linux, Windows without Windows Graphics Capture, and macOS before
// 14.4. UI thread, which must also release it.
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
    // Draw in our popups shown over the window (capture_popups()) where the capture does not show them
    // (Capture::from_screen). A recording draws them itself, over the dialogs it draws in, so that a popup
    // reaching past its dialog is not cut off.
    bool                popups = true;
    // Leave the 3D views as a capture that shows them as presented has them (Capture::canvases_shown) rather
    // than drawing each and compositing it in. A recording keeps them, since a video shows what the screen
    // showed; a screenshot draws them, so that it shows a change the view has not presented yet and can tell
    // a view that is not drawn yet.
    bool                keep_shown_canvases = false;
};

// One of our popups as drawn: a wx popup, a menu, a drop-down list or a tooltip, with the top-level window it
// shows over and its offset from that window's client area, in wx coordinates. The image is at that window's
// scale, with alpha where the popup is see-through, as a tooltip's background can be.
struct PopupCapture
{
    wxTopLevelWindow* owner = nullptr;
    wxPoint           offset;
    wxImage           image;
};

// Our popups shown now, bottom to top, for the captures that leave them out (not Capture::from_screen). Empty on
// X11, whose captures read them from the screen. On macOS, where its window-server capture is unavailable, wx's
// popups only: AppKit draws its menus and tooltips in windows of its own, which only the window server shows.
// Windows reads them from the screen and leaves out those the screen shows mixed with another window.
std::vector<PopupCapture> capture_popups();

// The OS parts of capture_popups() on Windows and macOS. capture_native_popup() draws one of wx's popups from its
// GetScreenPosition() on, at its scale: Windows reads it from the screen, where it shows topmost, and macOS
// draws its view. capture_native_popups() gives the system's popups over our windows, bottom to top: on Windows
// menus, tooltips and the lists of native drop-downs, and the drop-down lists and context menus web views show
// from WebView2's browser process, each over the window it belongs to (a menu over the one it was shown from),
// and none over a native dialog, which captures leave out; none on macOS. Windows leaves out a popup DWM hides
// or that a window above it covers in part, unless that window is a popup over the same window, drawn with it.
bool                      capture_native_popup(wxWindow& popup, wxImage& out);
std::vector<PopupCapture> capture_native_popups();

// Whether the app knows where its top-level windows are on the screen (not on Wayland).
bool window_positions_known();

// True when all but a stray few pixels are black.
bool is_blank(const wxImage& image);

// The canvas exactly as presented: scene, toolpaths, toolbars and ImGui overlays. All platforms.
std::optional<Capture> capture_canvas(GLCanvas3D& canvas, std::string& err, const CaptureOptions& options = {});

// A top-level window's client area: its own pixels, or where the capture reads the screen
// (Capture::from_screen) our windows over it too, never other applications' windows, with every visible 3D
// canvas composited in from its canvas capture, since window capture APIs miss or mangle OpenGL content on
// several platforms. Minimized windows fail.
std::optional<Capture> capture_window(wxTopLevelWindow& window, std::string& err, const CaptureOptions& options = {});

// The OS part of capture_window(): Windows Graphics Capture (or PrintWindow without it), macOS ScreenCaptureKit
// (or cacheDisplayInRect without it), Linux X11 XGetImage or (Wayland) gtk_widget_draw. Implemented per
// platform. Fills `out`'s image, its approximate flag and note, and, where it reads the screen, from_screen,
// covered and canvases_shown.
bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* sessions, Capture& out, std::string& err);

// Physical-pixel offset of `child`'s client area inside `top`'s client area.
wxPoint client_offset_in(wxWindow& child, wxTopLevelWindow& top);

// Offset of top-level `window`'s client area inside top-level `top`'s, in wx coordinates. Where window
// positions are unknown (Wayland), `window` is taken to be centred on the window it is transient for, and
// that one on its own, up to `top`, which is where compositors place a dialog: KWin centres its frame, title
// bar included, so the client area shows half a title bar lower than this.
wxPoint top_level_offset_in(wxTopLevelWindow& window, wxTopLevelWindow& top);

// The largest size within `max_dim` on its long edge that keeps the aspect ratio; sizes already
// small enough are unchanged.
wxSize fit_within(const wxSize& size, int max_dim);

// Copies `src` into `dst` at `at`, clipped to `dst`, leaving the `excluded` rectangles of `dst` as they are. A
// `src` with alpha is blended over `dst` by it.
void composite(wxImage& dst, const wxImage& src, const wxPoint& at, const std::vector<wxRect>& excluded = {});

// PNG bytes of an image.
std::string encode_png(const wxImage& image);

} // namespace Mcp
}} // namespace Slic3r::GUI
