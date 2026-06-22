// Phase 4.1.1 — Settings-page slot dispatcher.
//
// Walks every ORCA_SLOT_SETTINGS_PAGE whose vtable->tab matches the
// requested orca_settings_tab_t value, creates a Page via
// Tab::add_options_page, builds a PluginUiBuilder against the Page's
// vsizer, and invokes vtable->on_build with the builder vtable.
//
// Call once from TabPrint::build / TabFilament::build /
// TabPrinter::build after the built-in pages are added so the
// plugin-contributed pages appear at the bottom of the sidebar.
#pragma once

#include <cstdint>

namespace Slic3r { namespace GUI {

class Tab;

/// orca_settings_tab values: 0=Print, 1=Filament, 2=Printer.
void install_plugin_settings_pages_for(Tab* tab, std::uint32_t orca_settings_tab);

}} // namespace Slic3r::GUI
