// Print-object ordering strategies: implementation.
// Consolidates TSP post-processing, Snake, and Best-of-Strategies.

#include "OrderingStrategies.hpp"
#include "../Geometry.hpp"
#include "../ShortestPath.hpp"

#include <algorithm>
#include <limits>
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

    bool improved = false;
    for (int pass = 0; max_passes <= 0 || pass < max_passes; ++pass) {
        size_t best_i = pn, best_j = pn;
        double best_gain = 0;

        for (size_t i = 0; i < pn; ++i) {
            const Vec2d& pi   = centers[path[i]].cast<double>();
            const Vec2d& p_in = centers[path[(i + 1) % pn]].cast<double>();
            double d_i = (p_in - pi).norm();

            for (size_t j = i + 2; j < pn; ++j) {
                size_t j_next = (j + 1) % pn;
                // Skip the swap that would reverse the entire cycle (removes both
                // edges (0,1) and (pn-1,0), equivalent to traversing the cycle backwards).
                if (i == 0 && j_next == 0) continue;

                const Vec2d& pj   = centers[path[j]].cast<double>();
                const Vec2d& p_jn = centers[path[j_next]].cast<double>();
                double d_j = (p_jn - pj).norm();

                double new_a = (pj - pi).norm();
                double new_b = (p_jn - p_in).norm();
                double gain = d_i + d_j - new_a - new_b;

                if (gain > best_gain) {
                    best_gain = gain;
                    best_i = i; best_j = j;
                }
            }
        }

        if (best_i == pn) break;
        improved = true;
        size_t a = best_i + 1, b = best_j;
        while (a < b) std::swap(path[a++], path[b--]);
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

    // Open path: edges 0..pn-2. The closing edge (pn-1->0) is a travel move
    // and is not checked for crossings (consistent with how slicing treats it).
    // Note: tsp_2opt_improve treats the path as a cycle and CAN improve the
    // closing edge. This is intentional — 2-opt can shorten the travel move,
    // but we don't care if the travel move crosses other edges.
    size_t n_edges = pn - 1;

    // Scan for first crossing; returns {i, j} or {npos, npos} if none.
    auto find_crossing = [&]() -> std::pair<size_t, size_t> {
        for (size_t i = 0; i < n_edges; ++i) {
            const Point& ai = centers[path[i]];
            const Point& bi = centers[path[i + 1]];

            for (size_t j = i + 2; j < n_edges; ++j) {
                const Point& aj = centers[path[j]];
                const Point& bj = centers[path[j + 1]];

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
        size_t a = ci + 1, b = cj;
        while (a < b) std::swap(path[a++], path[b--]);
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

// Group points into rows by Y coordinate and traverse them in alternating direction.
static std::vector<size_t> row_serpentine_path(const Points& centers, double fraction_of_y_range = 0.02, double min_threshold_um = 1e4)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Row detection parameters.
    constexpr double MIN_GAP_FILTER = 1.0;        // ignore sub-micron gaps (coord_t = 1/100mm)
    constexpr double GAP_THRESHOLD_RATIO = 0.5;   // threshold = half the min gap
    constexpr double MAX_ROW_FRACTION = 0.3;      // at most 30% of points may be separate rows
    constexpr double ROW_GAP_MULTIPLIER = 2.0;    // min gap must be < 2× average gap

    // Compute row threshold from the minimum gap between distinct Y values.
    // This adapts to variable row spacing instead of using a fixed fraction
    // of the total Y range (which breaks when rows have different spacing).
    std::vector<double> ys;
    ys.reserve(n);
    double y_min = std::numeric_limits<double>::max();
    double y_max = -std::numeric_limits<double>::max();
    for (const auto& p : centers) {
        double y = static_cast<double>(p.y());
        ys.push_back(y);
        if (y < y_min) y_min = y;
        if (y > y_max) y_max = y;
    }
    std::sort(ys.begin(), ys.end());

    double min_gap = std::numeric_limits<double>::max();
    for (size_t i = 1; i < ys.size(); ++i) {
        double gap = ys[i] - ys[i - 1];
        if (gap > MIN_GAP_FILTER && gap < min_gap) min_gap = gap;
    }

    // Use min_gap-based threshold only if it produces a reasonable number of rows.
    // Estimate row count as range / threshold. If too many rows (> n * MAX_ROW_FRACTION),
    // fall back to fraction-based threshold to avoid splitting random noise into separate rows.
    // Also require min_gap to be significantly smaller than the average gap to
    // confirm a true row structure exists.
    double row_threshold;
    if (min_gap < std::numeric_limits<double>::max() && ys.size() > 1) {
        double est_rows = (y_max - y_min) / (min_gap * GAP_THRESHOLD_RATIO);
        double avg_gap = (y_max - y_min) / (ys.size() - 1);
        bool has_row_structure = est_rows <= n * MAX_ROW_FRACTION &&
                                 min_gap < avg_gap * ROW_GAP_MULTIPLIER;
        if (has_row_structure)
            row_threshold = min_gap * GAP_THRESHOLD_RATIO;
        else
            row_threshold = (y_max - y_min) * fraction_of_y_range;
    } else {
        row_threshold = (y_max - y_min) * fraction_of_y_range;
    }
    if (row_threshold < min_threshold_um) row_threshold = min_threshold_um;

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

    // Sort each row by X coordinate, then traverse greedily choosing the
    // direction that minimizes the transition to the next row.
    std::vector<size_t> path;
    path.reserve(n);

    for (size_t ri = 0; ri < rows.size(); ++ri) {
        auto& row = rows[ri].indices;
        std::sort(row.begin(), row.end(),
            [&](size_t a, size_t b) { return centers[a].x() < centers[b].x(); });

        if (ri == 0) {
            // First row: always left→right.
            path.insert(path.end(), row.begin(), row.end());
        } else {
            // Previous row ended at either its leftmost or rightmost point.
            // Choose the direction for this row that minimizes the transition.
            const Point& prev_end = centers[path.back()];
            const Point& row_left = centers[row.front()];
            const Point& row_right = centers[row.back()];
            double dist_to_left = (prev_end.cast<double>() - row_left.cast<double>()).squaredNorm();
            double dist_to_right = (prev_end.cast<double>() - row_right.cast<double>()).squaredNorm();

            if (dist_to_left <= dist_to_right) {
                // Transition to left end → traverse left→right.
                path.insert(path.end(), row.begin(), row.end());
            } else {
                // Transition to right end → traverse right→left.
                path.insert(path.end(), row.rbegin(), row.rend());
            }
        }
    }

    return path;
}

std::vector<size_t> snake_core(const Points& centers)
{
    if (centers.empty()) return {};

    // Row-based serpentine traversal with greedy inter-row direction choice.
    std::vector<size_t> path = row_serpentine_path(centers);

    // Post-processing: alternate 2-opt and crossing removal to eliminate
    // long inter-row jumps and self-crossings. Break early if neither improves.
    for (int iter = 0; iter < 3; ++iter) {
        bool improved = tsp_2opt_improve(path, centers);
        improved |= tsp_remove_crossings(path, centers);
        if (!improved) break;
    }
    tsp_rotate_minimize_closing(path, centers);

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
    size_t best = 0;
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (metrics[i].total_len < metrics[best].total_len ||
            (metrics[i].total_len == metrics[best].total_len && metrics[i].max_edge < metrics[best].max_edge)) {
            best = i;
        }
    }

    return candidates[best];
}

std::vector<const PrintInstance*> chain_print_object_instances_best_of(const Print& print)
{
    return chain_print_object_instances_best_of(print.objects().vector(), nullptr);
}

} // namespace Slic3r
