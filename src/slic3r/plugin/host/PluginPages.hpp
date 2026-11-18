#pragma once

#include <slic3r/GUI/LazyPage.hpp>
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
#include <wx/string.h>
#include <wx/webview.h>

class wxWebViewEvent;
class wxWindow;

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

protected:
    std::optional<std::string> page_html() override;
    bool on_page_message(const std::string& kind, const nlohmann::json& data) override;

private:
    void on_new_window(wxWebViewEvent& event);

    std::shared_ptr<PagesPluginCapability> m_cap;
    std::shared_ptr<std::atomic<PluginPage*>> m_lifetime;
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

    void relayout();

    void select_page(const PluginCapabilityId& id);

    // How many of the tabs, in order, fit in `room`, leaving room for the overflow button when not
    // all of them fit.
    static int fitting_tabs(int room, const std::vector<int>& widths, int overflow_width);
    // The plugin pages shown as tabs: the first visible_slots - 1 in `order`, then the page the user
    // picked from the overflow menu when it is in the overflow, else the first page there.
    static std::vector<PluginCapabilityId> plugin_tab_ids(const std::vector<PluginCapabilityId>& order, int visible_slots,
                                                          const std::optional<PluginCapabilityId>& picked);

private:
    std::shared_ptr<PagesPluginCapability> get_pages_cap(const PluginCapabilityId& id, bool is_enabled) const;
    bool create_page(const PluginCapabilityId& id);
    void remove_page(const PluginCapabilityId& id);

    void show_overflow_menu();
    // How many plugin tabs fit in the tab bar next to the built-in tabs.
    int fitting_page_count() const;
    static wxString page_tab_id(const PluginCapabilityId& id);

    struct Page
    {
        GUI::LazyPage<PluginPage>* page{nullptr};
        wxBitmap                   icon;
    };

    std::map<PluginCapabilityId, Page> m_pages;
    std::vector<PluginCapabilityId> m_order;
    Notebook* m_parent{nullptr};

    int m_visible_slots{1};

    std::optional<PluginCapabilityId> m_swapped_in_id; // picked by the user, never a default; kept until another pick or its page is removed
    wxWindow* m_overflow_button{nullptr};
};

} // namespace Slic3r
