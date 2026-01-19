//Copyright (c) 2018 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_POLYGONS_POINT_INDEX_H
#define UTILS_POLYGONS_POINT_INDEX_H

#include <vector>

#include "../../Point.hpp"
#include "../../Polygon.hpp"


namespace Slic3r::Arachne
{

// Identity function, used to be able to make templated algorithms where the input is sometimes points, sometimes things that contain or can be converted to points.
inline const Point &make_point(const Point &p) { return p; }

/*!
 * A class for iterating over the points in one of the polygons in a \ref Polygons object
 */
template<typename Paths>
class PathsPointIndex
{
public:
    /*!
     * The polygons into which this index is indexing.
     */
    const Paths* polygons; // (pointer to const polygons)

    unsigned int poly_idx; //!< The index of the polygon in \ref PolygonsPointIndex::polygons

    unsigned int point_idx; //!< The index of the point in the polygon in \ref PolygonsPointIndex::polygons

    /*!
     * Constructs an empty point index to no polygon.
     *
     * This is used as a placeholder for when there is a zero-construction
     * needed. Since the `polygons` field is const you can't ever make this
     * initialisation useful.
     */
    PathsPointIndex() : polygons(nullptr), poly_idx(0), point_idx(0) {}

    /*!
     * Constructs a new point index to a vertex of a polygon.
     * \param polygons The Polygons instance to which this index points.
     * \param poly_idx The index of the sub-polygon to point to.
     * \param point_idx The index of the vertex in the sub-polygon.
     */
    PathsPointIndex(const Paths *polygons, unsigned int poly_idx, unsigned int point_idx) : polygons(polygons), poly_idx(poly_idx), point_idx(point_idx) {}

    /*!
     * Copy constructor to copy these indices.
     */
    PathsPointIndex(const PathsPointIndex& original) = default;

    Point p() const
    {
        if (!polygons)
            return {0, 0};

        return make_point((*polygons)[poly_idx][point_idx]);
    }

    /*!
     * \brief Returns whether this point is initialised.
     */
    bool initialized() const { return polygons; }

    /*!
     * Get the polygon to which this PolygonsPointIndex refers
     */
    const Polygon &getPolygon() const { return (*polygons)[poly_idx]; }

    /*!
     * Test whether two iterators refer to the same polygon in the same polygon list.
     *
     * \param other The PolygonsPointIndex to test for equality
     * \return Wether the right argument refers to the same polygon in the same ListPolygon as the left argument.
     */
    bool operator==(const PathsPointIndex &other) const
    {
        return polygons == other.polygons && poly_idx == other.poly_idx && point_idx == other.point_idx;
    }
    bool operator!=(const PathsPointIndex &other) const
    {
        return !(*this == other);
    }
    bool operator<(const PathsPointIndex &other) const
    {
        return this->p() < other.p();
    }
    PathsPointIndex &operator=(const PathsPointIndex &other)
    {
        polygons  = other.polygons;
        poly_idx  = other.poly_idx;
        point_idx = other.point_idx;
        return *this;
    }
    //! move the iterator forward (and wrap around at the end)
    PathsPointIndex &operator++()
    {
        point_idx = (point_idx + 1) % (*polygons)[poly_idx].size();
        return *this;
    }
    //! move the iterator backward (and wrap around at the beginning)
    PathsPointIndex &operator--()
    {
        if (point_idx == 0)
            point_idx = (*polygons)[poly_idx].size();
        point_idx--;
        return *this;
    }
    //! move the iterator forward (and wrap around at the end)
    PathsPointIndex next() const
    {
        PathsPointIndex ret(*this);
        ++ret;
        return ret;
    }
    //! move the iterator backward (and wrap around at the beginning)
    PathsPointIndex prev() const
    {
        PathsPointIndex ret(*this);
        --ret;
        return ret;
    }
};

using PolygonsPointIndex = PathsPointIndex<Polygons>;

/*!
 * Locator to extract a line segment out of a \ref PolygonsPointIndex
 */
struct PolygonsPointIndexSegmentLocator
{
    std::pair<Point, Point> operator()(const PolygonsPointIndex &val) const
    {
        const Polygon &poly           = (*val.polygons)[val.poly_idx];
        Point          start          = poly[val.point_idx];
        unsigned int   next_point_idx = (val.point_idx + 1) % poly.size();
        Point          end            = poly[next_point_idx];
        return std::pair<Point, Point>(start, end);
    }
};

/*!
 * Locator of a \ref PolygonsPointIndex
 */
template<typename Paths>
struct PathsPointIndexLocator
{
    Point operator()(const PathsPointIndex<Paths>& val) const
    {
        return make_point(val.p());
    }
};

}//namespace Slic3r::Arachne

namespace std
{
/*!
 * Hash function for \ref PolygonsPointIndex
 */
template <>
struct hash<Slic3r::Arachne::PolygonsPointIndex>
{
    size_t operator()(const Slic3r::Arachne::PolygonsPointIndex& lpi) const
    {
        return Slic3r::PointHash{}(lpi.p());
    }
};
}//namespace std



#endif//UTILS_POLYGONS_POINT_INDEX_H
