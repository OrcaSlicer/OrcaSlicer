#include "FillTpmsAdaptive.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "../MarchingSquares.hpp"

namespace Slic3r {

namespace {

// At most 4 MB of depths; finer cells would not change the grading.
constexpr double MaxNodes    = double(1 << 20);
constexpr double MinCellSize = 0.5;

constexpr float  InfF = std::numeric_limits<float>::infinity();
constexpr double InfD = std::numeric_limits<double>::infinity();

// Squared distance transform of a line (Felzenszwalb & Huttenlocher); infinite samples are no sites.
void distance_transform_line(const float *f, float *d, int n, int *v, double *s)
{
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (f[q] == InfF)
            continue;
        double x = -InfD;
        while (k >= 0) {
            x = (f[q] + double(q) * q - f[v[k]] - double(v[k]) * v[k]) / (2. * (q - v[k]));
            if (x > s[k])
                break;
            --k;
        }
        if (k < 0)
            x = -InfD;
        v[++k]   = q;
        s[k]     = x;
        s[k + 1] = InfD;
    }
    if (k < 0) {
        std::fill(d, d + n, InfF);
        return;
    }
    for (int q = 0, j = 0; q < n; ++q) {
        while (s[j + 1] < q)
            ++j;
        d[q] = float(sqr(double(q - v[j])) + f[v[j]]);
    }
}

void distance_transform_axis(std::vector<float> &grid, const Vec3i32 &size, int axis, const std::function<void()> &throw_if_canceled)
{
    const int    n      = size[axis];
    const int    a1     = (axis + 1) % 3;
    const int    a2     = (axis + 2) % 3;
    const size_t stride = axis == 0 ? 1 : axis == 1 ? size_t(size.x()) : size_t(size.x()) * size.y();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, size_t(size[a1]) * size[a2]), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<float>  f(n), d(n);
        std::vector<int>    v(n);
        std::vector<double> s(n + 1);
        for (size_t line = range.begin(); line < range.end(); ++line) {
            Vec3i32 idx;
            idx[axis]          = 0;
            idx[a1]            = int(line % size[a1]);
            idx[a2]            = int(line / size[a1]);
            const size_t first = (size_t(idx.z()) * size.y() + idx.y()) * size.x() + idx.x();
            for (int i = 0; i < n; ++i)
                f[i] = grid[first + i * stride];
            distance_transform_line(f.data(), d.data(), n, v.data(), s.data());
            for (int i = 0; i < n; ++i)
                grid[first + i * stride] = d[i];
        }
        throw_if_canceled();
    });
}

// Marks the nodes inside the expolygons with infinity, by even-odd scanlines.
void rasterize(const ExPolygons &expolygons, const Vec2d &origin, double cell, int nx, int ny, float *nodes)
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
            std::fill(nodes + size_t(j) * nx + std::min(i0, i1), nodes + size_t(j) * nx + i1, InfF);
        }
    }
}

// Blend weight of the denser pattern giving the density fraction p between the two, measured at p = 0, 0.05, ..., 1.
float dense_weight(float p)
{
    static constexpr std::array<float, 21> weights{0.f,   .183f, .240f, .277f, .306f, .330f, .348f, .368f, .388f, .408f, .427f,
                                                   .446f, .464f, .482f, .500f, .523f, .549f, .580f, .623f, .690f, 1.f};
    const float  x = std::clamp(p, 0.f, 1.f) * float(weights.size() - 1);
    const size_t i = std::min(size_t(x), weights.size() - 2);
    return weights[i] + (weights[i + 1] - weights[i]) * (x - float(i));
}

double adaptive_density(const AdaptiveTpms &tpms, double depth)
{
    const double surface  = tpms.surface_density;
    const double interior = tpms.interior_density;
    switch (tpms.gradient) {
    case TpmsAdaptiveGradient::Quadratic: return surface + (interior - surface) * depth * depth;
    case TpmsAdaptiveGradient::Exponential: return surface * std::pow(interior / surface, depth);
    default: return surface + (interior - surface) * depth;
    }
}

} // namespace

TpmsDepthField::TpmsDepthField(const std::vector<Slice> &slices, const BoundingBox &bbox, const std::function<void()> &throw_if_canceled)
{
    assert(!slices.empty());
    const Vec3d min(unscaled(bbox.min.x()), unscaled(bbox.min.y()), slices.front().bottom_z);
    const Vec3d extent = Vec3d(unscaled(bbox.max.x()), unscaled(bbox.max.y()), slices.back().top_z) - min;

    // Padded by a node on each side, so that the border of the grid is outside.
    m_cell     = std::max(MinCellSize, std::cbrt(extent.prod() / MaxNodes));
    auto nodes = [this](double length) { return int(std::ceil(length / m_cell)) + 3; };
    while (double(nodes(extent.x())) * nodes(extent.y()) * nodes(extent.z()) > MaxNodes)
        m_cell *= 1.1;
    m_size   = Vec3i32(nodes(extent.x()), nodes(extent.y()), nodes(extent.z()));
    m_origin = min - Vec3d::Constant(m_cell);

    const size_t       level_nodes = size_t(m_size.x()) * m_size.y();
    std::vector<float> grid(level_nodes * m_size.z(), 0.f);
    tbb::parallel_for(tbb::blocked_range<int>(0, m_size.z()), [&](const tbb::blocked_range<int> &range) {
        for (int k = range.begin(); k < range.end(); ++k) {
            const double z  = m_origin.z() + k * m_cell;
            auto         it = std::lower_bound(slices.begin(), slices.end(), z, [](const Slice &s, double z) { return s.top_z < z; });
            if (it != slices.end() && z > it->bottom_z)
                rasterize(*it->expolygons, m_origin.head<2>(), m_cell, m_size.x(), m_size.y(), grid.data() + k * level_nodes);
        }
        throw_if_canceled();
    });
    for (int axis = 0; axis < 3; ++axis)
        distance_transform_axis(grid, m_size, axis, throw_if_canceled);

    // The surface lies about half a cell from the nearest node outside of the object.
    float max_depth = 0.f;
    for (float &d : grid) {
        d         = std::max(0.f, (std::sqrt(d) - 0.5f) * float(m_cell));
        max_depth = std::max(max_depth, d);
    }
    if (max_depth > 0.f)
        for (float &d : grid)
            d /= max_depth;
    m_depth = std::move(grid);
}

float TpmsDepthField::depth(const Vec3d &pt) const
{
    int   idx[3];
    float frac[3];
    for (int axis = 0; axis < 3; ++axis) {
        const double x = std::clamp((pt[axis] - m_origin[axis]) / m_cell, 0., double(m_size[axis] - 1));
        idx[axis]      = std::min(int(x), m_size[axis] - 2);
        frac[axis]     = float(x - idx[axis]);
    }
    const size_t sy = size_t(m_size.x());
    const size_t sz = sy * m_size.y();
    const float *p  = m_depth.data() + idx[2] * sz + idx[1] * sy + idx[0];
    auto lerp_x     = [&frac](const float *q) { return q[0] + (q[1] - q[0]) * frac[0]; };
    const float y0  = lerp_x(p) + (lerp_x(p + sy) - lerp_x(p)) * frac[1];
    const float y1  = lerp_x(p + sz) + (lerp_x(p + sz + sy) - lerp_x(p + sz)) * frac[1];
    return y0 + (y1 - y0) * frac[2];
}

} // namespace Slic3r

namespace marchsq {
using namespace Slic3r;

struct AdaptiveTpmsField
{
    static constexpr float gsizef = 0.40f;  // grid cell size in mm (roughly line segment length).
    static constexpr float rsizef = 0.004f; // raster pixel size in mm (roughly point accuracy).
    const coord_t          rsize  = scaled(rsizef);
    const long             gsize  = std::lround(gsizef / rsizef);

    const AdaptiveTpms   &tpms;
    const TpmsDepthField &depth_field;
    Point                 size;
    Point                 offs;
    float                 z;
    double                depth_z;
    double                cos_angle;
    double                sin_angle;
    double                dense_density;
    double                sparse_density;
    float                 dense_frequency;
    float                 sparse_frequency;

    AdaptiveTpmsField(const AdaptiveTpms &tpms, const TpmsDepthField &depth_field, const BoundingBox &bbox, coordf_t z, coordf_t depth_z, float angle)
        : tpms(tpms), depth_field(depth_field), size(bbox.size()), offs(bbox.min), z(float(z)), depth_z(depth_z)
        , cos_angle(std::cos(angle)), sin_angle(std::sin(angle))
    {
        const bool surface_denser = tpms.surface_density >= tpms.interior_density;
        dense_density    = surface_denser ? tpms.surface_density : tpms.interior_density;
        sparse_density   = surface_denser ? tpms.interior_density : tpms.surface_density;
        dense_frequency  = float(surface_denser ? tpms.surface_frequency : tpms.interior_frequency);
        sparse_frequency = float(surface_denser ? tpms.interior_frequency : tpms.surface_frequency);
    }

    float get_scalar(const Coord &p) const
    {
        const Point  pt = to_Point(p);
        const double x  = unscaled(pt.x());
        const double y  = unscaled(pt.y());
        // The depth field is in the object frame, the fill is rotated by -angle.
        const double depth = depth_field.depth(Vec3d(cos_angle * x - sin_angle * y, sin_angle * x + cos_angle * y, depth_z));
        const double range = dense_density - sparse_density;
        const float  w     = range > EPSILON ? dense_weight(float((adaptive_density(tpms, depth) - sparse_density) / range)) : 1.f;
        float        value = 0.f;
        if (w > 0.f)
            value += w * tpms.equation(dense_frequency * float(x), dense_frequency * float(y), dense_frequency * z);
        if (w < 1.f)
            value += (1.f - w) * tpms.equation(sparse_frequency * float(x), sparse_frequency * float(y), sparse_frequency * z);
        return value;
    }

    inline coord_t to_coord(long x) const { return x * rsize; }
    inline long    to_coordr(coord_t x) const { return x / rsize; }
    inline Point   to_Point(const Coord &p) const { return Point(to_coord(p.c) + offs.x(), to_coord(p.r) + offs.y()); }
};

template<> struct _RasterTraits<AdaptiveTpmsField>
{
    using ValueType = float;
    static float  get(const AdaptiveTpmsField &sf, size_t row, size_t col) { return sf.get_scalar(Coord(long(row), long(col))); }
    static size_t rows(const AdaptiveTpmsField &sf) { return sf.to_coordr(sf.size.y()); }
    static size_t cols(const AdaptiveTpmsField &sf) { return sf.to_coordr(sf.size.x()); }
};

} // namespace marchsq

namespace Slic3r {

Polylines make_adaptive_tpms(const AdaptiveTpms &tpms, const TpmsDepthField &depth_field, const BoundingBox &bbox,
                             coordf_t z, coordf_t layer_height, coordf_t spacing, float angle)
{
    const marchsq::AdaptiveTpmsField field(tpms, depth_field, bbox, z, z - 0.5 * layer_height, angle);
    const std::vector<marchsq::Ring> rings = marchsq::execute_with_policy(ex_tbb, field, 0.f, {field.gsize, field.gsize});

    // Loops pinched off by the blend print as blobs when narrower than two lines.
    const double min_loop_length = scaled(2. * PI * spacing);
    Polylines    polylines;
    polylines.reserve(rings.size());
    for (const marchsq::Ring &ring : rings) {
        Polyline polyline;
        polyline.points.reserve(ring.size() + 1);
        for (const marchsq::Coord &crd : ring)
            polyline.points.emplace_back(field.to_Point(crd));
        polyline.points.push_back(polyline.points.front());
        polyline.simplify(SCALED_SPARSE_INFILL_RESOLUTION);
        if (polyline.length() >= min_loop_length)
            polylines.push_back(std::move(polyline));
    }
    return polylines;
}

} // namespace Slic3r
