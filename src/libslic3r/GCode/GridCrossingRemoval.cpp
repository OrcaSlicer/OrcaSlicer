// Grid-based crossing removal: spatial hash edges into grid cells,
// only test edges within same/adjacent cells. O(n) average case vs O(n²).

#include "GridCrossingRemoval.hpp"
#include "../Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Slic3r {

// Grid-based crossing removal: spatial hash edges into grid cells,
// assigns each edge to all cells its bounding box touches.
void tsp_remove_crossings_grid(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn < 4) return;

    // Compute bounding box to determine grid cell size.
    double x_min = std::numeric_limits<double>::max(), y_min = std::numeric_limits<double>::max();
    double x_max = -std::numeric_limits<double>::max(), y_max = -std::numeric_limits<double>::max();
    for (const auto& pt : centers) {
        double px = static_cast<double>(pt.x()), py = static_cast<double>(pt.y());
        if (px < x_min) x_min = px;
        if (py < y_min) y_min = py;
        if (px > x_max) x_max = px;
        if (py > y_max) y_max = py;
    }

    double width = x_max - x_min, height = y_max - y_min;
    if (width == 0 && height == 0) return;

    // Grid cell size: target ~16 edges per cell on average.
    int grid_cols = std::max(2, static_cast<int>(std::sqrt(static_cast<double>(pn))));
    int grid_rows = grid_cols;
    double cell_w = width / grid_cols + 1e-9;
    double cell_h = height / grid_rows + 1e-9;

    // Hash: (col, row) → list of edge indices.
    auto hash_key = [&](int col, int row) -> size_t {
        return static_cast<size_t>(col) * static_cast<size_t>(grid_rows + 1) + static_cast<size_t>(row);
    };

    std::unordered_map<size_t, std::vector<int>> grid;

    // Assign each edge to ALL cells its bounding box touches.
    auto assign_edge = [&](int edge_idx) {
        size_t i = path[edge_idx];
        size_t j = path[(edge_idx + 1) % pn];
        double ex0 = static_cast<double>(centers[i].x());
        double ey0 = static_cast<double>(centers[i].y());
        double ex1 = static_cast<double>(centers[j].x());
        double ey1 = static_cast<double>(centers[j].y());

        int c0 = std::max(0, static_cast<int>((std::min(ex0, ex1) - x_min) / cell_w));
        int r0 = std::max(0, static_cast<int>((std::min(ey0, ey1) - y_min) / cell_h));
        int c1 = std::min(grid_cols - 1, static_cast<int>((std::max(ex0, ex1) - x_min) / cell_w));
        int r1 = std::min(grid_rows - 1, static_cast<int>((std::max(ey0, ey1) - y_min) / cell_h));

        for (int c = c0; c <= c1; ++c)
            for (int r = r0; r <= r1; ++r)
                grid[hash_key(c, r)].push_back(edge_idx);
    };

    bool had_crossing = true;
    int max_iters = static_cast<int>(pn * pn);

    while (had_crossing && max_iters-- > 0) {
        // Rebuild grid each iteration (path may have changed).
        grid.clear();
        for (int e = 0; e < static_cast<int>(pn); ++e) assign_edge(e);

        had_crossing = false;
        std::vector<std::pair<int, int>> crossings_to_fix;

        // For each cell, check edges in this cell against edges in adjacent cells.
        for (auto& [key, edges] : grid) {
            int col = static_cast<int>(key / (grid_rows + 1));
            int row = static_cast<int>(key % (grid_rows + 1));

            // Collect candidate edge indices from this cell and neighbors.
            std::vector<int> candidates;
            for (int dc = -1; dc <= 1; ++dc) {
                for (int dr = -1; dr <= 1; ++dr) {
                    int nc = col + dc, nr = row + dr;
                    if (nc >= 0 && nc < grid_cols && nr >= 0 && nr < grid_rows) {
                        auto it = grid.find(hash_key(nc, nr));
                        if (it != grid.end()) candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                    }
                }
            }

            // Deduplicate candidates.
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

            // Check all pairs of candidate edges.
            for (size_t a = 0; a < candidates.size() && !had_crossing; ++a) {
                int ea = candidates[a];
                size_t ia = path[ea], ja = path[(ea + 1) % pn];

                for (size_t b = a + 1; b < candidates.size() && !had_crossing; ++b) {
                    int eb = candidates[b];
                    // Skip adjacent edges (share a vertex).
                    if (eb == ea || eb == static_cast<int>((ea + 1) % pn)) continue;

                    size_t ib = path[eb], jb = path[(eb + 1) % pn];

                    if (Geometry::segments_intersect(centers[ia], centers[ja], centers[ib], centers[jb])) {
                        crossings_to_fix.emplace_back(ea, eb);
                        had_crossing = true;
                    }
                }
            }
        }

        // Apply first crossing fix found.
        if (!crossings_to_fix.empty()) {
            auto [ea, eb] = crossings_to_fix[0];
            size_t i_start = std::min(ea, eb);
            size_t j_start = std::max(ea, eb);
            // Reverse segment between the two crossing edges.
            while (i_start < j_start) {
                std::swap(path[i_start], path[j_start]);
                ++i_start; --j_start;
            }
        }
    }
}

} // namespace Slic3r
