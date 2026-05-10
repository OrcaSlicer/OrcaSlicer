// Min-Max Edge ordering: run all individual strategies and select the one with
// the smallest maximum edge length (bottleneck criterion), not shortest total path.

#include "MinMaxEdge.hpp"
#include "../Print.hpp"
#include "ConvexHullPeeling.hpp"
#include "AngleSortCycle.hpp"
#include "Boustrophedon.hpp"

#include <cmath>
#include <limits>

namespace Slic3r {

// Compute maximum edge length of a cycle (including closing edge).
static double max_edge_length(const std::vector<const PrintInstance*>& path)
{
    if (path.size() < 2) return 0.0;
    double mx = 0.0;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t next = (i + 1) % path.size();
        double d = (path[i]->shift.cast<double>() - path[next]->shift.cast<double>()).norm();
        if (d > mx) mx = d;
    }
    return mx;
}

// Compute total path length of a cycle (including closing edge).
static double total_path_length(const std::vector<const PrintInstance*>& path)
{
    if (path.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t next = (i + 1) % path.size();
        total += (path[i]->shift.cast<double>() - path[next]->shift.cast<double>()).norm();
    }
    return total;
}

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
    candidates.push_back({"Angle Sort", angle_sort_core(centers)});
    candidates.push_back({"Boustrophedon", boustrophedon_core(centers)});

    // Pick the one with smallest maximum edge, breaking ties by shortest total length.
    size_t best = 0;
    double best_max2 = std::numeric_limits<double>::max();
    double best_total = std::numeric_limits<double>::max();

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& path = candidates[i].path;
        if (path.empty()) continue;

        double mx2 = 0.0, total = 0.0;
        for (size_t j = 0; j < path.size(); ++j) {
            size_t k = (j + 1) % path.size();
            double d = (centers[path[j]].cast<double>() - centers[path[k]].cast<double>()).norm();
            total += d;
            double d2 = d * d;
            if (d2 > mx2) mx2 = d2;
        }

        // Primary: smaller max edge wins.  Secondary: shorter total length.
        if (mx2 < best_max2 || (mx2 == best_max2 && total < best_total)) {
            best_max2 = mx2; best_total = total; best = i;
        }
    }

    return candidates[best].path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_min_max_edge(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    if (print_objects.empty())
        return {};

    // Run all strategies.
    struct Candidate { std::string name; std::vector<const PrintInstance*> path; };
    std::vector<Candidate> candidates;
    candidates.push_back({"Convex Hull Peeling", chain_print_object_instances_convex_hull_peeling(print_objects, start_near)});
    candidates.push_back({"Angle Sort", chain_print_object_instances_angle_sort(print_objects, start_near)});
    candidates.push_back({"Boustrophedon", chain_print_object_instances_boustrophedon(print_objects, start_near)});

    // Pick the one with smallest maximum edge, breaking ties by shortest total length.
    size_t best = 0;
    double best_max = std::numeric_limits<double>::max();
    double best_total = std::numeric_limits<double>::max();
    for (size_t i = 0; i < candidates.size(); ++i) {
        double mx = max_edge_length(candidates[i].path);
        double total = total_path_length(candidates[i].path);
        if (mx < best_max || (mx == best_max && total < best_total)) {
            best_max = mx; best_total = total; best = i;
        }
    }

    return candidates[best].path;
}

std::vector<const PrintInstance*> chain_print_object_instances_min_max_edge(const Print& print)
{
    return chain_print_object_instances_min_max_edge(print.objects().vector(), nullptr);
}

} // namespace Slic3r
