#include "PanelBody.hpp"

#include <string>
#include <vector>

#include "Context.hpp"
#include "FontMeasure.hpp"
#include "Layout.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "Table.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

PanelBody::PanelBody(const Painter& painter, float left, float top, float width)
    : m_ui(*Context::current()), m_painter(painter), m_left(left), m_width(width), m_top(top)
    , m_y(top + m_ui.metrics().panel_body_padding)
{}

const Theme& PanelBody::theme() const { return m_ui.theme(); }

const Metrics& PanelBody::metrics() const { return m_ui.metrics(); }

float PanelBody::height() const { return m_y + metrics().panel_body_padding - m_top; }

void PanelBody::place() const { CanvasUi::place(Point(m_left, m_y)); }

void PanelBody::gap() { m_y += metrics().panel_section_gap; }

void PanelBody::table_header(const std::vector<TableCell>& cells, float swatch_side)
{
    place();
    CanvasUi::table_header(cells, m_width, swatch_side);
    m_y += metrics().table_row;
}

void PanelBody::section_title(const std::string& title, const std::string& note)
{
    std::vector<TableCell> cells = {{title, 0.f, CellAlign::Left}};
    if (!note.empty())
        cells.push_back({note, FontMeasure().width(note)});
    table_header(cells, 0.f);
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
