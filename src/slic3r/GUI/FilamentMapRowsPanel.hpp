#ifndef slic3r_GUI_FilamentMapRowsPanel_hpp_
#define slic3r_GUI_FilamentMapRowsPanel_hpp_

#include "GUI.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/FilamentInventory.hpp"
#include "SlotGridPanel.hpp"

#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#include <wx/panel.h>
#include <wx/string.h>

namespace Slic3r { namespace GUI {

class MaterialSyncItem;

// Orca: one tile per plate filament, each mapped to a physical filament through a slot picker.
// Two rows picking the same physical filament is a merge, not an error.
class FilamentMapRowsPanel : public wxPanel
{
public:
    // plate_filaments: 1-based ids. Rows start unassigned until ApplyProposal().
    FilamentMapRowsPanel(wxWindow                       *parent,
                         const std::vector<std::string> &filament_colors,
                         const std::vector<std::string> &filament_types,
                         const std::vector<std::string> &filament_names,
                         const std::vector<int>         &plate_filaments,
                         const FilamentInventory        &inventory,
                         size_t                          tool_count);

    // proposal: per plate filament, the physical id (0 = unassigned). Proposed rows get the
    // auto-matched badge. Fires no wxEVT_INVALID_MANUAL_MAP.
    void ApplyProposal(const std::vector<int> &proposal);

    // Full-length, 1-based; 0 for filaments not on this plate. A row's tool is its slot's index.
    std::vector<int> GetFilamentMaps() const;

    // Full-length, 0 = unassigned. Bootstrap-mode rows report 0.
    std::vector<int> GetPhysicalMaps() const;

    bool AllRowsAssigned() const;

    // Shown on its own line below the footer; "" clears it.
    void SetFooterWarning(const wxString &text);

private:
    struct Row
    {
        size_t            plate_index; // into plate_filaments
        int               filament_id; // 1-based
        MaterialSyncItem *tile;
        int               selected_index{-1}; // into m_options, i.e. the 0-based tool; -1 = unassigned
        bool              auto_matched{false}; // cleared by a hand pick
    };

    std::vector<SlotGridSlot> PickerRows(const Row &row, bool allow_bare_pick) const;
    // Rows sharing an extruder with another assigned row get the swap-and-purge tint.
    void UpdateSharedExtruderMarks();
    void OnTileClicked(size_t row_index);
    // Ends in wxEVT_INVALID_MANUAL_MAP, whose handlers must stay GL-free.
    void ApplyPick(size_t row_index, int option_index);
    void ApplySelectionToTile(size_t row_index);
    // Does not fire wxEVT_INVALID_MANUAL_MAP, so construction stays silent.
    void UpdateFooter();

    // false, leaving tool/id untouched, when the row has no selection.
    bool ResolveRow(const Row &row, int &tool, int &id) const;

    // A pick of a slot with nothing recorded: shown, but counted as unassigned so the user re-picks.
    bool RowTargetsEmptyTool(const Row &row) const;

    wxString EmptyToolWarningText() const;

    // merged: extra rows sharing a physical id, 0 in bootstrap mode.
    void ComputeStats(int &idle, int &swaps, int &merged) const;

    std::vector<Row>          m_rows;
    std::vector<std::string>  m_filament_types; // for the picker's material-family gate
    // One option per tool: the inventory's slots, padded with empty ones up to the tool count.
    std::vector<SlotGridSlot> m_options;
    std::vector<int>          m_option_ids; // physical id per option, 0 = nothing recorded
    // The inventory records nothing, so rows pick bare tools (see inventory_all_unset).
    bool                      m_bootstrap_mode{false};
    size_t                    m_filament_count;
    size_t                    m_tool_count;

    SlotPickPopup *m_picker_popup{nullptr}; // owned by wx
    // Multi-extruder changer: picks go through SlotPickDialog instead of the popup.
    bool           m_memm{false};

    Label   *m_footer{nullptr};
    Label   *m_warning_label{nullptr};
    wxString m_footer_warning;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentMapRowsPanel_hpp_
