// Convex hull peeling (onion peeling) with concave hulls:
// Repeatedly extract a concave hull of remaining points (tighter than convex).
// Visit each layer (outermost first) in perimeter order, connecting layers at closest points.
// Rotate the final path so the closing edge (last → first) is short.

#ifndef slic3r_ConvexHullPeeling_hpp_
#define slic3r_ConvexHullPeeling_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Chain instances of print objects using convex hull peeling (onion peeling).
// Repeatedly extracts a concave hull of remaining points, visits each layer in perimeter order,
// applies per-layer and global 2-opt, and rotates to minimize the closing edge.
std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const Print& print);

} // namespace Slic3r

#endif /* slic3r_ConvexHullPeeling_hpp_ */
