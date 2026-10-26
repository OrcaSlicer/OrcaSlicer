#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "Polygon.hpp"

namespace Slic3r {

// Exact Euclidean distance transforms on regular grids (Felzenszwalb & Huttenlocher, "Distance Transforms of Sampled
// Functions", 2012), and the rasterization of layer outlines that feeds them. A distance field of an object is made by
// rasterizing the outlines of its layers into the slabs of a 3D grid, then transforming the grid along each axis.

// Squared distance of each of the n samples of a line to the nearest site, in samples. A finite sample of f is a site
// at that squared distance, so 0 marks the sites; an infinite sample is not a site. A line without sites stays
// infinite. v and s are scratch space of n and n + 1 elements.
void squared_distance_transform_line(const float *f, float *d, int n, int *v, double *s);

// Applies squared_distance_transform_line() along one axis of a 3D grid stored x fastest, in parallel. Applied along
// each axis in turn, it gives every node the squared distance in nodes to its nearest site.
void squared_distance_transform_axis(std::vector<float> &grid, const Vec3i32 &size, int axis, const std::function<void()> &throw_if_canceled);

// Sets the nodes of an nx by ny raster stored x fastest that are inside the expolygons to value, by even-odd
// scanlines. Node (i, j) is at origin + (i, j) * cell, in mm.
template<typename T>
void rasterize_expolygons(const ExPolygons &expolygons, const Vec2d &origin, double cell, int nx, int ny, T value, T *nodes)
{
    std::vector<std::vector<double>> crossings(ny);
    auto add_crossings = [&](const Polygon &polygon) {
        const Points &pts = polygon.points;
        for (size_t i = 0; i < pts.size(); ++i) {
            const Vec2d a = unscaled(pts[i]);
            const Vec2d b = unscaled(pts[i + 1 == pts.size() ? 0 : i + 1]);
            if (a.y() == b.y())
                continue;
            const auto [lo, hi] = std::minmax(a.y(), b.y());
            const int j0        = std::max(0, int(std::ceil((lo - origin.y()) / cell)));
            const int j1        = std::min(ny, int(std::ceil((hi - origin.y()) / cell)));
            for (int j = j0; j < j1; ++j) {
                const double y = origin.y() + j * cell;
                crossings[j].push_back(a.x() + (b.x() - a.x()) * (y - a.y()) / (b.y() - a.y()));
            }
        }
    };
    for (const ExPolygon &expolygon : expolygons) {
        add_crossings(expolygon.contour);
        for (const Polygon &hole : expolygon.holes)
            add_crossings(hole);
    }
    for (int j = 0; j < ny; ++j) {
        std::vector<double> &xs = crossings[j];
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int i0 = std::max(0, int(std::ceil((xs[k] - origin.x()) / cell)));
            const int i1 = std::min(nx, int(std::ceil((xs[k + 1] - origin.x()) / cell)));
            std::fill(nodes + size_t(j) * nx + std::min(i0, i1), nodes + size_t(j) * nx + i1, value);
        }
    }
}

} // namespace Slic3r
