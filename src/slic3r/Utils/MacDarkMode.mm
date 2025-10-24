#import "MacDarkMode.hpp"
#include "../GUI/Widgets/Label.hpp"

#include "wx/osx/core/cfstring.h"

#import <algorithm>

#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <AppKit/NSScreen.h>
#import <WebKit/WebKit.h>

#include <objc/runtime.h>

@interface MacDarkMode : NSObject {}
@end

@implementation MacDarkMode

namespace Slic3r {
namespace GUI {

NSTextField* mainframe_text_field = nil;
bool is_in_full_screen_mode = false;

bool mac_dark_mode()
{
    NSString *style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    return style && [style isEqualToString:@"Dark"];

}

double mac_max_scaling_factor()
{
    double scaling = 1.;
    if ([NSScreen screens] == nil) {
        scaling = [[NSScreen mainScreen] backingScaleFactor];
    } else {
	    for (int i = 0; i < [[NSScreen screens] count]; ++ i)
	    	scaling = std::max<double>(scaling, [[[NSScreen screens] objectAtIndex:0] backingScaleFactor]);
	}
    return scaling;
}
    
void set_miniaturizable(void * window)
{
    CGFloat rFloat = 34/255.0;
    CGFloat gFloat = 34/255.0;
    CGFloat bFloat = 36/255.0;
    [(NSView*) window window].titlebarAppearsTransparent = true;
    [(NSView*) window window].backgroundColor = [NSColor colorWithCalibratedRed:rFloat green:gFloat blue:bFloat alpha:1.0];
    [(NSView*) window window].styleMask |= NSMiniaturizableWindowMask;

    NSEnumerator *viewEnum = [[[[[[[(NSView*) window window] contentView] superview] titlebarViewController] view] subviews] objectEnumerator];
    NSView *viewObject;

    while(viewObject = (NSView *)[viewEnum nextObject]) {
        if([viewObject class] == [NSTextField self]) {
            //[(NSTextField*)viewObject setTextColor :  NSColor.whiteColor];
            mainframe_text_field = viewObject;
        }
    }
}

void set_tag_when_enter_full_screen(bool isfullscreen)
{
  is_in_full_screen_mode = isfullscreen;
}

void set_title_colour_after_set_title(void * window)
{
  NSEnumerator *viewEnum = [[[[[[[(NSView*) window window] contentView] superview] titlebarViewController] view] subviews] objectEnumerator];
  NSView *viewObject;
  while(viewObject = (NSView *)[viewEnum nextObject]) {
    if([viewObject class] == [NSTextField self]) {
      [(NSTextField*)viewObject setTextColor : NSColor.whiteColor];
      mainframe_text_field = viewObject;
    }
  }

  if (mainframe_text_field) {
    [(NSTextField*)mainframe_text_field setTextColor : NSColor.whiteColor];
  }
}

void WKWebView_evaluateJavaScript(void * web, wxString const & script, void (*callback)(wxString const &))
{
    [(WKWebView*)web evaluateJavaScript:wxCFStringRef(script).AsNSString() completionHandler: ^(id result, NSError *error) {
        if (callback && error != nil) {
            wxString err = wxCFStringRef(error.localizedFailureReason).AsString();
            callback(err);
        }
    }];
}
    
void WKWebView_setTransparentBackground(void * web)
{
    WKWebView * webView = (WKWebView*)web;
    [webView layer].backgroundColor = [NSColor clearColor].CGColor;
    [webView registerForDraggedTypes: @[NSFilenamesPboardType]];
}

void openFolderForFile(wxString const & file)
{
    NSArray *fileURLs = [NSArray arrayWithObjects:wxCFStringRef(file).AsNSString(), /* ... */ nil];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:fileURLs];
}
    
}
}

@end

/* WKWebView */
@implementation WKWebView (DragDrop)

+ (void) load
{
    Method draggingEntered = class_getInstanceMethod([WKWebView class], @selector(draggingEntered:));
    Method draggingEntered2 = class_getInstanceMethod([WKWebView class], @selector(draggingEntered2:));
    method_exchangeImplementations(draggingEntered, draggingEntered2);

    Method draggingUpdated = class_getInstanceMethod([WKWebView class], @selector(draggingUpdated:));
    Method draggingUpdated2 = class_getInstanceMethod([WKWebView class], @selector(draggingUpdated2:));
    method_exchangeImplementations(draggingUpdated, draggingUpdated2);

    Method prepareForDragOperation = class_getInstanceMethod([WKWebView class], @selector(prepareForDragOperation:));
    Method prepareForDragOperation2 = class_getInstanceMethod([WKWebView class], @selector(prepareForDragOperation2:));
    method_exchangeImplementations(prepareForDragOperation, prepareForDragOperation2);

    Method performDragOperation = class_getInstanceMethod([WKWebView class], @selector(performDragOperation:));
    Method performDragOperation2 = class_getInstanceMethod([WKWebView class], @selector(performDragOperation2:));
    method_exchangeImplementations(performDragOperation, performDragOperation2);
}

- (NSDragOperation)draggingEntered2:(id<NSDraggingInfo>)sender
{
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated2:(id<NSDraggingInfo>)sender
{
    return NSDragOperationCopy;
}

- (BOOL)prepareForDragOperation2:(id<NSDraggingInfo>)info
{
    return TRUE;
}

- (BOOL)performDragOperation2:(id<NSDraggingInfo>)info
{
    NSURL* url = [NSURL URLFromPasteboard:[info draggingPasteboard]];
    if (!url) {
        return FALSE;
    }
    NSString * path = [url path];
    url = [NSURL fileURLWithPath: path];
    [self loadFileURL:url allowingReadAccessToURL:url];
    return TRUE;
}

@end

/* textColor for NSTextField */
@implementation NSTextField (textColor)

- (void)setTextColor2:(NSColor *)textColor
{
    if (Slic3r::GUI::mainframe_text_field != self){
        [self setTextColor2: textColor];
    }else{
        if(Slic3r::GUI::is_in_full_screen_mode){
            [self setTextColor2 : NSColor.darkGrayColor];
        }else{
            [self setTextColor2 : NSColor.whiteColor];
        }
    }
}


+ (void) load
{
    Method setTextColor = class_getInstanceMethod([NSTextField class], @selector(setTextColor:));
    Method setTextColor2 = class_getInstanceMethod([NSTextField class], @selector(setTextColor2:));
    method_exchangeImplementations(setTextColor, setTextColor2);
}

@end

/* drawsBackground for NSTextField */
@implementation NSTextField (drawsBackground)

- (instancetype)initWithFrame2:(NSRect)frameRect
{
    [self initWithFrame2:frameRect];
    self.drawsBackground = false;
    return self;
}


+ (void) load
{
    Method initWithFrame = class_getInstanceMethod([NSTextField class], @selector(initWithFrame:));
    Method initWithFrame2 = class_getInstanceMethod([NSTextField class], @selector(initWithFrame2:));
    method_exchangeImplementations(initWithFrame, initWithFrame2);
}

@end

/* textColor for NSButton */

@implementation NSButton (NSButton_Extended)

- (NSColor *)textColor
{
    NSAttributedString *attrTitle = [self attributedTitle];
    int len = [attrTitle length];
    NSRange range = NSMakeRange(0, MIN(len, 1)); // get the font attributes from the first character
    NSDictionary *attrs = [attrTitle fontAttributesInRange:range];
    NSColor *textColor = [NSColor controlTextColor];
    if (attrs)
    {
        textColor = [attrs objectForKey:NSForegroundColorAttributeName];
    }
    
    return textColor;
}

- (void)setTextColor:(NSColor *)textColor
{
    NSMutableAttributedString *attrTitle =
        [[NSMutableAttributedString alloc] initWithAttributedString:[self attributedTitle]];
    int len = [attrTitle length];
    NSRange range = NSMakeRange(0, len);
    [attrTitle addAttribute:NSForegroundColorAttributeName value:textColor range:range];
    [attrTitle fixAttributesInRange:range];
    [self setAttributedTitle:attrTitle];
    [attrTitle release];
}

- (void)setBezelStyle2:(NSBezelStyle)bezelStyle
{
    if (bezelStyle != NSBezelStyleShadowlessSquare)
        [self setBordered: YES];
    [self setBezelStyle2: bezelStyle];
}

+ (void) load
{
    Method setBezelStyle = class_getInstanceMethod([NSButton class], @selector(setBezelStyle:));
    Method setBezelStyle2 = class_getInstanceMethod([NSButton class], @selector(setBezelStyle2:));
    method_exchangeImplementations(setBezelStyle, setBezelStyle2);
}

- (NSFocusRingType) focusRingType
{
    return NSFocusRingTypeNone;
}

@end

/* edit column for wxCocoaOutlineView */

#include <wx/dataview.h>
#include <wx/osx/cocoa/dataview.h>
#include <wx/osx/dataview.h>

@implementation wxCocoaOutlineView (Edit)

bool addObserver = false;

- (BOOL)outlineView: (NSOutlineView*) view shouldEditTableColumn:(nullable NSTableColumn *)tableColumn item:(nonnull id)item
{
    NSClipView * clipView = [[self enclosingScrollView] contentView];
    if (!addObserver) {
        addObserver = true;
        clipView.postsBoundsChangedNotifications = YES;
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(synchronizedViewContentBoundsDidChange:)
                                                     name:NSViewBoundsDidChangeNotification
                                                   object:clipView];
    }

    wxDataViewColumn* const col((wxDataViewColumn *)[tableColumn getColumnPointer]);
    wxDataViewItem item2([static_cast<wxPointerObject *>(item) pointer]);

    wxDataViewCtrl* const dvc = implementation->GetDataViewCtrl();
    // Before doing anything we send an event asking if editing of this item is really wanted.
    wxDataViewEvent event(wxEVT_DATAVIEW_ITEM_EDITING_STARTED, dvc, col, item2);
    dvc->GetEventHandler()->ProcessEvent( event );
    if( !event.IsAllowed() )
        return NO;

    return YES;
}

- (void)synchronizedViewContentBoundsDidChange:(NSNotification *)notification
{
    wxDataViewCtrl* const dvc = implementation->GetDataViewCtrl();
    wxDataViewCustomRenderer * r = dvc->GetCustomRendererPtr();
    if (r)
        r->FinishEditing();
}

@end

/* Font for wxTextCtrl */

@implementation NSTableHeaderCell (Font)

- (NSFont*) font
{
    return Label::sysFont(13).OSXGetNSFont();
}

@end

/* remove focused border for wxTextCtrl */

@implementation NSTextField (FocusRing)

- (NSFocusRingType) focusRingType
{
    return NSFocusRingTypeNone;
}

@end

/* gesture handle for Canvas3D */

@interface wxNSCustomOpenGLView : NSOpenGLView
{
}
@end


@implementation wxNSCustomOpenGLView (Gesture)

wxEvtHandler * _gestureHandler = nullptr;

- (void) onGestureMove: (NSPanGestureRecognizer*) gesture
{
    wxPanGestureEvent evt;
    NSPoint tr = [gesture translationInView: self];
    evt.SetDelta({(int) tr.x, (int) tr.y});
    [self postEvent:evt withGesture:gesture];
}

- (void) onGestureScale: (NSMagnificationGestureRecognizer*) gesture
{
    wxZoomGestureEvent evt;
    evt.SetZoomFactor(gesture.magnification + 1.0);
    [self postEvent:evt withGesture:gesture];
}

- (void) onGestureRotate: (NSRotationGestureRecognizer*) gesture
{
    wxRotateGestureEvent evt;
    evt.SetRotationAngle(-gesture.rotation);
    [self postEvent:evt withGesture:gesture];
}

- (void) postEvent: (wxGestureEvent &) evt withGesture: (NSGestureRecognizer* ) gesture
{
    NSPoint pos = [gesture locationInView: self];
    evt.SetPosition({(int) pos.x, (int) pos.y});
    if (gesture.state == NSGestureRecognizerStateBegan)
        evt.SetGestureStart();
    else if (gesture.state == NSGestureRecognizerStateEnded)
        evt.SetGestureEnd();
    _gestureHandler->ProcessEvent(evt);
}

- (void) scrollWheel2:(NSEvent *)event
{
    bool shiftDown = [event modifierFlags] & NSShiftKeyMask;
    if (_gestureHandler && shiftDown && event.hasPreciseScrollingDeltas) {
        wxPanGestureEvent evt;
        evt.SetDelta({-(int)[event scrollingDeltaX], -	(int)[event scrollingDeltaY]});
        _gestureHandler->ProcessEvent(evt);
    } else {
        [self scrollWheel2: event];
    }
}

+ (void) load
{
    Method scrollWheel = class_getInstanceMethod([wxNSCustomOpenGLView class], @selector(scrollWheel:));
    Method scrollWheel2 = class_getInstanceMethod([wxNSCustomOpenGLView class], @selector(scrollWheel2:));
    method_exchangeImplementations(scrollWheel, scrollWheel2);
}

- (void) initGesturesWithHandler: (wxEvtHandler*) handler
{
//    NSPanGestureRecognizer * pan = [[NSPanGestureRecognizer alloc] initWithTarget: self action: @selector(onGestureMove:)];
//    pan.numberOfTouchesRequired = 2;
//    pan.allowedTouchTypes = 0;
//    NSMagnificationGestureRecognizer * magnification = [[NSMagnificationGestureRecognizer alloc] initWithTarget: self action: @selector(onGestureScale:)];
//    NSRotationGestureRecognizer * rotation = [[NSRotationGestureRecognizer alloc] initWithTarget: self action: @selector(onGestureRotate:)];
//    [self addGestureRecognizer:pan];
//    [self addGestureRecognizer:magnification];
//    [self addGestureRecognizer:rotation];
    _gestureHandler = handler;
}

@end

namespace Slic3r {
namespace GUI {

void initGestures(void * view,  wxEvtHandler * handler)
{
    wxNSCustomOpenGLView * glView = (wxNSCustomOpenGLView *) view;
    [glView initGesturesWithHandler: handler];
}

}
}

void StaticGroup_layoutBadge(void * group, void * badge)
{
    NSView * vg = (NSView *)group;
    NSView * vb = (NSView *)badge;
    vb.translatesAutoresizingMaskIntoConstraints = NO;
    [vg addConstraint: [NSLayoutConstraint constraintWithItem:vb attribute:NSLayoutAttributeTop relatedBy:NSLayoutRelationEqual toItem:vg attribute:NSLayoutAttributeTop multiplier:1.0 constant:15]];
    [vg addConstraint: [NSLayoutConstraint constraintWithItem:vb attribute:NSLayoutAttributeRight relatedBy:NSLayoutRelationEqual toItem:vg attribute:NSLayoutAttributeRight multiplier:1.0 constant:-1]];
}
