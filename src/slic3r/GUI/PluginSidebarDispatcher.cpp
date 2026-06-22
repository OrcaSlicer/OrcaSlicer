// Phase 4.1.2 — Sidebar-panel slot dispatcher implementation.

#include "PluginSidebarDispatcher.hpp"
#include "PluginUiBuilder.hpp"
#include "Plater.hpp"

#include "orca/Session.hpp"
#include "orca/Globals.hpp"
#include "orca/plugin_api.h"

#include <wx/collpane.h>
#include <wx/sizer.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace Slic3r { namespace GUI {

void install_plugin_sidebar_panels_for(Sidebar* sidebar) {
    if (!sidebar || !::orca::has_session()) return;

    auto& session = ::orca::session();
    auto slots = session.plugin_slots_of(
        static_cast<std::uint32_t>(ORCA_SLOT_SIDEBAR_PANEL));
    if (slots.empty()) return;

    std::sort(slots.begin(), slots.end(),
        [](const auto& a, const auto& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.plugin_id < b.plugin_id;
        });

    auto* host_sizer = sidebar->GetSizer();
    if (!host_sizer) return;

    static std::vector<std::unique_ptr<PluginUiBuilder>> s_builders;

    for (const auto& s : slots) {
        const auto* v = static_cast<const orca_slot_sidebar_panel_t*>(s.vtable);
        if (!v || v->struct_size < sizeof(orca_slot_sidebar_panel_t)) continue;
        if (!v->panel_title || v->panel_title[0] == '\0') continue;
        if (!v->on_build) continue;

        auto* pane = new wxCollapsiblePane(
            sidebar, wxID_ANY, wxString::FromUTF8(v->panel_title));
        host_sizer->Add(pane, 0, wxEXPAND | wxALL, 4);

        auto* inner = pane->GetPane();
        auto* inner_sizer = new wxBoxSizer(wxVERTICAL);
        inner->SetSizer(inner_sizer);

        auto builder = std::make_unique<PluginUiBuilder>(inner, inner_sizer);
        v->on_build(builder->page(), builder->vtable(), s.user_data);
        s_builders.push_back(std::move(builder));
    }
    host_sizer->Layout();
}

}} // namespace Slic3r::GUI
