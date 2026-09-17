#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>
#include <wx/event.h>

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

    // Register a named script message handler on an existing webview, serialized against the
    // factory's own "wx" registration. Deferred (never re-enters), so it is safe to call during
    // window construction. Use this instead of wxWebView::AddScriptMessageHandler for factory-created
    // webviews that need an extra channel (e.g. the Printago tab's "printago").
    static void AddScriptMessageHandler(wxWebView * webView, wxString const & name);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    // Marks "wx" as registered so CreateWebView's deferred add skips the duplicate.
    static void MarkScriptMessageHandlerAdded(wxWebView * webView);

    static void RecreateAll();
};

#endif // !slic3r_GUI_WebView_hpp_
