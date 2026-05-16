// Print-object ordering strategies and shared TSP post-processing utilities.

#ifndef slic3r_OrderingStrategies_hpp_
#define slic3r_OrderingStrategies_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#ifndef SLIC3R_TEST_HARNESS
#include "../Print.hpp"
#endif

#include <algorithm>
#include <vector>

namespace Slic3r {

// --- Path improvement (operate on index vectors into `centers`) ---

// Standard 2-opt: repeatedly reverse segments that reduce total path length.
// `max_passes` limits full-scan iterations (0 = unlimited). Default is 10 passes,
// which captures >99% of possible improvement while preventing O(n²) blow-up
// on pathological inputs.
void tsp_2opt_improve(std::vector<size_t>& path, const Points& centers, int max_passes = 10);

// Crossing removal: reverse any segment pair whose edges geometrically cross.
void tsp_remove_crossings(std::vector<size_t>& path, const Points& centers);

// Rotate the cycle so the closing edge (last → first) is minimized.
void tsp_rotate_minimize_closing(std::vector<size_t>& path, const Points& centers);

// Total Euclidean path length of a cycle (including closing edge).
inline double tsp_cycle_path_length(const std::vector<size_t>& path, const Points& centers)
{
    if (path.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t next = (i + 1) % path.size();
        total += (centers[path[i]].cast<double>() - centers[path[next]].cast<double>()).norm();
    }
    return total;
}

// Maximum edge length of a cycle (including closing edge).
inline double tsp_max_edge_length(const std::vector<size_t>& path, const Points& centers)
{
    if (path.size() < 2) return 0.0;
    double mx = 0.0;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t next = (i + 1) % path.size();
        double d = (centers[path[i]].cast<double>() - centers[path[next]].cast<double>()).norm();
        if (d > mx) mx = d;
    }
    return mx;
}



#ifndef SLIC3R_TEST_HARNESS

// --- Wrapper boilerplate ---

// Collect instance centers from PrintObjects, optionally pre-rotate to honour
// start_near, call a core algorithm, and map the result back to PrintInstance*.
template<typename CoreFn>
std::vector<const PrintInstance*> chain_instances_with_core(
    const std::vector<const PrintObject*>& print_objects,
    const Point* start_near,
    CoreFn&& core_fn)
{
    Points instance_centers;
    std::vector<std::pair<size_t, size_t>> instances;
    for (size_t i = 0; i < print_objects.size(); ++i) {
        const PrintObject& object = *print_objects[i];
        for (size_t j = 0; j < object.instances().size(); ++j) {
            instance_centers.emplace_back(object.instances()[j].shift);
            instances.emplace_back(i, j);
        }
    }

    if (instance_centers.empty()) return {};

    // If start_near is provided, pre-rotate so closest point is first.
    if (start_near != nullptr) {
        size_t best_start = 0;
        double best_d2 = std::numeric_limits<double>::max();
        for (size_t k = 0; k < instance_centers.size(); ++k) {
            double d2 = (instance_centers[k].cast<double>() - start_near->cast<double>()).squaredNorm();
            if (d2 < best_d2) { best_d2 = d2; best_start = k; }
        }
        std::rotate(instance_centers.begin(), instance_centers.begin() + best_start, instance_centers.end());
        std::rotate(instances.begin(), instances.begin() + best_start, instances.end());
    }

    auto path = core_fn(instance_centers);

    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

#endif // SLIC3R_TEST_HARNESS

// --- Core algorithms (operate on raw Points, return index permutations) ---

// Snake ordering: row grouping + serpentine traversal + post-processing.
std::vector<size_t> snake_core(const Points& centers);

#ifndef SLIC3R_TEST_HARNESS

// --- Production wrappers ---

// Snake ordering.
std::vector<const PrintInstance*> chain_print_object_instances_snake(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_snake(const Print& print);

// Best-of-strategies: run all strategies and return the shortest result.
// Primary: shortest total path; secondary tiebreaker: smallest max edge.
std::vector<const PrintInstance*> chain_print_object_instances_best_of(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_best_of(const Print& print);

#endif // SLIC3R_TEST_HARNESS

} // namespace Slic3r

#endif /* slic3r_OrderingStrategies_hpp_ */
