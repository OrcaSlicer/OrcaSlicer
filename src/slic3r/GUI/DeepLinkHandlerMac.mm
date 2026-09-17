#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <boost/log/trivial.hpp>
#include <string>

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
    // On a cold launch macOS delivers this event long before the GUI exists, so the
    // URL is buffered here and drained once the app is ready (see take_pending_deep_link).
    if (wxTheApp == nullptr) {
        BOOST_LOG_TRIVIAL(info) << "Deep link buffered, the app object does not exist yet";
        Slic3r::GUI::store_pending_deep_link([url UTF8String]);
        return;
    }
    Slic3r::GUI::wxGetApp().MacOpenURL(wxString::FromUTF8([url UTF8String]));
}
@end

namespace Slic3r {
namespace GUI {

static std::string g_pending_deep_link;

void store_pending_deep_link(const std::string &url) { g_pending_deep_link = url; }

std::string take_pending_deep_link()
{
    std::string url;
    url.swap(g_pending_deep_link);
    return url;
}

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
