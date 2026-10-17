#include "VoronoiLattice.hpp"
#include <Eigen/LU>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace Slic3r::Voronoi {
namespace {
// Order the four FCC parity classes in orthogonal half-cube coordinates,
// then split each simple-cubic class into its two coarser FCC checkerboards.
// Repeat at doubled spacing; the period is 256 primitive lattice steps.
double threshold(int64_t i, int64_t j, int64_t k, VoronoiCloudMethod method)
{
    uint32_t rank = 0;
    for (int bit = 0; bit < 8; ++bit) {
        const uint32_t a = (uint64_t(i) >> bit) & 1;
        const uint32_t b = (uint64_t(j) >> bit) & 1;
        const uint32_t c = (uint64_t(k) >> bit) & 1;
        // (u,v,w) = (i-j-k, i, k-j), with u+v+w even.
        const uint32_t atom = ((a ^ b ^ c) << 1) | a;
        const uint32_t checker = a ^ b ^ uint32_t(atom != 0);
        // Cubic Bayer order starts with a checkerboard and repeats at double spacing.
        const uint32_t digit = method == VoronoiCloudMethod::Fcc ? (atom << 1) | checker :
            ((a ^ b ^ c) << 2) | ((a ^ b) << 1) | a;
        rank = (rank << 3) | digit;
    }
    return (double(rank) + 0.5) / 16777216.;
}
} // namespace

LatticePointCloud::LatticePointCloud(VoronoiCloudMethod method, WallDistanceSamples distance, double maximum_ratio, double angle, const Vec2d &center, double hull_threshold)
    : WallDistancePointCloud(distance ? distance : [](const auto &points) {
          return std::vector<WallDistanceSample>(points.size(), {-2., 1.});
      }, maximum_ratio, angle, center, hull_threshold), m_method(method), m_variable(bool(distance))
{
    if (method != VoronoiCloudMethod::Fcc && method != VoronoiCloudMethod::Cubic)
        throw std::invalid_argument("Invalid Voronoi lattice method");
    Eigen::Matrix3d primitive, orientation;
    primitive << 1., -1., -1., 1., 0., 0., 0., -1., 1.;
    orientation.row(0) = Vec3d(1., -1., 0.).normalized();
    orientation.row(1) = Vec3d(1., 1., -2.).normalized();
    orientation.row(2) = Vec3d(1., 1., 1.).normalized();
    if (method == VoronoiCloudMethod::Fcc) m_basis = orientation * primitive / std::sqrt(2.);
    else m_basis = orientation;
    m_inverse = m_basis.inverse();
}

std::vector<Vec3d> LatticePointCloud::points_in(const BoundingBoxf3 &box, double spacing) const
{
    if (!std::isfinite(spacing) || spacing <= 0.) throw std::invalid_argument("Invalid lattice spacing");
    const double maximum = m_variable ? m_maximum_ratio : 1.;
    // Match Poisson site intensity: FCC primitive volume is a^3 / sqrt(2), cubic a^3.
    const double edge = (m_method == VoronoiCloudMethod::Fcc ? std::pow(2., 1. / 6.) : 1.) * spacing / maximum;
    BoundingBoxf3 lattice;
    for (double x : {box.min.x(), box.max.x()})
        for (double y : {box.min.y(), box.max.y()})
            for (double z : {box.min.z(), box.max.z()})
                lattice.merge(m_inverse * rotate(Vec3d(x, y, z), true) / edge);
    const Vec3i64 first = lattice.min.array().floor().cast<int64_t>().matrix() - Vec3i64::Ones();
    const Vec3i64 last = lattice.max.array().ceil().cast<int64_t>().matrix() + Vec3i64::Ones();
    std::vector<Vec3d> points;
    std::vector<double> draws;
    for (int64_t i = first.x(); i <= last.x(); ++i)
        for (int64_t j = first.y(); j <= last.y(); ++j)
            for (int64_t k = first.z(); k <= last.z(); ++k) {
                const Vec3d p = rotate(m_basis * Vec3d(double(i), double(j), double(k)) * edge);
                if ((p.array() >= box.min.array()).all() && (p.array() < box.max.array()).all()) {
                    points.push_back(p);
                    if (m_variable) draws.push_back(threshold(i, j, k, m_method));
                }
            }
    if (!m_variable || points.empty()) return points;
    const auto samples = m_samples(points);
    if (samples.size() != points.size()) throw std::invalid_argument("Invalid lattice density samples");
    std::vector<Vec3d> retained;
    for (size_t i = 0; i < points.size(); ++i) {
        const double ratio = samples[i].density / maximum;
        if (draws[i] < ratio * ratio * ratio) retained.push_back(points[i]);
    }
    return retained;
}

} // namespace Slic3r::Voronoi
