#include "VoronoiHull.hpp"
#include "../MarchingSquares.hpp"
#include "../ClipperUtils.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../libslic3r.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {

// Cached millimetre samples, interpolated by Orca's marching-squares routine.
// Fine raster coordinates refine crossings without additional field queries.
struct HullRaster {
    static constexpr long subdivisions = 50;
    size_t rows, cols;
    std::vector<double> values;
};

} // namespace Slic3r::Voronoi

namespace marchsq {
template<> struct _RasterTraits<Slic3r::Voronoi::HullRaster> {
    using ValueType = double;
    static size_t rows(const Slic3r::Voronoi::HullRaster &r) { return (r.rows - 1) * r.subdivisions + 1; }
    static size_t cols(const Slic3r::Voronoi::HullRaster &r) { return (r.cols - 1) * r.subdivisions + 1; }
    static double get(const Slic3r::Voronoi::HullRaster &r, size_t row, size_t col) {
        const double x = double(col) / r.subdivisions, y = double(row) / r.subdivisions;
        const size_t ix = std::min(size_t(x), r.cols - 2), iy = std::min(size_t(y), r.rows - 2);
        const double fx = x - ix, fy = y - iy;
        const size_t i = iy * r.cols + ix;
        return (1. - fy) * ((1. - fx) * r.values[i] + fx * r.values[i + 1]) +
               fy * ((1. - fx) * r.values[i + r.cols] + fx * r.values[i + r.cols + 1]);
    }
};
} // namespace marchsq

namespace Slic3r::Voronoi {

ExPolygons density_void(const PointCloudProvider &field, const BoundingBox &bounds, double z, double threshold)
{
    if (threshold == 0. || !bounds.defined) return {};
    if (!std::isfinite(threshold) || threshold < 0. || threshold > 1.)
        throw std::invalid_argument("Invalid Voronoi hull threshold");
    const Vec2d min = bounds.min.cast<double>() * SCALING_FACTOR;
    const Vec2d max = bounds.max.cast<double>() * SCALING_FACTOR;
    double step = 1.;
    size_t nx, ny;
    // Contour extraction must also remain bounded for exceptionally large objects.
    do {
        nx = size_t(std::ceil((max.x() - min.x()) / step)) + 5;
        ny = size_t(std::ceil((max.y() - min.y()) / step)) + 5;
        if (nx <= 4 * 1024 * 1024 / ny) break;
        step *= 2.;
    } while (true);
    const Vec2d origin = min - Vec2d::Constant(2. * step);
    HullRaster raster {ny, nx, {}};
    raster.values.reserve(nx * ny);
    std::vector<Vec3d> points;
    points.reserve(nx);
    for (size_t y = 0; y < ny; ++y) {
        points.clear();
        for (size_t x = 0; x < nx; ++x)
            points.emplace_back(origin.x() + x * step, origin.y() + y * step, z);
        auto values = field.relative_density(points);
        if (values.size() != nx) throw std::invalid_argument("Invalid Voronoi hull samples");
        raster.values.insert(raster.values.end(), values.begin(), values.end());
    }
    for (size_t y = 0; y < ny; ++y)
        for (size_t x = 0; x < nx; ++x)
            raster.values[y * nx + x] = x == 0 || y == 0 || x + 1 == nx || y + 1 == ny ?
                -1. : threshold - raster.values[y * nx + x];
    Polygons polygons;
    for (const auto &ring : marchsq::execute(raster, 0., {raster.subdivisions, raster.subdivisions})) {
        Points vertices;
        vertices.reserve(ring.size());
        for (const auto &p : ring)
            vertices.push_back(Point::new_scale(origin.x() + p.c * step / raster.subdivisions,
                                                origin.y() + p.r * step / raster.subdivisions));
        if (vertices.size() >= 3) polygons.emplace_back(std::move(vertices));
    }
    // Marching-square rings do not encode hole winding. Nesting determines
    // empty islands, including material surrounding a model's existing holes.
    return union_ex(polygons, pftEvenOdd);
}

HullSection hull_section(const PointCloudProvider &field, const BoundingBox &bounds,
                         double z, double height, double width, double threshold)
{
    if (threshold == 0.) return {};
    HullSection result;
    result.cavity = density_void(field, bounds, z, threshold);
    const auto bottom = density_void(field, bounds, z - 0.5 * height, threshold);
    const auto top = density_void(field, bounds, z + 0.5 * height, threshold);
    ExPolygons envelope = result.cavity;
    append(envelope, bottom);
    append(envelope, top);
    const auto core = intersection_ex(intersection_ex(bottom, top), result.cavity);
    result.skin = diff_ex(offset_ex(union_ex(envelope), float(scale_(width))), core);
    return result;
}

} // namespace Slic3r::Voronoi
