#pragma once

#include <wx/string.h>

namespace Slic3r { namespace GUI { namespace web_hosting {

// Shared by the hosts that show plugin HTML: WebDialog and WebPanel.

// The bundled blank page a plugin web view loads before the plugin HTML is swapped in.
constexpr const char* BOOTSTRAP_PAGE = "web/dialog/WebDialog/blank.html";

// The file:// URL of BOOTSTRAP_PAGE.
wxString bootstrap_url();

// The file:// base URL plugin HTML is loaded against, so relative URLs resolve to bundled resources.
wxString content_base_url();

// Whether `url` is the plugin HTML's base URL, ignoring any fragment. WebKit reports it for the
// injected page, a reload and a failed navigation alike, so a match alone is not a new document.
bool is_content_url(const wxString& url);

// The window.orca bridge of plugin windows and docked panels. Pages tabs ship their own.
const char* orca_bridge_script();

}}} // namespace Slic3r::GUI::web_hosting
