#pragma once

#include <functional>
#include <vector>

#include <wx/panel.h>

class wxFlexGridSizer;

namespace Slic3r {
namespace GUI {

class PartPlate;

// Orca: the "Filament nozzles" group of Plate Settings on a printer whose filaments are decoupled
// from its tools: one row per filament the current plate uses, with the tool (and so the nozzle)
// it prints from. The tools shown are the plate Print's resolution (FilamentCompaction); choosing
// a tool switches the plate to a manual filament map holding that resolution with the change
// applied, swapping with the filament that held the tool. "Reset" returns the plate to automatic.
//
// The rows are rebuilt from the event loop, never on the stack of the event that asked for it: a
// row's own combo asks for the rebuild that destroys it, and a plate the Tab still names may have
// been deleted since (the Tab's plate map is refreshed only when the object list re-selects).
class PlateFilamentNozzlesPanel : public wxPanel
{
public:
    // `on_rebuilt` runs after each rebuild, once the panel's shown state is known.
    explicit PlateFilamentNozzlesPanel(wxWindow* parent, std::function<void()> on_rebuilt = {});

    // Schedules a rebuild of the rows from `plate` (the one Plate Settings shows); the panel hides
    // when it does not apply.
    void update(PartPlate* plate);
    void rescale();

private:
    struct Row
    {
        int slot = 0; // 0-based project filament
        int tool = 0; // 0-based physical extruder (tool) it prints from
    };
    std::vector<Row> rows_of(const PartPlate& plate) const;
    // `m_plate` if the plater still holds it, else nullptr.
    PartPlate* live_plate() const;
    void       rebuild();
    void       assign(int slot, int tool);
    void       reset();

    wxFlexGridSizer*      m_grid  = nullptr;
    wxWindow*             m_reset = nullptr;
    PartPlate*            m_plate = nullptr;
    bool                  m_rebuild_pending = false;
    std::function<void()> m_on_rebuilt;
};

} // namespace GUI
} // namespace Slic3r
