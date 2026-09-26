#ifndef slic3r_GUI_SlotGridPanel_hpp_
#define slic3r_GUI_SlotGridPanel_hpp_

#include <functional>
#include <vector>

#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/colour.h>

#include "Widgets/PopupWindow.hpp"

namespace Slic3r {
struct FilamentInventory;
class PresetCollection;
namespace GUI {

// Orca: the printer's physical slots, drawn where the printer has them -- one titled box per
// changer unit with the unit's slots left to right in slot order, slots with no unit under a
// "Direct" heading, and on a multi-extruder changer (MEMM) a row of extruder tiles above that
// filters the slots by the extruder they feed. Each tile shows the slot's current virtual tool
// (T<n>, 1-indexed), its material type and, when asked, its extruder (E<n>).
//
// One panel, three consumers: the Printer Material Settings dialog (with edit pencils), the
// Synchronize Filament Information picker and the print mapping picker (pick mode: a click
// reports the slot index, tiles carry checked / disabled / wrong-type state). It owns no
// inventory data: slot_grid_rows() turns an inventory into the display rows, the consumer
// decorates them and hands them in with SetSlots().
struct SlotGridOptions
{
    bool show_extruder_row     = false; // MEMM: E<n> tiles that filter the slots
    bool show_extruder_on_tile = false; // third tile line "E<n>"
    bool pick_mode             = false; // tiles are buttons: checked / disabled / wrong-type
    bool show_edit_pencil      = false; // materials editor: a pencil on every editable tile
};

struct SlotGridSlot
{
    wxColour colour;                 // filament colour; ignored when empty
    bool     empty      = false;     // nothing loaded: grey with a diagonal line
    wxString type;                   // material type line
    wxString tooltip;                // hover text (preset, vendor, slot name, unit, extruder)
    wxString name;                   // the printer's own slot name
    wxString unit;                   // changer unit, by stable id; "" = no unit (Direct)
    wxString unit_label;             // the unit box's title; the id when the unit has no name
    int      slot         = 0;       // position within the unit
    int      extruder     = -1;      // 0-based extruder it feeds; -1 = unknown
    int      virtual_tool = -1;      // the T<n> the printer maps it to now; -1 = unknown
    bool     checked      = false;   // pick mode: the current pick
    bool     disabled     = false;   // pick mode: not selectable (the tooltip says why)
    bool     wrong_type   = false;   // pick mode: greyed but still enabled so its tooltip shows
    bool     edit_disabled = false;  // pencil hidden (the tooltip says why)
};

class SlotTile;
class ExtruderTile;

class SlotGridPanel : public wxPanel
{
public:
    SlotGridPanel(wxWindow* parent, const SlotGridOptions& options);

    // Rebuilds the grid from `slots` (one per inventory slot, inventory order). extruder_count
    // sizes the extruder row; slots feeding an extruder past it are shown but not filterable.
    void SetSlots(const std::vector<SlotGridSlot>& slots, size_t extruder_count);
    void SetOnSlotClicked(std::function<void(size_t slot_index)> cb) { m_on_slot = std::move(cb); }
    void SetOnEditClicked(std::function<void(size_t slot_index)> cb) { m_on_edit = std::move(cb); }

    // The extruder whose slots are shown at full strength, -1 = all. A pure view filter.
    int  SelectedExtruder() const { return m_selected_extruder; }
    void SelectExtruder(int extruder);

private:
    void OnSlotClicked(size_t slot_index);
    void OnExtruderClicked(int extruder);

    SlotGridOptions                        m_options;
    wxBoxSizer*                            m_sizer{nullptr};
    std::vector<SlotTile*>                 m_tiles;     // index = slot index
    std::vector<ExtruderTile*>             m_extruders; // index = extruder
    std::vector<SlotGridSlot>              m_slots;
    int                                    m_selected_extruder{-1};
    std::function<void(size_t)>            m_on_slot;
    std::function<void(size_t)>            m_on_edit;
};

// The display rows for an inventory: colour, type, the hover text (preset, vendor, slot name,
// unit, E<n>) and the topology. Consumers set the pick/edit flags afterwards.
std::vector<SlotGridSlot> slot_grid_rows(const FilamentInventory& inv, const PresetCollection& filaments);

// A transient popup under a tile with the slot grid in pick mode and no extruder row: the
// picker for a single-extruder changer (SEMM) and for the sync dialog. A pick reports the slot
// index and dismisses the popup.
class SlotPickPopup : public PopupWindow
{
public:
    explicit SlotPickPopup(wxWindow* parent);
    void Rebuild(const std::vector<SlotGridSlot>& slots, std::function<void(size_t slot_index)> on_pick);

private:
    SlotGridPanel*              m_grid{nullptr};
    std::function<void(size_t)> m_on_pick;
};

// A modal with the extruder row and the units: the picker for a multi-extruder changer (MEMM),
// where the pick is two-level (extruder, then slot) and a second filament landing on the same
// extruder should be a visible choice. Picked() is the slot index, -1 when cancelled.
class SlotPickDialog : public wxDialog
{
public:
    SlotPickDialog(wxWindow* parent, const std::vector<SlotGridSlot>& slots, size_t extruder_count, int current);
    int Picked() const { return m_picked; }

private:
    int m_picked{-1};
};

} // namespace GUI
} // namespace Slic3r

#endif
