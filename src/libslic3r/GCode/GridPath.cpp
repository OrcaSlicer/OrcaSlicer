// GridPath: row-based serpentine ordering with L1 (Manhattan) distance optimization.
// Avoids diagonal moves by using Manhattan distance for all decisions.

#include "GridPath.hpp"
#include "TSPPostProcessing.hpp"
#include "../Geometry.hpp"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Manhattan distance between two points.
static double manhattan_dist(const Point& a, const Point& b)
{
    return std::abs(static_cast<double>(a.x() - b.x())) +
           std::abs(static_cast<double>(a.y() - b.y()));
}

// L1-aware 2-opt: only accept swaps that improve Manhattan distance.
// Unlike Euclidean 2-opt, this preserves grid-aligned paths because diagonal
// shortcuts never improve L1 distance (triangle inequality is equality for axis-aligned moves).
static void tsp_2opt_l1_improve(std::vector<size_t>& path, const Points& centers, int max_passes = 10)
{
    size_t pn = path.size();
    if (pn < 4) return;

    bool improved = true;
    while (improved && --max_passes > 0) {
        improved = false;
        for (size_t i = 0; i < pn; ++i) {
            size_t i_next = (i + 1) % pn;
            for (size_t j = i + 2; j < pn; ++j) {
                if (j == i_next) continue;
                if (j == (pn - 1) && i == 0) continue;

                size_t j_next = (j + 1) % pn;

                // Current edges: (i→i+1) and (j→j+1)
                double current_dist = manhattan_dist(centers[path[i]], centers[path[i_next]]) +
                                      manhattan_dist(centers[path[j]], centers[path[j_next]]);

                // Swapped edges: (i→j) and (i+1→j+1)
                double new_dist = manhattan_dist(centers[path[i]], centers[path[j]]) +
                                  manhattan_dist(centers[path[i_next]], centers[path[j_next]]);

                if (new_dist < current_dist) {
                    // Reverse segment [i+1, j]
                    size_t a = i_next, b = j;
                    while (a < b) { std::swap(path[a], path[b]); ++a; --b; }
                    improved = true;
                    break;
                }
            }
            if (improved) break;
        }
    }
}

// Check if two line segments intersect (proper crossing, not touching at endpoints).
// Uses Geometry::segments_intersect() with shared-endpoint exclusion.
static bool segments_cross(const Point& a, const Point& b, const Point& c, const Point& d)
{
    // Reject shared endpoints — adjacent edges always share one vertex.
    if (a == c || a == d || b == c || b == d) return false;

    // Reuse the project's segment intersection test.
    return Geometry::segments_intersect(a, b, c, d);
}

// Crossing removal: reverse any segment pair whose edges geometrically cross.
// Always accepts the reversal (unconditional) to guarantee no self-intersections,
// even if it worsens Manhattan distance on non-grid layouts.
static void tsp_remove_crossings_l1(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn < 4) return;

    int max_iterations = static_cast<int>(pn * pn); // Hard cap to prevent infinite loops.
    while (--max_iterations > 0) {
        bool improved = false;
        for (size_t i = 0; i < pn && !improved; ++i) {
            size_t i_next = (i + 1) % pn;
            const Point& pi     = centers[path[i]];
            const Point& pi_nxt = centers[path[i_next]];

            for (size_t j = i + 2; j < pn && !improved; ++j) {
                if (j == i_next) continue;
                if (j == (pn - 1) && i == 0) continue;

                size_t j_next = (j + 1) % pn;
                const Point& pj     = centers[path[j]];
                const Point& pj_nxt = centers[path[j_next]];

                // Check if edges (i→i+1) and (j→j+1) cross.
                if (!segments_cross(pi, pi_nxt, pj, pj_nxt)) continue;

                // Always reverse to eliminate crossing.
                size_t a = i_next, b = j;
                while (a < b) { std::swap(path[a], path[b]); ++a; --b; }
                improved = true;
            }
        }
        if (!improved) break;
    }
}

// Rotate the cycle so the closing edge is minimized (using Manhattan distance).
static void tsp_rotate_minimize_closing_l1(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn < 3) return;

    size_t best_k = 0;
    double best_d = std::numeric_limits<double>::max();
    for (size_t k = 1; k < pn; ++k) {
        double d = manhattan_dist(centers[path[k - 1]], centers[path[k]]);
        if (d < best_d) { best_d = d; best_k = k; }
    }

    // Rotate so the shortest edge becomes the closing edge (last → first).
    std::rotate(path.begin(), path.begin() + best_k, path.end());
}

// Core algorithm: grid path on a raw point set.
std::vector<size_t> grid_path_core(const Points& centers)
{
    if (centers.empty()) return {};

    // Row-based serpentine traversal.
    std::vector<size_t> path = row_serpentine_path(centers);

    // L1-aware 2-opt: only accepts swaps that improve Manhattan distance.
    tsp_2opt_l1_improve(path, centers);

    // Rotate to minimize closing edge (Manhattan distance).
    tsp_rotate_minimize_closing_l1(path, centers);

    // Remove geometric crossings (unconditional reversal). Must run last,
    // since both 2-opt and rotation can create new crossings.
    tsp_remove_crossings_l1(path, centers);

    return path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_grid_path(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, grid_path_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_grid_path(const Print& print)
{
    return chain_print_object_instances_grid_path(print.objects().vector(), nullptr);
}

} // namespace Slic3r
