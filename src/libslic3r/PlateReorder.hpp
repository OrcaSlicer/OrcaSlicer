#pragma once

// Orca: where a plate dragged over the plate grid is inserted. Plates sit in the automatic grid in reading order:
// plate i is in column i % cols of row i / cols.
namespace Slic3r::PlateReorder {

struct Target
{
    int  insert{0};    // position in [0, count] the plate goes before; count is after the last plate
    int  plate{0};     // plate the cursor is over (or nearest to)
    bool after{false}; // the drop marker is right of that plate, otherwise left of it
};

// `col` and `row` locate the cursor in cells: a plate covers [c, c + plate_width) of column c, where plate_width is
// the plate's share of the cell (the rest is the gap to the next column), and row r covers [r, r + 1).
// A cursor in a plate's right half, or in the gap right of it, inserts after that plate. Past the last plate, or
// outside the grid, the nearest plate is used. `count` and `cols` must be at least 1.
Target drop_target(int count, int cols, double col, double row, double plate_width);

// The index the plate at `from` gets when inserted before position `insert`, or -1 when the order stays the same.
int index_after_insert(int from, int insert);

} // namespace Slic3r::PlateReorder
