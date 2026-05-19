#ifndef slic3r_GUI_DeepLinkHandlerMac_h_
#define slic3r_GUI_DeepLinkHandlerMac_h_

namespace Slic3r {
namespace GUI {

// Re-registers a Cocoa Apple Event handler for kInternetEventClass/kAEGetURL.
// Works around a regression observed after upgrading to wxWidgets 3.3.2 on
// macOS Tahoe (#13119) where wxWidgets' built-in handler is registered but
// never fires for orcaslicer:// deep links.
void register_mac_deep_link_handler();

} // namespace GUI
} // namespace Slic3r

#endif
