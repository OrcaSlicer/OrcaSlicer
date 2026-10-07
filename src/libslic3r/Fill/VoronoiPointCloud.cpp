#include "VoronoiPointCloud.hpp"
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

double PoissonPointCloud::spacing(double s, double h, double density) const
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
    const Vec3i64 first = (box.min / tile).array().floor().cast<int64_t>();
    const Vec3i64 last = (box.max / tile).array().ceil().cast<int64_t>();
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
                while (product > std::exp(-8.)) { ++count; product *= uniform(); }
                for (size_t i = 0; i < count; ++i) {
                    const double x = uniform(), y = uniform(), z = uniform();
                    const Vec3d point((double(ix) + x) * tile, (double(iy) + y) * tile, (double(iz) + z) * tile);
                    if ((point.array() >= box.min.array()).all() && (point.array() < box.max.array()).all())
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

void ActivePointCloud::extend(const BoundingBoxf3 &box, double spacing)
{
    auto ingest = [&](const BoundingBoxf3 &part) {
        auto points = m_provider->points_in(part, spacing);
        m_sites.insert(m_sites.end(), points.begin(), points.end());
    };
    if (!m_known.defined) {
        ingest(box);
    } else {
        // Six disjoint slabs cover the newly known shell. Do not regenerate
        // the overlap or impose a tiling scheme on the provider.
        BoundingBoxf3 overlap = box;
        for (int axis = 0; axis < 3; ++axis) {
            if (overlap.min[axis] < m_known.min[axis]) {
                BoundingBoxf3 part = overlap;
                part.max[axis] = m_known.min[axis];
                ingest(part);
                overlap.min[axis] = m_known.min[axis];
            }
            if (overlap.max[axis] > m_known.max[axis]) {
                BoundingBoxf3 part = overlap;
                part.min[axis] = m_known.max[axis];
                ingest(part);
                overlap.max[axis] = m_known.max[axis];
            }
        }
    }
    m_known = box;
}

bool ActivePointCloud::update(const BoundingBox &bounds, double z, double margin, double spacing,
                              const std::function<double(const std::vector<Vec3d> &)> &radius)
{
    const double low = z - margin;
    if (m_spacing != spacing || m_bounds.min != bounds.min || m_bounds.max != bounds.max || low < m_low ||
        (m_known.defined && low > m_known.max.z())) {
        m_sites.clear();
        m_known = BoundingBoxf3();
        m_retired = -std::numeric_limits<double>::infinity();
        m_bounds = bounds;
        m_spacing = spacing;
    }
    m_low = low;
    const auto extent = m_provider->extent();
    if (extent && !extent->defined) return false;
    const Vec2d min = bounds.min.cast<double>() * SCALING_FACTOR;
    const Vec2d max = bounds.max.cast<double>() * SCALING_FACTOR;
    double reach = spacing;
    for (;;) {
        BoundingBoxf3 wanted(Vec3d(min.x() - reach, min.y() - reach, low - reach),
                             Vec3d(max.x() + reach, max.y() + reach, z + margin + reach));
        wanted.min.z() = std::max(wanted.min.z(), m_retired);
        if (m_known.defined) {
            wanted.min = wanted.min.cwiseMin(m_known.min);
            wanted.max = wanted.max.cwiseMax(m_known.max);
        }
        const double limit = double(std::numeric_limits<coord_t>::max()) * SCALING_FACTOR / 4.;
        if (!wanted.min.allFinite() || !wanted.max.allFinite() ||
            wanted.min.cwiseAbs().maxCoeff() > limit || wanted.max.cwiseAbs().maxCoeff() > limit)
            return false;
        if (!m_known.defined || !contains(m_known, wanted)) extend(wanted, spacing);
        const double r = radius(m_sites);
        if (!std::isfinite(r)) {
            if (extent && contains(m_known, *extent) &&
                (m_known.max.array() > extent->max.array()).all()) return false;
            reach *= 2.;
            continue;
        }
        // Nearest-site distance is 1-Lipschitz in Z. This covers every point
        // of the working slab, with a fixed-point margin for rounding.
        const double required = r + margin + 4. * EPSILON;
        BoundingBoxf3 coverage(Vec3d(min.x() - required, min.y() - required, low - required),
                               Vec3d(max.x() + required, max.y() + required, z + margin + required));
        coverage.min.z() = std::max(coverage.min.z(), m_retired);
        if (!contains(m_known, coverage)) {
            reach = required;
            continue;
        }
        // At low, every XY position has a competitor within required, whose
        // Z is above cutoff. A lower site is farther there, and its squared
        // distance disadvantage grows with Z. The entire lower half-space
        // therefore stays irrelevant, even when more points are introduced.
        const double cutoff = low - required;
        m_retired = std::max(m_retired, cutoff);
        m_known.min.z() = m_retired;
        m_known.max.z() = std::min(m_known.max.z(), z + margin + required);
        m_sites.erase(std::remove_if(m_sites.begin(), m_sites.end(),
                                    [&](const Vec3d &point) {
                                        return point.z() < m_retired || point.z() >= m_known.max.z();
                                    }), m_sites.end());
        return true;
    }
}

} // namespace Slic3r::Voronoi
