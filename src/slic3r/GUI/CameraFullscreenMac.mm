#include "CameraFullscreenMac.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

#include <wx/window.h>

namespace Slic3r { namespace GUI {

namespace {

constexpr unsigned short EscapeKeyCode = 53;

constexpr NSInteger kCameraFullscreenWindowLevel = NSModalPanelWindowLevel;

struct CameraFullscreenEscapeMonitor
{
    id monitor{ nil };
    CameraFullscreenEscapeCallback callback{ nullptr };
    void *context{ nullptr };
};

struct CameraFullscreenPresentationState
{
    NSApplicationPresentationOptions presentation_options{ NSApplicationPresentationDefault };
};

NSWindow *camera_fullscreen_window(wxWindow *window)
{
    if (!window || !window->GetHandle())
        return nil;

    NSView *view = (NSView *) window->GetHandle();
    return [view window];
}

} // namespace

void *install_camera_fullscreen_escape_monitor(CameraFullscreenEscapeCallback callback, void *context)
{
    auto *state = new CameraFullscreenEscapeMonitor;
    state->callback = callback;
    state->context = context;
    state->monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        if ([event keyCode] == EscapeKeyCode) {
            if (state->callback)
                state->callback(state->context);
            return nil;
        }
        return event;
    }];
    return state;
}

void uninstall_camera_fullscreen_escape_monitor(void *monitor)
{
    auto *state = static_cast<CameraFullscreenEscapeMonitor *>(monitor);
    if (!state)
        return;
    if (state->monitor)
        [NSEvent removeMonitor:state->monitor];
    delete state;
}

void *enter_camera_fullscreen_presentation(wxWindow *window)
{
    auto *state = new CameraFullscreenPresentationState;
    state->presentation_options = [NSApp presentationOptions];

    NSApplicationPresentationOptions fullscreen_options = state->presentation_options;
    fullscreen_options &= ~(NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar);
    fullscreen_options |= NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar;
    [NSApp setPresentationOptions:fullscreen_options];

    apply_camera_fullscreen_frame(window);
    return state;
}

void apply_camera_fullscreen_frame(wxWindow *window)
{
    NSWindow *ns_window = camera_fullscreen_window(window);
    if (!ns_window)
        return;

    NSScreen *screen = [ns_window screen] ?: [NSScreen mainScreen];
    if (!screen)
        return;

    // Pin to the borderless-fullscreen level so MainFrame and ordinary floating
    // popups stay below; see kCameraFullscreenWindowLevel for the rationale on
    // why we intentionally stay BELOW the menu bar / Dock.
    [ns_window setLevel:kCameraFullscreenWindowLevel];
    [ns_window setFrame:[screen frame] display:YES animate:NO];
}

void restore_camera_fullscreen_presentation(void *presentation)
{
    auto *state = static_cast<CameraFullscreenPresentationState *>(presentation);
    if (!state)
        return;

    [NSApp setPresentationOptions:state->presentation_options];
    delete state;
}

void suspend_camera_fullscreen_topmost(wxWindow *window)
{
    NSWindow *ns_window = camera_fullscreen_window(window);
    if (!ns_window)
        return;

    [ns_window setLevel:NSNormalWindowLevel];
    [ns_window orderBack:nil];
}

void resume_camera_fullscreen_topmost(wxWindow *window)
{
    NSWindow *ns_window = camera_fullscreen_window(window);
    if (!ns_window)
        return;

    [ns_window setLevel:kCameraFullscreenWindowLevel];
    [ns_window orderFront:nil];
}

void attach_camera_fullscreen_overlay(wxWindow *parent, wxWindow *overlay)
{
    NSWindow *parent_ns  = camera_fullscreen_window(parent);
    NSWindow *overlay_ns = camera_fullscreen_window(overlay);
    if (!parent_ns || !overlay_ns)
        return;
    if ([overlay_ns parentWindow] == parent_ns)
        return;

    if (NSWindow *current_parent = [overlay_ns parentWindow])
        [current_parent removeChildWindow:overlay_ns];
    [parent_ns addChildWindow:overlay_ns ordered:NSWindowAbove];
}

void detach_camera_fullscreen_overlay(wxWindow *parent, wxWindow *overlay)
{
    NSWindow *parent_ns  = camera_fullscreen_window(parent);
    NSWindow *overlay_ns = camera_fullscreen_window(overlay);
    if (!parent_ns || !overlay_ns)
        return;
    if ([overlay_ns parentWindow] != parent_ns)
        return;
    [parent_ns removeChildWindow:overlay_ns];
}

void raise_main_window_after_camera_fullscreen(wxWindow *top_level)
{
    NSWindow *ns_window = camera_fullscreen_window(top_level);
    if (!ns_window)
        return;

    if ([ns_window isMiniaturized])
        [ns_window deminiaturize:nil];

    [NSApp activateIgnoringOtherApps:YES];
    [ns_window makeKeyAndOrderFront:nil];
}

}} // namespace Slic3r::GUI

#endif
