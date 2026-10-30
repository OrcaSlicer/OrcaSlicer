// macOS window capture for automation screenshots (see ScreenCapture.hpp).

#include "ScreenCapture.hpp"

#include <wx/toplevel.h>

#include <memory>
#include <string>

#import <AppKit/AppKit.h>

namespace Slic3r { namespace GUI { namespace Mcp {

std::shared_ptr<CaptureSessions> open_capture_sessions() { return nullptr; }

bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* /*sessions*/, wxImage& out, bool& approximate, std::string& note,
                           std::vector<wxRect>& /*covered*/, std::string& err)
{
    NSView* view = (NSView*) window.GetHandle();
    if (view == nil) {
        err = "The window has no native view";
        return false;
    }
    // The content view draws itself into a bitmap, so this needs no screen-recording permission
    // and works for covered windows. Layer-backed OpenGL and web views don't draw this way; the 3D
    // canvases are composited from their own frames afterwards, web views may stay blank.
    NSRect              bounds = [view bounds];
    NSBitmapImageRep*   rep    = [view bitmapImageRepForCachingDisplayInRect:bounds];
    if (rep == nil) {
        err = "Could not create a bitmap for the window";
        return false;
    }
    [view cacheDisplayInRect:bounds toBitmapImageRep:rep];

    const NSInteger w = [rep pixelsWide], h = [rep pixelsHigh];
    const NSInteger spp = [rep samplesPerPixel], bpr = [rep bytesPerRow];
    const unsigned char* data = [rep bitmapData];
    if (data == nullptr || w <= 0 || h <= 0 || spp < 3 || [rep bitsPerSample] != 8) {
        err = "Unsupported window bitmap format";
        return false;
    }
    out.Create(int(w), int(h), false);
    unsigned char* rgb = out.GetData();
    for (NSInteger y = 0; y < h; ++y) {
        const unsigned char* row = data + y * bpr;
        for (NSInteger x = 0; x < w; ++x, rgb += 3) {
            rgb[0] = row[x * spp + 0];
            rgb[1] = row[x * spp + 1];
            rgb[2] = row[x * spp + 2];
        }
    }
    approximate = true;
    note        = "drawn from the view hierarchy; web views may be blank";
    return true;
}

}}} // namespace Slic3r::GUI::Mcp
