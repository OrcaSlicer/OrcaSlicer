// Phase 4.1.4 — Device-tab slot dispatcher implementation.

#include "PluginDeviceTabDispatcher.hpp"
#include "PluginUiBuilder.hpp"

#include "orca/Session.hpp"
#include "orca/Globals.hpp"
#include "orca/plugin_api.h"

#include <wx/bookctrl.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace Slic3r { namespace GUI {

void install_plugin_device_tabs_for(wxBookCtrlBase* tabpanel) {
    if (!tabpanel || !::orca::has_session()) return;

    auto& session = ::orca::session();
    auto slots = session.plugin_slots_of(
        static_cast<std::uint32_t>(ORCA_SLOT_DEVICE_TAB));
    if (slots.empty()) return;

    std::sort(slots.begin(), slots.end(),
        [](const auto& a, const auto& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.plugin_id < b.plugin_id;
        });

    static std::vector<std::unique_ptr<PluginUiBuilder>> s_builders;

    for (const auto& s : slots) {
        const auto* v = static_cast<const orca_slot_device_tab_t*>(s.vtable);
        if (!v || v->struct_size < sizeof(orca_slot_device_tab_t)) continue;
        if (!v->tab_title || v->tab_title[0] == '\0') continue;
        if (!v->on_build) continue;

        auto* page  = new wxPanel(tabpanel);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        page->SetSizer(sizer);

        auto builder = std::make_unique<PluginUiBuilder>(page, sizer);
        v->on_build(builder->page(), builder->vtable(), s.user_data);
        s_builders.push_back(std::move(builder));

        tabpanel->AddPage(page, wxString::FromUTF8(v->tab_title));
    }
}

}} // namespace Slic3r::GUI
