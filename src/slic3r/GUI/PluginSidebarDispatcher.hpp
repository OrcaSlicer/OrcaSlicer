// Phase 4.1.2 — Sidebar-panel slot dispatcher.
//
// Walks every ORCA_SLOT_SIDEBAR_PANEL slot, builds a collapsible
// wxCollapsiblePane per slot under the sidebar's main sizer, and
// invokes the plugin's on_build against a PluginUiBuilder.
//
// Call once from Sidebar's constructor after the built-in sidebar
// content is laid out so plugin panels appear below.
#pragma once

class wxPanel;

namespace Slic3r { namespace GUI {

class Sidebar;

void install_plugin_sidebar_panels_for(Sidebar* sidebar);

}} // namespace Slic3r::GUI
