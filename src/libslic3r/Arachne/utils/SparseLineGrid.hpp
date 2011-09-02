//Copyright (c) 2018 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.


#ifndef UTILS_SPARSE_LINE_GRID_H
#define UTILS_SPARSE_LINE_GRID_H

#include <cassert>
#include <vector>
#include <functional>

#include "SparseGrid.hpp"

namespace Slic3r::Arachne {

/*! \brief Sparse grid which can locate spatially nearby elements efficiently.
 *
 * \tparam ElemT The element type to store.
 * \tparam Locator The functor to get the start and end locations from ElemT.
 *    must have: std::pair<Point, Point> operator()(const ElemT &elem) const
 *    which returns the location associated with val.
 */
template<class ElemT, class Locator> class SparseLineGrid : public SparseGrid<ElemT>
{
public:
    using Elem = ElemT;

    /*! \brief Constructs a sparse grid with the specified cell size.
     *
     * \param[in] cell_size The size to use for a cell (square) in the grid.
     *    Typical values would be around 0.5-2x of expected query radius.
     * \param[in] elem_reserve Number of elements to research space for.
     * \param[in] max_load_factor Maximum average load factor before rehashing.
     */
    SparseLineGrid(coord_t cell_size, size_t elem_reserve = 0U, float max_load_factor = 1.0f);

    /*! \brief Inserts elem into the sparse grid.
     *
     * \param[in] elem The element to be inserted.
     */
    void insert(const Elem &elem);

protected:
    using GridPoint = typename SparseGrid<ElemT>::GridPoint;

    /*! \brief Accessor for getting locations from elements. */
    Locator m_locator;
};

template<class ElemT, class Locator>
SparseLineGrid<ElemT, Locator>::SparseLineGrid(coord_t cell_size, size_t elem_reserve, float max_load_factor)
    : SparseGrid<ElemT>(cell_size, elem_reserve, max_load_factor) {}

template<class ElemT, class Locator> void SparseLineGrid<ElemT, Locator>::insert(const Elem &elem)
{
    const std::pair<Point, Point> line = m_locator(elem);
    using GridMap                      = std::unordered_multimap<GridPoint, Elem, PointHash>;
    // below is a workaround for the fact that lambda functions cannot access private or protected members
    // first we define a lambda which works on any GridMap and then we bind it to the actual protected GridMap of the parent class
    std::function<bool(GridMap *, const GridPoint)> process_cell_func_ = [&elem](GridMap *m_grid, const GridPoint grid_loc) {
        m_grid->emplace(grid_loc, elem);
        return true;
    };
    using namespace std::placeholders; // for _1, _2, _3...
    GridMap                             *m_grid = &(this->m_grid);
    std::function<bool(const GridPoint)> process_cell_func(std::bind(process_cell_func_, m_grid, _1));

    SparseGrid<ElemT>::processLineCells(line, process_cell_func);
}

#undef SGI_TEMPLATE
#undef SGI_THIS

} // namespace Slic3r::Arachne

#endif // UTILS_SPARSE_LINE_GRID_H
