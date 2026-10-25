#include "PlateLayout.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

namespace Slic3r::PlateLayout {

namespace {

using Key = std::pair<int, int>; // (row, column), so that std::map iterates in reading order

Key key(const Cell& cell) { return {cell.y(), cell.x()}; }

std::map<Key, int> occupants(const std::vector<Cell>& cells)
{
    std::map<Key, int> occupant;
    for (int i = 0; i < (int) cells.size(); ++i)
        occupant.emplace(key(cells[i]), i);
    return occupant;
}

} // namespace

std::map<int, Cell> plan_drop(const std::vector<Cell>& cells, int index, const Cell& cell, int limit)
{
    const auto in_range = [limit](const Cell& c) { return std::abs(c.x()) < limit && std::abs(c.y()) < limit; };
    if (index < 0 || index >= (int) cells.size() || !in_range(cell) || cell == cells[index])
        return {};

    const std::map<Key, int> occupant = occupants(cells);
    const auto plate_at = [&occupant](int col, int row) {
        const auto it = occupant.find({row, col});
        return it == occupant.end() ? -1 : it->second;
    };

    const Cell&         from = cells[index];
    std::map<int, Cell> moves{{index, cell}};
    if (plate_at(cell.x(), cell.y()) < 0)
        return moves;
    if (from.y() == cell.y()) {
        const int step = cell.x() > from.x() ? -1 : 1;
        for (int col = cell.x(); col != from.x(); col += step)
            if (const int i = plate_at(col, cell.y()); i >= 0)
                moves[i] = Cell(col + step, cell.y());
    } else {
        for (int col = cell.x(); plate_at(col, cell.y()) >= 0; ++col) {
            if (!in_range(Cell(col + 1, cell.y())))
                return {};
            moves[plate_at(col, cell.y())] = Cell(col + 1, cell.y());
        }
    }
    return moves;
}

std::vector<Cell> close_gaps(const std::vector<Cell>& cells)
{
    std::vector<Cell> result(cells.size());
    int               row = -1, col = 0, last_row = 0;
    for (const int i : reading_order(cells)) {
        if (row < 0 || cells[i].y() != last_row) {
            ++row;
            col      = 0;
            last_row = cells[i].y();
        }
        result[i] = Cell(col++, row);
    }
    return result;
}

std::vector<int> reading_order(const std::vector<Cell>& cells)
{
    std::vector<int> order(cells.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&cells](int a, int b) { return key(cells[a]) < key(cells[b]); });
    return order;
}

Cell next_free_cell(const std::vector<Cell>& cells)
{
    if (cells.empty())
        return Cell(0, 0);
    const std::map<Key, int> occupant = occupants(cells);
    Cell                     min = cells.front(), max = cells.front();
    for (const Cell& c : cells) {
        min = min.cwiseMin(c);
        max = max.cwiseMax(c);
    }
    for (int row = min.y(); row <= max.y(); ++row)
        for (int col = min.x(); col <= max.x(); ++col)
            if (occupant.count({row, col}) == 0)
                return Cell(col, row);
    return Cell(max.x() + 1, min.y());
}

} // namespace Slic3r::PlateLayout
