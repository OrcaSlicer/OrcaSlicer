#include "ScreenCapture.hpp"

#include "McpTypes.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"

#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    const unsigned char* alpha = src.HasAlpha() ? src.GetAlpha() : nullptr;
    std::vector<std::pair<int, int>> skip; // the row's excluded spans, sorted
    for (int y = y0; y < y1; ++y) {
        const size_t         row = size_t(y - at.y) * size_t(src.GetWidth());
        const unsigned char* s   = src.GetData() + row * 3;
        unsigned char*       d   = dst.GetData() + size_t(y) * size_t(dst.GetWidth()) * 3;
        // Copies, or blends by the source's alpha, the pixels from x to x_end.
        auto put = [&](int x, int x_end) {
            if (alpha == nullptr) {
                std::copy(s + size_t(x - at.x) * 3, s + size_t(x_end - at.x) * 3, d + size_t(x) * 3);
                return;
            }
            for (; x < x_end; ++x) {
                const int            a  = alpha[row + size_t(x - at.x)];
                const unsigned char* sp = s + size_t(x - at.x) * 3;
                unsigned char*       dp = d + size_t(x) * 3;
                for (int c = 0; c < 3; ++c)
                    dp[c] = (unsigned char) ((sp[c] * a + dp[c] * (255 - a) + 127) / 255);
            }
        };
        skip.clear();
        for (const wxRect& r : excluded)
            if (y >= r.GetTop() && y <= r.GetBottom())
                skip.emplace_back(r.GetLeft(), r.GetRight() + 1);
        std::sort(skip.begin(), skip.end());
        int x = x0;
        for (const auto& [from, to] : skip) {
            if (from > x)
                put(x, std::min(from, x1));
            x = std::max(x, to);
            if (x >= x1)
                break;
        }
        if (x < x1)
            put(x, x1);
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

#if defined(__WXGTK__)
namespace {

// The top-level window `window` was shown over: GTK's transient parent, which a compositor places it by.
wxTopLevelWindow* transient_parent(wxTopLevelWindow& window)
{
    auto* widget = static_cast<GtkWidget*>(window.GetHandle());
    GtkWindow* parent = widget != nullptr && GTK_IS_WINDOW(widget) ? gtk_window_get_transient_for(GTK_WINDOW(widget)) : nullptr;
    if (parent != nullptr)
        for (wxWindow* w : wxTopLevelWindows)
            if (w->GetHandle() == GTK_WIDGET(parent))
                return dynamic_cast<wxTopLevelWindow*>(w);
    return nullptr;
}

} // namespace
#endif

wxPoint top_level_offset_in(wxTopLevelWindow& window, wxTopLevelWindow& top)
{
#if defined(__WXGTK__)
    if (!window_positions_known()) {
        // Up the transient parents to `top`; a missing one, or a chain longer than there are windows, ends
        // at `top`.
        wxPoint           at(0, 0);
        wxTopLevelWindow* w = &window;
        for (std::size_t steps = 0; w != &top; ++steps) {
            wxTopLevelWindow* parent = transient_parent(*w);
            if (parent == nullptr || parent == w || steps > wxTopLevelWindows.size())
                parent = &top;
            const wxSize outer = parent->GetClientSize(), inner = w->GetClientSize();
            at += wxPoint((outer.x - inner.x) / 2, (outer.y - inner.y) / 2);
            w = parent;
        }
        return at;
    }
#endif
    return window.ClientToScreen(wxPoint(0, 0)) - top.ClientToScreen(wxPoint(0, 0));
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

// An X11 window capture reads the screen, so a window the tools don't see (the Action Replay window) that
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
    if (!capture_native_window(window, options.sessions, c, err))
        return std::nullopt;
    if (c.canvases_shown && options.keep_shown_canvases)
        return c; // the views as presented, and our popups over them where the capture reads the screen

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

    // Our popups over the window, over its 3D views too, as a context menu on the 3D view shows.
    if (options.popups && !c.from_screen) {
        const double scale = window.GetContentScaleFactor();
        for (const PopupCapture& p : capture_popups())
            if (p.owner == &window)
                composite(c.image, p.image, wxPoint(int(std::lround(p.offset.x * scale)), int(std::lround(p.offset.y * scale))));
    }

    // An OpenGL view with no frame capture: say so rather than return its area as the window
    // capture left it, unless the capture shows OpenGL views as presented.
    if (c.canvases_shown)
        return c;
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

#if !defined(__WXGTK__)
std::vector<PopupCapture> capture_popups()
{
    std::vector<PopupCapture> out;
    // wx's popups, each over the window it opened from, where its screen position puts it.
    for (wxWindow* top : wxTopLevelWindows) {
        auto* window = dynamic_cast<wxTopLevelWindow*>(top);
        if (window == nullptr || !window->IsShownOnScreen() || hidden_from_automation(window))
            continue;
        for (wxWindow* popup : popups_of(window)) {
            PopupCapture p;
            p.owner  = window;
            p.offset = popup->GetScreenPosition() - window->ClientToScreen(wxPoint(0, 0));
            if (capture_native_popup(*popup, p.image))
                out.push_back(std::move(p));
        }
    }
    // The system's over them: menus and tooltips show topmost.
    for (PopupCapture& p : capture_native_popups())
        if (p.owner->IsShownOnScreen() && !hidden_from_automation(p.owner))
            out.push_back(std::move(p));
    return out;
}
#endif

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

// Whether anything was drawn on a surface created transparent.
bool painted(cairo_surface_t* surface)
{
    cairo_surface_flush(surface);
    const int            w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
    const int            stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data   = cairo_image_surface_get_data(surface);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * size_t(stride));
        for (int x = 0; x < w; ++x)
            if ((row[x] >> 24) != 0)
                return true;
    }
    return false;
}

// The child a GtkWindow shows itself in: without the shadow of a client-side decoration, with the client
// area where wx puts it.
GtkWidget* content_of(GtkWidget* window)
{
    GtkWidget* child = GTK_IS_BIN(window) ? gtk_bin_get_child(GTK_BIN(window)) : nullptr;
    return child != nullptr ? child : window;
}

// Draws a GtkWindow's content from its widget tree, at `scale` image pixels per logical one. A relayout still
// pending, once a widget was shown or hidden, makes gtk_widget_draw() draw nothing, so it is run first, as the
// frame clock's layout phase would. A window just shown or resized waits for the compositor to size it before
// it can be laid out, and draws nothing until then: false.
bool draw_widget_tree(GtkWidget* window, int scale, wxImage& out)
{
    gtk_container_check_resize(GTK_CONTAINER(window));
    GtkWidget* content = content_of(window);
    const int  w = gtk_widget_get_allocated_width(content), h = gtk_widget_get_allocated_height(content);
    if (w <= 0 || h <= 0)
        return false;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t* cr = cairo_create(surface);
    gtk_widget_draw(content, cr);
    cairo_destroy(cr);
    const bool drawn = painted(surface);
    if (drawn)
        out = from_cairo(surface);
    cairo_surface_destroy(surface);
    return drawn && out.IsOk();
}

// Draws a popup GTK placed, a menu or a tooltip, as the compositor shows it: the whole window, since GTK
// paints a tooltip's background and border on the window rather than its content, cropped to the window's
// geometry, the part that is not shadow, found as the box of its opaque pixels. The image keeps the alpha
// to blend it by.
bool draw_popup_window(GtkWidget* window, int scale, wxImage& out)
{
    gtk_container_check_resize(GTK_CONTAINER(window));
    const int w = gtk_widget_get_allocated_width(window), h = gtk_widget_get_allocated_height(window);
    if (w <= 0 || h <= 0)
        return false;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t* cr = cairo_create(surface);
    gtk_widget_draw(window, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const int            sw = cairo_image_surface_get_width(surface), sh = cairo_image_surface_get_height(surface);
    const int            stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data   = cairo_image_surface_get_data(surface);
    auto pixel = [&](int x, int y) { return reinterpret_cast<const uint32_t*>(data + size_t(y) * size_t(stride))[x]; };
    int left = sw, top = sh, right = -1, bottom = -1;
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            if ((pixel(x, y) >> 24) >= 128) {
                left   = std::min(left, x);
                right  = std::max(right, x);
                top    = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    const bool drawn = right >= left;
    if (drawn) {
        out.Create(right - left + 1, bottom - top + 1, false);
        out.InitAlpha();
        unsigned char* rgb   = out.GetData();
        unsigned char* alpha = out.GetAlpha();
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x, rgb += 3, ++alpha) {
                // Premultiplied in the surface; straight in a wxImage.
                const uint32_t p = pixel(x, y);
                const int      a = int(p >> 24);
                auto           c = [a](uint32_t v) { return (unsigned char) (a == 0 ? 0 : std::min(255, int((v * 255 + a / 2) / a))); };
                rgb[0]           = c((p >> 16) & 0xFF);
                rgb[1]           = c((p >> 8) & 0xFF);
                rgb[2]           = c(p & 0xFF);
                *alpha           = (unsigned char) a;
            }
    }
    cairo_surface_destroy(surface);
    return drawn;
}

// Where a GtkWindow, and its content, are in GDK's root coordinates. On Wayland those are not the screen's,
// but GDK keeps a popup's in step with the window it shows over (see capture_popups()).
wxPoint origin_of(GdkWindow* window)
{
    gint x = 0, y = 0;
    gdk_window_get_origin(window, &x, &y);
    return {x, y};
}

wxPoint window_origin(GtkWidget* window) { return origin_of(gtk_widget_get_window(window)); }

wxPoint content_origin(GtkWidget* window)
{
    wxPoint at = window_origin(window);
    if (GtkWidget* content = content_of(window); content != window) {
        GtkAllocation a;
        gtk_widget_get_allocation(content, &a);
        at += wxPoint(a.x, a.y);
    }
    return at;
}

// Our top-level wx window whose GtkWindow `window` is, or null.
wxTopLevelWindow* wx_window_of(GtkWidget* window)
{
    for (wxWindow* w : wxTopLevelWindows)
        if (w->GetHandle() == window)
            return dynamic_cast<wxTopLevelWindow*>(w);
    return nullptr;
}

} // namespace

std::shared_ptr<CaptureSessions> open_capture_sessions() { return nullptr; }

bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* /*sessions*/, Capture& capture, std::string& err)
{
    wxImage&             out         = capture.image;
    bool&                approximate = capture.approximate;
    std::string&         note        = capture.note;
    std::vector<wxRect>& covered     = capture.covered;
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
        capture.from_screen = true;
        return true;
    }
#endif
    // Wayland offers no way to read a window back; draw the widget tree into an image instead.
    // Widgets drawn by GTK come out right; web views may stay blank, and the 3D canvases and our popups
    // are composited by the caller.
    if (!draw_widget_tree(widget, gtk_widget_get_scale_factor(content_of(widget)), out)) {
        err = "The window is still being laid out; capture it again in a moment";
        return false;
    }
    approximate = true;
    note        = "drawn from the widget tree (Wayland); web views may be blank";
    return true;
}

std::vector<PopupCapture> capture_popups()
{
    std::vector<PopupCapture> out;
    if (window_positions_known()) // X11 window captures read them from the screen
        return out;
    // wx shows a popup menu attached to no widget, so GTK makes it transient for no window: it shows over the
    // top-level window of the one popup_menu() was given, and GTK placed it relative to that one.
    struct WxMenu
    {
        GtkWidget*        popup;
        wxTopLevelWindow* window;
        wxWindow*         from;
    };
    std::vector<WxMenu> menus;
    for (const auto& [menu, from] : shown_menus())
        if (menu->m_menu != nullptr)
            menus.push_back({gtk_widget_get_toplevel(menu->m_menu), dynamic_cast<wxTopLevelWindow*>(owning_window(from)), from});
    // wx's own popups, which wx places itself.
    std::vector<GtkWidget*> wx_popups;
    for (wxWindow* top : wxTopLevelWindows)
        for (wxWindow* popup : popups_of(top))
            wx_popups.push_back(static_cast<GtkWidget*>(popup->GetHandle()));
    // Where a popup's geometry, the part that is not shadow, is on the content of the window it shows over.
    // GDK puts a popup's origin in its root coordinates at that offset from the content's origin, less where
    // the window GTK placed it relative to (`anchor`) is in the one it shows over: wx's menus are placed
    // relative to the window popup_menu() was given, a submenu or a drop-down menu relative to the widget it
    // is attached to, and a tooltip relative to the window it shows over. wx places its own popups at their
    // geometry, in the same coordinates. See calculate_moved_to_rect_result() and create_simple_positioner()
    // in GTK 3's gdkwindow-wayland.c.
    auto offset_on = [](GtkWidget* popup, GtkWidget* over, const wxPoint& anchor) {
        return window_origin(popup) - content_origin(over) + anchor - window_origin(over);
    };

    struct Shown
    {
        GtkWidget*        popup;
        wxTopLevelWindow* window;
        wxPoint           offset;
        int               depth;
        bool              wx;
    };
    std::vector<Shown> shown;
    // Not referenced by the list; drawing them runs no code that could destroy them, but they are held anyway.
    GList* toplevels = gtk_window_list_toplevels();
    g_list_foreach(toplevels, reinterpret_cast<GFunc>(g_object_ref), nullptr);
    for (GList* l = toplevels; l != nullptr; l = l->next) {
        GtkWidget* popup = GTK_WIDGET(l->data);
        if (gtk_window_get_window_type(GTK_WINDOW(popup)) != GTK_WINDOW_POPUP || !gtk_widget_get_mapped(popup))
            continue;
        // A popup shows over the window it is transient for, or over another popup that does, as a submenu
        // over its menu, or over the window its menu was shown from.
        GtkWidget*        at     = popup;
        wxTopLevelWindow* window = nullptr;
        wxPoint           offset(0, 0);
        int               depth = 1;
        for (;;) {
            GtkWidget* over = nullptr;
            std::optional<wxPoint> anchor;
            if (const auto menu = std::find_if(menus.begin(), menus.end(), [at](const WxMenu& m) { return m.popup == at; }); menu != menus.end()) {
                window = menu->window;
                over   = window != nullptr ? static_cast<GtkWidget*>(window->GetHandle()) : nullptr;
                GtkWidget* main = menu->from->m_wxwindow != nullptr ? menu->from->m_wxwindow : static_cast<GtkWidget*>(menu->from->GetHandle());
                if (gtk_widget_get_window(main) != nullptr)
                    anchor = origin_of(gtk_widget_get_window(main));
            } else if (GtkWindow* parent = gtk_window_get_transient_for(GTK_WINDOW(at)); parent != nullptr && depth < 16) {
                over = GTK_WIDGET(parent);
                if (gtk_window_get_window_type(parent) != GTK_WINDOW_POPUP)
                    window = wx_window_of(over);
                GtkWidget* child  = gtk_bin_get_child(GTK_BIN(at));
                GtkWidget* attach = child != nullptr && GTK_IS_MENU(child) ? gtk_menu_get_attach_widget(GTK_MENU(child)) : nullptr;
                if (attach != nullptr && gtk_widget_get_window(attach) != nullptr)
                    anchor = origin_of(gtk_widget_get_window(attach));
            }
            if (over == nullptr)
                break;
            offset += offset_on(at, over, anchor.value_or(window_origin(over)));
            if (gtk_window_get_window_type(GTK_WINDOW(over)) != GTK_WINDOW_POPUP)
                break;
            at = over;
            ++depth;
        }
        if (window != nullptr && window->IsShownOnScreen() && !hidden_from_automation(window))
            shown.push_back({popup, window, offset, depth, std::find(wx_popups.begin(), wx_popups.end(), popup) != wx_popups.end()});
    }
    // Submenus over their menus; among equals, the list's order, oldest first.
    std::stable_sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.depth < b.depth; });
    for (const Shown& s : shown) {
        PopupCapture p;
        p.owner  = s.window;
        p.offset = s.offset;
        // At its window's scale, so it composites into that window's capture as it is. A wx popup has no
        // shadow, so its content is its geometry.
        const int scale = gtk_widget_get_scale_factor(static_cast<GtkWidget*>(s.window->GetHandle()));
        if (s.wx ? draw_widget_tree(s.popup, scale, p.image) : draw_popup_window(s.popup, scale, p.image))
            out.push_back(std::move(p));
    }
    g_list_free_full(toplevels, g_object_unref);
    return out;
}

#endif // __WXGTK__

}}} // namespace Slic3r::GUI::Mcp
