#include "DockPanel.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/WebHosting.hpp"

#include <wx/weakref.h>

#include <algorithm>
#include <utility>

namespace Slic3r { namespace GUI {

std::string plugin_pane_name(const std::string& plugin_key, const std::string& title)
{
    std::string name = "plugin:" + plugin_key + ":" + title;
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '|' || c == ';' || c == '=' || c == '\\'; }, '_');
    return name;
}

DockPanel::DockPanel(wxWindow*          parent,
                     const std::string& html,
                     MessageHandler     on_message,
                     CloseHandler       on_close,
                     CloseHandler       on_destroyed)
    : WebPanel(parent, web_hosting::orca_bridge_script())
    , m_html(html)
    , m_on_message(std::move(on_message))
    , m_on_close(std::move(on_close))
    , m_on_destroyed(std::move(on_destroyed))
{
    // A link asking for a new window has nowhere to open from a docked panel.
    browser()->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& event) { event.Veto(); });
}

DockPanel::~DockPanel()
{
    if (m_on_destroyed)
        m_on_destroyed();
}

bool DockPanel::on_page_message(const std::string& kind, const nlohmann::json& data)
{
    if (kind == "message") {
        if (m_on_message)
            m_on_message(data);
        return true;
    }
    if (kind == "close") {
        request_close();
        return true;
    }
    return false;
}

void DockPanel::push_message(const nlohmann::json& data)
{
    if (!m_closing)
        post_to_page(data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void DockPanel::fire_close()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_on_close) {
        CloseHandler on_close = std::move(m_on_close);
        m_on_close            = nullptr;
        on_close();
    }
}

void DockPanel::request_close()
{
    if (m_closing)
        return;
    fire_close();
    // A page-requested close arrives inside the web view's script callback, so destroy later; another
    // close path may have destroyed the panel by then.
    wxWeakRef<DockPanel> self(this);
    CallAfter([self]() {
        if (self)
            self->remove_pane();
    });
}

void DockPanel::destroy_silently()
{
    m_closing  = true;
    m_on_close = nullptr;
    remove_pane();
}

void DockPanel::remove_pane()
{
    if (Plater* plater = wxGetApp().plater())
        plater->remove_dock_pane(this);
    else
        Destroy();
}

}} // namespace Slic3r::GUI
