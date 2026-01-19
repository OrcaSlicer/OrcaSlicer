//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef LIGHTNING_DISTANCE_FIELD_H
#define LIGHTNING_DISTANCE_FIELD_H

#include "../../BoundingBox.hpp"
#include "../../Point.hpp"
#include "../../Polygon.hpp"

//#define LIGHTNING_DISTANCE_FIELD_DEBUG_OUTPUT

namespace Slic3r::FillLightning
{

/*!
 * 2D field that maintains locations which need to be supported for Lightning
 * Infill.
 *
 * This field contains a set of "cells", spaced out in a grid. Each cell
 * maintains how far it is removed from the edge, which is used to determine
 * how it gets supported by Lightning Infill.
 */
class DistanceField
{
public:
    /*!
     * Construct a new field to calculate Lightning Infill with.
     * \param radius The radius of influence that an infill line is expected to
     * support in the layer above.
     * \param current_outline The total infill area on this layer.
     * \param current_overhang The overhang that needs to be supported on this
     * layer.
     */
    DistanceField(const coord_t& radius, const Polygons& current_outline, const BoundingBox& current_outlines_bbox, const Polygons& current_overhang);
    
    /*!
     * Gets the next unsupported location to be supported by a new branch.
     * \param p Output variable for the next point to support.
     * \return ``true`` if successful, or ``false`` if there are no more points
     * to consider.
     */
    bool tryGetNextPoint(Point *out_unsupported_location, size_t *out_unsupported_cell_idx, const size_t start_idx = 0) const
    {
        for (size_t point_idx = start_idx; point_idx < m_unsupported_points.size(); ++point_idx) {
            if (!m_unsupported_points_erased[point_idx]) {
                *out_unsupported_cell_idx = point_idx;
                *out_unsupported_location = m_unsupported_points[point_idx].loc;
                return true;
            }
        }

        return false;
    }

    /*!
     * Update the distance field with a newly added branch.
     *
     * The branch is a line extending from \p to_node to \p added_leaf . This
     * function updates the grid cells so that the distance field knows how far
     * off it is from being supported by the current pattern. Grid points are
     * updated with sampling points spaced out by the supporting radius along
     * the line.
     * \param to_node The node endpoint of the newly added branch.
     * \param added_leaf The location of the leaf of the newly added branch,
     * drawing a straight line to the node.
     */
    void update(const Point& to_node, const Point& added_leaf);

protected:
    /*!
     * Spacing between grid points to consider supporting.
     */
    coord_t m_cell_size;

    /*!
     * The radius of the area of the layer above supported by a point on a
     * branch of a tree.
     */
    coord_t m_supporting_radius;
    int64_t m_supporting_radius2;

    /*!
     * Represents a small discrete area of infill that needs to be supported.
     */
    struct UnsupportedCell
    {
        // The position of the center of this cell.
        Point loc;
        // How far this cell is removed from the ``current_outline`` polygon, the edge of the infill area.
        coord_t dist_to_boundary;
    };

    /*!
     * Cells which still need to be supported at some point.
     */
    std::vector<UnsupportedCell> m_unsupported_points;
    std::vector<bool>            m_unsupported_points_erased;

    /*!
     * BoundingBox of all points in m_unsupported_points. Used for mapping of sign integer numbers to positive integer numbers.
     */
    const BoundingBox          m_unsupported_points_bbox;

    /*!
     * Links the unsupported points to a grid point, so that we can quickly look
     * up the cell belonging to a certain position in the grid.
     */

    class UnsupportedPointsGrid
    {
    public:
        UnsupportedPointsGrid() = default;
        void initialize(const std::vector<UnsupportedCell> &unsupported_points, const std::function<Point(const Point &)> &map_cell_to_grid)
        {
            if (unsupported_points.empty())
                return;

            BoundingBox unsupported_points_bbox;
            for (const UnsupportedCell &cell : unsupported_points)
                unsupported_points_bbox.merge(cell.loc);

            m_size        = unsupported_points.size();
            m_grid_range  = BoundingBox(map_cell_to_grid(unsupported_points_bbox.min), map_cell_to_grid(unsupported_points_bbox.max));
            m_grid_size   = m_grid_range.size() + Point::Ones();

            m_data.assign(m_grid_size.y() * m_grid_size.x(), std::numeric_limits<size_t>::max());
            m_data_erased.assign(m_grid_size.y() * m_grid_size.x(), true);

            for (size_t cell_idx = 0; cell_idx < unsupported_points.size(); ++cell_idx) {
                const size_t flat_idx   = map_to_flat_array(map_cell_to_grid(unsupported_points[cell_idx].loc));
                assert(m_data[flat_idx] == std::numeric_limits<size_t>::max());
                m_data[flat_idx]        = cell_idx;
                m_data_erased[flat_idx] = false;
            }
        }

        size_t size() const { return m_size; }

        size_t find_cell_idx(const Point &grid_addr)
        {
            if (!m_grid_range.contains(grid_addr))
                return std::numeric_limits<size_t>::max();

            if (const size_t flat_idx = map_to_flat_array(grid_addr); !m_data_erased[flat_idx]) {
                assert(m_data[flat_idx] != std::numeric_limits<size_t>::max());
                return m_data[flat_idx];
            }

            return std::numeric_limits<size_t>::max();
        }

        void mark_erased(const Point &grid_addr)
        {
            assert(m_grid_range.contains(grid_addr));
            if (!m_grid_range.contains(grid_addr))
                return;

            const size_t flat_idx = map_to_flat_array(grid_addr);
            assert(!m_data_erased[flat_idx] && m_data[flat_idx] != std::numeric_limits<size_t>::max());
            assert(m_size != 0);

            m_data_erased[flat_idx] = true;
            --m_size;
        }

    private:
        size_t m_size = 0;

        BoundingBox m_grid_range;
        Point       m_grid_size;

        std::vector<size_t> m_data;
        std::vector<bool>   m_data_erased;

        inline size_t map_to_flat_array(const Point &loc) const
        {
            const Point  offset_loc = loc - m_grid_range.min;
            const size_t flat_idx   = m_grid_size.x() * offset_loc.y() + offset_loc.x();
            assert(offset_loc.x() >= 0 && offset_loc.y() >= 0);
            assert(flat_idx < size_t(m_grid_size.y() * m_grid_size.x()));
            return flat_idx;
        }
    };

    UnsupportedPointsGrid m_unsupported_points_grid;

    /*!
     * Maps the point to the grid coordinates.
     */
    Point to_grid_point(const Point &point) const {
        return (point - m_unsupported_points_bbox.min) / m_cell_size;
    }

    /*!
     * Maps the point to the grid coordinates.
     */
    Point from_grid_point(const Point &point) const {
        return point * m_cell_size + m_unsupported_points_bbox.min;
    }

#ifdef LIGHTNING_DISTANCE_FIELD_DEBUG_OUTPUT
    friend void export_distance_field_to_svg(const std::string &path, const Polygons &outline, const Polygons &overhang, const std::list<DistanceField::UnsupportedCell> &unsupported_points, const Points &points);
#endif
};

} // namespace Slic3r::FillLightning

#endif //LIGHTNING_DISTANCE_FIELD_H
