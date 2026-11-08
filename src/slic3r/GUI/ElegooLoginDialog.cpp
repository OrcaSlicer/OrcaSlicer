#include "ElegooLoginDialog.hpp"

#include <string>

#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/ElegooCloud.hpp"

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

ElegooLoginDialog::ElegooLoginDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Sign in to Elegoo"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxStaticText(this, wxID_ANY, _L("Region")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* region = new wxChoice(this, wxID_ANY);
    region->Append(_L("Global"));
    region->Append(_L("Mainland China"));
    region->SetSelection(ElegooCloud::instance().region() == "CN" ? 1 : 0);
    region->Bind(wxEVT_CHOICE, [this, region](wxCommandEvent&) {
        ElegooCloud::instance().set_region(region->GetSelection() == 1 ? "CN" : "global");
        load_page();
    });
    bar->Add(region, 0, wxALIGN_CENTER_VERTICAL);
    bar->AddStretchSpacer();
#ifdef _WIN32
    auto* import = new wxButton(this, wxID_ANY, _L("Use ElegooSlicer sign-in"));
    import->SetToolTip(_L("Copy the Elegoo account that ElegooSlicer is signed in to on this computer."));
    import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::string error;
        if (ElegooCloud::instance().import_from_elegooslicer(error))
            EndModal(wxID_OK);
        else
            wxMessageBox(wxString::FromUTF8(error), _L("Sign in to Elegoo"), wxOK | wxICON_WARNING, this);
    });
    bar->Add(import, 0, wxALIGN_CENTER_VERTICAL);
#endif
    sizer->Add(bar, 0, wxEXPAND | wxALL, FromDIP(10));

    m_browser = WebView::CreateWebView(this, wxEmptyString);
    if (m_browser != nullptr) {
        m_browser->SetUserAgent(wxString::FromUTF8(ElegooCloud::user_agent() + " Mozilla/5.0"));
        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ElegooLoginDialog::on_script_message, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& e) { wxLaunchDefaultBrowser(e.GetURL()); }, m_browser->GetId());
        sizer->Add(m_browser, 1, wxEXPAND);
    } else {
        sizer->Add(new wxStaticText(this, wxID_ANY, _L("The web view needed for signing in is not available.")), 1, wxALL, FromDIP(20));
    }
    SetSizer(sizer);
    SetSize(FromDIP(wxSize(500, 760)));
    sizer->SetSizeHints(this);
    SetSize(FromDIP(wxSize(500, 760)));
    CentreOnParent();
    load_page();
}

void ElegooLoginDialog::load_page()
{
    if (m_browser != nullptr)
        WebView::LoadUrl(m_browser, wxString::FromUTF8(ElegooCloud::instance().login_url(wxGetApp().dark_mode())));
}

void ElegooLoginDialog::on_dpi_changed(const wxRect&) { Layout(); }

void ElegooLoginDialog::respond(const wxString& id, const wxString& method, bool ok, const wxString& message)
{
    const json reply = {{"id", id.ToUTF8().data()},
                        {"method", method.ToUTF8().data()},
                        {"type", "response"},
                        {"code", ok ? 0 : -1},
                        {"message", ok ? "success" : message.ToUTF8().data()},
                        {"data", json::object()}};
    WebView::RunScript(m_browser, wxString::FromUTF8("window.HandleStudio && window.HandleStudio(" + reply.dump() + ")"));
}

// The page talks the same small JSON request/response protocol ElegooSlicer's pages use.
void ElegooLoginDialog::on_script_message(wxWebViewEvent& evt)
{
    const json msg = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
    if (!msg.is_object())
        return;
    const std::string type   = msg.value("type", "request");
    const std::string method = msg.value("method", "");
    const wxString    id     = wxString::FromUTF8(msg.contains("id") && msg["id"].is_string() ? msg["id"].get<std::string>() : "");
    if (type != "request")
        return;
    if (method == "report.userInfo") {
        std::string error;
        const bool  ok = ElegooCloud::instance().accept_login_report(msg.value("params", json::object()), error);
        respond(id, method, ok, wxString::FromUTF8(error));
        if (ok)
            CallAfter([this]() { EndModal(wxID_OK); });
    } else if (method == "report.ready") {
        respond(id, method, true, {});
        // The page asks for a stable device id; keep one per installation.
        AppConfig*  cfg       = wxGetApp().app_config;
        std::string device_id = cfg->get("vendor_send", "elegoo_device_id");
        if (device_id.empty()) {
            device_id = boost::uuids::to_string(boost::uuids::random_generator()());
            cfg->set("vendor_send", "elegoo_device_id", device_id);
        }
        const json event = {{"method", "client.setDeviceId"}, {"type", "event"}, {"data", {{"deviceId", device_id}}}};
        WebView::RunScript(m_browser, wxString::FromUTF8("window.HandleStudio && window.HandleStudio(" + event.dump() + ")"));
    } else if (method == "report.websiteOpen") {
        respond(id, method, true, {});
        const std::string url = msg.value("params", json::object()).value("url", "");
        if (url.rfind("https://", 0) == 0)
            wxLaunchDefaultBrowser(wxString::FromUTF8(url));
    } else if (method == "reload") {
        respond(id, method, true, {});
        CallAfter([this]() { load_page(); });
    } else {
        respond(id, method, true, {});
    }
}

bool ElegooLoginDialog::sign_in(wxWindow* parent)
{
    ElegooLoginDialog dlg(parent);
    dlg.ShowModal();
    return ElegooCloud::instance().signed_in();
}

}} // namespace Slic3r::GUI
