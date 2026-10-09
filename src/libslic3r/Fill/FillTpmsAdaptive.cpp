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
#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "FillBase.hpp"
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

// Two deepest points are in separate lobes when the depth between them drops below this ratio of the shallower one.
constexpr double NeckRatio = 0.8;
// Lobes shallower than this ratio of the deepest one of their body are graded as part of it.
constexpr double MinLobeRatio = 0.3;
// A lobe reaches twice as far as the side towards its neighbour, so that the side is half way to the surface.
constexpr double LobeReach = 2.;
// Width of the morph between the patterns of two lobes, in their distance to the center over its depth.
constexpr double LobeMorph = 0.1;
// Densities of the levels of Stepped shells, and of Smooth blend, are at most these ratios apart. Fewer levels blend
// with fewer lines running along the blends.
constexpr double ShellRatio = 1.5;
constexpr double BlendRatio = 2.5;
// Distance warp: samples of the mean depth along a ray.
constexpr int Samples = 32;

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

// Scale of the pattern relative to the surface at a depth from 0 at the surface to 1 at the deepest point.
double target_scale(double ratio, TpmsAdaptiveGradient gradient, double depth)
{
    switch (gradient) {
    case TpmsAdaptiveGradient::Quadratic: return 1. + (ratio - 1.) * depth * depth;
    case TpmsAdaptiveGradient::Exponential: return std::pow(ratio, depth);
    default: return 1. + (ratio - 1.) * depth;
    }
}

// Levels of Stepped shells and Smooth blend, geometric from the surface at 0 to the interior at count, and the
// continuous level of the target of a depth.
struct DensityLevels
{
    DensityLevels(double ratio, double step, TpmsAdaptiveGradient gradient)
        : ratio(ratio), gradient(gradient), count(int(std::ceil(std::abs(std::log(ratio)) / std::log(step) - EPSILON)))
    {}

    double scale(int level) const { return count == 0 ? 1. : std::pow(ratio, double(level) / count); }
    double level(double depth) const
    {
        return count == 0 ? 0. : count * std::log(target_scale(ratio, gradient, depth)) / std::log(ratio);
    }

    double               ratio;
    TpmsAdaptiveGradient gradient;
    int                  count;
};

// Scale of the pattern around the center at a radial coordinate t. The mean cell scale over the ball of radius t,
// t^-3 * integral of 3 t'^2 * target(t'), or over the disc in 2D, follows the gradient; beyond the surface the target
// is the surface scale.
class RadialScale
{
public:
    RadialScale(const AdaptiveTpms &tpms, int dimensions) : m_dimensions(dimensions)
    {
        const double ratio  = std::max(tpms.interior_frequency / tpms.surface_frequency, 1e-3);
        auto         target = [&tpms, ratio](double depth) { return target_scale(ratio, tpms.gradient, depth); };
        m_scale[0]    = target(1.);
        double volume = 0.;
        for (size_t i = 1; i < m_scale.size(); ++i) {
            const double t0 = double(i - 1) / double(m_scale.size() - 1);
            const double t1 = double(i) / double(m_scale.size() - 1);
            volume += (std::pow(t1, m_dimensions) - std::pow(t0, m_dimensions)) * target(1. - 0.5 * (t0 + t1));
            m_scale[i] = volume / std::pow(t1, m_dimensions);
        }
    }

    double operator()(double t) const
    {
        if (t >= 1.) {
            const double volume = std::pow(t, m_dimensions);
            return (m_scale.back() + volume - 1.) / volume;
        }
        const double x = t * double(m_scale.size() - 1);
        const size_t i = std::min(size_t(x), m_scale.size() - 2);
        return m_scale[i] + (m_scale[i + 1] - m_scale[i]) * (x - double(i));
    }

private:
    int                     m_dimensions;
    std::array<double, 257> m_scale;
};

} // namespace

TpmsRadialField::TpmsRadialField(const std::vector<Slice> &slices, const BoundingBox &bbox, TpmsAdaptiveMode mode,
                                 const std::function<void()> &throw_if_canceled)
    : m_mode(mode)
    , m_axis(mode == TpmsAdaptiveMode::NormalX ? 0 : mode == TpmsAdaptiveMode::NormalY ? 1 : mode == TpmsAdaptiveMode::NormalZ ? 2 : -1)
{
    assert(!slices.empty() && mode != TpmsAdaptiveMode::Disabled);
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
        if (axis != m_axis)
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
    // In the 2D modes, the steps within a section.
    auto in_section = [this](size_t step) { return int(step / 2) != m_axis; };
    std::vector<std::ptrdiff_t> neighbours;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx != 0 || dy != 0 || dz != 0) && (m_axis < 0 || Vec3i32(dx, dy, dz)[m_axis] == 0))
                    neighbours.push_back(std::ptrdiff_t(dz) * std::ptrdiff_t(sz) + std::ptrdiff_t(dy) * std::ptrdiff_t(sy) + dx);

    // Bodies are the connected inside nodes, none of which is on the border. The deepest nodes of a body are the
    // centers of its lobes, unless the depth between them stays above NeckRatio; where the depth ties, the center
    // is the node nearest to the middle of the tied nodes.
    const bool lobes = m_mode != TpmsAdaptiveMode::SteppedShells && m_mode != TpmsAdaptiveMode::SmoothBlend;
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
            for (size_t s = 0; s < steps.size(); ++s)
                if (const size_t j = body_nodes[k] + steps[s]; in_section(s) && depth[j] > 0.f && m_body[j] < 0) {
                    m_body[j] = id;
                    body_nodes.push_back(j);
                }
        }
        m_bodies.push_back({m_lobes.size(), 0, (std::sqrt(double(max_depth)) - 0.5) * m_cell});
        if (!lobes)
            continue;

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
        // A peak joins the first lobe it sees without a neck, if it is as deep. The lobes are made one at a time,
        // from the first remaining peak, testing the others in parallel.
        std::vector<std::vector<size_t>> ties;
        while (!peaks.empty()) {
            const size_t      front = peaks.front();
            std::vector<char> joins(peaks.size(), 0);
            tbb::parallel_for(tbb::blocked_range<size_t>(1, peaks.size()), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t k = range.begin(); k < range.end(); ++k)
                    if (!necked(front, peaks[k]))
                        joins[k] = std::sqrt(depth[peaks[k]]) >= std::sqrt(depth[front]) - 1.f ? 1 : 2;
            });
            std::vector<size_t> &tied = ties.emplace_back(1, front);
            std::vector<size_t>  remaining;
            for (size_t k = 1; k < peaks.size(); ++k)
                if (joins[k] == 0)
                    remaining.push_back(peaks[k]);
                else if (joins[k] == 1)
                    tied.push_back(peaks[k]);
            peaks = std::move(remaining);
        }
        m_bodies.back().lobes = ties.size();
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

    if (m_mode == TpmsAdaptiveMode::SteppedShells || m_mode == TpmsAdaptiveMode::SmoothBlend || m_mode == TpmsAdaptiveMode::DistanceWarp) {
        m_depth.assign(depth.size(), 0.f);
        for (size_t i = 0; i < depth.size(); ++i)
            if (m_body[i] >= 0)
                m_depth[i] = float(std::min(1., std::max(0., std::sqrt(double(depth[i])) - 0.5) * m_cell / m_bodies[m_body[i]].depth));
    }

    // The reach of a lobe is the first exit along each direction from its center, smoothed over the directions.
    // It is shortened towards a neighbouring lobe, from where the point is nearer to the other lobe relative to their depths.
    std::vector<int> lobe_body(m_lobes.size());
    for (size_t id = 0; id < m_bodies.size(); ++id)
        std::fill_n(lobe_body.begin() + m_bodies[id].first_lobe, m_bodies[id].lobes, int(id));
    const int rows      = this->directions() / Azimuth;
    auto      direction = [this](int i, int j) {
        const double azimuth = j * 2. * PI / Azimuth;
        Vec3d        dir     = Vec3d::Zero();
        if (m_axis < 0) {
            const double polar = (i + 0.5) * PI / Polar;
            dir = Vec3d(std::sin(polar) * std::cos(azimuth), std::sin(polar) * std::sin(azimuth), std::cos(polar));
        } else {
            dir[(m_axis + 1) % 3] = std::cos(azimuth);
            dir[(m_axis + 2) % 3] = std::sin(azimuth);
        }
        return dir;
    };
    // Two passes of a box filter over the neighbouring directions, for each of the values of a direction.
    auto smooth = [rows](std::vector<double> &values, size_t count) {
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<double> smoothed(values.size(), 0.);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < Azimuth; ++j)
                    for (size_t k = 0; k < count; ++k) {
                        double &sum = smoothed[size_t(i * Azimuth + j) * count + k];
                        for (int di = -1; di <= 1; ++di)
                            for (int dj = -1; dj <= 1; ++dj)
                                sum += values[size_t(std::clamp(i + di, 0, rows - 1) * Azimuth + (j + dj + Azimuth) % Azimuth) * count + k];
                        sum /= 9.;
                    }
            values = std::move(smoothed);
        }
    };
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_lobes.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            const int   id   = lobe_body[l];
            const Body &body = m_bodies[id];
            Lobe       &lobe = m_lobes[l];
            // The other lobes of the body, by the distance from the center beyond which they may be nearer.
            std::vector<std::pair<double, size_t>> others;
            for (size_t k = body.first_lobe; k < body.first_lobe + body.lobes; ++k)
                if (k != l)
                    others.emplace_back((m_lobes[k].center - lobe.center).norm() / (1. + m_lobes[k].depth / lobe.depth), k);
            std::sort(others.begin(), others.end());
            auto nearer_lobe = [&](const Vec3d &pt, double r) {
                for (auto it = others.begin(); it != others.end() && it->first < r; ++it)
                    if ((pt - m_lobes[it->second].center).norm() / m_lobes[it->second].depth < r / lobe.depth)
                        return true;
                return false;
            };
            const double        step = 0.5 * m_cell;
            std::vector<double> log_reach(this->directions());
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < Azimuth; ++j) {
                    const Vec3d dir   = direction(i, j);
                    double      r     = 0.;
                    double      limit = InfD;
                    for (;;) {
                        const Vec3d          pt = lobe.center + (r + step) * dir;
                        const std::ptrdiff_t n  = node_of(pt);
                        if (n < 0 || depth[n] == 0.f || m_body[n] != id || r + step >= limit)
                            break;
                        if (limit == InfD && nearer_lobe(pt, r + step))
                            limit = LobeReach * (r + step);
                        r += step;
                    }
                    log_reach[i * Azimuth + j] = std::log(std::min(r + 0.5 * step, limit));
                }
            smooth(log_reach, 1);
            lobe.reach.resize(log_reach.size());
            std::transform(log_reach.begin(), log_reach.end(), lobe.reach.begin(), [](double v) { return float(std::exp(v)); });

            if (m_mode == TpmsAdaptiveMode::DistanceWarp) {
                std::vector<double> mean(log_reach.size() * (Samples + 1));
                for (size_t d = 0; d < log_reach.size(); ++d) {
                    const Vec3d dir         = direction(int(d) / Azimuth, int(d) % Azimuth);
                    double      integral    = 0.;
                    double      previous    = this->depth(lobe.center);
                    mean[d * (Samples + 1)] = previous;
                    for (int k = 1; k <= Samples; ++k) {
                        // Exact for a depth linear between the samples, a + b * tau, weighted by tau^2.
                        const double t0 = double(k - 1) / Samples;
                        const double t1 = double(k) / Samples;
                        const double d1 = this->depth(lobe.center + t1 * lobe.reach[d] * dir);
                        const double b  = (d1 - previous) * Samples;
                        const double a  = previous - b * t0;
                        integral += a * (std::pow(t1, 3) - std::pow(t0, 3)) / 3. + b * (std::pow(t1, 4) - std::pow(t0, 4)) / 4.;
                        previous                    = d1;
                        mean[d * (Samples + 1) + k] = 3. * integral / std::pow(t1, 3);
                    }
                }
                // Smoothed like the reach: sharper profiles shear the pattern across the layer, adding lines.
                smooth(mean, Samples + 1);
                lobe.mean_depth.assign(mean.begin(), mean.end());
            }
        }
        throw_if_canceled();
    });

    // Every other node belongs to its nearest body, within its section in the 2D modes.
    if (m_axis >= 0) {
        std::vector<bool> has_body(m_size[m_axis], false);
        for (size_t i = 0; i < m_body.size(); ++i)
            if (m_body[i] >= 0)
                has_body[m_axis == 0 ? i % sy : m_axis == 1 ? i / sy % m_size.y() : i / sz] = true;
        m_section.assign(m_size[m_axis], -1);
        for (int k = 0; k < m_size[m_axis]; ++k)
            for (int d = 0; d < m_size[m_axis] && m_section[k] < 0; ++d)
                if (k - d >= 0 && has_body[k - d])
                    m_section[k] = k - d;
                else if (k + d < m_size[m_axis] && has_body[k + d])
                    m_section[k] = k + d;
    }
    // Stepped shells and Smooth blend only look up the depth.
    if (!lobes) {
        m_body = {};
        return;
    }
    if (m_bodies.size() == 1) {
        std::fill(m_body.begin(), m_body.end(), 0);
        return;
    }
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
            if (valid[k] && in_section(k) && m_body[i + steps[k]] < 0) {
                m_body[i + steps[k]] = m_body[i];
                queue.push_back(i + steps[k]);
            }
    }
}

double TpmsRadialField::depth(const Vec3d &pt) const
{
    assert(!m_depth.empty());
    const Vec3d           f = (pt - m_origin) / m_cell;
    std::array<int, 3>    n0;
    std::array<double, 3> w;
    for (int a = 0; a < 3; ++a) {
        n0[a] = std::clamp(int(std::floor(f[a])), 0, m_size[a] - 2);
        w[a]  = std::clamp(f[a] - n0[a], 0., 1.);
    }
    const size_t sy    = size_t(m_size.x());
    const size_t sz    = sy * size_t(m_size.y());
    double       value = 0.;
    for (int c = 0; c < 8; ++c) {
        const size_t n = size_t(n0[2] + (c >> 2)) * sz + size_t(n0[1] + (c >> 1 & 1)) * sy + size_t(n0[0] + (c & 1));
        value += (c & 1 ? w[0] : 1. - w[0]) * (c >> 1 & 1 ? w[1] : 1. - w[1]) * (c >> 2 ? w[2] : 1. - w[2]) * m_depth[n];
    }
    return value;
}

Vec3d TpmsRadialField::offset(const Vec3d &pt, const Vec3d &center) const
{
    Vec3d d = pt - center;
    if (m_axis >= 0)
        d[m_axis] = 0.;
    return d;
}

double TpmsRadialField::radial(const Lobe &lobe, const Vec3d &pt) const
{
    const Vec3d  d = this->offset(pt, lobe.center);
    const double r = d.norm();
    // Distance warp: the radial coordinate of the linear profile with the same mean depth, which is 1 - 3/4 of it.
    auto warp = [](double mean_depth) { return std::max(0., 4. / 3. * (1. - mean_depth)); };
    if (r < EPSILON)
        return lobe.mean_depth.empty() ? 0. : warp(lobe.mean_depth.front());
    int    i  = 0;
    double fi = 0.;
    double azimuth;
    if (m_axis < 0) {
        const double polar = std::clamp(std::acos(std::clamp(d.z() / r, -1., 1.)) / PI * Polar - 0.5, 0., double(Polar - 1));
        i       = std::min(int(polar), Polar - 2);
        fi      = polar - i;
        azimuth = std::atan2(d.y(), d.x());
    } else
        azimuth = std::atan2(d[(m_axis + 2) % 3], d[(m_axis + 1) % 3]);
    azimuth *= Azimuth / (2. * PI);
    if (azimuth < 0.)
        azimuth += Azimuth;
    const int    j0    = int(azimuth) % Azimuth;
    const int    j1    = (j0 + 1) % Azimuth;
    const double fj    = azimuth - std::floor(azimuth);
    auto         at    = [&lobe](int i, int j) { return double(lobe.reach[i * Azimuth + j]); };
    double       reach = at(i, j0) * (1. - fj) + at(i, j1) * fj;
    if (fi > 0.)
        reach = reach * (1. - fi) + (at(i + 1, j0) * (1. - fj) + at(i + 1, j1) * fj) * fi;
    const double tau = r / reach;
    if (lobe.mean_depth.empty())
        return tau;
    // Beyond the surface, the depth is zero.
    auto mean_at = [&lobe, tau](int i, int j) {
        const float *mean = lobe.mean_depth.data() + size_t(i * Azimuth + j) * (Samples + 1);
        if (tau >= 1.)
            return double(mean[Samples]) / (tau * tau * tau);
        const double x = tau * Samples;
        const int    k = std::min(int(x), Samples - 1);
        return mean[k] + (mean[k + 1] - mean[k]) * (x - k);
    };
    double mean = mean_at(i, j0) * (1. - fj) + mean_at(i, j1) * fj;
    if (fi > 0.)
        mean = mean * (1. - fi) + (mean_at(i + 1, j0) * (1. - fj) + mean_at(i + 1, j1) * fj) * fi;
    return warp(mean);
}

size_t TpmsRadialField::radial(const Vec3d &pt, Radials &out) const
{
    assert(!this->empty());
    Vec3i32 idx;
    for (int axis = 0; axis < 3; ++axis)
        idx[axis] = std::clamp<int>(int(std::lround((pt[axis] - m_origin[axis]) / m_cell)), 0, m_size[axis] - 1);
    auto node = [this](const Vec3i32 &idx) { return (size_t(idx.z()) * m_size.y() + idx.y()) * m_size.x() + idx.x(); };
    if (m_axis < 0)
        return this->body_radial(node(idx), pt, 1.f, out.data());

    // The sections around pt, or the nearest ones with a body.
    const double f     = std::clamp((pt[m_axis] - m_origin[m_axis]) / m_cell, 0., double(m_size[m_axis] - 1));
    const int    k     = std::min(int(f), m_size[m_axis] - 2);
    const float  w     = float(f - k);
    size_t       count = 0;
    if (w < 1.f) {
        idx[m_axis] = m_section[k];
        count += this->body_radial(node(idx), pt, 1.f - w, out.data());
    }
    if (w > 0.f) {
        idx[m_axis] = m_section[k + 1];
        count += this->body_radial(node(idx), pt, w, out.data() + count);
    }
    return count;
}

size_t TpmsRadialField::body_radial(size_t node, const Vec3d &pt, float weight, Radial *out) const
{
    const Body &body = m_bodies[m_body[node]];
    if (body.lobes == 1) {
        const Lobe &lobe = m_lobes[body.first_lobe];
        out[0]           = {lobe.center, radial(lobe, pt), weight};
        return 1;
    }

    // The lobes nearest relative to their depth; they morph into each other near the sides where they are as near.
    std::array<std::pair<double, size_t>, MaxMorph> nearest;
    size_t                                          count = 0;
    for (size_t l = body.first_lobe; l < body.first_lobe + body.lobes; ++l) {
        const double d = this->offset(pt, m_lobes[l].center).norm() / m_lobes[l].depth;
        if (count < MaxMorph)
            nearest[count++] = {d, l};
        else if (d < nearest.back().first)
            nearest.back() = {d, l};
        else
            continue;
        for (size_t k = count - 1; k > 0 && nearest[k].first < nearest[k - 1].first; --k)
            std::swap(nearest[k], nearest[k - 1]);
    }
    std::array<double, MaxMorph> blend;
    double                       total  = 0.;
    size_t                       morphs = 0;
    for (; morphs < count; ++morphs) {
        const double u = 0.5 - (nearest[morphs].first - nearest[0].first) / LobeMorph;
        if (u <= 0.)
            break;
        blend[morphs] = u * u * (3. - 2. * u);
        total += blend[morphs];
    }
    for (size_t k = 0; k < morphs; ++k) {
        const Lobe &lobe = m_lobes[nearest[k].second];
        out[k]           = {lobe.center, radial(lobe, pt), float(weight * blend[k] / total)};
    }
    return morphs;
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
    DensityLevels          levels;
    Point                  size;
    Point                  offs;
    double                 z;
    double                 cos_angle;
    double                 sin_angle;

    AdaptiveTpmsField(const AdaptiveTpms &tpms, const TpmsRadialField &radial_field, const BoundingBox &bbox, coordf_t z, float angle)
        : tpms(tpms), radial_field(radial_field), scale(tpms, radial_field.axis() < 0 ? 3 : 2)
        , levels(std::max(tpms.interior_frequency / tpms.surface_frequency, 1e-3), BlendRatio, tpms.gradient)
        , size(bbox.size()), offs(bbox.min), z(z)
        , cos_angle(std::cos(angle)), sin_angle(std::sin(angle))
    {}

    // The pattern is scaled around the center of the lobe, morphing into the pattern of a neighbouring lobe near the
    // side between them. In the 2D modes only within the section, with the interior frequency along the axis.
    // The radial field is in the object frame, the fill is rotated by -angle.
    float get_scalar(const Coord &p) const
    {
        const Point                            pt = to_Point(p);
        const double                           x  = unscaled(pt.x());
        const double                           y  = unscaled(pt.y());
        const Vec3d                            obj(cos_angle * x - sin_angle * y, sin_angle * x + cos_angle * y, z);
        if (radial_field.mode() == TpmsAdaptiveMode::SmoothBlend) {
            // The regular patterns of the two levels around the target of the depth, blended by a smoothstep.
            auto lattice = [this, x, y](int level) {
                const double frequency = tpms.surface_frequency * levels.scale(level);
                return tpms.equation(float(frequency * x), float(frequency * y), float(frequency * z));
            };
            if (levels.count == 0)
                return lattice(0);
            const double c = std::clamp(levels.level(radial_field.depth(obj)), 0., double(levels.count));
            const int    k = std::min(int(c), levels.count - 1);
            const double u = c - k;
            const double w = u * u * (3. - 2. * u);
            return float((w < 1. ? (1. - w) * lattice(k) : 0.) + (w > 0. ? w * lattice(k + 1) : 0.));
        }
        const int                              axis = radial_field.axis();
        TpmsRadialField::Radials               radials;
        const size_t                           count = radial_field.radial(obj, radials);
        float                                  value = 0.f;
        for (size_t i = 0; i < count; ++i) {
            const auto &[center, t, weight] = radials[i];
            Vec3d q                         = tpms.surface_frequency * scale(t) * (obj - center);
            if (axis >= 0)
                q[axis] = tpms.interior_frequency * obj[axis];
            value += weight * tpms.equation(float(cos_angle * q.x() + sin_angle * q.y()), float(cos_angle * q.y() - sin_angle * q.x()), float(q.z()));
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

// Continuous density level of the depth over a layer, in the object frame.
struct TpmsLevelField
{
    static constexpr float gsizef = 0.5f;
    static constexpr float rsizef = 0.05f;
    const coord_t          rsize  = scaled(rsizef);
    const long             gsize  = std::lround(gsizef / rsizef);

    const TpmsRadialField &field;
    const DensityLevels   &levels;
    Point                  size;
    Point                  offs;
    double                 z;

    TpmsLevelField(const TpmsRadialField &field, const DensityLevels &levels, const BoundingBox &bbox, coordf_t z)
        : field(field), levels(levels), size(bbox.size()), offs(bbox.min), z(z)
    {}

    float get_scalar(const Coord &p) const
    {
        const Point pt = to_Point(p);
        return float(levels.level(field.depth(Vec3d(unscaled(pt.x()), unscaled(pt.y()), z))));
    }

    inline coord_t to_coord(long x) const { return x * rsize; }
    inline long    to_coordr(coord_t x) const { return x / rsize; }
    inline Point   to_Point(const Coord &p) const { return Point(to_coord(p.c) + offs.x(), to_coord(p.r) + offs.y()); }
};

template<> struct _RasterTraits<TpmsLevelField>
{
    using ValueType = float;
    static float  get(const TpmsLevelField &sf, size_t row, size_t col) { return sf.get_scalar(Coord(long(row), long(col))); }
    static size_t rows(const TpmsLevelField &sf) { return sf.to_coordr(sf.size.y()); }
    static size_t cols(const TpmsLevelField &sf) { return sf.to_coordr(sf.size.x()); }
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

std::vector<TpmsShell> make_tpms_shells(const TpmsRadialField &field, const ExPolygon &expolygon, coordf_t z,
                                        float surface_density, float interior_density, TpmsAdaptiveGradient gradient)
{
    const DensityLevels levels(interior_density / surface_density, ShellRatio, gradient);
    if (levels.count == 0)
        return {{surface_density, {expolygon}}};
    // A fixed sampling grid, so that every region of a layer gets the same shells.
    const coord_t cell = scaled(marchsq::TpmsLevelField::gsizef);
    BoundingBox   bbox = get_extents(expolygon);
    bbox.offset(cell);
    bbox.merge(align_to_grid(bbox.min, Point(cell, cell)));
    const marchsq::TpmsLevelField raster(field, levels, bbox, z);

    // Each level takes the part deeper than the middle between it and the previous one.
    std::vector<TpmsShell> shells;
    ExPolygons             remaining{expolygon};
    for (int level = 0; level < levels.count && !remaining.empty(); ++level) {
        Polygons deeper;
        for (const marchsq::Ring &ring : marchsq::execute_with_policy(ex_tbb, raster, float(level + 0.5), {raster.gsize, raster.gsize})) {
            Polygon &polygon = deeper.emplace_back();
            polygon.points.reserve(ring.size());
            for (const marchsq::Coord &crd : ring)
                polygon.points.emplace_back(raster.to_Point(crd));
        }
        ExPolygons inner = intersection_ex(union_ex(deeper), remaining);
        shells.push_back({float(surface_density * levels.scale(level)), diff_ex(remaining, inner)});
        remaining = std::move(inner);
    }
    if (!remaining.empty())
        shells.push_back({interior_density, std::move(remaining)});
    return shells;
}

void fill_tpms_shells(const TpmsRadialField &field, const ExPolygon &expolygon, coordf_t z, const FillParams &params, coordf_t spacing,
                      const std::function<void(const FillParams &, const ExPolygon &)> &fill_shell)
{
    FillParams shell_params    = params;
    shell_params.tpms_adaptive = TpmsAdaptiveMode::Disabled;
    for (const TpmsShell &shell : make_tpms_shells(field, expolygon, z, params.density, params.tpms_interior_density, params.tpms_adaptive_gradient)) {
        shell_params.density = shell.density;
        for (const ExPolygon &part : offset_ex(shell.expolygons, -float(scale_(0.5 * spacing))))
            fill_shell(shell_params, part);
    }
}

} // namespace Slic3r
