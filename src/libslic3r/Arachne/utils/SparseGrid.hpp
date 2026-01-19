//Copyright (c) 2016 Scott Lenser
//Copyright (c) 2018 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_SPARSE_GRID_H
#define UTILS_SPARSE_GRID_H

#include <cassert>
#include <vector>
#include <functional>

#include "../../Point.hpp"
#include "SquareGrid.hpp"

namespace Slic3r::Arachne {

/*! \brief Sparse grid which can locate spatially nearby elements efficiently.
 * 
 * \note This is an abstract template class which doesn't have any functions to insert elements.
 * \see SparsePointGrid
 *
 * \tparam ElemT The element type to store.
 */
template<class ElemT> class SparseGrid : public SquareGrid
{
public:
    using Elem = ElemT;

    using GridPoint    = SquareGrid::GridPoint;
    using grid_coord_t = SquareGrid::grid_coord_t;
    using GridMap       = std::unordered_multimap<GridPoint, Elem, PointHash>;

    using iterator       = typename GridMap::iterator;
    using const_iterator = typename GridMap::const_iterator;

    /*! \brief Constructs a sparse grid with the specified cell size.
     *
     * \param[in] cell_size The size to use for a cell (square) in the grid.
     *    Typical values would be around 0.5-2x of expected query radius.
     * \param[in] elem_reserve Number of elements to research space for.
     * \param[in] max_load_factor Maximum average load factor before rehashing.
     */
    SparseGrid(coord_t cell_size, size_t elem_reserve=0U, float max_load_factor=1.0f);

    iterator begin() { return m_grid.begin(); }
    iterator end() { return m_grid.end(); }
    const_iterator begin() const { return m_grid.begin(); }
    const_iterator end() const { return m_grid.end(); }

    /*! \brief Returns all data within radius of query_pt.
     *
     * Finds all elements with location within radius of \p query_pt.  May
     * return additional elements that are beyond radius.
     *
     * Average running time is a*(1 + 2 * radius / cell_size)**2 +
     * b*cnt where a and b are proportionality constance and cnt is
     * the number of returned items.  The search will return items in
     * an area of (2*radius + cell_size)**2 on average.  The max range
     * of an item from the query_point is radius + cell_size.
     *
     * \param[in] query_pt The point to search around.
     * \param[in] radius The search radius.
     * \return Vector of elements found
     */
    std::vector<Elem> getNearby(const Point &query_pt, coord_t radius) const;

    /*! \brief Process elements from cells that might contain sought after points.
     *
     * Processes elements from cell that might have elements within \p
     * radius of \p query_pt.  Processes all elements that are within
     * radius of query_pt.  May process elements that are up to radius +
     * cell_size from query_pt.
     *
     * \param[in] query_pt The point to search around.
     * \param[in] radius The search radius.
     * \param[in] process_func Processes each element.  process_func(elem) is
     *    called for each element in the cell. Processing stops if function returns false.
     * \return Whether we need to continue processing after this function
     */
    bool processNearby(const Point &query_pt, coord_t radius, const std::function<bool(const ElemT &)> &process_func) const;

protected:
    /*! \brief Process elements from the cell indicated by \p grid_pt.
     *
     * \param[in] grid_pt The grid coordinates of the cell.
     * \param[in] process_func Processes each element.  process_func(elem) is
     *    called for each element in the cell. Processing stops if function returns false.
     * \return Whether we need to continue processing a next cell.
     */
    bool processFromCell(const GridPoint &grid_pt, const std::function<bool(const Elem &)> &process_func) const;

    /*! \brief Map from grid locations (GridPoint) to elements (Elem). */
    GridMap m_grid;
};

template<class ElemT> SparseGrid<ElemT>::SparseGrid(coord_t cell_size, size_t elem_reserve, float max_load_factor) : SquareGrid(cell_size)
{
    // Must be before the reserve call.
    m_grid.max_load_factor(max_load_factor);
    if (elem_reserve != 0U)
        m_grid.reserve(elem_reserve);
}

template<class ElemT> bool SparseGrid<ElemT>::processFromCell(const GridPoint &grid_pt, const std::function<bool(const Elem &)> &process_func) const
{
    auto grid_range = m_grid.equal_range(grid_pt);
    for (auto iter = grid_range.first; iter != grid_range.second; ++iter)
        if (!process_func(iter->second))
            return false;
    return true;
}

template<class ElemT>
bool SparseGrid<ElemT>::processNearby(const Point &query_pt, coord_t radius, const std::function<bool(const Elem &)> &process_func) const
{
    return SquareGrid::processNearby(query_pt, radius, [&process_func, this](const GridPoint &grid_pt) { return processFromCell(grid_pt, process_func); });
}

template<class ElemT> std::vector<typename SparseGrid<ElemT>::Elem> SparseGrid<ElemT>::getNearby(const Point &query_pt, coord_t radius) const
{
    std::vector<Elem>                       ret;
    const std::function<bool(const Elem &)> process_func = [&ret](const Elem &elem) {
        ret.push_back(elem);
        return true;
    };
    processNearby(query_pt, radius, process_func);
    return ret;
}

} // namespace Slic3r::Arachne

#endif // UTILS_SPARSE_GRID_H
