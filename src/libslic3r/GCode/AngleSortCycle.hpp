// Angle sort + 2-opt: sort points by angle around centroid, then 2-opt to improve.
// Produces a simple polygon that visits objects in angular order.

#ifndef slic3r_AngleSortCycle_hpp_
#define slic3r_AngleSortCycle_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Core algorithm: angle sort + 2-opt on a raw point set.
std::vector<size_t> angle_sort_core(const Points& centers);

// Chain instances of print objects by sorting by angle around centroid, then 2-opt.
// Produces a simple polygon that visits objects in angular order.
std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const Print& print);

} // namespace Slic3r

#endif /* slic3r_AngleSortCycle_hpp_ */
