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
    if (pn <= 2) return;

    // Best-first 2-opt: each pass finds the single best swap across all pairs,
    // then applies it. Converges in far fewer passes than first-improving.
    for (int pass = 0; max_passes <= 0 || pass < max_passes; ++pass) {
        size_t best_i = pn, best_j = pn;
        double best_gain = 0;

        for (size_t i = 0; i < pn; ++i) {
            const Vec2d& pi   = centers[path[i]].cast<double>();
            const Vec2d& p_in = centers[path[(i + 1) % pn]].cast<double>();
            double d_i = (p_in - pi).norm();

            for (size_t j = i + 2; j < pn; ++j) {
                size_t j_next = (j + 1) % pn;
                if ((i == 0 && j_next == 0)) continue; // closing edge pair, skip

                const Vec2d& pj   = centers[path[j]].cast<double>();
                const Vec2d& p_jn = centers[path[j_next]].cast<double>();
                double d_j = (p_jn - pj).norm();

                // Gain from reversing [i+1 .. j]: remove edges (i,i+1) and (j,j+1), add (i,j) and (i+1,j+1)
                double gain = d_i + d_j - (pj - pi).norm() - (p_jn - p_in).norm();
                if (gain > best_gain) {
                    best_gain = gain;
                    best_i = i; best_j = j;
                }
            }
        }

        // Apply the best swap found, or stop if none improved.
        if (best_i == pn) break;
        size_t a = best_i + 1, b = best_j;
        while (a < b) std::swap(path[a++], path[b--]);
    }
}

// Fast bounding-box overlap test (rejects most non-intersecting pairs).
static inline bool bboxes_overlap(const Point& a, const Point& b, const Point& c, const Point& d)
{
    return !(std::max(a.x(), b.x()) < std::min(c.x(), d.x()) ||
             std::max(c.x(), d.x()) < std::min(a.x(), b.x()) ||
             std::max(a.y(), b.y()) < std::min(c.y(), d.y()) ||
             std::max(c.y(), d.y()) < std::min(a.y(), b.y()));
}

void tsp_remove_crossings(std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn <= 3) return;

    int max_iters = static_cast<int>(pn * pn);
    while (max_iters-- > 0) {
        // Batch pass: collect all crossings, then reverse them all at once.
        // This avoids restarting the scan from i=0 after every single reversal.
        struct Crossing { size_t i, j; };
        std::vector<Crossing> crossings;
        crossings.reserve(pn / 2);

        for (size_t i = 0; i < pn; ++i) {
            size_t i_next = (i + 1) % pn;
            const Point& ai = centers[path[i]];
            const Point& bi = centers[path[i_next]];

            for (size_t j = i + 2; j < pn; ++j) {
                if (j == i_next) continue;
                if (j == (pn - 1) && i == 0) continue;
                size_t j_next = (j + 1) % pn;
                const Point& aj = centers[path[j]];
                const Point& bj = centers[path[j_next]];

                // Cheap bounding-box prefilter before expensive intersection test.
                if (!bboxes_overlap(ai, bi, aj, bj)) continue;

                if (Geometry::segments_intersect(ai, bi, aj, bj)) {
                    crossings.push_back({i, j});
                }
            }
        }

        if (crossings.empty()) break;

        // Apply all reversals. Process from highest i first so indices stay valid.
        for (auto it = crossings.rbegin(); it != crossings.rend(); ++it) {
            size_t a = it->i + 1, b = it->j;
            while (a < b) std::swap(path[a++], path[b--]);
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
