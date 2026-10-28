#include "FilamentInventoryEditor.hpp"

#include <cstddef>
#include <string>
#include <libslic3r/Config.hpp>
#include <wx/busycursor.h>
#include <libslic3r/FilamentInventory.hpp>
#include <vector>
#include <wx/clrpicker.h>
#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include "ActivePrinterSession.hpp"
#include "DeviceManager.hpp"
#include "FilamentInventoryStore.hpp"
#include "SlotGridPanel.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PresetComboBoxes.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

namespace Slic3r { namespace GUI {

// Shown for a slot with no color so it doesn't look like a real black/white pick.
static const wxColour UNSET_COLOR(0xD9, 0xD9, 0xD9);

FilamentInventoryEditor::FilamentInventoryEditor(wxWindow* parent, const std::string& printer_preset_name, size_t tool_count)
    : wxDialog(parent, wxID_ANY, _L("Printer Material Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_printer_preset_name(printer_preset_name)
{
    SetBackgroundColour(*wxWHITE);

    m_tool_count  = tool_count;

    m_store = load_filament_inventories();
    const Preset *printer_preset = wxGetApp().preset_bundle->printers.find_preset(printer_preset_name, false);
    refresh_read_only();

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(15));

    wxBoxSizer* device_sizer = new wxBoxSizer(wxHORIZONTAL);
    device_sizer->Add(new Label(this, wxString::Format(_L("Printer: %s"), from_u8(printer_preset_name))),
                       0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(device_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    {
        // Disabled controls show no tooltip on MSW, so explain read-only mode in a label.
        m_read_only_note = new Label(this, _L("Materials are reported by the printer and can't be edited here."));
        m_read_only_note->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        main_sizer->AddSpacer(FromDIP(6));
        main_sizer->Add(m_read_only_note, 0, wxLEFT | wxRIGHT, FromDIP(15));
        m_read_only_note->Show(m_read_only);
    }
    main_sizer->AddSpacer(FromDIP(15));

    {
        m_extruder_count = printer_preset ? addressable_tool_count_of(*printer_preset) : 1;
        const bool memm  = printer_preset && m_extruder_count > 1 && !reported_changer_of(printer_preset->config).empty();
        SlotGridOptions opts;
        opts.show_extruder_row     = memm;
        opts.show_extruder_on_tile = memm;
        opts.show_edit_pencil      = true;
        m_grid = new SlotGridPanel(this, opts);
        m_grid->SetOnEditClicked([this](size_t slot) { open_row_editor(slot); });
        main_sizer->Add(m_grid, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    }
    m_rows.resize(tool_count);
    main_sizer->AddSpacer(FromDIP(15));

    reload_rows_from_device();

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->AddStretchSpacer();

    auto* dlg_btns = new DialogButtons(bottom_panel, {"Sync from printer", "OK", "Cancel"}, "", 1);
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    // Evaluated once; the button doesn't follow a connection change while the dialog is open.
    Button* sync_btn = dlg_btns->GetButtonFromIndex(0);
    const ActivePrinterSession& session   = active_printer_session();
    MachineObject*              machine   = session.live_machine();
    NetworkAgent*               net_agent = session.sync_agent();
    // Pull-mode agents fetch on demand, so a stale is_connected() heartbeat must not disable
    // sync; only subscription mode needs a live connection.
    FilamentSyncMode sync_mode = net_agent ? net_agent->get_filament_sync_mode() : FilamentSyncMode::none;
    const bool live_context = net_agent != nullptr;
    m_sync_available = live_context && sync_mode != FilamentSyncMode::none &&
                       (sync_mode == FilamentSyncMode::pull || machine->is_connected());
    sync_btn->Enable(m_sync_available);
    if (!m_sync_available)
        sync_btn->SetToolTip(live_context
            ? _L("Connect to a printer that supports reading loaded filaments to use this.")
            : _L("No printer connected — settings are saved locally."));
    sync_btn->Bind(wxEVT_BUTTON, &FilamentInventoryEditor::on_sync_from_printer, this);

    // The printer is the source of truth for what's loaded; the read also records the push baseline.
    if (m_sync_available)
        CallAfter([this]() {
            wxBusyCursor busy;
            do_sync_from_printer(/*interactive=*/false);
        });

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &FilamentInventoryEditor::on_ok, this);
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    SetEscapeId(wxID_CANCEL);

    SetSizerAndFit(main_sizer);

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentInventoryEditor::update_clear_enabled(Row& row)
{
    row.action_btn->Enable(row.color_touched || row.type_touched);
}

const Preset* FilamentInventoryEditor::resolved_preset_of(const Row& row) const
{
    if (!row.type_touched || row.picked_preset.empty())
        return nullptr;
    return wxGetApp().preset_bundle->filaments.find_preset(row.picked_preset, false);
}

void FilamentInventoryEditor::refresh_grid()
{
    std::vector<SlotGridSlot> rows = slot_grid_rows(edited_inventory(), wxGetApp().preset_bundle->filaments);
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool tag_locked = m_tag_locked_tools.count(i) != 0;
        const bool printer_empty = !tag_locked && m_empty_on_printer.count(i) != 0;
        rows[i].edit_disabled    = m_read_only || tag_locked || printer_empty;
        wxString why;
        if (m_read_only)
            why = _L("This printer doesn't support writing filament settings back; the inventory is read-only.");
        else if (tag_locked)
            why = _L("This slot's filament is set by an NFC tag and can't be edited here.");
        else if (printer_empty)
            why = _L("No filament is loaded in this slot.");
        if (!why.IsEmpty())
            rows[i].tooltip = why;
    }
    m_grid->SetSlots(rows, m_extruder_count);
    Layout();
    Fit();
}

void FilamentInventoryEditor::add_row_widgets(size_t slot, wxWindow* parent, wxSizer* target_sizer)
{
    Row& row = m_rows[slot];

    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

    row.color_picker = new wxColourPickerCtrl(parent, wxID_ANY, row.last_color);

    // Selection is row state only, never the project's filament_presets.
    row.type_choice = new PhysicalFilamentComboBox(parent);
    row.type_choice->select_preset(row.type_touched ? row.picked_preset : std::string());

    auto* clear_btn = new Button(parent, _L("Clear"));
    clear_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    row.action_btn = clear_btn;

    row.color_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this, slot](wxColourPickerEvent&) {
        Row& r = m_rows[slot];
        r.color_touched = true;
        r.last_color    = r.color_picker->GetColour();
        update_clear_enabled(r);
    });
    row.type_choice->on_preset_picked = [this, slot]() {
        Row& r = m_rows[slot];
        const Preset* p = r.type_choice->get_selected_preset();
        r.picked_preset = p ? p->name : std::string();
        r.type_touched  = p != nullptr;
        update_clear_enabled(r);
    };
    row.action_btn->Bind(wxEVT_BUTTON, [this, slot](wxCommandEvent&) {
        Row& r = m_rows[slot];
        r.color_picker->SetColour(UNSET_COLOR);
        r.type_choice->select_preset(std::string());
        r.picked_preset.clear();
        r.loaded_type.clear(); // an explicit Clear also drops an unresolvable legacy type
        r.color_touched = false;
        r.type_touched  = false;
        r.last_color    = UNSET_COLOR;
        update_clear_enabled(r);
    });
    update_clear_enabled(row);

    row_sizer->Add(row.color_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(row.type_choice, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(row.action_btn, 0, wxALIGN_CENTER_VERTICAL);

    target_sizer->Add(row_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
}

void FilamentInventoryEditor::open_row_editor(size_t slot)
{
    Row& row = m_rows[slot];

    // The widgets write into `row` live; restored on Cancel.
    const Row snapshot = row;

    wxDialog dlg(this, wxID_ANY, wxString::Format(_L("Edit slot %d filament"), (int) slot + 1));
    wxGetApp().UpdateDlgDarkUI(&dlg);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(dlg.FromDIP(10));
    add_row_widgets(slot, &dlg, sizer);

    auto* dlg_btns = new DialogButtons(&dlg, {"OK", "Cancel"});
    sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, dlg.FromDIP(10));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_CANCEL); });
    dlg.SetEscapeId(wxID_CANCEL);

    wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(15));
    dlg.SetSizerAndFit(outer);
    dlg.CenterOnParent();

    if (dlg.ShowModal() == wxID_OK) {
        // The widgets died with `dlg`.
        row.color_picker = nullptr;
        row.type_choice  = nullptr;
        row.action_btn   = nullptr;
    } else {
        row = snapshot; // taken before the widgets existed
    }

    refresh_grid();
}

FilamentInventory& FilamentInventoryEditor::device()
{
    return m_store.for_preset(m_printer_preset_name, m_tool_count);
}

void FilamentInventoryEditor::reload_rows_from_device()
{
    // device() never shrinks, so it has a slot per row; a changer's first sync can add more.
    const FilamentInventory& inv = device();
    m_rows.assign(inv.slots.size(), Row{});
    for (size_t i = 0; i < inv.slots.size(); ++i) {
        const PhysicalFilament& slot = inv.slots[i];
        Row&                    row  = m_rows[i];
        row.color_touched = !slot.color.empty();
        row.loaded_type   = slot.type;
        if (row.color_touched) {
            wxColour c(slot.color);
            if (c.IsOk())
                row.last_color = c;
        }

        row.picked_preset = resolve_slot_preset(slot, wxGetApp().preset_bundle->filaments);
        row.type_touched  = !row.picked_preset.empty();
    }
    refresh_grid();
}

void FilamentInventoryEditor::on_sync_from_printer(wxCommandEvent&)
{
    do_sync_from_printer(/*interactive=*/true);
}

FilamentInventoryEditor::SlotSnapshot FilamentInventoryEditor::snapshot_of(const Row& row) const
{
    SlotSnapshot snap;
    snap.picked_preset = row.picked_preset;
    snap.loaded_type   = row.loaded_type;
    if (row.color_touched)
        snap.color = row.last_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    return snap;
}

void FilamentInventoryEditor::do_sync_from_printer(bool interactive)
{
    // The shared sync writes the record and the rows are rebuilt from it; per-tool state the
    // record can't hold (tag lock, empty slot) comes back in the outcome.
    const DeviceSyncOutcome sync = sync_filament_inventory_from_printer(m_store, device());
    using Status = DeviceSyncOutcome::Status;
    if (!sync.ok()) {
        if (interactive) {
            const wxString why = sync.status == Status::NoSession    ? _L("No connected printer is available to sync from.") :
                                 sync.status == Status::FetchFailed  ? _L("Failed to read filament info from the printer.") :
                                                                       _L("The printer did not report any filament information.");
            MessageDialog(this, why, _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
        }
        return;
    }

    m_synced_baseline.clear();
    m_tag_locked_tools.clear();
    m_empty_on_printer.clear();
    if (refresh_read_only()) {
        m_read_only_note->Show(m_read_only);
        Layout();
        Fit();
    }
    reload_rows_from_device();
    for (size_t t = 0; t < sync.slots.size() && t < m_rows.size(); ++t) {
        const DeviceSlotResolution& res = sync.slots[t];
        if (!res.present)
            m_empty_on_printer.insert(t);
        if (res.tag_locked)
            m_tag_locked_tools.insert(t);
        m_synced_baseline[t] = snapshot_of(m_rows[t]);
    }
    refresh_grid();
    if (sync.status == Status::Unchanged && interactive)
        MessageDialog(this, _L("The printer did not report any filament information."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
}

bool FilamentInventoryEditor::refresh_read_only()
{
    const NetworkAgent* agent     = active_printer_session().sync_agent();
    const bool          read_only = agent == nullptr || !agent->supports_filament_push();
    const bool          changed   = read_only != m_read_only;
    m_read_only                   = read_only;
    return changed;
}

FilamentInventory FilamentInventoryEditor::edited_inventory()
{
    FilamentInventory inv = device();
    inv.ensure_slot_count(m_rows.size());
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const Row&                 row  = m_rows[i];
        PhysicalFilament&          slot = inv.slots[i];
        const Preset*              p    = resolved_preset_of(row);
        const ConfigOptionStrings* ft   = p ? p->config.option<ConfigOptionStrings>("filament_type") : nullptr;
        slot.color  = row.color_touched ? row.last_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString() : std::string();
        slot.type   = (ft && !ft->values.empty()) ? ft->values.front() : row.loaded_type;
        slot.preset = p ? p->name : std::string();
    }
    return inv;
}

void FilamentInventoryEditor::on_ok(wxCommandEvent&)
{
    // ensure_ids mints ids for filled slots saved as 0; otherwise the next load would renumber them.
    FilamentInventory& inv = device();
    inv = edited_inventory();
    inv.ensure_ids();
    save_filament_inventories(m_store);
    push_changes_to_printer();
    EndModal(wxID_OK);
}

void FilamentInventoryEditor::push_changes_to_printer()
{
    // Cleared rows are skipped: slot config can't unload filament.
    if (m_synced_baseline.empty())
        return;
    const ActivePrinterSession& session   = active_printer_session();
    NetworkAgent*               net_agent = session.sync_agent();
    MachineObject*              machine   = session.live_machine();
    if (net_agent == nullptr || !net_agent->supports_filament_push())
        return;

    wxString     failures, tag_kept;
    wxBusyCursor busy;

    const FilamentInventory& inv = device(); // on_ok saved the edits into it
    for (const auto& [tool, baseline] : m_synced_baseline) {
        if (tool >= m_rows.size())
            continue;
        const Row&   row     = m_rows[tool];
        SlotSnapshot current = snapshot_of(row);
        if (current == baseline)
            continue;
        if (m_tag_locked_tools.count(tool)) {
            tag_kept += wxString::Format(" %d", (int) tool + 1);
            continue;
        }
        const Preset* p = resolved_preset_of(row);
        if (p == nullptr)
            continue;

        IPrinterAgent::FilamentSlotInfo info;
        info.slot   = (int) tool;
        info.name   = inv.slots[tool].name;
        info.vendor = p->config.opt_string("filament_vendor", 0u);
        info.type   = inv.slots[tool].type;
        if (info.type.empty())
            continue;
        info.sub_type = derive_filament_subtype(p->alias.empty() ? p->name : p->alias, info.vendor, info.type);
        const wxColour c = row.color_touched ? row.last_color : wxColour();
        info.color_rgba = wxString::Format("%02X%02X%02XFF", c.Red(), c.Green(), c.Blue()).ToStdString();

        if (!net_agent->push_filament_info(machine->get_dev_id(), info))
            failures += wxString::Format(" %d", (int) tool + 1);
    }

    if (!failures.empty())
        MessageDialog(this, wxString::Format(_L("Could not update the printer's filament settings for these tools:%s. Your changes are saved locally."), failures),
                       _L("Printer Material Settings"), wxOK | wxICON_WARNING).ShowModal();
    if (!tag_kept.empty())
        MessageDialog(this, wxString::Format(_L("These tools read their filament from an NFC tag; the tag's values were kept on the printer:%s"), tag_kept),
                       _L("Printer Material Settings"), wxOK | wxICON_INFORMATION).ShowModal();
}

}} // namespace Slic3r::GUI
