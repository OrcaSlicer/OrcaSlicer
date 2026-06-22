// Phase 4.2 — PluginManagerDialog.
//
// Lists every plugin currently registered with `::orca::session()` and
// shows manifest details for the selected entry. Provides "Reload all"
// (re-discover the user's plugin dir) and a Close button.
//
// Install / uninstall / per-plugin enable-disable toggles arrive in
// Phase 4.3 alongside the install flow + permission prompt.
#pragma once

#include <wx/dialog.h>

#include <string>

class wxListCtrl;
class wxListEvent;
class wxTextCtrl;
class wxButton;
class wxCommandEvent;

namespace Slic3r { namespace GUI {

class PluginManagerDialog : public wxDialog {
public:
    explicit PluginManagerDialog(wxWindow* parent);

private:
    void OnReloadAll(wxCommandEvent&);
    void OnInstall(wxCommandEvent&);
    void OnSelectionChanged(wxListEvent&);
    void RefreshList();
    void RefreshDetailsFor(const std::string& plugin_id);

    wxListCtrl* m_list    = nullptr;
    wxTextCtrl* m_details = nullptr;
    wxButton*   m_install = nullptr;
    wxButton*   m_reload  = nullptr;
    wxButton*   m_close   = nullptr;
};

}} // namespace Slic3r::GUI
