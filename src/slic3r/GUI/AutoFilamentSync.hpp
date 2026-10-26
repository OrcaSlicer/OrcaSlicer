#pragma once

#include <string>

namespace Slic3r { namespace GUI {

// Pull-mode printer agents (Moonraker, QIDI, ...) report their filaments only when asked, so the
// filament list stays as it was until the user presses Sync. With the "auto_sync_filaments_on_connect"
// preference set, call this when a printer connects: it syncs the filament list once per printer and
// session, as if Sync was pressed. A project that already has objects is left alone; a notification
// offers the sync instead.
void auto_sync_filaments_on_connect(const std::string& dev_id);

}} // namespace Slic3r::GUI
