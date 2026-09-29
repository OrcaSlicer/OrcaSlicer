#pragma once

#include "WebPanel.hpp"

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

// Stable across sessions so the saved layout finds the pane; free of wxAuiManager layout delimiters.
std::string plugin_pane_name(const std::string& plugin_key, const std::string& title);

// A WebPanel docked in the Plater, on the plugin-window bridge minus submit. It can be destroyed
// without the GIL, so its hooks must not capture pybind11 objects.
class DockPanel : public WebPanel
{
public:
    using MessageHandler = std::function<void(const nlohmann::json& data)>;
    using CloseHandler   = std::function<void()>;

    // on_close fires once, on a user or page close. on_destroyed runs on every destruction and must
    // touch host-side state only.
    DockPanel(wxWindow*          parent,
              const std::string& html,
              MessageHandler     on_message,
              CloseHandler       on_close,
              CloseHandler       on_destroyed);
    ~DockPanel() override;

    // Main thread only.
    void push_message(const nlohmann::json& data);
    // Fires on_close, then removes the pane.
    void request_close();
    // Removes the pane without on_close, for plugin unload. Destroys at once: unload always comes from
    // the host, never from this panel's own callbacks.
    void destroy_silently();
    // Fires on_close at most once; also run by the pane's own close button.
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
