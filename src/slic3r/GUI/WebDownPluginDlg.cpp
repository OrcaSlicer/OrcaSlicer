#include "WebDownPluginDlg.hpp"
#include "ConfigWizard.hpp"

#include <string.h>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"

#include <wx/sizer.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>

#include "MainFrame.hpp"
#include <boost/dll.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/Utils/Http.hpp>
#include <libslic3r/miniz_extension.hpp>
#include <libslic3r/Utils.hpp>

using namespace nlohmann;

namespace Slic3r { namespace GUI {

DownPluginFrame::DownPluginFrame(GUI_App *pGUI) : wxDialog((wxWindow *) (pGUI->mainframe), wxID_ANY, "Orca Slicer"), m_appconfig_new()
{
    // INI
    m_MainPtr = pGUI;

    // set the frame icon
    wxBoxSizer *topsizer = new wxBoxSizer(wxVERTICAL);
    wxString TargetUrl    = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/6/index.html").make_preferred().string());

    TargetUrl = "file://" + TargetUrl;

    // Create the webview
    m_browser = WebView::CreateWebView(this, TargetUrl);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    SetSizer(topsizer);
    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    // wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
    // wxLogMessage("Backend: %s Version: %s",
    // m_browser->GetClassInfo()->GetClassName(),wxWebView::GetBackendVersionInfo().ToString());
    // wxLogMessage("User Agent: %s", m_browser->GetUserAgent());

    // Set a more sensible size for web browsing
    wxSize pSize = FromDIP(wxSize(410, 200));
    SetSize(pSize);

    CenterOnParent();
    // int screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
    // int screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
    // int MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
    // MoveWindow(this->m_hWnd, (screenwidth - pSize.x) / 2, MaxY, pSize.x, pSize.y, TRUE);

    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &DownPluginFrame::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &DownPluginFrame::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &DownPluginFrame::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &DownPluginFrame::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &DownPluginFrame::OnNewWindow, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &DownPluginFrame::OnTitleChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_FULLSCREEN_CHANGED, &DownPluginFrame::OnFullScreenChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &DownPluginFrame::OnScriptMessage, this, m_browser->GetId());

}

DownPluginFrame::~DownPluginFrame()
{
    if (m_browser) {
        delete m_browser;
        m_browser = nullptr;
    }
}

void DownPluginFrame::load_url(wxString &url)
{
    BOOST_LOG_TRIVIAL(trace) << "app_start: DownPluginFrame url=" << url.ToStdString();
    this->Show();
    m_browser->LoadURL(url);
    m_browser->SetFocus();
    UpdateState();
}

/**
 * Method that retrieves the current state from the web control and updates
 * the GUI the reflect this current state.
 */
void DownPluginFrame::UpdateState()
{
    // SetTitle(m_browser->GetCurrentTitle());
}

void DownPluginFrame::OnIdle(wxIdleEvent &WXUNUSED(evt))
{
    if (m_browser->IsBusy()) {
        wxSetCursor(wxCURSOR_ARROWWAIT);
    } else {
        wxSetCursor(wxNullCursor);
    }
}

// void DownPluginFrame::OnClose(wxCloseEvent& evt)
//{
//    this->Hide();
//}

/**
 * Callback invoked when there is a request to load a new page (for instance
 * when the user clicks a link)
 */
void DownPluginFrame::OnNavigationRequest(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation request to '" + evt.GetURL() + "'
    // (target='" + evt.GetTarget() + "')");

    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
void DownPluginFrame::OnNavigationComplete(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");

    wxString NewUrl = evt.GetURL();

    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void DownPluginFrame::OnDocumentLoaded(wxWebViewEvent &evt)
{
    // Only notify if the document is the main frame, not a subframe
    wxString tmpUrl = evt.GetURL();
    wxString NowUrl = m_browser->GetCurrentURL();

    if (evt.GetURL() == m_browser->GetCurrentURL()) {
        // wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }
    UpdateState();

    // wxCommandEvent *event = new
    // wxCommandEvent(EVT_WEB_RESPONSE_MESSAGE,this->GetId()); wxQueueEvent(this,
    // event);
}

/**
 * On new window, we veto to stop extra windows appearing
 */
void DownPluginFrame::OnNewWindow(wxWebViewEvent &evt)
{
    wxString flag = " (other)";

    wxString NewUrl = evt.GetURL();
    wxLaunchDefaultBrowser(NewUrl);
    // if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }
    // wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    // if (m_tools_handle_new_window->IsChecked())
    //    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void DownPluginFrame::OnTitleChanged(wxWebViewEvent &evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void DownPluginFrame::OnFullScreenChanged(wxWebViewEvent &evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

void DownPluginFrame::OnScriptMessage(wxWebViewEvent &evt)
{
    try {
        wxString strInput = evt.GetString();
        json     j        = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];

        if (strCmd == "Begin_Download_network_plugin") {
            wxGetApp().CallAfter([this] { DownloadPlugin(); });
        }
        else if (strCmd == "netplugin_download_cancel") {
            wxGetApp().cancel_networking_install();
            this->EndModal(wxID_CANCEL);
            this->Close();
        }
        else if (strCmd == "begin_install_plugin") {
            wxGetApp().CallAfter([this] { InstallPlugin(); });
        }
        else if (strCmd == "restart_studio") {
            wxGetApp().restart_networking();
            this->EndModal(wxID_OK);
            this->Close();
        }
        else if (strCmd == "close_download_dialog") {
            this->EndModal(wxID_OK);
            this->Close();
        }
        else if (strCmd == "open_plugin_folder") {
            auto plugin_folder = (boost::filesystem::path(wxStandardPaths::Get().GetUserDataDir().ToUTF8().data()) / "plugins").make_preferred().string();
            desktop_open_any_folder(plugin_folder);
        }
    } catch (std::exception &) {
        // wxMessageBox(e.what(), "json Exception", MB_OK);
    }
}

void DownPluginFrame::RunScript(const wxString &javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    // m_javascript = javascript;

    // wxLogMessage("Running JavaScript:\n%s\n", javascript);

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}

#if wxUSE_WEBVIEW_IE
void DownPluginFrame::OnRunScriptObjectWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void DownPluginFrame::OnRunScriptDateWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void DownPluginFrame::OnRunScriptArrayWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void DownPluginFrame::OnError(wxWebViewEvent &evt)
{
#define WX_ERROR_CASE(type) \
    case type: category = #type; break;

    wxString category;
    switch (evt.GetInt()) {
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CONNECTION);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CERTIFICATE);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_AUTH);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_SECURITY);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_NOT_FOUND);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_REQUEST);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_USER_CANCELLED);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_OTHER);
    }

    // wxLogMessage("%s", "Error; url='" + evt.GetURL() + "', error='" +
    // category + " (" + evt.GetString() + ")'");

    // Show the info bar with an error
    // m_info->ShowMessage(_L("An error occurred loading ") + evt.GetURL() +
    // "\n" + "'" + category + "'", wxICON_ERROR);
    BOOST_LOG_TRIVIAL(info) << "DownPluginFrame::OnError: An error occurred loading " << evt.GetURL() << category;

    UpdateState();
}

void DownPluginFrame::OnScriptResponseMessage(wxCommandEvent &WXUNUSED(evt))
{

}

int DownPluginFrame::DownloadPlugin()
{
    return wxGetApp().download_plugin(
        "plugins", "network_plugin.zip", [this](int status, int percent, bool &cancel) { return ShowPluginStatus(status, percent, cancel); }, nullptr);
}

int DownPluginFrame::InstallPlugin()
{
    return wxGetApp().install_plugin(
        "plugins", "network_plugin.zip", [this](int status, int percent, bool &cancel) { return ShowPluginStatus(status, percent, cancel); });
}

int DownPluginFrame::ShowPluginStatus(int status, int percent, bool &cancel)
{
    static int nPercent = 0;
    if (nPercent == percent)
        return 0;

    nPercent = percent;

    json m_Data = json::object();
    m_Data["status"] = status;
    m_Data["percent"] = percent;

    json m_Res           = json::object();
    m_Res["command"]     = "ShowStatusPercent";
    m_Res["sequence_id"] = "10001";
    m_Res["data"]    = m_Data;

    wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', false, json::error_handler_t::ignore));

    wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });

    return 0;
}

}} // namespace Slic3r::GUI
