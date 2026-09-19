#pragma once

#include "PluginWebPanel.hpp"

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

// Name of a plugin's pane in the Plater's dock manager. It stays the same across sessions, so the
// saved window layout can put the pane back, and it never contains a wxAuiManager layout delimiter.
std::string plugin_pane_name(const std::string& plugin_key, const std::string& title);

// A PluginWebPanel docked in the Plater's dock manager, using the same window.orca bridge as
// PluginWebDialog (without submit, which only a plugin window handles). Python-agnostic for the same
// reason: it can be destroyed on the main thread without the GIL, so its hooks must not capture
// pybind11 objects.
class PluginDockPanel : public PluginWebPanel
{
public:
    using MessageHandler = std::function<void(const nlohmann::json& data)>;
    using CloseHandler   = std::function<void()>;

    // on_close fires once, on a user or page initiated close. on_destroyed runs from the destructor
    // on every path and must touch host-side state only.
    PluginDockPanel(wxWindow*          parent,
                    const std::string& html,
                    MessageHandler     on_message,
                    CloseHandler       on_close,
                    CloseHandler       on_destroyed);
    ~PluginDockPanel() override;

    // Main thread only.
    void push_message(const nlohmann::json& data);
    // Fires on_close, then removes the pane.
    void request_close();
    // Removes the pane without firing on_close, for plugin unload. Unlike request_close() it destroys
    // the panel at once, which is safe because nothing a plugin page can call unloads a plugin: unload
    // comes from the host (Plugins dialog, install, logout), never from this panel's own callbacks.
    void destroy_for_plugin();
    // Fires on_close at most once. Runs when the pane's own close button is used, through the handler
    // the panel is registered with.
    void fire_close();

protected:
    std::optional<std::string> page_html() override { return m_html; }
    bool on_page_message(const std::string& kind, const nlohmann::json& data) override;

private:
    void remove_pane();

    std::string    m_html;
    bool           m_closing{false};
    MessageHandler m_on_message;
    CloseHandler   m_on_close;
    CloseHandler   m_on_destroyed;
};

}} // namespace Slic3r::GUI
