// Hilbert curve ordering: map 2D positions to 1D Hilbert curve index, sort,
// then 2-opt and rotate to minimize closing edge.
// Excellent locality for clustered object layouts.

#include "HilbertCurve.hpp"
#include "../Print.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Convert (x, y) coordinates to Hilbert d-value using rotation/reflection.
// `bits` is the precision of the Hilbert curve (typically 10-14 for our range).
static uint64_t hilbert_xy2d(int bits, int64_t x, int64_t y)
{
    uint64_t d = 0;
    for (int s = bits - 1; s >= 0; --s) {
        int rx = int((x >> s) & 1);
        int ry = int((y >> s) & 1);
        d = (d << 2) | (rx * 3 + ry * 2);

        // Rotate / reflect based on sub-square.
        if (rx == 0) {
            if (ry == 0) {
                // 0: rotate 180 degrees
                // (no-op — already in correct orientation for the sub-square)
            } else {
                // 1: reflect across y=x
                std::swap(x, y);
            }
        } else {
            if (ry == 0) {
                // 3: reflect across y=x
                std::swap(x, y);
            } else {
                // 2: rotate 0 degrees
                // (no-op)
            }
        }
    }
    return d;
}

std::vector<const PrintInstance*> chain_print_object_instances_hilbert(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    // Compute bounding box.
    int64_t xmin = instance_centers[0].x(), xmax = xmin;
    int64_t ymin = instance_centers[0].y(), ymax = ymin;
    for (size_t i = 1; i < n; ++i) {
        xmin = std::min(xmin, instance_centers[i].x());
        xmax = std::max(xmax, instance_centers[i].x());
        ymin = std::min(ymin, instance_centers[i].y());
        ymax = std::max(ymax, instance_centers[i].y());
    }

    int64_t w = xmax - xmin;
    int64_t h = ymax - ymin;

    // Handle degenerate cases (all points at same location or collinear).
    if (w == 0 && h == 0) {
        // All identical — just return in order.
        std::vector<const PrintInstance*> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.emplace_back(&print_objects[instances[i].first]->instances()[instances[i].second]);
        return out;
    }

    // Scale to Hilbert grid. Use enough bits to resolve all points.
    // 12 bits = 4096 grid cells — more than enough for typical object counts.
    const int bits = 12;
    int64_t grid_size = (1LL << bits) - 1; // 4095

    // Map each point to Hilbert index.
    std::vector<std::pair<uint64_t, size_t>> hilbert_indices(n);
    for (size_t i = 0; i < n; ++i) {
        int64_t px = (w == 0) ? 0 : ((instance_centers[i].x() - xmin) * grid_size) / w;
        int64_t py = (h == 0) ? 0 : ((instance_centers[i].y() - ymin) * grid_size) / h;
        hilbert_indices[i] = { hilbert_xy2d(bits, px, py), i };
    }

    // Sort by Hilbert d-value.
    std::sort(hilbert_indices.begin(), hilbert_indices.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Extract sorted indices.
    std::vector<size_t> path(n);
    for (size_t i = 0; i < n; ++i)
        path[i] = hilbert_indices[i].second;

    // If start_near is provided, rotate so the closest point is first.
    if (start_near != nullptr) {
        size_t best_start = 0;
        double best_d2 = std::numeric_limits<double>::max();
        for (size_t k = 0; k < n; ++k) {
            double d2 = (instance_centers[path[k]].cast<double>() - start_near->cast<double>()).squaredNorm();
            if (d2 < best_d2) { best_d2 = d2; best_start = k; }
        }
        std::rotate(path.begin(), path.begin() + best_start, path.end());
    }

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
                    double current_dist = (instance_centers[path[i_next]].cast<double>() - instance_centers[path[i]].cast<double>()).norm()
                                       + (instance_centers[path[j_next]].cast<double>() - instance_centers[path[j]].cast<double>()).norm();
                    double new_dist     = (instance_centers[path[j]].cast<double>() - instance_centers[path[i]].cast<double>()).norm()
                                       + (instance_centers[path[j_next]].cast<double>() - instance_centers[path[i_next]].cast<double>()).norm();

                    if (new_dist < current_dist) {
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

    // Rotate to minimize the closing edge (last -> first).
    {
        size_t pn = path.size();
        size_t best_start = 0;
        double best_closing2 = std::numeric_limits<double>::max();
        for (size_t start = 0; start < pn; ++start) {
            size_t last = (start + pn - 1) % pn;
            double d2 = (instance_centers[path[start]].cast<double>() - instance_centers[path[last]].cast<double>()).squaredNorm();
            if (d2 < best_closing2) { best_closing2 = d2; best_start = start; }
        }
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

std::vector<const PrintInstance*> chain_print_object_instances_hilbert(const Print& print)
{
    return chain_print_object_instances_hilbert(print.objects().vector(), nullptr);
}

} // namespace Slic3r
