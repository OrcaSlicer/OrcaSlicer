#include "DevicePrintOptionsDialog.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/arrstr.h>
#include <string>
#include <vector>
#include <slic3r/GUI/PrintHostDialogs.hpp>
#include <utility>
#include <libslic3r/FilamentInventory.hpp>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/tglbtn.h>

#include "FilamentMapRowsView.hpp"
#include "FilamentInventoryStore.hpp"
#include "ActivePrinterSession.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "MsgDialog.hpp"
#include "slic3r/Utils/PrintHost.hpp"
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
    PrintHostSendDialog::init();

    AppConfig* app_config = wxGetApp().app_config;

    if (m_spec.supports_filament_mapping && !m_plate_filaments.empty()) {
        FilamentInventories store;
        FilamentInventory&  live_inventory = load_active_printer_inventory(store);
        // Refresh on every open so spools changed on the printer show here; failure keeps the record.
        sync_filament_inventory_from_printer(store, live_inventory);

        content_sizer->AddSpacer(VERT_SPACING);
        m_rows_view = new FilamentMapRowsView(this, m_plate_filaments, live_inventory,
                                              addressable_tool_count_of(active_printer_session().profile()),
                                              [this](bool all_assigned) {
                                                  if (m_print_btn)
                                                      m_print_btn->Enable(all_assigned && m_mapping_undeliverable_reason.IsEmpty());
                                                  Layout();
                                                  // A descendant rebuild does not invalidate this dialog's cached best size, which Fit() uses.
                                                  InvalidateBestSize();
                                                  Fit();
                                              });
        content_sizer->Add(m_rows_view, 0, wxEXPAND);
    }

    if (!m_spec.options.empty()) {
        content_sizer->AddSpacer(VERT_SPACING);
        auto* prefs_label = new wxStaticText(this, wxID_ANY, _L("Print Preferences"));
        prefs_label->SetFont(::Label::Head_14);
        content_sizer->Add(prefs_label);
    }

    for (const DevicePrintOption& opt : m_spec.options) {
        // Remembered per printer, so choices cannot leak onto another printer with the same key.
        std::string value = app_config->get_printer_setting(m_printer_preset_name, config_key(opt.key));
        if (value.empty())
            value = opt.default_value;
        m_options[opt.key] = value;

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

    // Plain upload is its own menu entry, so only the base's StartPrint button is kept, as "Print".
    if (auto* btn_upload = FindWindow(wxID_OK))
        btn_upload->Hide();
    m_print_btn = dynamic_cast<Button*>(FindWindow(wxID_YES));
    if (m_print_btn != nullptr) {
        m_print_btn->SetLabel(_L_CONTEXT("Print", "Verb"));
        if (!m_mapping_undeliverable_reason.IsEmpty()) {
            // Show the reason as text too: disabled controls get no mouse events on Windows, so no tooltip.
            m_print_btn->Disable();
            m_print_btn->SetToolTip(m_mapping_undeliverable_reason);
            wxStaticText* reason = new wxStaticText(this, wxID_ANY, m_mapping_undeliverable_reason);
            reason->SetForegroundColour(wxColour(0xFF, 0x6F, 0x00)); // house warning orange
            reason->Wrap(FromDIP(420));
            content_sizer->AddSpacer(FromDIP(6));
            content_sizer->Add(reason, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(12));
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

    // Orca: the rows panel's wrap sizer reports a one-tile minimum width, so widen past the measured size.
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
