// Best-of-strategies: run all custom ordering strategies, return the shortest result.

#include "BestOfStrategies.hpp"
#include "../Print.hpp"
#include "ConvexHullPeeling.hpp"
#include "Boustrophedon.hpp"
#include "GridPath.hpp"

#include <limits>

namespace Slic3r {

// Compute total path length of a cycle (including closing edge).
static double cycle_path_length(const std::vector<const PrintInstance*>& path)
{
    if (path.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t next = (i + 1) % path.size();
        total += (path[i]->shift.cast<double>() - path[next]->shift.cast<double>()).norm();
    }
    return total;
}

std::vector<const PrintInstance*> chain_print_object_instances_best_of(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    if (print_objects.empty())
        return {};

    // Run all strategies.
    std::vector<std::vector<const PrintInstance*>> candidates;
    candidates.push_back(chain_print_object_instances_convex_hull_peeling(print_objects, start_near));
    candidates.push_back(chain_print_object_instances_boustrophedon(print_objects, start_near));
    candidates.push_back(chain_print_object_instances_grid_path(print_objects, start_near));

    // Pick the shortest.
    size_t best = 0;
    double best_len = std::numeric_limits<double>::max();
    for (size_t i = 0; i < candidates.size(); ++i) {
        double len = cycle_path_length(candidates[i]);
        if (len < best_len) { best_len = len; best = i; }
    }

    return candidates[best];
}

std::vector<const PrintInstance*> chain_print_object_instances_best_of(const Print& print)
{
    return chain_print_object_instances_best_of(print.objects().vector(), nullptr);
}

} // namespace Slic3r
