// Nearest-neighbor TSP cycle: always pick the closest unvisited object, return to start.
// Produces a closed loop (cycle) through all object instances.

#include "NearestNeighborCycle.hpp"
#include "TSPPostProcessing.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Core algorithm: nearest-neighbor TSP cycle on a raw point set.
std::vector<size_t> nn_cycle_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Nearest-neighbor greedy: build the visit order.
    std::vector<bool> visited(n, false);
    std::vector<size_t> path;
    path.reserve(n);

    size_t current = 0;
    for (size_t step = 0; step < n; ++step) {
        visited[current] = true;
        path.emplace_back(current);

        // Find nearest unvisited instance.
        size_t next = 0;
        double min_dist2 = std::numeric_limits<double>::max();
        for (size_t j = 0; j < n; ++j) {
            if (visited[j]) continue;
            double d2 = (centers[j].cast<double>() - centers[current].cast<double>()).squaredNorm();
            if (d2 < min_dist2) { min_dist2 = d2; next = j; }
        }
        current = next;
    }

    // Post-processing: 2-opt, crossing removal, rotation.
    tsp_2opt_improve(path, centers);
    tsp_remove_crossings(path, centers);
    tsp_rotate_minimize_closing(path, centers);

    return path;
}

// Production wrapper: extract points from PrintObjects, call core, convert back.
std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, nn_cycle_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const Print& print)
{
    return chain_print_object_instances_nn_cycle(print.objects().vector(), nullptr);
}

} // namespace Slic3r
