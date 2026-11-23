#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Measure.hpp"
#include "Swatch.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;

enum class CellAlign : unsigned char { Left, Right };

struct TableCell
{
    std::string text;
    // Zero takes what the other cells leave; only one cell of a row should.
    float       width{0.f};
    CellAlign   align{CellAlign::Right};
    bool        muted{false};
    bool        bold{false};
};

struct TableRowOptions
{
    float                  width{0.f};
    std::optional<Swatch>  swatch;
    std::vector<TableCell> cells;
    // Something hidden: muted text and a hollow swatch.
    bool                   dimmed{false};
    // The one picked among the rows: a selection background and a bold first cell.
    bool                   selected{false};
    bool                   interactive{true};
    std::string            tooltip;
};

struct TableRowResult
{
    bool clicked{false};
    bool alt_clicked{false};
    bool context{false};
    bool hovered{false};
};

// The side of a row's swatch: larger when a number is written on it, none without one.
float table_swatch_side(const Metrics& metrics, const std::optional<Swatch>& swatch);

// Where the cells of a row of width go from left: after a swatch swatch_side wide, when it is not zero.
std::vector<RowSpan> table_columns(const Metrics& metrics, float left, float width, const std::vector<TableCell>& cells, float swatch_side);

TableRowResult table_row(const char* id, const TableRowOptions& options);
// The titles of a table's columns at the cursor, in line with rows whose swatches are swatch_side wide; the first one bold.
void table_header(const std::vector<TableCell>& cells, float width, float swatch_side);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
