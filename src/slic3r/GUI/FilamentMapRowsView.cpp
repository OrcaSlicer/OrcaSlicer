#include "FilamentMapRowsView.hpp"

#include "3DScene.hpp"       // adjust_color_for_rendering
#include "ActivePrinterSession.hpp"
#include "FilamentInventoryStore.hpp"
#include "FilamentMapPanel.hpp"   // wxEVT_INVALID_MANUAL_MAP
#include "FilamentMapRowsPanel.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp" // ThumbnailPanel (REUSE-AS-IS), see update_preview()
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>

#include <algorithm>
#include <unordered_map>

namespace Slic3r { namespace GUI {

// Orca: maximum plate-filament rows shown before FilamentMapRowsPanel is wrapped in a scrolled
// window. Keeps the host's height bounded regardless of how many filaments a plate uses.
static constexpr size_t MAX_VISIBLE_MAP_ROWS = 8;

// Height of one wrapped tile row in FilamentMapRowsPanel's tiles_sizer -- the tile itself is a
// fixed FromDIP(50), plus the FromDIP(5) top/bottom padding tiles_sizer adds around every tile.
// Used to size the scroll wrapper to a whole number of tile rows -- NOT
// GetBestSize().GetHeight() / filament_count, which assumed one row per filament and silently
// collapsed once tiles started wrapping horizontally.
static constexpr int TILE_ROW_HEIGHT_DIP = 60;

FilamentMapRowsView::FilamentMapRowsView(wxWindow                       *parent,
                                         const std::vector<std::string> &filament_color,
                                         const std::vector<std::string> &filament_type,
                                         const std::vector<std::string> &filament_names,
                                         const std::vector<int>         &filament_map,
                                         const std::vector<int>         &physical_filament_map,
                                         const std::vector<int>         &plate_filaments,
                                         const FilamentInventory         &inventory,
                                         size_t                           tool_count,
                                         const std::string               &printer_preset_name,
                                         bool                             filament_map_confirmed,
                                         std::function<void(bool)>        on_state_changed)
    : wxPanel(parent, wxID_ANY)
    , m_filament_names(filament_names)
    , m_plate_filaments(plate_filaments)
    , m_inventory(inventory)
    , m_tool_count(tool_count)
    , m_printer_preset_name(printer_preset_name)
    , m_filament_map(filament_map)
    , m_physical_filament_map(physical_filament_map)
    , m_filament_color(filament_color)
    , m_filament_type(filament_type)
    , m_on_state_changed(std::move(on_state_changed))
{
    SetBackgroundColour(*wxWHITE);

    m_filament_preset = wxGetApp().preset_bundle->filament_presets;
    // Orca: needed so the proposal uses ProjectFilamentInfo::vendor as a tie-break.
    // PresetBundle::full_config() returns BY VALUE: the result must be held in a named variable
    // before any option pointer into it is dereferenced, or the vector is copied out of destroyed
    // storage.
    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    if (const auto *vendors = full_config.option<ConfigOptionStrings>("filament_vendor"))
        m_filament_vendor = vendors->values;

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    m_rows_holder_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_rows_holder_sizer, 0, wxEXPAND);

    m_inventory_was_all_unset = inventory_all_unset(m_inventory);
    std::vector<int> proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor, m_filament_preset,
                                                              m_plate_filaments, m_physical_filament_map, m_filament_map, m_inventory,
                                                              filament_map_confirmed);
    m_initial_proposal          = proposal;
    // A never-confirmed plate's proposal is entirely auto-matcher output, so every row it seeds
    // starts with its auto-matched badge shown.
    m_initial_proposal_was_auto = !filament_map_confirmed;
    rebuild_rows_panel(proposal, m_initial_proposal_was_auto);

    // Orca: offer to persist the current picks as the printer's loaded-filament inventory when the
    // inventory used to build the proposal was empty; visibility is re-evaluated in seed_status()
    // since it depends on the (changing) row state too.
    {
        wxPanel *record_panel = new wxPanel(this);
        record_panel->SetBackgroundColour(*wxWHITE);
        auto *record_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_record_checkbox  = new CheckBox(record_panel);
        m_record_checkbox->SetValue(true);
        record_sizer->Add(m_record_checkbox, 0, wxALIGN_CENTER_VERTICAL);
        auto *record_label = new Label(record_panel, _L("Remember these as the loaded filaments"));
        record_label->SetFont(Label::Body_12);
        record_sizer->Add(record_label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
        record_panel->SetSizer(record_sizer);
        m_record_row = record_panel;
        main_sizer->AddSpacer(FromDIP(8));
        main_sizer->Add(m_record_row, 0);
    }

    // Orca: snapshot the current plate's cached renders ONCE and recolor from the snapshot from
    // then on -- this view must never trigger a render of its own (see update_preview).
    if (Plater *preview_plater = wxGetApp().plater()) {
        if (PartPlate *curr_plate = preview_plater->get_partplate_list().get_curr_plate()) {
            if (curr_plate->thumbnail_data.is_valid() && curr_plate->no_light_thumbnail_data.is_valid()) {
                m_preview_lit      = curr_plate->thumbnail_data;
                m_preview_no_light = curr_plate->no_light_thumbnail_data;
            }
        }
    }

    main_sizer->AddSpacer(FromDIP(8));
    m_preview_panel = new ThumbnailPanel(this);
    // Orca: ThumbnailPanel mis-parents its internal wxStaticBitmap to the panel's PARENT (us)
    // while positioning it with the panel's own sizer, so an empty static bitmap floats at
    // top-left of THIS view, above the row tiles in z-order -- painting a grey square over the
    // first wrap-row (filaments 1/2 invisible-but-clickable on Windows; the offset grey square
    // beside the preview in the same reports). It never displays anything -- set_thumbnail()
    // draws through the panel's own paint, not this child -- so hide the stray. Latent upstream
    // in SelectMachine, where the misapplied coordinates happen to land harmlessly.
    if (m_preview_panel->m_staticbitmap != nullptr)
        m_preview_panel->m_staticbitmap->Hide();
    m_preview_panel->SetMinSize(wxSize(FromDIP(180), FromDIP(180)));
    m_preview_panel->SetMaxSize(wxSize(FromDIP(180), FromDIP(180)));
    m_preview_panel->Hide();
    main_sizer->Add(m_preview_panel, 0, wxALIGN_CENTER_HORIZONTAL);

    SetSizerAndFit(main_sizer);

    // Re-pin the rows panel's honest height whenever our width changes -- how many rows the
    // tiles wrap into (and thus the content height) depends on it. MSW delivers size events
    // synchronously during the host dialog's own Fit/resize, so the host's next measurement
    // already sees the corrected min.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        e.Skip();
        repin_rows_min_height();
    });

    // m_rows_panel's own construction stays silent (no wxEVT_INVALID_MANUAL_MAP), so seed the
    // initial state explicitly.
    seed_status();

    // Pin the width only NOW. FilamentMapRowsPanel wraps its tiles, so its best size depends on
    // the width it is offered: with no floor, each later Fit() of the host re-measures it at the
    // width the previous Fit() chose and ratchets the dialog narrower. The floor has to be taken
    // after seed_status, because the profile-mismatch warning is a single unwrapped line that is
    // wider than the tile grid and does not exist until then -- measuring before it was set left
    // the dialog too narrow to show the end of the warning.
    Fit();
    SetMinSize(wxSize(GetSize().GetWidth(), -1));
}

bool FilamentMapRowsView::AllRowsAssigned() const
{
    return m_rows_panel != nullptr && m_rows_panel->AllRowsAssigned();
}

void FilamentMapRowsView::rebuild_rows_panel(const std::vector<int> &proposal, bool auto_matched)
{
    // Clear(true) destroys the previous panel (and any scroll wrapper) along with its sizer
    // item; m_rows_panel itself is about to be reassigned below.
    m_rows_holder_sizer->Clear(true);

    m_rows_panel = new FilamentMapRowsPanel(this, m_filament_color, m_filament_type, m_filament_names,
                                            m_plate_filaments, proposal, m_inventory, build_slot_preset_names(), m_tool_count,
                                            auto_matched);
    // Orca: every rebuild starts disarmed -- on_apply_automatic re-arms it right after calling
    // this, so a rebuild for any other reason (ctor, Reset, an inventory edit) always lands
    // disarmed. See m_live_auto_pending's doc.
    m_live_auto_pending = false;
    m_rows_panel->Bind(wxEVT_INVALID_MANUAL_MAP, [this](wxCommandEvent &event) {
        // A real tile pick (this panel's own construction above never fires this event) --
        // disarm live-auto: from here on OK saves a frozen map again, same as today.
        m_live_auto_pending = false;
        seed_status();
        event.Skip();
    });

    wxWindow *rows_widget = m_rows_panel;
    if (m_plate_filaments.size() > MAX_VISIBLE_MAP_ROWS) {
        // Orca: follows FilamentPickerDialog::CreateColorGrid's scrolled-window pattern -- cap
        // the visible height to MAX_VISIBLE_MAP_ROWS tile rows and let the rest scroll. per_row
        // is the fixed wrapped-tile-row height (see TILE_ROW_HEIGHT_DIP), not derived from the
        // panel's total best-size, which mixes in the footer/warning labels below the tile grid
        // and (pre-fix) assumed one row per filament.
        wxSize best    = m_rows_panel->GetBestSize();
        int    per_row = FromDIP(TILE_ROW_HEIGHT_DIP);

        auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxNO_BORDER);
        m_rows_panel->Reparent(scroll);
        auto *scroll_sizer = new wxBoxSizer(wxVERTICAL);
        scroll_sizer->Add(m_rows_panel, 1, wxEXPAND);
        scroll->SetSizer(scroll_sizer);
        scroll->SetMinSize(wxSize(best.GetWidth() + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X), per_row * (int) MAX_VISIBLE_MAP_ROWS));
        scroll->FitInside();
        scroll->SetScrollRate(0, per_row);
        rows_widget = scroll;
    }

    m_rows_holder_sizer->Add(rows_widget, 0, wxEXPAND);

    Layout();
}

void FilamentMapRowsView::repin_rows_min_height()
{
    // Scroll-wrapper mode sizes itself explicitly (see rebuild_rows_panel); only the direct
    // panel needs rescue from its wrap sizer's one-row-per-tile CalcMin.
    if (!m_rows_panel || m_rows_panel->GetParent() != this)
        return;
    m_rows_panel->Layout(); // wrap the tiles at the panel's current width first
    int bottom = 0;
    for (wxWindow *c : m_rows_panel->GetChildren()) {
        // The tile picker popup (wxPopupTransientWindow) is parented to the panel, so it
        // appears among the children while it is open -- during a live pick it sat "below"
        // the real content and poisoned this measurement, re-inflating the dialog on every
        // selection. Popups are top-level windows, not content; skip them.
        if (c->IsTopLevel() || !c->IsShown())
            continue;
        bottom = std::max(bottom, c->GetPosition().y + c->GetSize().GetHeight());
    }
    if (bottom <= 0)
        return; // nothing laid out yet (construction time) -- leave the default measurement
    const int wanted = bottom + FromDIP(6);
    if (m_rows_panel->GetMinSize().GetHeight() != wanted)
        m_rows_panel->SetMinSize(wxSize(-1, wanted));
}

void FilamentMapRowsView::seed_status()
{
    if (!m_rows_panel) return;

    bool all_assigned = m_rows_panel->AllRowsAssigned();

    if (m_record_row)
        m_record_row->Show(m_inventory_was_all_unset && all_assigned);

    update_mismatch_warning();
    update_preview();

    // Orca: the rows panel cannot report its own height honestly -- its tile wrap sizer's
    // CalcMin assumes one row per tile (see TILE_ROW_HEIGHT_DIP's doc), so a panel rebuilt
    // while the dialog is up (Reset/Automatic) claims far more height than its wrapped content
    // uses, and the host dialog's sizer crushes whatever sits below this view to satisfy it --
    // field report from Windows: pressing Reset squeezed the Print Preferences section and its
    // checkboxes to zero height. Lay out at the real width first, then pin the panel's min
    // height to what its content actually occupies (repin_rows_min_height), and lay out again
    // at the honest size before the host re-fits.
    Layout();
    repin_rows_min_height();
    InvalidateBestSize();
    Layout();
    // Notify LAST: the host re-fits itself from here, and showing/hiding the record row and the
    // preview above changes this panel's best size. Notifying first sized the host to a layout
    // that did not yet include the preview, which is how the send dialog ended up clipped.
    if (m_on_state_changed)
        m_on_state_changed(all_assigned);

    if (m_rows_panel)
        m_rows_panel->LogRowGeometry();

    // The relayout above MOVES children (record row appearing shifts everything below it). On
    // Windows the dialog does not erase the pixels a moved child used to cover, so its old
    // rendering survives as a ghost next to the new one -- field report: the preview thumbnail
    // drawn twice, diagonally offset, after the last row was mapped. Repaint the whole top-level
    // window so no stale pixels outlive the layout they belonged to.
    if (wxWindow* top = wxGetTopLevelParent(this))
        top->Refresh();
}

std::map<int, std::string> FilamentMapRowsView::build_slot_preset_names() const
{
    std::map<int, std::string> names;
    const PresetCollection &filament_presets = wxGetApp().preset_bundle->filaments;
    for (const auto &slots : m_inventory.tools)
        for (const PhysicalFilament &pf : slots) {
            if (pf.empty() || pf.id <= 0) continue;
            // Orca: shared slot_display_name (FilamentInventoryStore) -- the combo rows are
            // width-limited and the printer suffix is redundant inside a printer-scoped dialog.
            // The mismatch warning resolves names independently, so this map is display-only.
            names[pf.id] = slot_display_name(pf, filament_presets);
        }
    return names;
}

void FilamentMapRowsView::update_mismatch_warning()
{
    if (!m_rows_panel) return;

    // Orca: non-blocking warning only (spec: mapping dialog shows slot profiles and warns on
    // merged-profile mismatch) -- a project filament's preset that differs from the physical
    // slot it's been merged onto is surfaced, never rejected.
    std::vector<int> physical_map = m_rows_panel->GetPhysicalMaps();

    std::unordered_map<int, int> counts;
    for (int id : physical_map)
        if (id > 0) ++counts[id];

    wxString warning;
    for (size_t i = 0; i < physical_map.size() && warning.empty(); ++i) {
        int id = physical_map[i];
        if (id <= 0 || counts[id] < 2) continue; // only filaments actually merged together matter here

        int f = (int) i + 1;
        std::string project_preset = ((size_t) (f - 1) < m_filament_preset.size()) ? m_filament_preset[f - 1] : std::string();
        if (project_preset.empty()) continue;

        const PhysicalFilament *pf = m_inventory.find(id);
        std::string slot_preset = pf ? resolve_slot_preset(*pf, wxGetApp().preset_bundle->filaments) : std::string();
        if (slot_preset.empty() || slot_preset == project_preset) continue;

        // Orca: no leading " · " -- this now renders on its own line (FilamentMapRowsPanel::
        // m_warning_label), not appended after the stats line.
        warning = wxString::Format(_L("Profile mismatch: %s ≠ %s"), from_u8(project_preset), from_u8(slot_preset));
    }

    m_rows_panel->SetFooterWarning(warning);
}

void FilamentMapRowsView::update_preview()
{
    if (!m_preview_panel || !m_rows_panel)
        return;

    // Orca: a simplified take on SyncAmsInfoDialog's "after mapping" thumbnail --
    // recolors the CURRENT plate's cached render by this dialog's current row picks. Borrows the
    // core recolor formula from SyncAmsInfoDialog::change_default_normal (same no-light-alpha
    // filament-id decode, same lit-vs-flat brightness ratio to keep shading), but skips that
    // pipeline's edge-pixel smoothing pass (record_edge_pixels_data/final_deal_edge_pixels_data):
    // anti-aliased boundary pixels keep their un-recolored blend instead of being averaged from
    // neighbors -- a minor cosmetic difference, not visible without zooming in. Only the current
    // plate's "after" view is shown, no original/twin side, since this dialog only edits one plate.
    // Orca: the construction-time snapshot only (see the ctor) -- this method renders
    // nothing and touches no plate state, so it is safe to call from anywhere in the dialog's
    // life, including its own constructor and its modal loop. When no snapshot was available the
    // preview stays hidden; the dialog is fully usable without it.
    const ThumbnailData &lit      = m_preview_lit;
    const ThumbnailData &no_light = m_preview_no_light;
    if (!lit.is_valid() || !no_light.is_valid() || lit.width != no_light.width || lit.height != no_light.height) {
        m_preview_panel->Hide();
        Layout();
        return;
    }

    // 0-based project filament index -> physical filament id (0 = no pick, or bootstrap mode
    // where there's no physical color yet -- either way, that filament's pixels keep their
    // original lit color below).
    std::vector<int> physical_map = m_rows_panel->GetPhysicalMaps();

    std::vector<bool>     has_target(physical_map.size(), false);
    std::vector<wxColour> target_color(physical_map.size());
    for (size_t i = 0; i < physical_map.size(); ++i) {
        if (physical_map[i] <= 0) continue;
        const PhysicalFilament *pf = m_inventory.find(physical_map[i]);
        if (!pf || pf->color.empty()) continue;
        wxColour c(pf->color);
        if (!c.IsOk()) continue;
        // adjust_color_for_rendering matches the gamma handling the thumbnail's own GL render
        // already applied to every other volume (GLCanvas3D::render_thumbnail_*, and
        // SyncAmsInfoDialog::adjust_color_for_render for the same reason).
        Slic3r::ColorRGBA rgba{c.Red() / 255.0f, c.Green() / 255.0f, c.Blue() / 255.0f, 1.0f};
        Slic3r::ColorRGBA adjusted = adjust_color_for_rendering(rgba);
        target_color[i] = wxColour((unsigned char) (adjusted[0] * 255.0f), (unsigned char) (adjusted[1] * 255.0f),
                                    (unsigned char) (adjusted[2] * 255.0f));
        has_target[i] = true;
    }

    wxImage image((int) lit.width, (int) lit.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < lit.height; ++r) {
        unsigned int rr = (lit.height - 1 - r) * lit.width;
        for (unsigned int c = 0; c < lit.width; ++c) {
            const unsigned char *origin_px   = lit.pixels.data() + 4 * (rr + c);
            const unsigned char *no_light_px = no_light.pixels.data() + 4 * (rr + c);
            unsigned char        out[3]      = {origin_px[0], origin_px[1], origin_px[2]};

            if (origin_px[3] > 0) {
                // Orca: GLCanvas3D's flat/no-light thumbnail pass encodes each pixel's source
                // filament (0-based) as 255 - alpha (see the ban_light branch setting
                // new_color[3] = (255 - (vol->extruder_id - 1)) / 255.0f) -- the same convention
                // SyncAmsInfoDialog::change_default_normal decodes.
                int filament_idx = 255 - no_light_px[3];
                if (filament_idx >= 0 && (size_t) filament_idx < has_target.size() && has_target[filament_idx]) {
                    const wxColour &target       = target_color[filament_idx];
                    int             origin_rgb   = origin_px[0] + origin_px[1] + origin_px[2];
                    int             no_light_rgb = no_light_px[0] + no_light_px[1] + no_light_px[2];
                    if (no_light_rgb <= 0) {
                        out[0] = target.Red(); out[1] = target.Green(); out[2] = target.Blue();
                    } else if (origin_rgb >= no_light_rgb) {
                        out[0] = (unsigned char) std::clamp(target.Red()   + (origin_px[0] - no_light_px[0]), 0, 255);
                        out[1] = (unsigned char) std::clamp(target.Green() + (origin_px[1] - no_light_px[1]), 0, 255);
                        out[2] = (unsigned char) std::clamp(target.Blue()  + (origin_px[2] - no_light_px[2]), 0, 255);
                    } else {
                        float ratio = origin_rgb / (float) no_light_rgb;
                        out[0] = (unsigned char) std::clamp((int) (target.Red()   * ratio), 0, 255);
                        out[1] = (unsigned char) std::clamp((int) (target.Green() * ratio), 0, 255);
                        out[2] = (unsigned char) std::clamp((int) (target.Blue()  * ratio), 0, 255);
                    }
                }
            }
            image.SetRGB((int) c, (int) r, out[0], out[1], out[2]);
            image.SetAlpha((int) c, (int) r, origin_px[3]);
        }
    }

    const wxSize target_size = m_preview_panel->GetMinSize();
    if (target_size.GetWidth() > 0 && target_size.GetHeight() > 0)
        image = image.Rescale(target_size.GetWidth(), target_size.GetHeight(), wxIMAGE_QUALITY_BOX_AVERAGE);
    m_preview_panel->set_thumbnail(image);
    m_preview_panel->Show();
    Layout();
}

void FilamentMapRowsView::ResetToInitial()
{
    // Orca: reapply the construction-time proposal to the rows --
    // selections, tile wheels, auto-matched badges, and OK-gating -- without touching the plate
    // or closing. Reusing rebuild_rows_panel keeps this on the exact same path the ctor and
    // proposal-seeded rebuilds use to seed rows, so they stay in sync by
    // construction; m_initial_proposal_was_auto replays the same badge state the dialog opened
    // with (see its own doc), not just the same target picks.
    rebuild_rows_panel(m_initial_proposal, m_initial_proposal_was_auto);
    seed_status();
}

void FilamentMapRowsView::ApplyAutomatic()
{
    // Orca: the "automatic" escape hatch, distinct from Reset, which only reverts to the dialog's
    // initial state (see on_reset_to_initial). Recomputes a proposal the same way a plate's
    // first-ever (never confirmed) visit to this dialog would
    // (stored_map_confirmed=false ignores m_physical_filament_map/m_filament_map entirely, pure
    // auto-match), and shows it in the rows for review/adjustment.
    std::vector<int> auto_proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor, m_filament_preset, m_plate_filaments,
                                                                    m_physical_filament_map, m_filament_map, m_inventory,
                                                                    /*stored_map_confirmed=*/false);
    // auto_matched=true: this proposal is entirely fresh auto-matcher output, so every row it
    // assigns starts with its auto-matched badge shown.
    rebuild_rows_panel(auto_proposal, /*auto_matched=*/true);
    // Orca: arm live-auto -- rebuild_rows_panel above just disarmed it
    // (every rebuild starts disarmed), this button re-arms it right after. OK now, with no
    // manual tile pick in between, saves the plate in live mode instead of a frozen one; see
    // get_mode()/m_live_auto_pending's docs.
    m_live_auto_pending = true;
    seed_status();
}

FilamentInventory& FilamentMapRowsView::device()
{
    return current_inventory_for_preset(active_printer_session().profile(), m_store, m_tool_count);
}

void FilamentMapRowsView::Commit()
{
    if (m_rows_panel == nullptr)
        return;
    m_rows_panel->SetBaseMap(m_filament_map);
    m_rows_panel->SetBasePhysicalMap(m_physical_filament_map);
    m_physical_filament_map = m_rows_panel->GetPhysicalMaps();
    m_filament_map          = m_rows_panel->GetFilamentMaps();

    // Orca: only offer to persist picks when the inventory used to build this dialog's
    // proposal was entirely empty and every row ended up with a manual pick -- otherwise
    // the checkbox is hidden (seed_rows_mode_status) and stays unchecked-equivalent. This
    // is also exactly the bootstrap-mode case (FilamentMapRowsPanel::BuildTargetOptions):
    // every plate row's physical id is still 0 at this point, so the loop below backfills
    // m_physical_filament_map from the ids this write mints.
    if (m_record_row && m_record_row->IsShown() && m_record_checkbox && m_record_checkbox->GetValue()) {
        // Load-modify-save: start from what's on disk so this write can never discard
        // swappable rows recorded via the Physical Filaments editor (this offer only shows
        // for an all-unset inventory, but the disk may have moved since the dialog was
        // built), and keep any id a loaded slot already owns so existing physical-map
        // references stay valid. ensure_ids mints ids for the slots this write fills.
        m_store = load_filament_inventories();
        FilamentInventory &new_inv = device();
        for (int f : m_plate_filaments) {
            if (f < 1 || f > (int) m_filament_map.size()) continue;
            int tool = m_filament_map[f - 1];
            if (tool < 1 || tool > (int) m_tool_count) continue;
            LoadedFilament &slot = new_inv.tools[tool - 1][0];
            // Orca: preset is now
            // set here too (from m_filament_preset), so a slot recorded through this bootstrap
            // offer resolves a preset/vendor/mismatch-warning identically to one recorded
            // through the editor, instead of rendering as a bare type forever.
            slot = build_physical_filament(
                (f - 1 < (int) m_filament_color.size()) ? m_filament_color[f - 1] : std::string(),
                (f - 1 < (int) m_filament_type.size()) ? m_filament_type[f - 1] : std::string(),
                ((size_t) (f - 1) < m_filament_preset.size()) ? m_filament_preset[f - 1] : std::string(),
                slot.id, slot.kind);
        }
        new_inv.ensure_ids();
        save_filament_inventories(m_store);

        // Close the loop for bootstrap rows: point this plate's physical map at the ids we
        // just minted for the tools the user picked, so a later dialog open sees a
        // confirmed physical map instead of re-triggering bootstrap.
        for (int f : m_plate_filaments) {
            if (f < 1 || f > (int) m_physical_filament_map.size()) continue;
            if (m_physical_filament_map[f - 1] > 0) continue; // already a real target
            int tool = (f <= (int) m_filament_map.size()) ? m_filament_map[f - 1] : 0;
            if (tool < 1 || tool > (int) m_tool_count) continue;
            m_physical_filament_map[f - 1] = new_inv.tools[tool - 1][0].id;
        }
    }
}

FilamentMapRowsDialog::FilamentMapRowsDialog(wxWindow                       *parent,
                                             const std::vector<std::string> &filament_color,
                                             const std::vector<std::string> &filament_type,
                                             const std::vector<std::string> &filament_names,
                                             const std::vector<int>         &filament_map,
                                             const std::vector<int>         &physical_filament_map,
                                             const std::vector<int>         &plate_filaments,
                                             const FilamentInventory         &inventory,
                                             size_t                           tool_count,
                                             const std::string               &printer_preset_name,
                                             bool                             filament_map_confirmed,
                                             const wxString                  &title)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    // Width is content-driven: the row combos size themselves to their widest option label, so no
    // max cap here -- a cap would clip long preset-bearing labels (Fit() cannot grow past it).
    SetMinSize(wxSize(FromDIP(560), -1));

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(20));

    m_view = new FilamentMapRowsView(this, filament_color, filament_type, filament_names, filament_map,
                                     physical_filament_map, plate_filaments, inventory, tool_count,
                                     printer_preset_name, filament_map_confirmed,
                                     [this](bool all_assigned) {
                                         if (m_ok_btn)
                                             m_ok_btn->Enable(all_assigned);
                                         Layout();
                                         Fit();
                                     });
    main_sizer->Add(m_view, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    wxPanel    *bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);

    // "Reset" reverts the rows to what the dialog was opened with; "Automatic" fills them with a
    // freshly computed match. Neither touches a plate, and neither closes the dialog.
    auto *reset_btn = new Button(bottom_panel, _L("Reset"));
    reset_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    reset_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_view->ResetToInitial(); });
    bottom_sizer->Add(reset_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(15));

    auto *auto_btn = new Button(bottom_panel, _L("Automatic"));
    auto_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto_btn->SetToolTip(_L("Fill the rows with a freshly computed automatic match."));
    auto_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_view->ApplyAutomatic(); });
    bottom_sizer->Add(auto_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    bottom_sizer->AddStretchSpacer();
    auto *dlg_btns = new DialogButtons(bottom_panel, {"OK", "Cancel"});
    m_ok_btn       = dlg_btns->GetOK();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);

    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    m_ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_view->Commit();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    SetEscapeId(wxID_CANCEL);

    // The view seeded itself during construction, before m_ok_btn existed; seed the button now.
    m_ok_btn->Enable(m_view->AllRowsAssigned());

    SetSizer(main_sizer);
    Layout();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

std::optional<std::vector<int>> collect_device_map_table_for_send(wxWindow* parent, const std::vector<int>& plate_filaments, const wxString& title)
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    const std::vector<std::string> colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    const std::vector<std::string> types  = full_config.option<ConfigOptionStrings>("filament_type")->values;
    const std::vector<std::string> names  = full_config.option<ConfigOptionStrings>("filament_settings_id")->values;

    FilamentInventories store;
    const size_t            tool_count = resolve_active_printer_tool_count(store);
    const Preset&           printer    = active_printer_session().profile();
    const FilamentInventory inventory  = current_inventory_for_preset(printer, store, tool_count);

    // No plate to read a stored map from at send time: full-length all-zero base maps plus
    // filament_map_confirmed=false give "first-ever visit" semantics, so the stored maps are
    // ignored entirely and every row is proposed straight from the inventory auto-match.
    const std::vector<int> base_map(colors.size(), 0);

    FilamentMapRowsDialog dlg(parent, colors, types, names, base_map, base_map, plate_filaments,
                              inventory, tool_count, printer.name, /*filament_map_confirmed=*/false, title);
    if (dlg.ShowModal() != wxID_OK)
        return std::nullopt;
    return dlg.GetFilamentMaps();
}

}} // namespace Slic3r::GUI
