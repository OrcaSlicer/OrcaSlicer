// Phase 4.1.3 — Menu-command slot dispatcher implementation.

#include "PluginMenuDispatcher.hpp"

#include "orca/Session.hpp"
#include "orca/Globals.hpp"
#include "orca/plugin_api.h"

#include <wx/menu.h>
#include <wx/string.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

void install_plugin_menu_items_for(wxMenu* menu, std::uint32_t orca_menu) {
    if (!menu || !::orca::has_session()) return;

    auto& session = ::orca::session();
    const auto slots = session.plugin_slots_of(
        static_cast<std::uint32_t>(ORCA_SLOT_MENU_COMMAND));
    if (slots.empty()) return;

    // Stable ordering — by priority then plugin id — so menu items
    // appear in the same order across runs.
    auto items = slots;
    std::sort(items.begin(), items.end(),
        [](const auto& a, const auto& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.plugin_id < b.plugin_id;
        });

    bool inserted_separator = false;
    for (const auto& s : items) {
        const auto* v = static_cast<const orca_slot_menu_command_t*>(s.vtable);
        if (!v || v->struct_size < sizeof(orca_slot_menu_command_t)) continue;
        if (static_cast<std::uint32_t>(v->menu) != orca_menu) continue;
        if (!v->label || v->label[0] == '\0') continue;

        if (!inserted_separator) {
            menu->AppendSeparator();
            inserted_separator = true;
        }

        wxString label = wxString::FromUTF8(v->label);
        if (v->shortcut && v->shortcut[0] != '\0')
            label << "\t" << wxString::FromUTF8(v->shortcut);

        auto* item = menu->Append(wxID_ANY, label,
                                  wxString::FromUTF8("[plugin] ") +
                                  wxString::FromUTF8(s.plugin_id));

        auto user_data = s.user_data;
        auto on_click  = v->on_click;
        menu->Bind(wxEVT_MENU,
                   [on_click, user_data](wxCommandEvent&) {
                       if (on_click) on_click(user_data);
                   },
                   item->GetId());
    }
}

}} // namespace Slic3r::GUI
