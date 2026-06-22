// Phase 4.1.4 — Device-tab slot dispatcher.
//
// Walks every ORCA_SLOT_DEVICE_TAB slot and adds a top-level page to
// MainFrame's tab panel — appearing alongside Device / Calibration /
// Project. The match_vendor field on the slot vtable is reserved for
// future per-printer filtering; today every slot is shown.
#pragma once

class wxBookCtrlBase;

namespace Slic3r { namespace GUI {

void install_plugin_device_tabs_for(wxBookCtrlBase* tabpanel);

}} // namespace Slic3r::GUI
