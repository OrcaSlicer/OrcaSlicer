//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_SQUARE_GRID_H
#define UTILS_SQUARE_GRID_H

#include <stdint.h>
#include <cassert>
#include <vector>
#include <functional>
#include <utility>
#include <cinttypes>

#include "../../Point.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne {

/*!
 * Helper class to calculate coordinates on a square grid, and providing some
 * utility functions to process grids.
 *
 * Doesn't contain any data, except cell size. The purpose is only to
 * automatically generate coordinates on a grid, and automatically feed them to
 * functions.
 * The grid is theoretically infinite (bar integer limits).
 */
class SquareGrid
{
public:
    /*! \brief Constructs a grid with the specified cell size.
     * \param[in] cell_size The size to use for a cell (square) in the grid.
     */
    SquareGrid(const coord_t cell_size);

    /*!
     * Get the cell size this grid was created for.
     */
    coord_t getCellSize() const;

    using GridPoint = Point;
    using grid_coord_t = coord_t;

    /*! \brief Process cells along a line indicated by \p line.
     *
     * \param line The line along which to process cells.
     * \param process_func Processes each cell. ``process_func(elem)`` is called
     * for each cell. Processing stops if function returns false.
     * \return Whether we need to continue processing after this function.
     */
    bool processLineCells(const std::pair<Point, Point> line, const std::function<bool (GridPoint)>& process_cell_func);

    /*! \brief Process cells along a line indicated by \p line.
     *
     * \param line The line along which to process cells
     * \param process_func Processes each cell. ``process_func(elem)`` is called
     * for each cell. Processing stops if function returns false.
     * \return Whether we need to continue processing after this function.
     */
    bool processLineCells(const std::pair<Point, Point> line, const std::function<bool (GridPoint)>& process_cell_func) const;

    /*! \brief Process cells that might contain sought after points.
     *
     * Processes cells that might be within a square with twice \p radius as
     * width, centered around \p query_pt.
     * May process elements that are up to radius + cell_size from query_pt.
     * \param query_pt The point to search around.
     * \param radius The search radius.
     * \param process_func Processes each cell. ``process_func(loc)`` is called
     * for each cell coord within range. Processing stops if function returns
     * ``false``.
     * \return Whether we need to continue processing after this function.
     */
    bool processNearby(const Point &query_pt, coord_t radius, const std::function<bool(const GridPoint &)> &process_func) const;

    /*! \brief Compute the grid coordinates of a point.
     * \param point The actual location.
     * \return The grid coordinates that correspond to \p point.
     */
    GridPoint toGridPoint(const Vec2i64 &point) const;

    /*! \brief Compute the grid coordinate of a real space coordinate.
     * \param coord The actual location.
     * \return The grid coordinate that corresponds to \p coord.
     */
    grid_coord_t toGridCoord(const int64_t &coord) const;

    /*! \brief Compute the lowest coord in a grid cell.
     * The lowest point is the point in the grid cell closest to the origin.
     *
     * \param grid_coord The grid coordinate.
     * \return The print space coordinate that corresponds to \p grid_coord.
     */
    coord_t toLowerCoord(const grid_coord_t &grid_coord) const;

protected:
    /*! \brief The cell (square) size. */
    coord_t cell_size;

    /*!
     * Compute the sign of a number.
     *
     * The number 0 will result in a positive sign (1).
     * \param z The number to find the sign of.
     * \return 1 if the number is positive or 0, or -1 if the number is
     * negative.
     */
    grid_coord_t nonzeroSign(grid_coord_t z) const;
};

} // namespace Slic3r::Arachne

#endif //UTILS_SQUARE_GRID_H
