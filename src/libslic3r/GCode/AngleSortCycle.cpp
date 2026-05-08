// Angle sort + 2-opt: sort points by angle around centroid, then 2-opt to improve.
// Produces a simple polygon that visits objects in angular order.

#include "AngleSortCycle.hpp"
#include "../Print.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    // Compute centroid.
    Vec2d centroid(0, 0);
    for (const Point& pt : instance_centers)
        centroid += pt.cast<double>();
    centroid /= n;

    // Sort indices by angle around centroid.
    // For points with the same angle (collinear with centroid), sort by distance.
    std::vector<size_t> indices(n);
    for (size_t i = 0; i < n; ++i)
        indices[i] = i;

    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        Vec2d va = instance_centers[a].cast<double>() - centroid;
        Vec2d vb = instance_centers[b].cast<double>() - centroid;
        double angle_a = std::atan2(va.y(), va.x());
        double angle_b = std::atan2(vb.y(), vb.x());
        // Normalize to [0, 2*pi) for comparison.
        if (angle_a < 0) angle_a += 2 * M_PI;
        if (angle_b < 0) angle_b += 2 * M_PI;
        if (std::abs(angle_a - angle_b) > 1e-9)
            return angle_a < angle_b;
        // Same angle: closer points first.
        return va.squaredNorm() < vb.squaredNorm();
    });

    // If start_near is provided, rotate so the closest point is first.
    if (start_near != nullptr) {
        size_t best_start = 0;
        double best_d2 = std::numeric_limits<double>::max();
        for (size_t k = 0; k < n; ++k) {
            double d2 = (instance_centers[indices[k]].cast<double>() - start_near->cast<double>()).squaredNorm();
            if (d2 < best_d2) { best_d2 = d2; best_start = k; }
        }
        std::rotate(indices.begin(), indices.begin() + best_start, indices.end());
    }

    // 2-opt improvement pass.
    // For a simple polygon, 2-opt preserves simplicity (no new crossings).
    // This shortens the path while keeping it non-self-intersecting.
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
                    double current_dist = (instance_centers[indices[i_next]].cast<double>() - instance_centers[indices[i]].cast<double>()).norm()
                                       + (instance_centers[indices[j_next]].cast<double>() - instance_centers[indices[j]].cast<double>()).norm();
                    double new_dist     = (instance_centers[indices[j]].cast<double>() - instance_centers[indices[i]].cast<double>()).norm()
                                       + (instance_centers[indices[j_next]].cast<double>() - instance_centers[indices[i_next]].cast<double>()).norm();

                    if (new_dist < current_dist) {
                        while (i_next < j) {
                            std::swap(indices[i_next], indices[j]);
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

    // Rotate to minimize the closing edge (last -> first).
    {
        size_t pn = indices.size();
        size_t best_start = 0;
        double best_closing2 = std::numeric_limits<double>::max();
        for (size_t start = 0; start < pn; ++start) {
            size_t last = (start + pn - 1) % pn;
            double d2 = (instance_centers[indices[start]].cast<double>() - instance_centers[indices[last]].cast<double>()).squaredNorm();
            if (d2 < best_closing2) { best_closing2 = d2; best_start = start; }
        }
        std::rotate(indices.begin(), indices.begin() + best_start, indices.end());
    }

    // Emit the final cycle.
    std::vector<const PrintInstance*> out;
    out.reserve(indices.size());
    for (size_t step = 0; step < indices.size(); ++step) {
        size_t idx = indices[step];
        out.emplace_back(&print_objects[instances[idx].first]->instances()[instances[idx].second]);
    }

    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const Print& print)
{
    return chain_print_object_instances_angle_sort(print.objects().vector(), nullptr);
}

} // namespace Slic3r
