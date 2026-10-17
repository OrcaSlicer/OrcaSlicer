#pragma once

#include "../BoundingBox.hpp"
#include "libslic3r/Point.hpp"
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
    static double nominal_spacing(double extrusion_spacing, double nominal_height, double density);
    double spacing(double extrusion_spacing, double nominal_height, double density) const override
        { return nominal_spacing(extrusion_spacing, nominal_height, density); }
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
};

// Sample the complete domain, growing its exterior until the covering bound
// proves that unseen points cannot affect any section in the domain.
std::vector<Vec3d> complete_cloud(const PointCloudProvider &provider, const BoundingBoxf3 &domain,
                                double spacing);

} // namespace Slic3r::Voronoi
