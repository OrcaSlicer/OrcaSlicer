// macOS window capture for automation screenshots and recordings (see ScreenCapture.hpp).

#include "ScreenCapture.hpp"

#include "UiInspector.hpp"

#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>
#include <unistd.h>

// ScreenCaptureKit lets a process capture its own windows without the Screen Recording permission from macOS
// 14.4 on; an older SDK has no such call, and its builds draw views only.
#if defined(MAC_OS_VERSION_14_4) && __MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_14_4
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#define ORCA_SCREEN_CAPTURE_KIT 1
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// A top-level window's NSWindow: wx's handle for one is its content view.
NSWindow* ns_window_of(wxWindow& window)
{
    NSView* view = (NSView*) window.GetHandle();
    return view != nil ? [view window] : nil;
}

// The view drawn into a bitmap, which needs no permission and works for covered windows and windows on another
// Space. Layer-backed OpenGL and web views don't draw this way.
bool draw_view(NSView* view, wxImage& out, std::string& err)
{
    if (view == nil) {
        err = "The window has no native view";
        return false;
    }
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
    return true;
}

} // namespace

#ifdef ORCA_SCREEN_CAPTURE_KIT

namespace {

// How long the UI thread waits for the window server; a first capture takes about 60 ms.
constexpr std::chrono::seconds kAnswerWait{2};

// Why the window server does not capture for the rest of the run: it did not answer in time three times, as it
// would not if its answers needed the UI thread, which waits for them. Waiting again at every capture would hold
// the UI thread that long each time. UI thread.
std::string& unanswered()
{
    static std::string reason;
    return reason;
}

// What a ScreenCaptureKit call hands its completion handler, which runs on one of ScreenCaptureKit's queues.
// The block holds it too, so an answer that comes after the UI thread stopped waiting has somewhere to go.
struct Answer
{
    std::mutex              mutex;
    std::condition_variable arrived;
    bool                    done   = false;
    id                      object = nil;     // retained
    CGImageRef              image  = nullptr; // retained
    std::string             error;

    ~Answer()
    {
        [object release];
        CGImageRelease(image);
    }
    void set(id o, CGImageRef i, NSError* e)
    {
        @autoreleasepool {
            std::lock_guard<std::mutex> lock(mutex);
            object = [o retain];
            image  = i != nullptr ? CGImageRetain(i) : nullptr;
            if (e != nil)
                error = [[e localizedDescription] UTF8String] ?: "unknown error";
            done = true;
        }
        arrived.notify_all();
    }
    // False, with `err`, when no answer came in time; the third time turns window-server capture off for the run.
    bool wait(const char* what, std::string& err)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (arrived.wait_for(lock, kAnswerWait, [this] { return done; }))
            return true;
        static int timeouts = 0;
        err                 = std::string(what) + " did not answer in time";
        if (++timeouts >= 3)
            unanswered() = err;
        BOOST_LOG_TRIVIAL(warning) << "MCP capture: " << err << (timeouts >= 3 ? "; drawing views for the rest of the run" : "");
        return false;
    }
};

// The pixels of `image` as sRGB, or false when they cannot be read. ScreenCaptureKit hands BGRA, premultiplied,
// which is read as it is; a pixel no window covers, as at a window's rounded corners, reads black.
bool read_image(CGImageRef image, wxImage& out)
{
    const size_t       w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    const CGBitmapInfo info  = CGImageGetBitmapInfo(image);
    const uint32_t     alpha = info & kCGBitmapAlphaInfoMask;
    CFDataRef          data  = nullptr;
    size_t             bpr   = 0;
    if (CGImageGetBitsPerPixel(image) == 32 && (info & kCGBitmapByteOrderMask) == kCGBitmapByteOrder32Little &&
        (alpha == kCGImageAlphaPremultipliedFirst || alpha == kCGImageAlphaNoneSkipFirst)) {
        data = CGDataProviderCopyData(CGImageGetDataProvider(image));
        bpr  = CGImageGetBytesPerRow(image);
    } else {
        // Another layout: drawn into one of ours.
        CFMutableDataRef pixels = CFDataCreateMutable(nullptr, CFIndex(w * h * 4));
        if (pixels == nullptr)
            return false;
        CFDataSetLength(pixels, CFIndex(w * h * 4));
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef    ctx   = CGBitmapContextCreate(CFDataGetMutableBytePtr(pixels), w, h, 8, w * 4, space,
                                                      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(space);
        if (ctx == nullptr) {
            CFRelease(pixels);
            return false;
        }
        CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), image);
        CGContextRelease(ctx);
        data = pixels;
        bpr  = w * 4;
    }
    if (data == nullptr)
        return false;
    if (w == 0 || h == 0 || bpr < w * 4 || size_t(CFDataGetLength(data)) < bpr * (h - 1) + w * 4) {
        CFRelease(data);
        return false;
    }
    if (!out.Create(int(w), int(h), false)) {
        CFRelease(data);
        return false;
    }
    const unsigned char* base = CFDataGetBytePtr(data);
    unsigned char*       rgb  = out.GetData();
    for (size_t y = 0; y < h; ++y) {
        const unsigned char* bgra = base + y * bpr;
        for (size_t x = 0; x < w; ++x, bgra += 4, rgb += 3) {
            rgb[0] = bgra[2];
            rgb[1] = bgra[1];
            rgb[2] = bgra[0];
        }
    }
    CFRelease(data);
    return true;
}

// The window numbers of what a capture leaves out, sorted: our shown windows that automation leaves alone (the
// Action Replay window), their wx popups, and the menus and tooltips AppKit shows over them. AppKit does not say which
// window a menu or tooltip opened from; it shows over that one, so each of our windows that is not wx's goes
// with the topmost of wx's windows below it that it overlaps, as on Windows.
std::vector<NSInteger> excluded_windows()
{
    std::map<NSInteger, bool> wx; // wx's shown windows and popups, and whether automation leaves them alone
    bool                      any = false;
    auto                      add = [&](wxWindow& w, bool hidden) {
        if (NSWindow* window = ns_window_of(w); window != nil && [window windowNumber] > 0)
            wx[[window windowNumber]] = hidden;
    };
    for (wxWindow* top : wxTopLevelWindows) {
        if (!top->IsShown())
            continue;
        const bool hidden = hidden_from_automation(top);
        any               = any || hidden;
        add(*top, hidden);
        for (wxWindow* popup : popups_of(top))
            add(*popup, hidden);
    }
    std::vector<NSInteger> out;
    if (!any)
        return out;
    struct Shown
    {
        NSInteger number;
        CGRect    bounds;
    };
    std::vector<Shown> ours; // topmost first
    if (CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)) {
        const pid_t self = getpid();
        for (NSDictionary* info in (NSArray*) list) {
            CGRect r;
            if ([info[(id) kCGWindowOwnerPID] intValue] == self &&
                CGRectMakeWithDictionaryRepresentation((CFDictionaryRef) info[(id) kCGWindowBounds], &r))
                ours.push_back({NSInteger([info[(id) kCGWindowNumber] integerValue]), r});
        }
        CFRelease(list);
    }
    for (std::size_t i = 0; i < ours.size(); ++i) {
        if (const auto it = wx.find(ours[i].number); it != wx.end()) {
            if (it->second)
                out.push_back(ours[i].number);
            continue;
        }
        for (std::size_t j = i + 1; j < ours.size(); ++j)
            if (const auto below = wx.find(ours[j].number); below != wx.end() && CGRectIntersectsRect(ours[i].bounds, ours[j].bounds)) {
                if (below->second)
                    out.push_back(ours[i].number);
                break;
            }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// What a capture through ScreenCaptureKit keeps from one frame to the next: the process's shareable content
// and a filter per display. Both are fetched again when the excluded windows change or a window shows on a
// display the content does not list. UI thread.
class CaptureSessions
{
public:
    CaptureSessions() = default;
    CaptureSessions(const CaptureSessions&)            = delete;
    CaptureSessions& operator=(const CaptureSessions&) = delete;
    ~CaptureSessions() { reset(); }

    // The window's client area as the screen shows our windows there, or false with `err`. A failure of
    // ScreenCaptureKit itself is kept, so a recording does not ask it again at every frame.
    API_AVAILABLE(macos(14.4)) bool capture(wxTopLevelWindow& window, Capture& out, std::string& err);

private:
    API_AVAILABLE(macos(14.4)) SCContentFilter* filter_for(CGDirectDisplayID display, std::string& err);
    void reset()
    {
        [m_content release];
        m_content = nil;
        for (auto& [display, filter] : m_filters)
            [filter release];
        m_filters.clear();
    }

    id                              m_content = nil; // SCShareableContent, retained
    std::vector<NSInteger>          m_excluded;      // the windows the filters leave out
    std::map<CGDirectDisplayID, id> m_filters;       // SCContentFilter, retained
    std::string                     m_failure;
};

SCContentFilter* CaptureSessions::filter_for(CGDirectDisplayID display, std::string& err)
{
    std::vector<NSInteger> excluded = excluded_windows();
    auto                   listed   = [&] {
        for (SCDisplay* d in [(SCShareableContent*) m_content displays])
            if ([d displayID] == display)
                return d;
        return (SCDisplay*) nil;
    };
    if (m_content == nil || excluded != m_excluded || listed() == nil) {
        reset();
        auto answer = std::make_shared<Answer>();
        [SCShareableContent getCurrentProcessShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
            answer->set(content, nullptr, error);
        }];
        if (!answer->wait("ScreenCaptureKit's list of windows", err))
            return nil;
        std::lock_guard<std::mutex> lock(answer->mutex);
        if (answer->object == nil) {
            err = "ScreenCaptureKit did not list the windows: " + answer->error;
            return nil;
        }
        m_content  = [answer->object retain];
        m_excluded = std::move(excluded);
    }
    if (auto it = m_filters.find(display); it != m_filters.end())
        return (SCContentFilter*) it->second;
    SCShareableContent*   content = (SCShareableContent*) m_content;
    SCDisplay*            shown   = listed();
    SCRunningApplication* self    = nil;
    for (SCRunningApplication* app in [content applications])
        if ([app processID] == getpid())
            self = app;
    if (shown == nil || self == nil) {
        err = shown == nil ? "ScreenCaptureKit does not list the window's display" : "ScreenCaptureKit does not list OrcaSlicer";
        return nil;
    }
    NSMutableArray<SCWindow*>* except = [NSMutableArray array];
    for (SCWindow* w in [content windows])
        if (std::binary_search(m_excluded.begin(), m_excluded.end(), NSInteger([w windowID])))
            [except addObject:w];
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:shown includingApplications:@[self] exceptingWindows:except];
    m_filters[display]      = filter; // the retain alloc gave
    return filter;
}

bool CaptureSessions::capture(wxTopLevelWindow& window, Capture& out, std::string& err)
{
    if (!unanswered().empty() || !m_failure.empty()) {
        err = !unanswered().empty() ? unanswered() : m_failure;
        return false;
    }
    // The display the window shows on is its screen, the one with most of it.
    NSWindow* ns     = ns_window_of(window);
    NSScreen* screen = ns != nil ? [ns screen] : nil;
    if (screen == nil || ![ns isVisible] || [NSApp isHidden] || ![ns isOnActiveSpace]) {
        err = "the window is not on the screen";
        return false;
    }
    // wx's screen coordinates on macOS are the global display space's points, from the top left of the main
    // display, as CoreGraphics has them.
    const wxPoint origin = window.ClientToScreen(wxPoint(0, 0));
    const wxSize  size   = window.GetClientSize();
    if (size.x <= 0 || size.y <= 0) {
        err = "the window has no client area";
        return false;
    }
    // The window server shows only the part of a window on its display (and ScreenCaptureKit clamps the source
    // to it), where the views show all of it, so a window not wholly on its display is drawn from its views.
    const CGRect            client  = CGRectMake(origin.x, origin.y, size.x, size.y);
    const CGDirectDisplayID display = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    const CGRect            bounds  = CGDisplayBounds(display);
    if (!CGRectContainsRect(bounds, client)) {
        err = "part of the window is off its display";
        return false;
    }

    // The window server shows what was last committed, while the views may have changed since in this turn of
    // the event loop: they are drawn and committed first.
    for (NSWindow* w in [NSApp windows])
        if ([w isVisible])
            [w displayIfNeeded];
    [CATransaction flush];

    SCContentFilter* filter = filter_for(display, err);
    if (filter == nil) {
        m_failure = err;
        return false;
    }
    // The window's scale is its screen's, which ScreenCaptureKit captures at with the best resolution.
    const double scale = [ns backingScaleFactor];
    const size_t w = size_t(std::lround(size.x * scale)), h = size_t(std::lround(size.y * scale));
    SCStreamConfiguration* config = [[[SCStreamConfiguration alloc] init] autorelease];
    [config setSourceRect:CGRectOffset(client, -bounds.origin.x, -bounds.origin.y)];
    [config setWidth:w];
    [config setHeight:h];
    [config setCaptureResolution:SCCaptureResolutionBest];
    [config setPixelFormat:kCVPixelFormatType_32BGRA];
    [config setColorSpaceName:kCGColorSpaceSRGB];
    [config setShowsCursor:NO]; // a recording draws its own pointer
    auto answer = std::make_shared<Answer>();
    [SCScreenshotManager captureImageWithFilter:filter configuration:config completionHandler:^(CGImageRef captured, NSError* error) {
        answer->set(nil, captured, error);
    }];
    if (!answer->wait("the window server", err)) {
        m_failure = err;
        return false;
    }
    bool read = false;
    {
        std::lock_guard<std::mutex> lock(answer->mutex);
        if (answer->image == nullptr)
            err = "ScreenCaptureKit failed: " + answer->error;
        else if (CGImageGetWidth(answer->image) != w || CGImageGetHeight(answer->image) != h)
            err = "ScreenCaptureKit returned an image of another size";
        else if (!(read = read_image(answer->image, out.image)))
            err = "ScreenCaptureKit's image could not be read";
    }
    if (!read) {
        // A failure is kept for the session: a recording draws views from then on rather than ask at every frame.
        m_failure = err;
        BOOST_LOG_TRIVIAL(warning) << "MCP capture: " << err << "; drawing views instead";
        return false;
    }

    // Our windows over it, where a 3D view composited in must not go. The excluded ones are not drawn.
    if (CFArrayRef above = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenAboveWindow | kCGWindowListExcludeDesktopElements,
                                                      CGWindowID([ns windowNumber]))) {
        const pid_t self = getpid();
        for (NSDictionary* info in (NSArray*) above) {
            CGRect r;
            if ([info[(id) kCGWindowOwnerPID] intValue] != self || [info[(id) kCGWindowAlpha] doubleValue] <= 0. ||
                std::binary_search(m_excluded.begin(), m_excluded.end(), NSInteger([info[(id) kCGWindowNumber] integerValue])) ||
                !CGRectMakeWithDictionaryRepresentation((CFDictionaryRef) info[(id) kCGWindowBounds], &r))
                continue;
            r = CGRectIntersection(r, client);
            if (CGRectIsEmpty(r))
                continue;
            const int x0 = int(std::floor((r.origin.x - client.origin.x) * scale)), y0 = int(std::floor((r.origin.y - client.origin.y) * scale));
            const int x1 = int(std::ceil((CGRectGetMaxX(r) - client.origin.x) * scale)), y1 = int(std::ceil((CGRectGetMaxY(r) - client.origin.y) * scale));
            out.covered.emplace_back(x0, y0, x1 - x0, y1 - y0);
        }
        CFRelease(above);
    }
    out.from_screen    = true;
    out.canvases_shown = true;
    return true;
}

#endif // ORCA_SCREEN_CAPTURE_KIT

std::shared_ptr<CaptureSessions> open_capture_sessions()
{
#ifdef ORCA_SCREEN_CAPTURE_KIT
    if (@available(macOS 14.4, *))
        return std::make_shared<CaptureSessions>();
#endif
    return nullptr;
}

bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* sessions, Capture& out, std::string& err)
{
    std::string screen_err;
#ifdef ORCA_SCREEN_CAPTURE_KIT
    if (@available(macOS 14.4, *)) {
        std::shared_ptr<CaptureSessions> own;
        if (sessions == nullptr) {
            own      = std::make_shared<CaptureSessions>();
            sessions = own.get();
        }
        if (sessions->capture(window, out, screen_err))
            return true;
    }
#endif
    // The 3D canvases are composited from their own frames afterwards; web views may stay blank.
    if (!draw_view((NSView*) window.GetHandle(), out.image, err))
        return false;
    out.approximate = true;
    out.note        = "drawn from the view hierarchy; web views may be blank";
    if (!screen_err.empty())
        out.note += " (" + screen_err + ")";
    return true;
}

// A wx popup's content view is all of it: wx makes it a borderless window. A popup wx still reports shown can
// have its window ordered out, off the screen; only one whose window is on the screen is drawn.
bool capture_native_popup(wxWindow& popup, wxImage& out)
{
    NSView*     view = (NSView*) popup.GetHandle();
    std::string err;
    return view != nil && [[view window] isVisible] && draw_view(view, out, err);
}

// AppKit draws its menus and tooltips itself; they show only in a capture from the window server.
std::vector<PopupCapture> capture_native_popups() { return {}; }

}}} // namespace Slic3r::GUI::Mcp
