#include "ScreenCapture.hpp"

#include "McpTypes.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/mstream.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__WXGTK__)
#include <cairo.h>
#include <gtk/gtk.h>
#ifdef wxHAVE_GDK_X11
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/gdkx.h>
#endif
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

wxSize fit_within(const wxSize& size, int max_dim)
{
    if (size.x <= 0 || size.y <= 0 || max_dim <= 0 || std::max(size.x, size.y) <= max_dim)
        return size;
    const double scale = double(max_dim) / double(std::max(size.x, size.y));
    return {std::max(1, int(std::lround(size.x * scale))), std::max(1, int(std::lround(size.y * scale)))};
}

void composite(wxImage& dst, const wxImage& src, const wxPoint& at, const std::vector<wxRect>& excluded)
{
    if (!dst.IsOk() || !src.IsOk())
        return;
    const int x0 = std::max(0, at.x), y0 = std::max(0, at.y);
    const int x1 = std::min(dst.GetWidth(), at.x + src.GetWidth()), y1 = std::min(dst.GetHeight(), at.y + src.GetHeight());
    std::vector<std::pair<int, int>> skip; // the row's excluded spans, sorted
    for (int y = y0; y < y1; ++y) {
        const unsigned char* s = src.GetData() + size_t(y - at.y) * size_t(src.GetWidth()) * 3;
        unsigned char*       d = dst.GetData() + size_t(y) * size_t(dst.GetWidth()) * 3;
        skip.clear();
        for (const wxRect& r : excluded)
            if (y >= r.GetTop() && y <= r.GetBottom())
                skip.emplace_back(r.GetLeft(), r.GetRight() + 1);
        std::sort(skip.begin(), skip.end());
        int x = x0;
        for (const auto& [from, to] : skip) {
            if (from > x)
                std::copy(s + size_t(x - at.x) * 3, s + size_t(std::min(from, x1) - at.x) * 3, d + size_t(x) * 3);
            x = std::max(x, to);
            if (x >= x1)
                break;
        }
        if (x < x1)
            std::copy(s + size_t(x - at.x) * 3, s + size_t(x1 - at.x) * 3, d + size_t(x) * 3);
    }
}

std::string encode_png(const wxImage& image)
{
    wxMemoryOutputStream stream;
    if (!image.SaveFile(stream, wxBITMAP_TYPE_PNG))
        return {};
    std::string out(stream.GetLength(), '\0');
    stream.CopyTo(out.data(), out.size());
    return out;
}

wxPoint client_offset_in(wxWindow& child, wxTopLevelWindow& top)
{
    const wxPoint logical = child.ClientToScreen(wxPoint(0, 0)) - top.ClientToScreen(wxPoint(0, 0));
#if defined(__WXMSW__)
    return logical; // wxMSW coordinates are already physical pixels
#else
    const double scale = top.GetContentScaleFactor();
    return {int(std::lround(logical.x * scale)), int(std::lround(logical.y * scale))};
#endif
}

bool is_blank(const wxImage& image)
{
    // A surface that is not ready reads back black, give or take a stray pixel.
    const unsigned char* data   = image.GetData();
    const std::size_t    pixels = std::size_t(image.GetWidth()) * std::size_t(image.GetHeight());
    if (data == nullptr || pixels == 0)
        return true;
    std::size_t lit = 0;
    for (std::size_t i = 0; i < pixels; ++i, data += 3)
        if ((data[0] | data[1] | data[2]) != 0 && ++lit > pixels / 1000)
            return false;
    return true;
}

namespace {

bool canvas_frame(GLCanvas3D& canvas, wxImage& out, std::string& err, const CaptureOptions& options)
{
    return options.canvas_frame ? options.canvas_frame(canvas, out, err) : canvas.render_and_capture(out, err);
}

} // namespace

bool capture_reads_screen()
{
#if defined(__WXGTK__)
    return get_linux_display_backend() == LinuxDisplayBackend::X11;
#else
    return false;
#endif
}

bool window_positions_known()
{
#if defined(__WXGTK__)
    return get_linux_display_backend() != LinuxDisplayBackend::Wayland;
#else
    return true;
#endif
}

std::optional<Capture> capture_canvas(GLCanvas3D& canvas, std::string& err, const CaptureOptions& options)
{
    Capture c;
    if (!canvas_frame(canvas, c.image, err, options))
        return std::nullopt;
    c.blank_canvas = is_blank(c.image);
    return c;
}

#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
namespace {

// An X11 window capture reads the screen, so a window the tools don't see (the script runner) that
// covers the target would show in it. Such windows are hidden for the moment of the capture, and the
// target repainted where they were.
class HideCovering
{
public:
    explicit HideCovering(wxTopLevelWindow& target)
    {
        GdkDisplay* display = gdk_display_get_default();
        if (display == nullptr || !GDK_IS_X11_DISPLAY(display))
            return;
        const wxRect area = target.GetScreenRect();
        for (wxWindow* w : wxTopLevelWindows)
            if (w != &target && w->IsShown() && w->GetName() == kHiddenFromAutomation && w->GetScreenRect().Intersects(area)) {
                w->Hide();
                m_hidden.push_back(w);
            }
        if (m_hidden.empty())
            return;
        gdk_display_sync(display);
        target.Refresh();
        target.Update();
        gdk_display_sync(display);
    }
    ~HideCovering()
    {
        for (wxWindow* w : m_hidden)
            w->Show();
    }
    HideCovering(const HideCovering&)            = delete;
    HideCovering& operator=(const HideCovering&) = delete;

private:
    std::vector<wxWindow*> m_hidden;
};

} // namespace
#endif

std::optional<Capture> capture_window(wxTopLevelWindow& window, std::string& err, const CaptureOptions& options)
{
#if defined(__WXGTK__) && defined(wxHAVE_GDK_X11)
    std::optional<HideCovering> hide_covering;
    if (options.hide_covering)
        hide_covering.emplace(window);
#endif
    if (window.IsIconized() || !window.IsShownOnScreen()) {
        err = "The window is minimized or hidden";
        return std::nullopt;
    }
    Capture c;
    if (!capture_native_window(window, c.image, c.approximate, c.note, c.covered, err))
        return std::nullopt;

    // Composite every visible 3D canvas inside this window from an exact frame capture.
    std::vector<wxWindow*> captured;
    for (GLCanvas3D* canvas : ui_canvases()) {
        wxGLCanvas* glc = canvas->get_wxglcanvas();
        if (glc == nullptr)
            continue;
        captured.push_back(glc);
        if (!glc->IsShownOnScreen() || wxGetTopLevelParent(glc) != &window)
            continue;
        std::string canvas_err;
        wxImage     frame;
        if (!canvas_frame(*canvas, frame, canvas_err, options)) {
            c.approximate = true;
            c.note += (c.note.empty() ? "" : "; ") + std::string("3D view not composited: ") + canvas_err;
            continue;
        }
        c.blank_canvas = c.blank_canvas || is_blank(frame);
        // Not over our own windows on top of it, which an X11 capture drew in as they are on screen.
        composite(c.image, frame, client_offset_in(*glc, window), c.covered);
    }

    // An OpenGL view with no frame capture: say so rather than return its area as the window
    // capture left it.
    std::function<bool(wxWindow*)> other_gl = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return false;
        if (dynamic_cast<wxGLCanvas*>(w) != nullptr && std::find(captured.begin(), captured.end(), w) == captured.end())
            return true;
        for (wxWindow* child : w->GetChildren())
            if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr && other_gl(child))
                return true;
        return false;
    };
    if (other_gl(&window)) {
        c.approximate = true;
        c.note += (c.note.empty() ? "" : "; ") + std::string("an OpenGL view other than the 3D views is not captured");
    }
    return c;
}

#if defined(__WXGTK__)

namespace {

// RGB of a cairo ARGB32 surface (premultiplied, native-endian 32-bit words).
wxImage from_cairo(cairo_surface_t* surface)
{
    cairo_surface_flush(surface);
    const int            w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
    const int            stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data   = cairo_image_surface_get_data(surface);
    wxImage              img(w, h, false);
    unsigned char*       rgb = img.GetData();
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * size_t(stride));
        for (int x = 0; x < w; ++x, rgb += 3) {
            const uint32_t p = row[x];
            rgb[0]           = (p >> 16) & 0xFF;
            rgb[1]           = (p >> 8) & 0xFF;
            rgb[2]           = p & 0xFF;
        }
    }
    return img;
}

} // namespace

bool capture_native_window(wxTopLevelWindow& window, wxImage& out, bool& approximate, std::string& note, std::vector<wxRect>& covered,
                           std::string& err)
{
    GtkWidget* widget = static_cast<GtkWidget*>(window.GetHandle());
    GdkWindow* gdkwin = widget != nullptr ? gtk_widget_get_window(widget) : nullptr;
    if (gdkwin == nullptr) {
        err = "The window has no native surface yet";
        return false;
    }
#ifdef wxHAVE_GDK_X11
    if (GDK_IS_X11_DISPLAY(gdk_window_get_display(gdkwin))) {
        Display*          dpy = GDK_DISPLAY_XDISPLAY(gdk_window_get_display(gdkwin));
        const Window      xid = GDK_WINDOW_XID(gdkwin);
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, xid, &attr) || attr.map_state != IsViewable) {
            err = "The window is not viewable";
            return false;
        }
        // Only regions on the screen are defined without backing store.
        int    rx = 0, ry = 0;
        Window child;
        XTranslateCoordinates(dpy, xid, attr.root, 0, 0, &rx, &ry, &child);
        XWindowAttributes root;
        XGetWindowAttributes(dpy, attr.root, &root);
        // XGetImage fails with BadMatch for any part outside the screen, so only the visible part
        // is read; the rest stays black.
        const wxRect visible = wxRect(rx, ry, attr.width, attr.height).Intersect(wxRect(0, 0, root.width, root.height));
        if (visible.IsEmpty()) {
            err = "The window is off-screen";
            return false;
        }
        if (visible.GetSize() != wxSize(attr.width, attr.height)) {
            approximate = true;
            note        = "part of the window is off-screen";
        }
        XImage* img = XGetImage(dpy, xid, visible.x - rx, visible.y - ry, unsigned(visible.width), unsigned(visible.height), AllPlanes,
                                ZPixmap);
        if (img == nullptr) {
            err = "XGetImage failed";
            return false;
        }
        out.Create(attr.width, attr.height, true);
        auto shift = [](unsigned long mask) {
            int s = 0;
            while (mask != 0 && (mask & 1) == 0) {
                mask >>= 1;
                ++s;
            }
            return s;
        };
        // Copies an XImage into `out` with its top left at (dx, dy).
        auto blit = [&](XImage* src, int dx, int dy) {
            const int rs = shift(src->red_mask), gs = shift(src->green_mask), bs = shift(src->blue_mask);
            for (int y = 0; y < src->height; ++y) {
                if (dy + y < 0 || dy + y >= out.GetHeight())
                    continue;
                for (int x = 0; x < src->width; ++x) {
                    if (dx + x < 0 || dx + x >= out.GetWidth())
                        continue;
                    const unsigned long p = XGetPixel(src, x, y);
                    out.SetRGB(dx + x, dy + y, (unsigned char) ((p & src->red_mask) >> rs), (unsigned char) ((p & src->green_mask) >> gs),
                               (unsigned char) ((p & src->blue_mask) >> bs));
                }
            }
        };
        blit(img, visible.x - rx, visible.y - ry);
        XDestroyImage(img);

        // Without backing store, parts covered by another window read back black. Our own popups
        // and tooltips on top are read and drawn in, as they appear on screen; another
        // application's window over ours leaves a hole, which the result reports.
        Window top = xid;
        for (;;) {
            Window   root_ret = 0, parent = 0, *kids = nullptr;
            unsigned nkids = 0;
            if (!XQueryTree(dpy, top, &root_ret, &parent, &kids, &nkids))
                break;
            if (kids != nullptr)
                XFree(kids);
            if (parent == 0 || parent == attr.root)
                break;
            top = parent;
        }
        // A window manager that reparents puts every top-level window, ours included, in a frame window
        // of its own, so a child of the root is our window or a frame holding it a level or two down.
        GdkDisplay*                        gdisplay   = gdk_window_get_display(gdkwin);
        std::function<Window(Window, int)> own_client = [&](Window w, int depth) -> Window {
            if (gdk_x11_window_lookup_for_display(gdisplay, w) != nullptr)
                return w;
            Window   found = 0, r = 0, p = 0, *kids = nullptr;
            unsigned n = 0;
            if (depth > 0 && XQueryTree(dpy, w, &r, &p, &kids, &n) && kids != nullptr) {
                for (unsigned k = n; k-- > 0 && found == 0;) // topmost first
                    found = own_client(kids[k], depth - 1);
                XFree(kids);
            }
            return found;
        };
        Window   root_ret = 0, parent = 0, *stack = nullptr;
        unsigned nstack = 0;
        if (XQueryTree(dpy, attr.root, &root_ret, &parent, &stack, &nstack) && stack != nullptr) {
            const wxRect mine = visible; // only on-screen parts can be read
            bool         above = false;
            for (unsigned i = 0; i < nstack; ++i) { // bottom to top
                if (stack[i] == top) {
                    above = true;
                    continue;
                }
                XWindowAttributes a;
                if (!above || !XGetWindowAttributes(dpy, stack[i], &a) || a.map_state != IsViewable)
                    continue;
                const wxRect frame_cover = mine.Intersect(wxRect(a.x + a.border_width, a.y + a.border_width, a.width, a.height));
                if (frame_cover.IsEmpty())
                    continue;
                wxRect            cover;
                XImage*           part   = nullptr;
                const Window      client = own_client(stack[i], 3);
                XWindowAttributes ca;
                int               cx = 0, cy = 0;
                Window            child = 0;
                if (client != 0 && XGetWindowAttributes(dpy, client, &ca) && ca.map_state == IsViewable &&
                    XTranslateCoordinates(dpy, client, attr.root, 0, 0, &cx, &cy, &child)) {
                    cover = mine.Intersect(wxRect(cx, cy, ca.width, ca.height));
                    if (!cover.IsEmpty())
                        part = XGetImage(dpy, client, cover.x - cx, cover.y - cy, unsigned(cover.width), unsigned(cover.height), AllPlanes,
                                         ZPixmap);
                }
                if (part != nullptr) {
                    blit(part, cover.x - rx, cover.y - ry);
                    XDestroyImage(part);
                    covered.emplace_back(cover.x - rx, cover.y - ry, cover.width, cover.height);
                }
                // The rest is another application's window, or the decorations of a frame around ours.
                if (part == nullptr || cover != frame_cover) {
                    approximate = true;
                    note        = note.empty() ? "another window covers part of it" : note + "; another window covers part of it";
                }
            }
            XFree(stack);
        }
        return true;
    }
#endif
    // Wayland offers no way to read a window back; draw the widget tree into an image instead.
    // Widgets drawn by GTK come out right; web views may stay blank, and the 3D canvases are
    // composited by the caller. The toplevel's allocation includes the shadow of its client-side
    // decoration; its child is the window as it shows, with the client area where wx puts it.
    GtkWidget* content = GTK_IS_BIN(widget) && gtk_bin_get_child(GTK_BIN(widget)) != nullptr ? gtk_bin_get_child(GTK_BIN(widget)) : widget;
    const int  scale   = gtk_widget_get_scale_factor(content);
    const int  w = gtk_widget_get_allocated_width(content), h = gtk_widget_get_allocated_height(content);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t* cr = cairo_create(surface);
    gtk_widget_draw(content, cr);
    cairo_destroy(cr);
    out = from_cairo(surface);
    cairo_surface_destroy(surface);
    approximate = true;
    note        = "drawn from the widget tree (Wayland); web views may be blank";
    return out.IsOk();
}

#endif // __WXGTK__

}}} // namespace Slic3r::GUI::Mcp
