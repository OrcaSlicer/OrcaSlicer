#ifndef slic3r_GUI_FilamentMapRowsPanel_hpp_
#define slic3r_GUI_FilamentMapRowsPanel_hpp_

#include "GUI.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/FilamentInventory.hpp"

#include <wx/colour.h>
#include <map>
#include <vector>
#include <string>
#include <memory>

namespace Slic3r { namespace GUI {

class MaterialSyncItem;
class FilamentMapPickerPopup;

// Orca: one-row-per-plate-filament mapping panel for opted-in non-BBL multi-tool printers,
// used as an alternative to the drag-drop FilamentMapToolsPanel. Each row shows an AMS-style
// tile (MaterialSyncItem, reused as-is from AmsMappingPopup.hpp) for a plate filament -- index
// prefix, colour, and a "wheel" showing the currently-picked target's colour. Clicking a tile
// opens a popup (FilamentMapPickerPopup, defined in the .cpp) positioned under it, hosting one
// per-tool MappingContainer of MappingItems built from that row's TargetOption list, mirroring
// how SyncAmsInfoDialog pairs its MaterialSyncItem grid with AmsMapingPopup. Two rows picking the
// same physical filament is a merge, not an error, and the footer reflects that.
class FilamentMapRowsPanel : public wxPanel
{
public:
    // One dropdown/tile entry: either a real physical filament (id > 0, tool = its owning tool)
    // or, in bootstrap mode (or for a tool with nothing recorded), a plain per-tool placeholder
    // (id == 0, tool = that tool's index). Shared by every row -- the same options, in the same
    // order, are offered to every row's picker popup. Public so FilamentMapPickerPopup (.cpp) can
    // consume it directly.
    struct TargetOption
    {
        int      id;   // physical filament id, or 0 for a bootstrap/empty-tool placeholder
        int      tool; // 0-based owning/placeholder tool index
        std::string type; // physical material type ("" for bootstrap/bare-tool placeholders)
        wxString label;
        wxColour colour; // transparent (alpha 0) for a placeholder with no recorded colour
    };


    // filament_colors/filament_types/filament_names: full project lists, 0-based by filament id.
    // plate_filaments: 1-based used filament ids, in plate order.
    // proposal: one entry per plate_filaments entry, from compute_physical_map_proposal: the
    //   proposed physical filament id (0 = unassigned) in normal mode, or -(1-based tool) in
    //   bootstrap mode (see `inventory`) -- both this panel and compute_physical_map_proposal
    //   derive bootstrap-ness from `inventory` via Slic3r::inventory_all_unset, so the encoding
    //   in use is always consistent with it.
    // inventory: printer's currently known physical filaments (loaded + swappable). When it has
    //   no recorded filament at all (a fresh machine profile never edited), the panel falls back
    //   to one dropdown entry per tool (bootstrap mode, see BuildTargetOptions) so a first-time
    //   "record these as loaded" pass is still possible.
    // slot_preset_names: physical filament id -> resolved preset display name (see
    //   FilamentInventoryStore::resolve_slot_preset); an id absent here (or mapped to "") keeps
    //   the type-based label. Resolved by the dialog, since this panel has no PresetBundle access.
    // tool_count: physical tool count.
    // auto_matched_proposal: true when `proposal` came entirely from the auto-matcher rather than
    //   a stored/confirmed pick. Every row that ends up with a selection starts with its
    //   auto-matched badge shown (Row::auto_matched); a later hand pick clears it (OnTileClicked).
    FilamentMapRowsPanel(wxWindow                        *parent,
                         const std::vector<std::string> &filament_colors,
                         const std::vector<std::string> &filament_types,
                         const std::vector<std::string> &filament_names,
                         const std::vector<int>          &plate_filaments,
                         const std::vector<int>          &proposal,
                         const FilamentInventory          &inventory,
                         const std::map<int, std::string> &slot_preset_names,
                         size_t                            tool_count,
                         bool                              auto_matched_proposal);

    // Full-length 1-based filament->tool passthrough map for filaments not on this plate; also
    // used as the tool for a bootstrap-mode row (see GetFilamentMaps). Defaults to 1 if never
    // called. Call before GetFilamentMaps().
    void SetBaseMap(const std::vector<int> &full_map);

    // Full-length physical-filament-id passthrough map for filaments not on this plate (0 =
    // unassigned). Defaults to all-0 if never called. Call before GetPhysicalMaps().
    void SetBasePhysicalMap(const std::vector<int> &full_map);

    // Full-length 1-based filament->tool map, DERIVED per plate row from the chosen physical
    // filament's owning tool (inventory.tool_of(id)+1); a bootstrap-mode row (no physical
    // filament exists yet) reports its raw tool pick directly instead. Every other filament
    // keeps SetBaseMap()'s passthrough value.
    std::vector<int> GetFilamentMaps() const;

    // Full-length physical filament id per project filament (0 = unassigned/not on this plate's
    // rows); a bootstrap-mode row always reports 0, since no physical filament exists for it yet
    // (see FilamentMapDialog::on_ok's record-to-inventory follow-up, which mints one and can then
    // backfill this). Every other filament keeps SetBasePhysicalMap()'s passthrough value.
    std::vector<int> GetPhysicalMaps() const;

    // True when every row has a target selected.
    bool AllRowsAssigned() const;
    // Orca: field diagnostic (warning level -- Windows configs filter info) for tiles that
    // occupy layout space but paint nothing: logs every row tile's shown-state, rect and colour.
    void LogRowGeometry() const;

    // Sum over tools of max(0, distinct-targets-on-that-tool - 1): how many tool swaps this
    // plate will need. Rows sharing the same physical filament on a tool count as one target
    // (a merge), not a swap. In bootstrap mode (see BuildTargetOptions) every row is its own
    // target, so this reduces to the tool-index-only count.
    int SwapCount() const;

    // Appends a warning suffix to the footer (e.g. the dialog's merged-profile-mismatch check,
    // which needs preset resolution this panel doesn't have -- see the constructor's
    // slot_preset_names doc). Pass an empty string to clear. Safe to call any time after
    // construction; re-derives the whole footer text so it stays in sync with the current rows.
    void SetFooterWarning(const wxString &text);

private:
    struct Row
    {
        int               filament_id; // 1-based
        MaterialSyncItem *tile;
        int               selected_index{-1}; // index into m_target_options, or -1 = unassigned
        // True when the CURRENT selection came from the auto-matcher and has not been hand-picked
        // since -- seeded from the ctor's auto_matched_proposal argument, cleared by a pick in
        // OnTileClicked. Read by ApplySelectionToTile to show/hide badge.
        bool              auto_matched{false};
    };

    // Populates m_target_options and m_bootstrap_mode from the inventory: one entry per
    // non-empty physical filament (loaded slot first, then swappable), grouped/labeled by tool;
    // or, when the inventory has no recorded filament at all, one plain "Tool N" entry per tool
    // (bootstrap mode -- see the class comment and GetPhysicalMaps).
    void BuildTargetOptions(const FilamentInventory &inventory, const std::map<int, std::string> &slot_preset_names);
    // Opens (creating on first use) the shared picker popup under row's tile, offering
    // m_target_options grouped by tool; picking one updates the row and fires the
    // wxEVT_INVALID_MANUAL_MAP/UpdateFooter path.
    void OnTileClicked(size_t row_index);
    // Orca: the whole selection-change route, in one named place -- what the picker's
    // pick callback runs, for EVERY target option alike. A swappable slot (tool slot index >= 1)
    // is an ordinary m_target_options entry that differs from a loaded one only in its label
    // suffix, so it takes exactly this path with no branch of its own; nothing here invalidates
    // or regenerates a thumbnail. The wxEVT_INVALID_MANUAL_MAP it ends on is what drives the
    // dialog's seed_rows_mode_status()/update_preview(), which must stay GL-free -- see
    // FilamentMapDialog::update_preview.
    void ApplyPick(size_t row_index, int option_index);
    // Reflects m_rows[row_index]'s current selected_index onto its tile: the target's colour +
    // a short "T<n>" wheel label when assigned (MaterialItem::m_match becomes true), or a dash
    // (m_match becomes false -- the widget's own "not mapped" glyph) when unassigned.
    void ApplySelectionToTile(size_t row_index);
    // Recomputes and applies the footer label text only; does not fire wxEVT_INVALID_MANUAL_MAP.
    // Used both after each row change and once at the end of construction, since the
    // constructor itself must stay silent: the owning dialog seeds its own initial OK state
    // by querying AllRowsAssigned() right after construction, rather than reacting to an event.
    void UpdateFooter();

    // Resolves a row's current selection to (tool, physical id) via m_target_options. Returns
    // false (leaving tool/id untouched) when the row has no selection.
    bool ResolveRow(const Row &row, int &tool, int &id) const;

    // Orca: true when row's current selection targets a tool that
    // has nothing physically recorded on it right now, outside bootstrap mode. This is reachable
    // even after libslic3r's matcher/assignment fixes (auto_map_filaments,
    // compute_physical_map_proposal) because both deliberately REPROPOSE a prior confirmed bare
    // tool pick (physical id 0) as long as its tool is still empty -- see
    // compute_physical_map_proposal's "mixed mode" branch -- so the row keeps displaying that
    // pick for continuity instead of silently dropping it. Such a selection is "resolved" (has a
    // tool) but must not count as a real assignment: AllRowsAssigned/UpdateFooter both treat it
    // as unassigned so the user is forced to re-pick or press Automatic.
    bool RowTargetsEmptyTool(const Row &row) const;

    // Orca: footer-warning text listing every tool a row is currently (invalidly) pointed
    // at via RowTargetsEmptyTool, e.g. "Tool 3 has no filament loaded". Empty when no row is in
    // that state. Combined with the dialog-supplied m_footer_warning in UpdateFooter.
    wxString EmptyToolWarningText() const;

    // Shared by UpdateFooter (footer text) and SwapCount (its own totals): idle tool count,
    // swap count (extra distinct targets per tool beyond the first), and merged-row count
    // (extra rows sharing a physical id beyond the first, 0 in bootstrap mode).
    void ComputeStats(int &idle, int &swaps, int &merged) const;

    std::vector<Row>          m_rows;
    // Project filament types, 0-based by filament id -- kept for the picker's material-family
    // gate (each row restricts its options to type-compatible physical filaments).
    std::vector<std::string>  m_filament_types;
    std::vector<TargetOption> m_target_options;
    bool                      m_bootstrap_mode{false};
    std::vector<int>          m_base_map;
    std::vector<int>          m_base_physical_map;
    size_t                    m_filament_count;
    size_t                    m_tool_count;

    FilamentMapPickerPopup *m_picker_popup{nullptr}; // owned by wx (child of this panel)

    Label   *m_footer{nullptr};
    // Orca: own line below m_footer, orange, shown only while m_footer_warning is non-empty --
    // see SetFooterWarning / UpdateFooter.
    Label   *m_warning_label{nullptr};
    wxString m_footer_warning; // dialog-supplied suffix, see SetFooterWarning
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentMapRowsPanel_hpp_
