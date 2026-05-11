// GridPath: row-based serpentine ordering with L1 (Manhattan) distance optimization.
// Designed for grid-like layouts where diagonal moves should be avoided.
// Groups points into rows by Y, traverses each row alternating direction,
// then applies L1-aware 2-opt that only accepts swaps improving Manhattan distance.

#ifndef slic3r_GridPath_hpp_
#define slic3r_GridPath_hpp_

#include "../Point.hpp"

#include <vector>

namespace Slic3r {

// Core algorithm: grid path on a raw point set.
std::vector<size_t> grid_path_core(const Points& centers);

} // namespace Slic3r

#ifndef SLIC3R_TEST_HARNESS

#include "Print.hpp"

namespace Slic3r {

// Production wrappers.
std::vector<const PrintInstance*> chain_print_object_instances_grid_path(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_grid_path(const Print& print);

} // namespace Slic3r

#endif // SLIC3R_TEST_HARNESS

#endif /* slic3r_GridPath_hpp_ */
