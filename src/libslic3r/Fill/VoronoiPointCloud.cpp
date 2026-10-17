#include "VoronoiPointCloud.hpp"
#include "../KDTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include <algorithm>
#include <cmath>
#include <math.h>
#include <cstddef>
#include <random>
#include <limits>
#include <stdexcept>
#include <cstdint>
#include <vector>

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
                // exp(-8), fixed independently of the platform math library.
                while (product > 0.00033546262790251185) { ++count; product *= uniform(); }
                for (size_t i = 0; i < count; ++i) {
                    const double x = uniform(), y = uniform(), z = uniform();
                    const Vec3d point((double(ix) + x) * tile, (double(iy) + y) * tile, (double(iz) + z) * tile);
                    if ((point.array() >= box.min.array()).all() && (point.array() < box.max.array()).all())
                        sites.push_back(point);
                }
            }
    return sites;
}

std::vector<Vec3d> complete_cloud(const PointCloudProvider &provider, const BoundingBoxf3 &domain,
                                double spacing)
{
    const auto extent = provider.extent();
    if (extent && !extent->defined) return {};
    const Vec3i64 intervals = (domain.size() / spacing).array().ceil().max(1.).cast<int64_t>();
    const Vec3d step = domain.size().cwiseQuotient(intervals.cast<double>());
    double reach = spacing;
    for (;;) {
        const BoundingBoxf3 known(domain.min - Vec3d::Constant(reach), domain.max + Vec3d::Constant(reach));
        const double limit = double(std::numeric_limits<coord_t>::max()) * SCALING_FACTOR / 4.;
        if (!known.min.allFinite() || !known.max.allFinite() ||
            known.min.cwiseAbs().maxCoeff() > limit || known.max.cwiseAbs().maxCoeff() > limit) return {};
        auto sites = provider.points_in(known, spacing);
        double required = std::numeric_limits<double>::infinity();
        if (!sites.empty()) {
            auto coordinate = [&sites](size_t i, size_t axis) { return sites[i][axis]; };
            KDTreeIndirect<3, double, decltype(coordinate)> nearest(coordinate, sites.size());
            required = 0.;
            for (int64_t x = 0; x <= intervals.x(); ++x)
                for (int64_t y = 0; y <= intervals.y(); ++y)
                    for (int64_t z = 0; z <= intervals.z(); ++z) {
                        const Vec3d p = domain.min + step.cwiseProduct(Vec3d(x, y, z));
                        required = std::max(required, (p - sites[find_closest_point(nearest, p)]).norm());
                    }
            // Every domain point is within half a grid diagonal of a sample.
            required += 0.5 * step.norm() + 4. * EPSILON;
        }
        if (std::isfinite(required) && required <= reach) return sites;
        if (!std::isfinite(required) && extent && contains(known, *extent) &&
            (known.max.array() > extent->max.array()).all()) return {};
        reach = std::isfinite(required) ? required : 2. * reach;
    }
}

} // namespace Slic3r::Voronoi
