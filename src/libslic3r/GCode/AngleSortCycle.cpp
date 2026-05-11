// Angle sort + 2-opt: sort points by angle around centroid, then 2-opt to improve.
// Produces a simple polygon that visits objects in angular order.

#include "AngleSortCycle.hpp"
#include "TSPPostProcessing.hpp"
#include "../Print.hpp"

#include <cmath>
#include <algorithm>
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

    // Post-processing: 2-opt, crossing removal, rotation.
    tsp_2opt_improve(indices, centers);
    tsp_remove_crossings(indices, centers);
    tsp_rotate_minimize_closing(indices, centers);

    return indices;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, angle_sort_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_angle_sort(const Print& print)
{
    return chain_print_object_instances_angle_sort(print.objects().vector(), nullptr);
}

} // namespace Slic3r
