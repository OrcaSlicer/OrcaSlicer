#ifndef slic3r_Geometry_ConvexHull_hpp_
#define slic3r_Geometry_ConvexHull_hpp_

#include <vector>

#include "../Polygon.hpp"

namespace Slic3r {

class ExPolygon;
using ExPolygons = std::vector<ExPolygon>;

namespace Geometry {

Pointf3s convex_hull(Pointf3s points);
Polygon convex_hull(Points points);
Polygon convex_hull(const Polygons &polygons);
Polygon convex_hull(const ExPolygons &expolygons);
Polygon convex_hulll(const Polylines &polylines);

// Returns true if the intersection of the two convex polygons A and B
// is not an empty set.
bool convex_polygons_intersect(const Polygon &A, const Polygon &B);

// Decompose source convex hull points into top / bottom chains with monotonically increasing x,
// creating an implicit trapezoidal decomposition of the source convex polygon.
// The source convex polygon has to be CCW oriented. O(n) time complexity.
std::pair<std::vector<Vec2d>, std::vector<Vec2d>> decompose_convex_polygon_top_bottom(const std::vector<Vec2d> &src);

// Convex polygon check using a top / bottom chain decomposition with O(log n) time complexity.
bool inside_convex_polygon(const std::pair<std::vector<Vec2d>, std::vector<Vec2d>> &top_bottom_decomposition, const Vec2d &pt);

} } // namespace Slicer::Geometry

#endif
