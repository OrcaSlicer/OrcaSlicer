#include "FillBone.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../DistanceTransform.hpp"
#include "../ExPolygon.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../LocalesUtils.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"
#include "FillBase.hpp"

namespace Slic3r {

namespace {

// Wall area per volume of the foam, as a factor of line width over cell spacing: a target density
// rho gives cells spaced line_width * FoamWallFactor / rho apart.
constexpr double FoamWallFactor = 2.0;
// How fast a closing cavity may shrink per mm of height, relative to the overhang allowance, so the
// cut wall ends can grow back inwards: the cavity gets a pointed roof instead of a flat ceiling.
constexpr double CavityCloseRate = 0.4;
constexpr double CavityLookAhead = 16.;
constexpr double CavityLookStep  = 0.8;
// Supported wall pieces shorter than this that don't reach a junction are dropped.
constexpr double MinWallPiece = 0.3;
// Cells along the edge of the hollow core, whose walls are cut away, are kept this small relative to the
// smallest cells, so that they don't reach into the dense band around it.
constexpr double CavityCellFactor = 1.5;

struct Segment
{
    Vec2d a;
    Vec2d b;
};
using Segments = std::vector<Segment>;

double smoothstep(double t) { return t * t * (3. - 2. * t); }

// Random numbers from std::mt19937_64, whose sequence the standard fixes, rather than from the standard
// distributions or std::shuffle, whose algorithms are left to the library: the infill is the same on every
// platform.
class Random
{
public:
    explicit Random(uint64_t seed) : m_engine(seed) {}

    // In [0, 1).
    double uniform() { return double(m_engine() >> 11) * 0x1.0p-53; }
    // In [0, n).
    size_t below(size_t n) { return size_t(uniform() * double(n)); }

private:
    std::mt19937_64 m_engine;
};

// Smooth value noise in [0, 1).
uint32_t hash3(int i, int j, int k, int seed)
{
    uint32_t h = uint32_t(i) * 374761393u ^ uint32_t(j) * 668265263u ^ uint32_t(k) * 2147483647u ^ uint32_t(seed) * 1274126177u;
    h          = (h ^ (h >> 13)) * 1103515245u;
    return h ^ (h >> 16);
}

double noise3(double x, double y, double z, int seed)
{
    const double xf = std::floor(x), yf = std::floor(y), zf = std::floor(z);
    const int    xi = int(xf), yi = int(yf), zi = int(zf);
    const double u = smoothstep(x - xf), v = smoothstep(y - yf), w = smoothstep(z - zf);
    auto corner = [&](int dx, int dy, int dz) { return double(hash3(xi + dx, yi + dy, zi + dz, seed)) / 4294967296.; };
    auto lerp   = [](double a, double b, double t) { return a + (b - a) * t; };
    return lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u), lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u), lerp(corner(0, 1, 1), corner(1, 1, 1), u), v), w);
}

// ---------------------------------------------------------------------------------------------
// Signed distance to the surface of the object, on a voxel grid built from the layer outlines.

struct DistanceGrid
{
    Vec3d              lo;
    double             cell { 1. };
    int                nx { 0 }, ny { 0 }, nz { 0 };
    std::vector<float> v;

    // Trilinear; beyond the grid, the distance to it is added.
    double sample(double x, double y, double z) const
    {
        const double cx = std::clamp(x, lo.x(), lo.x() + (nx - 1) * cell - 1e-9);
        const double cy = std::clamp(y, lo.y(), lo.y() + (ny - 1) * cell - 1e-9);
        const double cz = std::clamp(z, lo.z(), lo.z() + (nz - 1) * cell - 1e-9);
        const double fx = (cx - lo.x()) / cell, fy = (cy - lo.y()) / cell, fz = (cz - lo.z()) / cell;
        const int    i = int(fx), j = int(fy), k = int(fz);
        const double u = fx - i, w = fy - j, t = fz - k;
        const size_t sy = size_t(nx), sz = size_t(nx) * ny, b = size_t(k) * sz + size_t(j) * sy + i;
        const double c00 = v[b] + (v[b + 1] - v[b]) * u, c10 = v[b + sy] + (v[b + sy + 1] - v[b + sy]) * u;
        const double c01 = v[b + sz] + (v[b + sz + 1] - v[b + sz]) * u;
        const double c11 = v[b + sz + sy] + (v[b + sz + sy + 1] - v[b + sz + sy]) * u;
        const double c0 = c00 + (c10 - c00) * w, c1 = c01 + (c11 - c01) * w;
        return c0 + (c1 - c0) * t + std::sqrt(sqr(x - cx) + sqr(y - cy) + sqr(z - cz));
    }
};

DistanceGrid build_distance_grid(const std::vector<BoneLayer> &layers, const BoundingBoxf &bbox, const std::function<void()> &throw_if_canceled)
{
    constexpr double margin = 4., above = CavityLookAhead + 2.;
    constexpr float  inf    = std::numeric_limits<float>::infinity();
    DistanceGrid     grid;
    grid.lo           = Vec3d(bbox.min.x() - margin, bbox.min.y() - margin, layers.front().bottom - 1.);
    const Vec3d hi    = Vec3d(bbox.max.x() + margin, bbox.max.y() + margin, layers.back().top + above);
    grid.cell         = 0.4;
    while ((hi.x() - grid.lo.x()) * (hi.y() - grid.lo.y()) * (hi.z() - grid.lo.z()) / std::pow(grid.cell, 3) > 1.2e7)
        grid.cell *= 1.1;
    grid.nx = int(std::ceil((hi.x() - grid.lo.x()) / grid.cell)) + 1;
    grid.ny = int(std::ceil((hi.y() - grid.lo.y()) / grid.cell)) + 1;
    grid.nz = int(std::ceil((hi.z() - grid.lo.z()) / grid.cell)) + 1;
    const Vec3i32 size(grid.nx, grid.ny, grid.nz);
    const size_t  slice = size_t(grid.nx) * grid.ny;

    // Inside nodes are infinite: they are not sites, so the transform gives them their squared distance to the
    // outside, and the outside 0.
    std::vector<float> to_outside(slice * grid.nz, 0.f);
    tbb::parallel_for(tbb::blocked_range<int>(0, grid.nz), [&](const tbb::blocked_range<int> &range) {
        for (int k = range.begin(); k < range.end(); ++k) {
            const double z  = grid.lo.z() + k * grid.cell;
            // The layer whose band (bottom, top] holds z.
            auto         it = std::lower_bound(layers.begin(), layers.end(), z - 1e-6, [](const BoneLayer &l, double v) { return l.top < v; });
            if (it != layers.end() && z > it->bottom - 1e-6)
                rasterize_expolygons(*it->outline, Vec2d(grid.lo.x(), grid.lo.y()), grid.cell, grid.nx, grid.ny, inf,
                                     to_outside.data() + size_t(k) * slice);
        }
    });
    throw_if_canceled();

    std::vector<float> to_inside(to_outside.size());
    for (size_t i = 0; i < to_outside.size(); ++i)
        to_inside[i] = to_outside[i] == inf ? 0.f : inf;
    for (int axis = 0; axis < 3; ++axis) {
        squared_distance_transform_axis(to_inside, size, axis, throw_if_canceled);
        squared_distance_transform_axis(to_outside, size, axis, throw_if_canceled);
    }
    // A node is inside when it is away from the outside. The surface lies half a voxel from the nodes on either
    // side of it, and inside is negative.
    grid.v = std::move(to_outside);
    for (size_t i = 0; i < grid.v.size(); ++i)
        grid.v[i] = grid.v[i] > 0.f ? -float((std::sqrt(double(grid.v[i])) - 0.5) * grid.cell) :
                                      float((std::sqrt(double(to_inside[i])) - 0.5) * grid.cell);
    return grid;
}

// ---------------------------------------------------------------------------------------------
// Target density inside the object.

class Field
{
public:
    Field(const DistanceGrid &grid, const BoneParams &params) : m_grid(grid), m_params(params), m_hollow_from(params.gradient.hollow_from()) {}

    double depth(double x, double y, double z) const { return -m_grid.sample(x, y, z); }

    double density(double depth, double x, double y, double z) const
    {
        double rho = m_params.gradient.density(depth - m_params.shell);
        if (m_params.irregularity > 0.)
            rho *= 1. + 0.35 * m_params.irregularity * (2. * noise3(x / 7., y / 7., z / 7., m_params.seed) - 1.);
        return std::max(rho, 0.005);
    }

    // How far a point is inside the hollow core, negative outside. A point stays hollow only if the
    // cavity can still retreat from it before any point above must be dense.
    double cavity_margin(double x, double y, double z) const
    {
        if (!std::isfinite(m_hollow_from))
            return -1.;
        const double from = m_params.shell + m_hollow_from;
        double       m    = depth(x, y, z) - from;
        for (double h = CavityLookStep; h <= CavityLookAhead && m > 0.; h += CavityLookStep)
            m = std::min(m, depth(x, y, z + h) - from + CavityCloseRate * h);
        return m;
    }

    bool hollow(double x, double y, double z) const { return cavity_margin(x, y, z) > 0.; }

private:
    const DistanceGrid &m_grid;
    const BoneParams   &m_params;
    // Depth below the walls.
    double              m_hollow_from;
};

// ---------------------------------------------------------------------------------------------
// Cell centres in 3D, spaced by the target density: dart throwing until saturated, each centre
// needing its own spacing clear (in a metric stretched along Z) of the earlier ones.

struct Seeds
{
    std::vector<double> x, y, z, spacing;
    double              max_spacing { 0. };
};

Seeds place_seeds(const Field &field, const BoneParams &params, const DistanceGrid &grid, const BoundingBoxf &bbox, double z0, double z1,
                  const std::function<void()> &throw_if_canceled)
{
    constexpr double margin = 3.;
    const Vec3d      lo(bbox.min.x() - margin, bbox.min.y() - margin, z0 - 1.), hi(bbox.max.x() + margin, bbox.max.y() + margin, z1 + 1.);
    const double     min_spacing = params.line_width * FoamWallFactor / (params.gradient.wall_density * (1. + 0.35 * params.irregularity));
    const double     exclusion   = 0.85 - 0.25 * params.irregularity;
    auto spacing_at = [&](double x, double y, double z) {
        // The walls of the cells in the hollow core are cut away; only the cells along its edge must be small, so
        // that the cells of the band don't reach into it. Deeper in, a cell can be as wide as its depth into the
        // core and still stay inside it, as the face to a smaller cell lies half its spacing from its centre.
        if (const double cavity = field.cavity_margin(x, y, z); cavity > 0.)
            return std::clamp(cavity, CavityCellFactor * min_spacing, 60.);
        return std::min(60., params.line_width * FoamWallFactor / field.density(std::max(field.depth(x, y, z), 0.), x, y, z));
    };

    const double cs = std::max(1., min_spacing);
    const int    gx = int(std::ceil((hi.x() - lo.x()) / cs)), gy = int(std::ceil((hi.y() - lo.y()) / cs)), gz = int(std::ceil((hi.z() - lo.z()) / cs));
    std::vector<std::vector<int>> cells(size_t(gx) * gy * gz);
    Seeds                         out, sorted;
    Random                        rng(uint64_t(params.seed));

    // Candidates, about ten per cell to place, in every hash cell near the object, then shuffled: dart
    // throwing in random order until saturated, so the saturation spacing sets the density.
    std::vector<Vec3d> candidates;
    const double       cell_volume = cs * cs * cs;
    for (int k = 0; k < gz; ++k) {
        throw_if_canceled();
        for (int j = 0; j < gy; ++j)
            for (int i = 0; i < gx; ++i) {
                const Vec3d c = lo + Vec3d(i + 0.5, j + 0.5, k + 0.5) * cs;
                if (grid.sample(c.x(), c.y(), c.z()) > margin + cs)
                    continue;
                const double expected = 10. * params.stretch * cell_volume / std::pow(spacing_at(c.x(), c.y(), c.z()), 3);
                const int    count    = int(expected + rng.uniform());
                for (int n = 0; n < count; ++n) {
                    const double u = rng.uniform(), v = rng.uniform(), w = rng.uniform();
                    candidates.emplace_back(lo + Vec3d(i + u, j + v, k + w) * cs);
                }
            }
    }
    for (size_t i = candidates.size(); i > 1; --i)
        std::swap(candidates[i - 1], candidates[rng.below(i)]);

    for (size_t t = 0; t < candidates.size(); ++t) {
        if ((t & 0xffff) == 0)
            throw_if_canceled();
        const double x = candidates[t].x(), y = candidates[t].y(), z = candidates[t].z();
        if (grid.sample(x, y, z) > margin)
            continue;
        const double sp = spacing_at(x, y, z);
        const double r  = exclusion * sp;
        const int    ci = std::min(gx - 1, int((x - lo.x()) / cs)), cj = std::min(gy - 1, int((y - lo.y()) / cs)), ck = std::min(gz - 1, int((z - lo.z()) / cs));
        const int    rc = int(std::ceil(r / cs)), rz = int(std::ceil(r * params.stretch / cs));
        bool         ok = true;
        for (int k = std::max(0, ck - rz); k <= std::min(gz - 1, ck + rz) && ok; ++k)
            for (int j = std::max(0, cj - rc); j <= std::min(gy - 1, cj + rc) && ok; ++j)
                for (int i = std::max(0, ci - rc); i <= std::min(gx - 1, ci + rc) && ok; ++i)
                    for (int q : cells[(size_t(k) * gy + j) * gx + i]) {
                        const double dz = (out.z[q] - z) / params.stretch;
                        if (sqr(out.x[q] - x) + sqr(out.y[q] - y) + dz * dz < r * r) {
                            ok = false;
                            break;
                        }
                    }
        if (!ok)
            continue;
        cells[(size_t(ck) * gy + cj) * gx + ci].push_back(int(out.x.size()));
        out.x.push_back(x);
        out.y.push_back(y);
        out.z.push_back(z);
        out.spacing.push_back(sp);
    }

    // Sorted by z, so each layer takes a window of them.
    std::vector<size_t> order(out.x.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&out](size_t a, size_t b) { return out.z[a] < out.z[b] || (out.z[a] == out.z[b] && a < b); });
    for (size_t i : order) {
        sorted.x.push_back(out.x[i]);
        sorted.y.push_back(out.y[i]);
        sorted.z.push_back(out.z[i]);
        sorted.spacing.push_back(out.spacing[i]);
        sorted.max_spacing = std::max(sorted.max_spacing, out.spacing[i]);
    }
    return sorted;
}

// ---------------------------------------------------------------------------------------------
// Cross-section of the foam at height z: the cross-section of a 3D power diagram is the 2D power
// diagram of the centres with weights w_i - (z - z_i)^2.

constexpr int MaxCellVertices = 96;

struct CellPolygon
{
    std::array<double, MaxCellVertices> x, y;
    // Label of the edge from vertex k to k + 1: the seed that cut it, -1 for the domain boundary.
    std::array<int, MaxCellVertices> label;
    int                              n { 0 };
};

// Keep the part of `a` (local coordinates) where ax * x + ay * y <= c.
void clip_half_plane(const CellPolygon &a, double ax, double ay, double c, int j, CellPolygon &o)
{
    int m = 0;
    for (int k = 0; k < a.n && m < MaxCellVertices - 2; ++k) {
        const int    k1 = k + 1 == a.n ? 0 : k + 1;
        const double x0 = a.x[k], y0 = a.y[k], x1 = a.x[k1], y1 = a.y[k1];
        const double f0 = ax * x0 + ay * y0 - c, f1 = ax * x1 + ay * y1 - c;
        if (f0 <= 0.) {
            o.x[m] = x0, o.y[m] = y0, o.label[m] = a.label[k], ++m;
            if (f1 > 0.) {
                const double t = f0 / (f0 - f1);
                o.x[m] = x0 + t * (x1 - x0), o.y[m] = y0 + t * (y1 - y0), o.label[m] = j, ++m;
            }
        } else if (f1 <= 0.) {
            const double t = f0 / (f0 - f1);
            o.x[m] = x0 + t * (x1 - x0), o.y[m] = y0 + t * (y1 - y0), o.label[m] = a.label[k], ++m;
        }
    }
    o.n = m;
}

// The walls between the cells of the seeds, each once.
void power_diagram_walls(const std::vector<double> &X, const std::vector<double> &Y, const std::vector<double> &W, const BoundingBoxf &dom,
                         double hash_cell, Segments &out)
{
    const int n = int(X.size());
    if (n < 2)
        return;
    const int gx = std::max(1, int(std::ceil(dom.size().x() / hash_cell))), gy = std::max(1, int(std::ceil(dom.size().y() / hash_cell)));
    std::vector<int> head(size_t(gx) * gy, -1), next(n, -1);
    std::vector<double> sorted_w(W);
    std::nth_element(sorted_w.begin(), sorted_w.begin() + n / 2, sorted_w.end());
    // Seeds with large weights (big cells) would stretch the ring search of every cell, so they go in a
    // coarser grid of their own and are tested only within the distance where their faces can reach a
    // cell. The rest are found by a ring search bounded by the power distance.
    const double giant_weight      = sorted_w[n / 2] + 4. * hash_cell * hash_cell;
    double       max_normal_weight = -std::numeric_limits<double>::infinity(), max_giant_weight = -std::numeric_limits<double>::infinity();
    auto cell_of = [&](double x, double y) {
        return std::make_pair(std::clamp(int((x - dom.min.x()) / hash_cell), 0, gx - 1), std::clamp(int((y - dom.min.y()) / hash_cell), 0, gy - 1));
    };
    const double     giant_cell = 4. * hash_cell;
    const int        ggx = std::max(1, int(std::ceil(dom.size().x() / giant_cell))), ggy = std::max(1, int(std::ceil(dom.size().y() / giant_cell)));
    std::vector<int> giant_head(size_t(ggx) * ggy, -1), giant_next(n, -1);
    for (int i = 0; i < n; ++i) {
        if (W[i] > giant_weight) {
            max_giant_weight = std::max(max_giant_weight, W[i]);
            const int u = std::clamp(int((X[i] - dom.min.x()) / giant_cell), 0, ggx - 1), v = std::clamp(int((Y[i] - dom.min.y()) / giant_cell), 0, ggy - 1);
            giant_next[i] = giant_head[size_t(v) * ggx + u];
            giant_head[size_t(v) * ggx + u] = i;
            continue;
        }
        max_normal_weight = std::max(max_normal_weight, W[i]);
        auto [cx, cy]     = cell_of(X[i], Y[i]);
        next[i]           = head[size_t(cy) * gx + cx];
        head[size_t(cy) * gx + cx] = i;
    }
    const bool any_giant = max_giant_weight > -std::numeric_limits<double>::infinity();

    CellPolygon pa, pb;
    for (int i = 0; i < n; ++i) {
        const double xi = X[i], yi = Y[i], wi = W[i];
        CellPolygon *a = &pa, *b = &pb;
        a->n = 4;
        a->x[0] = dom.min.x() - xi, a->y[0] = dom.min.y() - yi;
        a->x[1] = dom.max.x() - xi, a->y[1] = dom.min.y() - yi;
        a->x[2] = dom.max.x() - xi, a->y[2] = dom.max.y() - yi;
        a->x[3] = dom.min.x() - xi, a->y[3] = dom.max.y() - yi;
        a->label[0] = a->label[1] = a->label[2] = a->label[3] = -1;
        auto clip_by = [&](int j) {
            if (j == i || a->n == 0)
                return;
            const double dx = X[j] - xi, dy = Y[j] - yi, d2 = dx * dx + dy * dy;
            if (d2 < 1e-12)
                return;
            const double c = d2 - W[j] + wi;
            bool         cuts = false;
            for (int k = 0; k < a->n && !cuts; ++k)
                cuts = 2. * (dx * a->x[k] + dy * a->y[k]) > c;
            if (!cuts)
                return;
            clip_half_plane(*a, 2. * dx, 2. * dy, c, j, *b);
            std::swap(a, b);
        };
        auto [ci, cj]  = cell_of(xi, yi);
        const int rmax = std::max(gx, gy);
        for (int r = 0; r <= rmax && a->n > 0; ++r) {
            for (int v = cj - r; v <= cj + r; ++v) {
                if (v < 0 || v >= gy)
                    continue;
                const bool edge = v == cj - r || v == cj + r;
                for (int u = ci - r; u <= ci + r; u += (edge || r == 0) ? 1 : 2 * r) {
                    if (u >= 0 && u < gx)
                        for (int j = head[size_t(v) * gx + u]; j >= 0; j = next[j])
                            clip_by(j);
                    if (r == 0)
                        break;
                }
            }
            // Seeds beyond this ring are at least r * hash_cell away; their bisectors lie beyond the cell.
            double r2 = 0.;
            for (int k = 0; k < a->n; ++k)
                r2 = std::max(r2, a->x[k] * a->x[k] + a->y[k] * a->y[k]);
            const double d = r * hash_cell;
            if (d > 0. && d / 2. + std::min(0., wi - max_normal_weight) / (2. * d) > std::sqrt(r2))
                break;
        }
        // A giant at distance D cuts the cell only if its face, (D^2 + wi - wj) / (2 D) from the seed, is
        // within the cell's radius R: D < R + sqrt(R^2 + wj - wi).
        if (any_giant && a->n > 0) {
            double r2 = 0.;
            for (int k = 0; k < a->n; ++k)
                r2 = std::max(r2, a->x[k] * a->x[k] + a->y[k] * a->y[k]);
            const double R = std::sqrt(r2), reach = R + std::sqrt(std::max(0., r2 + max_giant_weight - wi));
            const int    u0 = std::max(0, int((xi - reach - dom.min.x()) / giant_cell)), u1 = std::min(ggx - 1, int((xi + reach - dom.min.x()) / giant_cell));
            const int    v0 = std::max(0, int((yi - reach - dom.min.y()) / giant_cell)), v1 = std::min(ggy - 1, int((yi + reach - dom.min.y()) / giant_cell));
            for (int v = v0; v <= v1; ++v)
                for (int u = u0; u <= u1; ++u)
                    for (int j = giant_head[size_t(v) * ggx + u]; j >= 0; j = giant_next[j])
                        clip_by(j);
        }
        for (int k = 0; k < a->n; ++k) {
            const int j = a->label[k];
            if (j <= i)
                continue;
            const int k1 = k + 1 == a->n ? 0 : k + 1;
            out.push_back({Vec2d(a->x[k] + xi, a->y[k] + yi), Vec2d(a->x[k1] + xi, a->y[k1] + yi)});
        }
    }
}

// The walls of the foam at layer z, clipped to the sparse infill region and cut away in the hollow core.
Segments layer_walls(const Seeds &seeds, const Field &field, const BoneParams &params, const BoneLayer &layer, const BoundingBoxf &dom)
{
    Segments out;
    if (layer.sparse.empty())
        return out;
    const double z = 0.5 * (layer.bottom + layer.top), reach = 1.6 * params.stretch;
    const double window = reach * seeds.max_spacing;
    const auto   first  = std::lower_bound(seeds.z.begin(), seeds.z.end(), z - window) - seeds.z.begin();
    std::vector<double> X, Y, W;
    for (size_t i = size_t(first); i < seeds.z.size() && seeds.z[i] < z + window; ++i)
        if (std::abs(seeds.z[i] - z) < reach * seeds.spacing[i]) {
            X.push_back(seeds.x[i]);
            Y.push_back(seeds.y[i]);
            // A quarter of the spacing squared puts the face between a big and a small cell in
            // proportion to their sizes instead of midway, which would blur the gradient.
            W.push_back(0.25 * sqr(seeds.spacing[i]) - sqr((seeds.z[i] - z) / params.stretch));
        }
    Segments raw;
    power_diagram_walls(X, Y, W, dom, std::max(2., 2. * params.line_width / std::max(params.gradient.wall_density, 0.05)), raw);

    Polylines lines;
    lines.reserve(raw.size());
    for (const Segment &sg : raw)
        lines.emplace_back(Points{Point(scaled<coord_t>(sg.a.x()), scaled<coord_t>(sg.a.y())), Point(scaled<coord_t>(sg.b.x()), scaled<coord_t>(sg.b.y()))});
    lines = intersection_pl(lines, layer.sparse);

    // Cut the walls in the hollow core, bisecting each crossing.
    constexpr double step = 0.25;
    for (const Polyline &pl : lines)
        for (size_t p = 1; p < pl.points.size(); ++p) {
            const Vec2d a = unscaled(pl.points[p - 1]), b = unscaled(pl.points[p]);
            const double len = (b - a).norm();
            if (len < 1e-4)
                continue;
            auto at     = [&](double t) { return a + (b - a) * t; };
            auto keep   = [&](double t) { const Vec2d q = at(t); return !field.hollow(q.x(), q.y(), z); };
            const int ns = std::max(1, int(std::ceil(len / step)));
            bool      in = keep(0.);
            double    t0 = 0.;
            for (int q = 1; q <= ns; ++q) {
                const double t   = double(q) / ns;
                const bool   now = keep(t);
                if (now == in)
                    continue;
                double lo = double(q - 1) / ns, hi = t;
                for (int it = 0; it < 12; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    (keep(mid) == in ? lo : hi) = mid;
                }
                const double tc = 0.5 * (lo + hi);
                if (in)
                    out.push_back({at(t0), at(tc)});
                in = now;
                t0 = tc;
            }
            if (in)
                out.push_back({at(t0), b});
        }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Printability: what rests on the layer below.

// A raster of the layer below: is a point outside its sparse region (on solid infill, walls or skins),
// or within the overhang allowance of one of its kept walls?
class SupportRaster
{
public:
    SupportRaster(const BoundingBoxf &dom, double resolution)
        : m_lo(dom.min), m_res(resolution), m_nx(int(std::ceil(dom.size().x() / resolution)) + 1), m_ny(int(std::ceil(dom.size().y() / resolution)) + 1),
          m_bits(size_t(m_nx) * m_ny)
    {}

    void set(const ExPolygons &sparse, const Segments &walls, double tol)
    {
        std::fill(m_bits.begin(), m_bits.end(), uint8_t(0));
        rasterize_expolygons(sparse, m_lo + Vec2d::Constant(0.5 * m_res), m_res, m_nx, m_ny, Sparse, m_bits.data());
        const double tol2 = tol * tol;
        for (const Segment &sg : walls) {
            const Vec2d  e  = sg.b - sg.a;
            const double ll = e.squaredNorm();
            const int    i0 = std::max(0, int((std::min(sg.a.x(), sg.b.x()) - tol - m_lo.x()) / m_res));
            const int    i1 = std::min(m_nx - 1, int((std::max(sg.a.x(), sg.b.x()) + tol - m_lo.x()) / m_res) + 1);
            const int    j0 = std::max(0, int((std::min(sg.a.y(), sg.b.y()) - tol - m_lo.y()) / m_res));
            const int    j1 = std::min(m_ny - 1, int((std::max(sg.a.y(), sg.b.y()) + tol - m_lo.y()) / m_res) + 1);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const Vec2d  p(m_lo.x() + (i + 0.5) * m_res, m_lo.y() + (j + 0.5) * m_res);
                    const double t = ll > 0. ? std::clamp((p - sg.a).dot(e) / ll, 0., 1.) : 0.;
                    if ((p - sg.a - e * t).squaredNorm() <= tol2)
                        m_bits[size_t(j) * m_nx + i] |= Wall;
                }
        }
    }

    bool supported(const Vec2d &p) const
    {
        const int i = int((p.x() - m_lo.x()) / m_res), j = int((p.y() - m_lo.y()) / m_res);
        if (i < 0 || j < 0 || i >= m_nx || j >= m_ny)
            return true;
        const uint8_t b = m_bits[size_t(j) * m_nx + i];
        return (b & Sparse) == 0 || (b & Wall) != 0;
    }

private:
    static constexpr uint8_t Sparse = 1, Wall = 2;
    Vec2d                    m_lo;
    double                   m_res;
    int                      m_nx, m_ny;
    std::vector<uint8_t>     m_bits;
};

uint64_t point_key(const Vec2d &p)
{
    const int64_t x = int64_t(std::llround(p.x() * 200.)), y = int64_t(std::llround(p.y() * 200.));
    return (uint64_t(x) << 32) ^ uint64_t(uint32_t(y));
}

// Supported pieces are kept. Unsupported pieces become edges of a graph whose nodes are wall junctions
// and anchors (where a piece meets supported wall). New wall is kept as a bridge when it hangs
// between anchors: dead-end branches are stripped, and no point may be more than max_bridge / 2 along
// the walls from its nearest anchor. Under a ceiling, where cells get smaller going up, new walls
// appear over larger cells below; this lets them span across the walls below.
Segments prune(const Segments &walls, const SupportRaster &below, double max_bridge)
{
    struct Edge
    {
        int    a, b;
        double len;
        Vec2d  p0, p1;
    };
    Segments                          out;
    std::unordered_map<uint64_t, int> node_of;
    std::vector<char>                 anchor;
    std::vector<Edge>                 edges;
    auto junction = [&](const Vec2d &p) {
        auto [it, inserted] = node_of.emplace(point_key(p), int(anchor.size()));
        if (inserted)
            anchor.push_back(0);
        return it->second;
    };
    auto new_anchor = [&]() {
        anchor.push_back(1);
        return int(anchor.size()) - 1;
    };
    constexpr double step = 0.1;
    std::vector<char> ok;
    for (const Segment &sg : walls) {
        const double len = (sg.b - sg.a).norm();
        const int    ns  = std::max(1, int(std::ceil(len / step)));
        auto at = [&](int q) { return sg.a + (sg.b - sg.a) * (double(q) / ns); };
        ok.resize(ns + 1);
        for (int q = 0; q <= ns; ++q)
            ok[q] = below.supported(at(q));
        const int start = junction(sg.a), end = junction(sg.b);
        for (int q = 0; q <= ns;) {
            int e = q;
            while (e < ns && ok[e + 1] == ok[q])
                ++e;
            if (ok[q]) {
                // A supported piece anchors the junctions it touches.
                if (q == 0)
                    anchor[start] = 1;
                if (e == ns)
                    anchor[end] = 1;
                if (double(e - q) * len / ns >= MinWallPiece || q == 0 || e == ns)
                    out.push_back({at(q), at(e)});
            } else {
                const int qa = q > 0 ? q - 1 : 0, qb = e < ns ? e + 1 : ns;
                edges.push_back({q > 0 ? new_anchor() : start, e < ns ? new_anchor() : end, double(qb - qa) * len / ns, at(qa), at(qb)});
            }
            q = e + 1;
        }
    }
    if (edges.empty())
        return out;

    // Alternate until stable: strip dead ends (cantilevers), then drop edges too far from an anchor,
    // which can leave new dead ends behind.
    const int                     n_nodes = int(anchor.size());
    std::vector<char>             alive(edges.size(), 1);
    std::vector<int>              degree(n_nodes, 0);
    std::vector<std::vector<int>> adjacent(n_nodes);
    for (size_t i = 0; i < edges.size(); ++i) {
        ++degree[edges[i].a], ++degree[edges[i].b];
        adjacent[edges[i].a].push_back(int(i));
        adjacent[edges[i].b].push_back(int(i));
    }
    auto kill = [&](size_t i) {
        alive[i] = 0;
        --degree[edges[i].a], --degree[edges[i].b];
    };
    // A loose end that is no anchor: its last edge goes, which can loosen the next node along.
    auto dead_end = [&](int v) { return degree[v] == 1 && !anchor[v]; };
    std::vector<int> loose;
    auto strip_dead_ends = [&]() {
        for (int v = 0; v < n_nodes; ++v)
            if (dead_end(v))
                loose.push_back(v);
        while (!loose.empty()) {
            const int v = loose.back();
            loose.pop_back();
            if (!dead_end(v))
                continue;
            for (int i : adjacent[v])
                if (alive[i]) {
                    kill(size_t(i));
                    if (const int w = edges[i].a == v ? edges[i].b : edges[i].a; dead_end(w))
                        loose.push_back(w);
                    break;
                }
        }
    };
    std::vector<double> dist(n_nodes);
    for (int pass = 0; pass < 20; ++pass) {
        strip_dead_ends();
        // Distance along the walls to the nearest anchor.
        std::fill(dist.begin(), dist.end(), std::numeric_limits<double>::infinity());
        using Item = std::pair<double, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        for (int v = 0; v < n_nodes; ++v)
            if (anchor[v] && degree[v] > 0) {
                dist[v] = 0.;
                queue.emplace(0., v);
            }
        while (!queue.empty()) {
            auto [d, v] = queue.top();
            queue.pop();
            if (d > dist[v] || d > max_bridge)
                continue;
            for (int i : adjacent[v]) {
                if (!alive[i])
                    continue;
                const int    w  = edges[i].a == v ? edges[i].b : edges[i].a;
                const double nd = d + edges[i].len;
                if (nd < dist[w]) {
                    dist[w] = nd;
                    queue.emplace(nd, w);
                }
            }
        }
        bool dropped = false;
        for (size_t i = 0; i < edges.size(); ++i)
            if (alive[i] && 0.5 * (dist[edges[i].a] + dist[edges[i].b] + edges[i].len) > 0.5 * max_bridge) {
                kill(i);
                dropped = true;
            }
        if (!dropped)
            break;
    }
    for (size_t i = 0; i < edges.size(); ++i)
        if (alive[i])
            out.push_back({edges[i].p0, edges[i].p1});
    return out;
}

// Join the segments of a layer into polylines through the junctions where exactly two meet.
Polylines join_segments(const Segments &segs)
{
    std::unordered_map<uint64_t, std::vector<int>> ends;
    for (size_t s = 0; s < segs.size(); ++s) {
        ends[point_key(segs[s].a)].push_back(int(2 * s));
        ends[point_key(segs[s].b)].push_back(int(2 * s + 1));
    }
    auto point = [&](int t) { return (t & 1) ? segs[t >> 1].b : segs[t >> 1].a; };
    auto to_point = [](const Vec2d &p) { return Point(scaled<coord_t>(p.x()), scaled<coord_t>(p.y())); };
    std::vector<char> used(segs.size(), 0);
    Polylines         out;
    auto walk = [&](int t) {
        Polyline pl;
        pl.points.push_back(to_point(point(t)));
        while (t >= 0 && !used[t >> 1]) {
            used[t >> 1] = 1;
            const int other = t ^ 1;
            pl.points.push_back(to_point(point(other)));
            const std::vector<int> &at = ends[point_key(point(other))];
            t = -1;
            if (at.size() == 2)
                for (int c : at)
                    if (c != other && !used[c >> 1])
                        t = c;
        }
        if (pl.points.size() >= 2)
            out.push_back(std::move(pl));
    };
    // Start at the ends and branch points, then go round what is left (closed loops). In the order of the
    // segments rather than of the map, so that the result doesn't depend on the library.
    for (size_t s = 0; s < segs.size(); ++s)
        for (int t : { int(2 * s), int(2 * s + 1) })
            if (!used[s] && ends[point_key(point(t))].size() != 2)
                walk(t);
    for (size_t s = 0; s < segs.size(); ++s)
        if (!used[s])
            walk(int(2 * s));
    return out;
}

} // namespace

BoneGradient BoneGradient::from_config(const ConfigBase &config)
{
    BoneGradient g;
    g.wall_density = std::clamp(config.option<ConfigOptionPercent>("sparse_infill_density")->value / 100., 0.02, 1.);
    g.core_density = std::clamp(config.option<ConfigOptionPercent>("adaptive_infill_interior_density")->value / 100., 0.01, 1.);
    g.falloff      = config.option<ConfigOptionFloat>("bone_falloff")->value;
    g.hollow_depth = config.option<ConfigOptionFloat>("bone_hollow_depth")->value;
    g.curve        = config.option<ConfigOptionEnum<BoneFalloffCurve>>("bone_falloff_curve")->value;
    g.custom       = parse_custom(config.option<ConfigOptionString>("bone_falloff_custom")->value);
    return g;
}

std::array<double, 4> BoneGradient::parse_custom(const std::string &serialized)
{
    const std::array<double, 4> linear { 0.2, 0.4, 0.6, 0.8 };
    std::vector<std::string>    parts;
    boost::split(parts, serialized, boost::is_any_of(","));
    if (parts.size() != linear.size())
        return linear;
    std::array<double, 4> out;
    for (size_t i = 0; i < parts.size(); ++i) {
        boost::trim(parts[i]);
        size_t       parsed = 0;
        const double value  = string_to_double_decimal_point(parts[i], &parsed);
        if (parts[i].empty() || parsed != parts[i].size() || !std::isfinite(value))
            return linear;
        out[i] = std::clamp(value / 100., 0., 1.);
    }
    return out;
}

std::string BoneGradient::serialize_custom(const std::array<double, 4> &custom)
{
    std::string out;
    for (double v : custom)
        out += (out.empty() ? "" : ",") + float_to_string_decimal_point(std::round(std::clamp(v, 0., 1.) * 1000.) / 10.);
    return out;
}

double BoneGradient::progress(double t) const
{
    t = std::clamp(t, 0., 1.);
    // Steepness of the exponential curves.
    constexpr double k = 4.;
    switch (curve) {
    case BoneFalloffCurve::Linear: return t;
    case BoneFalloffCurve::SCurve: return smoothstep(t);
    case BoneFalloffCurve::Exponential: return (1. - std::exp(-k * t)) / (1. - std::exp(-k));
    case BoneFalloffCurve::Logarithmic: return (std::exp(k * t) - 1.) / (std::exp(k) - 1.);
    case BoneFalloffCurve::Custom: break;
    }
    // Monotone cubic (Fritsch-Carlson) through the custom points and the two ends, so the curve doesn't
    // overshoot between the points.
    constexpr size_t            n = 6;
    constexpr double            h = 1. / (n - 1);
    const std::array<double, n> y { 0., custom[0], custom[1], custom[2], custom[3], 1. };
    std::array<double, n - 1>   d;
    for (size_t i = 0; i + 1 < n; ++i)
        d[i] = (y[i + 1] - y[i]) / h;
    std::array<double, n> m;
    m.front() = d.front();
    m.back()  = d.back();
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = d[i - 1] * d[i] <= 0. ? 0. : 2. / (1. / d[i - 1] + 1. / d[i]);
    const size_t i  = std::min(n - 2, size_t(t / h));
    const double u  = (t - i * h) / h;
    const double u2 = u * u, u3 = u2 * u;
    return std::clamp((2. * u3 - 3. * u2 + 1.) * y[i] + (u3 - 2. * u2 + u) * h * m[i] + (-2. * u3 + 3. * u2) * y[i + 1] +
                          (u3 - u2) * h * m[i + 1],
                      0., 1.);
}

double BoneGradient::density(double depth) const
{
    return wall_density + (core_density - wall_density) * progress(depth / std::max(falloff, 0.5));
}

double BoneGradient::hollow_from() const
{
    double from = hollow_depth > 0. ? hollow_depth : std::numeric_limits<double>::infinity();
    if (core_density < HollowBelowDensity)
        for (double depth = 0.; depth < from; depth += 0.05)
            if (density(depth) < HollowBelowDensity) {
                from = depth;
                break;
            }
    return from;
}

BoneParams BoneParams::from_config(const ConfigBase &config, double line_width, double shell)
{
    BoneParams p;
    p.gradient     = BoneGradient::from_config(config);
    p.line_width   = line_width;
    p.shell        = shell;
    p.irregularity = std::clamp(config.option<ConfigOptionPercent>("bone_irregularity")->value / 100., 0., 1.);
    p.stretch      = 1. + 2. * (1. - std::clamp(config.option<ConfigOptionPercent>("bone_isotropy")->value / 100., 0., 1.));
    p.max_bridge   = config.option<ConfigOptionFloat>("bone_max_bridge")->value;
    return p;
}

bool uses_bone_infill(const PrintRegionConfig &config)
{
    return config.sparse_infill_pattern == ipBone && config.sparse_infill_density > 0 && config.sparse_infill_density < 100;
}

std::vector<Polylines> generate_bone_walls(const std::vector<BoneLayer> &layers, const BoneParams &params, const std::function<void()> &throw_if_canceled)
{
    std::vector<Polylines> out(layers.size());
    BoundingBox            bbox;
    for (const BoneLayer &layer : layers)
        bbox.merge(get_extents(*layer.outline));
    if (!bbox.defined)
        return out;
    const BoundingBoxf box(unscaled(bbox.min), unscaled(bbox.max));

    const DistanceGrid grid = build_distance_grid(layers, box, throw_if_canceled);
    const Field        field(grid, params);
    const Seeds        seeds = place_seeds(field, params, grid, box, layers.front().bottom, layers.back().top, throw_if_canceled);
    throw_if_canceled();

    // Every layer is a cross-section of the same foam, so they are cut in parallel.
    constexpr double      margin = 3.;
    const BoundingBoxf    dom(Vec2d(box.min.x() - margin, box.min.y() - margin), Vec2d(box.max.x() + margin, box.max.y() + margin));
    std::vector<Segments> walls(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            throw_if_canceled();
            walls[l] = layer_walls(seeds, field, params, layers[l], dom);
        }
    });

    // Bottom-up, only what rests on the layer below is kept.
    const double  tol0 = 0.5 * params.line_width;
    SupportRaster raster(dom, std::clamp(0.4 * tol0, 0.05, 0.15));
    for (size_t l = 1; l < layers.size(); ++l) {
        if ((l & 31) == 0)
            throw_if_canceled();
        if (walls[l].empty())
            continue;
        const double tol = std::min(tol0, (layers[l].top - layers[l].bottom) * std::tan(M_PI / 4.));
        raster.set(layers[l - 1].sparse, walls[l - 1], tol);
        walls[l] = prune(walls[l], raster, params.max_bridge);
    }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l)
            out[l] = join_segments(walls[l]);
    });
    return out;
}

BoneInfill::BoneInfill(const PrintObject &object, const std::function<void()> &throw_if_canceled)
{
    const auto &layers = object.layers();
    if (layers.empty())
        return;

    // Settings from the first region using the bone infill.
    const PrintRegion *bone_region = nullptr;
    for (size_t i = 0; i < object.num_printing_regions() && !bone_region; ++i)
        if (uses_bone_infill(object.printing_region(i).config()))
            bone_region = &object.printing_region(i);
    if (!bone_region)
        return;
    const PrintRegionConfig &config       = bone_region->config();
    const double             layer_height = object.config().layer_height.value;
    const int                wall_loops   = config.wall_loops.value;
    const double             shell        = wall_loops > 0 ? bone_region->flow(object, frExternalPerimeter, layer_height).width() +
                                                      (wall_loops - 1) * bone_region->flow(object, frPerimeter, layer_height).width() :
                                                                 0.;
    const BoneParams params = BoneParams::from_config(config, bone_region->flow(object, frInfill, layer_height).width(), shell);

    std::vector<BoneLayer> data;
    data.reserve(layers.size());
    for (const Layer *layer : layers) {
        BoneLayer d { layer->bottom_z(), layer->print_z, &layer->lslices, {} };
        for (const LayerRegion *layerm : layer->regions())
            if (uses_bone_infill(layerm->region().config()))
                for (const Surface &surface : layerm->fill_surfaces.surfaces)
                    if (surface.surface_type == stInternal)
                        d.sparse.push_back(surface.expolygon);
        if (d.sparse.size() > 1)
            d.sparse = union_ex(d.sparse);
        m_print_z.push_back(d.top);
        data.push_back(std::move(d));
    }
    m_walls = generate_bone_walls(data, params, throw_if_canceled);
}

const Polylines &BoneInfill::walls(coordf_t print_z) const
{
    static const Polylines none;
    const auto it = std::lower_bound(m_print_z.begin(), m_print_z.end(), print_z - EPSILON);
    return it != m_print_z.end() && std::abs(*it - print_z) < EPSILON ? m_walls[it - m_print_z.begin()] : none;
}

void FillBone::_fill_surface_single(const FillParams              &params,
                                    unsigned int                   thickness_layers,
                                    const std::pair<float, Point> &direction,
                                    ExPolygon                      expolygon,
                                    Polylines                     &polylines_out)
{
    if (bone_infill == nullptr)
        return;
    Polylines lines = intersection_pl(bone_infill->walls(this->z), expolygon);
    chain_or_connect_infill(std::move(lines), expolygon, polylines_out, this->spacing, params);
}

} // namespace Slic3r
