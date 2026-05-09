// Angle sort + 2-opt: sort points by angle around centroid, then 2-opt to improve.
// Produces a simple polygon that visits objects in angular order.

#include "AngleSortCycle.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Core algorithm: angle sort + 2-opt on a raw point set.
std::vector<size_t> angle_sort_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Compute centroid.
    Vec2d centroid(0, 0);
    for (const Point& pt : centers)
        centroid += pt.cast<double>();
    centroid /= n;

    // Sort indices by angle around centroid.
    std::vector<size_t> indices(n);
    for (size_t i = 0; i < n; ++i) indices[i] = i;

    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        Vec2d va = centers[a].cast<double>() - centroid;
        Vec2d vb = centers[b].cast<double>() - centroid;
        double angle_a = std::atan2(va.y(), va.x());
        double angle_b = std::atan2(vb.y(), vb.x());
        if (angle_a < 0) angle_a += 2 * M_PI;
        if (angle_b < 0) angle_b += 2 * M_PI;
        if (std::abs(angle_a - angle_b) > 1e-9) return angle_a < angle_b;
        return va.squaredNorm() < vb.squaredNorm();
    });

    // 2-opt improvement pass.
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

                    if (new_dist < current_dist) {
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
std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    auto path = angle_sort_core(instance_centers);

    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const Print& print)
{
    return chain_print_object_instances_angle_sort(print.objects().vector(), nullptr);
}

} // namespace Slic3r
