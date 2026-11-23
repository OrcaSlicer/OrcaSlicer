#include "PreviewPanelParts.hpp"

#include <functional>
#include <string>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/PanelBody.hpp"
#include "slic3r/GUI/CanvasUi/Swatch.hpp"
#include "slic3r/GUI/CanvasUi/Table.hpp"

namespace Slic3r {
namespace GUI {

LegendUnits translated_legend_units()
{
    LegendUnits u;
    // TRN Days and hours of a time in a column of the preview legend, as "2d3h".
    u.days_hours = _u8L_CONTEXT("%1%d%2%h", "Legend time");
    // TRN Hours and minutes of a time in a column of the preview legend, as "1h20m".
    u.hours_minutes = _u8L_CONTEXT("%1%h%2%m", "Legend time");
    // TRN Minutes and seconds of a time in a column of the preview legend, as "25m56s".
    u.minutes_seconds = _u8L_CONTEXT("%1%m%2%s", "Legend time");
    // TRN Seconds of a time in a column of the preview legend, as "42s".
    u.seconds = _u8L_CONTEXT("%1%s", "Legend time");
    // TRN A time under one second in the preview legend.
    u.under_second = _u8L_CONTEXT("<1s", "Legend time");
    // TRN Days and hours of the print time in the print summary and the statistics of all plates, as "2d 3h".
    u.long_days_hours = _u8L_CONTEXT("%1%d %2%h", "Legend time");
    // TRN Hours and minutes of the print time in the print summary and the statistics of all plates, as "1h 27m".
    u.long_hours_minutes = _u8L_CONTEXT("%1%h %2%m", "Legend time");
    // TRN Minutes and seconds of the print time in the print summary and the statistics of all plates, as "25m 56s".
    u.long_minutes_seconds = _u8L_CONTEXT("%1%m %2%s", "Legend time");
    // TRN Seconds of the print time in the print summary and the statistics of all plates, as "42s".
    u.long_seconds = _u8L_CONTEXT("%1%s", "Legend time");
    // TRN A mass in grams in the preview legend.
    u.grams = _u8L_CONTEXT("%1% g", "Legend unit");
    u.kilograms = _u8L_CONTEXT("%1% kg", "Legend unit");
    u.ounces = _u8L_CONTEXT("%1% oz", "Legend unit");
    // TRN A length in metres in the preview legend.
    u.meters = _u8L_CONTEXT("%1% m", "Legend unit");
    u.millimeters = _u8L_CONTEXT("%1% mm", "Legend unit");
    u.inches = _u8L_CONTEXT("%1% in", "Legend unit");
    return u;
}

std::string isolate_hint(bool isolated)
{
    // TRN How a filament's row of the preview legend or the print summary acts on a click.
    return isolated ? _u8L_CONTEXT("Click shows every filament again.", "Legend") : _u8L_CONTEXT("Click keeps only this filament in colour.", "Legend");
}

CanvasUi::TableRowResult filament_row(CanvasUi::PanelBody& body, const LegendFilament& filament, const FilamentRowOptions& options,
                                      const std::function<void(int)>& hover, const std::function<void(int)>& isolate)
{
    const std::string         name = filament.type + (filament.name.empty() ? "" : " " + filament.name);
    CanvasUi::TableRowOptions row;
    row.width       = body.width();
    row.swatch      = CanvasUi::Swatch{filament.color, false, std::to_string(filament.number), filament.translucent};
    row.cells       = {{name, 0.f, CanvasUi::CellAlign::Left}, options.mass};
    row.interactive = options.several;
    row.selected    = options.isolated == filament.number;
    row.dimmed      = options.isolated != 0 && !row.selected;
    row.tooltip     = name + "\n" + options.details;
    if (options.several)
        row.tooltip += "\n\n" + isolate_hint(row.selected);
    body.place();
    const CanvasUi::ScopedId       id{filament.number};
    const CanvasUi::TableRowResult result = CanvasUi::table_row("##filament", row);
    if (options.several && result.hovered && hover)
        hover(filament.number);
    if (options.several && result.clicked && isolate)
        isolate(filament.number);
    body.advance(body.metrics().table_row);
    return result;
}

} // namespace GUI
} // namespace Slic3r
