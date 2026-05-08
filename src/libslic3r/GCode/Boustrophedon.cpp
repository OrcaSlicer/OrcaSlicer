// Boustrophedon (snake-like) ordering: sort objects into rows by Y,
// traverse each row alternating direction for smooth back-and-forth path.

#include "Boustrophedon.hpp"
#include "../Print.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
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

    // Compute Y range to determine row grouping threshold.
    double y_min = std::numeric_limits<double>::max();
    double y_max = -std::numeric_limits<double>::max();
    for (const auto& p : instance_centers) {
        double y = p.y();
        if (y < y_min) y_min = y;
        if (y > y_max) y_max = y;
    }
    double y_range = y_max - y_min;
    if (y_range < 1.0) y_range = 1.0;

    // Row threshold: ~10% of Y range, minimum 500µm to avoid over-splitting.
    double row_threshold = std::max(y_range * 0.1, 500.0);

    // Sort indices by Y coordinate.
    std::vector<size_t> indices(n);
    for (size_t i = 0; i < n; ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(),
        [&](size_t a, size_t b) { return instance_centers[a].y() < instance_centers[b].y(); });

    // Group into rows: start a new row when Y gap exceeds threshold.
    std::vector<std::vector<size_t>> rows;
    std::vector<size_t> current_row;
    current_row.push_back(indices[0]);

    for (size_t k = 1; k < indices.size(); ++k) {
        double y_diff = std::abs(instance_centers[indices[k]].y() - instance_centers[indices[k - 1]].y());
        if (y_diff > row_threshold) {
            rows.push_back(std::move(current_row));
            current_row.clear();
        }
        current_row.push_back(indices[k]);
    }
    if (!current_row.empty())
        rows.push_back(std::move(current_row));

    // Sort each row by X coordinate, then traverse alternating direction.
    std::vector<size_t> path;
    path.reserve(n);

    for (size_t r = 0; r < rows.size(); ++r) {
        // Sort row by X.
        std::sort(rows[r].begin(), rows[r].end(),
            [&](size_t a, size_t b) { return instance_centers[a].x() < instance_centers[b].x(); });

        // Odd-indexed rows are reversed for the snake pattern.
        if (r % 2 == 1)
            std::reverse(rows[r].begin(), rows[r].end());

        for (size_t idx : rows[r])
            path.push_back(idx);
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

std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const Print& print)
{
    return chain_print_object_instances_boustrophedon(print.objects().vector(), nullptr);
}

} // namespace Slic3r
