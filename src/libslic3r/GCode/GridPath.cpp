// GridPath: row-based serpentine ordering with L1 (Manhattan) distance optimization.
// Avoids diagonal moves by using Manhattan distance for all decisions.

#include "GridPath.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

// Row threshold parameters (in µm).
constexpr double ROW_FRACTION_OF_Y_RANGE = 0.1;   // ~10% of Y range
constexpr double MIN_ROW_THRESHOLD_UM     = 1e4;   // 10 mm floor to avoid over-splitting

// Manhattan distance between two points.
static double manhattan_dist(const Point& a, const Point& b)
{
    return std::abs(static_cast<double>(a.x() - b.x())) +
           std::abs(static_cast<double>(a.y() - b.y()));
}

// Total path length using Manhattan distance (including closing edge).
static double manhattan_path_length(const std::vector<size_t>& path, const Points& centers)
{
    if (path.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t next = (i + 1) % path.size();
        total += manhattan_dist(centers[path[i]], centers[path[next]]);
    }
    return total;
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
static bool segments_cross(const Point& a, const Point& b, const Point& c, const Point& d)
{
    // Reject shared endpoints.
    if (a == c || a == d || b == c || b == d) return false;

    // Cross product of vectors.
    auto cross = [](const Point& p1, const Point& p2) {
        return static_cast<long long>(p1.x()) * static_cast<long long>(p2.y())
             - static_cast<long long>(p1.y()) * static_cast<long long>(p2.x());
    };

    auto ab = Point(b.x() - a.x(), b.y() - a.y());
    auto cd = Point(d.x() - c.x(), d.y() - c.y());
    auto ac = Point(c.x() - a.x(), c.y() - a.y());
    auto ad = Point(d.x() - a.x(), d.y() - a.y());
    auto ca = Point(a.x() - c.x(), a.y() - c.y());
    auto cb = Point(b.x() - c.x(), b.y() - c.y());

    long long cp1 = cross(ab, ac);
    long long cp2 = cross(ab, ad);
    long long cp3 = cross(cd, ca);
    long long cp4 = cross(cd, cb);

    // Proper crossing: endpoints of each segment lie on opposite sides of the other.
    if (((cp1 > 0 && cp2 < 0) || (cp1 < 0 && cp2 > 0)) &&
        ((cp3 > 0 && cp4 < 0) || (cp3 < 0 && cp4 > 0))) {
        return true;
    }

    // Collinear cases: only flag as crossing if segments are on different lines.
    // If all four points are collinear, overlapping edges are unavoidable and not a real crossing.
    bool ab_collinear_with_cd = (cp1 == 0 && cp2 == 0);
    bool cd_collinear_with_ab = (cp3 == 0 && cp4 == 0);
    if (ab_collinear_with_cd || cd_collinear_with_ab) return false;

    // Collinear cases: check if a point lies on the segment.
    auto on_segment = [](const Point& p, const Point& q, const Point& r) {
        return q.x() <= std::max(p.x(), r.x()) && q.x() >= std::min(p.x(), r.x())
            && q.y() <= std::max(p.y(), r.y()) && q.y() >= std::min(p.y(), r.y());
    };

    if (cp1 == 0 && on_segment(a, c, b)) return true;
    if (cp2 == 0 && on_segment(a, d, b)) return true;
    if (cp3 == 0 && on_segment(c, a, d)) return true;
    if (cp4 == 0 && on_segment(c, b, d)) return true;

    return false;
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

    size_t n = centers.size();

    // Compute Y range to determine row grouping threshold.
    double y_min = std::numeric_limits<double>::max();
    double y_max = -std::numeric_limits<double>::max();
    for (const auto& p : centers) {
        double y = static_cast<double>(p.y());
        if (y < y_min) y_min = y;
        if (y > y_max) y_max = y;
    }

    // Row threshold: fraction of Y range, with a minimum floor.
    double row_threshold = (y_max - y_min) * ROW_FRACTION_OF_Y_RANGE;
    if (row_threshold < MIN_ROW_THRESHOLD_UM) row_threshold = MIN_ROW_THRESHOLD_UM;

    // Group into rows using hash-map binning by Y coordinate.
    std::unordered_map<int, std::vector<size_t>> row_map;
    for (size_t i = 0; i < n; ++i) {
        int y_key = static_cast<int>(centers[i].y() / row_threshold);
        row_map[y_key].push_back(i);
    }

    // Convert to vector of rows with precomputed average Y.
    struct Row { double avg_y; std::vector<size_t> indices; };
    std::vector<Row> rows;
    rows.reserve(row_map.size());
    for (auto& [key, indices] : row_map) {
        double sum = 0;
        for (size_t idx : indices) sum += static_cast<double>(centers[idx].y());
        rows.push_back({sum / static_cast<double>(indices.size()), std::move(indices)});
    }

    // Sort rows by average Y.
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.avg_y < b.avg_y;
    });

    // Sort each row by X coordinate, then traverse alternating direction (serpentine).
    std::vector<size_t> path;
    path.reserve(n);
    bool reverse = false;

    for (auto& [avg_y, row] : rows) {
        std::sort(row.begin(), row.end(),
            [&](size_t a, size_t b) { return centers[a].x() < centers[b].x(); });

        if (reverse) {
            path.insert(path.end(), row.rbegin(), row.rend());
        } else {
            path.insert(path.end(), row.begin(), row.end());
        }
        reverse = !reverse;
    }

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

    auto path = grid_path_core(instance_centers);

    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_grid_path(const Print& print)
{
    return chain_print_object_instances_grid_path(print.objects().vector(), nullptr);
}

} // namespace Slic3r
