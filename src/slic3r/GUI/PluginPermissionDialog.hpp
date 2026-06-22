// Phase 4.3.2 — PluginPermissionDialog.
//
// Modal shown after PluginInstaller::peek_identity but BEFORE the
// archive is extracted. Lists the plugin's requested ORCA_PERM_* bits
// in human-readable form so the user can give informed consent. Allow
// = return wxID_OK; Cancel = wxID_CANCEL.
#pragma once

#include <wx/dialog.h>

#include <cstdint>
#include <string>

namespace Slic3r { namespace GUI {

class PluginPermissionDialog : public wxDialog {
public:
    PluginPermissionDialog(wxWindow* parent,
                           const std::string& plugin_id,
                           const std::string& plugin_name,
                           const std::string& plugin_version,
                           std::uint64_t      requested_permissions);
};

}} // namespace Slic3r::GUI
