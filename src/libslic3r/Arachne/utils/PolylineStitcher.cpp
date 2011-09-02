//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "PolylineStitcher.hpp"

#include "ExtrusionLine.hpp"
#include "libslic3r/Arachne/utils/PolygonsPointIndex.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {
namespace Arachne {
struct ExtrusionJunction;
}  // namespace Arachne
}  // namespace Slic3r

namespace Slic3r::Arachne {

template<> bool PolylineStitcher<VariableWidthLines, ExtrusionLine, ExtrusionJunction>::canReverse(const PathsPointIndex<VariableWidthLines> &ppi)
{
    if ((*ppi.polygons)[ppi.poly_idx].is_odd)
        return true;
    else
        return false;
}

template<> bool PolylineStitcher<Polygons, Polygon, Point>::canReverse(const PathsPointIndex<Polygons> &)
{
    return true;
}

template<> bool PolylineStitcher<VariableWidthLines, ExtrusionLine, ExtrusionJunction>::canConnect(const ExtrusionLine &a, const ExtrusionLine &b)
{
    return a.is_odd == b.is_odd;
}

template<> bool PolylineStitcher<Polygons, Polygon, Point>::canConnect(const Polygon &, const Polygon &)
{
    return true;
}

template<> bool PolylineStitcher<VariableWidthLines, ExtrusionLine, ExtrusionJunction>::isOdd(const ExtrusionLine &line)
{
    return line.is_odd;
}

template<> bool PolylineStitcher<Polygons, Polygon, Point>::isOdd(const Polygon &)
{
    return false;
}

} // namespace Slic3r::Arachne
