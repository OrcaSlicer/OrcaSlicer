#include "Table.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "ComponentParts.hpp"
#include "Context.hpp"
#include "Font.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "Swatch.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Tooltip.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

float table_swatch_side(const Metrics& metrics, const std::optional<Swatch>& swatch)
{
    if (!swatch)
        return 0.f;
    return swatch->label.empty() ? metrics.table_swatch : metrics.table_swatch_numbered;
}

std::vector<RowSpan> table_columns(const Metrics& metrics, float left, float width, const std::vector<TableCell>& cells, float swatch_side)
{
    float from = left + metrics.table_padding_left;
    if (swatch_side > 0.f)
        from += swatch_side + metrics.table_swatch_gap;
    const float to = left + width - metrics.table_padding_right;
    std::vector<RowSlot> slots;
    for (const TableCell& cell : cells)
        slots.push_back({cell.width, cell.width <= 0.f, false});
    return row_layout(from, to, metrics.table_cell_gap, slots);
}

namespace {

void draw_cells(ImDrawList& list, const std::vector<TableCell>& cells, const std::vector<RowSpan>& spans, float top, float height, bool dimmed,
                bool header, bool tinted = false)
{
    const Context& ui = *Context::current();
    const Theme&   t  = ui.theme();
    const Metrics& m  = ui.metrics();
    for (size_t i = 0; i < cells.size() && i < spans.size(); ++i) {
        const TableCell& cell = cells[i];
        Font font = cell.bold || (header && i == 0) ? ui.bold_font() : ui.regular_font();
        if (header)
            font = font.resized(m.px(TextRole::label.size));
        const FontMeasure measure(font);
        const std::string text  = ellipsize(measure, cell.text, spans[i].width);
        const float       w     = measure.width(text);
        const float       x     = cell.align == CellAlign::Right ? spans[i].right() - w : spans[i].left;
        const Color       color = header ? t.muted_foreground : cell.muted || dimmed ? muted_text(t, tinted) : t.foreground;
        list.AddText(face_of(font), font.size(), ImVec2(x, top + .5f * (height - font.size())), u32(color), text.c_str());
    }
}

} // namespace

TableRowResult table_row(const char* id, const TableRowOptions& options)
{
    const Metrics& m      = metrics();
    const Theme&   t      = theme();
    const float    height = m.table_row;
    const ImVec2   min    = ImGui::GetCursorScreenPos();
    const float    width  = options.width;
    TableRowResult result;
    if (options.interactive) {
        const Hit h       = hit(id, ImVec2(width, height), Availability());
        result.hovered    = h.hovered;
        result.clicked    = h.clicked && !ImGui::GetIO().KeyAlt;
        result.alt_clicked = h.clicked && ImGui::GetIO().KeyAlt;
        result.context    = h.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    } else {
        const ImRect bounds(min, ImVec2(min.x + width, min.y + height));
        ImGui::ItemSize(bounds.GetSize());
        ImGui::ItemAdd(bounds, ImGui::GetID(id));
        result.hovered = ImGui::IsItemHovered();
    }
    ImDrawList& list = *ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + width, min.y + height);
    if (options.selected)
        fill_rounded(list, min, max, u32(t.selection), m.table_row_radius);
    else if (result.hovered && options.interactive)
        fill_rounded(list, min, max, u32(t.accent), m.table_row_radius);

    const float side = table_swatch_side(m, options.swatch);
    if (options.swatch) {
        Swatch swatch = *options.swatch;
        swatch.hollow = swatch.hollow || options.dimmed;
        const float x = min.x + m.table_padding_left;
        const float y = std::round(min.y + .5f * (height - side));
        draw_swatch(Painter::current(), Rect(x, y, x + side, y + side), swatch, t.card);
    }
    const std::vector<RowSpan> spans = table_columns(m, min.x, width, options.cells, side);
    std::vector<TableCell>     cells = options.cells;
    if (options.selected && !cells.empty())
        cells.front().bold = true;
    draw_cells(list, cells, spans, min.y, height, options.dimmed, false, options.selected || (result.hovered && options.interactive));
    if (result.hovered && !options.tooltip.empty())
        item_tooltip(options.tooltip);
    return result;
}

void table_header(const std::vector<TableCell>& cells, float width, float swatch_side)
{
    const Metrics& m   = metrics();
    const ImVec2   min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, m.table_row));
    const std::vector<RowSpan> spans = table_columns(m, min.x, width, cells, swatch_side);
    draw_cells(*ImGui::GetWindowDrawList(), cells, spans, min.y, m.table_row, false, true);
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
