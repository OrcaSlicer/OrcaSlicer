#include "WebViewHostDialog.hpp"

#include "WebView.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <nlohmann/json.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/log.h>
#include <wx/sizer.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

namespace {

// CSS "#rrggbb" for a wxColour (portable accessor used throughout the codebase).
std::string css_color(const wxColour& c) { return c.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(); }

// "dark"/"light" for the live app theme — the value of both data-orca-theme and color-scheme.
std::string host_theme_name() { return wxGetApp().dark_mode() ? "dark" : "light"; }

// The host theme "contract": CSS custom properties filled from the LIVE app theme,
// plus color-scheme. Consumed by resources/web/dialog/css/theme.css and by plugin
// content. Variables only — no element styling — so it never fights a page's CSS.
std::string host_theme_vars_css()
{
    GUI_App&       app    = wxGetApp();
    const wxColour bg     = app.get_window_default_clr();
    const wxColour fg     = app.get_label_clr_default();
    const wxColour muted  = app.get_label_clr_sys();
    const wxColour border = app.get_highlight_default_clr();
    const wxColour accent = StateColor::darkModeColorFor(wxColour("#009688"));
    std::string    font   = app.normal_font().GetFaceName().ToStdString();
    // Strip characters that could break out of the CSS value / <style> block.
    font.erase(std::remove_if(font.begin(), font.end(), [](char c) {
                   return c == '\'' || c == '"' || c == '<' || c == '>' || c == '{' || c == '}' || c == ';';
               }),
               font.end());

    std::string s;
    s += ":root{";
    s += "--orca-bg:" + css_color(bg) + ";";
    s += "--orca-fg:" + css_color(fg) + ";";
    s += "--orca-muted:" + css_color(muted) + ";";
    s += "--orca-border:" + css_color(border) + ";";
    s += "--orca-accent:" + css_color(accent) + ";";
    s += "--orca-accent-fg:#ffffff;";
    s += "--orca-font:" + (font.empty() ? std::string() : "'" + font + "',") +
         "system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;";
    s += "color-scheme:" + host_theme_name() + ";";
    s += "}";
    return s;
}

// Document-start user script: injects the contract <style>, stamps data-orca-theme before
// first paint, and raises a JS flag so the legacy globalapi.js dark.css poll stands down for
// host-themed pages. The WebView2 timing guard lives in document_start_injector().
std::string host_theme_user_script()
{
    const std::string style = "<style id=\"orca-host-theme-vars\">" + host_theme_vars_css() + "</style>";
    return WebViewHostDialog::document_start_injector(
        style, "orca-host-theme-vars", "afterbegin",
        "window.__orcaHostThemed=true;var theme=\"" + host_theme_name() + "\";",
        "if(document.documentElement)document.documentElement.setAttribute('data-orca-theme',theme);");
}

// JS to re-theme an already-loaded document live (no reload): replace the injected
// style's contents and update data-orca-theme. Everything downstream (theme.css
// tokens, plugin element defaults, page layout) re-cascades from these values.
std::string host_theme_apply_js()
{
    const std::string vars_literal = nlohmann::json(host_theme_vars_css()).dump();
    const std::string theme = host_theme_name();
    return "(function(){var css=" + vars_literal + ";var theme=\"" + theme + "\";" + R"JS(
var el=document.getElementById('orca-host-theme-vars');
if(el){el.textContent=css;}
else if(document.head){document.head.insertAdjacentHTML('afterbegin','<style id="orca-host-theme-vars"></style>');var e2=document.getElementById('orca-host-theme-vars');if(e2)e2.textContent=css;}
if(document.documentElement)
    document.documentElement.setAttribute('data-orca-theme',theme);
})();)JS";
}

} // namespace

std::string WebViewHostDialog::document_start_injector(const std::string& markup,
                                                       const char*        dom_id,
                                                       const char*        position,
                                                       const std::string& prelude,
                                                       const std::string& on_inject)
{
    const std::string literal = nlohmann::json(markup).dump();
    std::string       s;
    s += "(function(){";
    s += prelude;
    s += "var css=" + literal + ";";
    s += "function inject(){";
    s += "var root=document.head||document.documentElement;if(!root)return false;";
    s += "if(!document.getElementById('" + std::string(dom_id) + "'))root.insertAdjacentHTML('" +
         std::string(position) + "',css);";
    s += on_inject;
    s += "return true;}";
    s += "if(inject())return;";
    s += "var obs=new MutationObserver(function(){if(inject())obs.disconnect();});";
    s += "obs.observe(document,{childList:true});})();";
    return s;
}

WebViewHostDialog::WebViewHostDialog(wxWindow* parent,
                                     wxWindowID id,
                                     const wxString& title,
                                     const wxPoint& pos,
                                     const wxSize& size,
                                     long style)
    : DPIDialog(parent, id, title, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
}

bool WebViewHostDialog::create_webview(const std::string& resource_path,
                                       const wxString& title,
                                       const wxSize& dialog_size,
                                       const wxSize& min_size)
{
    SetTitle(title);
    SetMinSize(FromDIP(min_size));

    const wxString target_url = build_resource_url(resource_path);
    wxBoxSizer*    topsizer   = new wxBoxSizer(wxVERTICAL);

    m_browser = WebView::CreateWebView(this, target_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        delete topsizer;
        return false;
    }

    SetSizer(topsizer);
    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    SetSize(FromDIP(dialog_size));
    CenterOnParent();

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebViewHostDialog::on_script_message_event, this, m_browser->GetId());

    // Inject the shared host theme contract BEFORE the first load so the page paints in
    // the app theme with no flash, and re-theme live when the app theme toggles.
    register_theme_user_scripts();
    m_browser->Bind(EVT_WEBVIEW_RECREATED, &WebViewHostDialog::on_webview_recreated, this);

    load_url(target_url);
    wxGetApp().UpdateDlgDarkUI(this);
    return true;
}

wxString WebViewHostDialog::build_resource_url(const std::string& resource_path) const
{
    wxString target_url = from_u8((boost::filesystem::path(resources_dir()) / resource_path).make_preferred().string());

    if (append_language_to_url()) {
        const wxString lang = wxGetApp().current_language_code_safe();
        if (!lang.empty()) {
            target_url += wxT("?lang=");
            target_url += lang;
        }
    }

    return wxString("file://") + target_url;
}

void WebViewHostDialog::load_url(const wxString& url)
{
    if (!m_browser)
        return;

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " enter, url=" << into_u8(url);
    WebView::LoadUrl(m_browser, url);
    m_browser->SetFocus();
}

bool WebViewHostDialog::run_script(const wxString& script)
{
    if (!m_browser)
        return false;

    return WebView::RunScript(m_browser, script);
}

void WebViewHostDialog::call_web_handler(const nlohmann::json& payload, const wxString& handler)
{
    const wxString payload_text = wxString::FromUTF8(payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
    const wxString script       = handler + wxT("(") + payload_text + wxT(")");

    wxGetApp().CallAfter([this, script] { run_script(script); });
}

bool WebViewHostDialog::handle_common_script_command(const nlohmann::json& payload, int close_return_code)
{
    const std::string command = payload.value("command", "");
    if (command == "close_page") {
        if (IsModal())
            EndModal(close_return_code);
        else
            Close();
        return true;
    }

    return false;
}

void WebViewHostDialog::on_dpi_changed(const wxRect&)
{
    Refresh();
}

void WebViewHostDialog::on_script_message_parse_error(const wxString& payload, const std::exception& error)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "; payload=" << into_u8(payload) << "; error=" << error.what();
}

void WebViewHostDialog::on_script_message_event(wxWebViewEvent& event)
{
    const wxString payload = event.GetString();

    try {
        on_script_message(nlohmann::json::parse(payload.utf8_string()));
    } catch (const std::exception& e) {
        on_script_message_parse_error(payload, e);
    }
}

void WebViewHostDialog::register_theme_user_scripts()
{
    if (!m_browser)
        return;
    // Added once, at creation. Deliberately no RemoveAllUserScripts() here: the "wx"
    // script message handler is registered separately (AddScriptMessageHandler), but on
    // some backends RemoveAllUserScripts() drops it too, which would break
    // window.wx.postMessage / HandleStudio. Live re-theme goes through apply_theme_live().
    m_browser->AddUserScript(wxString::FromUTF8(host_theme_user_script()));
    add_user_scripts();
}

void WebViewHostDialog::apply_theme_live()
{
    if (!m_browser)
        return;
    // Update the already-loaded document in place (no reload, no flash) by rewriting the
    // injected :root variables + data-orca-theme; the whole cascade re-flows from these.
    // The document-start script keeps the creation-time theme for any later reload, and
    // these dialogs are not reloaded on a theme toggle (see WebView::RecreateAll).
    run_script(wxString::FromUTF8(host_theme_apply_js()));
}

void WebViewHostDialog::on_webview_recreated(wxCommandEvent&)
{
    // Handled: do NOT Skip(), so WebView::RecreateAll skips the redundant reload.
    apply_theme_live();
}

}} // namespace Slic3r::GUI
