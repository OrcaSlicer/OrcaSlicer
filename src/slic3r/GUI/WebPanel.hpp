#pragma once

#include <nlohmann/json.hpp>

#include <wx/panel.h>
#include <wx/webview.h>

#include <optional>
#include <string>

namespace Slic3r { namespace GUI {

// Host-supplied HTML in a web view panel, for any window that embeds or derives from it (today plugin
// Pages tabs and docked panels): bootstrap page and swap, theme and bridge scripts, live re-theming, and
// window.orca messages routed to on_page_message().
class WebPanel : public wxPanel
{
public:
    WebPanel(wxWindow* parent, const char* bridge_script);

protected:
    wxWebView* browser() const { return m_browser; }

    // Delivers an already serialised JSON value to the page's window.orca.onMessage handlers,
    // waiting briefly for the bridge while the page is still loading. Main thread only.
    void post_to_page(const std::string& json);

    // The plugin HTML to show once the bootstrap page has loaded, and again when WebKit reloads it;
    // std::nullopt leaves it blank.
    virtual std::optional<std::string> page_html() = 0;
    // A window.orca message from the page; false for a kind this host does not handle (logged).
    virtual bool on_page_message(const std::string& kind, const nlohmann::json& data) = 0;

private:
    void on_load_event(wxWebViewEvent& event);
    void on_navigated(wxWebViewEvent& event);
    void on_script_message(wxWebViewEvent& event);
    void on_webview_recreated(wxCommandEvent& event);
    void apply_theme();
    void load_page_html();

    wxWebView* m_browser{nullptr};
    bool       m_content_loaded{false};
    bool       m_own_page_load{false};   // a SetPage of the plugin HTML is in flight
    bool       m_content_navigated{false}; // a navigation to the base URL has committed
};

}} // namespace Slic3r::GUI
