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
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

// Orca: neutral placeholder for a slot with no color recorded yet. Distinct from the "unset"
// signal itself -- that's carried by Row::color_touched/type_touched -- this is just what the
// picker/card shows so an empty slot doesn't look like a real black/white pick.
static const wxColour UNSET_COLOR(0xD9, 0xD9, 0xD9);

// One tile in the card strip: a tool's loaded filament (slot 0, "main" size) or one of its
// the tool's loaded filament.
// Whole-card background is the filament color (or UNSET_COLOR for an untouched slot); text color
// is auto-contrast off that background, then adjusted for the app's dark/light UI via
// StateColor::darkModeColorFor, matching the house pattern used throughout AmsMappingPopup.cpp's
// MaterialItem/MappingItem family. Purely a paint+click widget -- it owns no filament data itself,
// FilamentInventoryEditor pushes content into it via set_content()/set_on_edit()/set_on_remove().
class FilamentCard : public wxPanel
{
public:
    // All cards share this width so the column's cards line up.
    // in height (less vertical content: no "T%d" top label).
    static constexpr int CARD_WIDTH = 96;

    // Height includes room for the material-type and vendor lines shown in set_content().
    FilamentCard(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(CARD_WIDTH, 82)))
    {
#ifdef __WXMSW__
        SetDoubleBuffered(true);
#endif
        m_edit_btn = new ScalableButton(this, wxID_ANY, "edit", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                         wxBU_EXACTFIT | wxNO_BORDER, true, 14);
        m_edit_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_on_edit) m_on_edit(); });

        wxBoxSizer* overlay = new wxBoxSizer(wxHORIZONTAL);
        overlay->AddStretchSpacer();
        overlay->Add(m_edit_btn, 0, wxTOP | wxRIGHT, FromDIP(2));

        wxBoxSizer* v = new wxBoxSizer(wxVERTICAL);
        v->Add(overlay, 0, wxEXPAND);
        // Not SetSizerAndFit: the card's footprint is the fixed size passed to the wxPanel ctor
        // above (the whole point of the card is a filled color swatch, not a shrink-wrap around
        // the icon row) -- SetSizer only, so the icon row overlays the top-right corner without
        // resizing the panel.
        SetSizer(v);

        Bind(wxEVT_PAINT, &FilamentCard::paintEvent, this);
    }

    // color/top_label/type_line/vendor_line/edit_disabled mirror one Row's current plain data;
    // top_label is the "T%d" slot label. type_line/vendor_line are the
    // material type ("PLA") and the vendor/brand ("PolyTerra"), shown on their own lines below
    // top_label so both can be read without opening the row editor -- a single truncated line
    // used to hide whichever of the two didn't fit first. edit_disabled covers both an NFC-tag
    // lock and a printer-reported empty tool; disabled_reason is the tooltip shown in either
    // case (ignored when edit_disabled is false).
    void set_content(const wxColour& color, const wxString& top_label, const wxString& type_line,
                      const wxString& vendor_line, bool edit_disabled, const wxString& disabled_reason = wxString())
    {
        m_color      = color;
        m_top_label  = top_label;
        m_type_line  = type_line;
        m_vendor_line = vendor_line;
        m_locked    = edit_disabled;
        m_edit_btn->Enable(!edit_disabled);
        m_edit_btn->SetToolTip(edit_disabled ? disabled_reason : _L("Edit filament"));
        m_edit_btn->SetBackgroundColour(color);
        Refresh();
    }

    void set_on_edit(std::function<void()> cb) { m_on_edit = std::move(cb); }

private:
    void paintEvent(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        render(dc);
    }

    // Orca: MSW GDI backend flicker mitigation -- every custom-painted tile in AmsMappingPopup.cpp
    // (MaterialItem::render, MappingItem::render, MappingContainer::render) redirects through an
    // offscreen wxMemoryDC + wxGCDC before blitting; mirrored here for the same reason.
    void render(wxDC& dc)
    {
#ifdef __WXMSW__
        wxSize     size = GetSize();
        wxMemoryDC memdc;
        wxBitmap   bmp(size.x, size.y);
        memdc.SelectObject(bmp);
        memdc.Blit({0, 0}, size, &dc, {0, 0});
        {
            wxGCDC dc2(memdc);
            doRender(dc2);
        }
        memdc.SelectObject(wxNullBitmap);
        dc.DrawBitmap(bmp, 0, 0);
#else
        doRender(dc);
#endif
    }

    void doRender(wxDC& dc)
    {
        const wxSize size = GetSize();
        // Orca: a white/near-white filament (or an untouched slot, UNSET_COLOR) is barely
        // distinguishable from the dialog's white background without an outline -- house pattern
        // for a subtle themed border (DragCanvas.cpp:21, CapsuleButton.cpp:75) is
        // StateColor::darkModeColorFor on a fixed light-gray hex; "#DBDBDB" is already the
        // registered "Input/Combo Box Border Color" pair (-> "#4A4A51" in dark mode), so every
        // card, including a white one, always shows a visible rounded-rect edge.
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#DBDBDB")), 1));
        dc.SetBrush(wxBrush(m_color));
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(6));

        // Auto-contrast text off the card's own background color, then run it through
        // StateColor::darkModeColorFor per the house pattern (AmsMappingPopup.cpp:377 et al.)
        // so the same tile still reads correctly when the app is switched to dark mode.
        wxColour text_color = m_color.GetLuminance() < 0.6 ? *wxWHITE : wxColour(0x26, 0x2E, 0x30);
        text_color = StateColor::darkModeColorFor(text_color);
        dc.SetTextForeground(text_color);
        dc.SetFont(GetFont());

        // Orca: label sits at the top-left, the edit/remove icons at the top-right (overlay
        // sizer, ctor above) -- reserve their cluster width plus a small gap so long names
        // truncate with an ellipsis instead of running underneath the icons (was especially
        // visible on swap cards: "Gene…" colliding with the pencil/cross). wxControl::Ellipsize
        // is the same house-consistent DC-text-truncation entry point BBLTopbar.cpp and
        // CreatePresetsDialog.cpp already use for custom-painted labels.
        int icon_cluster_width = m_edit_btn->GetSize().GetWidth() + FromDIP(2);
        const int max_text_width = std::max(0, size.x - FromDIP(6) - FromDIP(4) - icon_cluster_width);

        int y = FromDIP(8);
        if (!m_top_label.empty()) {
            wxString top_label = wxControl::Ellipsize(m_top_label, dc, wxELLIPSIZE_END, max_text_width);
            dc.DrawText(top_label, FromDIP(6), y);
            y += dc.GetTextExtent(top_label).GetHeight() + FromDIP(2);
        }
        // Material type and vendor/brand each get their own line (both independently
        // ellipsized to the icon-safe width) instead of being squeezed into one -- see
        // set_content's doc for why.
        if (!m_type_line.empty()) {
            wxString type_line = wxControl::Ellipsize(m_type_line, dc, wxELLIPSIZE_END, max_text_width);
            dc.DrawText(type_line, FromDIP(6), y);
            y += dc.GetTextExtent(type_line).GetHeight() + FromDIP(2);
        }
        if (!m_vendor_line.empty()) {
            wxString vendor_line = wxControl::Ellipsize(m_vendor_line, dc, wxELLIPSIZE_END, max_text_width);
            dc.DrawText(vendor_line, FromDIP(6), y);
        }
    }

    wxColour               m_color{UNSET_COLOR};
    wxString               m_top_label;
    wxString               m_type_line;
    wxString               m_vendor_line;
    bool                   m_locked{false};
    ScalableButton*        m_edit_btn{nullptr};
    std::function<void()>  m_on_edit;
};

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

    // device() already sizes inv.tools to at least tool_count with slot 0 present per tool
    // (FilamentInventory::deserialize's invariant, preserved by for_preset's padding). This is
    // the load-modify-save working model: every row's id/kind is copied from it verbatim and
    // never touched again unless the row is brand new (added via "Add filament" in this session,
    // see Row::is_new).
    m_store = load_filament_inventories();
    m_store.for_preset(printer_preset_name, tool_count); // creates the entry on first use
    const Preset *printer_preset = wxGetApp().preset_bundle->printers.find_preset(printer_preset_name, false);
    if (printer_preset) {
    }
    // Editing is only offered when the bound agent can actually deliver the edits to the
    // printer (supports_filament_push, today the Snapmaker U1 dialect). Printers we can only
    // READ from -- e.g. an mmu-shaped bridge like WonderSync -- open read-only: inventory and
    // sync stay useful, but nothing is editable that could never reach the machine. This is
    // an agent-capability gate, so a future AFC write dialect lifts it with no UI change.
    {
        const NetworkAgent* agent = active_printer_session().sync_agent();
        m_read_only = agent == nullptr || !agent->supports_filament_push();
    }

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(15));

    // Static label: which printer preset's inventory the cards below edit. The device combo/
    // Add/Rename affordances were dropped -- the printer preset IS the
    // device identity now, so there is nothing left to switch between from inside this dialog.
    wxBoxSizer* device_sizer = new wxBoxSizer(wxHORIZONTAL);
    device_sizer->Add(new Label(this, wxString::Format(_L("Printer: %s"), from_u8(printer_preset_name))),
                       0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(device_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    main_sizer->AddSpacer(FromDIP(15));

    // Horizontal card strip: one column per tool, each holding the tool's loaded-filament card.
    wxBoxSizer* strip_sizer = new wxBoxSizer(wxHORIZONTAL);
    main_sizer->Add(strip_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(15));

    m_tools.resize(tool_count);
    for (size_t i = 0; i < tool_count; ++i) {
        ToolGroup& group = m_tools[i];

        wxBoxSizer* col_sizer = new wxBoxSizer(wxVERTICAL);

        group.main_card = new FilamentCard(this);
        col_sizer->Add(group.main_card, 0, wxALIGN_CENTER_HORIZONTAL);
        col_sizer->AddSpacer(FromDIP(6));

        strip_sizer->Add(col_sizer, 0, wxALIGN_TOP | wxRIGHT, FromDIP(15));
    }
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

void FilamentInventoryEditor::update_card(size_t tool_idx, size_t row_idx, FilamentCard* card)
{
    const Row& row      = m_tools[tool_idx].rows[row_idx];
    const bool is_loaded = (row_idx == 0);
    const bool empty     = !row.color_touched && !row.type_touched && row.loaded_type.empty();
    const wxColour color = row.color_touched ? row.last_color : UNSET_COLOR;
    const std::string type_str   = material_type_of(row);
    const std::string vendor_str = vendor_of(row);
    const bool tag_locked = is_loaded && m_tag_locked_tools.count(tool_idx) != 0;
    // Orca: "no filament present" rule -- the last sync explicitly reported this tool's loaded
    // slot as empty, so there's nothing to set a material/color on. Only a live printer's
    // explicit report blocks editing this way; a tool never covered by a sync (offline
    // pre-configuration) is unaffected and stays fully editable, matching m_empty_on_printer's
    // per-sync-pass population in do_sync_from_printer. Superseded by tag_locked (NFC tags always
    // carry filament, so the two are not expected to overlap, but tag wording takes priority).
    const bool printer_empty = is_loaded && !tag_locked && m_empty_on_printer.count(tool_idx) != 0;
    const bool edit_disabled = m_read_only || tag_locked || printer_empty;
    wxString disabled_reason;
    if (m_read_only)
        disabled_reason = _L("This printer doesn't support writing filament settings back; the inventory is read-only.");
    else if (tag_locked)
        disabled_reason = _L("This tool's filament is set by an NFC tag and can't be edited here.");
    else if (printer_empty)
        disabled_reason = _L("No filament is loaded on this tool.");
    const wxString top_label = is_loaded ? wxString::Format("T%d", (int) tool_idx + 1) : wxString();

    card->set_content(color, top_label, empty ? _L("(empty)") : from_u8(type_str),
                       empty ? wxString() : from_u8(vendor_str), edit_disabled, disabled_reason);
    card->set_on_edit([this, tool_idx, row_idx]() { open_row_editor(tool_idx, row_idx); });
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

    update_card(tool_idx, 0, group.main_card);

    Layout();
    Fit();
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

    wxDialog dlg(this, wxID_ANY, is_loaded ? wxString::Format(_L("Edit tool %d filament"), (int) tool_idx + 1)
                                            : wxString::Format(_L("Edit swap filament (tool %d)"), (int) tool_idx + 1));
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

    for (size_t i = 0; i < m_tool_count; ++i) {
        ToolGroup& group = m_tools[i];
        group.rows.clear();
        group.rows.resize(inv.tools[i].size());
        for (size_t s = 0; s < inv.tools[i].size(); ++s) {
            const PhysicalFilament& slot = inv.tools[i][s];
            Row& row = group.rows[s];
            row.id            = slot.id;
            row.is_new        = false;
            row.kind          = slot.kind;
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
        rebuild_tool_rows(i);
    }

    Layout();
    Fit();
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
    // Orca: best-effort sync. Any failure is non-blocking and leaves the editor
    // untouched -- offline manual editing stays the baseline workflow. Sync only ever targets
    // each tool's slot-0 (loaded) row; swappable rows and their ids are left exactly as they
    // were, since the printer only ever reports what's currently loaded.
    const ActivePrinterSession& session   = active_printer_session();
    NetworkAgent*               net_agent = session.sync_agent();
    MachineObject*              machine   = session.live_machine();
    if (net_agent == nullptr) {
        if (interactive)
            MessageDialog(this, _L("No connected printer is available to sync from."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    // fetch_filament_info() is a synchronous/blocking REST call (see IPrinterAgent.hpp); pull-mode
    // agents populate the machine's DevFilaSystem before returning. Subscription-mode agents keep
    // DevFilaSystem current via MQTT pushes already, so there's nothing to fetch here. Mirrors the
    // existing pattern in Sidebar::build_filament_ams_list/load_ams_list (Plater.cpp).
    if (net_agent->get_filament_sync_mode() == FilamentSyncMode::pull) {
        if (!net_agent->fetch_filament_info(machine->get_dev_id())) {
            if (interactive)
                MessageDialog(this, _L("Failed to read filament info from the printer."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
            return;
        }
    }

    std::shared_ptr<DevFilaSystem> fila_system = machine->GetFilaSystem();
    std::map<int, DevAmsSlotId> tray_map = fila_system ? fila_system->GetTrayIndexMap() : std::map<int, DevAmsSlotId>();

    // Orca: tray -> physical tool correspondence is ambiguous for toolchangers (DevFilaSystem's
    // trays are AMS-shaped, not tool-shaped). Best-effort: real AMS trays, sorted by tray index,
    // map 1:1 onto tool rows up to tool_count. Rows beyond the reported tray count, and trays
    // beyond the row count, are left alone.
    //
    // GetTrayIndexMap() unconditionally seeds two virtual/external-spool pseudo-tray entries
    // (VIRTUAL_TRAY_MAIN_ID/DEPUTY_ID) alongside any real AMS trays, so the map is never actually
    // empty -- it can't be used as an "any data?" signal. Those pseudo-trays live in
    // MachineObject::vt_slot, not in DevFilaSystem's amsList, so GetAmsTray() below can never
    // resolve them anyway; skip them explicitly (rather than relying on that null result) so they
    // don't silently consume a tool row on AMS-less toolchangers, and count rows actually applied
    // to know whether the sync produced anything.
    size_t tool_idx     = 0;
    size_t rows_applied = 0;
    m_synced_baseline.clear();
    m_tag_locked_tools.clear();
    m_empty_on_printer.clear();
    std::set<size_t> tools_to_refresh; // cards to repaint once, after all tray data is applied
    for (const auto& [tray_index, slot_id] : tray_map) {
        if (devPrinterUtil::IsVirtualSlot(slot_id.first))
            continue;
        if (tool_idx >= m_tools.size())
            break;
        Row&   row       = m_tools[tool_idx].rows[0]; // slot 0 = loaded; sync never touches swappable rows
        size_t this_tool = tool_idx;
        ++tool_idx;
        tools_to_refresh.insert(this_tool);

        DevAmsTray* tray = fila_system->GetAmsTray(std::to_string(slot_id.first), std::to_string(slot_id.second));
        if (!tray || !tray->is_exists) {
            // The printer reported this slot with no filament loaded. Sync mirrors the machine:
            // clear the row rather than leaving stale data (an unreported slot never reaches
            // here -- tray_map only carries what the agent published). Also block editing this
            // tool's material/color until a later sync reports filament (m_empty_on_printer,
            // consumed by update_card) -- there's nothing physically present to set either on.
            bool was_set = row.color_touched || row.type_touched || !row.loaded_type.empty();
            if (was_set) {
                row.last_color     = UNSET_COLOR;
                row.picked_preset.clear();
                row.loaded_type.clear();
                row.color_touched = false;
                row.type_touched  = false;
                ++rows_applied;
            }
            m_empty_on_printer.insert(this_tool);
            m_synced_baseline[this_tool] = snapshot_of(row); // baseline: empty slot
            continue;
        }

        bool applied = false;
        if (!tray->color.empty()) {
            row.last_color     = tray->get_color();
            row.color_touched = true;
            applied = true;
        }
        const std::string type = tray->get_filament_type();
        {
            // Best target first: the agent may have resolved an exact profile for this spool
            // (DevAmsTray::setting_id carries the filament_id it matched -- see e.g.
            // SnapmakerPrinterAgent::fetch_filament_info's vendor/type/color lookup). Fall back
            // to the material's Generic preset (alias-aware); if neither is installed, remember
            // the type for save (loaded_type) without changing the visible selection.
            const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
            const Preset* target = nullptr;
            if (!tray->setting_id.empty())
                for (auto it = filaments.begin(); it != filaments.end(); ++it)
                    if (it->filament_id == tray->setting_id && it->is_visible && it->is_compatible) { target = &*it; break; }
            if (target == nullptr)
                target = find_generic_filament_preset(type, filaments);

            if (target != nullptr) {
                row.picked_preset = target->name;
                row.type_touched  = true;
                applied = true;
            }
            if (!type.empty())
                row.loaded_type = type;
        }
        if (applied)
            ++rows_applied;
        m_synced_baseline[this_tool] = snapshot_of(row);
        // Non-zero tag_uid = the slot's data came from an NFC tag (see SnapmakerPrinterAgent's
        // fetch); the tag is authoritative, so this tool is excluded from push_changes_to_printer.
        if (!tray->tag_uid.empty() && tray->tag_uid.find_first_not_of('0') != std::string::npos)
            m_tag_locked_tools.insert(this_tool);
    }

    // Orca: write-through to the registry immediately, for every tool this sync actually covered
    // (always favor what's reported from the printer over a stale local
    // view -- OK's local-save role becomes redundant for these tools; its real job when connected
    // is the push_changes_to_printer diff below, unchanged). Without this, every consumer that
    // reads the registry (the mapping dialog's target options, compute_physical_map_proposal, the
    // auto-mapper, the Slicing Result summary) would keep seeing whatever was last saved by OK
    // until the user opens this dialog and presses OK again. Tools never covered by this sync
    // (tray_map ran out, or the printer never reported them) are untouched, matching the class's
    // existing "no baseline = no connection-known state" rule for push_changes_to_printer.
    if (!tools_to_refresh.empty()) {
        FilamentInventory& inv = device();
        for (size_t t : tools_to_refresh)
            inv.apply_synced_loaded_slot(t, slot_from_row(m_tools[t].rows[0]));
        inv.next_id = m_next_id;
        inv.ensure_ids();
        m_next_id = inv.next_id;
        // Adopt whatever id ensure_ids settled on (freshly minted for a new filament, or zeroed
        // for a clear) so a later on_ok save -- which rebuilds its own PhysicalFilament from
        // row.id -- reuses the same id this write-through just persisted, instead of minting a
        // second, different one for the same physical slot.
        for (size_t t : tools_to_refresh)
            m_tools[t].rows[0].id = inv.tools[t][0].id;
        save_filament_inventories(m_store);
    }

    for (size_t t : tools_to_refresh)
        rebuild_tool_rows(t);

    if (rows_applied == 0 && interactive)
        MessageDialog(this, _L("The printer did not report any filament information."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
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
    return build_physical_filament(color, type, preset, row.id, row.kind);
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
    FilamentInventory inv;
    inv.next_id = m_next_id;
    inv.tools.resize(m_tools.size());
    for (size_t i = 0; i < m_tools.size(); ++i) {
        const ToolGroup& group = m_tools[i];
        inv.tools[i].resize(1); // slot 0 (loaded) always present
        for (size_t r = 0; r < group.rows.size(); ++r) {
            const Row& row = group.rows[r];
            PhysicalFilament slot = slot_from_row(row);
            if (r == 0)
                inv.tools[i][0] = slot;
            else if (!slot.empty())
                inv.tools[i].push_back(slot); // an added row left fully empty isn't a filament
        }
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
