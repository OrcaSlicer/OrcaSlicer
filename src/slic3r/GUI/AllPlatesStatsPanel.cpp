#include "AllPlatesStatsPanel.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/PreviewPanelParts.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/Swatch.hpp"
#include "slic3r/GUI/CanvasUi/Table.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {
namespace GUI {

void render_all_plates_stats(const AllPlatesStats& stats, const CanvasUi::Point& center)
{
    const CanvasUi::Context& ui    = *CanvasUi::Context::current();
    const CanvasUi::Metrics& m     = ui.metrics();
    const CanvasUi::Theme&   t     = ui.theme();
    const LegendUnits        units = translated_legend_units();

    std::array<bool, LEGEND_PART_COUNT> shown{};
    for (const LegendFilament& row : stats.rows)
        for (size_t part = 0; part < LEGEND_PART_COUNT; ++part)
            shown[part] = shown[part] || row.grams[part] > 0.0 || row.mm[part] > 0.0;
    const bool total_shown = shown[size_t(LegendPart::Support)] || shown[size_t(LegendPart::Flushed)] || shown[size_t(LegendPart::Tower)];

    const float column = m.px(CanvasUi::space<76>());
    const std::string part_names[] = {_u8L("Model"), _u8L("Support"), _u8L("Flushed"), _u8L("Tower")};
    std::vector<CanvasUi::TableCell> header = {{_u8L("Filament"), 0.f, CanvasUi::CellAlign::Left}};
    for (size_t part = 0; part < LEGEND_PART_COUNT; ++part)
        if (shown[part])
            header.push_back({part_names[part], column});
    if (total_shown)
        header.push_back({_u8L("Total"), column});

    const float pad    = m.card_padding;
    const float width  = std::round(m.px(CanvasUi::space<120>()) + float(header.size() - 1) * (column + m.table_cell_gap) + 2.f * pad);
    const float title  = m.panel_header;
    const float line   = m.whole_px(CanvasUi::TextRole::body.leading);
    const float height = title + m.table_row * float(stats.rows.size() + 1) + m.card_gap + 2.f * line + pad;

    CanvasUi::CardStyle style;
    style.pivot   = CanvasUi::Point(.5f, .5f);
    style.padding = 0.f;
    const CanvasUi::Card    card("Statistics of All Plates", center, CanvasUi::Size(width, height), style);
    const CanvasUi::Painter paint = card.painter();
    const CanvasUi::Point   origin = card.rect().min();
    const float             inner  = width - 2.f * pad;
    paint.text(ui.bold_font(), CanvasUi::Rect(origin.x + pad, origin.y, origin.x + width, origin.y + title), t.foreground, _u8L("Statistics of All Plates"));
    paint.divider(origin.x, origin.x + width, origin.y + title);

    float y = origin.y + title;
    CanvasUi::place(CanvasUi::Point(origin.x + pad, y));
    CanvasUi::table_header(header, inner, m.table_swatch);
    y += m.table_row;
    int index = 0;
    for (const LegendFilament& row : stats.rows) {
        CanvasUi::TableRowOptions options;
        options.width       = inner;
        options.interactive = false;
        options.swatch      = CanvasUi::Swatch{row.color};
        options.cells       = {{std::to_string(row.number), 0.f, CanvasUi::CellAlign::Left}};
        std::string lengths;
        for (size_t part = 0; part < LEGEND_PART_COUNT; ++part) {
            if (!shown[part])
                continue;
            options.cells.push_back({legend_mass(row.grams[part], stats.imperial, units), column});
            lengths += "\n" + part_names[part] + ": " + legend_filament_length(row.mm[part], stats.imperial, units);
        }
        if (total_shown) {
            options.cells.push_back({legend_mass(row.total_grams(), stats.imperial, units), column, CanvasUi::CellAlign::Right, false, true});
            lengths += "\n" + _u8L("Total") + ": " + legend_filament_length(row.total_mm(), stats.imperial, units);
        }
        options.tooltip = format(_u8L_CONTEXT("Filament %1%", "Legend"), row.number) + lengths;
        CanvasUi::place(CanvasUi::Point(origin.x + pad, y));
        CanvasUi::ScopedId id(index++);
        CanvasUi::table_row("##plates_row", options);
        y += m.table_row;
    }
    y += m.card_gap;
    paint.divider(origin.x, origin.x + width, y);
    const CanvasUi::Font regular = ui.regular_font();
    const CanvasUi::Font bold    = ui.bold_font();
    const auto           total   = [&](const std::string& label, const std::string& value) {
        paint.text(regular, CanvasUi::Rect(origin.x + pad, y, origin.x + width, y + line), t.muted_foreground, label);
        paint.text(bold, CanvasUi::Rect(origin.x + width - pad - bold.width(value), y, origin.x + width, y + line), t.foreground, value);
        y += line;
    };
    total(_u8L("Total time"), legend_long_time(stats.time, units));
    total(_u8L("Total cost"), legend_value(stats.cost, 2));
}

} // namespace GUI
} // namespace Slic3r
