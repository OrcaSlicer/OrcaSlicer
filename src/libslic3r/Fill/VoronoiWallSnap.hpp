#pragma once

#include "VoronoiPointCloud.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r::Voronoi {

// Move nearby control points towards the raw boundary after optional relaxation.
class SnappedPointCloud : public PointCloudProvider {
public:
    SnappedPointCloud(std::shared_ptr<const PointCloudProvider> source, WallDistanceSamples distance,
                      double site_spacing, double gradient_step);
    double spacing(double width, double height, double density) const override
        { return m_source->spacing(width, height, density); }
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
    bool has_cutoff() const override { return m_source->has_cutoff(); }
    std::vector<double> relative_density(const std::vector<Vec3d> &points) const override
        { return m_source->relative_density(points); }
    std::vector<double> local_spacing(const std::vector<Vec3d> &points, double spacing) const override
        { return m_source->local_spacing(points, spacing); }
    std::optional<BoundingBoxf3> extent() const override;
private:
    std::shared_ptr<const PointCloudProvider> m_source;
    WallDistanceSamples m_distance;
    double m_radius, m_gradient_step;
};

} // namespace Slic3r::Voronoi
