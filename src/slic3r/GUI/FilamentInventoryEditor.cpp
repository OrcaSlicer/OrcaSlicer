#include "FilamentInventoryEditor.hpp"

#include <algorithm>
#include <functional>

#include <wx/clrpicker.h>
#include <wx/dcgraph.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include "ActivePrinterSession.hpp"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#include "FilamentInventoryStore.hpp"
#include "SlotGridPanel.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Tab.hpp"
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
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

// Orca: neutral placeholder for a slot with no color recorded yet. Distinct from the "unset"
// signal itself -- that's carried by Row::color_touched/type_touched -- this is just what the
// picker/card shows so an empty slot doesn't look like a real black/white pick.
static const wxColour UNSET_COLOR(0xD9, 0xD9, 0xD9);

FilamentInventoryEditor::FilamentInventoryEditor(wxWindow* parent, const std::string& printer_preset_name, size_t tool_count)
    : wxDialog(parent, wxID_ANY, _L("Printer Material Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_printer_preset_name(printer_preset_name)
{
    SetBackgroundColour(*wxWHITE);

    // Addressable tool count, resolved by the caller (addressable_tool_count_of) -- normally the
    // printer's physical nozzle count; devices that address more logical tools than nozzles
    // report their logical count. Fixed for the dialog's lifetime: the device itself can't change
    // mid-dialog anymore (its identity is this printer preset), so there's nothing that could
    // need a different tool count later.
    m_tool_count  = tool_count;

    // device() already sizes inv.slots to at least tool_count (FilamentInventory::deserialize's
    // padding, preserved by for_preset). This is
    // the load-modify-save working model: every row's id/kind is copied from it verbatim and
    // never touched again unless the row is brand new (added via "Add filament" in this session,
    // see Row::is_new).
    m_store = load_filament_inventories();
    m_store.for_preset(printer_preset_name, tool_count); // creates the entry on first use
    const Preset *printer_preset = wxGetApp().preset_bundle->printers.find_preset(printer_preset_name, false);
    if (printer_preset) {
    }
    // Editing is only offered when the bound agent can actually deliver the edits to the
    // printer (supports_filament_push). Printers we can only READ from open read-only:
    // inventory and sync stay useful, but nothing is editable that could never reach the
    // machine. Before the first sync the agent may not know the changer yet, so this is
    // re-evaluated after every sync (refresh_read_only) -- the pencils used to be missing on
    // the first open and present on the second.
    refresh_read_only();

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(15));

    // Static label: which printer preset's inventory the cards below edit. The device combo/
    // Add/Rename affordances were dropped -- the printer preset IS the
    // device identity now, so there is nothing left to switch between from inside this dialog.
    wxBoxSizer* device_sizer = new wxBoxSizer(wxHORIZONTAL);
    device_sizer->Add(new Label(this, wxString::Format(_L("Printer: %s"), from_u8(printer_preset_name))),
                       0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(device_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    {
        // The cards carry no edit affordance while read-only (see FilamentCard::set_content);
        // this note is the visible explanation a disabled-control tooltip can't give on MSW.
        m_read_only_note = new Label(this, _L("Materials are reported by the printer and can't be edited here."));
        m_read_only_note->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        main_sizer->AddSpacer(FromDIP(6));
        main_sizer->Add(m_read_only_note, 0, wxLEFT | wxRIGHT, FromDIP(15));
        m_read_only_note->Show(m_read_only);
    }
    main_sizer->AddSpacer(FromDIP(15));

    // The printer's slots, drawn where the printer has them (SlotGridPanel). A multi-extruder
    // printer with a reported changer (MEMM) gets the extruder row and E<n> on the tiles.
    {
        const DynamicPrintConfig* cfg = printer_preset ? &printer_preset->config : nullptr;
        const auto* nozzles = cfg ? cfg->option<ConfigOptionFloats>("nozzle_diameter") : nullptr;
        m_extruder_count    = nozzles && !nozzles->empty() ? nozzles->size() : 1;
        const bool memm     = m_extruder_count > 1 && cfg && !reported_changer_of(*cfg).empty();
        SlotGridOptions opts;
        opts.show_extruder_row     = memm;
        opts.show_extruder_on_tile = memm;
        opts.show_edit_pencil      = true;
        m_grid = new SlotGridPanel(this, opts);
        m_grid->SetOnEditClicked([this](size_t slot) { open_row_editor(slot, 0); });
        main_sizer->Add(m_grid, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    }
    m_tools.resize(tool_count);
    main_sizer->AddSpacer(FromDIP(15));

    // Seeds every tool group's rows from the current device and paints their cards.
    reload_rows_from_device();

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->AddStretchSpacer();

    // Orca: "Sync from printer" is the leftmost entry; OK/Cancel stay last. left_aligned_buttons_count
    // = 1 keeps it pinned left of the stretch spacer while OK/Cancel are pushed right as usual.
    auto* dlg_btns = new DialogButtons(bottom_panel, {"Sync from printer", "OK", "Cancel"}, "", 1);
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    // Orca: only offer sync when a connected machine actually supports it ("Sync from
    // printer" is best-effort, offline editing stays the baseline). Recomputed once here; if the
    // machine connects/disconnects while the dialog is open the button state won't follow it.
    Button* sync_btn = dlg_btns->GetButtonFromIndex(0);
    const ActivePrinterSession& session   = active_printer_session();
    MachineObject*              machine   = session.live_machine();
    NetworkAgent*               net_agent = session.sync_agent();
    // is_connected() is a liveness heartbeat (last status update within a timeout). Pull-mode
    // agents fetch on demand over REST, so a stale heartbeat (app just started, poll interval
    // elapsed) must not disable the button -- a selected machine is enough, and a truly
    // unreachable printer surfaces through fetch_filament_info's own error dialog. Only
    // subscription mode, whose data freshness comes from the live connection, keeps the
    // heartbeat requirement.
    FilamentSyncMode sync_mode = net_agent ? net_agent->get_filament_sync_mode() : FilamentSyncMode::none;
    // A live session is the same "is there anyone to sync/push against at all" bar
    // do_sync_from_printer's own guard uses. A live printer that just doesn't support sync (wrong
    // sync_mode, or a subscription-mode printer with a stale heartbeat) keeps its own, more
    // specific tooltip below -- only a genuinely offline project (no printer corresponding to the
    // active profile) gets the "saved locally" wording, since that's the only case where there is
    // truly nothing to connect to.
    const bool live_context = net_agent != nullptr;
    m_sync_available = live_context && sync_mode != FilamentSyncMode::none &&
                       (sync_mode == FilamentSyncMode::pull || machine->is_connected());
    sync_btn->Enable(m_sync_available);
    if (!m_sync_available)
        sync_btn->SetToolTip(live_context
            ? _L("Connect to a printer that supports reading loaded filaments to use this.")
            : _L("No printer connected — settings are saved locally."));
    sync_btn->Bind(wxEVT_BUTTON, &FilamentInventoryEditor::on_sync_from_printer, this);

    // The printer is the source of truth for what's physically loaded: when a connection is
    // available, always read on open (after the dialog paints). The read also records each
    // tool's baseline so OK can push back only what the user actually changed.
    if (m_sync_available)
        CallAfter([this]() {
            wxBusyCursor busy;
            do_sync_from_printer(/*interactive=*/false);
        });

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &FilamentInventoryEditor::on_ok, this);
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    SetEscapeId(wxID_CANCEL);

    SetSizer(main_sizer);
    Layout();
    Fit();

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

wxColour FilamentInventoryEditor::effective_color_of(const Row& row) const
{
    return row.color_picker ? row.color_picker->GetColour() : row.last_color;
}

std::string FilamentInventoryEditor::material_type_of(const Row& row) const
{
    // Prefer the resolved preset's own filament_type option (matches slot_from_row's type
    // resolution) so a preset whose type differs from the row's stale loaded_type still shows
    // the current value; fall back to loaded_type for an unresolved/legacy row.
    if (const Preset* p = resolved_preset_of(row)) {
        const ConfigOptionStrings* ft = p->config.option<ConfigOptionStrings>("filament_type");
        if (ft && !ft->values.empty())
            return ft->values.front();
    }
    return row.loaded_type;
}

std::string FilamentInventoryEditor::vendor_of(const Row& row) const
{
    // Orca: delegates to the single shared filament_vendor_of (FilamentInventoryStore) --
    // also used by PhysicalFilamentComboBox's vendor grouping, so the two can never disagree on
    // the same preset. A bare loaded_type (no resolved preset) carries no vendor to show.
    const Preset* p = resolved_preset_of(row);
    return p ? filament_vendor_of(*p) : std::string();
}

void FilamentInventoryEditor::refresh_grid()
{
    // The grid draws what the rows say now (an edit not yet saved included), on the topology the
    // record carries for each slot.
    FilamentInventory shown = device();
    shown.ensure_slot_count(m_tools.size());
    for (size_t i = 0; i < m_tools.size(); ++i) {
        if (m_tools[i].rows.empty()) continue;
        const PhysicalFilament& was  = shown.slots[i];
        PhysicalFilament        slot = slot_from_row(m_tools[i].rows[0]);
        slot.unit = was.unit; slot.unit_label = was.unit_label; slot.head = was.head; slot.slot = was.slot;
        slot.extruder = was.extruder; slot.virtual_tool = was.virtual_tool;
        shown.slots[i] = slot;
    }
    std::vector<SlotGridSlot> rows = slot_grid_rows(shown, wxGetApp().preset_bundle->filaments);
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool tag_locked = m_tag_locked_tools.count(i) != 0;
        // Orca: "no filament present" rule -- the last sync explicitly reported this slot as
        // empty, so there's nothing to set a material/color on. Only a live printer's explicit
        // report blocks editing this way; a slot never covered by a sync (offline
        // pre-configuration) stays fully editable. Tag wording takes priority over it.
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

void FilamentInventoryEditor::rebuild_tool_rows(size_t tool_idx)
{
    ToolGroup& group = m_tools[tool_idx];

    // Capture each row's current widget state into plain data *before* destroying any live
    // row-edit widgets below -- this only matters for a row whose small editor dialog happens to
    // be open (its widgets are children of that dialog, not of this rebuild's targets), but is
    // harmless (a no-op) for every other row since their widget pointers are already null.
    for (Row& row : group.rows) {
        if (row.color_picker)
            row.last_color = row.color_picker->GetColour();
        if (row.type_choice) {
            const Preset* p    = row.type_choice->get_selected_preset();
            row.picked_preset  = p ? p->name : std::string();
        }
    }

    refresh_grid();
}

void FilamentInventoryEditor::add_row_widgets(size_t tool_idx, size_t row_idx, wxWindow* parent, wxSizer* target_sizer)
{
    ToolGroup& group = m_tools[tool_idx];
    Row& row = group.rows[row_idx];
    const bool is_loaded = (row_idx == 0); // slot 0 = loaded, always present, gets a Clear button

    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

    row.color_picker = new wxColourPickerCtrl(parent, wxID_ANY, row.last_color);

    // The same grouped preset selector as the sidebar's filament section (user/system groups,
    // color chips) -- selection is row state only, never the project's filament_presets. Bare
    // material types are not offered: every material has a Generic <type> preset.
    row.type_choice = new PhysicalFilamentComboBox(parent);
    row.type_choice->select_preset(row.type_touched ? row.picked_preset : std::string());

    if (is_loaded) {
        auto* clear_btn = new Button(parent, _L("Clear"));
        clear_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        row.action_btn = clear_btn;
    }
    // Swap rows (row_idx > 0) get no action button here -- their Remove affordance lives on the
    // card itself (FilamentCard's own remove icon), not inside this per-row editor.

    row.color_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this, tool_idx, row_idx](wxColourPickerEvent&) {
        Row& r = m_tools[tool_idx].rows[row_idx];
        r.color_touched = true;
        r.last_color    = r.color_picker->GetColour();
        if (row_idx == 0)
            update_clear_enabled(r);
    });
    row.type_choice->on_preset_picked = [this, tool_idx, row_idx]() {
        Row& r = m_tools[tool_idx].rows[row_idx];
        const Preset* p = r.type_choice->get_selected_preset();
        r.picked_preset = p ? p->name : std::string();
        r.type_touched  = p != nullptr;
        if (row_idx == 0)
            update_clear_enabled(r);
    };
    if (is_loaded) {
        row.action_btn->Bind(wxEVT_BUTTON, [this, tool_idx](wxCommandEvent&) {
            Row& r = m_tools[tool_idx].rows[0];
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
    }

    row_sizer->Add(row.color_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(row.type_choice, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    if (row.action_btn)
        row_sizer->Add(row.action_btn, 0, wxALIGN_CENTER_VERTICAL);

    target_sizer->Add(row_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
}

void FilamentInventoryEditor::open_row_editor(size_t tool_idx, size_t row_idx)
{
    ToolGroup& group = m_tools[tool_idx];
    Row&       row   = group.rows[row_idx];
    const bool is_loaded = (row_idx == 0);

    // The row-edit machinery below writes live into `row` as the user interacts with it; on
    // Cancel, restore this snapshot so an edit the user backs out of doesn't stick.
    const Row snapshot = row;

    wxDialog dlg(this, wxID_ANY, is_loaded ? wxString::Format(_L("Edit slot %d filament"), (int) tool_idx + 1)
                                            : wxString::Format(_L("Edit swap filament (slot %d)"), (int) tool_idx + 1));
    wxGetApp().UpdateDlgDarkUI(&dlg);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(dlg.FromDIP(10));
    add_row_widgets(tool_idx, row_idx, &dlg, sizer);

    auto* dlg_btns = new DialogButtons(&dlg, {"OK", "Cancel"});
    sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, dlg.FromDIP(10));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_CANCEL); });
    dlg.SetEscapeId(wxID_CANCEL);

    wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(15));
    dlg.SetSizerAndFit(outer);
    dlg.CenterOnParent();

    const int result = dlg.ShowModal();

    // The row's widgets are children of `dlg` and are destroyed with it; row-edit lambdas bound
    // above (color_picker/type_choice/Clear) must never fire again after this point.
    row.color_picker = nullptr;
    row.type_choice   = nullptr;
    row.action_btn    = nullptr;

    if (result != wxID_OK) {
        row.color_touched = snapshot.color_touched;
        row.type_touched  = snapshot.type_touched;
        row.last_color     = snapshot.last_color;
        row.picked_preset  = snapshot.picked_preset;
        row.loaded_type     = snapshot.loaded_type;
    }

    rebuild_tool_rows(tool_idx);
}

FilamentInventory& FilamentInventoryEditor::device()
{
    // m_printer_preset_name never changes mid-dialog now that the device combo is gone, so this
    // always resolves the same entry -- for_preset just pads it to m_tool_count on every call.
    return m_store.for_preset(m_printer_preset_name, m_tool_count);
}

void FilamentInventoryEditor::reload_rows_from_device()
{
    const FilamentInventory& inv = device();
    m_next_id = inv.next_id;

    // One group per reported slot: the printer can report more slots than the dialog was
    // opened with (a first sync on a changer), so the groups follow the inventory.
    if (m_tools.size() < inv.slots.size())
        m_tools.resize(inv.slots.size());
    for (size_t i = 0; i < m_tools.size(); ++i) {
        ToolGroup& group = m_tools[i];
        group.rows.clear();
        group.rows.resize(1);
        if (i < inv.slots.size()) {
            const PhysicalFilament& slot = inv.slots[i];
            Row& row = group.rows[0];
            row.id            = slot.id;
            row.is_new        = false;
            row.kind          = slot.kind;
            row.slot_name     = slot.name;
            row.unit          = slot.unit;
            row.head          = slot.head;
            row.color_touched = !slot.color.empty();
            row.loaded_type   = slot.type;
            if (row.color_touched) {
                wxColour c(slot.color);
                if (c.IsOk())
                    row.last_color = c;
            }

            // Row seeding: the slot's preset if installed, else "Generic <type>" (deleted
            // profile / legacy type-only slot), else unselected -- exactly resolve_slot_preset's
            // contract. loaded_type (above) keeps an unresolvable legacy type alive for on_ok.
            row.picked_preset = resolve_slot_preset(slot, wxGetApp().preset_bundle->filaments);
            row.type_touched  = !row.picked_preset.empty();
        }
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
        snap.color = effective_color_of(row).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    return snap;
}

void FilamentInventoryEditor::do_sync_from_printer(bool interactive)
{
    // One sync for every surface (sync_filament_inventory_from_printer): it writes the record --
    // slot names, the same-spool guard, the changer dialect, the protocol seeding -- and this
    // dialog then rebuilds its rows FROM the record, so the two can never disagree. What the
    // record cannot hold (a tag-locked tool, an empty slot on the printer) comes back per tool.
    const DeviceSyncOutcome sync = sync_filament_inventory_from_printer(m_store, device(), m_tool_count);
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
    // The fetch just told the agent which changer it is talking to; the pencils follow.
    if (refresh_read_only()) {
        m_read_only_note->Show(m_read_only);
        Layout();
        Fit();
    }
    reload_rows_from_device();
    for (size_t t = 0; t < sync.slots.size() && t < m_tools.size(); ++t) {
        const DeviceSlotResolution& res = sync.slots[t];
        if (!res.present)
            m_empty_on_printer.insert(t); // block editing until a later sync reports filament
        // The tag is authoritative, so this tool is excluded from push_changes_to_printer.
        if (res.tag_locked)
            m_tag_locked_tools.insert(t);
        // Baseline: what the printer has now; a later push sends only rows that differ from it.
        m_synced_baseline[t] = snapshot_of(m_tools[t].rows[0]);
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

PhysicalFilament FilamentInventoryEditor::slot_from_row(const Row& row) const
{
    // The row's plain data IS the "set" signal (kept live-synchronized by add_row_widgets'
    // bindings and reconciled on modal Cancel by open_row_editor), so a stale flag can
    // never silently drop a pick. No selection keeps the row's unresolvable legacy type
    // (loaded_type), which an explicit Clear empties.
    const std::string color = row.color_touched ? effective_color_of(row).GetAsString(wxC2S_HTML_SYNTAX).ToStdString() : std::string();
    const Preset*     p     = resolved_preset_of(row);
    std::string        preset, type;
    if (p != nullptr) {
        preset = p->name;
        const ConfigOptionStrings* ft = p->config.option<ConfigOptionStrings>("filament_type");
        type   = (ft && !ft->values.empty()) ? ft->values.front() : row.loaded_type;
    } else {
        type = row.loaded_type;
    }
    PhysicalFilament slot = build_physical_filament(color, type, preset, row.id, row.kind);
    slot.name             = row.slot_name;
    slot.unit             = row.unit;
    slot.head             = row.head;
    return slot;
}

void FilamentInventoryEditor::on_ok(wxCommandEvent&)
{
    // Load-modify-save: rows keep the id they were loaded with; ensure_ids below mints one for
    // every non-empty slot that lacks one -- rows added this session AND slot-0 rows first
    // filled on a fresh inventory (which load as id 0; saving them as id 0 would make
    // deserialize's reconcile pass renumber them on the next launch, silently invalidating
    // every plate filament_physical_map entry that pointed at them). A cancelled dialog never
    // advances the on-disk allocator. Every synced tool's slot 0 was already write-through-saved
    // by do_sync_from_printer as the sync happened (see its own comment) -- this pass still
    // recomputes and saves it from the row again, which is redundant but harmless (same data,
    // same id) whenever nothing was edited since; it stays authoritative for anything the sync
    // path doesn't cover (offline edits, swap rows, tools never synced).
    FilamentInventory inv = device(); // keeps dialect and every slot's topology
    inv.next_id = m_next_id;
    inv.ensure_slot_count(m_tools.size());
    for (size_t i = 0; i < m_tools.size(); ++i) {
        const PhysicalFilament& was  = inv.slots[i];
        PhysicalFilament        slot = slot_from_row(m_tools[i].rows[0]);
        slot.unit         = was.unit;
        slot.unit_label   = was.unit_label;
        slot.head         = was.head;
        slot.slot         = was.slot;
        slot.extruder     = was.extruder;
        slot.virtual_tool = was.virtual_tool;
        inv.slots[i]      = slot;
    }
    inv.ensure_ids();
    device() = inv;
    save_filament_inventories(m_store);
    push_changes_to_printer();
    EndModal(wxID_OK);
}

void FilamentInventoryEditor::push_changes_to_printer()
{
    // Best-effort write-back of USER CHANGES only: a tool is pushed when a sync recorded its
    // baseline this session (no baseline = no known printer state = nothing to diff against)
    // and the slot-0 row now differs from it. Swappable rows are never pushed -- the printer
    // models one loaded filament per tool. Tag-locked tools are skipped: the NFC tag is
    // authoritative. Cleared rows are skipped too -- slot config can't unload filament.
    if (m_synced_baseline.empty())
        return;
    const ActivePrinterSession& session   = active_printer_session();
    NetworkAgent*               net_agent = session.sync_agent();
    MachineObject*              machine   = session.live_machine();
    if (net_agent == nullptr || !net_agent->supports_filament_push())
        return;

    wxString     failures, tag_kept;
    wxBusyCursor busy;

    for (const auto& [tool, baseline] : m_synced_baseline) {
        if (tool >= m_tools.size() || m_tools[tool].rows.empty())
            continue;
        const Row&   row     = m_tools[tool].rows[0];
        SlotSnapshot current = snapshot_of(row);
        if (current == baseline)
            continue;
        if (m_tag_locked_tools.count(tool)) {
            tag_kept += wxString::Format(" %d", (int) tool + 1);
            continue;
        }
        const Preset* p = resolved_preset_of(row);
        if (p == nullptr)
            continue; // cleared/unresolvable row: nothing representable to write

        IPrinterAgent::FilamentSlotInfo info;
        info.slot   = (int) tool;
        info.name   = row.slot_name; // the printer's own slot name, as the sync recorded it
        info.vendor = p->config.opt_string("filament_vendor", 0u);
        const ConfigOptionStrings* ft = p->config.option<ConfigOptionStrings>("filament_type");
        info.type = (ft && !ft->values.empty()) ? ft->values.front() : row.loaded_type;
        if (info.type.empty())
            continue;
        info.sub_type = derive_filament_subtype(p->alias.empty() ? p->name : p->alias, info.vendor, info.type);
        const wxColour c = row.color_touched ? effective_color_of(row) : wxColour();
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
