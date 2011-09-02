#include "PrinterCloudAuthDialog.hpp"
#include <wx/sizer.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>

#include <nlohmann/json.hpp>
#include "MainFrame.hpp"
#include <boost/dll.hpp>

#include <sstream>
#include <slic3r/GUI/Widgets/WebView.hpp>
//------------------------------------------
//          PrinterCloundAuthDialog
//------------------------------------------
namespace Slic3r { namespace GUI {

PrinterCloudAuthDialog::PrinterCloudAuthDialog(wxWindow* parent, PrintHost* host)
    : wxDialog((wxWindow*) (wxGetApp().mainframe), wxID_ANY, "Login")
{
    SetBackgroundColour(*wxWHITE);
    // Url
    host->get_login_url(m_TargetUrl);
    BOOST_LOG_TRIVIAL(info) << "login url = " << m_TargetUrl.ToStdString();

    // Create the webview
    m_browser = WebView::CreateWebView(this, m_TargetUrl);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->Hide();
    m_browser->SetSize(0, 0);

    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &PrinterCloudAuthDialog::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &PrinterCloudAuthDialog::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &PrinterCloudAuthDialog::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrinterCloudAuthDialog::OnNewWindow, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterCloudAuthDialog::OnScriptMessage, this, m_browser->GetId());

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
}

PrinterCloudAuthDialog::~PrinterCloudAuthDialog() {}

void PrinterCloudAuthDialog::OnNavigationRequest(wxWebViewEvent& evt)
{
    //todo
}

void PrinterCloudAuthDialog::OnNavigationComplete(wxWebViewEvent& evt)
{
    m_browser->Show();
    Layout();
    //fortest
    //WebView::RunScript(m_browser, "window.wx.postMessage('This is a web message')");
}

void PrinterCloudAuthDialog::OnDocumentLoaded(wxWebViewEvent& evt)
{
    // todo
}

void PrinterCloudAuthDialog::OnNewWindow(wxWebViewEvent& evt) {

}

void PrinterCloudAuthDialog::OnScriptMessage(wxWebViewEvent& evt)
{
    wxString str_input = evt.GetString();
    try {
        json     j      = json::parse(into_u8(str_input));
        wxString strCmd = j["command"];
        if (strCmd == "login_token") {
            auto token = j["data"]["token"];
            m_apikey = token;
        }
        Close();
    } catch (std::exception& e) {
        wxMessageBox(e.what(), "parse json failed", wxICON_WARNING);
        Close();
    }
}

}
} // namespace Slic3r::GUI