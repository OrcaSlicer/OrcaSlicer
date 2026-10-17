#pragma once

#include "../BoundingBox.hpp"
#include <memory>
#include <optional>
#include <vector>
#include <cstddef>

namespace Slic3r::Voronoi {

// Immutable spatial field. Queries use millimeters and half-open boxes.
// Splitting or revisiting a box must produce the same sites, without duplicates.
class PointCloudProvider {
public:
    virtual ~PointCloudProvider() = default;
    virtual double spacing(double extrusion_spacing, double nominal_height, double density) const = 0;
    virtual std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const = 0;
    // A finite field can declare its complete extent, including an undefined
    // box for an empty field. An unbounded field must eventually supply points.
    virtual std::optional<BoundingBoxf3> extent() const { return std::nullopt; }
};

class PoissonPointCloud : public PointCloudProvider {
public:
    explicit PoissonPointCloud(double angle = 0., const Vec2d &center = Vec2d::Zero());
    double spacing(double extrusion_spacing, double nominal_height, double density) const override;
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
protected:
    // Source coordinates retain candidate identity through rotation and thinning.
    std::vector<Vec3d> candidates_in(const BoundingBoxf3 &box, double spacing) const;
    Vec3d rotate(const Vec3d &point, bool inverse = false) const;
private:
    double m_cos, m_sin;
    Vec2d m_center;
};

// Sample the complete domain, growing its exterior until the covering bound
// proves that unseen points cannot affect any section in the domain.
std::vector<Vec3d> complete_cloud(const PointCloudProvider &provider, const BoundingBoxf3 &domain,
                                double spacing);

} // namespace Slic3r::Voronoi
