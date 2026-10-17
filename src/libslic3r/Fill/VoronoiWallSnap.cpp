#include "VoronoiWallSnap.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {

SnappedPointCloud::SnappedPointCloud(std::shared_ptr<const PointCloudProvider> source, WallDistanceSamples distance,
                                   double site_spacing, double gradient_step)
    : m_source(std::move(source)), m_distance(std::move(distance)),
      m_radius(site_spacing / 2.), m_gradient_step(gradient_step)
{
    if (!m_source || !m_distance || !std::isfinite(site_spacing) || site_spacing <= 0. ||
        !std::isfinite(gradient_step) || gradient_step <= 0.)
        throw std::invalid_argument("Invalid Voronoi boundary snapping");
}

std::optional<BoundingBoxf3> SnappedPointCloud::extent() const
{
    auto box = m_source->extent();
    if (box && box->defined) {
        box->min.array() -= m_radius;
        box->max.array() += m_radius;
    }
    return box;
}

std::vector<Vec3d> SnappedPointCloud::points_in(const BoundingBoxf3 &box, double spacing) const
{
    const BoundingBoxf3 expanded(box.min - Vec3d::Constant(m_radius), box.max + Vec3d::Constant(m_radius));
    auto sites = m_source->points_in(expanded, spacing);
    if (sites.empty()) return sites;
    const auto samples = m_distance(sites);
    if (samples.size() != sites.size()) throw std::invalid_argument("Invalid Voronoi snapping samples");
    std::vector<size_t> eligible;
    std::vector<Vec3d> probes;
    for (size_t i = 0; i < sites.size(); ++i)
        if (std::abs(samples[i].distance) <= m_radius && samples[i].distance != 0.) {
            eligible.push_back(i);
            for (int axis = 0; axis < 3; ++axis) {
                Vec3d offset = Vec3d::Zero();
                offset[axis] = m_gradient_step;
                probes.push_back(sites[i] - offset);
                probes.push_back(sites[i] + offset);
            }
        }
    const auto neighbours = m_distance(probes);
    if (neighbours.size() != probes.size()) throw std::invalid_argument("Invalid Voronoi snapping gradients");
    std::vector<Vec3d> projected;
    projected.reserve(eligible.size());
    for (size_t i = 0; i < eligible.size(); ++i) {
        Vec3d gradient;
        for (int axis = 0; axis < 3; ++axis)
            gradient[axis] = (neighbours[6 * i + 2 * axis + 1].distance -
                              neighbours[6 * i + 2 * axis].distance) / (2. * m_gradient_step);
        const double norm = gradient.squaredNorm();
        Vec3d step = Vec3d::Zero();
        if (std::isfinite(norm) && norm > 1e-8) {
            step = gradient * (samples[eligible[i]].distance / norm);
            const double length = step.norm();
            if (length > m_radius) step *= m_radius / length;
        }
        projected.push_back(sites[eligible[i]] - step);
    }
    const auto distances = m_distance(projected);
    if (distances.size() != projected.size()) throw std::invalid_argument("Invalid Voronoi projected samples");
    for (size_t i = 0; i < eligible.size(); ++i)
        if (std::abs(distances[i].distance) < std::abs(samples[eligible[i]].distance))
            sites[eligible[i]] = projected[i];
    sites.erase(std::remove_if(sites.begin(), sites.end(), [&](const auto &p) {
        return (p.array() < box.min.array()).any() || (p.array() >= box.max.array()).any();
    }), sites.end());
    return sites;
}

} // namespace Slic3r::Voronoi
