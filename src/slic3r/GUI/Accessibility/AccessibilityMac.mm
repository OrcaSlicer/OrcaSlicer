#include "AccessibilityMac.h"

#include <objc/runtime.h>
#include <wx/window.h>
#include "wx/osx/core/cfstring.h"
#import <Cocoa/Cocoa.h>

// ---------------------------------------------------------------------------
// VoiceOver / Voice Control accessibility for custom-drawn controls.
//
// OrcaSlicer renders its dialog controls with a plain wxNSView (backed by wxWindow),
// which assistive technologies neither name nor identify. The NSAccessibility
// attributes of those views are configured through AppKit's instance-scoped setters
// (setAccessibilityRole:, setAccessibilityTitle:, setAccessibilityEnabled:,
// setAccessibilityIdentifier:, setAccessibilityElement:), which confines the change
// to the specific views we opt in.
//
// The press action has no setter, so accessibilityPerformPress is swizzled onto
// NSView once (install_press_action()). The replacement runs the press block stored
// on the view and otherwise forwards to the original implementation, so views that
// were never configured behave exactly as before.
// ---------------------------------------------------------------------------

static const void* kOrcaAccessibilityPressKey = &kOrcaAccessibilityPressKey;

// Swizzle accessibilityPerformPress on NSView, keeping the original implementation
// reachable through the captured IMP. dispatch_once makes this safe to trigger from
// every apply() call.
static void install_press_action()
{
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        SEL sel = @selector(accessibilityPerformPress);
        Method original = class_getInstanceMethod([NSView class], sel);
        if (!original)
            return;

        IMP original_imp = method_getImplementation(original);
        IMP replacement = imp_implementationWithBlock(^BOOL(NSView* view) {
            void (^handler)(void) = objc_getAssociatedObject(view, kOrcaAccessibilityPressKey);
            if (handler) {
                handler();
                return YES;
            }
            return ((BOOL (*)(id, SEL))original_imp)(view, sel);
        });

        const char* types = method_getTypeEncoding(original);
        if (!class_addMethod([NSView class], sel, replacement, types)) {
            // NSView implements the method itself rather than inheriting it, so adding
            // failed; exchange the implementations instead. The captured original_imp
            // stays valid and is called directly, so there is no recursion risk.
            SEL stored = @selector(orca_accessibilityPerformPress);
            if (class_addMethod([NSView class], stored, replacement, types))
                method_exchangeImplementations(original, class_getInstanceMethod([NSView class], stored));
        }
    });
}

namespace Slic3r {
namespace GUI {
namespace Accessibility {

namespace {

// Maps a Role to its NSAccessibility role string. Keep in sync with Role in
// AccessibilityMac.h: every enumerator needs a case here, otherwise it falls through to
// NSAccessibilityUnknownRole.
NSString* role_string(Role role)
{
    switch (role) {
    case Role::Button:   return NSAccessibilityButtonRole;
    case Role::Checkbox: return NSAccessibilityCheckBoxRole;
    }
    return NSAccessibilityUnknownRole;
}

// Voice Control identifier. wxWindow::GetId() is stable for the window's lifetime but
// can be recycled once a window is destroyed; Voice Control's spoken-number overlay
// only needs the mapping to stay stable while the element is on screen.
wxString default_identifier(const wxWindow* win)
{
    return wxString::Format("wxID_%d", (int)win->GetId());
}

} // namespace

void apply(wxWindow* win, const Descriptor& desc)
{
    if (!win)
        return;

    NSView* view = (NSView*)win->GetHandle();
    if (!view)
        return;

    // A native control (an NSControl-backed wxWindow such as a wxBitmapToggleButton)
    // already exposes its role, value, enabled state and press action through AppKit, so
    // we only fill in the two pieces AppKit cannot derive for our controls: the name and
    // the Voice Control identifier. A custom-drawn view (a plain wxNSView) exposes none
    // of this, so it is configured in full below.
    const bool native = [view isKindOfClass:[NSControl class]];

    // The element's name. Fall back to the tooltip so icon-only controls get a name.
    wxString name = desc.name;
    if (name.empty())
        name = win->GetToolTipText();
    [view setAccessibilityTitle:name.empty() ? nil : wxCFStringRef(name).AsNSString()];

    // A stable id Voice Control uses to map spoken commands back to the element.
    [view setAccessibilityIdentifier:wxCFStringRef(default_identifier(win)).AsNSString()];

    if (native)
        return;

    install_press_action();

    [view setAccessibilityRole:role_string(desc.role)];

    // wxWindow::IsEnabled() includes disabled ancestors; kept current by re-calling
    // apply() from the widget's DoEnable() (wx cascades DoEnable to children).
    [view setAccessibilityEnabled:win->IsEnabled()];

    if (desc.press) {
        auto press = desc.press; // copy the std::function into the stored block
        objc_setAssociatedObject(view, kOrcaAccessibilityPressKey, ^{ press(); }, OBJC_ASSOCIATION_COPY_NONATOMIC);
    } else {
        objc_setAssociatedObject(view, kOrcaAccessibilityPressKey, nil, OBJC_ASSOCIATION_COPY_NONATOMIC);
    }

    // Opt this view into the accessibility tree.
    [view setAccessibilityElement:YES];
}

void detach(wxWindow* win)
{
    NSView* view = win ? (NSView*)win->GetHandle() : nil;
    if (view)
        objc_setAssociatedObject(view, kOrcaAccessibilityPressKey, nil, OBJC_ASSOCIATION_COPY_NONATOMIC);
}

} // namespace Accessibility
} // namespace GUI
} // namespace Slic3r
