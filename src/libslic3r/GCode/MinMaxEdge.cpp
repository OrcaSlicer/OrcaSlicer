// Min-Max Edge ordering: run all individual strategies and select the one with
// the smallest maximum edge length (bottleneck criterion), not shortest total path.

#include "MinMaxEdge.hpp"
#include "TSPPostProcessing.hpp"
#include "ConvexHullPeeling.hpp"
#include "Boustrophedon.hpp"
#include "GridPath.hpp"

namespace Slic3r {

// Core algorithm: run all strategies, pick the one with smallest max edge.
std::vector<size_t> min_max_edge_core(const Points& centers)
{
    if (centers.empty()) return {};
    size_t n = centers.size();
    if (n == 1) return {0};

    // Run all individual strategies.
    struct Candidate { std::string name; std::vector<size_t> path; };
    std::vector<Candidate> candidates;
    candidates.push_back({"Convex Hull Peeling", convex_hull_peeling_core(centers)});
    candidates.push_back({"Boustrophedon", boustrophedon_core(centers)});
    candidates.push_back({"Grid Path", grid_path_core(centers)});

    // Pick the one with smallest maximum edge, breaking ties by shortest total length.
    size_t best = 0;
    double best_max = tsp_max_edge_length(candidates[0].path, centers);
    double best_total = tsp_cycle_path_length(candidates[0].path, centers);

    for (size_t i = 1; i < candidates.size(); ++i) {
        const auto& path = candidates[i].path;
        if (path.empty()) continue;

        double mx = tsp_max_edge_length(path, centers);
        double total = tsp_cycle_path_length(path, centers);

        // Primary: smaller max edge wins.  Secondary: shorter total length.
        if (mx < best_max || (mx == best_max && total < best_total)) {
            best_max = mx; best_total = total; best = i;
        }
    }

    return candidates[best].path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_min_max_edge(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, min_max_edge_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_min_max_edge(const Print& print)
{
    return chain_print_object_instances_min_max_edge(print.objects().vector(), nullptr);
}

} // namespace Slic3r
