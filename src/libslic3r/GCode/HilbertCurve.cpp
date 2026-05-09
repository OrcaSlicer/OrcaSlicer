// Hilbert curve ordering: map 2D positions to 1D Hilbert curve index, sort,
// then 2-opt and rotate to minimize closing edge.

#include "HilbertCurve.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

static uint64_t hilbert_xy2d(int bits, int64_t x, int64_t y)
{
    uint64_t d = 0;
    for (int s = bits - 1; s >= 0; --s) {
        int rx = int((x >> s) & 1);
        int ry = int((y >> s) & 1);
        d = (d << 2) | (rx * 3 + ry * 2);
        if (rx == 0) {
            if (ry != 0) std::swap(x, y);
        } else {
            if (ry == 0) std::swap(x, y);
        }
    }
    return d;
}

// Core algorithm: Hilbert curve ordering on a raw point set.
std::vector<size_t> hilbert_curve_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Compute bounding box.
    int64_t xmin = centers[0].x(), xmax = xmin;
    int64_t ymin = centers[0].y(), ymax = ymin;
    for (size_t i = 1; i < n; ++i) {
        xmin = std::min(xmin, centers[i].x());
        xmax = std::max(xmax, centers[i].x());
        ymin = std::min(ymin, centers[i].y());
        ymax = std::max(ymax, centers[i].y());
    }

    int64_t w = xmax - xmin;
    int64_t h = ymax - ymin;

    if (w == 0 && h == 0) {
        std::vector<size_t> path(n);
        for (size_t i = 0; i < n; ++i) path[i] = i;
        return path;
    }

    const int bits = 12;
    int64_t grid_size = (1LL << bits) - 1;

    std::vector<std::pair<uint64_t, size_t>> hilbert_indices(n);
    for (size_t i = 0; i < n; ++i) {
        int64_t px = (w == 0) ? 0 : ((centers[i].x() - xmin) * grid_size) / w;
        int64_t py = (h == 0) ? 0 : ((centers[i].y() - ymin) * grid_size) / h;
        hilbert_indices[i] = { hilbert_xy2d(bits, px, py), i };
    }

    std::sort(hilbert_indices.begin(), hilbert_indices.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<size_t> path(n);
    for (size_t i = 0; i < n; ++i) path[i] = hilbert_indices[i].second;

    // 2-opt improvement pass.
    {
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
                    double current_dist = (centers[path[i_next]].cast<double>() - centers[path[i]].cast<double>()).norm()
                                       + (centers[path[j_next]].cast<double>() - centers[path[j]].cast<double>()).norm();
                    double new_dist     = (centers[path[j]].cast<double>() - centers[path[i]].cast<double>()).norm()
                                       + (centers[path[j_next]].cast<double>() - centers[path[i_next]].cast<double>()).norm();

                    if (new_dist < current_dist) {
                        while (i_next < j) { std::swap(path[i_next], path[j]); ++i_next; --j; }
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
        size_t pn = path.size();
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
                            centers[path[i]], centers[path[i_next]],
                            centers[path[j]], centers[path[j_next]])) {
                        while (i_next < j) { std::swap(path[i_next], path[j]); ++i_next; --j; }
                        had_crossing = true;
                    }
                }
            }
        }
    }

    // Rotate to minimize the closing edge.
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

    return path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_hilbert(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    auto path = hilbert_curve_core(instance_centers);

    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_hilbert(const Print& print)
{
    return chain_print_object_instances_hilbert(print.objects().vector(), nullptr);
}

} // namespace Slic3r
