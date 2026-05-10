// Convex hull peeling (onion peeling) with concave hulls:
// Repeatedly extract a concave hull of remaining points (tighter than convex).
// Visit each layer (outermost first) in perimeter order, connecting layers at closest points.
// Rotate the final path so the closing edge (last → first) is short.

#include "ConvexHullPeeling.hpp"
#include "TSPPostProcessing.hpp"
#include "../Print.hpp"
#include "../KDTreeIndirect.hpp"
#include "../Geometry/ConvexHull.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Core algorithm: convex hull peeling on a raw point set.
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

    // Per-layer 2-opt.
    for (auto& layer : layers) {
        if (layer.size() >= 4)
            tsp_2opt_improve(layer, centers);
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

    return path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, convex_hull_peeling_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const Print& print)
{
    return chain_print_object_instances_convex_hull_peeling(print.objects().vector(), nullptr);
}

} // namespace Slic3r
