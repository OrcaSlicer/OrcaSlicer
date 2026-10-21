#include "VoronoiPointCloud.hpp"
#include "../KDTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include <functional>
#include <math.h>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <cstdint>

namespace Slic3r::Voronoi {
namespace {
uint64_t mix(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool contains(const BoundingBoxf3 &outer, const BoundingBoxf3 &inner)
{
    return (outer.min.array() <= inner.min.array()).all() && (outer.max.array() >= inner.max.array()).all();
}
} // namespace

double PoissonPointCloud::nominal_spacing(double s, double h, double density)
{
    // Isotropic Poisson-Voronoi sections have L/A = 2.285851322762418 * lambda^(1/3).
    // Compensate for shallow floors using nominal extrusion dimensions, keeping
    // first-layer widths and variable layer heights out of the persistent field.
    const double width = s + h * (1. - M_PI / 4.);
    const double cutoff = width / std::hypot(width, 2. * h);
    const double tail = M_PI / 4. - 0.5 * (cutoff * std::sqrt(std::max(0., 1. - cutoff * cutoff)) + std::asin(cutoff));
    const double effective = s + (4. / M_PI) * (h * (1. - cutoff * cutoff) - s * tail);
    return 2.285851322762418 * effective / std::min(density, 1.);
}

std::vector<Vec3d> PoissonPointCloud::points_in(const BoundingBoxf3 &box, double spacing) const
{
    const double tile = 2. * spacing;
    auto sites = points_in_cells((box.min / tile).array().floor().cast<int64_t>(),
        (box.max / tile).array().ceil().cast<int64_t>(), spacing);
    sites.erase(std::remove_if(sites.begin(), sites.end(), [&](const Vec3d &point) {
        return (point.array() < box.min.array()).any() || (point.array() >= box.max.array()).any();
    }), sites.end());
    return sites;
}

std::vector<Vec3d> PoissonPointCloud::points_in_cells(const Vec3i64 &first, const Vec3i64 &last, double spacing) const
{
    const double tile = 2. * spacing;
    std::vector<Vec3d> sites;
    for (int64_t ix = first.x(); ix < last.x(); ++ix)
        for (int64_t iy = first.y(); iy < last.y(); ++iy)
            for (int64_t iz = first.z(); iz < last.z(); ++iz) {
                uint64_t seed = mix(0x564f524f4e4f49ULL ^ uint64_t(ix));
                seed = mix(seed ^ uint64_t(iy));
                seed = mix(seed ^ uint64_t(iz));
                std::mt19937_64 rng(seed);
                auto uniform = [&]() { return double(rng() >> 11) * (1. / 9007199254740992.); };
                size_t count = 0;
                double product = uniform();
                // exp(-8), fixed independently of the platform math library.
                while (product > 0.00033546262790251185) { ++count; product *= uniform(); }
                for (size_t i = 0; i < count; ++i) {
                    const double x = uniform(), y = uniform(), z = uniform();
                    const Vec3d point((double(ix) + x) * tile, (double(iy) + y) * tile, (double(iz) + z) * tile);
                    sites.push_back(point);
                }
            }
    return sites;
}

ActivePointCloud::ActivePointCloud(std::shared_ptr<const PointCloudProvider> provider) : m_provider(std::move(provider))
{
    if (!m_provider) throw std::invalid_argument("Missing Voronoi point-cloud provider");
}

double ActivePointCloud::spacing(double s, double h, double density) const
{
    return m_provider->spacing(s, h, density);
}

void ActivePointCloud::extend(const Vec3i64 &first, const Vec3i64 &last, double spacing)
{
    auto ingest = [&](const Vec3i64 &lo, const Vec3i64 &hi) {
        auto points = m_provider->points_in_cells(lo, hi, spacing);
        if (!points.empty()) ++m_revision;
        m_sites.insert(m_sites.end(), points.begin(), points.end());
    };
    if (!m_known.defined) ingest(first, last);
    else {
        Vec3i64 lo = first, hi = last;
        for (int axis = 0; axis < 3; ++axis) {
            if (lo[axis] < m_first[axis]) {
                auto end = hi;
                end[axis] = m_first[axis];
                ingest(lo, end);
                lo[axis] = m_first[axis];
            }
            if (hi[axis] > m_last[axis]) {
                auto begin = lo;
                begin[axis] = m_last[axis];
                ingest(begin, hi);
                hi[axis] = m_last[axis];
            }
        }
    }
    m_first = first;
    m_last = last;
    const double step = m_provider->cell_size(spacing);
    m_known = BoundingBoxf3(first.cast<double>() * step, last.cast<double>() * step);
}

bool ActivePointCloud::update(const BoundingBox &bounds, double z, double margin, double spacing,
                              const std::function<double(const std::vector<Vec3d> &)> &radius,
                              const std::function<void(double)> &retire)
{
    const double low = z - margin;
    if (m_spacing != spacing || m_bounds.min != bounds.min || m_bounds.max != bounds.max || low < m_low ||
        (m_known.defined && low > m_known.max.z())) {
        if (retire) retire(std::numeric_limits<double>::infinity());
        m_sites.clear();
        ++m_revision;
        m_known = BoundingBoxf3();
        m_retired = -std::numeric_limits<double>::infinity();
        m_bounds = bounds;
        m_spacing = spacing;
    }
    m_low = low;
    const auto extent = m_provider->extent();
    if (extent && !extent->defined) return false;
    const auto source_extent = extent ? std::optional<BoundingBoxf3>(m_provider->generation_bounds(*extent)) : std::nullopt;
    const double step = m_provider->cell_size(spacing);
    if (!std::isfinite(step) || step <= 0.) throw std::invalid_argument("Invalid Voronoi generation cell size");
    const Vec2d min = bounds.min.cast<double>() * SCALING_FACTOR;
    const Vec2d max = bounds.max.cast<double>() * SCALING_FACTOR;
    double reach = spacing;
    unsigned empty_expansions = 0;
    for (;;) {
        const double r = radius(m_sites);
        // A known competitor bounds nearest-site distance over the whole slab.
        // Retiring a lower half-space cannot change this or any future slice.
        if (std::isfinite(r)) reach = r + margin + 4. * EPSILON;
        BoundingBoxf3 wanted = m_provider->generation_bounds(BoundingBoxf3(
            Vec3d(min.x() - reach, min.y() - reach, low - reach),
            Vec3d(max.x() + reach, max.y() + reach, z + margin + reach)));
        wanted.min.z() = std::max(wanted.min.z(), m_retired);
        const double limit = double(std::numeric_limits<coord_t>::max()) * SCALING_FACTOR / 4.;
        if (!wanted.min.allFinite() || !wanted.max.allFinite() ||
            wanted.min.cwiseAbs().maxCoeff() > limit || wanted.max.cwiseAbs().maxCoeff() > limit) return false;
        if (m_known.defined && std::isfinite(r) && contains(m_known, wanted)) return true;
        if (!std::isfinite(r) && source_extent && contains(m_known, *source_extent) &&
            (m_known.max.array() > source_extent->max.array()).all()) return false;
        if (!std::isfinite(r) && !source_extent && ++empty_expansions > 4)
            throw std::invalid_argument("Voronoi point sampling found no control points; check infill density and modifiers");

        // Cull complete cell rows only when more cells are needed, before allocating
        // their points or inserting them into the persistent Delaunay network.
        if (m_known.defined && std::isfinite(r)) {
            const int64_t first_z = int64_t(std::floor((low - reach) / step));
            if (first_z > m_first.z()) {
                const double cutoff = double(first_z) * step;
                const auto end = std::remove_if(m_sites.begin(), m_sites.end(), [&](const Vec3d &p) { return p.z() < cutoff; });
                if (end != m_sites.end()) { m_sites.erase(end, m_sites.end()); ++m_revision; }
                if (retire) retire(cutoff);
                m_first.z() = first_z;
                m_known.min.z() = cutoff;
                m_retired = cutoff;
            }
        }
        Vec3i64 first = (wanted.min / step).array().floor().cast<int64_t>();
        Vec3i64 last = (wanted.max / step).array().ceil().cast<int64_t>();
        if (m_known.defined) {
            first = first.cwiseMin(m_first);
            last = last.cwiseMax(m_last);
            if (std::isfinite(m_retired)) first.z() = std::max(first.z(), m_first.z());
        }
        extend(first, last, spacing);
        if (!std::isfinite(r)) reach *= 2.;
    }
}

} // namespace Slic3r::Voronoi
