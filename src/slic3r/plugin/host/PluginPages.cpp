#include "PluginPages.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/Widgets/WebViewHostDialog.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <libslic3r/Utils.hpp>

#include <algorithm>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <wx/bookctrl.h>
#include <wx/menu.h>
#include <wx/sizer.h>

#include <utility>

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
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_cap(std::move(capability))
    , m_lifetime(std::make_shared<std::atomic<PluginPage*>>(this))
{
    auto* topsizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(topsizer);

    m_browser = WebView::CreateWebView(this, bootstrap_url());
    if (m_browser == nullptr) {
        wxLogError("Could not initialize plugin page web view");
        return;
    }

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PluginPage::on_bootstrap_event, this);
    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PluginPage::on_bootstrap_event, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PluginPage::on_new_window, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PluginPage::on_script_message, this);
    m_browser->AddUserScript(wxString::FromUTF8(GUI::WebViewHostDialog::theme_user_script()));
    m_browser->AddUserScript(wxString::FromUTF8(GUI::WebViewHostDialog::plugin_defaults_user_script()));
    m_browser->AddUserScript(PLUGIN_PAGE_BRIDGE_JS);

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

wxString PluginPage::web_base_url() const
{
    const auto path = (boost::filesystem::path(resources_dir()) / "web").make_preferred().string();
    return wxString("file://") + GUI::from_u8(path) + "/";
}

wxString PluginPage::bootstrap_url() const
{
    const auto path = (boost::filesystem::path(resources_dir()) / "web/dialog/PluginWebDialog/blank.html").make_preferred().string();
    return wxString("file://") + GUI::from_u8(path);
}

void PluginPage::on_bootstrap_event(wxWebViewEvent& event)
{
    load_plugin_content();
    event.Skip();
}

void PluginPage::load_plugin_content()
{
    if (m_content_loaded || m_browser == nullptr || m_cap == nullptr)
        return;

    m_content_loaded = true;
    try {
        m_browser->SetPage(wxString::FromUTF8(m_cap->get_ui()), web_base_url());
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load plugin page '" << m_cap->name() << "': " << error.what();
        detach_capability();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load plugin page '" << m_cap->name() << "'";
        detach_capability();
    }
}

void PluginPage::on_new_window(wxWebViewEvent& event)
{
    const wxString url = event.GetURL();
    if (!url.empty() && m_browser != nullptr)
        m_browser->LoadURL(url);
    event.Veto();
}

void PluginPage::on_script_message(wxWebViewEvent& event)
{
    if (!m_cap)
        return;

    const wxString payload = event.GetString();
    nlohmann::json root    = nlohmann::json::parse(payload.utf8_string(), nullptr, false);
    if (root.is_discarded() || root.value("channel", std::string()) != "orca" ||
        root.value("kind", std::string()) != "message")
        return;

    const auto data = root.find("data");
    try {
        m_cap->on_message(data == root.end()
                              ? "null"
                              : data->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << m_cap->name() << "': " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << m_cap->name() << "'";
    }
}

void PluginPage::push_message(const std::string& message)
{
    if (m_browser == nullptr)
        return;

    // PagesPluginCapability::post_message() already dumps JSON, so accept it as-is; only a
    // non-JSON payload needs wrapping as a string literal.
    const std::string payload = nlohmann::json::accept(message)
                                    ? message
                                    : nlohmann::json(message).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    WebView::RunScript(m_browser, wxString::Format(
        "(function dispatch(payload, attempts) {\n"
        "  if (typeof window.__orcaDispatch === 'function') { window.__orcaDispatch(payload); return; }\n"
        "  if (attempts < 100) window.setTimeout(function() { dispatch(payload, attempts + 1); }, 25);\n"
        "})({data: %s}, 0);",
        wxString::FromUTF8(payload)));
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

    for (const auto& capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Pages)) {
        if (capability)
            create_page(capability->identity());
    }
    relayout();
}

void PluginPages::shutdown()
{
    while (!m_pages.empty())
        remove_page(m_pages.begin()->first);
    m_parent = nullptr;
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

    auto* page = new PluginPage(m_parent, std::move(capability));
    if (!page->is_valid()) {
        page->Destroy();
        return false;
    }

    if (!icon.empty()) {
        try {
            boost::filesystem::path icon_path(icon);
            const std::string extension = icon_path.extension().string();
            if (extension == ".svg" || extension == ".png")
                icon_path.replace_extension();

            page->set_icon(create_scaled_bitmap(icon_path.string(), m_parent, 20));
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to load icon for plugin " << id.plugin_key << ": " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to load icon for plugin " << id.plugin_key;
        }
    }

    m_pages.emplace(id, page);
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
    for (auto it = m_pages.begin(); it != m_pages.end();) {
        if (it->first.plugin_key != plugin_key) {
            ++it;
            continue;
        }

        const PluginCapabilityId id = it->first;
        ++it;
        remove_page(id);
    }
}

void PluginPages::remove_page(const PluginCapabilityId& id)
{
    auto it = m_pages.find(id);
    if (it == m_pages.end())
        return;

    PluginPage* page = it->second;
    page->detach_capability();

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
    bool up_to_date = page_count >= tab_ids.size();
    for (size_t i = 0; up_to_date && i < tab_ids.size(); ++i)
        up_to_date = m_parent->GetPageName(page_count - tab_ids.size() + i) == page_tab_id(tab_ids[i]);
    for (const auto& [id, page] : m_pages) {
        if (!up_to_date)
            break;
        const bool wanted = std::find(tab_ids.begin(), tab_ids.end(), id) != tab_ids.end();
        up_to_date = (m_parent->FindPage(page) != wxNOT_FOUND) == wanted;
    }

    if (!up_to_date) {
        const wxString id_to_reselect = m_parent->GetSelectedPageName();

        for (const auto& [id, page] : m_pages) {
            const int idx = m_parent->FindPage(page);
            if (idx != wxNOT_FOUND)
                m_parent->RemovePage(idx);
        }

        for (const auto& id : tab_ids) {
            PluginPage* page = m_pages.at(id);
            m_parent->InsertPage(m_parent->GetPageCount(), page_tab_id(id), page, wxString::FromUTF8(id.name), "",
                                 false, page->icon());
        }

        if (!id_to_reselect.empty())
            m_parent->SelectPageByName(id_to_reselect);
    }

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
        m_swapped_in_id = overflow_ids[index];
        relayout();
        m_parent->SelectPageByName(page_tab_id(*m_swapped_in_id));
    });
    m_overflow_button->PopupMenu(&menu);
}

} // namespace Slic3r
