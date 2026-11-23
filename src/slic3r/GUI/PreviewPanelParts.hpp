#pragma once

#include <functional>
#include <string>

#include "LegendModel.hpp"
#include "LegendText.hpp"
#include "CanvasUi/Table.hpp"

namespace Slic3r {
namespace GUI {

namespace CanvasUi {
class PanelBody;
} // namespace CanvasUi

LegendUnits translated_legend_units();
// What a click on a filament's row does, for its tooltip.
std::string isolate_hint(bool isolated);

struct FilamentRowOptions
{
    // After the name.
    CanvasUi::TableCell mass;
    // The tooltip's lines under the name.
    std::string details;
    // The filament kept in colour, from 1, or 0 for none.
    int  isolated{0};
    bool several{false};
};

// A filament's row of the legend and the print summary on the body's cursor, its number on its swatch. With several
// filaments, hovering the row shows the filament's paths and a click keeps it alone in colour; the one kept is selected and
// the others dimmed.
CanvasUi::TableRowResult filament_row(CanvasUi::PanelBody& body, const LegendFilament& filament, const FilamentRowOptions& options,
                                      const std::function<void(int)>& hover, const std::function<void(int)>& isolate);

} // namespace GUI
} // namespace Slic3r
