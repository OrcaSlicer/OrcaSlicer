// Spiral ordering: sort objects by distance from centroid, then by angle within
// distance bands. Produces concentric/spiral paths. Followed by 2-opt and rotation.

#include "SpiralOrdering.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Core algorithm: spiral ordering on a raw point set.
std::vector<size_t> spiral_order_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Compute centroid.
    Vec2d centroid(0, 0);
    for (const Point& pt : centers)
        centroid += pt.cast<double>();
    centroid /= n;

    // Compute polar coordinates for each point.
    struct PolarPt {
        size_t idx;
        double dist, angle;
    };
    std::vector<PolarPt> polars(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2d v = centers[i].cast<double>() - centroid;
        polars[i] = {i, v.norm(), std::atan2(v.y(), v.x())};
        if (polars[i].angle < 0) polars[i].angle += 2 * M_PI;
    }

    // Sort by distance to identify bands.
    std::vector<size_t> sorted_by_dist(n);
    for (size_t i = 0; i < n; ++i) sorted_by_dist[i] = i;
    std::sort(sorted_by_dist.begin(), sorted_by_dist.end(),
              [&](size_t a, size_t b) { return polars[a].dist < polars[b].dist; });

    // Split into distance bands (10% relative tolerance on distance).
    std::vector<std::vector<size_t>> bands;
    {
        std::vector<size_t> cur_band;
        for (size_t k = 0; k < n; ++k) {
            size_t idx = sorted_by_dist[k];
            if (cur_band.empty()) {
                cur_band.push_back(idx);
            } else {
                double d_cur = polars[idx].dist;
                double d_ref = polars[cur_band[0]].dist;
                double mean_d = (d_cur + d_ref) / 2.0;
                if (mean_d > 1e-9 && std::abs(d_cur - d_ref) / mean_d < 0.1) {
                    cur_band.push_back(idx);
                } else {
                    bands.push_back(std::move(cur_band));
                    cur_band.push_back(idx);
                }
            }
        }
        if (!cur_band.empty()) bands.push_back(std::move(cur_band));
    }

    // Sort each band by angle.
    for (auto& band : bands) {
        std::sort(band.begin(), band.end(),
                  [&](size_t a, size_t b) { return polars[a].angle < polars[b].angle; });
    }

    // Concatenate bands, rotating each to minimize transition edge.
    std::vector<size_t> indices;
    indices.reserve(n);
    for (size_t b = 0; b < bands.size(); ++b) {
        if (b == 0) {
            indices.insert(indices.end(), bands[0].begin(), bands[0].end());
        } else {
            auto& band = bands[b];
            size_t prev_last = indices.back();
            if (band.size() >= 2) {
                size_t best_rot = 0;
                double best_d2 = std::numeric_limits<double>::max();
                for (size_t r = 0; r < band.size(); ++r) {
                    double d2 = (centers[band[r]].cast<double>() - centers[prev_last].cast<double>()).squaredNorm();
                    if (d2 < best_d2) { best_d2 = d2; best_rot = r; }
                }
                std::rotate(band.begin(), band.begin() + best_rot, band.end());
            }
            indices.insert(indices.end(), band.begin(), band.end());
        }
    }

    // Compute median nearest-neighbor distance for edge-length constraint.
    double max_nnd_sq = 0;
    {
        std::vector<double> nnds;
        nnds.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            double best_d2 = std::numeric_limits<double>::max();
            for (size_t j = 0; j < n; ++j) {
                if (i == j) continue;
                double d2 = (centers[i].cast<double>() - centers[j].cast<double>()).squaredNorm();
                if (d2 < best_d2) best_d2 = d2;
            }
            nnds.push_back(best_d2);
        }
        std::sort(nnds.begin(), nnds.end());
        max_nnd_sq = nnds[n / 2]; // median NND squared
    }
    double edge_limit_sq = 9.0 * max_nnd_sq; // 3× median NND threshold (squared)

    // Distance-constrained 2-opt improvement pass.
    {
        size_t pn = indices.size();
        bool improved = true;
        while (improved) {
            improved = false;
            for (size_t i = 0; i < pn; ++i) {
                size_t i_next = (i + 1) % pn;
                for (size_t j = i + 2; j < pn; ++j) {
                    if (j == i_next) continue;
                    if (j == (pn - 1) && i == 0) continue;

                    size_t j_next = (j + 1) % pn;
                    double current_dist = (centers[indices[i_next]].cast<double>() - centers[indices[i]].cast<double>()).norm()
                                       + (centers[indices[j_next]].cast<double>() - centers[indices[j]].cast<double>()).norm();
                    double new_dist     = (centers[indices[j]].cast<double>() - centers[indices[i]].cast<double>()).norm()
                                       + (centers[indices[j_next]].cast<double>() - centers[indices[i_next]].cast<double>()).norm();

                    // Only accept if shorter AND new edges are within threshold.
                    double edge1_sq = ((centers[indices[j]].cast<double>() - centers[indices[i]].cast<double>())).squaredNorm();
                    double edge2_sq = ((centers[indices[j_next]].cast<double>() - centers[indices[i_next]].cast<double>())).squaredNorm();
                    if (new_dist < current_dist && edge1_sq <= edge_limit_sq && edge2_sq <= edge_limit_sq) {
                        while (i_next < j) { std::swap(indices[i_next], indices[j]); ++i_next; --j; }
                        improved = true;
                        break;
                    }
                }
                if (improved) break;
            }
        }
    }

    // Crossing removal pass.
    {
        size_t pn = indices.size();
        int max_iters = static_cast<int>(pn * pn);
        bool had_crossing = true;
        while (had_crossing && max_iters-- > 0) {
            had_crossing = false;
            for (size_t i = 0; i < pn && !had_crossing; ++i) {
                size_t i_next = (i + 1) % pn;
                for (size_t j = i + 2; j < pn && !had_crossing; ++j) {
                    if (j == i_next) continue;
                    if (j == (pn - 1) && i == 0) continue;
                    size_t j_next = (j + 1) % pn;
                    if (Geometry::segments_intersect(
                            centers[indices[i]], centers[indices[i_next]],
                            centers[indices[j]], centers[indices[j_next]])) {
                        while (i_next < j) { std::swap(indices[i_next], indices[j]); ++i_next; --j; }
                        had_crossing = true;
                    }
                }
            }
        }
    }

    // Rotate to minimize the closing edge.
    {
        size_t pn = indices.size();
        size_t best_start = 0;
        double best_closing2 = std::numeric_limits<double>::max();
        for (size_t start = 0; start < pn; ++start) {
            size_t last = (start + pn - 1) % pn;
            double d2 = (centers[indices[start]].cast<double>() - centers[indices[last]].cast<double>()).squaredNorm();
            if (d2 < best_closing2) { best_closing2 = d2; best_start = start; }
        }
        std::rotate(indices.begin(), indices.begin() + best_start, indices.end());
    }

    return indices;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_spiral(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    auto path = spiral_order_core(instance_centers);

    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_spiral(const Print& print)
{
    return chain_print_object_instances_spiral(print.objects().vector(), nullptr);
}

} // namespace Slic3r
