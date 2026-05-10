// Boustrophedon (snake-like) ordering: sort objects into rows by Y,
// traverse each row alternating direction for smooth back-and-forth path.

#include "Boustrophedon.hpp"
#include "TSPPostProcessing.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

// Row threshold parameters (in µm).
constexpr double ROW_FRACTION_OF_Y_RANGE = 0.1;   // ~10% of Y range
constexpr double MIN_ROW_THRESHOLD_UM     = 1e4;   // 10 mm floor to avoid over-splitting

// Core algorithm: boustrophedon (snake) ordering on a raw point set.
std::vector<size_t> boustrophedon_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();

    // Compute Y range to determine row grouping threshold.
    double y_min = std::numeric_limits<double>::max();
    double y_max = -std::numeric_limits<double>::max();
    for (const auto& p : centers) {
        double y = p.y();
        if (y < y_min) y_min = y;
        if (y > y_max) y_max = y;
    }

    // Row threshold: fraction of Y range, with a minimum floor.
    double row_threshold = (y_max - y_min) * ROW_FRACTION_OF_Y_RANGE;
    if (row_threshold < MIN_ROW_THRESHOLD_UM) row_threshold = MIN_ROW_THRESHOLD_UM;

    // Group into rows using hash-map binning by Y coordinate.
    std::unordered_map<int, std::vector<size_t>> row_map;
    for (size_t i = 0; i < n; ++i) {
        int y_key = static_cast<int>(centers[i].y() / row_threshold);
        row_map[y_key].push_back(i);
    }

    // Convert to vector of rows with precomputed average Y.
    struct Row { double avg_y; std::vector<size_t> indices; };
    std::vector<Row> rows;
    rows.reserve(row_map.size());
    for (auto& [key, indices] : row_map) {
        double sum = 0;
        for (size_t idx : indices) sum += centers[idx].y();
        rows.push_back({sum / static_cast<double>(indices.size()), std::move(indices)});
    }

    // Sort rows by average Y.
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.avg_y < b.avg_y;
    });

    // Sort each row by X coordinate, then traverse alternating direction.
    std::vector<size_t> path;
    path.reserve(n);
    bool reverse = false;

    for (auto& [avg_y, row] : rows) {
        std::sort(row.begin(), row.end(),
            [&](size_t a, size_t b) { return centers[a].x() < centers[b].x(); });

        if (reverse) {
            path.insert(path.end(), row.rbegin(), row.rend());
        } else {
            path.insert(path.end(), row.begin(), row.end());
        }
        reverse = !reverse;
    }

    // Post-processing: 2-opt, crossing removal, rotation.
    tsp_2opt_improve(path, centers);
    tsp_remove_crossings(path, centers);
    tsp_rotate_minimize_closing(path, centers);

    return path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, boustrophedon_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const Print& print)
{
    return chain_print_object_instances_boustrophedon(print.objects().vector(), nullptr);
}

} // namespace Slic3r
