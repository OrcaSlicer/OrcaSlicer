#import "MacIME.hpp"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

// The canvas view's class (wxNSCustomOpenGLView) gets the NSTextInputClient methods added at runtime.
// Declaring the protocol at runtime does not give a view an input context, so the view also overrides
// -inputContext and owns its NSTextInputContext. The client models only the composition: it never sees
// the field's text, so the composition is the whole document as far as the input method is concerned.
// Per-view state lives in associated objects. Compiled without ARC.

namespace {

char key_installed; // on the class: methods added
char key_context;   // NSTextInputContext*
char key_active;    // NSNumber: an ImGui text field wants text input
char key_marked;    // NSString*: the composition, nil when there is none
char key_selected;  // NSValue (NSRange): the selection inside the composition
char key_caret;     // NSValue (NSRect): the text cursor line in view coordinates
char key_ime_event; // NSEvent*: the last key-down that changed a composition

id get(id object, const void* key) { return objc_getAssociatedObject(object, key); }

void set(id object, const void* key, id value) { objc_setAssociatedObject(object, key, value, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }

bool is_active(id view) { return [get(view, &key_active) boolValue]; }

NSString* marked_text(id view) { return get(view, &key_marked); }

void set_marked_text(id view, NSString* text, NSRange selected)
{
    const bool composing = text.length > 0;
    set(view, &key_marked, composing ? text : nil);
    set(view, &key_selected, composing ? [NSValue valueWithRange:selected] : nil);
}

// The key-down being dispatched went to the input method.
void claim_current_key(id view)
{
    NSEvent* event = [NSApp currentEvent];
    if (event.type == NSEventTypeKeyDown)
        set(view, &key_ime_event, event);
}

NSString* plain_string(id text) { return [text isKindOfClass:[NSAttributedString class]] ? [text string] : text; }

// wx's own insertText: sends the text as wxEVT_CHAR, preceded by a key-down when it is the character of
// the key being pressed.
void send_text(id view, NSString* text)
{
    if (text.length > 0)
        [view performSelector:@selector(insertText:) withObject:text];
}

NSTextInputContext* ime_input_context(id self, SEL) { return is_active(self) ? get(self, &key_context) : nil; }

void ime_insert_text(id self, SEL, id text, NSRange)
{
    if (marked_text(self) != nil) {
        claim_current_key(self);
        set_marked_text(self, nil, {});
    }
    send_text(self, plain_string(text));
}

void ime_set_marked_text(id self, SEL, id text, NSRange selected, NSRange)
{
    claim_current_key(self);
    set_marked_text(self, plain_string(text), selected);
}

// Accepts the composition as typed.
void ime_unmark_text(id self, SEL)
{
    NSString* text = [[marked_text(self) retain] autorelease];
    if (text == nil)
        return;
    claim_current_key(self);
    set_marked_text(self, nil, {});
    send_text(self, text);
}

BOOL ime_has_marked_text(id self, SEL) { return marked_text(self) != nil; }

NSRange ime_marked_range(id self, SEL)
{
    NSString* text = marked_text(self);
    return text != nil ? NSMakeRange(0, text.length) : NSMakeRange(NSNotFound, 0);
}

NSRange ime_selected_range(id self, SEL)
{
    NSValue* selected = get(self, &key_selected);
    return selected != nil ? selected.rangeValue : NSMakeRange(NSNotFound, 0);
}

NSRect ime_first_rect(id self, SEL, NSRange range, NSRangePointer actual_range)
{
    if (actual_range != nullptr)
        *actual_range = range;
    NSView* view  = self;
    NSValue* caret = get(self, &key_caret);
    // Without a reported cursor, the top-left corner of the canvas.
    const NSRect rect = caret != nil ? caret.rectValue : NSMakeRect(0, 0, 1, 16);
    return view.window != nil ? [view.window convertRectToScreen:[view convertRect:rect toView:nil]] : NSZeroRect;
}

NSAttributedString* ime_attributed_substring(id self, SEL, NSRange range, NSRangePointer actual_range)
{
    NSString* text = marked_text(self);
    const NSRange clamped = text != nil ? NSIntersectionRange(range, NSMakeRange(0, text.length)) : NSMakeRange(0, 0);
    if (clamped.length == 0)
        return nil;
    if (actual_range != nullptr)
        *actual_range = clamped;
    return [[[NSAttributedString alloc] initWithString:[text substringWithRange:clamped]] autorelease];
}

NSArray* ime_valid_attributes(id, SEL) { return @[]; }

NSUInteger ime_character_index(id, SEL, NSPoint) { return NSNotFound; }

void install(Class cls)
{
    if (get(cls, &key_installed) != nil)
        return;
    set(cls, &key_installed, @YES);

    Protocol* protocol = @protocol(NSTextInputClient);
    class_addProtocol(cls, protocol);
    const struct { SEL sel; IMP imp; } methods[] = {
        { @selector(insertText:replacementRange:), (IMP) ime_insert_text },
        { @selector(setMarkedText:selectedRange:replacementRange:), (IMP) ime_set_marked_text },
        { @selector(unmarkText), (IMP) ime_unmark_text },
        { @selector(hasMarkedText), (IMP) ime_has_marked_text },
        { @selector(markedRange), (IMP) ime_marked_range },
        { @selector(selectedRange), (IMP) ime_selected_range },
        { @selector(firstRectForCharacterRange:actualRange:), (IMP) ime_first_rect },
        { @selector(attributedSubstringForProposedRange:actualRange:), (IMP) ime_attributed_substring },
        { @selector(validAttributesForMarkedText), (IMP) ime_valid_attributes },
        { @selector(characterIndexForPoint:), (IMP) ime_character_index },
    };
    for (const auto& method : methods)
        class_addMethod(cls, method.sel, method.imp, protocol_getMethodDescription(protocol, method.sel, YES, YES).types);
    // The view's own doCommandBySelector: (from wx) stays: keys the input method passes on reach wx unchanged.
    class_addMethod(cls, @selector(inputContext), (IMP) ime_input_context,
                    method_getTypeEncoding(class_getInstanceMethod([NSView class], @selector(inputContext))));
}

} // namespace

namespace Slic3r::GUI {

void mac_ime_set_active(void* ns_view, bool active)
{
    NSView* view = (NSView*) ns_view;
    if (view == nil || active == is_active(view))
        return;
    NSTextInputContext* context = get(view, &key_context);
    if (context == nil) {
        install([view class]);
        context = [[[NSTextInputContext alloc] initWithClient:(id<NSTextInputClient>) view] autorelease];
        set(view, &key_context, context);
    }
    // An open composition belongs to the field that lost input or to an earlier session; dropping it
    // first keeps the input method's callbacks from committing it.
    set_marked_text(view, nil, {});
    set(view, &key_active, @(active));
    if (active)
        [context activate];
    else {
        [context discardMarkedText];
        [context deactivate];
    }
}

void mac_ime_set_caret(void* ns_view, int x, int y, int height)
{
    NSView* view = (NSView*) ns_view;
    if (view == nil)
        return;
    const CGFloat scale = view.window != nil ? view.window.backingScaleFactor : 1.0;
    set(view, &key_caret, [NSValue valueWithRect:NSMakeRect(x / scale, y / scale, 1.0, height / scale)]);
}

bool mac_ime_owns_key(void* ns_view)
{
    NSView* view = (NSView*) ns_view;
    NSEvent* event = [NSApp currentEvent];
    return view != nil && is_active(view) && event.type == NSEventTypeKeyDown &&
           (marked_text(view) != nil || get(view, &key_ime_event) == event);
}

} // namespace Slic3r::GUI
