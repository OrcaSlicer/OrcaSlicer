#include "ScreenCapture.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/glcanvas.h>
#include <wx/mstream.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__WXGTK__)
#include <gtk/gtk.h>
#ifdef wxHAVE_GDK_X11
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

void composite(wxImage& dst, const wxImage& src, const wxPoint& at)
{
    if (!dst.IsOk() || !src.IsOk())
        return;
    const int x0 = std::max(0, at.x), y0 = std::max(0, at.y);
    const int x1 = std::min(dst.GetWidth(), at.x + src.GetWidth()), y1 = std::min(dst.GetHeight(), at.y + src.GetHeight());
    for (int y = y0; y < y1; ++y) {
        const unsigned char* s = src.GetData() + (size_t(y - at.y) * size_t(src.GetWidth()) + size_t(x0 - at.x)) * 3;
        unsigned char*       d = dst.GetData() + (size_t(y) * size_t(dst.GetWidth()) + size_t(x0)) * 3;
        std::copy(s, s + size_t(std::max(0, x1 - x0)) * 3, d);
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

std::optional<Capture> capture_canvas(GLCanvas3D& canvas, std::string& err)
{
    Capture c;
    if (!canvas.render_and_capture(c.image, err))
        return std::nullopt;
    return c;
}

std::optional<Capture> capture_window(wxTopLevelWindow& window, std::string& err)
{
    if (window.IsIconized() || !window.IsShownOnScreen()) {
        err = "The window is minimized or hidden";
        return std::nullopt;
    }
    Capture c;
    if (!capture_native_window(window, c.image, c.approximate, c.note, err))
        return std::nullopt;

    // Composite every visible 3D canvas inside this window from an exact frame capture.
    Plater* plater = wxGetApp().plater();
    if (plater != nullptr)
        for (GLCanvas3D* canvas : {plater->get_view3D_canvas3D(), plater->get_preview_canvas3D(), plater->get_assmeble_canvas3D()}) {
            if (canvas == nullptr || canvas->get_wxglcanvas() == nullptr)
                continue;
            wxGLCanvas* glc = canvas->get_wxglcanvas();
            if (!glc->IsShownOnScreen() || wxGetTopLevelParent(glc) != &window)
                continue;
            std::string canvas_err;
            wxImage     frame;
            if (!canvas->render_and_capture(frame, canvas_err)) {
                c.approximate = true;
                c.note += (c.note.empty() ? "" : "; ") + std::string("3D view not composited: ") + canvas_err;
                continue;
            }
            composite(c.image, frame, client_offset_in(*glc, window));
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

bool capture_native_window(wxTopLevelWindow& window, wxImage& out, bool& approximate, std::string& note, std::string& err)
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
        if (rx < 0 || ry < 0 || rx + attr.width > root.width || ry + attr.height > root.height) {
            approximate = true;
            note        = "part of the window is off-screen";
        }
        XImage* img = XGetImage(dpy, xid, 0, 0, unsigned(attr.width), unsigned(attr.height), AllPlanes, ZPixmap);
        if (img == nullptr) {
            err = "XGetImage failed";
            return false;
        }
        out.Create(attr.width, attr.height, false);
        unsigned char* rgb   = out.GetData();
        auto           shift = [](unsigned long mask) {
            int s = 0;
            while (mask != 0 && (mask & 1) == 0) {
                mask >>= 1;
                ++s;
            }
            return s;
        };
        const int rs = shift(img->red_mask), gs = shift(img->green_mask), bs = shift(img->blue_mask);
        for (int y = 0; y < attr.height; ++y)
            for (int x = 0; x < attr.width; ++x, rgb += 3) {
                const unsigned long p = XGetPixel(img, x, y);
                rgb[0]                = (unsigned char) ((p & img->red_mask) >> rs);
                rgb[1]                = (unsigned char) ((p & img->green_mask) >> gs);
                rgb[2]                = (unsigned char) ((p & img->blue_mask) >> bs);
            }
        XDestroyImage(img);
        return true;
    }
#endif
    // Wayland offers no way to read a window back; draw the widget tree into an image instead.
    // Widgets drawn by GTK come out right; web views may stay blank, and the 3D canvases are
    // composited by the caller.
    const int scale = gtk_widget_get_scale_factor(widget);
    const int w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t* cr = cairo_create(surface);
    gtk_widget_draw(widget, cr);
    cairo_destroy(cr);
    out = from_cairo(surface);
    cairo_surface_destroy(surface);
    approximate = true;
    note        = "drawn from the widget tree (Wayland); web views may be blank";
    return out.IsOk();
}

#endif // __WXGTK__

}}} // namespace Slic3r::GUI::Mcp
