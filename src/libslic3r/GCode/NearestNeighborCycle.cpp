// Nearest-neighbor TSP cycle: always pick the closest unvisited object, return to start.
// Produces a closed loop (cycle) through all object instances.

#include "NearestNeighborCycle.hpp"
#include "../Print.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    // Collect all instance centers.
    Points instance_centers;
    std::vector<std::pair<size_t, size_t>> instances;
    for (size_t i = 0; i < print_objects.size(); ++i) {
        const PrintObject& object = *print_objects[i];
        for (size_t j = 0; j < object.instances().size(); ++j) {
            instance_centers.emplace_back(object.instances()[j].shift);
            instances.emplace_back(i, j);
        }
    }

    if (instance_centers.empty())
        return {};

    size_t n = instance_centers.size();

    // Choose starting point: closest to start_near, or index 0.
    size_t current = 0;
    if (start_near != nullptr) {
        double min_dist2 = std::numeric_limits<double>::max();
        for (size_t i = 0; i < n; ++i) {
            double d2 = (instance_centers[i].cast<double>() - start_near->cast<double>()).squaredNorm();
            if (d2 < min_dist2) {
                min_dist2 = d2;
                current = i;
            }
        }
    }

    // Nearest-neighbor greedy: build the visit order as a ring buffer of indices.
    std::vector<bool> visited(n, false);
    std::vector<size_t> path;
    path.reserve(n);

    for (size_t step = 0; step < n; ++step) {
        visited[current] = true;
        path.emplace_back(current);

        // Find nearest unvisited instance.
        size_t next = 0;
        double min_dist2 = std::numeric_limits<double>::max();
        for (size_t j = 0; j < n; ++j) {
            if (visited[j]) continue;
            double d2 = (instance_centers[j].cast<double>() - instance_centers[current].cast<double>()).squaredNorm();
            if (d2 < min_dist2) {
                min_dist2 = d2;
                next = j;
            }
        }
        current = next;
    }

    // Rotate the path to minimize the closing edge (last -> first).
    // This ensures the loop is closed: the printer finishes near where it started.
    size_t best_start = 0;
    double best_close2 = std::numeric_limits<double>::max();
    for (size_t s = 0; s < n; ++s) {
        size_t idx_last = (s + n - 1) % n;
        size_t idx_first = s;
        double close2 = (instance_centers[idx_last].cast<double>() - instance_centers[idx_first].cast<double>()).squaredNorm();
        if (close2 < best_close2) {
            best_close2 = close2;
            best_start = s;
        }
    }

    // Apply the rotation.
    std::vector<size_t> cycle(n);
    for (size_t step = 0; step < n; ++step)
        cycle[step] = path[(best_start + step) % n];

    // 2-opt improvement: repeatedly try reversing segments to reduce total path length.
    // Reversing a segment of a non-crossing cycle preserves the non-crossing property,
    // so the result stays non-self-intersecting while getting shorter.
    bool improved = true;
    while (improved) {
        improved = false;
        for (size_t i = 0; i < n; ++i) {
            size_t i_next = (i + 1) % n;
            for (size_t j = i + 2; j < n; ++j) {
                // Skip adjacent edges (they share a vertex, can't truly cross).
                if (j == i_next) continue;
                if (j == (n - 1) && i == 0) continue; // closing edge adjacent to first edge

                size_t j_next = (j + 1) % n;

                // Current edges: (i -> i_next) and (j -> j_next)
                // New edges after reversal: (i -> j) and (i_next -> j_next)
                double current_dist = (instance_centers[cycle[i_next]].cast<double>() - instance_centers[cycle[i]].cast<double>()).norm()
                                   + (instance_centers[cycle[j_next]].cast<double>() - instance_centers[cycle[j]].cast<double>()).norm();
                double new_dist     = (instance_centers[cycle[j]].cast<double>() - instance_centers[cycle[i]].cast<double>()).norm()
                                   + (instance_centers[cycle[j_next]].cast<double>() - instance_centers[cycle[i_next]].cast<double>()).norm();

                if (new_dist < current_dist) {
                    // Reverse the segment from i_next to j.
                    while (i_next < j) {
                        std::swap(cycle[i_next], cycle[j]);
                        ++i_next;
                        --j;
                    }
                    improved = true;
                    break; // restart scan after modification
                }
            }
            if (improved) break;
        }
    }

    // Emit the final cycle.
    std::vector<const PrintInstance*> out;
    out.reserve(n);
    for (size_t step = 0; step < n; ++step) {
        size_t idx = cycle[step];
        out.emplace_back(&print_objects[instances[idx].first]->instances()[instances[idx].second]);
    }

    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const Print& print)
{
    return chain_print_object_instances_nn_cycle(print.objects().vector(), nullptr);
}

} // namespace Slic3r
