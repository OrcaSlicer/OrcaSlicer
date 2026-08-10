#include "PluginWebDialog.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>

#include <wx/event.h>

#include <utility>

namespace Slic3r { namespace GUI {

namespace {

// Low-specificity element defaults (no !important) for UNSTYLED plugin HTML, so a bare
// plugin page looks native while any CSS the plugin ships still wins. Built on the
// --orca-* variables the host injects (see WebViewHostDialog); document-start injected
// AFTER the host contract so the variables are defined (shares the base injector's
// WebView2 timing guard).
std::string plugin_defaults_user_script()
{
    std::string css;
    css += "<style id=\"orca-plugin-defaults\">";
    css += "html,body{background:var(--orca-bg);color:var(--orca-fg);"
           "font-family:var(--orca-font);font-size:13px;}";
    css += "body{margin:0;}";
    css += "h1,h2,h3,h4,h5,h6{color:var(--orca-fg);font-weight:600;}";
    css += "a{color:var(--orca-accent);}";
    css += "hr{border:0;border-top:1px solid var(--orca-border);}";
    css += "button{font:inherit;color:var(--orca-accent-fg);background:var(--orca-accent);"
           "border:1px solid var(--orca-accent);border-radius:4px;padding:5px 14px;cursor:pointer;}";
    css += "button:hover{filter:brightness(1.1);}";
    css += "button:disabled{opacity:.5;cursor:default;}";
    css += "input,select,textarea{font:inherit;color:var(--orca-fg);"
           "background:var(--orca-bg);border:1px solid var(--orca-border);"
           "border-radius:4px;padding:4px 8px;}";
    css += "input:focus,select:focus,textarea:focus{outline:none;border-color:var(--orca-accent);}";
    css += "table{border-collapse:collapse;}";
    css += "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid var(--orca-border);}";
    css += "th{color:var(--orca-muted);font-weight:600;}";
    css += "::-webkit-scrollbar{width:12px;height:12px;}";
    css += "::-webkit-scrollbar-thumb{background:var(--orca-border);border-radius:6px;}";
    css += "::-webkit-scrollbar-track{background:transparent;}";
    css += "</style>";
    return WebViewHostDialog::document_start_injector(css, "orca-plugin-defaults", "beforeend");
}

// Injected into the top-level page at document start (before the plugin's own
// scripts). Defines window.orca as the only host surface the page may use. It
// references window.wx lazily (at call time) so it never races the backend's
// deferred registration of the "wx" message handler. Guarded against
// double-injection so it is harmless if also prepended.
constexpr char ORCA_BRIDGE_JS[] = R"JS(
(function () {
  if (window.top !== window.self) return;
  if (window.orca) return;
  var handlers = [];
  function send(kind, data) {
    try {
      window.wx.postMessage(JSON.stringify({
        channel: 'orca', kind: kind, data: (data === undefined ? null : data)
      }));
    } catch (e) { /* bridge not ready yet */ }
  }
  window.orca = {
    postMessage: function (d) { send('message', d); },
    submit:      function (d) { send('submit', d); },
    close:       function ()  { send('close'); },
    onMessage:   function (cb) { if (typeof cb === 'function') handlers.push(cb); }
  };
  window.__orcaDispatch = function (payload) {
    var data = payload ? payload.data : null;
    for (var i = 0; i < handlers.length; i++) {
      try { handlers[i](data); } catch (e) {}
    }
  };
})();
)JS";

// file:// base URL for plugin HTML loaded via SetPage, so self-referencing
// relative URLs resolve against the bundled web resources directory.
wxString web_base_url()
{
    const std::string dir = (boost::filesystem::path(resources_dir()) / "web").make_preferred().string();
    return wxString("file://") + from_u8(dir) + "/";
}

} // namespace

PluginWebDialog::PluginWebDialog(wxWindow*          parent,
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
    create_webview("web/dialog/PluginWebDialog/blank.html", title, size, wxSize(320, 240));

    // Paint the window/webview in the themed background so there is no white
    // flash before the (transparent) bootstrap page and plugin HTML render.
    SetBackgroundColour(wxGetApp().get_window_default_clr());

    if (wxWebView* wv = browser()) {
        wv->SetBackgroundColour(wxGetApp().get_window_default_clr());
        // Theme contract + plugin defaults + bridge are registered by the base
        // create_webview() via add_user_scripts(); nothing to add here.
        // Swap in the plugin HTML once the bootstrap page settles. Bind ERROR too so a
        // missing/blocked bootstrap resource (e.g. a packaged build) still triggers it.
        Bind(wxEVT_WEBVIEW_LOADED, &PluginWebDialog::on_bootstrap_event, this, wv->GetId());
        Bind(wxEVT_WEBVIEW_ERROR, &PluginWebDialog::on_bootstrap_event, this, wv->GetId());
    }
    Bind(wxEVT_CLOSE_WINDOW, &PluginWebDialog::on_close_window, this);
}

void PluginWebDialog::add_user_scripts()
{
    if (wxWebView* wv = browser()) {
        wv->AddUserScript(wxString::FromUTF8(plugin_defaults_user_script()));
        wv->AddUserScript(ORCA_BRIDGE_JS);
    }
}

PluginWebDialog::~PluginWebDialog()
{
    // Runs on every destruction path. Deliberately NOT a wxEVT_DESTROY handler:
    // that event is sent from the base ~wxDialog(), after this subclass's members
    // are already destroyed. Here the members are still alive, and the callback
    // only touches the host-side registry (no Python), so this is safe.
    if (m_on_destroyed)
        m_on_destroyed();
}

void PluginWebDialog::post_message(PluginWebDialog* dialog, const nlohmann::json& data)
{
    if (dialog != nullptr && dialog->is_open())
        dialog->push_message(data);
}

void PluginWebDialog::request_close(PluginWebDialog* dialog)
{
    if (dialog != nullptr)
        dialog->Close();
}

void PluginWebDialog::destroy_for_plugin(PluginWebDialog* dialog)
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

void PluginWebDialog::on_bootstrap_event(wxWebViewEvent& event)
{
    // The first bootstrap load (or its error) triggers the swap to plugin HTML;
    // the resulting plugin-page load is ignored (guarded by m_content_loaded).
    load_plugin_content();
    event.Skip();
}

void PluginWebDialog::load_plugin_content()
{
    if (m_content_loaded)
        return;
    m_content_loaded = true;
    if (wxWebView* wv = browser())
        wv->SetPage(wxString::FromUTF8(m_html), web_base_url());
}

void PluginWebDialog::on_script_message(const nlohmann::json& payload)
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

void PluginWebDialog::push_message(const nlohmann::json& data)
{
    if (!m_open)
        return;
    nlohmann::json envelope;
    envelope["data"] = data;
    call_web_handler(envelope, wxT("__orcaDispatch"));
}

void PluginWebDialog::finish(bool submitted, const nlohmann::json& data)
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

void PluginWebDialog::on_close_window(wxCloseEvent&)
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

void PluginWebDialog::fire_submit(const nlohmann::json& data)
{
    if (m_on_submit) {
        SubmitHandler cb = std::move(m_on_submit);
        cb(data);
    }
}

void PluginWebDialog::fire_close()
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
