#pragma once

#include "CoExtrusionTypes.hpp"

#include "../Point.hpp"

#include <optional>

namespace Slic3r::CoExtrusion {

enum class DirectionIntentKind {
    OrientSurface,
    HoldPrevious,
    DisableControl,
    MissingColorSector,
    InvalidGeometry,
};

// Circular interval represented by a normalized center and a half width.
// Equivalent intervals repeat every 360 degrees.
struct AngularInterval {
    double center_deg { 0.0 };
    double half_width_deg { 0.0 };

    bool full_circle() const { return half_width_deg >= 180.0; }
    double nearest_angle(double reference_angle_deg) const;
    bool contains(double angle_deg, double epsilon_deg = 1e-9) const;
};

struct DirectionResolverConfig {
    // Positive azimuth runs from +X toward +Y. For a machine with +Y toward
    // the observer this is clockwise from above; +1 means C follows it.
    int    axis_direction { 1 };
    double axis_zero_offset_deg { 0.0 };
    double filament_calibration_offset_deg { 0.0 };
    double normal_xy_threshold { 0.05 };
    double angular_margin_deg { 0.0 };
    double primary_reference_azimuth_deg { 0.0 };
    TopBottomStrategy top_bottom_strategy { TopBottomStrategy::HoldLast };
    // Deposited cross-section dimensions, perpendicular to the path tangent.
    // Missing/invalid dimensions retain the circular model. Used by TangentFollow.
    double bead_width_mm { 0.0 };
    double bead_height_mm { 0.0 };
    // Explicit XY-only mode bypasses both the bend and ellipse correction.
    bool use_xy_normal_only { false };
};

struct DirectionResolution {
    DirectionIntentKind          kind { DirectionIntentKind::InvalidGeometry };
    std::optional<SurfaceColorId> color_id;
    double                       projected_normal_length { 0.0 };
    std::optional<double>        surface_azimuth_deg;
    std::optional<AngularInterval> acceptable_angles;
    std::optional<double>        target_angle_deg;
};

class SurfaceDirectionResolver
{
public:
    explicit SurfaceDirectionResolver(DirectionResolverConfig config) : m_config(config) {}

    DirectionResolution resolve(
        const Vec3d &surface_normal,
        const Vec2d &path_tangent,
        SurfaceColorId color_id,
        const Profile &profile,
        std::optional<double> reference_angle_deg) const;

    DirectionResolution resolve(
        const Vec3d &surface_normal,
        const Vec2d &path_tangent,
        const ColorSector &sector,
        std::optional<double> reference_angle_deg) const;

    DirectionResolution resolve(
        const Vec3d &surface_normal,
        const Vec3d &path_tangent,
        SurfaceColorId color_id,
        const Profile &profile,
        std::optional<double> reference_angle_deg) const;

    DirectionResolution resolve(
        const Vec3d &surface_normal,
        const Vec3d &path_tangent,
        const ColorSector &sector,
        std::optional<double> reference_angle_deg) const;

private:
    DirectionResolution resolve_for_azimuth(
        double azimuth_deg,
        double projected_normal_length,
        const ColorSector &sector,
        std::optional<double> reference_angle_deg) const;

    DirectionResolverConfig m_config;
};

} // namespace Slic3r::CoExtrusion
