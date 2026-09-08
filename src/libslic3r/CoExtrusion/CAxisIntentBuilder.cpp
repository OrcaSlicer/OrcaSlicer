#include "CAxisIntentBuilder.hpp"

#include <utility>

namespace Slic3r::CoExtrusion {

std::vector<CAxisPathIntent> CAxisIntentBuilder::build(
    const std::vector<MatchedSurfacePathSegment> &segments,
    const ObjectSurfaceProvenanceResolver &surface_resolver,
    double slice_z_mm,
    const Profile &profile,
    std::optional<double> initial_reference_angle_deg) const
{
    std::vector<CAxisPathIntent> intents;
    intents.reserve(segments.size());
    std::optional<double> reference_angle_deg = initial_reference_angle_deg;

    for (const MatchedSurfacePathSegment &segment : segments) {
        CAxisPathIntent intent;
        intent.segment       = segment.segment;
        intent.path_start_mm = segment.path_start_mm;
        intent.path_end_mm   = segment.path_end_mm;
        intent.control_source_path_mm = segment.path_start_mm;
        intent.slice_source  = segment.surface;
        if (segment.surface)
            intent.target_color_id = segment.surface->color_id;

        if (!segment.surface) {
            intent.status = CAxisIntentStatus::NoSurfaceSource;
            intents.emplace_back(std::move(intent));
            continue;
        }
        if (!segment.surface->color_id) {
            intent.status = CAxisIntentStatus::UncoloredSurface;
            intents.emplace_back(std::move(intent));
            continue;
        }

        const Point midpoint = segment.segment.midpoint();
        const Vec3d query_point(
            unscale<double>(midpoint.x()),
            unscale<double>(midpoint.y()),
            segment.query_z_mm.value_or(slice_z_mm));
        intent.surface = surface_resolver.provenance_for_face(
            segment.surface->volume_id,
            segment.surface->face_index,
            query_point);
        if (!intent.surface) {
            intent.status = CAxisIntentStatus::MissingFaceProvenance;
            intents.emplace_back(std::move(intent));
            continue;
        }

        const Vec2d xy_delta = (segment.segment.b - segment.segment.a).cast<double>() * SCALING_FACTOR;
        const Vec3d tangent(xy_delta.x(), xy_delta.y(), segment.z_delta_mm);
        intent.direction = m_direction_resolver.resolve(
            intent.surface->surface_normal,
            tangent,
            *segment.surface->color_id,
            profile,
            reference_angle_deg);

        if (intent.direction->kind == DirectionIntentKind::OrientSurface ||
            intent.direction->kind == DirectionIntentKind::HoldPrevious) {
            intent.status = CAxisIntentStatus::Resolved;
            if (intent.direction->target_angle_deg)
                reference_angle_deg = intent.direction->target_angle_deg;
        } else {
            intent.status = CAxisIntentStatus::DirectionUnavailable;
        }
        intents.emplace_back(std::move(intent));
    }

    return intents;
}

} // namespace Slic3r::CoExtrusion
