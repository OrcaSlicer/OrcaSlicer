#include "PluginPages.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <algorithm>

#include <atomic>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <memory>
#include <exception>
#include <cstddef>
#include <nlohmann/json.hpp>

#include "slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp"
#include "slic3r/GUI/WebPanel.hpp"
#include <optional>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/GUI/LazyPage.hpp"
#include <stdexcept>
#include <string>
#include <wx/app.h>
#include <vector>
#include <wx/bookctrl.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/sizer.h>

#include <utility>
#include <wx/webview.h>
#include <wx/string.h>

namespace Slic3r {
namespace {

constexpr char PLUGIN_PAGE_BRIDGE_JS[] = R"JS(
(function () {
  if (window.top !== window.self) return;
  if (window.orca) return;
  var handlers = [];
  function deliver(payload, attempts) {
    try {
      if (window.wx && typeof window.wx.postMessage === 'function') {
        window.wx.postMessage(payload);
        return;
      }
    } catch (e) { /* retry while the native handler is being registered */ }
    if (attempts < 100)
      window.setTimeout(function () { deliver(payload, attempts + 1); }, 25);
  }
  function send(data) {
    deliver(JSON.stringify({
      channel: 'orca', kind: 'message', data: (data === undefined ? null : data)
    }), 0);
  }
  window.orca = {
    postMessage: function (data) { send(data); },
    onMessage: function (callback) {
      if (typeof callback === 'function') handlers.push(callback);
    }
  };
  window.__orcaDispatch = function (payload) {
    var data = payload ? payload.data : null;
    for (var i = 0; i < handlers.length; i++) {
      try { handlers[i](data); } catch (e) {}
    }
  };
})();
)JS";

} // namespace

PluginPage::PluginPage(wxWindow* parent, std::shared_ptr<PagesPluginCapability> capability)
    : GUI::WebPanel(parent, PLUGIN_PAGE_BRIDGE_JS)
    , m_cap(std::move(capability))
    , m_lifetime(std::make_shared<std::atomic<PluginPage*>>(this))
{
    browser()->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PluginPage::on_new_window, this);

    const std::shared_ptr<std::atomic<PluginPage*>> lifetime = m_lifetime;
    m_cap->set_message_sender([lifetime](const std::string& message) {
        if (wxTheApp == nullptr)
            return;

        GUI::wxGetApp().CallAfter([lifetime, message] {
            if (PluginPage* page = lifetime->load(std::memory_order_acquire))
                page->push_message(message);
        });
    });

}

PluginPage::~PluginPage()
{
    detach_capability();
    if (m_lifetime)
        m_lifetime->store(nullptr, std::memory_order_release);
}

void PluginPage::detach_capability()
{
    if (m_lifetime)
        m_lifetime->store(nullptr, std::memory_order_release);
    if (m_cap)
        m_cap->clear_message_sender();
    m_cap.reset();
}

std::optional<std::string> PluginPage::page_html()
{
    if (m_cap == nullptr)
        return std::nullopt;

    try {
        return m_cap->get_ui();
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load plugin page '" << m_cap->name() << "': " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load plugin page '" << m_cap->name() << "'";
    }
    detach_capability();
    return std::nullopt;
}

void PluginPage::on_new_window(wxWebViewEvent& event)
{
    const wxString url = event.GetURL();
    if (!url.empty())
        browser()->LoadURL(url);
    event.Veto();
}

bool PluginPage::on_page_message(const std::string& kind, const nlohmann::json& data)
{
    if (kind != "message")
        return false;
    if (!m_cap)
        return true;

    try {
        m_cap->on_message(data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << m_cap->name() << "': " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << m_cap->name() << "'";
    }
    return true;
}

void PluginPage::push_message(const std::string& message)
{
    // PagesPluginCapability::post_message() already dumps JSON, so accept it as-is; only a
    // non-JSON payload needs wrapping as a string literal.
    post_to_page(nlohmann::json::accept(message)
                     ? message
                     : nlohmann::json(message).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

PluginPages::~PluginPages()
{
    shutdown();
}

void PluginPages::initialize(Notebook* parent)
{
    shutdown();
    m_parent = parent;
    if (m_parent == nullptr)
        return;

    for (const auto& capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Pages)) {
        if (capability)
            create_page(capability->identity());
    }
    relayout();
}

void PluginPages::shutdown()
{
    // Removing the selected tab selects the tab to its left. m_order is in tab order, so the plugin
    // tabs to its left are gone by then and that is a built-in tab, not an unbuilt plugin page that
    // is removed next and would be built only to be destroyed.
    for (const PluginCapabilityId& id : std::vector<PluginCapabilityId>(m_order))
        remove_page(id);
    m_parent = nullptr;
}

std::shared_ptr<PagesPluginCapability> PluginPages::get_pages_cap(const PluginCapabilityId& id, bool is_enabled) const
{
    auto capability = PluginManager::instance().get_plugin_capability(id, /*only_enabled=*/false);
    if (!capability || capability->is_enabled() != is_enabled || capability->type() != PluginCapabilityType::Pages)
        return nullptr;

    return std::dynamic_pointer_cast<PagesPluginCapability>(capability);
}

bool PluginPages::create_page(const PluginCapabilityId& id)
{
    if (m_pages.find(id) != m_pages.end())
        return false;

    auto capability = get_pages_cap(id, true);
    if (!capability)
        return false;

    std::string icon;
    try {
        icon = capability->get_icon();
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to get icon for plugin " << id.plugin_key << ": " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to get icon for plugin " << id.plugin_key;
    }

    wxBitmap bitmap;
    if (!icon.empty()) {
        try {
            boost::filesystem::path icon_path(icon);
            const std::string extension = icon_path.extension().string();
            if (extension == ".svg" || extension == ".png")
                icon_path.replace_extension();

            bitmap = create_scaled_bitmap(icon_path.string(), m_parent, 20);
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to load icon for plugin " << id.plugin_key << ": " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to load icon for plugin " << id.plugin_key;
        }
    }

    // Built the first time its tab is shown, so a language switch never creates the page's browser
    // while the main window is being rebuilt. Never prebuilt: the idle queue cannot drop a page
    // that remove_page() destroys.
    auto* page = new GUI::LazyPage<PluginPage>(m_parent, GUI::into_u8(page_tab_id(id)), -1,
                                               [capability = std::move(capability)](wxWindow* parent) {
                                                   return new PluginPage(parent, capability);
                                               });

    m_pages.emplace(id, Page{page, bitmap});
    m_order.push_back(id);
    return true;
}

void PluginPages::on_cap_register(const PluginCapabilityId& id)
{
    if (m_parent == nullptr)
        return;

    if (create_page(id))
        relayout();
}

void PluginPages::on_cap_deregister(const PluginCapabilityId& id)
{
    remove_page(id);
}

void PluginPages::on_plugin_register(const std::string& plugin_key)
{
    for (const auto& capability : PluginManager::instance().get_plugin_capabilities(plugin_key, PluginCapabilityType::Pages)) {
        if (capability)
            on_cap_register(capability->identity());
    }
}

void PluginPages::on_plugin_deregister(const std::string& plugin_key)
{
    // In tab order, as in shutdown().
    for (const PluginCapabilityId& id : std::vector<PluginCapabilityId>(m_order))
        if (id.plugin_key == plugin_key)
            remove_page(id);
}

void PluginPages::remove_page(const PluginCapabilityId& id)
{
    auto it = m_pages.find(id);
    if (it == m_pages.end())
        return;

    GUI::LazyPage<PluginPage>* page = it->second.page;
    // Only a built page has installed a message sender on the capability.
    if (PluginPage* built = page->get())
        built->detach_capability();

    m_pages.erase(it);
    if (m_swapped_in_id == id)
        m_swapped_in_id.reset();
    m_order.erase(std::remove(m_order.begin(), m_order.end(), id), m_order.end());

    const int idx = m_parent != nullptr ? m_parent->FindPage(page) : wxNOT_FOUND;
    if (idx != wxNOT_FOUND)
        m_parent->RemovePage(idx);

    relayout();
    page->Destroy();
}

wxString PluginPages::page_tab_id(const PluginCapabilityId& id)
{
    return wxString::FromUTF8("plugin." + id.plugin_key + "." + id.name);
}

void PluginPages::relayout()
{
    if (m_parent == nullptr)
        return;

    m_order.erase(std::remove_if(m_order.begin(), m_order.end(),
        [this](const PluginCapabilityId& id) {
            const bool orphaned = m_pages.find(id) == m_pages.end();
            if (orphaned)
                BOOST_LOG_TRIVIAL(error) << "PluginPages::relayout: '" << id.name << "' was in m_order but not m_pages, dropping";
            return orphaned;
        }),
        m_order.end());

    // In the order the user dragged the tabs to; pages never placed keep registration order, last.
    std::stable_sort(m_order.begin(), m_order.end(), [this](const PluginCapabilityId& a, const PluginCapabilityId& b) {
        return m_parent->GetPageRank(page_tab_id(a)) < m_parent->GetPageRank(page_tab_id(b));
    });

    m_visible_slots = std::max(1, fitting_page_count());
    const int visible_slots = m_visible_slots;
    const bool need_overflow = static_cast<int>(m_order.size()) > visible_slots;

    // When there's overflow, the last slot shows the page the user picked from the overflow menu
    // (or through select_page()) while it is in the overflow, else the first page there. The pick is
    // kept while it shows in a normal slot, e.g. on a tab without Slice/Print where all pages fit,
    // and is dropped only for another pick or when its page is removed.
    const std::vector<PluginCapabilityId> tab_ids = plugin_tab_ids(m_order, visible_slots, m_swapped_in_id);

    // MainFrame::show_device() relayouts on every printer change and most of those change
    // nothing, so only touch the notebook when the pages in it are not already tab_ids — a
    // rebuild destroys and recreates every tab button and rasterizes every icon. Where each tab
    // sits is the notebook's page order, which the user can mix with the built-in tabs.
    bool up_to_date = true;
    for (const auto& [id, entry] : m_pages) {
        const bool wanted = std::find(tab_ids.begin(), tab_ids.end(), id) != tab_ids.end();
        up_to_date = up_to_date && entry.page->in_book() == wanted;
    }

    if (!up_to_date) {
        const wxString id_to_reselect = m_parent->GetSelectedPageName();

        // In tab order, as in shutdown().
        for (const auto& id : m_order) {
            const int idx = m_parent->FindPage(m_pages.at(id).page);
            if (idx != wxNOT_FOUND) {
                m_parent->RemovePage(idx);
                // Removing the selected page leaves it shown, and a page that is not
                // inserted again would stay drawn over the next selected one.
                m_pages.at(id).page->Hide();
            }
        }

        // Notebook::InsertPage() moves a page with a place in the saved order to that place.
        for (const auto& id : tab_ids) {
            const Page& entry = m_pages.at(id);
            m_parent->InsertPage(m_parent->GetPageCount(), page_tab_id(id), entry.page, wxString::FromUTF8(id.name), "",
                                 false, entry.icon);
        }

        if (!id_to_reselect.empty())
            m_parent->SelectPageByName(id_to_reselect);
    }

    const bool overflow_changed = need_overflow != (m_overflow_button != nullptr);
    if (need_overflow) {
        if (m_overflow_button == nullptr) {
            auto* btn = new Button(m_parent->GetBtnsListCtrl(), wxString(L"\u25BE"), wxString(), wxNO_BORDER);
            btn->SetCornerRadius(0);
            const int em = em_unit(m_parent);
            btn->SetMinSize({40 * em / 10, 36 * em / 10});
            btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_overflow_menu(); });
            GUI::wxGetApp().UpdateDarkUI(btn);
            m_overflow_button = btn;
        }
        m_parent->SetOverflowButton(m_overflow_button);
    } else if (m_overflow_button != nullptr) {
        m_parent->SetOverflowButton(nullptr);
        m_overflow_button->Destroy();
        m_overflow_button = nullptr;
    }

    // The tab strip changed width; compact the labels again so the side tools stay visible.
    if ((!up_to_date || overflow_changed) && !GUI::wxGetApp().is_closing())
        if (GUI::MainFrame* mainframe = GUI::wxGetApp().mainframe)
            mainframe->fit_tab_labels();
}

int PluginPages::fitting_page_count() const
{
    // Tabs are counted with their labels shown: the count must not depend on whether
    // MainFrame::fit_tab_labels() has compacted them to icons.
    ButtonsListCtrl* bar  = m_parent->GetBtnsListCtrl();
    int              room = bar->GetTabsAreaWidth();
    for (size_t i = 0; i < m_parent->GetPageCount(); ++i)
        if (!Notebook::IsPluginPageId(m_parent->GetPageName(i)))
            room -= bar->GetFullTabWidth(m_parent->GetPageLabel(i));

    std::vector<int> widths;
    for (const PluginCapabilityId& id : m_order)
        widths.push_back(bar->GetFullTabWidth(wxString::FromUTF8(id.name)));
    // The overflow button takes an icon-only tab's width.
    return fitting_tabs(room, widths, bar->GetTabWidth(true));
}

std::vector<PluginCapabilityId> PluginPages::plugin_tab_ids(const std::vector<PluginCapabilityId>& order, int visible_slots,
                                                            const std::optional<PluginCapabilityId>& picked)
{
    if (static_cast<int>(order.size()) <= visible_slots)
        return order;

    const auto overflow_begin = order.begin() + (std::max(1, visible_slots) - 1);
    std::vector<PluginCapabilityId> ids(order.begin(), overflow_begin);
    ids.push_back(picked && std::find(overflow_begin, order.end(), *picked) != order.end() ? *picked : *overflow_begin);
    return ids;
}

int PluginPages::fitting_tabs(int room, const std::vector<int>& widths, int overflow_width)
{
    int used = 0;
    for (const int width : widths)
        used += width;
    if (used <= room)
        return static_cast<int>(widths.size());

    int count = 0;
    for (used = overflow_width; count < static_cast<int>(widths.size()) && used + widths[count] <= room; ++count)
        used += widths[count];
    return count;
}

void PluginPages::select_page(const PluginCapabilityId& id)
{
    if (m_parent == nullptr || m_pages.find(id) == m_pages.end())
        return;

    // Swap the page into the visible slot first when it lives behind the overflow menu.
    m_swapped_in_id = id;
    relayout();
    m_parent->SelectPageByName(page_tab_id(id));
}

void PluginPages::show_overflow_menu()
{
    const int visible_slots = m_visible_slots;
    if (m_overflow_button == nullptr || static_cast<int>(m_order.size()) <= visible_slots)
        return;

    const std::vector<PluginCapabilityId> overflow_ids(m_order.begin() + (visible_slots - 1), m_order.end());

    wxMenu menu;
    for (size_t i = 0; i < overflow_ids.size(); ++i)
        menu.AppendRadioItem(static_cast<int>(wxID_HIGHEST + 1 + i), wxString::FromUTF8(overflow_ids[i].name));
    if (m_swapped_in_id) {
        const auto it = std::find(overflow_ids.begin(), overflow_ids.end(), *m_swapped_in_id);
        if (it != overflow_ids.end())
            menu.Check(static_cast<int>(wxID_HIGHEST + 1 + (it - overflow_ids.begin())), true);
    }

    menu.Bind(wxEVT_MENU, [this, overflow_ids](wxCommandEvent& evt) {
        const size_t index = static_cast<size_t>(evt.GetId() - (wxID_HIGHEST + 1));
        if (index >= overflow_ids.size())
            return;
        const PluginCapabilityId id = overflow_ids[index];
        m_swapped_in_id = id;
        relayout();
        m_parent->SelectPageByName(page_tab_id(id));
    });
    m_overflow_button->PopupMenu(&menu);
}

} // namespace Slic3r
