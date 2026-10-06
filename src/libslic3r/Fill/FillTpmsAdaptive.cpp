#include "FillTpmsAdaptive.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Execution/ExecutionTBB.hpp"
#include "../MarchingSquares.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"

namespace Slic3r {

namespace {

// At most 4 MB of body indices; finer cells would not change the grading.
constexpr double MaxNodes    = double(1 << 20);
constexpr double MinCellSize = 0.5;

// Directions of the reach of a lobe, on a latitude-longitude grid.
constexpr int Polar   = 24;
constexpr int Azimuth = 48;

// Two deepest points are in separate lobes when the depth between them drops below this ratio of the shallower one.
constexpr double NeckRatio = 0.8;
// Lobes shallower than this ratio of the deepest one of their body are graded as part of it.
constexpr double MinLobeRatio = 0.3;
// A lobe reaches twice as far as the side towards its neighbour, so that the side is half way to the surface.
constexpr double LobeReach = 2.;
// Width of the morph between the patterns of two lobes, in their distance to the center over its depth.
constexpr double LobeMorph = 0.1;

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

// Scale of the pattern around the center at a radial coordinate t. The mean cell scale over the ball of radius t,
// t^-3 * integral of 3 t'^2 * target(t'), follows the gradient; beyond the surface the target is the surface scale.
class RadialScale
{
public:
    explicit RadialScale(const AdaptiveTpms &tpms)
    {
        const double ratio  = std::max(tpms.interior_frequency / tpms.surface_frequency, 1e-3);
        auto         target = [&tpms, ratio](double depth) {
            switch (tpms.gradient) {
            case TpmsAdaptiveGradient::Quadratic: return 1. + (ratio - 1.) * depth * depth;
            case TpmsAdaptiveGradient::Exponential: return std::pow(ratio, depth);
            default: return 1. + (ratio - 1.) * depth;
            }
        };
        m_scale[0]    = target(1.);
        double volume = 0.;
        for (size_t i = 1; i < m_scale.size(); ++i) {
            const double t0 = double(i - 1) / double(m_scale.size() - 1);
            const double t1 = double(i) / double(m_scale.size() - 1);
            volume += (t1 * t1 * t1 - t0 * t0 * t0) * target(1. - 0.5 * (t0 + t1));
            m_scale[i] = volume / (t1 * t1 * t1);
        }
    }

    double operator()(double t) const
    {
        if (t >= 1.)
            return (m_scale.back() + t * t * t - 1.) / (t * t * t);
        const double x = t * double(m_scale.size() - 1);
        const size_t i = std::min(size_t(x), m_scale.size() - 2);
        return m_scale[i] + (m_scale[i + 1] - m_scale[i]) * (x - double(i));
    }

private:
    std::array<double, 257> m_scale;
};

} // namespace

TpmsRadialField::TpmsRadialField(const std::vector<Slice> &slices, const BoundingBox &bbox, const std::function<void()> &throw_if_canceled)
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

    const size_t       sy = size_t(m_size.x());
    const size_t       sz = sy * m_size.y();
    std::vector<float> depth(sz * m_size.z(), 0.f);
    tbb::parallel_for(tbb::blocked_range<int>(0, m_size.z()), [&](const tbb::blocked_range<int> &range) {
        for (int k = range.begin(); k < range.end(); ++k) {
            const double z  = m_origin.z() + k * m_cell;
            auto         it = std::lower_bound(slices.begin(), slices.end(), z, [](const Slice &s, double z) { return s.top_z < z; });
            if (it != slices.end() && z > it->bottom_z)
                rasterize(*it->expolygons, m_origin.head<2>(), m_cell, m_size.x(), m_size.y(), depth.data() + k * sz);
        }
        throw_if_canceled();
    });
    for (int axis = 0; axis < 3; ++axis)
        distance_transform_axis(depth, m_size, axis, throw_if_canceled);

    auto position = [this, sy, sz](size_t i) {
        return Vec3d(m_origin + m_cell * Vec3d(double(i % sy), double(i / sy % m_size.y()), double(i / sz)));
    };
    const std::array<std::ptrdiff_t, 6> steps{1, -1, std::ptrdiff_t(sy), -std::ptrdiff_t(sy), std::ptrdiff_t(sz), -std::ptrdiff_t(sz)};

    auto node_of = [this, sy, sz](const Vec3d &pt) -> std::ptrdiff_t {
        const Vec3d f = (pt - m_origin) / m_cell;
        const long  x = std::lround(f.x()), y = std::lround(f.y()), z = std::lround(f.z());
        if (x < 0 || y < 0 || z < 0 || x >= m_size.x() || y >= m_size.y() || z >= m_size.z())
            return -1;
        return std::ptrdiff_t(size_t(z) * sz + size_t(y) * sy + size_t(x));
    };
    std::array<std::ptrdiff_t, 26> neighbours;
    for (int dz = -1, k = 0; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx != 0 || dy != 0 || dz != 0)
                    neighbours[k++] = std::ptrdiff_t(dz) * std::ptrdiff_t(sz) + std::ptrdiff_t(dy) * std::ptrdiff_t(sy) + dx;

    // Bodies are the connected inside nodes, none of which is on the border. The deepest nodes of a body are the
    // centers of its lobes, unless the depth between them stays above NeckRatio; where the depth ties, the center
    // is the node nearest to the middle of the tied nodes.
    m_body.assign(depth.size(), -1);
    std::vector<size_t> body_nodes;
    for (size_t seed = 0; seed < depth.size(); ++seed) {
        if (depth[seed] == 0.f || m_body[seed] >= 0)
            continue;
        const int id = int(m_bodies.size());
        body_nodes.assign(1, seed);
        m_body[seed]    = id;
        float max_depth = 0.f;
        for (size_t k = 0; k < body_nodes.size(); ++k) {
            max_depth = std::max(max_depth, depth[body_nodes[k]]);
            for (std::ptrdiff_t step : steps)
                if (const size_t j = body_nodes[k] + step; depth[j] > 0.f && m_body[j] < 0) {
                    m_body[j] = id;
                    body_nodes.push_back(j);
                }
        }

        std::vector<size_t> peaks;
        for (size_t i : body_nodes)
            if (depth[i] >= sqr(MinLobeRatio) * max_depth &&
                std::all_of(neighbours.begin(), neighbours.end(), [&](std::ptrdiff_t n) { return depth[i + n] <= depth[i]; }))
                peaks.push_back(i);
        std::sort(peaks.begin(), peaks.end(), [&depth](size_t a, size_t b) { return depth[a] > depth[b] || (depth[a] == depth[b] && a < b); });
        auto necked = [&](size_t a, size_t b) {
            const Vec3d  pa = position(a), pb = position(b);
            const double limit = sqr(NeckRatio) * std::min(depth[a], depth[b]);
            const int    samples = int(std::ceil((pb - pa).norm() / (0.5 * m_cell)));
            for (int s = 1; s < samples; ++s)
                if (depth[node_of(pa + (pb - pa) * (double(s) / samples))] < limit)
                    return true;
            return false;
        };
        std::vector<std::vector<size_t>> ties;
        for (size_t i : peaks) {
            auto lobe = std::find_if(ties.begin(), ties.end(), [&](const std::vector<size_t> &t) { return !necked(t.front(), i); });
            if (lobe == ties.end())
                ties.push_back({i});
            else if (std::sqrt(depth[i]) >= std::sqrt(depth[lobe->front()]) - 1.f)
                lobe->push_back(i);
        }
        m_bodies.push_back({m_lobes.size(), ties.size()});
        for (const std::vector<size_t> &tied : ties) {
            Vec3d middle(0., 0., 0.);
            for (size_t i : tied)
                middle += position(i);
            middle /= double(tied.size());
            Vec3d center = position(tied.front());
            for (size_t i : tied)
                if ((position(i) - middle).squaredNorm() < (center - middle).squaredNorm())
                    center = position(i);
            m_lobes.push_back({center, (std::sqrt(double(depth[tied.front()])) - 0.5) * m_cell, {}});
        }
    }
    throw_if_canceled();

    // The reach of a lobe is the first exit along each direction from its center, smoothed over the directions.
    // It is shortened towards a neighbouring lobe, from where the point is nearer to the other lobe relative to their depths.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_bodies.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t id = range.begin(); id < range.end(); ++id) {
            const Body &body = m_bodies[id];
            for (size_t l = body.first_lobe; l < body.first_lobe + body.lobes; ++l) {
                Lobe               &lobe = m_lobes[l];
                const double        step = 0.5 * m_cell;
                std::vector<double> log_reach(Polar * Azimuth);
                auto                nearest_lobe = [&](const Vec3d &pt) {
                    size_t nearest = l;
                    for (size_t k = body.first_lobe; k < body.first_lobe + body.lobes; ++k)
                        if ((pt - m_lobes[k].center).norm() / m_lobes[k].depth < (pt - m_lobes[nearest].center).norm() / m_lobes[nearest].depth)
                            nearest = k;
                    return nearest;
                };
                for (int i = 0; i < Polar; ++i)
                    for (int j = 0; j < Azimuth; ++j) {
                        const double polar   = (i + 0.5) * PI / Polar;
                        const double azimuth = j * 2. * PI / Azimuth;
                        const Vec3d  dir(std::sin(polar) * std::cos(azimuth), std::sin(polar) * std::sin(azimuth), std::cos(polar));
                        double       r     = 0.;
                        double       limit = InfD;
                        for (;;) {
                            const Vec3d          pt = lobe.center + (r + step) * dir;
                            const std::ptrdiff_t n  = node_of(pt);
                            if (n < 0 || depth[n] == 0.f || m_body[n] != int(id) || r + step >= limit)
                                break;
                            if (limit == InfD && body.lobes > 1 && nearest_lobe(pt) != l)
                                limit = LobeReach * (r + step);
                            r += step;
                        }
                        log_reach[i * Azimuth + j] = std::log(std::min(r + 0.5 * step, limit));
                    }
                for (int pass = 0; pass < 2; ++pass) {
                    std::vector<double> smoothed(log_reach.size(), 0.);
                    for (int i = 0; i < Polar; ++i)
                        for (int j = 0; j < Azimuth; ++j) {
                            for (int di = -1; di <= 1; ++di)
                                for (int dj = -1; dj <= 1; ++dj)
                                    smoothed[i * Azimuth + j] += log_reach[std::clamp(i + di, 0, Polar - 1) * Azimuth + (j + dj + Azimuth) % Azimuth];
                            smoothed[i * Azimuth + j] /= 9.;
                        }
                    log_reach = std::move(smoothed);
                }
                lobe.reach.resize(log_reach.size());
                std::transform(log_reach.begin(), log_reach.end(), lobe.reach.begin(), [](double v) { return float(std::exp(v)); });
            }
        }
        throw_if_canceled();
    });

    // Every other node belongs to its nearest body.
    std::deque<size_t> queue;
    for (size_t i = 0; i < m_body.size(); ++i)
        if (m_body[i] >= 0)
            queue.push_back(i);
    while (!queue.empty()) {
        const size_t i = queue.front();
        queue.pop_front();
        const size_t              x = i % sy, y = i / sy % m_size.y(), z = i / sz;
        const std::array<bool, 6> valid{x + 1 < sy, x > 0, y + 1 < size_t(m_size.y()), y > 0, z + 1 < size_t(m_size.z()), z > 0};
        for (size_t k = 0; k < steps.size(); ++k)
            if (valid[k] && m_body[i + steps[k]] < 0) {
                m_body[i + steps[k]] = m_body[i];
                queue.push_back(i + steps[k]);
            }
    }
}

double TpmsRadialField::radial(const Lobe &lobe, const Vec3d &pt) const
{
    const Vec3d  d = pt - lobe.center;
    const double r = d.norm();
    if (r < EPSILON)
        return 0.;
    const double polar   = std::clamp(std::acos(std::clamp(d.z() / r, -1., 1.)) / PI * Polar - 0.5, 0., double(Polar - 1));
    const int    i       = std::min(int(polar), Polar - 2);
    const double fi      = polar - i;
    double       azimuth = std::atan2(d.y(), d.x()) / (2. * PI) * Azimuth;
    if (azimuth < 0.)
        azimuth += Azimuth;
    const int    j0    = int(azimuth) % Azimuth;
    const int    j1    = (j0 + 1) % Azimuth;
    const double fj    = azimuth - std::floor(azimuth);
    auto         at    = [&lobe](int i, int j) { return double(lobe.reach[i * Azimuth + j]); };
    const double reach = (at(i, j0) * (1. - fj) + at(i, j1) * fj) * (1. - fi) + (at(i + 1, j0) * (1. - fj) + at(i + 1, j1) * fj) * fi;
    return r / reach;
}

size_t TpmsRadialField::radial(const Vec3d &pt, std::array<Radial, 2> &out) const
{
    if (m_bodies.empty()) {
        out[0] = {pt, 1., 1.f};
        return 1;
    }
    size_t node = 0;
    for (int axis = 2; axis >= 0; --axis)
        node = node * m_size[axis] + size_t(std::clamp<long>(std::lround((pt[axis] - m_origin[axis]) / m_cell), 0, m_size[axis] - 1));
    const Body &body = m_bodies[m_body[node]];
    if (body.lobes == 1) {
        const Lobe &lobe = m_lobes[body.first_lobe];
        out[0]           = {lobe.center, radial(lobe, pt), 1.f};
        return 1;
    }

    // The two lobes nearest relative to their depth; morph between them near the side where they are as near.
    size_t first = body.first_lobe, second = body.first_lobe + 1;
    auto   distance = [this, &pt](size_t l) { return (pt - m_lobes[l].center).norm() / m_lobes[l].depth; };
    if (distance(second) < distance(first))
        std::swap(first, second);
    for (size_t l = body.first_lobe + 2; l < body.first_lobe + body.lobes; ++l)
        if (distance(l) < distance(first)) {
            second = first;
            first  = l;
        } else if (distance(l) < distance(second))
            second = l;
    const double u      = std::clamp(0.5 - (distance(second) - distance(first)) / LobeMorph, 0., 1.);
    const double s      = u * u * (3. - 2. * u);
    const float  weight = float(s / (0.5 + s));
    out[0]              = {m_lobes[first].center, radial(m_lobes[first], pt), 1.f - weight};
    if (weight == 0.f)
        return 1;
    out[1] = {m_lobes[second].center, radial(m_lobes[second], pt), weight};
    return 2;
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

    const AdaptiveTpms    &tpms;
    const TpmsRadialField &radial_field;
    RadialScale            scale;
    Point                  size;
    Point                  offs;
    double                 z;
    double                 cos_angle;
    double                 sin_angle;

    AdaptiveTpmsField(const AdaptiveTpms &tpms, const TpmsRadialField &radial_field, const BoundingBox &bbox, coordf_t z, float angle)
        : tpms(tpms), radial_field(radial_field), scale(tpms), size(bbox.size()), offs(bbox.min), z(z)
        , cos_angle(std::cos(angle)), sin_angle(std::sin(angle))
    {}

    // The pattern is scaled around the center of the lobe, morphing into the pattern of a neighbouring lobe near the
    // side between them. The radial field is in the object frame, the fill is rotated by -angle.
    float get_scalar(const Coord &p) const
    {
        const Point                            pt = to_Point(p);
        const double                           x  = unscaled(pt.x());
        const double                           y  = unscaled(pt.y());
        std::array<TpmsRadialField::Radial, 2> radials;
        const size_t count = radial_field.radial(Vec3d(cos_angle * x - sin_angle * y, sin_angle * x + cos_angle * y, z), radials);
        float        value = 0.f;
        for (size_t i = 0; i < count; ++i) {
            const auto  &[center, t, weight] = radials[i];
            const double frequency           = tpms.surface_frequency * scale(t);
            const double cx                  = cos_angle * center.x() + sin_angle * center.y();
            const double cy                  = cos_angle * center.y() - sin_angle * center.x();
            value += weight * tpms.equation(float(frequency * (x - cx)), float(frequency * (y - cy)), float(frequency * (z - center.z())));
        }
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

Polylines make_adaptive_tpms(const AdaptiveTpms &tpms, const TpmsRadialField &field, BoundingBox bbox,
                             coordf_t z, coordf_t layer_height, coordf_t spacing, float angle)
{
    // A cell of margin for the rings closed along the raster border, and a fixed sampling grid for every region.
    const coord_t cell = scaled(marchsq::AdaptiveTpmsField::gsizef);
    bbox.offset(cell);
    bbox.merge(align_to_grid(bbox.min, Point(cell, cell)));
    const marchsq::AdaptiveTpmsField raster(tpms, field, bbox, z - 0.5 * layer_height, angle);
    const std::vector<marchsq::Ring> rings = marchsq::execute_with_policy(ex_tbb, raster, 0.f, {raster.gsize, raster.gsize});

    // Loops narrower than two lines print as blobs.
    const double min_loop_length = scaled(2. * PI * spacing);
    Polylines    polylines;
    polylines.reserve(rings.size());
    for (const marchsq::Ring &ring : rings) {
        Polyline polyline;
        polyline.points.reserve(ring.size() + 1);
        for (const marchsq::Coord &crd : ring)
            polyline.points.emplace_back(raster.to_Point(crd));
        polyline.points.push_back(polyline.points.front());
        polyline.simplify(SCALED_SPARSE_INFILL_RESOLUTION);
        if (polyline.length() >= min_loop_length)
            polylines.push_back(std::move(polyline));
    }
    return polylines;
}

} // namespace Slic3r
