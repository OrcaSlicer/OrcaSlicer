// Print-object ordering strategies: implementation.
// Consolidates TSP post-processing, Snake, and Best-of-Strategies.

#include "OrderingStrategies.hpp"
#include "../Geometry.hpp"
#include "../ShortestPath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

/* ====================================================================
 * TSP post-processing utilities
 * ==================================================================== */

bool tsp_2opt_improve(std::vector<size_t>& path, const Points& centers, int max_passes)
{
    size_t pn = path.size();
    if (pn <= 2) return false;

    // Pre-compute edge lengths once per pass to avoid redundant norm() calls.
    auto recompute_edges = [&]() {
        std::vector<double> el(pn);
        for (size_t i = 0; i < pn; ++i) {
            size_t ni = (i + 1) % pn;
            el[i] = (centers[path[i]].cast<double>() - centers[path[ni]].cast<double>()).norm();
        }
        return el;
    };
    std::vector<double> el = recompute_edges();

    // Pre-compute squared edge lengths for early rejection in the inner loop.
    auto recompute_edges_sq = [&]() {
        std::vector<double> elsq(pn);
        for (size_t i = 0; i < pn; ++i) {
            size_t ni = (i + 1) % pn;
            elsq[i] = (centers[path[i]].cast<double>() - centers[path[ni]].cast<double>()).squaredNorm();
        }
        return elsq;
    };
    std::vector<double> elsq = recompute_edges_sq();

    bool improved = false;
    for (int pass = 0; max_passes <= 0 || pass < max_passes; ++pass) {
        size_t best_i = pn, best_j = pn;
        double best_gain = 0;

        for (size_t i = 0; i < pn; ++i) {
            const Vec2d& pi   = centers[path[i]].cast<double>();
            const Vec2d& p_in = centers[path[(i + 1) % pn]].cast<double>();
            double d_i = el[i];
            double d_i_sq = elsq[i];

            for (size_t j = i + 2; j < pn; ++j) {
                size_t j_next = (j + 1) % pn;
                // Skip the swap that would reverse the entire cycle (removes both
                // edges (0,1) and (pn-1,0), equivalent to traversing the cycle backwards).
                if (i == 0 && j_next == 0) continue;

                const Vec2d& pj   = centers[path[j]].cast<double>();
                const Vec2d& p_jn = centers[path[j_next]].cast<double>();
                double d_j = el[j];

                // Early rejection using squared distances (avoids 2 sqrt calls).
                double new_a_sq = (pj - pi).squaredNorm();
                double new_b_sq = (p_jn - p_in).squaredNorm();
                if (new_a_sq >= d_i_sq && new_b_sq >= elsq[j]) continue;

                double new_a = std::sqrt(new_a_sq);
                double new_b = std::sqrt(new_b_sq);
                double gain = d_i + d_j - new_a - new_b;

                if (gain > best_gain) {
                    best_gain = gain;
                    best_i = i; best_j = j;
                }
            }
        }

        if (best_i == pn) break;
        improved = true;
        // Reverse the best swap segment
        std::reverse(path.begin() + best_i + 1, path.begin() + best_j + 1);

        // Recompute edge lengths after reversal
        el = recompute_edges();
        elsq = recompute_edges_sq();
    }
    return improved;
}

// Fast bounding-box overlap test (rejects most non-intersecting pairs).
static inline bool bboxes_overlap(const Point& a, const Point& b, const Point& c, const Point& d)
{
    return !(std::max(a.x(), b.x()) < std::min(c.x(), d.x()) ||
             std::max(c.x(), d.x()) < std::min(a.x(), b.x()) ||
             std::max(a.y(), b.y()) < std::min(c.y(), d.y()) ||
             std::max(c.y(), d.y()) < std::min(a.y(), b.y()));
}

bool tsp_remove_crossings(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn <= 3) return false;

    // Treat path as a cycle: include the closing edge (pn-1 -> 0), consistent with the other
    // TSP helpers (2-opt, closing-edge rotation) that operate on the full cycle.
    size_t n_edges = pn;

    // Scan for first crossing; returns {i, j} or {npos, npos} if none.
    auto find_crossing = [&]() -> std::pair<size_t, size_t> {
        for (size_t i = 0; i < n_edges; ++i) {
            const Point& ai = centers[path[i]];
            const Point& bi = centers[path[(i + 1) % pn]];

            for (size_t j = i + 2; j < n_edges; ++j) {
                // Skip the (0, pn-1) pair: edges (0,1) and (pn-1,0) share node 0.
                if (i == 0 && j == pn - 1) continue;

                const Point& aj = centers[path[j]];
                const Point& bj = centers[path[(j + 1) % pn]];

                if (!bboxes_overlap(ai, bi, aj, bj)) continue;
                if (Geometry::segments_intersect(ai, bi, aj, bj))
                    return {i, j};
            }
        }
        return {std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
    };

    // Process crossings one at a time: find first, reverse it, restart scan.
    // Cap iterations to prevent infinite loops on collinear/overlapping segments.
    int max_iters = static_cast<int>(pn * pn);
    bool improved = false;
    while (max_iters-- > 0) {
        auto [ci, cj] = find_crossing();
        if (ci == std::numeric_limits<size_t>::max()) break;
        improved = true;
        std::reverse(path.begin() + ci + 1, path.begin() + cj + 1);
    }
    return improved;
}

void tsp_rotate_minimize_closing(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    size_t best_start = 0;
    double best_closing2 = std::numeric_limits<double>::max();
    for (size_t start = 0; start < pn; ++start) {
        size_t last = (start + pn - 1) % pn;
        double d2 = (centers[path[start]].cast<double>() - centers[path[last]].cast<double>()).squaredNorm();
        if (d2 < best_closing2) { best_closing2 = d2; best_start = start; }
    }
    std::rotate(path.begin(), path.begin() + best_start, path.end());
}

/* ====================================================================
 * Snake ordering
 * ==================================================================== */

struct SnakeRow { double avg_y; std::vector<size_t> indices; };

// --- Row threshold computation ---
// Extract unique Y values and use the median gap between them to determine
// the row threshold.
static double compute_row_threshold(const std::vector<double>& sorted_ys,
                                    double y_min, double y_max,
                                    size_t n,
                                    double fraction_of_y_range,
                                    double min_threshold_um)
{
    constexpr double MIN_GAP_FILTER = 1.0;  // ignore sub-micron gaps (coord_t = 1/100mm)

    // Extract unique Y values
    std::vector<double> unique_ys;
    unique_ys.reserve(sorted_ys.size());
    unique_ys.push_back(sorted_ys[0]);
    for (size_t i = 1; i < sorted_ys.size(); ++i) {
        if (sorted_ys[i] - sorted_ys[i - 1] > MIN_GAP_FILTER)
            unique_ys.push_back(sorted_ys[i]);
    }

    double fallback_threshold = (y_max - y_min) * fraction_of_y_range;
    if (unique_ys.size() <= 1) {
        return std::max(fallback_threshold, min_threshold_um);
    }

    // Compute gaps between consecutive unique Y values
    std::vector<double> gaps;
    gaps.reserve(unique_ys.size() - 1);
    for (size_t i = 1; i < unique_ys.size(); ++i)
        gaps.push_back(unique_ys[i] - unique_ys[i - 1]);

    if (gaps.empty()) {
        return std::max(fallback_threshold, min_threshold_um);
    }

    // Sort gaps to find the median
    std::sort(gaps.begin(), gaps.end());
    double median_gap = gaps[gaps.size() / 2];
    double min_gap = gaps.front();

    // Threshold: half the gap between consecutive unique Y values.
    double threshold = (median_gap < min_gap * 1.5) ? min_gap * 0.5 : median_gap * 0.5;

    bool has_row_structure;
    if (unique_ys.size() * 2 <= n) {
        has_row_structure = true;
    } else {
        // Single-column or sparse: uniform gaps indicate a deliberate grid
        double max_gap = *std::max_element(gaps.begin(), gaps.end());
        has_row_structure = (max_gap < min_gap * 2.0);
    }

    if (has_row_structure) {
        // For grid-like data, use the gap-based threshold directly.
        return threshold;
    }

    return std::max(fallback_threshold, min_threshold_um);
}

// --- Row grouping ---
// Bin points into rows by quantising Y / threshold
static std::vector<SnakeRow> group_into_rows(const Points& centers, double row_threshold)
{
    size_t n = centers.size();
    std::unordered_map<int64_t, std::vector<size_t>> row_map;
    for (size_t i = 0; i < n; ++i) {
        int64_t y_key = static_cast<int64_t>(std::floor(static_cast<double>(centers[i].y()) / row_threshold));
        row_map[y_key].push_back(i);
    }

    std::vector<SnakeRow> rows;
    rows.reserve(row_map.size());
    for (auto& [key, indices] : row_map) {
        double avg_y = std::accumulate(indices.begin(), indices.end(), 0.0,
            [&](double acc, size_t idx) { return acc + static_cast<double>(centers[idx].y()); })
            / indices.size();
        rows.push_back({avg_y, std::move(indices)});
    }

    std::sort(rows.begin(), rows.end(),
              [](const SnakeRow& a, const SnakeRow& b) { return a.avg_y < b.avg_y; });

    return rows;
}

// Sort each row by X and greedily pick the direction (left->right or right->left)
// that minimises the transition distance from the previous row's endpoint.
static std::vector<size_t> build_serpentine_path(const Points& centers,
                                                  std::vector<SnakeRow>& rows)
{
    std::vector<size_t> path;
    path.reserve(centers.size());

    for (size_t ri = 0; ri < rows.size(); ++ri) {
        auto& row = rows[ri].indices;
        std::sort(row.begin(), row.end(),
                  [&](size_t a, size_t b) { return centers[a].x() < centers[b].x(); });

        if (ri == 0) {
            path.insert(path.end(), row.begin(), row.end());
        } else {
            const Point& prev_end = centers[path.back()];
            double dist_to_left  = (prev_end.cast<double>() - centers[row.front()].cast<double>()).squaredNorm();
            double dist_to_right = (prev_end.cast<double>() - centers[row.back()].cast<double>()).squaredNorm();

            if (dist_to_left <= dist_to_right)
                path.insert(path.end(), row.begin(), row.end());
            else
                path.insert(path.end(), row.rbegin(), row.rend());
        }
    }

    return path;
}

// Row-based serpentine traversal: detect rows, bin points, snake through them.
static std::vector<size_t> row_serpentine_path(const Points& centers,
                                                double fraction_of_y_range = 0.02,
                                                double min_threshold_um = 1e4)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Collect and sort Y coordinates.
    std::vector<double> sorted_ys;
    sorted_ys.reserve(n);
    for (const auto& p : centers) sorted_ys.push_back(static_cast<double>(p.y()));
    std::sort(sorted_ys.begin(), sorted_ys.end());

    auto [ymin, ymax] = std::minmax_element(sorted_ys.begin(), sorted_ys.end());
    double y_min = *ymin, y_max = *ymax;

    double row_threshold = compute_row_threshold(sorted_ys, y_min, y_max, n,
                                                  fraction_of_y_range, min_threshold_um);

    auto rows = group_into_rows(centers, row_threshold);
    return build_serpentine_path(centers, rows);
}

std::vector<size_t> snake_core(const Points& centers)
{
    if (centers.empty()) return {};

    std::vector<size_t> path = row_serpentine_path(centers);

    for (int iter = 0; iter < 3; ++iter) {
        bool improved = tsp_2opt_improve(path, centers);
        improved |= tsp_remove_crossings(path, centers);
        if (!improved) break;
    }

    return path;
}

std::vector<const PrintInstance*> chain_print_object_instances_snake(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, snake_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_snake(const Print& print)
{
    return chain_print_object_instances_snake(print.objects().vector(), nullptr);
}

/* ====================================================================
 * Best-of-strategies meta-strategy
 * ==================================================================== */

std::vector<const PrintInstance*> chain_print_object_instances_best_of(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    if (print_objects.empty())
        return {};

    // Run all strategies.
    std::vector<std::vector<const PrintInstance*>> candidates;
    candidates.push_back(chain_print_object_instances(print_objects, start_near));
    candidates.push_back(chain_print_object_instances_snake(print_objects, start_near));

    // Compute metrics for each candidate.
    struct Candidate { double total_len; double max_edge; };
    std::vector<Candidate> metrics;
    metrics.reserve(candidates.size());

    for (size_t i = 0; i < candidates.size(); ++i) {
        double total = 0.0;
        double mx = 0.0;
        for (size_t j = 0; j < candidates[i].size(); ++j) {
            size_t k = (j + 1) % candidates[i].size();
            double d = (candidates[i][j]->shift.cast<double>() - candidates[i][k]->shift.cast<double>()).norm();
            total += d;
            if (d > mx) mx = d;
        }
        metrics.push_back({total, mx});
    }

    // Pick shortest total path; tiebreak on smallest max edge.
    auto best_it = std::min_element(metrics.begin(), metrics.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.total_len < b.total_len ||
                   (a.total_len == b.total_len && a.max_edge < b.max_edge);
        });
    size_t best = static_cast<size_t>(std::distance(metrics.begin(), best_it));

    return candidates[best];
}

std::vector<const PrintInstance*> chain_print_object_instances_best_of(const Print& print)
{
    return chain_print_object_instances_best_of(print.objects().vector(), nullptr);
}

/* ====================================================================
 * Island-level ordering entry point
 * ==================================================================== */

std::vector<size_t> order_points_with_strategy(const Points& points, PrintOrder print_order, const Point* start_near)
{
    if (points.empty())
        return {};

    if (print_order != PrintOrder::Snake && print_order != PrintOrder::BestOfStrategies)
        // Nearest neighbor + post-processing; honours start_near natively.
        return chain_points_with_postprocessing(points, start_near);

    auto run_snake = [&points, start_near]() {
        std::vector<size_t> path = snake_core(points);
        if (start_near != nullptr && !path.empty()) {
            // Start the cycle at the point closest to start_near.
            size_t best_start = 0;
            double best_d2 = std::numeric_limits<double>::max();
            for (size_t k = 0; k < points.size(); ++k) {
                double d2 = (points[k].cast<double>() - start_near->cast<double>()).squaredNorm();
                if (d2 < best_d2) { best_d2 = d2; best_start = k; }
            }
            auto it = std::find(path.begin(), path.end(), best_start);
            if (it != path.begin() && it != path.end())
                std::rotate(path.begin(), it, path.end());
        } else {
            tsp_rotate_minimize_closing(path, points);
        }
        return path;
    };

    if (print_order == PrintOrder::Snake)
        return run_snake();

    // Best-of: pick the shortest total cycle; tiebreak on smallest max edge.
    std::vector<std::vector<size_t>> candidates;
    candidates.emplace_back(chain_points_with_postprocessing(points, start_near));
    candidates.emplace_back(run_snake());

    size_t best      = 0;
    double best_len  = std::numeric_limits<double>::max();
    double best_edge = std::numeric_limits<double>::max();
    for (size_t i = 0; i < candidates.size(); ++i) {
        double len  = tsp_cycle_path_length(candidates[i], points);
        double edge = tsp_max_edge_length(candidates[i], points);
        if (len < best_len || (len == best_len && edge < best_edge)) {
            best_len = len; best_edge = edge; best = i;
        }
    }
    return candidates[best];
}

} // namespace Slic3r
