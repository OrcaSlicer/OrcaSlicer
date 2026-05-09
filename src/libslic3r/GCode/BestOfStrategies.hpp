// Best-of-strategies: run all custom ordering strategies, return the shortest result.

#ifndef slic3r_BestOfStrategies_hpp_
#define slic3r_BestOfStrategies_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Run all custom ordering strategies on the same input and return the one
// with the shortest total path length.
std::vector<const PrintInstance*> chain_print_object_instances_best_of(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_best_of(const Print& print);

} // namespace Slic3r

#endif /* slic3r_BestOfStrategies_hpp_ */
