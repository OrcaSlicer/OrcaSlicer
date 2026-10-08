#pragma once

#include "../BoundingBox.hpp"
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>
#include <utility>
#include <cstddef>

namespace Slic3r::Voronoi {

// Immutable spatial field. Queries use millimeters and half-open boxes.
// Splitting or revisiting a box must produce the same sites, without duplicates.
class PointCloudProvider {
public:
    virtual ~PointCloudProvider() = default;
    virtual double spacing(double extrusion_spacing, double nominal_height, double density) const = 0;
    virtual std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const = 0;
    // Relative density for hull extraction, independent of site generation.
    // Uniform fields never fall below a threshold at or below the setting.
    virtual std::vector<double> relative_density(const std::vector<Vec3d> &points) const
        { return std::vector<double>(points.size(), 1.); }
    // A finite field can declare its complete extent, including an undefined
    // box for an empty field. An unbounded field must eventually supply points.
    virtual std::optional<BoundingBoxf3> extent() const { return std::nullopt; }
};

class PoissonPointCloud : public PointCloudProvider {
public:
    double spacing(double extrusion_spacing, double nominal_height, double density) const override;
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
};

// Signed distances in millimeters, negative inside the model. Smoothing is a
// field-construction stage, independent of candidate selection and density.
struct WallDistanceSample {
    double distance;
    double smoothed;
};
using WallDistanceSamples = std::function<std::vector<WallDistanceSample>(const std::vector<Vec3d> &)>;

class WallDistancePointCloud : public PoissonPointCloud {
public:
    static constexpr double near_wall_distance = 2.;
    explicit WallDistancePointCloud(WallDistanceSamples distance, double decay = 6.);
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double spacing) const override;
    std::vector<double> relative_density(const std::vector<Vec3d> &points) const override;
    // Relative local infill density, before cubing to obtain site intensity.
    static double density_ratio(double distance, double sampled_mean, double decay = 6.);
private:
    WallDistanceSamples m_distance;
    double m_decay;
};

// Owns ingestion and retirement, independently of the distribution and routing.
// The geometry callback bounds nearest-site distance over the XY rectangle at z.
class ActivePointCloud {
public:
    explicit ActivePointCloud(std::shared_ptr<const PointCloudProvider> provider = std::make_shared<PoissonPointCloud>());
    double spacing(double extrusion_spacing, double nominal_height, double density) const;
    bool update(const BoundingBox &bounds, double z, double margin, double spacing,
                const std::function<double(const std::vector<Vec3d> &)> &radius);
    const std::vector<Vec3d> &sites() const { return m_sites; }
    const BoundingBoxf3 &known_bounds() const { return m_known; }
    double retired_below() const { return m_retired; }

private:
    void extend(const BoundingBoxf3 &box, double spacing);
    std::shared_ptr<const PointCloudProvider> m_provider;
    std::vector<Vec3d> m_sites;
    BoundingBoxf3 m_known;
    BoundingBox m_bounds;
    double m_spacing { 0. };
    double m_low { -std::numeric_limits<double>::infinity() };
    double m_retired { -std::numeric_limits<double>::infinity() };
};

} // namespace Slic3r::Voronoi
