// Nearest-neighbor TSP cycle: closed loop through all object instances.

#ifndef slic3r_NearestNeighborCycle_hpp_
#define slic3r_NearestNeighborCycle_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Core algorithm: nearest-neighbor TSP cycle on a raw point set.
// Returns permutation indices into `centers` forming a closed loop.
std::vector<size_t> nn_cycle_core(const Points& centers);

// Chain instances of print objects into a nearest-neighbor TSP cycle (closed loop).
// Starts at the object closest to start_near (or first object if start_near is nullptr),
// always visits the nearest unvisited object, applies 2-opt to shorten the path,
// and rotates to minimize the closing edge.
std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const Print& print);

} // namespace Slic3r

#endif /* slic3r_NearestNeighborCycle_hpp_ */
