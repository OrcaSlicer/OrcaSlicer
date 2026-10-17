#pragma once

#include "VoronoiPointCloud.hpp"
#include "../VoronoiCloudMethod.hpp"
#include <Eigen/Core>
#include <vector>

namespace Slic3r::Voronoi {

// Cubic or FCC sites with a vertical [111] axis. Ordered thinning follows density.
class LatticePointCloud : public WallDistancePointCloud {
public:
    explicit LatticePointCloud(VoronoiCloudMethod method, WallDistanceSamples distance = {}, double maximum_ratio = density_ratio(0., 2.),
                               double angle = 0., const Vec2d &center = Vec2d::Zero());
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
private:
    VoronoiCloudMethod m_method;
    bool m_variable;
    Eigen::Matrix3d m_basis, m_inverse;
};

} // namespace Slic3r::Voronoi
