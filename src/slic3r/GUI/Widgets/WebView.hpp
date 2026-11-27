#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/string.h>
#include <wx/setup.h>
#include <wx/webview.h>
#include <wx/event.h>

#include <functional>
#include <string>

class wxWebView;
class wxWindow;

wxDECLARE_EVENT(EVT_WEBVIEW_RECREATED, wxCommandEvent);

class WebView
{
public:
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url);
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    // Returns the URL truncated at the first '?' or '#', so a query/fragment carrying secrets
    // (e.g. a session-handoff token) is never written to a log.
    static wxString RedactUrl(const wxString& url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    // Runs `javascript`, an expression whose value is a string, in the page's main frame and hands
    // that value to `done` on the UI thread: done(true, value), or done(false, why) when it could
    // not run. Unlike wxWebView::RunScriptAsync(), whose completion keeps a raw pointer to the web
    // view, the native completion holds `done` alone, so the web view may be destroyed while the
    // script is pending; `done` then reports an error, or on WebView2 is never called. The script is
    // run as given, without wx's eval() wrapper, so it also runs in pages whose CSP forbids eval.
    static void RunScriptAsync(wxWebView* webView, const wxString& javascript, std::function<void(bool ok, const std::string& result)> done);

    // Marks "wx" as registered so CreateWebView's deferred add skips the duplicate.
    static void MarkScriptMessageHandlerAdded(wxWebView * webView);

    // On Windows, a WebView2 backend created during a GUI rebuild (language switch) can come up
    // ignoring every navigation. A panel that gets true here recreates its view on first Show().
    static bool NeedsRecreateOnShow();

    // True when the linked WebKitGTK predates 2.42, where accelerated compositing
    // could fail silently on XWayland sessions, so it is forced off as a fallback.
    // False on every other platform. Safe to call before GTK initialises.
    static bool NeedsCompositingWorkaround();

    // True when the linked WebKitGTK is new enough (>= 2.44) to honour
    // WEBKIT_DMABUF_RENDERER_FORCE_SHM, the shared-memory transport switch.
    // False on every other platform. Safe to call before GTK initialises.
    static bool SupportsForceSharedMemory();

    static void RecreateAll();
};

#endif // !slic3r_GUI_WebView_hpp_
