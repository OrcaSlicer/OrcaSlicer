// Min-Max Edge ordering: run all individual strategies and select the one with
// the smallest maximum edge length (bottleneck criterion), not shortest total path.

#ifndef slic3r_MinMaxEdge_hpp_
#define slic3r_MinMaxEdge_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Core algorithm: min-max-edge selection on a raw point set.
std::vector<size_t> min_max_edge_core(const Points& centers);

// Chain instances using min-max-edge ordering.
std::vector<const PrintInstance*> chain_print_object_instances_min_max_edge(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_min_max_edge(const Print& print);

} // namespace Slic3r

#endif /* slic3r_MinMaxEdge_hpp_ */
