#ifndef slic3r_GUI_SlotGridPanel_hpp_
#define slic3r_GUI_SlotGridPanel_hpp_

#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/colour.h>

#include "Widgets/PopupWindow.hpp"
#include <cstddef>
#include <utility>

namespace Slic3r {
struct FilamentInventory;
class PresetCollection;
namespace GUI {

// Orca: the printer's physical slots, one titled box per changer unit ("Direct" for slots with
// no unit), with an optional row of extruder tiles that filters the slots by extruder. Scrolls
// vertically once the boxes are taller than a share of the screen.
struct SlotGridOptions
{
    bool show_extruder_row     = false; // MEMM: E<n> tiles that filter the slots
    bool show_extruder_on_tile = false; // third tile line "E<n>"
    bool pick_mode             = false; // tiles are buttons: checked / disabled / wrong-type
    bool show_edit_pencil      = false; // materials editor: a pencil on every editable tile
};

struct SlotGridSlot
{
    wxColour colour;
    bool     empty      = false;
    wxString type;
    wxString tooltip;
    wxString unit;                   // stable id; "" = no unit (Direct)
    wxString unit_label;
    int      slot         = 0;
    int      extruder     = -1;      // 0-based; -1 = unknown
    int      virtual_tool = -1;      // 0-based T<n>; -1 = unknown
    bool     checked      = false;
    bool     disabled     = false;
    bool     wrong_type   = false;   // greyed but enabled, so its tooltip shows
    bool     edit_disabled = false;
};

class SlotTile;
class ExtruderTile;

class SlotGridPanel : public wxScrolledWindow
{
public:
    SlotGridPanel(wxWindow* parent, const SlotGridOptions& options);

    // `slots` is in inventory order; slots feeding an extruder >= extruder_count are not filterable.
    void SetSlots(const std::vector<SlotGridSlot>& slots, size_t extruder_count);
    void SetOnSlotClicked(std::function<void(size_t slot_index)> cb) { m_on_slot = std::move(cb); }
    void SetOnEditClicked(std::function<void(size_t slot_index)> cb) { m_on_edit = std::move(cb); }

    // -1 = all.
    void SelectExtruder(int extruder);

private:
    void OnSlotClicked(size_t slot_index);
    void OnExtruderClicked(int extruder);

    SlotGridOptions                        m_options;
    wxBoxSizer*                            m_sizer{nullptr};
    std::vector<SlotTile*>                 m_tiles;
    std::vector<ExtruderTile*>             m_extruders;
    std::vector<SlotGridSlot>              m_slots;
    int                                    m_selected_extruder{-1};
    std::function<void(size_t)>            m_on_slot;
    std::function<void(size_t)>            m_on_edit;
};

// Consumers set the pick/edit flags afterwards.
std::vector<SlotGridSlot> slot_grid_rows(const FilamentInventory& inv, const PresetCollection& filaments);

// "#RRGGBB" or "#RRGGBBAA"; `fallback` when empty or malformed.
wxColour colour_or(const std::string& hex, const wxColour& fallback);

// Slot picker for the sync dialog on a toolchanger.
class SlotPickPopup : public PopupWindow
{
public:
    explicit SlotPickPopup(wxWindow* parent);
    void Rebuild(const std::vector<SlotGridSlot>& slots, std::function<void(size_t slot_index)> on_pick);
    // Shows the popup just below `anchor`.
    void PopupBelow(wxWindow* anchor);

private:
    SlotGridPanel*              m_grid{nullptr};
    std::function<void(size_t)> m_on_pick;
};

} // namespace GUI
} // namespace Slic3r

#endif
