#include "ExclusionVolumePathCheck.hpp"

#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r {

namespace {

constexpr double Z_EPSILON_MM = 1e-6;

Point scaled_point(const Vec3d &point)
{
    return { scale_(point.x()), scale_(point.y()) };
}

BoundingBox line_bbox(const Point &a, const Point &b)
{
    BoundingBox bbox;
    bbox.merge(a);
    bbox.merge(b);
    bbox.offset(SCALED_EPSILON);
    return bbox;
}

bool intervals_overlap(coord_t a_min, coord_t a_max, coord_t b_min, coord_t b_max)
{
    if (a_min > a_max)
        std::swap(a_min, a_max);
    if (b_min > b_max)
        std::swap(b_min, b_max);
    return std::max(a_min, b_min) <= std::min(a_max, b_max);
}

bool collinear_segments_overlap(const Line &a, const Line &b)
{
    const Vec2crd a_dir = a.b - a.a;
    if (a_dir == Vec2crd::Zero())
        return a.a == b.a || a.a == b.b;

    if (cross2(a_dir, b.a - a.a) != 0 || cross2(a_dir, b.b - a.a) != 0)
        return false;

    return std::abs(a_dir.x()) >= std::abs(a_dir.y()) ?
        intervals_overlap(a.a.x(), a.b.x(), b.a.x(), b.b.x()) :
        intervals_overlap(a.a.y(), a.b.y(), b.a.y(), b.b.y());
}

bool overlaps_collinear_boundary(const Line &line, const Lines &edges)
{
    const BoundingBox line_bounds = line_bbox(line.a, line.b);
    for (const Line &edge : edges) {
        if (line_bounds.overlap(line_bbox(edge.a, edge.b)) && collinear_segments_overlap(line, edge))
            return true;
    }
    return false;
}

bool clip_segment_to_z_range(
    const Vec3d &from,
    const Vec3d &to,
    double z_min,
    double z_max,
    Vec3d &out_from,
    Vec3d &out_to)
{
    const double segment_min_z = std::min(from.z(), to.z());
    const double segment_max_z = std::max(from.z(), to.z());
    if (segment_max_z < z_min - Z_EPSILON_MM || segment_min_z > z_max + Z_EPSILON_MM)
        return false;

    double t0 = 0.0;
    double t1 = 1.0;
    const double dz = to.z() - from.z();
    if (std::abs(dz) < Z_EPSILON_MM) {
        if (from.z() < z_min - Z_EPSILON_MM || from.z() > z_max + Z_EPSILON_MM)
            return false;
    } else {
        const double z0 = (z_min - from.z()) / dz;
        const double z1 = (z_max - from.z()) / dz;
        t0 = std::max(t0, std::min(z0, z1));
        t1 = std::min(t1, std::max(z0, z1));
        if (t0 > t1 + Z_EPSILON_MM)
            return false;
    }

    t0 = std::clamp(t0, 0.0, 1.0);
    t1 = std::clamp(t1, 0.0, 1.0);
    const Vec3d delta = to - from;
    out_from = from + t0 * delta;
    out_to = from + t1 * delta;
    return true;
}

} // namespace

void ExclusionVolumePathChecker::configure(const PrintConfig &config)
{
    clear();
    m_configured = true;
    m_extruder_offsets = config.extruder_offset.values;

    const std::vector<std::vector<BedExcludeRegion>> regions =
        get_bed_excluded_regions_by_extruder(config);
    m_regions_by_extruder.resize(regions.size());
    if (m_extruder_offsets.size() < regions.size())
        m_extruder_offsets.resize(regions.size(), Vec2d::Zero());

    for (size_t extruder_id = 0; extruder_id < regions.size(); ++extruder_id) {
        std::vector<PreparedRegion> &prepared = m_regions_by_extruder[extruder_id];
        prepared.reserve(regions[extruder_id].size());
        for (const BedExcludeRegion &region : regions[extruder_id]) {
            if (region.polygon.points.size() < 3 || region.z_max < region.z_min)
                continue;

            PreparedRegion item;
            item.region = region;
            item.bbox = BoundingBox(region.polygon.points);
            item.bbox.offset(SCALED_EPSILON);
            item.edge_tree = AABBTreeLines::LinesDistancer<Line>(to_lines(region.polygon));
            prepared.emplace_back(std::move(item));
        }
    }
}

void ExclusionVolumePathChecker::clear()
{
    m_configured = false;
    m_extruder_offsets.clear();
    m_regions_by_extruder.clear();
    m_result = {};
}

bool ExclusionVolumePathChecker::segment_intersects_region(
    const Vec3d &from,
    const Vec3d &to,
    const bool z_known,
    const PreparedRegion &region) const
{
    Vec3d clipped_from = from;
    Vec3d clipped_to = to;
    if (z_known && !clip_segment_to_z_range(
            from, to, region.region.z_min, region.region.z_max, clipped_from, clipped_to))
        return false;

    const Point a = scaled_point(clipped_from);
    const Point b = scaled_point(clipped_to);
    if (!line_bbox(a, b).overlap(region.bbox))
        return false;

    if (region.region.polygon.contains(a) || region.region.polygon.contains(b))
        return true;
    if (a == b)
        return false;

    const Line line(a, b);
    if (!region.edge_tree.intersections_with_line<false>(line).empty())
        return true;

    // The line tree intentionally omits collinear intersections.
    return overlaps_collinear_boundary(line, region.edge_tree.get_lines());
}

void ExclusionVolumePathChecker::check_motion(
    const Vec3d &from,
    const Vec3d &to,
    const bool start_xy_known,
    const bool end_xy_known,
    const bool start_z_known,
    const bool end_z_known,
    const int active_extruder_id,
    const ExclusionVolumeMotionType motion_type,
    const unsigned char source_move_type,
    const unsigned int gcode_id,
    const size_t move_id)
{
    if (!m_configured || m_result.has_any_conflict || !start_xy_known || !end_xy_known ||
        m_regions_by_extruder.empty())
        return;

    const bool z_known = start_z_known && end_z_known;
    const bool active_extruder_valid =
        active_extruder_id >= 0 && size_t(active_extruder_id) < m_regions_by_extruder.size();
    const size_t first_extruder = active_extruder_valid ? size_t(active_extruder_id) : 0;
    const size_t last_extruder = active_extruder_valid ? first_extruder + 1 : m_regions_by_extruder.size();

    for (size_t extruder_id = first_extruder; extruder_id < last_extruder; ++extruder_id) {
        const Vec2d offset = extruder_id < m_extruder_offsets.size() ?
            m_extruder_offsets[extruder_id] : Vec2d::Zero();
        Vec3d model_from = from;
        Vec3d model_to = to;
        model_from.head<2>() += offset;
        model_to.head<2>() += offset;

        const std::vector<PreparedRegion> &regions = m_regions_by_extruder[extruder_id];
        for (size_t region_id = 0; region_id < regions.size(); ++region_id) {
            if (!segment_intersects_region(model_from, model_to, z_known, regions[region_id]))
                continue;

            m_result.has_any_conflict = true;
            m_result.has_travel_conflict = motion_type == ExclusionVolumeMotionType::Travel;
            m_result.has_extrusion_conflict = motion_type == ExclusionVolumeMotionType::Extrude;
            m_result.has_other_motion_conflict = motion_type == ExclusionVolumeMotionType::Other;
            m_result.first_hit = ExclusionVolumePathHit{
                move_id,
                gcode_id,
                motion_type,
                source_move_type,
                int(extruder_id),
                region_id,
                model_from,
                model_to,
                !z_known
            };
            return;
        }
    }
}

} // namespace Slic3r
