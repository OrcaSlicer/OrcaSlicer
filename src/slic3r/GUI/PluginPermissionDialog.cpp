// Phase 4.3.2 — PluginPermissionDialog implementation.

#include "PluginPermissionDialog.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

namespace {

struct PermInfo {
    std::uint64_t bit;
    const char*   label;
    const char*   description;
};

constexpr PermInfo kPerms[] = {
    {1ull << 0,  "Network",
     "Open outbound HTTP / WebSocket / mDNS connections."},
    {1ull << 1,  "Read files",
     "Read files anywhere your user account can reach."},
    {1ull << 2,  "Write files",
     "Create or overwrite files anywhere your user can reach."},
    {1ull << 3,  "Read settings",
     "Read your current Print / Filament / Printer settings."},
    {1ull << 4,  "Modify settings",
     "Change your current Print / Filament / Printer settings."},
    {1ull << 5,  "Install profiles",
     "Add new vendor / printer / filament profiles to the slicer."},
    {1ull << 6,  "Control device",
     "Send commands to your 3D printer (start/cancel print, send G-code)."},
    {1ull << 7,  "Intercept slicing",
     "Halt or alter the slicing pipeline mid-run."},
    {1ull << 8,  "Modify G-code",
     "Rewrite generated G-code before it is exported."},
    {1ull << 9,  "Attach UI",
     "Add settings pages, menus, sidebar panels, or device tabs."},
    {1ull << 10, "Raw event bus",
     "Subscribe to every event the engine emits, including future ones."},
};

} // namespace

PluginPermissionDialog::PluginPermissionDialog(wxWindow*          parent,
                                               const std::string& plugin_id,
                                               const std::string& plugin_name,
                                               const std::string& plugin_version,
                                               std::uint64_t      requested_permissions)
    : wxDialog(parent, wxID_ANY, "Install plugin?",
               wxDefaultPosition, wxSize(560, 480),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(
        this, wxID_ANY,
        wxString::Format("Allow \"%s\" (%s) v%s?\n"
                         "ID: %s\n"
                         "Granting these capabilities lets the plugin do "
                         "what's listed below.",
                         wxString::FromUTF8(plugin_name),
                         wxString::FromUTF8(plugin_id),
                         wxString::FromUTF8(plugin_version),
                         wxString::FromUTF8(plugin_id)));
    outer->Add(header, 0, wxALL, 12);

    auto* req_label = new wxStaticText(
        this, wxID_ANY,
        requested_permissions == 0
          ? "This plugin requests no special permissions."
          : "Requested permissions:");
    outer->Add(req_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    if (requested_permissions != 0) {
        auto* grid = new wxBoxSizer(wxVERTICAL);
        for (const auto& p : kPerms) {
            if ((requested_permissions & p.bit) == 0) continue;
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY,
                wxString::Format("  - %s", p.label)),
                0, wxALIGN_TOP | wxRIGHT, 6);
            row->Add(new wxStaticText(this, wxID_ANY,
                wxString::Format("(%s)", p.description)),
                1, wxEXPAND);
            grid->Add(row, 0, wxBOTTOM | wxLEFT | wxRIGHT | wxEXPAND, 4);
        }
        outer->Add(grid, 1, wxEXPAND | wxALL, 12);
    } else {
        outer->AddStretchSpacer();
    }

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    auto* cancel  = new wxButton(this, wxID_CANCEL, "Cancel");
    auto* allow   = new wxButton(this, wxID_OK,     "Allow + install");
    btn_row->AddStretchSpacer();
    btn_row->Add(cancel, 0, wxALL, 8);
    btn_row->Add(allow,  0, wxALL, 8);
    outer->Add(btn_row, 0, wxEXPAND);

    allow->SetDefault();

    SetSizer(outer);
    Layout();
}

}} // namespace Slic3r::GUI
