#include <unistd.h>
#include <sys/sysctl.h>
#import <Cocoa/Cocoa.h>
#import <wx/osx/cocoa/dataview.h>
#import "GUI_Utils.hpp"
#include "libslic3r/Color.hpp"
#include <dispatch/dispatch.h>
#include <functional>
#include <optional>
#include <utility>

namespace Slic3r {
namespace GUI {

void dataview_remove_insets(wxDataViewCtrl* dv) {
    NSScrollView* scrollview = (NSScrollView*) ((wxCocoaDataViewControl*)dv->GetDataViewPeer())->GetWXWidget();
    NSOutlineView* outlineview = scrollview.documentView;
    [outlineview setIntercellSpacing: NSMakeSize(0.0, 1.0)];
    if (@available(macOS 11, *)) {
        [outlineview setStyle:NSTableViewStylePlain];
    }
}

void staticbox_remove_margin(wxStaticBox* sb) {
    NSBox* nativeBox = (NSBox*)sb->GetHandle();
    [nativeBox setBoxType:NSBoxCustom];
    [nativeBox setBorderWidth:0];
}

// wxOSX SetShape only clears the window background; it cannot clip to a region. Clipping the
// window's view layer to a rounded rect is what actually rounds the opaque webview inside.
void set_window_corner_radius(wxWindow* win, int radius) {
    if (!win)
        return;
    NSView* view = (NSView*)win->GetHandle();
    if (!view)
        return;
    NSWindow* window = [view window];
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [view setWantsLayer:YES];
    [[view layer] setCornerRadius:radius];
    [[view layer] setMasksToBounds:YES];
}

void sample_screen_color(std::function<void(std::optional<ColorRGBA>)> done)
{
    // Owned until handler runs; handler is always called, with nil on cancel.
    __block NSColorSampler* sampler = [[NSColorSampler alloc] init];
    __block auto callback = std::move(done);
    [sampler showSamplerWithSelectionHandler:^(NSColor* picked) {
        NSColor* srgb = picked ? [picked colorUsingColorSpace:[NSColorSpace sRGBColorSpace]] : nil;
        std::optional<ColorRGBA> color;
        if (srgb)
            color = ColorRGBA(static_cast<float>([srgb redComponent]), static_cast<float>([srgb greenComponent]),
                              static_cast<float>([srgb blueComponent]), 1.f);
        dispatch_async(dispatch_get_main_queue(), ^{
            [sampler release];
            sampler = nil;
            auto finished = std::move(callback);
            callback = nullptr;
            finished(color);
        });
    }];
}

bool is_debugger_present()
// Returns true if the current process is being debugged (either
// running under the debugger or has a debugger attached post facto).
// https://stackoverflow.com/a/2200786/3289421
{
    int                 junk;
    int                 mib[4];
    struct kinfo_proc   info;
    size_t              size;

    // Initialize the flags so that, if sysctl fails for some bizarre
    // reason, we get a predictable result.

    info.kp_proc.p_flag = 0;

    // Initialize mib, which tells sysctl the info we want, in this case
    // we're looking for information about a specific process ID.

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    // Call sysctl.

    size = sizeof(info);
    junk = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
    assert(junk == 0);

    // We're being debugged if the P_TRACED flag is set.

    return ( (info.kp_proc.p_flag & P_TRACED) != 0 );
}

}
}

