#pragma once

#include <slic3r/GUI/WebPanel.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/webview.h>

class Notebook;

namespace Slic3r {

class PluginPage : public GUI::WebPanel
{
public:
    PluginPage(wxWindow* parent, std::shared_ptr<PagesPluginCapability> capability);
    ~PluginPage() override;

    PluginPage() = delete;

    void detach_capability();
    void push_message(const std::string& message);
    void set_icon(const wxBitmap& icon) { m_icon = icon; }
    const wxBitmap& icon() const { return m_icon; }

protected:
    std::optional<std::string> page_html() override;
    bool on_page_message(const std::string& kind, const nlohmann::json& data) override;

private:
    void on_new_window(wxWebViewEvent& event);

    std::shared_ptr<PagesPluginCapability> m_cap;
    std::shared_ptr<std::atomic<PluginPage*>> m_lifetime;
    wxBitmap m_icon;
};

class PluginPages
{
public:
    PluginPages() = default;
    ~PluginPages();

    PluginPages(const PluginPages&)            = delete;
    PluginPages& operator=(const PluginPages&) = delete;

    void initialize(Notebook* parent);
    void shutdown();

    void on_cap_register(const PluginCapabilityId& id);
    void on_cap_deregister(const PluginCapabilityId& id);
    void on_plugin_register(const std::string& plugin_key);
    void on_plugin_deregister(const std::string& plugin_key);

    void set_visible_page_count(int count);

    void relayout();

private:
    std::shared_ptr<PagesPluginCapability> get_pages_cap(const PluginCapabilityId& id, bool is_enabled) const;
    bool create_page(const PluginCapabilityId& id);
    void remove_page(const PluginCapabilityId& id);

    void show_overflow_menu();
    static wxString page_tab_id(const PluginCapabilityId& id);

    std::map<PluginCapabilityId, PluginPage*> m_pages;
    std::vector<PluginCapabilityId> m_order;
    Notebook* m_parent{nullptr};

    int m_visible_page_count{0};

    std::optional<PluginCapabilityId> m_swapped_in_id;
    wxWindow* m_overflow_button{nullptr};
};

} // namespace Slic3r
