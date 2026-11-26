#pragma once

#include <string>
#include <vector>

#include "Paint.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

class Context;
struct Metrics;
struct TableCell;
struct Theme;

// The part of a panel that scrolls, written from its top down: a cursor that components are placed at and that moves
// down past each, so that the height is known once the body is drawn. The padding above the first component and under
// the last is the body's.
class PanelBody
{
public:
    PanelBody(const Painter& painter, float left, float top, float width);

    const Context& ui() const { return m_ui; }
    const Theme&   theme() const;
    const Metrics& metrics() const;
    const Painter& painter() const { return m_painter; }
    float          left() const { return m_left; }
    float          width() const { return m_width; }
    float          right() const { return m_left + m_width; }
    float          y() const { return m_y; }
    float          height() const;

    void advance(float height) { m_y += height; }
    void move_to(float y) { m_y = y; }
    // The next component at the left edge, on the cursor.
    void place() const;
    // Between sections.
    void gap();
    // The titles of the columns of the rows under them, in line with the rows' swatches swatch_side wide.
    void table_header(const std::vector<TableCell>& cells, float swatch_side);
    // A section's title, and on its right a note such as the unit of its values.
    void section_title(const std::string& title, const std::string& note = {});

private:
    const Context& m_ui;
    Painter        m_painter;
    float          m_left;
    float          m_width;
    float          m_top;
    float          m_y;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
