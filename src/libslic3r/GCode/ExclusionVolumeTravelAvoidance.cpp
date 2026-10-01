#include "ExclusionVolumeTravelAvoidance.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExclusionVolumeGeometry.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polygon.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r {

namespace {

constexpr double T_EPSILON = 1e-9;
constexpr size_t MAX_REROUTE_ITERATIONS = 16;

struct SegmentIntersection
{
    double t { 0.0 };
    Point point;
    size_t edge_idx { 0 };
};

double boundary_epsilon()
{
    return std::max<double>(SCALED_EPSILON, scale_(0.005));
}

bool line_intersection_t(
    const Point &a,
    const Point &b,
    const Point &c,
    const Point &d,
    double &t,
    Point &intersection)
{
    const Vec2d p = a.cast<double>();
    const Vec2d r = (b - a).cast<double>();
    const Vec2d q = c.cast<double>();
    const Vec2d s = (d - c).cast<double>();
    const double denominator = cross2(r, s);
    if (std::abs(denominator) < T_EPSILON)
        return false;

    const Vec2d qp = q - p;
    const double candidate_t = cross2(qp, s) / denominator;
    const double u = cross2(qp, r) / denominator;
    if (candidate_t < -T_EPSILON || candidate_t > 1.0 + T_EPSILON ||
        u < -T_EPSILON || u > 1.0 + T_EPSILON)
        return false;

    t = std::clamp(candidate_t, 0.0, 1.0);
    intersection = (p + t * r).cast<coord_t>();
    return true;
}

template<typename Fn>
void for_each_expolygon_contour(const ExPolygon &expolygon, Fn &&fn)
{
    fn(expolygon.contour);
    for (const Polygon &hole : expolygon.holes)
        fn(hole);
}

void add_unique_param(std::vector<double> &params, double t)
{
    const double clamped = std::clamp(t, 0.0, 1.0);
    if (std::none_of(params.begin(), params.end(), [clamped](double value) {
            return std::abs(value - clamped) < T_EPSILON;
        }))
        params.emplace_back(clamped);
}

std::vector<SegmentIntersection> contour_intersections(
    const Point &a,
    const Point &b,
    const Polygon &polygon)
{
    std::vector<SegmentIntersection> intersections;
    if (polygon.points.size() < 2)
        return intersections;

    intersections.reserve(4);
    for (size_t edge_idx = 0; edge_idx < polygon.points.size(); ++edge_idx) {
        const Point &c = polygon.points[edge_idx];
        const Point &d = polygon.points[(edge_idx + 1) % polygon.points.size()];
        double t = 0.0;
        Point point;
        if (line_intersection_t(a, b, c, d, t, point) &&
            std::none_of(intersections.begin(), intersections.end(), [t](const SegmentIntersection &other) {
                return std::abs(other.t - t) < T_EPSILON;
            }))
            intersections.push_back({ t, point, edge_idx });
    }

    std::sort(intersections.begin(), intersections.end(), [](const SegmentIntersection &lhs, const SegmentIntersection &rhs) {
        return lhs.t < rhs.t;
    });
    return intersections;
}

bool point_in_expolygons(const ExPolygons &expolygons, const Point &point, bool border_result)
{
    return std::any_of(expolygons.begin(), expolygons.end(), [&point, border_result](const ExPolygon &expolygon) {
        return expolygon.contains(point, border_result);
    });
}

bool point_on_expolygon_boundary(const ExPolygon &expolygon, const Point &point)
{
    const double epsilon = boundary_epsilon();
    if (expolygon.contour.on_boundary(point, epsilon))
        return true;
    return std::any_of(expolygon.holes.begin(), expolygon.holes.end(), [&point, epsilon](const Polygon &hole) {
        return hole.on_boundary(point, epsilon);
    });
}

bool point_on_expolygons_boundary(const ExPolygons &expolygons, const Point &point)
{
    return std::any_of(expolygons.begin(), expolygons.end(), [&point](const ExPolygon &expolygon) {
        return point_on_expolygon_boundary(expolygon, point);
    });
}

bool point_in_expolygons_interior(const ExPolygons &expolygons, const Point &point)
{
    return point_in_expolygons(expolygons, point, false) &&
           !point_on_expolygons_boundary(expolygons, point);
}

std::vector<double> boundary_params_for_segment(
    const Point &a,
    const Point &b,
    const ExPolygons &expolygons)
{
    std::vector<double> params { 0.0, 1.0 };
    for (const ExPolygon &expolygon : expolygons) {
        for_each_expolygon_contour(expolygon, [&](const Polygon &contour) {
            for (const SegmentIntersection &intersection : contour_intersections(a, b, contour))
                add_unique_param(params, intersection.t);
        });
    }
    std::sort(params.begin(), params.end());
    return params;
}

Point interpolate(const Point &a, const Point &b, double t)
{
    return (a.cast<double>() + t * (b - a).cast<double>()).cast<coord_t>();
}

bool segment_enters_expolygons_interior(
    const Point &a,
    const Point &b,
    const ExPolygons &expolygons)
{
    if (expolygons.empty())
        return false;
    if (point_in_expolygons_interior(expolygons, a) || point_in_expolygons_interior(expolygons, b))
        return true;

    const std::vector<double> params = boundary_params_for_segment(a, b, expolygons);
    for (size_t idx = 1; idx < params.size(); ++idx) {
        const double low = params[idx - 1];
        const double high = params[idx];
        if (high - low < T_EPSILON)
            continue;
        if (point_in_expolygons_interior(expolygons, interpolate(a, b, 0.5 * (low + high))))
            return true;
    }
    return false;
}

std::optional<double> first_interior_param_for_segment(
    const Point &a,
    const Point &b,
    const ExPolygons &expolygons)
{
    if (expolygons.empty())
        return std::nullopt;
    if (point_in_expolygons_interior(expolygons, a))
        return 0.0;

    const std::vector<double> params = boundary_params_for_segment(a, b, expolygons);
    for (size_t idx = 1; idx < params.size(); ++idx) {
        const double low = params[idx - 1];
        const double high = params[idx];
        if (high - low < T_EPSILON)
            continue;
        if (point_in_expolygons_interior(expolygons, interpolate(a, b, 0.5 * (low + high))))
            return low;
    }

    return point_in_expolygons_interior(expolygons, b) ? std::optional<double>(1.0) : std::nullopt;
}

bool segment_inside_bed(const Point &a, const Point &b, const ExPolygons &valid_bed)
{
    if (valid_bed.empty())
        return true;
    if (!point_in_expolygons(valid_bed, a, true) || !point_in_expolygons(valid_bed, b, true))
        return false;

    const std::vector<double> params = boundary_params_for_segment(a, b, valid_bed);
    for (size_t idx = 1; idx < params.size(); ++idx) {
        const double low = params[idx - 1];
        const double high = params[idx];
        if (high - low < T_EPSILON)
            continue;
        if (!point_in_expolygons(valid_bed, interpolate(a, b, 0.5 * (low + high)), false))
            return false;
    }
    return true;
}

double polyline_length(const Polyline &polyline)
{
    double length = 0.0;
    for (size_t idx = 1; idx < polyline.points.size(); ++idx)
        length += (polyline.points[idx] - polyline.points[idx - 1]).cast<double>().norm();
    return length;
}

bool points_near(const Point &lhs, const Point &rhs)
{
    const double epsilon = boundary_epsilon();
    return (lhs - rhs).cast<double>().squaredNorm() <= epsilon * epsilon;
}

bool replacement_matches_original_segment(const Polyline &replacement, const Point &a, const Point &b)
{
    return replacement.points.size() == 2 &&
           points_near(replacement.points.front(), a) && points_near(replacement.points.back(), b);
}

void append_point(Points &points, const Point &point)
{
    if (points.empty() || points.back() != point)
        points.emplace_back(point);
}

Polyline make_forward_detour(
    const Point &a,
    const Point &b,
    const Polygon &contour,
    const SegmentIntersection &entry,
    const SegmentIntersection &exit)
{
    Points points;
    append_point(points, a);
    append_point(points, entry.point);

    const size_t count = contour.points.size();
    for (size_t idx = (entry.edge_idx + 1) % count; idx != (exit.edge_idx + 1) % count; idx = (idx + 1) % count)
        append_point(points, contour.points[idx]);

    append_point(points, exit.point);
    append_point(points, b);
    return Polyline(std::move(points));
}

Polyline make_backward_detour(
    const Point &a,
    const Point &b,
    const Polygon &contour,
    const SegmentIntersection &entry,
    const SegmentIntersection &exit)
{
    Points points;
    append_point(points, a);
    append_point(points, entry.point);

    const size_t count = contour.points.size();
    const size_t last_vertex = (exit.edge_idx + 1) % count;
    for (size_t idx = entry.edge_idx;; idx = idx == 0 ? count - 1 : idx - 1) {
        append_point(points, contour.points[idx]);
        if (idx == last_vertex)
            break;
    }

    append_point(points, exit.point);
    append_point(points, b);
    return Polyline(std::move(points));
}

bool routed_polyline_inside_bed(
    const Polyline &polyline,
    const ExPolygons &valid_bed,
    const bool allow_start_outside,
    const bool allow_end_outside)
{
    if (valid_bed.empty())
        return true;

    const size_t last_idx = polyline.points.size() - 1;
    for (size_t idx = 0; idx < polyline.points.size(); ++idx) {
        if ((idx == 0 && allow_start_outside) || (idx == last_idx && allow_end_outside))
            continue;
        if (!point_in_expolygons(valid_bed, polyline.points[idx], true))
            return false;
    }

    for (size_t idx = 1; idx < polyline.points.size(); ++idx) {
        // A start or end produced by machine G-code may legitimately lie on
        // or beyond the printable boundary. Constrain only the route vertices
        // Orca adds; the firmware-defined transition to the bed remains intact.
        if ((idx == 1 && allow_start_outside) || (idx == last_idx && allow_end_outside))
            continue;
        if (!segment_inside_bed(polyline.points[idx - 1], polyline.points[idx], valid_bed))
            return false;
    }
    return true;
}

bool polyline_intersects_obstacles(const Polyline &polyline, const ExPolygons &obstacles)
{
    for (size_t idx = 1; idx < polyline.points.size(); ++idx)
        if (segment_enters_expolygons_interior(polyline.points[idx - 1], polyline.points[idx], obstacles))
            return true;
    return false;
}

std::optional<size_t> first_intersected_obstacle(
    const Point &a,
    const Point &b,
    const ExPolygons &obstacles)
{
    std::optional<size_t> best_idx;
    double best_t = std::numeric_limits<double>::infinity();
    for (size_t idx = 0; idx < obstacles.size(); ++idx) {
        const ExPolygons single { obstacles[idx] };
        if (const std::optional<double> t = first_interior_param_for_segment(a, b, single); t && *t < best_t) {
            best_t = *t;
            best_idx = idx;
        }
    }
    return best_idx;
}

std::optional<Polyline> detour_around_obstacle(
    const Point &a,
    const Point &b,
    const ExPolygon &obstacle,
    const ExPolygons &obstacles,
    const ExPolygons &valid_bed,
    const bool allow_start_outside,
    const bool allow_end_outside)
{
    const std::vector<SegmentIntersection> intersections = contour_intersections(a, b, obstacle.contour);
    if (intersections.size() < 2)
        return std::nullopt;

    const SegmentIntersection &entry = intersections.front();
    const SegmentIntersection &exit = intersections.back();
    const Polyline forward = make_forward_detour(a, b, obstacle.contour, entry, exit);
    const Polyline backward = make_backward_detour(a, b, obstacle.contour, entry, exit);

    const bool forward_bed_valid = routed_polyline_inside_bed(
        forward, valid_bed, allow_start_outside, allow_end_outside);
    const bool backward_bed_valid = routed_polyline_inside_bed(
        backward, valid_bed, allow_start_outside, allow_end_outside);
    const bool forward_clear = forward_bed_valid && !polyline_intersects_obstacles(forward, obstacles);
    const bool backward_clear = backward_bed_valid && !polyline_intersects_obstacles(backward, obstacles);

    if (forward_clear || backward_clear) {
        if (forward_clear && !backward_clear)
            return forward;
        if (!forward_clear && backward_clear)
            return backward;
        return polyline_length(forward) <= polyline_length(backward) ? forward : backward;
    }

    // If another obstacle blocks both immediate choices, retain the valid local
    // detour and let the outer repair loop address the next crossing.
    if (!forward_bed_valid && !backward_bed_valid)
        return std::nullopt;
    if (forward_bed_valid && !backward_bed_valid)
        return forward;
    if (!forward_bed_valid && backward_bed_valid)
        return backward;
    return polyline_length(forward) <= polyline_length(backward) ? forward : backward;
}

void replace_segment(Polyline &path, size_t segment_idx, const Polyline &replacement)
{
    Points points;
    points.reserve(path.points.size() + replacement.points.size());
    for (size_t idx = 0; idx < segment_idx; ++idx)
        append_point(points, path.points[idx]);
    for (const Point &point : replacement.points)
        append_point(points, point);
    for (size_t idx = segment_idx + 2; idx < path.points.size(); ++idx)
        append_point(points, path.points[idx]);
    path.points = std::move(points);
}

Polyline simplify_path(const Polyline &path, const ExPolygons &obstacles, const ExPolygons &valid_bed)
{
    if (path.points.size() <= 2)
        return path;

    Points simplified;
    simplified.reserve(path.points.size());
    size_t current = 0;
    append_point(simplified, path.points.front());
    while (current + 1 < path.points.size()) {
        size_t next = path.points.size() - 1;
        for (; next > current + 1; --next) {
            if (segment_inside_bed(path.points[current], path.points[next], valid_bed) &&
                !segment_enters_expolygons_interior(path.points[current], path.points[next], obstacles))
                break;
        }
        append_point(simplified, path.points[next]);
        current = next;
    }
    return Polyline(std::move(simplified));
}

} // namespace

void ExclusionVolumeTravelAvoidance::init(const PrintConfig &config, const Vec3d &plate_origin)
{
    clear();

    double max_nozzle_diameter = 0.0;
    for (double diameter : config.nozzle_diameter.values)
        max_nozzle_diameter = std::max(max_nozzle_diameter, diameter);

    // This is a numerical/perimeter-walk clearance, not a hidden toolhead
    // footprint. The exact configured volume remains Stage 2's source of truth.
    const coord_t minimum_clearance = static_cast<coord_t>(SCALED_EPSILON);
    const coord_t configured_clearance =
        static_cast<coord_t>(scale_(std::max(0.05, 0.1 * max_nozzle_diameter)));
    m_clearance = std::max(minimum_clearance, configured_clearance);

    std::vector<std::vector<BedExcludeRegion>> regions_by_extruder =
        get_bed_excluded_regions_by_extruder(config);
    m_spaces.resize(regions_by_extruder.size());

    const Polygon base_bed_shape(get_bed_shape(config));
    const Vec2d plate_xy = plate_origin.head<2>();
    for (size_t extruder_id = 0; extruder_id < regions_by_extruder.size(); ++extruder_id) {
        RoutingSpace &space = m_spaces[extruder_id];
        space.regions = std::move(regions_by_extruder[extruder_id]);
        space.regions.erase(std::remove_if(space.regions.begin(), space.regions.end(),
            [](const BedExcludeRegion &region) { return !region.is_collision_volume(); }), space.regions.end());

        const Vec2d extruder_offset = extruder_id < config.extruder_offset.values.size() ?
            config.extruder_offset.values[extruder_id] : Vec2d::Zero();
        // Resolved regions are in nozzle/model space. The generator routes in
        // emitted G-code space (model - active nozzle offset), before the writer
        // subtracts the plate origin, so transform both obstacles and bed alike.
        const Vec2d translation_mm = plate_xy - extruder_offset;
        const Point translation = scaled<coord_t>(translation_mm);
        for (BedExcludeRegion &region : space.regions) {
            region.polygon.translate(translation);
            BoundingBox bbox(region.polygon.points);
            bbox.offset(m_clearance);
            space.region_bboxes.emplace_back(std::move(bbox));
        }

        Polygon bed_shape = base_bed_shape;
        bed_shape.translate(translation);
        bed_shape.make_counter_clockwise();
        if (bed_shape.points.size() >= 3) {
            space.valid_bed = offset_ex(bed_shape, -float(m_clearance));
            if (space.valid_bed.empty())
                space.valid_bed = union_ex(Polygons { std::move(bed_shape) });
        }
    }
}

void ExclusionVolumeTravelAvoidance::clear()
{
    m_spaces.clear();
    m_clearance = static_cast<coord_t>(SCALED_EPSILON);
}

bool ExclusionVolumeTravelAvoidance::empty() const
{
    return std::all_of(m_spaces.begin(), m_spaces.end(), [](const RoutingSpace &space) {
        return space.regions.empty();
    });
}

const ExPolygons *
ExclusionVolumeTravelAvoidance::active_obstacles(
    RoutingSpace &space,
    double z_min,
    double z_max)
{
    std::vector<size_t> active_region_ids;
    active_region_ids.reserve(space.regions.size());
    for (size_t region_id = 0; region_id < space.regions.size(); ++region_id) {
        const BedExcludeRegion &region = space.regions[region_id];
        if (bed_exclusion_z_ranges_overlap(z_min, z_max, region.z_min, region.z_max))
            active_region_ids.emplace_back(region_id);
    }
    if (active_region_ids.empty())
        return nullptr;

    const auto cached = space.obstacle_cache.find(active_region_ids);
    if (cached != space.obstacle_cache.end())
        return cached->second.empty() ? nullptr : &cached->second;

    std::vector<BedExcludeRegion> active_regions;
    active_regions.reserve(active_region_ids.size());
    for (const size_t region_id : active_region_ids)
        active_regions.emplace_back(space.regions[region_id]);

    ExPolygons obstacles = active_bed_exclusion_footprints(
        active_regions, z_min, z_max, Point(0, 0), m_clearance);
    const auto cache_result = space.obstacle_cache.emplace(
        std::move(active_region_ids), std::move(obstacles));
    return cache_result.first->second.empty() ? nullptr : &cache_result.first->second;
}

ExclusionVolumeTravelAvoidance::Result ExclusionVolumeTravelAvoidance::route(
    const Polyline &travel,
    double start_z,
    double end_z,
    int extruder_id)
{
    Result result;
    result.path = travel;
    if (travel.points.size() < 2)
        return result;
    if (extruder_id < 0 || size_t(extruder_id) >= m_spaces.size()) {
        result.status = Status::Failed;
        result.detail = Detail::UnknownExtruder;
        return result;
    }

    RoutingSpace &space = m_spaces[size_t(extruder_id)];
    if (space.regions.empty()) {
        result.detail = Detail::NoActiveObstacles;
        return result;
    }

    const double z_min = std::min(start_z, end_z);
    const double z_max = std::max(start_z, end_z);

    // Most travels are nowhere near an exclusion. Reject them using the cheap
    // precomputed boxes before unioning and inflating obstacle polygons.
    const BoundingBox travel_bbox(travel.points);
    bool has_active_z = false;
    bool may_intersect = false;
    for (size_t region_id = 0; region_id < space.regions.size(); ++region_id) {
        const BedExcludeRegion &region = space.regions[region_id];
        if (!bed_exclusion_z_ranges_overlap(z_min, z_max, region.z_min, region.z_max))
            continue;
        has_active_z = true;
        if (region_id < space.region_bboxes.size() && space.region_bboxes[region_id].overlap(travel_bbox)) {
            may_intersect = true;
            break;
        }
    }
    if (!has_active_z) {
        result.detail = Detail::NoActiveObstacles;
        return result;
    }
    if (!may_intersect) {
        result.detail = Detail::NoIntersection;
        return result;
    }

    const ExPolygons *active = active_obstacles(space, z_min, z_max);
    if (active == nullptr || active->empty()) {
        result.detail = Detail::NoActiveObstacles;
        return result;
    }
    result.active_obstacles = active->size();

    const Point &start = travel.points.front();
    const Point &end = travel.points.back();
    const bool start_outside_bed = !space.valid_bed.empty() &&
        !point_in_expolygons(space.valid_bed, start, true);
    const bool end_outside_bed = !space.valid_bed.empty() &&
        !point_in_expolygons(space.valid_bed, end, true);
    const ExPolygons *start_obstacles = active_obstacles(space, start_z, start_z);
    const ExPolygons *end_obstacles = active_obstacles(space, end_z, end_z);
    if ((start_obstacles != nullptr && point_in_expolygons_interior(*start_obstacles, start)) ||
        (end_obstacles != nullptr && point_in_expolygons_interior(*end_obstacles, end))) {
        result.status = Status::EndpointInside;
        result.detail = Detail::EndpointInside;
        return result;
    }

    bool needs_reroute = false;
    for (size_t idx = 1; idx < travel.points.size(); ++idx) {
        if (segment_enters_expolygons_interior(travel.points[idx - 1], travel.points[idx], *active)) {
            needs_reroute = true;
            break;
        }
    }
    if (!needs_reroute) {
        result.detail = Detail::NoIntersection;
        return result;
    }

    Polyline path = travel;
    bool rerouted = false;
    for (size_t iteration = 0; iteration < MAX_REROUTE_ITERATIONS; ++iteration) {
        result.iterations = iteration + 1;
        bool changed = false;
        for (size_t segment_idx = 0; segment_idx + 1 < path.points.size(); ++segment_idx) {
            const Point &a = path.points[segment_idx];
            const Point &b = path.points[segment_idx + 1];
            const bool allow_a_outside = start_outside_bed && segment_idx == 0;
            const bool allow_b_outside = end_outside_bed && segment_idx + 2 == path.points.size();
            const Polyline segment({a, b});
            if (!routed_polyline_inside_bed(
                    segment, space.valid_bed, allow_a_outside, allow_b_outside)) {
                result.status = Status::Failed;
                result.detail = Detail::SegmentOutsideBed;
                return result;
            }

            const std::optional<size_t> obstacle_idx = first_intersected_obstacle(a, b, *active);
            if (!obstacle_idx)
                continue;

            const std::optional<Polyline> detour = detour_around_obstacle(
                a, b, (*active)[*obstacle_idx], *active, space.valid_bed,
                allow_a_outside, allow_b_outside);
            if (!detour) {
                result.status = Status::Failed;
                result.detail = Detail::DetourFailed;
                return result;
            }
            if (replacement_matches_original_segment(*detour, a, b))
                continue;

            replace_segment(path, segment_idx, *detour);
            changed = true;
            rerouted = true;
            break;
        }

        if (!changed) {
            path = simplify_path(path, *active, space.valid_bed);
            if (!routed_polyline_inside_bed(
                    path, space.valid_bed, start_outside_bed, end_outside_bed) ||
                polyline_intersects_obstacles(path, *active)) {
                result.status = Status::Failed;
                result.detail = Detail::FinalPathInvalid;
                return result;
            }
            result.path = std::move(path);
            result.status = rerouted ? Status::Rerouted : Status::Unchanged;
            result.detail = rerouted ? Detail::None : Detail::NoIntersection;
            return result;
        }
    }

    result.status = Status::Failed;
    result.detail = Detail::IterationLimit;
    return result;
}

} // namespace Slic3r
