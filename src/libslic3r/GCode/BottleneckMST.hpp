// Bottleneck MST ordering: minimize the maximum edge length in the path.
// Uses binary search on threshold T to find minimum connected subgraph,
// then DFS-traverses preferring short edges. O(n² log n) total.

#ifndef slic3r_BottleneckMST_hpp_
#define slic3r_BottleneckMST_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Core algorithm: bottleneck MST ordering on a raw point set.
std::vector<size_t> bottleneck_mst_core(const Points& centers);

// Chain instances using bottleneck MST ordering.
std::vector<const PrintInstance*> chain_print_object_instances_bottleneck_mst(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_bottleneck_mst(const Print& print);

} // namespace Slic3r

#endif /* slic3r_BottleneckMST_hpp_ */
