#include "WebView.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"

#include <libslic3r/Utils.hpp>

#include <algorithm>
#include <boost/log/trivial.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
#include <thread>

#include <wx/chartype.h>
#include <wx/setup.h>
#include <wx/webview.h>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/sharedptr.h>
#include <wx/vector.h>
#include <wx/event.h>
#include <vector>
#include <wx/object.h>
#include <wx/log.h>
#include <utility>
#include <wx/webviewarchivehandler.h>
#include <wx/webviewfshandler.h>
#include <wx/weakref.h>
#if wxUSE_WEBVIEW_EDGE
#include <wx/msw/webview_edge.h>
#elif defined(__WXMAC__)
#include <wx/osx/webview_webkit.h>
#endif
#include <wx/uri.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <ios>
#include <string>
#include <wx/versioninfo.h>

namespace fs = boost::filesystem;
#if defined(__WIN32__) || defined(__WXMAC__)
#include "wx/private/jsscriptwrapper.h"
#endif

#ifdef __WIN32__
#include <WebView2.h>
#include <Shellapi.h>
#include <slic3r/Utils/Http.hpp>
#elif defined __linux__
#include <gtk/gtk.h>
#define WEBKIT_API
struct WebKitWebView;
struct WebKitJavascriptResult;
struct WebKitSettings;
extern "C" {
WEBKIT_API WebKitSettings *
webkit_settings_new                                  (void);
WEBKIT_API const gchar *
webkit_settings_get_user_agent                       (WebKitSettings            *settings);
WEBKIT_API void
webkit_web_view_run_javascript                       (WebKitWebView             *web_view,
                                                      const gchar               *script,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback       callback,
                                                      gpointer                  user_data);
WEBKIT_API WebKitJavascriptResult *
webkit_web_view_run_javascript_finish                (WebKitWebView             *web_view,
                                                      GAsyncResult              *result,
						      GError                    **error);
WEBKIT_API void
webkit_javascript_result_unref              (WebKitJavascriptResult *js_result);
WEBKIT_API guint
webkit_get_major_version                     (void);
WEBKIT_API guint
webkit_get_minor_version                     (void);
}
#endif

// Private storage root for an isolated (plugin) profile. Plugin pages must not reach the shared
// default store's cookies/session (e.g. the OrcaCloud login), so each profile gets its own dir,
// shared by every view of that profile. macOS isolates with an in-memory store instead (see
// CreateWebView), so this is unused there.
#if !defined(__WXOSX__)
static wxString isolated_profile_path(const wxString& profile)
{
    wxString safe;
    safe.reserve(profile.size());
    for (wxString::const_iterator it = profile.begin(); it != profile.end(); ++it) {
        const wxChar c = *it;
        const bool keep = (c >= wxT('a') && c <= wxT('z')) || (c >= wxT('A') && c <= wxT('Z')) ||
                          (c >= wxT('0') && c <= wxT('9')) || c == wxT('.') || c == wxT('-') || c == wxT('_');
        safe += keep ? c : wxT('_');
    }
    if (safe.empty())
        safe = wxT("default");
    return wxString::FromUTF8((boost::filesystem::path(Slic3r::data_dir()) / "web_profiles" / safe.ToStdString()).string());
}

// A profile's backend state - the cookie jar, storage and the live session - belongs to the object
// wx builds from a wxWebViewConfiguration: the WebView2 environment on Windows, the
// WebKitWebContext on Linux. One configuration per profile is therefore what lets two views of the
// same plugin see each other's login; a configuration per view would share only the directory on
// disk, not the session in memory.
static const wxWebViewConfiguration& profile_configuration(const wxString& profile)
{
    static std::map<wxString, wxWebViewConfiguration> configs;

    const auto existing = configs.find(profile);
    if (existing != configs.end())
        return existing->second;

#if defined(__WIN32__)
    wxWebViewConfiguration config = wxWebView::NewConfiguration(wxWebViewBackendEdge);
#else
    wxWebViewConfiguration config = wxWebView::NewConfiguration(wxWebViewBackendWebKit);
#endif
    // Has to precede the first view, i.e. the environment/context built from the configuration.
    config.SetDataPath(isolated_profile_path(profile));
    return configs.emplace(profile, config).first->second;
}
#endif // !__WXOSX__

#ifdef __WIN32__
// Run Download and Install in another thread so we don't block the UI thread
DWORD DownloadAndInstallWV2RT() {

  int returnCode = 2; // Download failed
  // Use fwlink to download WebView2 Bootstrapper at runtime and invoke installation
  // Broken/Invalid Https Certificate will fail to download
  // Use of the download link below is governed by the below terms. You may acquire the link
  // for your use at https://developer.microsoft.com/microsoft-edge/webview2/. Microsoft owns
  // all legal right, title, and interest in and to the WebView2 Runtime Bootstrapper
  // ("Software") and related documentation, including any intellectual property in the
  // Software. You must acquire all code, including any code obtained from a Microsoft URL,
  // under a separate license directly from Microsoft, including a Microsoft download site
  // (e.g., https://developer.microsoft.com/microsoft-edge/webview2/).
  // HRESULT hr = URLDownloadToFileW(NULL, L"https://go.microsoft.com/fwlink/p/?LinkId=2124703",
  //                               L".\\plugin\\MicrosoftEdgeWebview2Setup.exe", 0, 0);
  fs::path target_file_path = (fs::temp_directory_path() / "MicrosoftEdgeWebview2Setup.exe");
  bool downloaded = false;
  Slic3r::Http::get("https://go.microsoft.com/fwlink/p/?LinkId=2124703")
      .on_error([](std::string body, std::string error, unsigned http_status) {

      })
      .on_complete([&downloaded, target_file_path](std::string body, unsigned http_status) {
        fs::fstream file(target_file_path, std::ios::out | std::ios::binary | std::ios::trunc);
        file.write(body.c_str(), body.size());
        file.flush();
        file.close();

        downloaded = true;
      })
      .perform_sync();
  // Sleep for 1 second to wait for the buffer writen into disk
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  if (downloaded) {
    // Either Package the WebView2 Bootstrapper with your app or download it using fwlink
    // Then invoke install at Runtime.
    // Keep the path string alive for the duration of the ShellExecuteExW call;
    // assigning .c_str() of a temporary directly would leave lpFile dangling.
    const std::wstring installer_path = target_file_path.generic_wstring();
    SHELLEXECUTEINFOW shExInfo = {0};
    shExInfo.cbSize = sizeof(shExInfo);
    shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExInfo.hwnd = 0;
    shExInfo.lpVerb = L"runas";
    shExInfo.lpFile = installer_path.c_str();
    shExInfo.lpParameters = L" /silent /install";
    shExInfo.lpDirectory = 0;
    shExInfo.nShow = 0;
    shExInfo.hInstApp = 0;

    if (ShellExecuteExW(&shExInfo)) {
      WaitForSingleObject(shExInfo.hProcess, INFINITE);
      returnCode = 0; // Install successfull
    } else {
      returnCode = 1; // Install failed
    }
  }
  return returnCode;
}

class WebViewEdge : public wxWebViewEdge
{
public:
    WebViewEdge() = default;
    explicit WebViewEdge(const wxWebViewConfiguration& config) : wxWebViewEdge(config) {}

    bool SetUserAgent(const wxString &userAgent) override
    {
        bool dark = userAgent.Contains("dark");
        SetColorScheme(dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);

        ICoreWebView2 *webView2 = (ICoreWebView2 *) GetNativeBackend();
        if (webView2) {
            ICoreWebView2Settings *settings;
            HRESULT                hr = webView2->get_Settings(&settings);
            if (hr == S_OK) {
                ICoreWebView2Settings2 *settings2;
                hr = settings->QueryInterface(&settings2);
                if (hr == S_OK) {
                    settings2->put_UserAgent(userAgent.wc_str());
                    settings2->Release();
                    return true;
                }
            }
            settings->Release();
            return false;
        }
        pendingUserAgent = userAgent;
        return true;
    }

    bool SetColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME colorScheme)
    {
        ICoreWebView2 *webView2 = (ICoreWebView2 *) GetNativeBackend();
        if (webView2) {
            ICoreWebView2_13 * webView2_13;
            HRESULT           hr = webView2->QueryInterface(&webView2_13);
            if (hr == S_OK) {
                ICoreWebView2Profile *profile;
                hr = webView2_13->get_Profile(&profile);
                if (hr == S_OK) {
                    profile->put_PreferredColorScheme(colorScheme);
                    profile->Release();
                    return true;
                }
                webView2_13->Release();
            }
            return false;
        }
        pendingColorScheme = colorScheme;
        return true;
    }

    void DoGetClientSize(int *x, int *y) const override
    {
        if (!pendingUserAgent.empty()) {
            auto thiz = const_cast<WebViewEdge *>(this);
            auto userAgent = std::move(thiz->pendingUserAgent);
            thiz->pendingUserAgent.clear();
            thiz->SetUserAgent(userAgent);
        }
        if (pendingColorScheme) {
            auto thiz      = const_cast<WebViewEdge *>(this);
            auto colorScheme = pendingColorScheme;
            thiz->pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
            thiz->SetColorScheme(colorScheme);
        }
        wxWebViewEdge::DoGetClientSize(x, y);
    };
private:
    wxString pendingUserAgent;
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
};

#elif defined __WXOSX__

class WebViewWebKit : public wxWebViewWebKit
{
public:
    WebViewWebKit()
        : wxWebViewWebKit(wxWebView::NewConfiguration(wxWebViewBackendWebKit))
    {
    }

    explicit WebViewWebKit(const wxWebViewConfiguration& config)
        : wxWebViewWebKit(config)
    {
    }

    ~WebViewWebKit() override
    {
        RemoveScriptMessageHandler("wx");
    }
};

#endif

class FakeWebView : public wxWebView
{
    virtual bool Create(wxWindow* parent, wxWindowID id, const wxString& url, const wxPoint& pos, const wxSize& size, long style, const wxString& name) override { return false; }
    virtual wxString GetCurrentTitle() const override { return wxString(); }
    virtual wxString GetCurrentURL() const override { return wxString(); }
    virtual bool IsBusy() const override { return false; }
    virtual bool IsEditable() const override { return false; }
    virtual void LoadURL(const wxString& url) override { }
    virtual void Print() override { }
    virtual void RegisterHandler(wxSharedPtr<wxWebViewHandler> handler) override { }
    virtual void Reload(wxWebViewReloadFlags flags = wxWEBVIEW_RELOAD_DEFAULT) override { }
    virtual bool RunScript(const wxString& javascript, wxString* output = NULL) const override { return false; }
    virtual void SetEditable(bool enable = true) override { }
    virtual void Stop() override { }
    virtual bool CanGoBack() const override { return false; }
    virtual bool CanGoForward() const override { return false; }
    virtual void GoBack() override { }
    virtual void GoForward() override { }
    virtual void ClearHistory() override { }
    virtual void EnableHistory(bool enable = true) override { }
    virtual wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetBackwardHistory() override { return {}; }
    virtual wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetForwardHistory() override { return {}; }
    virtual void LoadHistoryItem(wxSharedPtr<wxWebViewHistoryItem> item) override { }
    virtual bool CanSetZoomType(wxWebViewZoomType type) const override { return false; }
    virtual float GetZoomFactor() const override { return 0.0f; }
    virtual wxWebViewZoomType GetZoomType() const override { return wxWebViewZoomType(); }
    virtual void SetZoomFactor(float zoom) override { }
    virtual void SetZoomType(wxWebViewZoomType zoomType) override { }
    virtual bool CanUndo() const override { return false; }
    virtual bool CanRedo() const override { return false; }
    virtual void Undo() override { }
    virtual void Redo() override { }
    virtual void* GetNativeBackend() const override { return nullptr; }
    virtual void DoSetPage(const wxString& html, const wxString& baseUrl) override { }
};

wxDEFINE_EVENT(EVT_WEBVIEW_RECREATED, wxCommandEvent);

static std::vector<wxWebView*> g_webviews;
// Webviews waiting for their script handler while another one is added; adding it yields, so a
// view can be destroyed while it waits.
static std::vector<wxWeakRef<wxWebView>> g_delay_webviews;

class WebViewRef : public wxObjectRefData
{
public:
    WebViewRef(wxWebView *webView) : m_webView(webView) {}
    ~WebViewRef() {
        auto iter = std::find(g_webviews.begin(), g_webviews.end(), m_webView);
        assert(iter != g_webviews.end());
        if (iter != g_webviews.end())
            g_webviews.erase(iter);
    }
    wxWebView *m_webView;
    // Guards against registering the "wx" handler twice (a duplicate throws on WKWebView).
    bool m_script_handler_added = false;
};

static WebViewRef *webview_ref(wxWebView *webView)
{
    return webView ? static_cast<WebViewRef *>(webView->GetRefData()) : nullptr;
}

#ifdef __linux__
// WebKitGTK's own User-Agent with the tags the bundled pages (globalapi.js) and Bambu's cloud read
// appended. wx's GTK backend cannot report the stock string, so it comes from a throwaway settings object.
static wxString linux_user_agent()
{
    static const wxString stock = [] {
        WebKitSettings *settings = webkit_settings_new();
        wxString        ua       = wxString::FromUTF8(webkit_settings_get_user_agent(settings));
        g_object_unref(settings);
        return ua;
    }();
    return wxString::Format("%s BBL-Slicer/v%s (%s) BBL-Language/%s", stock, Slic3r::GUI::wxGetApp().get_bbl_client_version(),
                            Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light",
                            Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_'));
}
#endif

wxWebView* WebView::CreateWebView(wxWindow * parent, wxString const & url, wxString const & profile)
{
#if wxUSE_WEBVIEW_EDGE
    // Check if a fixed version of edge is present in
    // $executable_path/edge_fixed and use it
    wxFileName edgeFixedDir(wxStandardPaths::Get().GetExecutablePath());
    edgeFixedDir.SetFullName("");
    edgeFixedDir.AppendDir("edge_fixed");
    if (edgeFixedDir.DirExists()) {
        wxWebViewEdge::MSWSetBrowserExecutableDir(edgeFixedDir.GetFullPath());
        wxLogMessage("Using fixed edge version");
    }
#endif
    auto url2  = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << url2.ToUTF8();

    const bool isolated = !profile.IsEmpty();

#ifdef __WIN32__
    wxWebView* webView = nullptr;
    if (isolated) {
        // Reusing the configuration reuses its WebView2 environment, so every view of a profile
        // belongs to the same browser profile and a login in one view is visible in another.
        webView = new WebViewEdge(profile_configuration(profile));
    } else {
        webView = new WebViewEdge;
    }
#elif defined(__WXOSX__)
    wxWebView* webView = nullptr;
    if (isolated) {
        // wx has no SetDataPath on macOS, and WKWebView copies the configuration for each view
        // (wx's "wx" script message handler lives on that per-view copy), so the views keep one
        // configuration each and share a session through the profile's in-memory data store.
        wxWebViewConfiguration config = wxWebView::NewConfiguration(wxWebViewBackendWebKit);
        Slic3r::GUI::WKWebViewConfiguration_setSharedNonPersistentDataStore(config.GetNativeConfiguration(), profile);
        webView = new WebViewWebKit(config);
    } else {
        webView = new WebViewWebKit;
    }
#else
    wxWebView* webView = nullptr;
    if (isolated) {
        // Reusing the configuration reuses its WebKitWebContext, so every view of a profile shares
        // the same cookies, storage and live session.
        webView = wxWebView::New(profile_configuration(profile));
    } else {
        webView = wxWebView::New();
    }
#endif
    if (webView) {
        webView->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

        wxString language_code = Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_');
        language_code          = language_code.ToStdString();
#ifdef __WIN32__
        webView->SetUserAgent(wxString::Format("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                               "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52 BBL-Slicer/v%s (%s) BBL-Language/%s",
                                               Slic3r::GUI::wxGetApp().get_bbl_client_version(), Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light", language_code.mb_str()));
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        // We register the wxfs:// protocol for testing purposes
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("bbl")));
        // And the memory: file system
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
#else
        // With WKWebView handlers need to be registered before creation.
#ifdef __linux__
        // WebKitGTK registers a URI scheme on a WebKitWebContext, once per context, and wx answers
        // a request through the view whose RegisterHandler() registered it. A view of the shared
        // store has a context of its own, so it registers here. The views of a profile share one
        // context, but registering from one of them would make every other view depend on that
        // view's lifetime and would give untrusted plugin pages the local-file access of "wxfs";
        // plugin pages load their own document only, so their context registers no scheme.
        if (!isolated) {
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("wxfs")));
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
        }
#else
        static bool s_schemes_registered = false;
        if (!s_schemes_registered) {
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("wxfs")));
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
            s_schemes_registered = true;
        }
#endif
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
#ifdef __linux__
        webView->SetUserAgent(linux_user_agent());
#else
        webView->SetUserAgent(wxString::Format("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) BBL-Slicer/v%s (%s) BBL-Language/%s",
                                               Slic3r::GUI::wxGetApp().get_bbl_client_version(), Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light", language_code.mb_str()));
#endif
#endif
#ifdef __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_setTransparentBackground(wkWebView);
#endif
        auto addScriptMessageHandler = [] (wxWebView *webView) {
            // Skip if SendAPIKey() already registered "wx"; a duplicate add throws an
            // uncatchable NSException on WKWebView, killing the app at startup.
            WebViewRef *ref = webview_ref(webView);
            if (ref && ref->m_script_handler_added)
                return;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": begin to add script message handler for wx.";
            Slic3r::GUI::wxGetApp().set_adding_script_handler(true);
            if (!webView->AddScriptMessageHandler("wx"))
                wxLogError("Could not add script message handler");
            else if (ref)
                ref->m_script_handler_added = true;
            Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished add script message handler for wx.";
        };
#ifndef __WIN32__
        webView->CallAfter([webView, addScriptMessageHandler] {
#endif
            if (Slic3r::GUI::wxGetApp().is_adding_script_handler()) {
                g_delay_webviews.push_back(webView);
            } else {
                addScriptMessageHandler(webView);
                while (!g_delay_webviews.empty()) {
                    auto views = std::move(g_delay_webviews);
                    for (const wxWeakRef<wxWebView>& wv : views)
                        if (wv)
                            addScriptMessageHandler(wv.get());
                }
            }
#ifndef __WIN32__
        });
#endif
        webView->EnableContextMenu(true);
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": failed. Use fake web view.";
        webView = new FakeWebView;
    }
    webView->SetRefData(new WebViewRef(webView));
    g_webviews.push_back(webView);
    return webView;
}

void WebView::MarkScriptMessageHandlerAdded(wxWebView * webView)
{
    if (WebViewRef *ref = webview_ref(webView))
        ref->m_script_handler_added = true;
}

bool WebView::NeedsRecreateOnShow()
{
    const bool recreating = Slic3r::GUI::wxGetApp().is_recreating_gui();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": is_recreating_gui = " << recreating;
#ifdef __WIN32__
    return recreating;
#else
    return false;
#endif
}

bool WebView::NeedsCompositingWorkaround()
{
#ifdef __linux__
    const guint major = webkit_get_major_version();
    const guint minor = webkit_get_minor_version();
    return major < 2 || (major == 2 && minor < 42);
#else
    return false;
#endif
}

bool WebView::SupportsForceSharedMemory()
{
#ifdef __linux__
    const guint major = webkit_get_major_version();
    const guint minor = webkit_get_minor_version();
    return major > 2 || (major == 2 && minor >= 44);
#else
    return false;
#endif
}

#if wxUSE_WEBVIEW_EDGE
bool WebView::CheckWebViewRuntime()
{
    wxWebViewFactoryEdge factory;
    auto wxVersion = factory.GetVersionInfo(wxVersionContext::RunTime);
    bool present = wxVersion.GetMajor() != 0;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": WebView2 runtime "
                            << (present ? "found, version " : "not found (")
                            << wxVersion.ToString().ToUTF8().data() << (present ? "" : ")");
    return present;
}

bool WebView::DownloadAndInstallWebViewRuntime()
{
    return DownloadAndInstallWV2RT() == 0;
}
#endif
wxString WebView::RedactUrl(const wxString& url)
{
    const size_t cut = url.find_first_of(wxString("?#"));
    return cut == wxString::npos ? url : url.Left(cut);
}

void WebView::LoadUrl(wxWebView * webView, wxString const &url)
{
    auto url2  = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << RedactUrl(url2).ToUTF8();
    webView->LoadURL(url2);
}

bool WebView::RunScript(wxWebView *webView, wxString const &javascript)
{
    if (Slic3r::GUI::wxGetApp().app_config->get("internal_developer_mode") == "true"
            && javascript.find("studio_userlogin") == wxString::npos)
        wxLogMessage("Running JavaScript:\n%s\n", javascript);

    try {
#ifdef __WIN32__
        ICoreWebView2 *   webView2 = (ICoreWebView2 *) webView->GetNativeBackend();
        if (webView2 == nullptr)
            return false;
        return webView2->ExecuteScript(javascript, NULL) == 0;
#elif defined __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_evaluateJavaScript(wkWebView, javascript, nullptr);
        return true;
#else
        WebKitWebView *wkWebView = (WebKitWebView *) webView->GetNativeBackend();
        webkit_web_view_run_javascript(
            wkWebView, javascript.utf8_str(), NULL,
            [](GObject *wkWebView, GAsyncResult *res, void *) {
                GError * error = NULL;
                auto result = webkit_web_view_run_javascript_finish((WebKitWebView*)wkWebView, res, &error);
                if (!result)
                    g_error_free (error);
                else
                    webkit_javascript_result_unref (result);
        }, NULL);
        return true;
#endif
    } catch (std::exception &) {
        return false;
    }
}

void WebView::RecreateAll()
{
    auto dark = Slic3r::GUI::wxGetApp().dark_mode();
    wxString language_code = Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_');
    language_code          = language_code.ToStdString();
    for (auto webView : g_webviews) {
#ifdef __linux__
        webView->SetUserAgent(linux_user_agent());
#else
        webView->SetUserAgent(wxString::Format("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) BBL-Slicer/v%s (%s) BBL-Language/%s",
                                               Slic3r::GUI::wxGetApp().get_bbl_client_version(), dark ? "dark" : "light", language_code.mb_str()));
#endif
        // A host-themed WebViewHostDialog re-themes in place (no reload). If it handles
        // the event, skip the reload; legacy pages fall through and reload as before
        // (their own dark.css swap re-themes them on reload).
        wxCommandEvent evt(EVT_WEBVIEW_RECREATED);
        evt.SetEventObject(webView);
        if (!webView->GetEventHandler()->ProcessEvent(evt))
            webView->Reload();
    }
}
