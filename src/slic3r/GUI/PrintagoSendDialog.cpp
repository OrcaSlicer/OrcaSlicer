#include "PrintagoSendDialog.hpp"

#include <wx/display.h>
#include <wx/sizer.h>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Utils.hpp"

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

PrintagoSendDialog::PrintagoSendDialog(wxWindow* parent, const PrintagoProject& project, const wxString& url)
    : wxDialog((wxWindow*) (wxGetApp().mainframe), wxID_ANY, _L("Printago"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_project(project)
    , m_url(url)
{
    SetBackgroundColour(*wxWHITE);

    // Create the webview WITHOUT loading a URL yet, register the "printago" script-message handler so
    // window.printago exists before the page's JS runs, then load the page. We deliberately use
    // wxWebView::New rather than WebView::CreateWebView: the factory registers its own "wx" handler via
    // a deferred, re-entrancy-guarded path (a WKWebView limitation), and adding a second named handler
    // against it can trip that same re-entrancy. A single synchronous handler on a fresh webview (as
    // WipeTowerDialog does) is the safe, proven pattern. Storage still persists in the backend's
    // default data store, so the Printago (Firebase) login survives restarts.
    m_browser = wxWebView::New(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxWebViewBackendDefault, wxNO_BORDER);
    if (m_browser == nullptr) {
        wxLogError("Printago: could not create webview");
        return;
    }
    m_browser->AddScriptMessageHandler("printago");
    m_bridge = std::make_unique<PrintagoBridge>(m_browser);

#ifdef __linux__
    // WebKit2GTK does not persist cookies to disk by default; point it at a persistent SQLite store so
    // the login survives restarts (mirrors PrinterWebView). A dedicated file avoids contending with the
    // Printago tab's cookie store.
    {
        auto cookiesPath   = boost::filesystem::path(data_dir() + "/cache/printago_send_cookies.db");
        auto wv            = static_cast<WebKitWebView*>(m_browser->GetNativeBackend());
        auto cookieManager = webkit_web_context_get_cookie_manager(webkit_web_view_get_context(wv));
        webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesPath.c_str(),
                                                     WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
#endif

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrintagoSendDialog::on_script_message, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrintagoSendDialog::on_new_window, this, m_browser->GetId());

    // Diagnose / self-heal an occasional blank load: log load errors and completions, and reload once
    // if the initial navigation errors out.
    Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent& evt) {
        BOOST_LOG_TRIVIAL(error) << "Printago dialog webview error: " << into_u8(evt.GetString())
                                 << " url=" << into_u8(evt.GetURL());
        if (!m_load_retried && m_browser) {
            m_load_retried = true;
            m_browser->LoadURL(m_url);
        }
    }, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, [](wxWebViewEvent& evt) {
        BOOST_LOG_TRIVIAL(info) << "Printago dialog webview loaded: " << into_u8(evt.GetURL());
    }, m_browser->GetId());

    m_browser->LoadURL(m_url);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);

    // Fixed initial size (the dialog is resizable). Not driven by the page content; tune here if the
    // embedded pages get taller/shorter. Also cap to the working display so it never exceeds the
    // screen on smaller laptops.
    wxSize size = FromDIP(wxSize(936, 720)); // ~30% wider than the old 720 to fit the page UI better
    const wxRect area = wxDisplay(wxDisplay::GetFromWindow(this)).GetClientArea();
    size.y = std::min(size.y, int(area.height * 0.92));
    size.x = std::min(size.x, area.width);
    SetSize(size);
    CenterOnParent();
}

PrintagoSendDialog::~PrintagoSendDialog() = default; // m_bridge dtor cancels any in-flight upload

void PrintagoSendDialog::on_new_window(wxWebViewEvent& evt)
{
    // target=_blank / window.open — never open in the embedded webview.
    wxLaunchDefaultBrowser(evt.GetURL());
}

void PrintagoSendDialog::on_script_message(wxWebViewEvent& evt)
{
    json msg;
    try {
        msg = json::parse(into_u8(evt.GetString()));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Printago: could not parse bridge message: " << e.what();
        return;
    }

    // Ignore other protocol versions and malformed envelopes.
    if (msg.contains("v") && msg["v"].is_number_integer() && msg["v"].get<int>() != 1)
        return;
    if (!msg.contains("type") || !msg["type"].is_string())
        return;

    const std::string type    = msg["type"].get<std::string>();
    const json        payload = msg.contains("payload") && msg["payload"].is_object() ? msg["payload"] : json::object();

    if (type == "page:ready") {
        handle_page_ready();
    } else if (type == "upload:begin") {
        if (m_bridge)
            m_bridge->start_upload(payload, m_project.file_path);
    } else if (type == "dialog:close") {
        if (m_bridge)
            m_bridge->cancel_upload();
        // Defer the close so we don't tear the webview down from inside its own event handler.
        CallAfter([this]() { EndModal(wxID_OK); });
    } else if (type == "link:external") {
        if (payload.contains("url") && payload["url"].is_string())
            wxLaunchDefaultBrowser(from_u8(payload["url"].get<std::string>()));
    } else if (type == "part:replaced") {
        // Replace-dialog flow (/orca/replace?partId=...): the part's file has been swapped
        // server-side (or the user canceled). The page follows up with dialog:close either way.
        const bool ok = payload.value("ok", false);
        if (ok) {
            BOOST_LOG_TRIVIAL(info) << "Printago dialog: part:replaced ok";
            if (auto* plater = wxGetApp().plater(); plater && plater->get_notification_manager())
                plater->get_notification_manager()->push_notification(_u8L("Part updated in Printago."));
        } else {
            BOOST_LOG_TRIVIAL(warning) << "Printago dialog: part:replaced failed/canceled: "
                                       << payload.value("error", std::string());
        }
    } else if (type == "page:authed" || type == "part:created" || type == "build:queued" || type == "page:error") {
        BOOST_LOG_TRIVIAL(info) << "Printago page event [" << type << "]: " << payload.dump();
    } else {
        BOOST_LOG_TRIVIAL(debug) << "Printago: ignoring unknown message type: " << type;
    }
}

void PrintagoSendDialog::handle_page_ready()
{
    if (!m_bridge)
        return;
    // Re-init rule: resend host:init + project:info on EVERY page:ready. The page's JS state is reset
    // by internal navigation (e.g. a login round trip), so these must be idempotent and stateless.
    m_bridge->send_host_init();

    json info;
    info["fileName"]      = m_project.file_name;
    info["projectName"]   = m_project.project_name;
    info["fileSizeBytes"] = m_project.file_size_bytes;
    info["plateCount"]    = m_project.plate_count;
    m_bridge->send("project:info", info);

    m_bridge->send_theme(wxGetApp().dark_mode());
}

}} // namespace Slic3r::GUI
