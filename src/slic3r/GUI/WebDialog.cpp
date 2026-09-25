#include "WebDialog.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/WebHosting.hpp"

#include <wx/event.h>

#include <utility>

namespace Slic3r { namespace GUI {

WebDialog::WebDialog(wxWindow*          parent,
                     const wxString&    title,
                     const std::string& html,
                     const wxSize&      size,
                     MessageHandler     on_message,
                     SubmitHandler      on_submit,
                     CloseHandler       on_close,
                     CloseHandler       on_destroyed,
                     long               wx_style)
    : WebViewHostDialog(parent, wxID_ANY, title, wxDefaultPosition, size, wx_style)
    , m_html(html)
    , m_on_message(std::move(on_message))
    , m_on_submit(std::move(on_submit))
    , m_on_close(std::move(on_close))
    , m_on_destroyed(std::move(on_destroyed))
{
    // A tiny bundled bootstrap page brings the webview up; the real plugin HTML
    // is swapped in via SetPage once the bootstrap finishes loading.
    create_webview(web_hosting::BOOTSTRAP_PAGE, title, size, wxSize(320, 240));

    // Paint the window/webview in the themed background so there is no white
    // flash before the (transparent) bootstrap page and plugin HTML render.
    SetBackgroundColour(wxGetApp().get_window_default_clr());

    if (wxWebView* wv = browser()) {
        wv->SetBackgroundColour(wxGetApp().get_window_default_clr());
        // Theme contract + plugin defaults + bridge are registered by the base
        // create_webview() via add_user_scripts(); nothing to add here.
        // Swap in the plugin HTML once the bootstrap page settles. Bind ERROR too so a
        // missing/blocked bootstrap resource (e.g. a packaged build) still triggers it.
        Bind(wxEVT_WEBVIEW_LOADED, &WebDialog::on_bootstrap_event, this, wv->GetId());
        Bind(wxEVT_WEBVIEW_ERROR, &WebDialog::on_bootstrap_event, this, wv->GetId());
        Bind(wxEVT_WEBVIEW_NAVIGATED, &WebDialog::on_navigated, this, wv->GetId());
    }
    Bind(wxEVT_CLOSE_WINDOW, &WebDialog::on_close_window, this);
}

void WebDialog::add_user_scripts()
{
    if (wxWebView* wv = browser()) {
        wv->AddUserScript(wxString::FromUTF8(WebViewHostDialog::element_defaults_user_script()));
        wv->AddUserScript(wxString::FromUTF8(web_hosting::orca_bridge_script()));
    }
}

WebDialog::~WebDialog()
{
    // Runs on every destruction path. Deliberately NOT a wxEVT_DESTROY handler:
    // that event is sent from the base ~wxDialog(), after this subclass's members
    // are already destroyed. Here the members are still alive, and the callback
    // only touches the host-side registry (no Python), so this is safe.
    if (m_on_destroyed)
        m_on_destroyed();
}

void WebDialog::post_message(WebDialog* dialog, const nlohmann::json& data)
{
    if (dialog != nullptr && dialog->is_open())
        dialog->push_message(data);
}

void WebDialog::request_close(WebDialog* dialog)
{
    if (dialog != nullptr)
        dialog->Close();
}

void WebDialog::destroy_silently(WebDialog* dialog)
{
    if (dialog == nullptr)
        return;

    // Forced plugin teardown must not invoke Python close callbacks. End a modal
    // loop first, otherwise destroying the window can leave ShowModal() running.
    if (dialog->IsModal()) {
        dialog->m_open = false;
        dialog->EndModal(wxID_CANCEL);
    }
    dialog->Destroy();
}

void WebDialog::on_bootstrap_event(wxWebViewEvent& event)
{
    const bool loaded = event.GetEventType() == wxEVT_WEBVIEW_LOADED;
    // The first bootstrap load (or its error) triggers the swap to plugin HTML.
    if (!m_content_loaded)
        load_page_html();
    // WebKit reloads the SetPage base URL, so a committed load of it that we did not start is a reload.
    // A failed navigation is reported against the page that stayed but never commits. Edge ignores the
    // base URL and restores SetPage content itself, so nothing matches there.
    else if (web_hosting::is_content_url(event.GetURL())) {
        if (m_own_page_load)
            m_own_page_load = false;
        else if (loaded && m_content_navigated)
            load_page_html();
    }
    if (loaded)
        m_content_navigated = false;
    event.Skip();
}

void WebDialog::on_navigated(wxWebViewEvent& event)
{
    m_content_navigated = web_hosting::is_content_url(event.GetURL());
    event.Skip();
}

void WebDialog::load_page_html()
{
    m_content_loaded = true;
    if (wxWebView* wv = browser()) {
        m_own_page_load = true;
        wv->SetPage(wxString::FromUTF8(m_html), web_hosting::content_base_url());
    }
}

void WebDialog::on_script_message(const nlohmann::json& payload)
{
    if (payload.value("channel", std::string()) == "orca") {
        const std::string    kind = payload.value("kind", std::string());
        const nlohmann::json data = payload.contains("data") ? payload["data"] : nlohmann::json();
        if (kind == "message") {
            if (m_on_message)
                m_on_message(data);
        } else if (kind == "submit") {
            finish(true, data);
        } else if (kind == "close") {
            finish(false, nlohmann::json());
        }
        return;
    }

    // Fall back to the shared shell commands (e.g. "close_page").
    handle_common_script_command(payload);
}

void WebDialog::push_message(const nlohmann::json& data)
{
    if (!m_open)
        return;
    nlohmann::json envelope;
    envelope["data"] = data;
    call_web_handler(envelope, wxT("__orcaDispatch"));
}

void WebDialog::finish(bool submitted, const nlohmann::json& data)
{
    if (!m_open)
        return;
    m_open = false;
    if (submitted) {
        m_result = data;
        fire_submit(data);
    } else {
        m_result.reset();
        fire_close();
    }

    if (IsModal())
        EndModal(submitted ? wxID_OK : wxID_CANCEL);
    else
        Close();
}

void WebDialog::on_close_window(wxCloseEvent&)
{
    if (!m_open) {
        // finish() already dispatched submit/close and requested the close.
        // Modeless windows still need to be destroyed after that request.
        if (!IsModal())
            Destroy();
        return;
    }

    m_open = false;
    m_result.reset();
    fire_close();
    if (IsModal()) {
        EndModal(wxID_CANCEL);
        return;
    }
    Destroy();
}

void WebDialog::fire_submit(const nlohmann::json& data)
{
    if (m_on_submit) {
        SubmitHandler cb = std::move(m_on_submit);
        cb(data);
    }
}

void WebDialog::fire_close()
{
    if (m_close_fired)
        return;
    m_close_fired = true;
    if (m_on_close) {
        CloseHandler cb = m_on_close;
        m_on_close      = nullptr;
        cb();
    }
}

}} // namespace Slic3r::GUI
