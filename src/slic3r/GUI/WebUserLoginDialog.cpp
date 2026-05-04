#include "WebUserLoginDialog.hpp"

#include <string.h>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"
#include "slic3r/Utils/Http.hpp"

#include <wx/sizer.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#ifdef WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <boost/cast.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/lexical_cast.hpp>

#include <nlohmann/json.hpp>
#include "MainFrame.hpp"
#include <boost/dll.hpp>

#include <sstream>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/GUI/Widgets/HyperLink.hpp> // ORCA
using namespace std;

using namespace nlohmann;

namespace Slic3r { namespace GUI {

namespace {
#ifdef WIN32
static bool pjarczak_open_external_browser(const wxString& url)
{
    HINSTANCE rc = ::ShellExecuteW(nullptr, L"open", url.wc_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc > 32)
        return true;

    if (wxLaunchDefaultBrowser(url, wxBROWSER_NEW_WINDOW))
        return true;

    const wxString cmd = wxString::Format("explorer.exe \"%s\"", url);
    const long code = wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
    return code != 0;
}
#else
static bool pjarczak_open_external_browser(const wxString& url)
{
    return wxLaunchDefaultBrowser(url, wxBROWSER_NEW_WINDOW);
}
#endif

std::string pjarczak_browser_login_url(const std::string& host_value, const std::string& locale, const std::string& localhost_base)
{
    std::string host = host_value;
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    if (host.rfind("http://", 0) != 0 && host.rfind("https://", 0) != 0)
        host = "https://bambulab.com";
    std::string lang = locale.empty() ? "en" : locale;
    std::string callback = host + "/sign-in/callback?source=portal&locale=" + Http::url_encode(lang) +
                           "&redirect_url=" + Http::url_encode(localhost_base) +
                           "&openBy=suite&from=studio&slicerLoginType=ticket";
    return host + "/sign-in?&from=studio&source=portal&to=" + Http::url_encode(callback);
}
}


#define NETWORK_OFFLINE_TIMER_ID 10001

BEGIN_EVENT_TABLE(ZUserLogin, wxDialog)
EVT_TIMER(NETWORK_OFFLINE_TIMER_ID, ZUserLogin::OnTimer)
END_EVENT_TABLE()

int ZUserLogin::web_sequence_id = 20000;

namespace {

int reserve_loopback_port()
{
    try {
        boost::asio::io_service       io_service;
        boost::asio::ip::tcp::acceptor acceptor(io_service, {boost::asio::ip::tcp::v4(), 0});
        return static_cast<int>(acceptor.local_endpoint().port());
    } catch (...) {
        return 0;
    }
}

std::string rewrite_loopback_url(std::string url, int port)
{
    if (port <= 0) {
        return url;
    }

    const std::string old_port = std::to_string(LOCALHOST_PORT);
    const std::string new_port = std::to_string(port);

    boost::replace_all(url, std::string(LOCALHOST_URL) + old_port, std::string(LOCALHOST_URL) + new_port);
    boost::replace_all(url, "http://127.0.0.1:" + old_port, "http://127.0.0.1:" + new_port);
    boost::replace_all(url, "http%3A%2F%2Flocalhost%3A" + old_port, "http%3A%2F%2Flocalhost%3A" + new_port);
    boost::replace_all(url, "http%3A%2F%2F127.0.0.1%3A" + old_port, "http%3A%2F%2F127.0.0.1%3A" + new_port);

    return url;
}

}

int ZUserLogin::ensure_loopback_port()
{
    if (m_loopback_port <= 0) {
        m_loopback_port = reserve_loopback_port();
    }
    int port = m_loopback_port > 0 ? m_loopback_port : LOCALHOST_PORT;
    wxGetApp().start_http_server(port, m_cloud_agent->get_id());
    return port;
}

ZUserLogin::ZUserLogin(std::shared_ptr<ICloudServiceAgent> cloud_agent)
    : wxDialog((wxWindow*) (wxGetApp().mainframe), wxID_ANY, "OrcaSlicer"), m_cloud_agent(cloud_agent)
{
    SetBackgroundColour(*wxWHITE);
    const auto bblnetwork_enabled =wxGetApp().app_config->get_bool("installed_networking");
    // Url
    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent) {

        SetBackgroundColour(*wxWHITE);

        wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
        auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
        m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

        auto* m_message = new wxStaticText(this, wxID_ANY, _L("Bambu Network plug-in not detected."), wxDefaultPosition, wxDefaultSize, 0);
        m_message->SetForegroundColour(*wxBLACK);
        m_message->Wrap(FromDIP(360));

        // ORCA standardized HyperLink
        auto m_download_hyperlink = new HyperLink(this, _L("Click here to download it."));
        m_download_hyperlink->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            this->Close();
            wxGetApp().ShowDownNetPluginDlg();
        });
        m_sizer_main->Add(m_message, 0, wxALIGN_CENTER | wxALL, FromDIP(15));
        m_sizer_main->Add(m_download_hyperlink, 0, wxALIGN_CENTER | wxALL, FromDIP(10));
        m_sizer_main->Add(0, 0, 1, wxBOTTOM, 10);

        SetSizer(m_sizer_main);
        m_sizer_main->SetSizeHints(this);
        Layout();
        Fit();
        CentreOnParent();
    }
    else {
#if defined(WIN32) || defined(__APPLE__) || defined(__WXMAC__)
        if (Slic3r::PJarczakLinuxBridge::enabled()) {
            m_external_browser_mode = true;
            SetTitle(_L("Login"));
            wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
            auto* m_message = new wxStaticText(this, wxID_ANY, _L("Login opens in your default browser. Finish sign-in there and this dialog will close automatically."), wxDefaultPosition, wxDefaultSize, 0);
            m_message->Wrap(FromDIP(420));
            m_sizer_main->Add(m_message, 0, wxALL | wxEXPAND, FromDIP(16));
            SetSizer(m_sizer_main);
            m_sizer_main->SetSizeHints(this);
            SetSize(FromDIP(wxSize(460, 140)));
            CentreOnParent();
        } else {
#endif
        // Get the login URL from the cloud service agent
        wxString strlang = wxGetApp().current_language_code_safe();
        strlang.Replace("_", "-");
        TargetUrl = wxString::FromUTF8(m_cloud_agent->get_cloud_login_url(strlang.ToStdString()));

        BOOST_LOG_TRIVIAL(info) << "login url = " << TargetUrl.ToStdString();

        m_bbl_user_agent = wxString::Format("BBL-Slicer/v%s", wxGetApp().get_bbl_client_version());

        // Create the webview
        m_browser = WebView::CreateWebView(this, TargetUrl);
        if (m_browser == nullptr) {
            wxLogError("Could not init m_browser");
            return;
        }
        m_browser->Hide();
        m_browser->SetSize(0, 0);

        // Connect the webview events
        Bind(wxEVT_WEBVIEW_NAVIGATING, &ZUserLogin::OnNavigationRequest, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NAVIGATED, &ZUserLogin::OnNavigationComplete, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_LOADED, &ZUserLogin::OnDocumentLoaded, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_ERROR, &ZUserLogin::OnError, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NEWWINDOW, &ZUserLogin::OnNewWindow, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &ZUserLogin::OnTitleChanged, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_FULLSCREEN_CHANGED, &ZUserLogin::OnFullScreenChanged, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ZUserLogin::OnScriptMessage, this, m_browser->GetId());

        // UI
        SetTitle(_L("Login"));
        // Set a more sensible size for web browsing
        wxSize pSize = FromDIP(wxSize(650, 840));
        SetSize(pSize);

        int     screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
        int     screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
        int     MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
        wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
        Move(tmpPT);
#if defined(WIN32) || defined(__APPLE__) || defined(__WXMAC__)
        }
#endif
    }
    wxGetApp().UpdateDlgDarkUI(this);
}

ZUserLogin::~ZUserLogin() {
    if (m_timer != NULL) {
        m_timer->Stop();
        delete m_timer;
        m_timer = NULL;
    }
}

void ZUserLogin::OnTimer(wxTimerEvent &event) {
    m_timer->Stop();

    if (m_networkOk == false)
    {
        ShowErrorPage();
    }
}

bool ZUserLogin::run() {
    m_timer = new wxTimer(this, NETWORK_OFFLINE_TIMER_ID);
    m_timer->Start(m_external_browser_mode ? 30000 : 8000);

#if defined(WIN32) || defined(__APPLE__) || defined(__WXMAC__)
    if (m_external_browser_mode) {
        NetworkAgent* agent = wxGetApp().getAgent();
        if (agent) {
            wxString strlang = wxGetApp().current_language_code_safe();
            strlang.Replace("_", "-");
            const std::string host = agent->get_cloud_service_host();
            m_loopback_port = LOCALHOST_PORT;
            wxGetApp().start_http_server(m_loopback_port);
            const std::string localhost = std::string(LOCALHOST_URL) + std::to_string(m_loopback_port);
            const std::string browser_url = pjarczak_browser_login_url(host, strlang.ToStdString(), localhost);
            BOOST_LOG_TRIVIAL(info) << "external login url = " << browser_url;
            const wxString browser_url_wx = wxString::FromUTF8(browser_url);
            const bool browser_opened = pjarczak_open_external_browser(browser_url_wx);
            BOOST_LOG_TRIVIAL(info) << "external login browser_opened=" << (browser_opened ? 1 : 0);
            if (!browser_opened)
                BOOST_LOG_TRIVIAL(error) << "failed to open external browser for login";
            m_networkOk = true;
        }
    }
#endif

    if (this->ShowModal() == wxID_OK) {
        return true;
    } else {
        return false;
    }
}


void ZUserLogin::load_url(wxString &url)
{
    m_browser->LoadURL(url);
    m_browser->SetFocus();
    UpdateState();
}


/**
 * Method that retrieves the current state from the web control and updates
 * the GUI the reflect this current state.
 */
void ZUserLogin::UpdateState()
{
    // SetTitle(m_browser->GetCurrentTitle());
}

void ZUserLogin::OnIdle(wxIdleEvent &WXUNUSED(evt))
{
    if (m_browser->IsBusy()) {
        wxSetCursor(wxCURSOR_ARROWWAIT);
    } else {
        wxSetCursor(wxNullCursor);
    }
}

// void ZUserLogin::OnClose(wxCloseEvent& evt)
//{
//    this->Hide();
//}

/**
 * Callback invoked when there is a request to load a new page (for instance
 * when the user clicks a link)
 */
void ZUserLogin::OnNavigationRequest(wxWebViewEvent &evt)
{
    //wxLogMessage("%s", "Navigation request to '" + evt.GetURL() + "'(target='" + evt.GetTarget() + "')");

    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
void ZUserLogin::OnNavigationComplete(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    m_browser->Show();
    Layout();
    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void ZUserLogin::OnDocumentLoaded(wxWebViewEvent &evt)
{
    // Only notify if the document is the main frame, not a subframe
    wxString tmpUrl = evt.GetURL();
    std::string strHost = m_cloud_agent->get_cloud_service_host();

    if (tmpUrl.StartsWith("file://") || tmpUrl.Contains(strHost)) {
        m_networkOk = true;
        // wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }

    UpdateState();
}

/**
 * On new window, we veto to stop extra windows appearing
 */
void ZUserLogin::OnNewWindow(wxWebViewEvent &evt)
{
    wxString flag = " (other)";

    if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }

    // wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void ZUserLogin::OnTitleChanged(wxWebViewEvent &evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void ZUserLogin::OnFullScreenChanged(wxWebViewEvent &evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

void ZUserLogin::OnScriptMessage(wxWebViewEvent &evt)
{
    wxString str_input = evt.GetString();

    try {
        json j = json::parse(into_u8(str_input));
        wxString strCmd = j["command"];
        
        if (m_cloud_agent && strCmd == "get_login_cmd") {
            // Return login config (backend_url, apikey, pkce)
            // WebView handles provider selection internally
            std::string login_cmd = m_cloud_agent->build_login_cmd();
            m_loopback_port       = 0;
            try {
                json cfg = json::parse(login_cmd);
                if (cfg.contains("pkce")) {
                    const auto& pkce = cfg["pkce"];
                    if (pkce.contains("loopback_port")) {
                        if (pkce["loopback_port"].is_number_integer()) {
                            m_loopback_port = pkce["loopback_port"].get<int>();
                        } else if (pkce["loopback_port"].is_string()) {
                            m_loopback_port = std::stoi(pkce["loopback_port"].get<std::string>());
                        }
                    }

                    if (m_loopback_port <= 0 && pkce.contains("redirect_uri") && pkce["redirect_uri"].is_string()) {
                        const std::string redirect_uri = pkce["redirect_uri"].get<std::string>();
                        const char*       prefixes[]   = {"localhost:", "127.0.0.1:"};
                        for (const char* prefix : prefixes) {
                            auto start = redirect_uri.find(prefix);
                            if (start == std::string::npos)
                                continue;
                            start += strlen(prefix);
                            auto        end      = redirect_uri.find('/', start);
                            std::string port_str = redirect_uri.substr(start, end - start);
                            try {
                                m_loopback_port = std::stoi(port_str);
                            } catch (...) {
                                m_loopback_port = 0;
                            }
                            break;
                        }
                    }
                }
            } catch (...) {
                m_loopback_port = 0;
            }
            wxString str_js = wxString::FromUTF8("window.postMessage(") + wxString::FromUTF8(login_cmd.c_str()) +
                              wxString::FromUTF8(", '*')");
            this->RunScript(str_js);
            return;
        }

        if (strCmd == "autotest_token")
        {
            m_AutotestToken = j["data"]["token"];
        }
        if (strCmd == "user_login" || strCmd == "user_ticket_login") {
            j["data"]["autotest_token"] = m_AutotestToken;
            std::string message_json = j.dump();

            // End modal dialog first to unblock event loop before processing callbacks
            EndModal(wxID_OK);

            // Handle message after modal dialog ends to avoid deadlock
            // Use wxTheApp->CallAfter to ensure it runs after modal loop exits
            const auto provider = m_cloud_agent->get_id();
            wxTheApp->CallAfter([message_json, provider]() { wxGetApp().handle_script_message(message_json, provider); });
        }
        else if (strCmd == "get_localhost_url") {
            int loopback_port = ensure_loopback_port();
            std::string sequence_id = j["sequence_id"].get<std::string>();
            CallAfter([this, sequence_id] {
                json ack_j;
                ack_j["command"] = "get_localhost_url";
                int loopback_port = m_loopback_port > 0 ? m_loopback_port : LOCALHOST_PORT;
                ack_j["response"]["base_url"] = std::string(LOCALHOST_URL) + std::to_string(loopback_port);
                ack_j["response"]["result"] = "success";
                ack_j["sequence_id"] = sequence_id;
                wxString str_js = wxString::Format("window.postMessage(%s)", ack_j.dump());
                this->RunScript(str_js);
            });
        }
        else if (strCmd == "thirdparty_login") {
            if (j["data"].contains("url")) {
                std::string jump_url = j["data"]["url"].get<std::string>();
                int loopback_port = ensure_loopback_port();
                jump_url = rewrite_loopback_url(jump_url, loopback_port);
                CallAfter([this, jump_url] {
                    wxString url = wxString::FromUTF8(jump_url);
                    wxLaunchDefaultBrowser(url);
                    });
            }
        }
        else if (strCmd == "new_webpage") {
            if (j["data"].contains("url")) {
                std::string jump_url = j["data"]["url"].get<std::string>();
                CallAfter([this, jump_url] {
                    wxString url = wxString::FromUTF8(jump_url);
                    wxLaunchDefaultBrowser(url);
                    });
            }
            return;
        }
    } catch (std::exception &e) {
        wxMessageBox(e.what(), "parse json failed", wxICON_WARNING);
        Close();
    }
}

void ZUserLogin::RunScript(const wxString &javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    m_javascript = javascript;

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}
#if wxUSE_WEBVIEW_IE
void ZUserLogin::OnRunScriptObjectWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void ZUserLogin::OnRunScriptDateWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void ZUserLogin::OnRunScriptArrayWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void ZUserLogin::OnError(wxWebViewEvent &evt)
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

    if( evt.GetInt()==wxWEBVIEW_NAV_ERR_CONNECTION )
    {
        if(m_timer!=NULL)
            m_timer->Stop();

        if (m_networkOk==false)
            ShowErrorPage();
    }

    // wxLogMessage("%s", "Error; url='" + evt.GetURL() + "', error='" +
    // category + " (" + evt.GetString() + ")'");

    // Show the info bar with an error
    // m_info->ShowMessage(_L("An error occurred loading ") + evt.GetURL() +
    // "\n" + "'" + category + "'", wxICON_ERROR);

    UpdateState();
}

void ZUserLogin::OnScriptResponseMessage(wxCommandEvent &WXUNUSED(evt))
{
    // if (!m_response_js.empty())
    //{
    //    RunScript(m_response_js);
    //}

    // RunScript("This is a message to Web!");
    // RunScript("postMessage(\"AABBCCDD\");");
}

bool  ZUserLogin::ShowErrorPage()
{
    wxString ErrortUrl = from_u8((boost::filesystem::path(resources_dir()) / "web\\login\\error.html").make_preferred().string());
    load_url(ErrortUrl);

    return true;
}


}} // namespace Slic3r::GUI
