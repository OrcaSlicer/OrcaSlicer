// Phase 4.2 — PluginManagerDialog implementation.

#include "PluginManagerDialog.hpp"
#include "PluginPermissionDialog.hpp"

#include "orca/Session.hpp"
#include "orca/Globals.hpp"
#include "orca/Slicer.hpp"

#include "../Utils/PluginInstaller.hpp"
#include "libslic3r/Utils.hpp"   // Slic3r::data_dir

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

constexpr int COL_LOADED  = 0;
constexpr int COL_ID      = 1;
constexpr int COL_NAME    = 2;
constexpr int COL_VERSION = 3;
constexpr int COL_AUTHOR  = 4;

// Human-readable permission list from the ORCA_PERM_* bitmask.
std::string format_permissions(std::uint64_t bits) {
    if (bits == 0) return "(none)";
    static const std::pair<std::uint64_t, const char*> kPerms[] = {
        {1ull << 0,  "NETWORK"},
        {1ull << 1,  "FILESYSTEM_READ"},
        {1ull << 2,  "FILESYSTEM_WRITE"},
        {1ull << 3,  "SETTINGS_READ"},
        {1ull << 4,  "SETTINGS_WRITE"},
        {1ull << 5,  "PROFILES_INSTALL"},
        {1ull << 6,  "DEVICE_CONTROL"},
        {1ull << 7,  "SLICE_INTERCEPT"},
        {1ull << 8,  "GCODE_MODIFY"},
        {1ull << 9,  "UI_ATTACH"},
        {1ull << 10, "EVENTS_RAW"},
    };
    std::string out;
    for (const auto& [bit, name] : kPerms) {
        if (bits & bit) {
            if (!out.empty()) out += ", ";
            out += name;
        }
    }
    return out.empty() ? "(unknown)" : out;
}

} // namespace

PluginManagerDialog::PluginManagerDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Plugins", wxDefaultPosition,
               wxSize(720, 480), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- list ----
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->InsertColumn(COL_LOADED,  "Loaded",  wxLIST_FORMAT_CENTER,  60);
    m_list->InsertColumn(COL_ID,      "ID",      wxLIST_FORMAT_LEFT,    220);
    m_list->InsertColumn(COL_NAME,    "Name",    wxLIST_FORMAT_LEFT,    180);
    m_list->InsertColumn(COL_VERSION, "Version", wxLIST_FORMAT_LEFT,    80);
    m_list->InsertColumn(COL_AUTHOR,  "Author",  wxLIST_FORMAT_LEFT,    140);
    outer->Add(m_list, 1, wxEXPAND | wxALL, 6);

    // ---- details ----
    auto* details_label = new wxStaticText(this, wxID_ANY, "Details:");
    outer->Add(details_label, 0, wxLEFT | wxTOP, 6);
    m_details = new wxTextCtrl(this, wxID_ANY, "",
                               wxDefaultPosition, wxSize(-1, 120),
                               wxTE_MULTILINE | wxTE_READONLY);
    outer->Add(m_details, 0, wxEXPAND | wxALL, 6);

    // ---- buttons ----
    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    m_install = new wxButton(this, wxID_ANY, "Install...");
    m_reload  = new wxButton(this, wxID_ANY, "Reload all");
    m_close   = new wxButton(this, wxID_CLOSE, "Close");
    btn_row->Add(m_install, 0, wxALL, 6);
    btn_row->Add(m_reload,  0, wxALL, 6);
    btn_row->AddStretchSpacer();
    btn_row->Add(m_close,   0, wxALL, 6);
    outer->Add(btn_row, 0, wxEXPAND);

    SetSizer(outer);
    Layout();

    m_install->Bind(wxEVT_BUTTON, &PluginManagerDialog::OnInstall,    this);
    m_reload ->Bind(wxEVT_BUTTON, &PluginManagerDialog::OnReloadAll,  this);
    m_close  ->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });
    m_list   ->Bind(wxEVT_LIST_ITEM_SELECTED,
                    &PluginManagerDialog::OnSelectionChanged, this);

    RefreshList();
}

void PluginManagerDialog::RefreshList() {
    m_list->DeleteAllItems();

    if (!::orca::has_session()) return;
    auto& session = ::orca::session();

    const auto loaded_ids = session.loaded_plugin_ids();
    const auto manifests  = session.plugin_manifests();

    long row = 0;
    for (const auto& m : manifests) {
        const bool is_loaded = std::find(loaded_ids.begin(), loaded_ids.end(), m.id)
                               != loaded_ids.end();
        m_list->InsertItem(row, is_loaded ? "yes" : "no");
        m_list->SetItem(row, COL_ID,      m.id);
        m_list->SetItem(row, COL_NAME,    m.name);
        m_list->SetItem(row, COL_VERSION, m.version);
        m_list->SetItem(row, COL_AUTHOR,  m.author);
        ++row;
    }

    if (row > 0) {
        m_list->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        RefreshDetailsFor(m_list->GetItemText(0, COL_ID).utf8_string());
    } else {
        m_details->SetValue("No plugins discovered.");
    }
}

void PluginManagerDialog::RefreshDetailsFor(const std::string& plugin_id) {
    if (!::orca::has_session()) {
        m_details->SetValue("Engine session not available.");
        return;
    }
    auto& session = ::orca::session();

    const auto manifests = session.plugin_manifests();
    auto it = std::find_if(manifests.begin(), manifests.end(),
        [&](const orca::Session::ManifestInfo& m) { return m.id == plugin_id; });
    if (it == manifests.end()) {
        m_details->SetValue("Manifest not found for: " + wxString::FromUTF8(plugin_id));
        return;
    }

    std::ostringstream s;
    s << "ID:          " << it->id          << "\n"
      << "Name:        " << it->name        << "\n"
      << "Version:     " << it->version     << "\n"
      << "Author:      " << it->author      << "\n"
      << "Loaded:      " << (session.is_plugin_loaded(it->id) ? "yes" : "no") << "\n"
      << "Permissions: " << format_permissions(it->permissions) << "\n"
      << "\n"
      << "Description:\n"
      << it->description;
    m_details->SetValue(wxString::FromUTF8(s.str()));
}

void PluginManagerDialog::OnSelectionChanged(wxListEvent& evt) {
    const auto id = m_list->GetItemText(evt.GetIndex(), COL_ID).utf8_string();
    RefreshDetailsFor(id);
}

void PluginManagerDialog::OnInstall(wxCommandEvent&) {
    if (!::orca::has_session()) {
        wxMessageBox("Engine session not available.", "Plugins",
                     wxOK | wxICON_ERROR, this);
        return;
    }

    wxFileDialog pick(this, "Install plugin",
                      wxEmptyString, wxEmptyString,
                      "Orca plugin archive (*.orcaplugin;*.zip)|*.orcaplugin;*.zip",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (pick.ShowModal() != wxID_OK) return;

    const std::filesystem::path archive = pick.GetPath().utf8_string();

    // 1. Peek the manifest to learn what we're about to install.
    auto peek = Slic3r::PluginInstaller::peek_identity(archive);
    if (peek.status != Slic3r::PluginInstaller::Status::Ok) {
        wxMessageBox("Plugin archive is invalid:\n" +
                     wxString::FromUTF8(peek.error_message),
                     "Plugins", wxOK | wxICON_ERROR, this);
        return;
    }

    // 2. Permission prompt — Allow / Cancel.
    PluginPermissionDialog perms(this,
                                 peek.identity.id,
                                 peek.identity.name,
                                 peek.identity.version,
                                 peek.identity.permissions);
    if (perms.ShowModal() != wxID_OK) return;

    // 3. Phase 4.3.3 hot-reload: if a slice is in progress, don't yank
    //    the plugin's slot vtables out from under it. Block extraction
    //    until the slicer is idle.
    auto& session = ::orca::session();
    if (session.slicer().is_busy()) {
        wxMessageBox(
            "Cannot install plugins while a slice is in progress.\n"
            "Wait for the current job to finish and try again.",
            "Plugins", wxOK | wxICON_WARNING, this);
        return;
    }

    // 4. Extract into <data_dir>/plugins/<id>/, replacing any prior
    //    install of the same id.
    const auto plugins_root = std::filesystem::path(Slic3r::data_dir()) / "plugins";
    std::error_code ec;
    std::filesystem::create_directories(plugins_root, ec);

    if (session.is_plugin_loaded(peek.identity.id))
        session.unload_plugin(peek.identity.id);

    auto inst = Slic3r::PluginInstaller::install(
        archive, plugins_root, /*replace_existing=*/true);
    if (inst.status != Slic3r::PluginInstaller::Status::Ok) {
        wxMessageBox("Install failed:\n" + wxString::FromUTF8(inst.error_message),
                     "Plugins", wxOK | wxICON_ERROR, this);
        return;
    }

    // 5. Bring it online immediately.
    auto load_rc = session.load_plugin(inst.install_dir);
    if (!load_rc.ok()) {
        wxMessageBox("Plugin installed but failed to load:\n" +
                     wxString::FromUTF8(load_rc.error().message),
                     "Plugins", wxOK | wxICON_WARNING, this);
    } else {
        wxMessageBox("Installed and loaded:\n" +
                     wxString::FromUTF8(peek.identity.name) +
                     " v" + wxString::FromUTF8(peek.identity.version),
                     "Plugins", wxOK | wxICON_INFORMATION, this);
    }

    RefreshList();
}

void PluginManagerDialog::OnReloadAll(wxCommandEvent&) {
    if (!::orca::has_session()) {
        wxMessageBox("Engine session not available.", "Plugins",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    auto& session = ::orca::session();

    const auto plugins_dir = std::filesystem::path(Slic3r::data_dir()) / "plugins";
    if (!std::filesystem::is_directory(plugins_dir)) {
        wxMessageBox(
            "No plugins directory at:\n" + wxString::FromUTF8(plugins_dir.string()),
            "Plugins", wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Drop everything we have loaded; the discover_and_load_plugins
    // below re-walks the disk and picks up any changes.
    session.unload_all_plugins();
    const auto added = session.discover_and_load_plugins(plugins_dir);

    RefreshList();
    wxMessageBox(
        wxString::Format("Reloaded plugin directory.\nNewly loaded: %zu",
                         added),
        "Plugins", wxOK | wxICON_INFORMATION, this);
}

}} // namespace Slic3r::GUI
