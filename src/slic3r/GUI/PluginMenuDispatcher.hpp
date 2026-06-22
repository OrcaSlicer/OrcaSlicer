// Phase 4.1.3 — Menu-command slot dispatcher.
//
// Walks every ORCA_SLOT_MENU_COMMAND slot registered with the engine
// and appends a wxMenuItem to the matching wxMenu. Call once per
// top-level menu after MainFrame finishes building it; the slot's
// on_click fires on the wx UI thread.
#pragma once

#include <cstdint>

class wxMenu;

namespace Slic3r { namespace GUI {

/// `orca_menu` is the orca_menu_t enum value from plugin_api.h
/// (ORCA_MENU_FILE = 0, _EDIT, _VIEW, _TOOLS, _HELP).
void install_plugin_menu_items_for(wxMenu* menu, std::uint32_t orca_menu);

}} // namespace Slic3r::GUI
