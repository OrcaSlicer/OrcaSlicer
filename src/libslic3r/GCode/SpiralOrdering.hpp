// Spiral ordering: sort objects by distance from centroid, then by angle.
// Naturally produces concentric/spiral paths.

#ifndef slic3r_SpiralOrdering_hpp_
#define slic3r_SpiralOrdering_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Chain instances using a spiral pattern: sort by distance from centroid, then angle.
std::vector<const PrintInstance*> chain_print_object_instances_spiral(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_spiral(const Print& print);

} // namespace Slic3r

#endif /* slic3r_SpiralOrdering_hpp_ */
