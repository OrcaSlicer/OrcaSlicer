#pragma once

#include "libslic3r/Point.hpp"

#include <map>
#include <vector>

// Orca: rules of the custom plate layout, where every plate sits in a cell (column, row) of the plate grid.
// Rows grow toward -Y on the bed. Cells may be negative, and no two plates share a cell.
namespace Slic3r::PlateLayout {

using Cell = Vec2i32;

// The cells every plate takes when plate `index` (a position in `cells`) is dropped on `cell`.
// An occupied cell makes room: in the plate's own row the plates in between shift one cell toward
// the cell it leaves; in another row the plate there and its neighbors on the right shift one cell right,
// up to the first free cell. Returns the plates that move with their new cells, or nothing when the drop
// is impossible: the cell is the plate's own, or a cell would be `limit` or more cells from (0, 0).
std::map<int, Cell> plan_drop(const std::vector<Cell>& cells, int index, const Cell& cell, int limit);

// The first free cell inside the layout's bounding box by reading order, or right of the layout on its top row.
Cell next_free_cell(const std::vector<Cell>& cells);

} // namespace Slic3r::PlateLayout
