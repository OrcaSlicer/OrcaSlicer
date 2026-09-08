#include "SurfaceDirectionResolver.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::CoExtrusion {

namespace {

constexpr double RAD_TO_DEG = 57.2957795130823208768;
// Keep the selected material centre within +/-5 degrees of the requested
// visible surface direction. Narrow sectors and larger safety margins may
// reduce this interval further, but it must never become wider than 5 degrees.
constexpr double CENTER_TRACKING_MAX_HALF_WIDTH_DEG = 5.0;

double azimuth_degrees(const Vec2d &direction)
{
    return normalize_degrees(std::atan2(direction.y(), direction.x()) * RAD_TO_DEG);
}

} // namespace

double AngularInterval::nearest_angle(double reference_angle_deg) const
{
    if (full_circle())
        return reference_angle_deg;

    const double unwrapped_center = unwrap_near(normalize_degrees(center_deg), reference_angle_deg);
    return std::clamp(reference_angle_deg, unwrapped_center - half_width_deg, unwrapped_center + half_width_deg);
}

bool AngularInterval::contains(double angle_deg, double epsilon_deg) const
{
    return full_circle() || std::abs(shortest_angular_distance(center_deg, angle_deg)) <= half_width_deg + epsilon_deg;
}

DirectionResolution SurfaceDirectionResolver::resolve(
    const Vec3d &surface_normal,
    const Vec2d &path_tangent,
    SurfaceColorId color_id,
    const Profile &profile,
    std::optional<double> reference_angle_deg) const
{
    return resolve(surface_normal, Vec3d(path_tangent.x(), path_tangent.y(), 0.0),
                   color_id, profile, reference_angle_deg);
}

DirectionResolution SurfaceDirectionResolver::resolve(
    const Vec3d &surface_normal,
    const Vec2d &path_tangent,
    const ColorSector &sector,
    std::optional<double> reference_angle_deg) const
{
    return resolve(surface_normal, Vec3d(path_tangent.x(), path_tangent.y(), 0.0),
                   sector, reference_angle_deg);
}

DirectionResolution SurfaceDirectionResolver::resolve(
    const Vec3d &surface_normal,
    const Vec3d &path_tangent,
    SurfaceColorId color_id,
    const Profile &profile,
    std::optional<double> reference_angle_deg) const
{
    const ColorSector *sector = profile.find_sector(color_id);
    if (sector == nullptr) {
        DirectionResolution result;
        result.kind     = DirectionIntentKind::MissingColorSector;
        result.color_id = color_id;
        return result;
    }
    return resolve(surface_normal, path_tangent, *sector, reference_angle_deg);
}

DirectionResolution SurfaceDirectionResolver::resolve(
    const Vec3d &surface_normal,
    const Vec3d &path_tangent,
    const ColorSector &sector,
    std::optional<double> reference_angle_deg) const
{
    DirectionResolution result;
    result.color_id = sector.color_id;

    if (!surface_normal.allFinite() || surface_normal.squaredNorm() <= 0.0 ||
        (m_config.axis_direction != 1 && m_config.axis_direction != -1)) {
        result.kind = DirectionIntentKind::InvalidGeometry;
        return result;
    }

    const Vec3d normal = surface_normal.normalized();
    const Vec2d projected(normal.x(), normal.y());
    result.projected_normal_length = projected.norm();

    if (m_config.use_xy_normal_only) {
        if (result.projected_normal_length > 1e-9 &&
            result.projected_normal_length >= m_config.normal_xy_threshold) {
            return resolve_for_azimuth(
                azimuth_degrees(projected), result.projected_normal_length, sector, reference_angle_deg);
        }
        // Pure top/bottom normals have no XY azimuth, even with threshold = 0.
        result.kind = DirectionIntentKind::HoldPrevious;
        result.target_angle_deg = reference_angle_deg;
        return result;
    }

    if (m_config.top_bottom_strategy == TopBottomStrategy::TangentFollow) {
        if (!path_tangent.allFinite() || path_tangent.squaredNorm() <= 0.0) {
            result.kind = DirectionIntentKind::InvalidGeometry;
            return result;
        }

        // Tangent is nozzle motion relative to the model, not bed motor motion.
        // In the ideal untwisted bend model, the deposited material trails along
        // -t. Let u be the XY heading, l its lateral direction, and b the upward
        // radial direction perpendicular to t. Undo the bend with
        // q = (n dot l) l - (n dot b) u, where b = (-t.z * u, |t.xy|).
        // Thus horizontal travel still maps bottom to +u and top to -u.
        const Vec3d travel_direction = path_tangent.normalized();
        const double horizontal_length = travel_direction.head<2>().norm();
        Vec2d cylinder_direction = Vec2d::Zero();
        double radial_normal_length = 0.0;
        if (horizontal_length > 1e-9) {
            const Vec2d heading = travel_direction.head<2>() / horizontal_length;
            const Vec2d lateral(-heading.y(), heading.x());
            const double upward_normal = horizontal_length * normal.z() -
                travel_direction.z() * projected.dot(heading);
            const double lateral_normal = projected.dot(lateral);
            cylinder_direction = lateral_normal * lateral - upward_normal * heading;
            // Keep the geometric degeneracy threshold independent of bead size
            // and flattening; weighting must not amplify a nearly axial normal.
            radial_normal_length = cylinder_direction.norm();
            if (std::isfinite(m_config.bead_width_mm) && m_config.bead_width_mm > 0.0 &&
                std::isfinite(m_config.bead_height_mm) && m_config.bead_height_mm > 0.0) {
                // For an affinely flattened circular color distribution, the
                // visible projected area is weighted by H*n_l and W*n_b.
                // Equivalent to q = n_l*l - (W/H)*n_b*u; common scaling avoids
                // a large W/H while preserving the azimuth. This optimizes the
                // visible sector area, not the normal of a single ellipse point.
                const double size = std::max(m_config.bead_width_mm, m_config.bead_height_mm);
                cylinder_direction = (m_config.bead_height_mm / size) * lateral_normal * lateral -
                    (m_config.bead_width_mm / size) * upward_normal * heading;
            }
        } else if (travel_direction.z() > 0.0) {
            // Upward travel leaves the trailing filament aligned with the
            // downward nozzle flow. Downward travel needs an undefined U-turn;
            // leave q zero in that case and retain the previous orientation.
            cylinder_direction = projected;
            radial_normal_length = projected.norm();
        }
        result.projected_normal_length = radial_normal_length;
        if (result.projected_normal_length > 1e-9 &&
            result.projected_normal_length >= m_config.normal_xy_threshold) {
            return resolve_for_azimuth(
                azimuth_degrees(cylinder_direction),
                result.projected_normal_length,
                sector,
                reference_angle_deg);
        }

        // A near-zero result means that the supplied normal is effectively parallel to the
        // deposited cylinder axis, so it cannot define a radial colour direction.
        result.kind = DirectionIntentKind::HoldPrevious;
        result.target_angle_deg = reference_angle_deg;
        return result;
    }

    if (result.projected_normal_length >= m_config.normal_xy_threshold) {
        return resolve_for_azimuth(
            azimuth_degrees(projected), result.projected_normal_length, sector, reference_angle_deg);
    }

    switch (m_config.top_bottom_strategy) {
    case TopBottomStrategy::HoldLast:
        result.kind = DirectionIntentKind::HoldPrevious;
        result.target_angle_deg = reference_angle_deg;
        return result;
    case TopBottomStrategy::DisableControl:
        result.kind = DirectionIntentKind::DisableControl;
        return result;
    case TopBottomStrategy::PrimaryColor:
        return resolve_for_azimuth(
            normalize_degrees(m_config.primary_reference_azimuth_deg),
            result.projected_normal_length,
            sector,
            reference_angle_deg);
    case TopBottomStrategy::TangentFollow:
        break;
    }

    result.kind = DirectionIntentKind::InvalidGeometry;
    return result;
}

DirectionResolution SurfaceDirectionResolver::resolve_for_azimuth(
    double azimuth_deg,
    double projected_normal_length,
    const ColorSector &sector,
    std::optional<double> reference_angle_deg) const
{
    DirectionResolution result;
    result.kind                    = DirectionIntentKind::OrientSurface;
    result.color_id                = sector.color_id;
    result.projected_normal_length = projected_normal_length;
    result.surface_azimuth_deg     = normalize_degrees(azimuth_deg);

    // All azimuths use +X = 0 and +X toward +Y as positive (clockwise from
    // above on the specified +Y-toward-observer machine). Physical sector
    // azimuth = sector center + installation calibration +
    // mechanical zero offset + axis_direction * commanded angle.
    const double command_center = double(m_config.axis_direction) *
        (*result.surface_azimuth_deg - sector.center_angle_deg -
         m_config.filament_calibration_offset_deg - m_config.axis_zero_offset_deg);
    const double normalized_command_center = normalize_degrees(command_center);

    // Stay in the middle portion of the selected material sector. The narrow interval avoids
    // edge contamination while still suppressing tiny C-axis corrections on curved walls.
    const double safe_half_width = std::max(
        0.0, 0.5 * sector.angular_width_deg - std::max(0.0, m_config.angular_margin_deg));
    const double centered_half_width = std::min(
        safe_half_width, CENTER_TRACKING_MAX_HALF_WIDTH_DEG);
    result.acceptable_angles = AngularInterval { normalized_command_center, centered_half_width };
    result.target_angle_deg = reference_angle_deg ?
        result.acceptable_angles->nearest_angle(*reference_angle_deg) :
        normalized_command_center;
    return result;
}

} // namespace Slic3r::CoExtrusion
