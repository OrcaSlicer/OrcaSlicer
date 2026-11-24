#ifndef slic3r_GUI_AccessibilityMac_h_
#define slic3r_GUI_AccessibilityMac_h_

#include <functional>
#include <wx/string.h>

#ifdef __WXOSX__

class wxWindow;

namespace Slic3r {
namespace GUI {

// macOS accessibility (VoiceOver / Voice Control) for custom-drawn and native widgets.
//
// OrcaSlicer draws many controls with a plain NSView (backed by wxWindow), which
// assistive technologies neither name nor identify. This module configures the
// NSAccessibility metadata those views need so VoiceOver can focus, name and activate
// them and Voice Control can target them. The C++ API below is the only thing the rest
// of the codebase touches; every Cocoa and NSAccessibility detail stays in
// AccessibilityMac.mm.
namespace Accessibility {

// The NSAccessibility role an element advertises. Add values here as more custom
// widgets are exposed. Keep in sync with role_string() in AccessibilityMac.mm.
enum class Role {
    Button,   // NSAccessibilityButtonRole
    Checkbox, // NSAccessibilityCheckBoxRole
};

// How a wxWindow should be exposed to assistive technologies.
struct Descriptor {
    Role role{ Role::Button };
    // Accessible name. When empty, the window's tooltip is used so icon-only controls
    // still get a name.
    wxString name;
    // Default action invoked when the assistive technology activates the element. Stored
    // as a block on the window's NSView (see detach()) and may capture the widget's
    // `this`. Ignored for native controls, which have their own press action.
    std::function<void()> press;
};

// Expose `win` to assistive technologies as described. Idempotent; call again whenever
// the name or enabled state changes (for example after a button's label is updated).
//
// The call is a no-op for a native control (an NSControl-backed wxWindow, e.g. a
// wxBitmapToggleButton): AppKit already exposes its role, value, enabled state and press
// action, so apply() only fills in the name and Voice Control identifier. Custom-drawn
// views are configured in full.
void apply(wxWindow* win, const Descriptor& desc);

// Clear the press block apply() stored on the window's NSView. Call when the widget is
// destroyed so the view cannot outlive it with a press block capturing the freed widget.
void detach(wxWindow* win);

} // namespace Accessibility
} // namespace GUI
} // namespace Slic3r

#endif // __WXOSX__

#endif // !slic3r_GUI_AccessibilityMac_h_
