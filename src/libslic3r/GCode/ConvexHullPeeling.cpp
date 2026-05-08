// Convex hull peeling (onion peeling) with concave hulls:
// Repeatedly extract a concave hull of remaining points (tighter than convex).
// Visit each layer (outermost first) in perimeter order, connecting layers at closest points.
// Rotate the final path so the closing edge (last → first) is short.

#include "ConvexHullPeeling.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"
#include "../Geometry/ConvexHull.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    if (instance_centers.empty())
        return {};

    size_t n = instance_centers.size();

    // Small epsilon for matching hull vertices back to original points (in squared microns).
    const double match_eps2 = 100.0; // 10 microns tolerance

    // Helper: compute median nearest-neighbor distance of a point set (in microns).
    auto compute_median_nnd = [&](const Points& pts) -> double {
        if (pts.size() < 2) return 1000.0;
        std::vector<double> dists;
        dists.reserve(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            double min_d2 = std::numeric_limits<double>::max();
            for (size_t j = 0; j < pts.size(); ++j) {
                if (i == j) continue;
                double d2 = (pts[i].cast<double>() - pts[j].cast<double>()).squaredNorm();
                if (d2 < min_d2) min_d2 = d2;
            }
            dists.push_back(std::sqrt(min_d2));
        }
        std::sort(dists.begin(), dists.end());
        return dists[dists.size() / 2];
    };

    // Helper: compute concave hull of `remaining` points.
    // Starts from convex hull, then greedily inserts interior points for long edges.
    // Returns indices into `remaining` in perimeter order.
    auto concave_hull_indices = [&](const Points& remaining, double max_edge_len) -> std::vector<size_t> {
        // 1. Compute convex hull.
        Polygon hull = Geometry::convex_hull(remaining);

        // 2. Match hull vertices to remaining indices.
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
                // Grab duplicates at same location.
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

        // 3. Greedy insertion: for each edge longer than max_edge_len,
        //    insert the closest unmatched interior point.
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t ei = 0; ei < hull_idx.size(); ++ei) {
                size_t ej = (ei + 1) % hull_idx.size();
                double edge_len = (remaining[hull_idx[ei]].cast<double>() - remaining[hull_idx[ej]].cast<double>()).norm();
                if (edge_len > max_edge_len) {
                    // Find closest unmatched point to this edge (project onto segment).
                    size_t best_k = std::numeric_limits<size_t>::max();
                    double best_d2 = std::numeric_limits<double>::max();
                    const Vec2d& ea = remaining[hull_idx[ei]].cast<double>();
                    const Vec2d& eb = remaining[hull_idx[ej]].cast<double>();
                    Vec2d edge_vec = eb - ea;
                    double edge_len2 = edge_vec.squaredNorm();
                    for (size_t k = 0; k < remaining.size(); ++k) {
                        if (matched[k]) continue;
                        Vec2d pk = remaining[k].cast<double>();
                        // Project pk onto edge, clamp to [0,1].
                        double t = std::max(0.0, std::min(1.0, (pk - ea).dot(edge_vec) / edge_len2));
                        Vec2d proj = ea + t * edge_vec;
                        double d2 = (pk - proj).squaredNorm();
                        if (d2 < best_d2) { best_d2 = d2; best_k = k; }
                    }
                    if (best_k != std::numeric_limits<size_t>::max() && best_d2 < max_edge_len * max_edge_len) {
                        matched[best_k] = true;
                        // Insert after ei (before ej in the cycle).
                        hull_idx.insert(hull_idx.begin() + ((ej > ei) ? ej : ej + 1), best_k);
                        changed = true;
                        break; // restart scan after insertion
                    }
                }
            }
        }

        return hull_idx;
    };

    // Onion peeling: repeatedly compute concave hull of remaining points.
    // Each hull layer stores the indices into the original instance list, in perimeter order.
    std::vector<bool> removed(n, false);
    std::vector<std::vector<size_t>> layers;

    while (true) {
        // Collect remaining points.
        Points remaining;
        std::vector<size_t> remaining_map; // remaining[k] -> original index
        for (size_t i = 0; i < n; ++i) {
            if (!removed[i]) {
                remaining.emplace_back(instance_centers[i]);
                remaining_map.emplace_back(i);
            }
        }

        if (remaining.empty()) break;

        // Small remaining set: just add all as a single layer and finish.
        if (remaining.size() <= 3) {
            layers.emplace_back();
            for (size_t k = 0; k < remaining.size(); ++k)
                layers.back().emplace_back(remaining_map[k]);
            break;
        }

        // Compute median nearest-neighbor distance for this set.
        double median_nnd = compute_median_nnd(remaining);
        // Max hull edge length: 2x median NND creates tighter layers.
        double max_edge_len = 2.0 * median_nnd;

        // Compute concave hull of remaining points.
        std::vector<size_t> hull_local = concave_hull_indices(remaining, max_edge_len);

        if (hull_local.empty()) break;

        // Map local indices back to original indices.
        layers.emplace_back();
        for (size_t li : hull_local)
            layers.back().emplace_back(remaining_map[li]);
        for (size_t li : hull_local)
            removed[remaining_map[li]] = true;

        // Safety: if no points were removed, break.
        if (layers.back().empty()) break;
    }

    // 2-opt improvement within each layer to remove crossings and shorten edges.
    // We optimize per-layer (not the full cycle) so the path stays on each layer
    // longer before transitioning to the next one.
    for (auto& layer : layers) {
        size_t ln = layer.size();
        if (ln < 4) continue; // too small to benefit

        bool improved = true;
        while (improved) {
            improved = false;
            for (size_t i = 0; i < ln; ++i) {
                size_t i_next = (i + 1) % ln;
                for (size_t j = i + 2; j < ln; ++j) {
                    if (j == i_next) continue;
                    if (j == (ln - 1) && i == 0) continue;

                    size_t j_next = (j + 1) % ln;
                    double current_dist = (instance_centers[layer[i_next]].cast<double>() - instance_centers[layer[i]].cast<double>()).norm()
                                       + (instance_centers[layer[j_next]].cast<double>() - instance_centers[layer[j]].cast<double>()).norm();
                    double new_dist     = (instance_centers[layer[j]].cast<double>() - instance_centers[layer[i]].cast<double>()).norm()
                                       + (instance_centers[layer[j_next]].cast<double>() - instance_centers[layer[i_next]].cast<double>()).norm();

                    if (new_dist < current_dist) {
                        while (i_next < j) {
                            std::swap(layer[i_next], layer[j]);
                            ++i_next;
                            --j;
                        }
                        improved = true;
                        break;
                    }
                }
                if (improved) break;
            }
        }
    }

    // Build the final path by traversing each (optimized) layer in order.
    // Connect consecutive layers at their closest points.
    // For middle layers, try both traversal directions and pick the one whose
    // exit point is closer to the next layer's best entry.
    // For the last layer, pick the direction whose exit is closer to the
    // first layer's entry (to keep the closing edge short).
    std::vector<size_t> path;
    path.reserve(n);

    for (size_t layer = 0; layer < layers.size(); ++layer) {
        const auto& cur = layers[layer];

        if (layer == 0) {
            // First layer: choose starting point closest to start_near (or index 0).
            size_t start_idx = 0;
            if (start_near != nullptr) {
                double min_dist2 = std::numeric_limits<double>::max();
                for (size_t k = 0; k < cur.size(); ++k) {
                    double d2 = (instance_centers[cur[k]].cast<double>() - start_near->cast<double>()).squaredNorm();
                    if (d2 < min_dist2) { min_dist2 = d2; start_idx = k; }
                }
            }

            // Rotate so start_idx is first.
            for (size_t k = start_idx; k < cur.size(); ++k) path.emplace_back(cur[k]);
            for (size_t k = 0; k < start_idx; ++k) path.emplace_back(cur[k]);
        } else {
            // Connect to previous layer: find the point in cur closest to the last point added.
            size_t last = path.back();
            size_t best_k = 0;
            double best_d2 = std::numeric_limits<double>::max();
            for (size_t k = 0; k < cur.size(); ++k) {
                double d2 = (instance_centers[cur[k]].cast<double>() - instance_centers[last].cast<double>()).squaredNorm();
                if (d2 < best_d2) { best_d2 = d2; best_k = k; }
            }

            // Decide traversal direction based on exit-point proximity.
            // Middle layers: exit should be close to next layer.
            // Last layer: exit should be close to first layer (closing edge).

            if (cur.size() >= 2) {
                // Forward exit: element before best_k in the cycle
                size_t fwd_exit = (best_k + cur.size() - 1) % cur.size();
                // Reverse exit: element after best_k in the cycle
                size_t rev_exit = (best_k + 1) % cur.size();

                // Find closest point in target layer for each exit
                double fwd_best2 = std::numeric_limits<double>::max();
                double rev_best2 = std::numeric_limits<double>::max();

                if (layer + 1 < layers.size()) {
                    const auto& next = layers[layer + 1];
                    for (size_t k = 0; k < next.size(); ++k) {
                        double d2f = (instance_centers[next[k]].cast<double>() - instance_centers[cur[fwd_exit]].cast<double>()).squaredNorm();
                        double d2r = (instance_centers[next[k]].cast<double>() - instance_centers[cur[rev_exit]].cast<double>()).squaredNorm();
                        if (d2f < fwd_best2) fwd_best2 = d2f;
                        if (d2r < rev_best2) rev_best2 = d2r;
                    }
                } else {
                    // Last layer: compare distance to first path element
                    fwd_best2 = (instance_centers[path.front()].cast<double>() - instance_centers[cur[fwd_exit]].cast<double>()).squaredNorm();
                    rev_best2 = (instance_centers[path.front()].cast<double>() - instance_centers[cur[rev_exit]].cast<double>()).squaredNorm();
                }

                if (rev_best2 < fwd_best2) {
                    // Reverse direction
                    for (int k = 0; k < (int)cur.size(); ++k)
                        path.emplace_back(cur[(best_k + cur.size() - k) % cur.size()]);
                } else {
                    // Forward direction
                    for (size_t k = best_k; k < cur.size(); ++k) path.emplace_back(cur[k]);
                    for (size_t k = 0; k < best_k; ++k) path.emplace_back(cur[k]);
                }
            } else {
                for (size_t k = best_k; k < cur.size(); ++k) path.emplace_back(cur[k]);
                for (size_t k = 0; k < best_k; ++k) path.emplace_back(cur[k]);
            }
        }
    }

    // Global 2-opt pass with a max-edge-length constraint.
    // Only allow swaps that reduce total distance AND don't create edges longer
    // than 5× the median nearest-neighbor distance. This smooths layer transitions
    // without destroying the overall onion-peel structure.
    {
        double max_nnd = compute_median_nnd(instance_centers);
        double max_edge2 = 25.0 * max_nnd * max_nnd; // (5× NND)²
        size_t pn = path.size();

        bool improved = true;
        while (improved) {
            improved = false;
            for (size_t i = 0; i < pn; ++i) {
                size_t i_next = (i + 1) % pn;
                for (size_t j = i + 2; j < pn; ++j) {
                    if (j == i_next) continue;
                    if (j == (pn - 1) && i == 0) continue;

                    size_t j_next = (j + 1) % pn;
                    const Vec2d& ci     = instance_centers[path[i]].cast<double>();
                    const Vec2d& ci_next = instance_centers[path[i_next]].cast<double>();
                    const Vec2d& cj     = instance_centers[path[j]].cast<double>();
                    const Vec2d& cj_next = instance_centers[path[j_next]].cast<double>();

                    double current_dist = (ci_next - ci).norm() + (cj_next - cj).norm();
                    double new_dist     = (cj - ci).norm() + (cj_next - ci_next).norm();

                    // Only swap if it shortens the path AND neither new edge exceeds the max.
                    if (new_dist < current_dist
                        && (cj - ci).squaredNorm() < max_edge2
                        && (cj_next - ci_next).squaredNorm() < max_edge2) {
                        while (i_next < j) {
                            std::swap(path[i_next], path[j]);
                            ++i_next;
                            --j;
                        }
                        improved = true;
                        break;
                    }
                }
                if (improved) break;
            }
        }
    }

    // Rotate the full path to minimize the closing edge (last → first).
    // This ensures the cycle is properly closed with a short final jump.
    {
        size_t pn = path.size();
        size_t best_start = 0;
        double best_closing2 = std::numeric_limits<double>::max();
        for (size_t start = 0; start < pn; ++start) {
            size_t last = (start + pn - 1) % pn;
            double d2 = (instance_centers[path[start]].cast<double>() - instance_centers[path[last]].cast<double>()).squaredNorm();
            if (d2 < best_closing2) { best_closing2 = d2; best_start = start; }
        }
        // Rotate path so best_start is first.
        std::rotate(path.begin(), path.begin() + best_start, path.end());
    }

    // Emit the final cycle.
    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step = 0; step < path.size(); ++step) {
        size_t idx = path[step];
        out.emplace_back(&print_objects[instances[idx].first]->instances()[instances[idx].second]);
    }

    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_convex_hull_peeling(const Print& print)
{
    return chain_print_object_instances_convex_hull_peeling(print.objects().vector(), nullptr);
}

} // namespace Slic3r
