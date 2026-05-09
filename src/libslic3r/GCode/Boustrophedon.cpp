// Boustrophedon (snake-like) ordering: sort objects into rows by Y,
// traverse each row alternating direction for smooth back-and-forth path.

#include "Boustrophedon.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Core algorithm: boustrophedon (snake) ordering on a raw point set.
std::vector<size_t> boustrophedon_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Compute Y range to determine row grouping threshold.
    double y_min = std::numeric_limits<double>::max();
    double y_max_val = -std::numeric_limits<double>::max();
    for (const auto& p : centers) {
        double y = p.y();
        if (y < y_min) y_min = y;
        if (y > y_max_val) y_max_val = y;
    }

    // Row threshold: ~10% of Y range, minimum 10mm to avoid over-splitting.
    double row_threshold = (y_max_val - y_min) * 0.1;
    if (row_threshold < 10000.0) row_threshold = 10000.0;

    // Group into rows using integer binning by Y coordinate.
    std::vector<std::pair<int, std::vector<size_t>>> rows;
    for (size_t i = 0; i < n; ++i) {
        int y_key = static_cast<int>(centers[i].y() / row_threshold);
        bool found = false;
        for (auto& [key, row] : rows) {
            if (key == y_key) {
                row.push_back(i);
                found = true;
                break;
            }
        }
        if (!found) {
            rows.emplace_back(y_key, std::vector<size_t>{i});
        }
    }

    // Sort rows by average Y.
    std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
        double avg_a = 0, avg_b = 0;
        for (size_t idx : a.second) avg_a += centers[idx].y();
        for (size_t idx : b.second) avg_b += centers[idx].y();
        return avg_a / static_cast<double>(a.second.size()) < avg_b / static_cast<double>(b.second.size());
    });

    // Sort each row by X coordinate, then traverse alternating direction.
    std::vector<size_t> path;
    path.reserve(n);
    bool reverse = false;

    for (auto& [key, row] : rows) {
        std::sort(row.begin(), row.end(),
            [&](size_t a, size_t b) { return centers[a].x() < centers[b].x(); });

        if (reverse) {
            path.insert(path.end(), row.rbegin(), row.rend());
        } else {
            path.insert(path.end(), row.begin(), row.end());
        }
        reverse = !reverse;
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
std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    auto path = boustrophedon_core(instance_centers);

    std::vector<const PrintInstance*> out;
    out.reserve(path.size());
    for (size_t step : path) {
        out.emplace_back(&print_objects[instances[step].first]->instances()[instances[step].second]);
    }
    return out;
}

std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const Print& print)
{
    return chain_print_object_instances_boustrophedon(print.objects().vector(), nullptr);
}

} // namespace Slic3r
