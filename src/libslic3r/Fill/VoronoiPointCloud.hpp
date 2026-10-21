#pragma once

#include "../BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include <functional>
#include <limits>
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
    // Whole-cell streaming uses this sampling frame, which preserves print Z.
    virtual BoundingBoxf3 generation_bounds(const BoundingBoxf3 &box) const { return box; }
    virtual double cell_size(double spacing) const { return 2. * spacing; }
    virtual std::vector<Vec3d> points_in_cells(const Vec3i64 &first, const Vec3i64 &last, double spacing) const
        { return points_in(BoundingBoxf3(first.cast<double>() * cell_size(spacing), last.cast<double>() * cell_size(spacing)), spacing); }
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
    std::vector<Vec3d> points_in_cells(const Vec3i64 &first, const Vec3i64 &last, double spacing) const override;
};

// Owns ingestion and retirement, independently of the distribution and routing.
// The geometry callback bounds nearest-site distance over the XY rectangle at z.
class ActivePointCloud {
public:
    explicit ActivePointCloud(std::shared_ptr<const PointCloudProvider> provider = std::make_shared<PoissonPointCloud>());
    double spacing(double extrusion_spacing, double nominal_height, double density) const;
    bool update(const BoundingBox &bounds, double z, double margin, double spacing,
                const std::function<double(const std::vector<Vec3d> &)> &radius,
                const std::function<void(double)> &retire = {});
    const std::vector<Vec3d> &sites() const { return m_sites; }
    size_t revision() const { return m_revision; }
    const BoundingBoxf3 &known_bounds() const { return m_known; }
    double retired_below() const { return m_retired; }

private:
    void extend(const Vec3i64 &first, const Vec3i64 &last, double spacing);
    std::shared_ptr<const PointCloudProvider> m_provider;
    std::vector<Vec3d> m_sites;
    BoundingBoxf3 m_known; // Loaded whole cells in the provider's sampling frame.
    Vec3i64 m_first = Vec3i64::Zero(), m_last = Vec3i64::Zero();
    size_t m_revision = 0;
    BoundingBox m_bounds;
    double m_spacing { 0. };
    double m_low { -std::numeric_limits<double>::infinity() };
    double m_retired { -std::numeric_limits<double>::infinity() };
};

} // namespace Slic3r::Voronoi
