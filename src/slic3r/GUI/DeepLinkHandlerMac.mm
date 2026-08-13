#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <boost/log/trivial.hpp>

#include "DeepLinkHandlerMac.h"
#include "GUI_App.hpp"

@interface OrcaDeepLinkHandler : NSObject
- (void)handleGetURLEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation OrcaDeepLinkHandler
- (void)handleGetURLEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    NSString *url = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    if (url == nil || url.length == 0)
        return;
    BOOST_LOG_TRIVIAL(info) << "Deep link received: " << [url UTF8String];
    Slic3r::GUI::wxGetApp().MacOpenURL(wxString::FromUTF8([url UTF8String]));
}
@end

namespace Slic3r {
namespace GUI {

void register_mac_deep_link_handler()
{
    static OrcaDeepLinkHandler *handler = nil;
    if (handler == nil)
        handler = [[OrcaDeepLinkHandler alloc] init];

    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:handler
            andSelector:@selector(handleGetURLEvent:withReplyEvent:)
          forEventClass:kInternetEventClass
             andEventID:kAEGetURL];
}

} // namespace GUI
} // namespace Slic3r
