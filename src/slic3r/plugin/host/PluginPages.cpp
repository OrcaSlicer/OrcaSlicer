#include "PluginPages.hpp"
#include "PluginPageOrder.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
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
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/sizer.h>

#include <utility>
#include <wx/webview.h>
#include <wx/string.h>

namespace Slic3r {
namespace {

constexpr char PLUGIN_PAGES_ORDER_SETTING[] = "plugin_pages_order";

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

    m_visible_page_count = GUI::wxGetApp().app_config->get_plugin_pages_visible_count();
    const std::string saved_order = GUI::wxGetApp().app_config->get(PLUGIN_PAGES_ORDER_SETTING);
    if (const auto parsed = parse_plugin_page_order(saved_order))
        m_preferred_order = *parsed;
    else {
        m_preferred_order.clear();
        if (!saved_order.empty())
            BOOST_LOG_TRIVIAL(warning) << "Ignoring malformed saved plugin page order";
    }
    m_parent->Bind(wxCUSTOMEVT_NOTEBOOK_TAB_REORDER, &PluginPages::on_tab_reorder, this);

    for (const auto& capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Pages)) {
        if (capability)
            create_page(capability->identity());
    }
    relayout();
}

void PluginPages::shutdown()
{
    if (m_parent != nullptr) {
        m_parent->Unbind(wxCUSTOMEVT_NOTEBOOK_TAB_REORDER, &PluginPages::on_tab_reorder, this);
        remove_notebook_pages();
        if (m_overflow_button != nullptr) {
            m_parent->SetOverflowButton(nullptr);
            m_overflow_button->Destroy();
            m_overflow_button = nullptr;
        }
    }
    m_parent = nullptr;
    m_swapped_in_id.reset();
    // No relayout during teardown: it could insert a hidden page only to remove it next.
    for (const PluginCapabilityId& id : std::vector<PluginCapabilityId>(m_order))
        remove_page(id);
}

void PluginPages::set_visible_page_count(int count)
{
    const int clamped = std::clamp(count, PLUGIN_PAGES_VISIBLE_COUNT_MIN, PLUGIN_PAGES_VISIBLE_COUNT_MAX);
    if (clamped == m_visible_page_count)
        return;

    m_visible_page_count = clamped;
    relayout();
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
    if (std::find(m_preferred_order.begin(), m_preferred_order.end(), id) == m_preferred_order.end())
        m_preferred_order.push_back(id);
    order_plugin_pages(m_order, m_preferred_order);
    // A late registration at a saved position can push the active page into overflow.
    const int visible_slots = std::max(1, m_visible_page_count);
    if (static_cast<int>(m_order.size()) > visible_slots) {
        wxWindow* selected_page = m_parent->GetCurrentPage();
        for (auto it = m_order.begin() + (visible_slots - 1); it != m_order.end(); ++it)
            if (m_pages.at(*it).page == selected_page) {
                m_swapped_in_id = *it;
                break;
            }
    }
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

void PluginPages::remove_notebook_pages()
{
    if (m_parent == nullptr)
        return;

    // Notebook order can differ from m_order after a drag or overflow selection.
    // Removing left to right never selects/builds an unvisited plugin page to the left.
    for (size_t index = 0; index < m_parent->GetPageCount();) {
        wxWindow* page = m_parent->GetPage(index);
        const bool is_plugin_page = std::any_of(m_pages.begin(), m_pages.end(), [page](const auto& entry) {
            return entry.second.page == page;
        });
        if (is_plugin_page)
            m_parent->RemovePage(index);
        else
            ++index;
    }
}

void PluginPages::on_tab_reorder(wxCommandEvent& event)
{
    if (m_parent == nullptr || event.GetEventObject() != m_parent)
        return;

    const int source_index = event.GetInt();
    const long target_index = event.GetExtraLong();
    if (source_index < 0 || target_index < 0 ||
        static_cast<size_t>(source_index) >= m_parent->GetPageCount() ||
        static_cast<size_t>(target_index) >= m_parent->GetPageCount())
        return;
    if (event.GetString() != "before" && event.GetString() != "after")
        return;

    wxWindow* source_page = m_parent->GetPage(static_cast<size_t>(source_index));
    wxWindow* target_page = m_parent->GetPage(static_cast<size_t>(target_index));
    const auto source = std::find_if(m_pages.begin(), m_pages.end(), [source_page](const auto& entry) {
        return entry.second.page == source_page;
    });
    const auto target = std::find_if(m_pages.begin(), m_pages.end(), [target_page](const auto& entry) {
        return entry.second.page == target_page;
    });
    // Only actual, active plugin pages participate; built-in tabs are never movable.
    if (source == m_pages.end() || target == m_pages.end())
        return;

    std::optional<PluginCapabilityId> selected;
    wxWindow* selected_page = m_parent->GetCurrentPage();
    for (const auto& [id, entry] : m_pages)
        if (entry.page == selected_page)
            selected = id;
    if (!move_plugin_page(m_preferred_order, source->first, target->first, event.GetString() == "before"))
        return;

    order_plugin_pages(m_order, m_preferred_order);
    // Preserve the active page ahead of keeping the dragged page visible in overflow.
    const int visible_slots = std::max(1, m_visible_page_count);
    if (static_cast<int>(m_order.size()) > visible_slots) {
        const auto overflow_begin = m_order.begin() + (visible_slots - 1);
        if (selected && std::find(overflow_begin, m_order.end(), *selected) != m_order.end())
            m_swapped_in_id = *selected;
        else if (std::find(overflow_begin, m_order.end(), source->first) != m_order.end())
            m_swapped_in_id = source->first;
    }
    relayout();
    // AppConfig marks the value dirty; the usual idle/exit save persists this user action.
    GUI::wxGetApp().app_config->set(PLUGIN_PAGES_ORDER_SETTING, serialize_plugin_page_order(m_preferred_order));
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

    const int visible_slots = std::max(1, m_visible_page_count);
    const bool need_overflow = static_cast<int>(m_order.size()) > visible_slots;

    // Every visible slot is a normal, individual tab hosting its own page. When there's
    // overflow, the last slot's page is swappable via m_overflow_button/show_overflow_menu()
    // rather than being a fixed page — m_swapped_in_id tracks which one currently sits there.
    std::vector<PluginCapabilityId> tab_ids;
    if (!need_overflow) {
        tab_ids = m_order;
        m_swapped_in_id.reset();
    } else {
        const auto overflow_begin = m_order.begin() + (visible_slots - 1);
        tab_ids.assign(m_order.begin(), overflow_begin);

        if (!m_swapped_in_id || std::find(overflow_begin, m_order.end(), *m_swapped_in_id) == m_order.end())
            m_swapped_in_id = *overflow_begin;
        tab_ids.push_back(*m_swapped_in_id);
    }

    // MainFrame::show_device() relayouts on every printer change and most of those change
    // nothing, so only touch the notebook when the trailing slots don't already spell out
    // tab_ids — a rebuild destroys and recreates every tab button and rasterizes every icon.
    const size_t page_count = m_parent->GetPageCount();
    bool same_membership = page_count >= tab_ids.size();
    for (const auto& [id, entry] : m_pages) {
        if (!same_membership)
            break;
        const bool wanted = std::find(tab_ids.begin(), tab_ids.end(), id) != tab_ids.end();
        same_membership = entry.page->in_book() == wanted;
    }
    bool up_to_date = same_membership;
    for (size_t i = 0; up_to_date && i < tab_ids.size(); ++i)
        up_to_date = m_parent->GetPage(page_count - tab_ids.size() + i) == m_pages.at(tab_ids[i]).page;

    bool reordered = false;
    if (!up_to_date && same_membership) {
        // A permutation can move the existing buttons/pages without selection events,
        // flicker or rebuilding a browser. Membership changes still use the normal path.
        reordered = true;
        for (size_t i = 0; i < tab_ids.size(); ++i) {
            const int source = m_parent->FindPage(m_pages.at(tab_ids[i]).page);
            const size_t destination = page_count - tab_ids.size() + i;
            if (source == wxNOT_FOUND || (static_cast<size_t>(source) != destination &&
                !m_parent->MovePage(static_cast<size_t>(source), destination))) {
                reordered = false;
                break;
            }
        }
    }

    if (!up_to_date && !reordered) {
        wxWindow* selected_page = m_parent->GetCurrentPage();

        remove_notebook_pages();

        for (const auto& id : tab_ids) {
            const Page& entry = m_pages.at(id);
            m_parent->InsertPage(m_parent->GetPageCount(), page_tab_id(id), entry.page, wxString::FromUTF8(id.name), "",
                                 false, entry.icon);
        }

        if (const int selection = m_parent->FindPage(selected_page); selection != wxNOT_FOUND)
            m_parent->SetSelection(static_cast<size_t>(selection));
    }

    const bool overflow_changed = need_overflow != (m_overflow_button != nullptr);
    if (need_overflow) {
        if (m_overflow_button == nullptr) {
            auto* btn = new Button(m_parent->GetBtnsListCtrl(), wxString(), "sidebutton_dropdown", wxNO_BORDER, 14);
            btn->SetName(_L("More"));
            btn->SetToolTip(_L("More"));
            btn->SetCornerRadius(0);
            btn->SetBackgroundColor(StateColor(
                std::pair{wxColour(107, 107, 107), int(StateColor::Hovered)},
                std::pair{wxColour(59, 68, 70), int(StateColor::Normal)}));
            btn->SetTextColor(StateColor(wxColour(254, 254, 254)));
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

void PluginPages::select_page(const PluginCapabilityId& id)
{
    if (m_parent == nullptr || m_pages.find(id) == m_pages.end())
        return;

    // Swap the page into the visible slot first when it lives behind the overflow menu.
    m_swapped_in_id = id;
    relayout();
    if (const int selection = m_parent->FindPage(m_pages.at(id).page); selection != wxNOT_FOUND)
        m_parent->SetSelection(static_cast<size_t>(selection));
}

void PluginPages::show_overflow_menu()
{
    const int visible_slots = std::max(1, m_visible_page_count);
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
        select_page(overflow_ids[index]);
    });
    m_overflow_button->PopupMenu(&menu);
}

} // namespace Slic3r
