// Shared TSP post-processing utilities: implementation.

#include "TSPPostProcessing.hpp"
#include "../Geometry.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

void tsp_2opt_improve(std::vector<size_t>& path, const Points& centers, int max_passes)
{
    size_t pn = path.size();
    bool improved = true;
    while (improved) {
        if (max_passes > 0 && --max_passes == 0) break; // iteration limit reached
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

void tsp_remove_crossings(std::vector<size_t>& path, const Points& centers)
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

void tsp_rotate_minimize_closing(std::vector<size_t>& path, const Points& centers)
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

} // namespace Slic3r
