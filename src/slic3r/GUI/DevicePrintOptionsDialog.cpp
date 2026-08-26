#include "DevicePrintOptionsDialog.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>

#include "FilamentMapDialog.hpp"
#include "FilamentMapRowsView.hpp"
#include "FilamentInventoryStore.hpp"
#include "ActivePrinterSession.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace GUI {

DevicePrintOptionsDialog::DevicePrintOptionsDialog(const boost::filesystem::path& path,
                                                   PrintHostPostUploadActions     post_actions,
                                                   const wxArrayString&           groups,
                                                   const wxArrayString&           storage_paths,
                                                   const wxArrayString&           storage_names,
                                                   bool                           switch_to_device_tab,
                                                   DevicePrintSpec                spec,
                                                   std::string                    printer_preset_name,
                                                   std::vector<int>               plate_filaments,
                                                   wxString                       mapping_undeliverable_reason)
    : PrintHostSendDialog(path, post_actions, groups, storage_paths, storage_names, switch_to_device_tab)
    , m_spec(std::move(spec))
    , m_printer_preset_name(std::move(printer_preset_name))
    , m_plate_filaments(std::move(plate_filaments))
    , m_mapping_undeliverable_reason(std::move(mapping_undeliverable_reason))
{}

void DevicePrintOptionsDialog::init()
{
    // The base builds the filename field, storage/group pickers and the Upload / Upload and Print
    // buttons; everything declared by the spec is appended below it. content_sizer and btn_sizer
    // are separate (MsgDialog), so appending here still lands above the button row.
    PrintHostSendDialog::init();

    AppConfig* app_config = wxGetApp().app_config;

    // Mapping and preview sit directly above the options, so the whole decision -- which filament
    // prints on which tool, what the plate will look like, and what the printer should do before
    // it starts -- is visible in one dialog.
    if (m_spec.supports_filament_mapping && !m_plate_filaments.empty()) {
        auto full_config = wxGetApp().preset_bundle->full_config();
        const std::vector<std::string> colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
        const std::vector<std::string> types  = full_config.option<ConfigOptionStrings>("filament_type")->values;
        const std::vector<std::string> names  = full_config.option<ConfigOptionStrings>("filament_settings_id")->values;

        FilamentInventories store;
        const size_t        tool_count = resolve_active_printer_tool_count(store);
        const Preset&       printer    = active_printer_session().profile();
        FilamentInventory&  live_inventory = current_inventory_for_preset(printer, store, tool_count);
        // Refresh from the printer on EVERY open, not only first-ever bootstrap: a spool changed
        // on the printer otherwise kept showing its stale recorded color/material here until the
        // user manually synced from the materials editor (field report and fix by ricvil25). One
        // blocking fetch, same as the materials editor's sync; failure (or no live session) just
        // keeps whatever is recorded, and an unchanged spool keeps its recorded detail (see
        // sync_filament_inventory_from_printer's same-spool guard).
        sync_filament_inventory_from_printer(store, live_inventory, tool_count);
        const FilamentInventory inventory = live_inventory;

        // No plate to read a stored map from at send time: full-length all-zero base maps plus
        // filament_map_confirmed=false gives "first-ever visit" semantics, so every row is
        // proposed straight from the inventory auto-match.
        const std::vector<int> base_map(colors.size(), 0);

        content_sizer->AddSpacer(VERT_SPACING);
        m_rows_view = new FilamentMapRowsView(this, colors, types, names, base_map, base_map, m_plate_filaments,
                                              inventory, tool_count, printer.name, /*filament_map_confirmed=*/false,
                                              [this](bool all_assigned) {
                                                  if (m_print_btn)
                                                      m_print_btn->Enable(all_assigned && m_mapping_undeliverable_reason.IsEmpty());
                                                  Layout();
                                                  // The mapping view just rebuilt or reflowed its content (Reset/Automatic
                                                  // recreate the rows panel), but a descendant rebuild does not invalidate
                                                  // THIS dialog's cached best size -- and Fit() consults that cache. Stale
                                                  // cache meant Layout() above already gave the view its new, taller
                                                  // allocation while Fit() kept the old dialog height, clipping everything
                                                  // below the view under the button row (field report: "Print Preferences
                                                  // disappears" after pressing Reset). Re-measure before fitting.
                                                  InvalidateBestSize();
                                                  Fit();
                                              });
        content_sizer->Add(m_rows_view, 0, wxEXPAND);

        auto* map_btns = new wxBoxSizer(wxHORIZONTAL);
        auto* reset_btn = new Button(this, _L("Reset"));
        reset_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        reset_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_rows_view->ResetToInitial(); });
        map_btns->Add(reset_btn, 0);
        auto* auto_btn = new Button(this, _L("Automatic"));
        auto_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        auto_btn->SetToolTip(_L("Fill the rows with a freshly computed automatic match."));
        auto_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_rows_view->ApplyAutomatic(); });
        map_btns->Add(auto_btn, 0, wxLEFT, FromDIP(8));
        content_sizer->Add(map_btns, 0, wxTOP, FromDIP(4));
    }

    content_sizer->AddSpacer(VERT_SPACING);
    auto* prefs_label = new wxStaticText(this, wxID_ANY, _L("Print Preferences"));
    prefs_label->SetFont(::Label::Head_14);
    content_sizer->Add(prefs_label);

    for (const DevicePrintOption& opt : m_spec.options) {
        // Remembered per printer, so one printer's choices cannot leak onto another that happens
        // to declare the same option key; the declared default applies until the user chooses.
        std::string value = app_config->get_printer_setting(m_printer_preset_name, config_key(opt.key));
        if (value.empty())
            value = opt.default_value;
        m_options[opt.key] = value;

        // Choice options are declared but not rendered yet: the value still reaches the start
        // script (its declared default), and rendering lands with the first vendor that has one,
        // so the picker ships with a consumer who can test it.
        if (opt.kind != DevicePrintOptionKind::Bool)
            continue;

        auto* row      = new wxBoxSizer(wxHORIZONTAL);
        auto* checkbox = new ::CheckBox(this);
        checkbox->SetValue(value == "1");
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, key = opt.key](wxCommandEvent& e) {
            m_options[key] = e.IsChecked() ? "1" : "0";
            e.Skip();
        });
        row->Add(checkbox, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));

        auto* label = new wxStaticText(this, wxID_ANY, _(opt.label));
        label->SetFont(::Label::Body_13);
        row->Add(label, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));

        if (!opt.tooltip.empty()) {
            const wxString tooltip = _(opt.tooltip);
            checkbox->SetToolTip(tooltip);
            label->SetToolTip(tooltip);
        }
        content_sizer->Add(row);
    }

    // This dialog is reached from "Print"; a plain upload is its own menu entry and never shows
    // mapping or options at all, so the base's "Upload" button would be a second, quieter way to
    // do something the user did not come here for. The base's StartPrint button already does
    // exactly what Print means (upload, then start), so relabel that one and hide the other.
    if (auto* btn_upload = FindWindow(wxID_OK))
        btn_upload->Hide();
    m_print_btn = dynamic_cast<Button*>(FindWindow(wxID_YES));
    if (m_print_btn != nullptr) {
        m_print_btn->SetLabel(_L_CONTEXT("Print", "Verb"));
        if (!m_mapping_undeliverable_reason.IsEmpty()) {
            // Refuse up front rather than accepting the click and silently downgrading the send to
            // an upload afterwards: the user should never believe a mapping was delivered.
            m_print_btn->Disable();
            m_print_btn->SetToolTip(m_mapping_undeliverable_reason);
        } else if (m_rows_view != nullptr) {
            m_print_btn->Enable(m_rows_view->AllRowsAssigned());
            m_print_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
                m_rows_view->Commit();
                m_filament_map = m_rows_view->GetFilamentMaps();
                e.Skip(); // on to the base's handler: sets StartPrint and closes
            });
        }
    }

    finalize();

    // Orca: widen past what the content measured. The mapping view cannot report a trustworthy
    // width -- FilamentMapRowsPanel lays its tiles out in a wrap sizer, whose minimum is one tile
    // rather than a row, so the measurement misses both how many tiles want to sit side by side
    // and the unwrapped profile-mismatch warning underneath them, which is the widest line in the
    // dialog. Measuring later (after the warning exists) did not help, which is what ruled the
    // measurement out. A flat margin over whatever was measured is the honest fix until the panel
    // reports its own width; the floor only grows the dialog, so a plate with few filaments still
    // gets a small one.
    if (m_rows_view != nullptr) {
        SetMinSize(wxSize(GetSize().GetWidth() * 5 / 4, -1));
        Fit();
    }
}

void DevicePrintOptionsDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {
        AppConfig* app_config = wxGetApp().app_config;
        for (const auto& [key, value] : m_options)
            app_config->set_printer_setting(m_printer_preset_name, config_key(key), value);
    }
    PrintHostSendDialog::EndModal(ret);
}

}} // namespace Slic3r::GUI
