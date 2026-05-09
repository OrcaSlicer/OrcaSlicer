// Nearest-neighbor TSP cycle: always pick the closest unvisited object, return to start.
// Produces a closed loop (cycle) through all object instances.

#include "NearestNeighborCycle.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"

#include <cmath>
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

    // 2-opt improvement: repeatedly try reversing segments to reduce total path length.
    bool improved = true;
    while (improved) {
        improved = false;
        for (size_t i = 0; i < n; ++i) {
            size_t i_next = (i + 1) % n;
            for (size_t j = i + 2; j < n; ++j) {
                if (j == i_next) continue;
                if (j == (n - 1) && i == 0) continue;

                size_t j_next = (j + 1) % n;
                double current_dist = (centers[path[i_next]].cast<double>() - centers[path[i]].cast<double>()).norm()
                                   + (centers[path[j_next]].cast<double>() - centers[path[j]].cast<double>()).norm();
                double new_dist     = (centers[path[j]].cast<double>() - centers[path[i]].cast<double>()).norm()
                                   + (centers[path[j_next]].cast<double>() - centers[path[i_next]].cast<double>()).norm();

                if (new_dist < current_dist) {
                    while (i_next < j) { std::swap(path[i_next], path[j]); ++i_next; --j; }
                    improved = true;
                    break;
                }
            }
            if (improved) break;
        }
    }

    // Crossing removal pass: reverse segments that cross, regardless of distance impact.
    {
        int max_iters = static_cast<int>(n * n);
        bool had_crossing = true;
        while (had_crossing && max_iters-- > 0) {
            had_crossing = false;
            for (size_t i = 0; i < n && !had_crossing; ++i) {
                size_t i_next = (i + 1) % n;
                for (size_t j = i + 2; j < n && !had_crossing; ++j) {
                    if (j == i_next) continue;
                    if (j == (n - 1) && i == 0) continue;
                    size_t j_next = (j + 1) % n;
                    if (Geometry::segments_intersect(
                            centers[path[i]], centers[path[i_next]],
                            centers[path[j]], centers[path[j_next]])) {
                        while (i_next < j) { std::swap(path[i_next], path[j]); ++i_next; --j; }
                        had_crossing = true;
                    }
                }
            }
        }
    }

    // Rotate the path to minimize the closing edge (last -> first).
    {
        size_t best_start = 0;
        double best_close2 = std::numeric_limits<double>::max();
        for (size_t s = 0; s < n; ++s) {
            size_t idx_last = (s + n - 1) % n;
            double close2 = (centers[path[idx_last]].cast<double>() - centers[path[s]].cast<double>()).squaredNorm();
            if (close2 < best_close2) { best_close2 = close2; best_start = s; }
        }
        std::rotate(path.begin(), path.begin() + best_start, path.end());
    }

    return path;
}

// Production wrapper: extract points from PrintObjects, call core, convert back.
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

    if (instance_centers.empty()) return {};

    // If start_near is provided, rotate the point set so closest point is first.
    // The core algorithm always starts at index 0, so we pre-rotate to honor start_near.
    if (start_near != nullptr) {
        size_t best_start = 0;
        double best_d2 = std::numeric_limits<double>::max();
        for (size_t k = 0; k < instance_centers.size(); ++k) {
            double d2 = (instance_centers[k].cast<double>() - start_near->cast<double>()).squaredNorm();
            if (d2 < best_d2) { best_d2 = d2; best_start = k; }
        }
        // Rotate instance_centers and instances together.
        std::rotate(instance_centers.begin(), instance_centers.begin() + best_start, instance_centers.end());
        std::rotate(instances.begin(), instances.begin() + best_start, instances.end());
    }

    auto path = nn_cycle_core(instance_centers);

    // Emit the final cycle.
    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_nn_cycle(const Print& print)
{
    return chain_print_object_instances_nn_cycle(print.objects().vector(), nullptr);
}

} // namespace Slic3r
