#pragma once

#include "VoronoiPointCloud.hpp"
#include "../BoundingBox.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r::Voronoi {

// An optional preparation stage, independent of point generation and slicing.
// Layer queries read the same saved constellation throughout the object.
class RelaxedPointCloud : public PointCloudProvider {
public:
    RelaxedPointCloud(std::shared_ptr<const PointCloudProvider> source, const BoundingBoxf3 &bounds,
                      double site_spacing, int iterations, const std::function<void(double)> &progress = {});
    double spacing(double width, double height, double density) const override
        { return m_source->spacing(width, height, density); }
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
    std::vector<double> relative_density(const std::vector<Vec3d> &points) const override
        { return m_source->relative_density(points); }
    std::vector<double> local_spacing(const std::vector<Vec3d> &points, double spacing) const override
        { return m_source->local_spacing(points, spacing); }
    std::optional<BoundingBoxf3> extent() const override { return m_bounds; }
private:
    std::shared_ptr<const PointCloudProvider> m_source;
    BoundingBoxf3 m_bounds;
    std::vector<Vec3d> m_sites;
};

} // namespace Slic3r::Voronoi
