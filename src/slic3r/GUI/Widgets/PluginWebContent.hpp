#pragma once

#include <wx/string.h>

namespace Slic3r { namespace GUI { namespace plugin_web {

// What every plugin web view is built from, shared by the hosts that show plugin HTML
// (PluginWebDialog, PluginWebPanel and its subclasses) without either of them owning it.

// The bundled blank page a plugin web view loads before the plugin HTML is swapped in.
constexpr const char* BOOTSTRAP_PAGE = "web/dialog/PluginWebDialog/blank.html";

// The file:// base URL plugin HTML is loaded against, so relative URLs resolve against the bundled
// web resources.
wxString content_base_url();

// Whether a loaded document is the plugin HTML's own base URL, ignoring any fragment the page
// navigated to. WebKit reports that URL both for the injected page and for the reload that replaces
// it with the directory itself. It reports it for a failed navigation too, against the document that
// stayed, so a match alone does not mean the document changed.
bool is_content_url(const wxString& url);

// The window.orca bridge of plugin windows and docked panels: postMessage, submit, close and
// onMessage. Each host acts only on the message kinds it supports; Pages tabs ship their own bridge.
const char* orca_bridge_script();

}}} // namespace Slic3r::GUI::plugin_web
