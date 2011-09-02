//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_POLYGONS_SEGMENT_INDEX_H
#define UTILS_POLYGONS_SEGMENT_INDEX_H

#include <vector>

#include "PolygonsPointIndex.hpp"

namespace Slic3r::Arachne
{

/*!
 * A class for iterating over the points in one of the polygons in a \ref Polygons object
 */
class PolygonsSegmentIndex : public PolygonsPointIndex
{
public:
    PolygonsSegmentIndex() : PolygonsPointIndex(){};
    PolygonsSegmentIndex(const Polygons *polygons, unsigned int poly_idx, unsigned int point_idx) : PolygonsPointIndex(polygons, poly_idx, point_idx){};

    Point from() const { return PolygonsPointIndex::p(); }

    Point to() const { return PolygonsSegmentIndex::next().p(); }
};

} // namespace Slic3r::Arachne

namespace boost::polygon {

template<> struct geometry_concept<Slic3r::Arachne::PolygonsSegmentIndex>
{
    typedef segment_concept type;
};

template<> struct segment_traits<Slic3r::Arachne::PolygonsSegmentIndex>
{
    typedef coord_t       coordinate_type;
    typedef Slic3r::Point point_type;

    static inline point_type get(const Slic3r::Arachne::PolygonsSegmentIndex &CSegment, direction_1d dir)
    {
        return dir.to_int() ? CSegment.to() : CSegment.from();
    }
};

} // namespace boost::polygon

#endif//UTILS_POLYGONS_SEGMENT_INDEX_H
