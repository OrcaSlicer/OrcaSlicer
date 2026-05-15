// Print-object ordering strategies: implementation.
// Consolidates TSP post-processing, Snake, Convex Hull Peeling, and Best-of-Strategies.

#include "OrderingStrategies.hpp"
#include "../Geometry.hpp"
#include "../Geometry/ConvexHull.hpp"
#include "../KDTreeIndirect.hpp"
#include "../ShortestPath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

/* ====================================================================
 * TSP post-processing utilities
 * ==================================================================== */

void tsp_2opt_improve(std::vector<size_t>& path, const Points& centers, int max_passes)
{
    size_t pn = path.size();
    if (pn <= 2) return;

    // Best-first 2-opt: each pass finds the single best swap across all pairs,
    // then applies it. Converges in far fewer passes than first-improving.
    for (int pass = 0; max_passes <= 0 || pass < max_passes; ++pass) {
        size_t best_i = pn, best_j = pn;
        double best_gain = 0;

        for (size_t i = 0; i < pn; ++i) {
            const Vec2d& pi   = centers[path[i]].cast<double>();
            const Vec2d& p_in = centers[path[(i + 1) % pn]].cast<double>();
            double d_i = (p_in - pi).norm();

            for (size_t j = i + 2; j < pn; ++j) {
                size_t j_next = (j + 1) % pn;
                if ((i == 0 && j_next == 0)) continue; // closing edge pair, skip

                const Vec2d& pj   = centers[path[j]].cast<double>();
                const Vec2d& p_jn = centers[path[j_next]].cast<double>();
                double d_j = (p_jn - pj).norm();

                // Gain from reversing [i+1 .. j]: remove edges (i,i+1) and (j,j+1), add (i,j) and (i+1,j+1)
                double gain = d_i + d_j - (pj - pi).norm() - (p_jn - p_in).norm();
                if (gain > best_gain) {
                    best_gain = gain;
                    best_i = i; best_j = j;
                }
            }
        }

        // Apply the best swap found, or stop if none improved.
        if (best_i == pn) break;
        size_t a = best_i + 1, b = best_j;
        while (a < b) std::swap(path[a++], path[b--]);
    }
}

// Fast bounding-box overlap test (rejects most non-intersecting pairs).
static inline bool bboxes_overlap(const Point& a, const Point& b, const Point& c, const Point& d)
{
    return !(std::max(a.x(), b.x()) < std::min(c.x(), d.x()) ||
             std::max(c.x(), d.x()) < std::min(a.x(), b.x()) ||
             std::max(a.y(), b.y()) < std::min(c.y(), d.y()) ||
             std::max(c.y(), d.y()) < std::min(a.y(), b.y()));
}

void tsp_remove_crossings(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn <= 3) return;

    // Open path: edges 0..pn-2 (no artificial closing edge pn-1→0).
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
    while (max_iters-- > 0) {
        auto [ci, cj] = find_crossing();
        if (ci == std::numeric_limits<size_t>::max()) break;
        size_t a = ci + 1, b = cj;
        while (a < b) std::swap(path[a++], path[b--]);
    }
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
static std::vector<size_t> row_serpentine_path(const Points& centers, double fraction_of_y_range = 0.1, double min_threshold_um = 1e4)
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
    double row_threshold = (y_max - y_min) * fraction_of_y_range;
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

    return path;
}

std::vector<size_t> snake_core(const Points& centers)
{
    if (centers.empty()) return {};

    // Row-based serpentine traversal.
    std::vector<size_t> path = row_serpentine_path(centers);

    // Post-processing: 2-opt, crossing removal, rotation.
    tsp_2opt_improve(path, centers);
    tsp_remove_crossings(path, centers);
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
 * Convex hull peeling (onion peeling) with concave hulls
 * ==================================================================== */

std::vector<size_t> convex_hull_peeling_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    const double match_eps2 = 100.0; // 10 microns tolerance

    // Median nearest-neighbor distance via KD-tree (O(n log n)).
    auto compute_median_nnd = [&](const Points& pts) -> double {
        if (pts.size() < 2) return 1000.0;

        auto coord_fn = [&](size_t idx, size_t dim) -> double {
            const Point& p = pts[idx];
            return (dim == 0) ? static_cast<double>(p.x()) : static_cast<double>(p.y());
        };
        KDTreeIndirect<2, double, decltype(coord_fn)> kdtree(coord_fn, pts.size());

        std::vector<double> dists;
        dists.reserve(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            auto closest = find_closest_points<1>(kdtree,
                Vec2d(static_cast<double>(pts[i].x()), static_cast<double>(pts[i].y())),
                [i](size_t idx) { return idx != i; });

            if (closest[0] != KDTreeIndirect<2, double, decltype(coord_fn)>::npos) {
                Vec2d pi(pts[i].cast<double>());
                Vec2d pj(pts[closest[0]].cast<double>());
                dists.push_back((pi - pj).norm());
            }
        }
        std::sort(dists.begin(), dists.end());
        return dists[dists.size() / 2];
    };

    auto concave_hull_indices = [&](const Points& remaining, double max_edge_len) -> std::vector<size_t> {
        Polygon hull = Geometry::convex_hull(remaining);

        std::vector<size_t> hull_idx;
        std::vector<bool> matched(remaining.size(), false);
        for (const Point& hp : hull) {
            size_t best_k = std::numeric_limits<size_t>::max();
            double best_d2 = match_eps2;
            for (size_t k = 0; k < remaining.size(); ++k) {
                if (matched[k]) continue;
                double d2 = (remaining[k].cast<double>() - hp.cast<double>()).squaredNorm();
                if (d2 < best_d2) { best_d2 = d2; best_k = k; }
            }
            if (best_k != std::numeric_limits<size_t>::max()) {
                hull_idx.push_back(best_k);
                matched[best_k] = true;
                for (size_t k = 0; k < remaining.size(); ++k) {
                    if (matched[k]) continue;
                    if ((remaining[k].cast<double>() - hp.cast<double>()).squaredNorm() < match_eps2) {
                        hull_idx.push_back(k);
                        matched[k] = true;
                    }
                }
            }
        }
        if (hull_idx.empty()) return {};

        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t ei = 0; ei < hull_idx.size(); ++ei) {
                size_t ej = (ei + 1) % hull_idx.size();
                double edge_len = (remaining[hull_idx[ei]].cast<double>() - remaining[hull_idx[ej]].cast<double>()).norm();
                if (edge_len > max_edge_len) {
                    size_t best_k = std::numeric_limits<size_t>::max();
                    double best_d2 = std::numeric_limits<double>::max();
                    const Vec2d& ea = remaining[hull_idx[ei]].cast<double>();
                    const Vec2d& eb = remaining[hull_idx[ej]].cast<double>();
                    Vec2d edge_vec = eb - ea;
                    double edge_len2 = edge_vec.squaredNorm();
                    for (size_t k = 0; k < remaining.size(); ++k) {
                        if (matched[k]) continue;
                        Vec2d pk = remaining[k].cast<double>();
                        double t = std::max(0.0, std::min(1.0, (pk - ea).dot(edge_vec) / edge_len2));
                        Vec2d proj = ea + t * edge_vec;
                        double d2 = (pk - proj).squaredNorm();
                        if (d2 < best_d2) { best_d2 = d2; best_k = k; }
                    }
                    if (best_k != std::numeric_limits<size_t>::max() && best_d2 < max_edge_len * max_edge_len) {
                        matched[best_k] = true;
                        hull_idx.insert(hull_idx.begin() + ((ej > ei) ? ej : ej + 1), best_k);
                        changed = true;
                        break;
                    }
                }
            }
        }

        return hull_idx;
    };

    // Onion peeling.
    std::vector<bool> removed(n, false);
    std::vector<std::vector<size_t>> layers;

    while (true) {
        Points remaining;
        std::vector<size_t> remaining_map;
        for (size_t i = 0; i < n; ++i) {
            if (!removed[i]) {
                remaining.emplace_back(centers[i]);
                remaining_map.emplace_back(i);
            }
        }

        if (remaining.empty()) break;

        if (remaining.size() <= 3) {
            layers.emplace_back();
            for (size_t k = 0; k < remaining.size(); ++k)
                layers.back().emplace_back(remaining_map[k]);
            break;
        }

        double median_nnd = compute_median_nnd(remaining);
        double max_edge_len = 2.0 * median_nnd;

        std::vector<size_t> hull_local = concave_hull_indices(remaining, max_edge_len);
        if (hull_local.empty()) break;

        layers.emplace_back();
        for (size_t li : hull_local)
            layers.back().emplace_back(remaining_map[li]);
        for (size_t li : hull_local)
            removed[remaining_map[li]] = true;

        if (layers.back().empty()) break;
    }

    // Per-layer crossing removal + 2-opt.
    for (auto& layer : layers) {
        if (layer.size() >= 4) {
            tsp_remove_crossings(layer, centers);
            tsp_2opt_improve(layer, centers);
        }
    }

    // Build path by traversing layers.
    std::vector<size_t> path;
    path.reserve(n);

    for (size_t layer = 0; layer < layers.size(); ++layer) {
        const auto& cur = layers[layer];

        if (layer == 0) {
            // First layer: start at index 0.
            for (size_t k = 0; k < cur.size(); ++k) path.emplace_back(cur[k]);
        } else {
            size_t last = path.back();
            size_t best_k = 0;
            double best_d2 = std::numeric_limits<double>::max();
            for (size_t k = 0; k < cur.size(); ++k) {
                double d2 = (centers[cur[k]].cast<double>() - centers[last].cast<double>()).squaredNorm();
                if (d2 < best_d2) { best_d2 = d2; best_k = k; }
            }

            if (cur.size() >= 2) {
                size_t fwd_exit = (best_k + cur.size() - 1) % cur.size();
                size_t rev_exit = (best_k + 1) % cur.size();

                double fwd_best2 = std::numeric_limits<double>::max();
                double rev_best2 = std::numeric_limits<double>::max();

                if (layer + 1 < layers.size()) {
                    const auto& next = layers[layer + 1];
                    for (size_t k = 0; k < next.size(); ++k) {
                        double d2f = (centers[next[k]].cast<double>() - centers[cur[fwd_exit]].cast<double>()).squaredNorm();
                        double d2r = (centers[next[k]].cast<double>() - centers[cur[rev_exit]].cast<double>()).squaredNorm();
                        if (d2f < fwd_best2) fwd_best2 = d2f;
                        if (d2r < rev_best2) rev_best2 = d2r;
                    }
                } else {
                    fwd_best2 = (centers[path.front()].cast<double>() - centers[cur[fwd_exit]].cast<double>()).squaredNorm();
                    rev_best2 = (centers[path.front()].cast<double>() - centers[cur[rev_exit]].cast<double>()).squaredNorm();
                }

                if (rev_best2 < fwd_best2) {
                    for (int k = 0; k < (int)cur.size(); ++k)
                        path.emplace_back(cur[(best_k + cur.size() - k) % cur.size()]);
                } else {
                    for (size_t k = best_k; k < cur.size(); ++k) path.emplace_back(cur[k]);
                    for (size_t k = 0; k < best_k; ++k) path.emplace_back(cur[k]);
                }
            } else {
                for (size_t k = best_k; k < cur.size(); ++k) path.emplace_back(cur[k]);
                for (size_t k = 0; k < best_k; ++k) path.emplace_back(cur[k]);
            }
        }
    }

    // Global 2-opt with max-edge-length constraint.
    {
        double max_nnd = compute_median_nnd(centers);
        double max_edge2 = 25.0 * max_nnd * max_nnd;
        size_t pn = path.size();

        bool improved = true;
        int max_passes = 5;
        while (improved) {
            if (--max_passes == 0) break;
            improved = false;
            for (size_t i = 0; i < pn; ++i) {
                size_t i_next = (i + 1) % pn;
                for (size_t j = i + 2; j < pn; ++j) {
                    if (j == i_next) continue;
                    if (j == (pn - 1) && i == 0) continue;

                    size_t j_next = (j + 1) % pn;
                    const Vec2d& ci     = centers[path[i]].cast<double>();
                    const Vec2d& ci_next = centers[path[i_next]].cast<double>();
                    const Vec2d& cj     = centers[path[j]].cast<double>();
                    const Vec2d& cj_next = centers[path[j_next]].cast<double>();

                    double current_dist = (ci_next - ci).norm() + (cj_next - cj).norm();
                    double new_dist     = (cj - ci).norm() + (cj_next - ci_next).norm();

                    if (new_dist < current_dist
                        && (cj - ci).squaredNorm() < max_edge2
                        && (cj_next - ci_next).squaredNorm() < max_edge2) {
                        while (i_next < j) { std::swap(path[i_next], path[j]); ++i_next; --j; }
                        improved = true;
                        break;
                    }
                }
                if (improved) break;
            }
        }
    }

    // Post-processing: crossing removal, rotation.
    tsp_remove_crossings(path, centers);
    tsp_rotate_minimize_closing(path, centers);

    // Final crossing removal on complete path (inter-layer connections can create crossings).
    tsp_remove_crossings(path, centers);

    return path;
}

std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, convex_hull_peeling_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const Print& print)
{
    return chain_print_object_instances_convex_hull_peeling(print.objects().vector(), nullptr);
}

/* ====================================================================
 * Best-of-strategies meta-strategy
 * ==================================================================== */

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
    candidates.push_back(chain_print_object_instances(print_objects, start_near));
    candidates.push_back(chain_print_object_instances_convex_hull_peeling(print_objects, start_near));
    candidates.push_back(chain_print_object_instances_snake(print_objects, start_near));

    // Compute metrics for each candidate.
    struct Candidate { double total_len; double max_edge; };
    std::vector<Candidate> metrics;
    metrics.reserve(candidates.size());

    for (size_t i = 0; i < candidates.size(); ++i) {
        double total = cycle_path_length(candidates[i]);
        double mx = 0.0;
        for (size_t j = 0; j < candidates[i].size(); ++j) {
            size_t k = (j + 1) % candidates[i].size();
            double d = (candidates[i][j]->shift.cast<double>() - candidates[i][k]->shift.cast<double>()).norm();
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
