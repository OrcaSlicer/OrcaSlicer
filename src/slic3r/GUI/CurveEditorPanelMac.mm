#include "CurveEditorPanel.hpp"

#include <optional>
#include <wx/window.h>
#import <AppKit/NSApplication.h>
#import <AppKit/NSEvent.h>
#import <AppKit/NSView.h>

namespace Slic3r::GUI {

std::optional<CurveEditorPanel::NativeScroll> CurveEditorPanel::mac_scroll_input(bool horizontal) const
{
    NSEvent* event = NSApp.currentEvent;
    NSView* view = (NSView*)GetHandle();
    if (!event || !view || event.window != view.window || event.type != NSEventTypeScrollWheel) return std::nullopt;
    const bool gesture = event.phase != NSEventPhaseNone || event.momentumPhase != NSEventPhaseNone;
    // Precision alone also occurs on some mice. Gesture phases distinguish
    // fluid scrolling from an ordinary wheel, including its momentum tail.
    if (!gesture || !event.hasPreciseScrollingDeltas) return NativeScroll{gesture, {}};
    // AppKit supplies screen distances. Match wx's horizontal sign convention,
    // but keep the fractional part that its integer wheel rotation discards.
    return NativeScroll{true, horizontal ? -event.scrollingDeltaX : event.scrollingDeltaY};
}

} // namespace Slic3r::GUI
