#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>
#include <wx/event.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

wxDECLARE_EVENT(EVT_WEBVIEW_RECREATED, wxCommandEvent);

enum class WebViewDownloadState {
    Started,
    Downloading,
    Completed,
    Failed,
    Cancelled
};

struct WebViewDownloadRequest {
    std::string id;
    wxString suggested_filename;
    wxString url;
};

struct WebViewDownloadUpdate {
    std::string id;
    wxString destination;
    std::int64_t bytes_received { 0 };
    std::int64_t total_bytes { -1 };
    WebViewDownloadState state { WebViewDownloadState::Started };
    wxString error;
};

struct WebViewDownloadCallbacks {
    std::function<std::optional<wxString>(const WebViewDownloadRequest&)> select_destination;
    std::function<void(const WebViewDownloadRequest&, const WebViewDownloadUpdate&)> on_started;
    std::function<void(const WebViewDownloadUpdate&)> on_updated;
    std::function<void(const wxString&)> on_capability_unavailable;
};

class WebViewDownloadSubscription
{
public:
    virtual ~WebViewDownloadSubscription() = default;
    virtual bool available() const = 0;
    virtual void cancel(const std::string& id) = 0;
};

class WebView
{
public:
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url);
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    // Download handling is opt-in. Existing webviews retain native/default behavior
    // unless their owner keeps the returned subscription alive.
    static std::unique_ptr<WebViewDownloadSubscription>
    EnableDownloads(wxWebView* webView, WebViewDownloadCallbacks callbacks);

    // Marks "wx" as registered so CreateWebView's deferred add skips the duplicate.
    static void MarkScriptMessageHandlerAdded(wxWebView * webView);

    static void RecreateAll();
};

#endif // !slic3r_GUI_WebView_hpp_
