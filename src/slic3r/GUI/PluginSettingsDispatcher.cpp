// Phase 4.1.1 — Settings-page slot dispatcher implementation.

#include "PluginSettingsDispatcher.hpp"
#include "PluginUiBuilder.hpp"
#include "Tab.hpp"

#include "orca/Session.hpp"
#include "orca/Globals.hpp"
#include "orca/plugin_api.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace Slic3r { namespace GUI {

void install_plugin_settings_pages_for(Tab* tab, std::uint32_t orca_settings_tab) {
    if (!tab || !::orca::has_session()) return;

    auto& session = ::orca::session();
    auto slots = session.plugin_slots_of(
        static_cast<std::uint32_t>(ORCA_SLOT_SETTINGS_PAGE));
    if (slots.empty()) return;

    std::sort(slots.begin(), slots.end(),
        [](const auto& a, const auto& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.plugin_id < b.plugin_id;
        });

    for (const auto& s : slots) {
        const auto* v = static_cast<const orca_slot_settings_page_t*>(s.vtable);
        if (!v || v->struct_size < sizeof(orca_slot_settings_page_t)) continue;
        if (static_cast<std::uint32_t>(v->tab) != orca_settings_tab) continue;
        if (!v->page_title || v->page_title[0] == '\0') continue;
        if (!v->on_build) continue;

        const wxString title = wxString::FromUTF8(v->page_title);
        // No per-page icon today — pass the engine's generic plugin icon
        // name; Tab::add_options_page falls back if the resource is
        // absent. Icon = "custom-gcode_setting_override" so the page
        // still renders in the sidebar.
        auto page = tab->add_options_page(title, "custom-gcode_setting_override");
        if (!page) continue;

        // PluginUiBuilder must outlive its wx controls — they capture
        // its `this` through Bind() lambdas. Tabs survive for the
        // app's lifetime, so we park each builder in a process-scope
        // store and let the wx reaper handle the controls.
        static std::vector<std::unique_ptr<PluginUiBuilder>> s_builders;
        auto builder = std::make_unique<PluginUiBuilder>(
            page->parent(), page->vsizer());
        v->on_build(builder->page(), builder->vtable(), s.user_data);
        s_builders.push_back(std::move(builder));
    }
}

}} // namespace Slic3r::GUI
