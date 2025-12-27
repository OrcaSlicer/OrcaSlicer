#ifndef slic3r_StreamUtils_hpp_
#define slic3r_StreamUtils_hpp_

#include "Point.hpp"
#include "libslic3r.h"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "ExPolygon.hpp"
#include <sstream>
#include <vector>

namespace Slic3r {

inline std::ostream& operator<<(std::ostream& os, const Points& pts)
{
    os << "[" << pts.size() << "]:";
    for (Point p : pts)
        os << " (" << p << ")";
    os << "\n";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const MultiPoint& mpts)
{
    os << "Multipoint" << mpts.points;
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const Polygon& poly)
{
    os << "Polygon" << poly.points;
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const Polygons& polys)
{
    os << "Polygons[" << polys.size() << "]:" << "\n";
    for (Polygon p : polys)
        os << " " << p;
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const ExPolygon& epoly)
{
    os << "ExPolygon:\n";
    os << "  contour: " << epoly.contour;
    os << "  holes: " << epoly.holes;
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const ExPolygons& epolys)
{
    os << "ExPolygons[" << epolys.size() << "]:" << "\n";
    for (ExPolygon p : epolys)
        os << " " << p;
    return os;
}

} // namespace Slic3r
#endif // slic3r_StreamUtils_hpp_
