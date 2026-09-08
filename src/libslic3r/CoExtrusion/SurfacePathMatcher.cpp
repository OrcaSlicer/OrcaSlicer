#include "SurfacePathMatcher.hpp"
#include "SurfaceProvenance.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r::CoExtrusion {

namespace {

template<typename MatchFn>
std::vector<MatchedSurfacePathSegment> split_and_match(
    const ExtrusionPath &path,
    double max_segment_length_mm,
    MatchFn &&match_fn)
{
    std::vector<MatchedSurfacePathSegment> matched;
    if (path.polyline.points.size() < 2)
        return matched;

    const auto *sloped = dynamic_cast<const ExtrusionPathSloped*>(&path);
    const double total_path_length_mm = path.polyline.length() * SCALING_FACTOR;
    double path_distance_mm = 0.0;
    for (size_t point_index = 1; point_index < path.polyline.points.size(); ++point_index) {
        const Point3 &from3 = path.polyline.points[point_index - 1];
        const Point3 &to3   = path.polyline.points[point_index];
        const Point from(from3.x(), from3.y());
        const Point to(to3.x(), to3.y());
        const Vec2d delta = (to - from).cast<double>();
        const double length_mm = unscale<double>(delta.norm());
        if (length_mm <= 0.0)
            continue;

        const size_t subdivisions = max_segment_length_mm > 0.0 ?
            std::max<size_t>(1, size_t(std::ceil(length_mm / max_segment_length_mm))) : 1;
        Point segment_start = from;
        double segment_start_z_mm = unscale<double>(from3.z());
        for (size_t subdivision = 1; subdivision <= subdivisions; ++subdivision) {
            const double ratio = double(subdivision) / double(subdivisions);
            const Point segment_end(
                coord_t(std::llround(double(from.x()) + ratio * delta.x())),
                coord_t(std::llround(double(from.y()) + ratio * delta.y())));
            const double segment_end_z_mm = unscale<double>(from3.z()) +
                ratio * unscale<double>(to3.z() - from3.z());
            if (segment_start != segment_end) {
                const Line segment(segment_start, segment_end);
                const double segment_length_mm = unscale<double>((segment_end - segment_start).cast<double>().norm());
                const double segment_midpoint_path_mm = path_distance_mm + 0.5 * segment_length_mm;
                const double segment_midpoint_z_offset_mm = 0.5 * (segment_start_z_mm + segment_end_z_mm);
                auto [query_z_mm, surface] = match_fn(
                    segment, segment_midpoint_path_mm, segment_midpoint_z_offset_mm);
                double z_delta_mm = path.z_contoured ? segment_end_z_mm - segment_start_z_mm : 0.0;
                if (!path.z_contoured && sloped != nullptr && total_path_length_mm > 0.0) {
                    const double start_ratio = std::clamp(path_distance_mm / total_path_length_mm, 0.0, 1.0);
                    const double end_ratio = std::clamp(
                        (path_distance_mm + segment_length_mm) / total_path_length_mm, 0.0, 1.0);
                    z_delta_mm = path.height *
                        (sloped->interpolate(end_ratio).z_ratio - sloped->interpolate(start_ratio).z_ratio);
                }
                matched.push_back({
                    segment,
                    path_distance_mm,
                    path_distance_mm + segment_length_mm,
                    query_z_mm,
                    std::move(surface),
                    z_delta_mm
                });
                path_distance_mm += segment_length_mm;
            }
            segment_start = segment_end;
            segment_start_z_mm = segment_end_z_mm;
        }
    }
    return matched;
}

std::optional<SurfaceSliceMatch> surface_match_from_provenance(
    const std::optional<SurfaceProvenance> &provenance,
    ExtrusionRole role)
{
    if (!provenance || !provenance->color_id)
        return std::nullopt;

    if ((role == erTopSolidInfill && provenance->surface_normal.z() < 0.5) ||
        (role == erBottomSurface && provenance->surface_normal.z() > -0.5))
        return std::nullopt;

    SurfaceSliceMatch match;
    match.volume_id     = provenance->volume_id;
    match.face_index    = provenance->face_index;
    match.color_id      = provenance->color_id;
    match.nearest_point = Point(
        scale_(provenance->surface_point.x()),
        scale_(provenance->surface_point.y()));
    match.distance_mm   = provenance->distance_mm;
    return match;
}

} // namespace

std::vector<MatchedSurfacePathSegment> SurfacePathMatcher::match_external_path(
    const SurfaceSliceSidecar &sidecar,
    size_t layer_index,
    const ExtrusionPath &path,
    double max_segment_length_mm,
    double max_source_distance_mm,
    double min_direction_alignment)
{
    if ((path.role() != erExternalPerimeter && path.role() != erOverhangPerimeter) || path.polyline.points.size() < 2)
        return {};

    return split_and_match(path, max_segment_length_mm,
        [&](const Line &segment, double, double) {
            return std::make_pair(
                std::optional<double>{},
                sidecar.match_segment(layer_index, segment, max_source_distance_mm, min_direction_alignment));
    });
}

std::vector<MatchedSurfacePathSegment> SurfacePathMatcher::match_horizontal_surface_path(
    const ObjectSurfaceProvenanceResolver &surface_resolver,
    double slice_z_mm,
    const ExtrusionPath &path,
    double max_segment_length_mm,
    double max_surface_distance_mm)
{
    const bool is_top = path.role() == erTopSolidInfill;
    const bool is_bottom = path.role() == erBottomSurface;
    if ((!is_top && !is_bottom) || path.polyline.points.size() < 2)
        return {};

    return split_and_match(path, max_segment_length_mm, [&](const Line &segment, double, double) {
        const Point midpoint = segment.midpoint();
        const Vec3d query_point(
            unscale<double>(midpoint.x()),
            unscale<double>(midpoint.y()),
            slice_z_mm);
        return std::make_pair(
            std::optional<double>(slice_z_mm),
            surface_match_from_provenance(
                surface_resolver.nearest_provenance(query_point, max_surface_distance_mm), path.role()));
    });
}

std::vector<MatchedSurfacePathSegment> SurfacePathMatcher::match_nonplanar_surface_path(
    const ObjectSurfaceProvenanceResolver &surface_resolver,
    double object_print_z_mm,
    const ExtrusionPath &path,
    double max_segment_length_mm,
    double max_surface_distance_mm)
{
    const bool supported_role = path.role() == erExternalPerimeter ||
        path.role() == erOverhangPerimeter || path.role() == erTopSolidInfill ||
        path.role() == erBottomSurface;
    const ExtrusionPathSloped *sloped = dynamic_cast<const ExtrusionPathSloped*>(&path);
    if (!supported_role || path.polyline.points.size() < 2 || (!path.z_contoured && sloped == nullptr))
        return {};

    const double total_path_length_mm = path.polyline.length() * SCALING_FACTOR;
    return split_and_match(path, max_segment_length_mm,
        [&](const Line &segment, double midpoint_path_mm, double midpoint_z_offset_mm) {
            double query_z_mm = object_print_z_mm + midpoint_z_offset_mm;
            if (!path.z_contoured && sloped != nullptr && total_path_length_mm > 0.0) {
                const double ratio = std::clamp(midpoint_path_mm / total_path_length_mm, 0.0, 1.0);
                const double z_ratio = sloped->interpolate(ratio).z_ratio;
                query_z_mm = object_print_z_mm - path.height + z_ratio * path.height;
            }

            const Point midpoint = segment.midpoint();
            const Vec3d query_point(
                unscale<double>(midpoint.x()),
                unscale<double>(midpoint.y()),
                query_z_mm);
            return std::make_pair(
                std::optional<double>(query_z_mm),
                surface_match_from_provenance(
                    surface_resolver.nearest_provenance(query_point, max_surface_distance_mm), path.role()));
        });
}

} // namespace Slic3r::CoExtrusion
