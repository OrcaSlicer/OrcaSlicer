#include "WipePathHelpers.hpp"

#include "../AABBTreeLines.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <tuple>

namespace Slic3r {

// Orca: miter limit ratio. Matches DefaultMiterLimit from ClipperUtils.hpp.
// When the miter join extends more than miter_limit * offset_dist from the
// original vertex, the miter is replaced by a bevel join.
static constexpr double miter_limit = 3.0;

// Orca: threshold for detecting near-reversal (backtracking spike).
// Normalized dot product below this means the segments point in nearly
// opposite directions (angle > ~172°). Offsetting such a path is unsafe.
static constexpr double reversal_dot_threshold = -0.99;

// Orca: candidates pointing more than 60 degrees away from the selected inner
// wall are too tangent to distinguish the material side reliably at a cusp.
static constexpr double min_support_alignment = 0.5;

Point sample_path_at_distance(const ExtrusionPaths &paths, bool forward, double target)
{
    assert(!paths.empty());
    if (paths.empty())
        return Point(0, 0);

    double remaining = target;
    Point result = forward ? paths.front().first_point() : paths.back().last_point();
    for (int pi = forward ? 0 : (int)paths.size() - 1;
         pi >= 0 && pi < (int)paths.size() && remaining > 0.;
         pi += forward ? 1 : -1) {
        const Points3 &pts = paths[pi].polyline.points;
        for (int i = forward ? 0 : (int)pts.size() - 1;
             remaining > 0. && (forward ? i + 1 < (int)pts.size() : i > 0);
             i += forward ? 1 : -1) {
            const int j = forward ? i + 1 : i - 1;
            const Point cur(pts[i].x(), pts[i].y());
            const Point next(pts[j].x(), pts[j].y());
            const double segment_length = (next - cur).cast<double>().norm();
            if (segment_length < SCALED_EPSILON)
                continue;
            if (remaining <= segment_length) {
                const double ratio = remaining / segment_length;
                return Point(coord_t(cur.x() + ratio * (next.x() - cur.x())),
                             coord_t(cur.y() + ratio * (next.y() - cur.y())));
            }
            remaining -= segment_length;
            result = next;
        }
    }
    return result;
}

// Orca: consecutive duplicates carry no path length and can be removed safely.
// A reversal, however, is real travelled distance: removing its vertex would
// replace a long backtracking wipe with a short, unrelated shortcut.
static bool prepare_source(Points &pts)
{
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());

    if (pts.size() < 2)
        return false;

    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const Vec2d v_prev = (pts[i] - pts[i - 1]).cast<double>();
        const Vec2d v_next = (pts[i + 1] - pts[i]).cast<double>();
        const double dot = v_prev.dot(v_next) / (v_prev.norm() * v_next.norm());
        if (dot < reversal_dot_threshold)
            return false;
    }
    return true;
}

static bool build_offset_polyline(const Points &original, int dir, double offset_dist,
                                  Points &result, size_t &first_join_index)
{
    if (original.size() < 2)
        return false;

    // Orca: collapse all consecutive duplicates first, then reject any
    // backtracking in the cleaned path instead of replacing travelled distance
    // with a shortcut.
    Points source = original;
    if (! prepare_source(source))
        return false;

    const size_t n = source.size();

    // Orca: compute the perpendicular offset for segment i->i+1 as an infinite Line.
    auto offset_segment = [dir, offset_dist](const Point &a, const Point &b) -> Line {
        Vec2d  v = (b - a).cast<double>();
        double len = v.norm();
        Vec2d  perp(0, 0);
        if (len > SCALED_EPSILON)
            perp = Vec2d(-v.y(), v.x()) * (dir * offset_dist / len);
        return Line(Point(coord_t(a.x() + perp.x()), coord_t(a.y() + perp.y())),
                    Point(coord_t(b.x() + perp.x()), coord_t(b.y() + perp.y())));
    };

    result.clear();
    result.reserve(n);
    first_join_index = 0;

    // Orca: the first point is perpendicular to the first segment.
    Line l_prev = offset_segment(source[0], source[1]);
    result.push_back(l_prev.a);

    // Orca: use the analytic intersection of adjacent offset segments for a
    // miter join. Intersecting the already rounded Line endpoints amplifies
    // coordinate quantization when the source segments are nearly parallel.
    for (size_t i = 1; i + 1 < n; ++i) {
        Line l_next = offset_segment(source[i], source[i + 1]);
        const Vec2d previous = (source[i] - source[i - 1]).cast<double>().normalized();
        const Vec2d next = (source[i + 1] - source[i]).cast<double>().normalized();
        const double denominator = 1. + previous.dot(next);

        bool need_bevel = denominator <= EPSILON;
        Point pt;
        if (! need_bevel) {
            const Vec2d previous_normal(-previous.y(), previous.x());
            const Vec2d next_normal(-next.y(), next.x());
            const Vec2d miter = (previous_normal + next_normal) * (dir * offset_dist / denominator);
            if (miter.norm() > miter_limit * offset_dist) {
                need_bevel = true;
            } else {
                pt = Point(coord_t(source[i].x() + miter.x()),
                           coord_t(source[i].y() + miter.y()));
            }
        }

        if (need_bevel) {
            result.push_back(l_prev.b);
            if (l_next.a != result.back())
                result.push_back(l_next.a);
        } else {
            result.push_back(pt);
        }
        if (i == 1)
            first_join_index = result.size() - 1;
        l_prev = l_next;
    }

    // Orca: the last point is perpendicular to the last segment.
    result.push_back(l_prev.b);

    return true;
}

int wipe_offset_direction(bool is_ccw, bool is_hole)
{
    const int loop_inside = is_ccw ? +1 : -1;
    return is_hole ? -loop_inside : loop_inside;
}

static bool starts_by_backtracking(const Polyline &path, Point actual_start)
{
    if (path.points.size() < 3)
        return false;
    // Orca: points[0] is only a storage sentinel; use the nozzle position for
    // the executable connector, particularly after a wipe_on_loops pre-move.
    const Vec2d connector = (path.points[1] - actual_start).cast<double>();
    const Vec2d outgoing = (path.points[2] - path.points[1]).cast<double>();
    return connector.dot(outgoing) < 0.;
}

// Orca: sample the outgoing perimeter without copying or clipping its full loop.
static Point sample_polyline_at_distance(const Polyline &polyline, double target)
{
    assert(! polyline.points.empty());
    Point result = polyline.first_point();
    for (size_t i = 1; i < polyline.points.size() && target > 0.; ++i) {
        const Vec2d segment = (polyline.points[i] - result).cast<double>();
        const double length = segment.norm();
        if (length <= SCALED_EPSILON)
            continue;
        if (target <= length)
            return (result.cast<double>() + segment * (target / length)).cast<coord_t>();
        target -= length;
        result = polyline.points[i];
    }
    return result;
}

// Orca: convert an executable path into Wipe::wipe()'s stored representation.
// The first point is a dummy replaced by the actual nozzle position, while the
// remaining points are clipped to the configured wipe distance.
static bool store_wipe_path(Polyline &destination, Point seam_start,
                            Polyline actual_path, double max_wipe_length)
{
    if (actual_path.points.size() < 2 || max_wipe_length <= SCALED_EPSILON)
        return false;

    const double actual_length = actual_path.length();
    if (actual_length <= SCALED_EPSILON)
        return false;
    if (actual_length - max_wipe_length > SCALED_EPSILON)
        actual_path.clip_end(actual_length - max_wipe_length);
    if (actual_path.points.size() < 2)
        return false;
    for (size_t i = 1; i < actual_path.points.size(); ++i)
        if (actual_path.points[i - 1] == actual_path.points[i])
            return false;

    Polyline stored_path;
    stored_path.points.reserve(actual_path.points.size());
    stored_path.points.push_back(seam_start);
    stored_path.points.insert(stored_path.points.end(), actual_path.points.begin() + 1, actual_path.points.end());
    stored_path.reset_to_linear_move();
    destination = std::move(stored_path);
    return true;
}

bool offset_wipe_path(Polyline &polyline, Point seam_start, Point seam_end, Point wipe_start,
                      int dir, double offset_dist, double max_wipe_length)
{
    assert(dir == +1 || dir == -1);
    assert(offset_dist > 0);
    if (polyline.points.empty() || polyline.first_point() != seam_start ||
        max_wipe_length <= SCALED_EPSILON)
        return false;

    const Polyline original = polyline;
    const double original_length = original.length();
    if (original_length <= SCALED_EPSILON)
        return false;

    double source_length = std::min(original_length, max_wipe_length);
    for (;;) {
        Polyline source = original;
        const double clip_distance = original_length - source_length;
        if (clip_distance > SCALED_EPSILON)
            source.clip_end(clip_distance);

        Points wrapped_source;
        wrapped_source.reserve(source.points.size() + 1);
        if (seam_start == seam_end) {
            // Orca: the stored loop is open at seam_start even when the seam gap is
            // zero. Prepend the closing edge so build_offset_polyline() creates
            // the proper join between that edge and the first outgoing edge,
            // instead of leaving the first offset point on the closing wall.
            size_t closing_index = original.points.size();
            while (closing_index > 0 && original.points[closing_index - 1] == seam_start)
                --closing_index;
            if (closing_index == 0)
                return false; // Orca: the entire path is a single point.
            wrapped_source.push_back(original.points[closing_index - 1]);
        } else {
            // Orca: use the unextruded seam-gap edge to determine the incoming
            // direction at the seam. Its offset is construction geometry only;
            // wiping along it would create a Z-shaped detour before the outgoing
            // perimeter offset.
            wrapped_source.push_back(seam_end);
        }
        wrapped_source.insert(wrapped_source.end(), source.points.begin(), source.points.end());

        Points offset_points;
        size_t first_join_index = 0;
        if (! build_offset_polyline(wrapped_source, dir, offset_dist, offset_points, first_join_index) ||
            first_join_index == 0 || first_join_index >= offset_points.size())
            return false;
        // Orca: discard the offset of the prepended edge and, for a bevel, its
        // incoming endpoint. The executable wipe starts at the seam join and
        // then follows only the already printed outgoing perimeter.
        offset_points.erase(offset_points.begin(), offset_points.begin() + first_join_index);

        Polyline actual_path;
        actual_path.points.reserve(offset_points.size() + 1);
        actual_path.points.push_back(wipe_start);
        actual_path.points.insert(actual_path.points.end(), offset_points.begin(), offset_points.end());

        if (seam_start != seam_end && wipe_start == seam_end &&
            starts_by_backtracking(actual_path, wipe_start)) {
            // Orca: a wide seam gap or a sharp cusp may put the first miter
            // behind its outgoing edge. Reject this offset candidate so the
            // caller can try the opposite side or the translated fallback.
            return false;
        }

        const double actual_length = actual_path.length();
        const bool source_exhausted = original_length - source_length <= SCALED_EPSILON;
        if (actual_length + SCALED_EPSILON < max_wipe_length && ! source_exhausted) {
            // Orca: offset joins may shorten the path at every corner. Grow the
            // source until the executable offset path, not a heuristic source
            // margin, reaches the configured wipe distance.
            const double deficit = max_wipe_length - actual_length;
            const double next_length = std::min(original_length,
                source_length + std::max(deficit, 2. * SCALED_EPSILON));
            if (next_length - source_length <= SCALED_EPSILON)
                return false;
            source_length = next_length;
            continue;
        }

        // Orca: unlike an extruded offset, a wipe may safely cross or retrace the
        // just-printed perimeter. The caller validates the complete executable
        // path against current and earlier printed perimeter geometry.
        return store_wipe_path(polyline, seam_start, std::move(actual_path), max_wipe_length);
    }
}

static bool translated_wipe_path(Polyline &polyline, Point seam_start, Point seam_end, Point wipe_start,
                                 const Vec2d &translation, double max_wipe_length)
{
    if (translation.norm() <= SCALED_EPSILON || max_wipe_length <= SCALED_EPSILON)
        return false;

    const Polyline original = polyline;
    Polyline actual_path;
    actual_path.points.reserve(original.points.size() + 2);
    actual_path.points.push_back(wipe_start);

    const auto append_translated = [&actual_path, &translation](const Point &point) {
        const Point translated = (point.cast<double>() + translation).cast<coord_t>();
        if (translated != actual_path.points.back())
            actual_path.points.push_back(translated);
    };

    // Orca: translate the seam join directly. Translating seam_end and then
    // following the unextruded gap back to seam_start makes the wipe double
    // back whenever a gap ends near a sharp corner.
    append_translated(seam_start);
    for (const Point &point : original.points)
        append_translated(point);

    if (seam_start != seam_end && wipe_start == seam_end &&
        starts_by_backtracking(actual_path, wipe_start)) {
        // Orca: at a wide gap next to a cusp, the translated seam join may
        // lie behind the outgoing edge. Prefer a shorter local inward move
        // at the actual extrusion end over a longer lightning-shaped wipe.
        actual_path.points.resize(1);
        append_translated(seam_end);
    }

    return store_wipe_path(polyline, seam_start, std::move(actual_path), max_wipe_length);
}

static std::optional<double> wipe_path_support_score(
    const Polyline &polyline, Point wipe_start,
    const AABBTreeLines::LinesDistancer<Line> &target_distancer,
    const AABBTreeLines::LinesDistancer<Line> &all_support_distancer,
    double max_distance)
{
    if (polyline.points.size() < 2 || target_distancer.get_lines().empty() || max_distance <= 0)
        return std::nullopt;

    // Orca: require a local neighbour, not merely an earlier perimeter elsewhere in
    // the region. At a convex corner, an inner wall's miter is farther from the
    // external seam than its normal wall spacing, so allow the same bounded miter
    // reach as the offset construction without accepting a remote island.
    if (target_distancer.distance_from_lines<false>(wipe_start) >
        miter_limit * max_distance + 4. * SCALED_EPSILON)
        return std::nullopt;

    const auto is_supported = [max_distance](const Point &point, const AABBTreeLines::LinesDistancer<Line> &distancer) {
        // Orca: offset joins and closest-point projection involve several
        // rounded scaled coordinates. Keep the tolerance below G-code XY
        // resolution while absorbing their compounded quantization error.
        return distancer.distance_from_lines<false>(point) <= max_distance + 4. * SCALED_EPSILON;
    };

    Point previous = wipe_start;
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        // Orca: a tightly curved inward path may cross back over the current wall.
        // This is safe for a non-extruding wipe as long as the complete path
        // remains over current or earlier printed perimeter geometry.
        if (! is_supported(polyline.points[i], all_support_distancer))
            return std::nullopt;

        const Vec2d segment = (polyline.points[i] - previous).cast<double>();
        const size_t samples = size_t(std::ceil(segment.norm() / max_distance));
        for (size_t sample = 1; sample < samples; ++sample) {
            const Point point = (previous.cast<double>() +
                segment * (double(sample) / double(samples))).cast<coord_t>();
            if (! is_supported(point, all_support_distancer))
                return std::nullopt;
        }
        previous = polyline.points[i];
    }

    // Orca: decide direction at the seam. Scoring the complete path may select
    // the wrong initial side when two contours converge and the later prefix
    // happens to run closer to unrelated support.
    return target_distancer.distance_from_lines<false>(polyline.points[1]);
}

static bool initial_connector_is_clear(
    const Polyline &polyline, Point wipe_start, Point seam_start,
    AABBTreeLines::LinesDistancer<Line> &current_perimeter_distancer,
    double contact_tolerance)
{
    if (polyline.points.size() < 2 || polyline.points[1] == wipe_start)
        return false;

    // Orca: without a seam gap, the connector necessarily starts at the wall
    // and a self-touching cusp may share that same endpoint on several edges.
    if (seam_start == wipe_start)
        return true;

    const Line connector(wipe_start, polyline.points[1]);
    const auto intersections = current_perimeter_distancer.intersections_with_line<false>(connector);
    for (const auto &intersection : intersections) {
        if ((intersection.first - wipe_start).cast<double>().norm() > contact_tolerance)
            return false;
    }

    Point closest;
    // Orca: integer offset joins may miss the exact seam-start coordinate by
    // a few microns. Treat a close pass through that point as retracing the
    // external wall, but keep the unavoidable contact at the actual start.
    if (connector.distance_to_squared(seam_start, &closest) <= contact_tolerance * contact_tolerance &&
        (closest - wipe_start).cast<double>().norm() > contact_tolerance)
        return false;

    return true;
}

static std::optional<Vec2d> support_offset_at_start(
    const Polyline &source, Point local_origin, bool disambiguate_branch,
    AABBTreeLines::LinesDistancer<Line> &support_distancer,
    double max_support_distance)
{
    if (source.points.size() < 2)
        return std::nullopt;

    // Orca: a nonzero gap may put the seam beside the wrong branch of a cusp.
    // Sample farther along the path to identify its actual neighbouring wall.
    const Point support_query = disambiguate_branch ?
        sample_polyline_at_distance(source, 2. * max_support_distance) : source.first_point();
    const auto nearest_result = support_distancer.distance_from_lines_extra<false>(support_query);
    const Line &nearest_line = support_distancer.get_line(std::get<1>(nearest_result));
    Vec2d sampled_offset = std::get<2>(nearest_result) - support_query.cast<double>();

    if (disambiguate_branch) {
        // Orca: an endpoint projection also contains distance along the support
        // segment. Remove that tangent component before comparing wall sides.
        const Vec2d support_edge = (nearest_line.b - nearest_line.a).cast<double>();
        if (support_edge.norm() > SCALED_EPSILON) {
            const Vec2d support_tangent = support_edge.normalized();
            sampled_offset -= support_tangent * sampled_offset.dot(support_tangent);
        }
    }
    if (sampled_offset.norm() <= SCALED_EPSILON)
        return std::nullopt;

    if (! disambiguate_branch)
        return sampled_offset;

    // Orca: find the local point on the same material-side branch. Using the
    // sampled point itself would add the distance already travelled along the
    // perimeter and turn a normal transition into a long diagonal move.
    const Vec2d sampled_direction = sampled_offset.normalized();
    Vec2d local_offset = sampled_offset;
    double best_local_score = std::numeric_limits<double>::infinity();
    for (size_t line_index : support_distancer.all_lines_in_radius(
             local_origin, 2. * max_support_distance + 4. * SCALED_EPSILON)) {
        Point local_support;
        const Line &line = support_distancer.get_line(line_index);
        const double distance_squared = line.distance_to_squared(local_origin, &local_support);
        const Vec2d candidate_offset = local_support.cast<double>() - local_origin.cast<double>();
        const double candidate_distance = std::sqrt(distance_squared);
        if (candidate_distance <= SCALED_EPSILON)
            continue;
        const double alignment = candidate_offset.normalized().dot(sampled_direction);
        if (alignment < min_support_alignment)
            continue;
        const double score = candidate_distance / alignment;
        if (score < best_local_score) {
            best_local_score = score;
            local_offset = candidate_offset;
        }
    }
    return local_offset;
}

static double executable_path_length(const Polyline &stored_path, Point wipe_start)
{
    if (stored_path.points.size() < 2)
        return 0.;

    // Orca: points[0] is the storage sentinel, so measure the first segment
    // from the actual nozzle position and the remaining stored segments normally.
    double length = (stored_path.points[1] - wipe_start).cast<double>().norm();
    for (size_t index = 2; index < stored_path.points.size(); ++index)
        length += (stored_path.points[index] - stored_path.points[index - 1]).cast<double>().norm();
    return length;
}

static Lines material_side_support_lines(const Polyline &path, Point seam, int preferred_dir,
                                         const Lines &support_lines)
{
    if (path.points.size() < 4 || path.first_point() != path.last_point())
        return {};

    // Orca: the bisector of the incoming and outgoing material-side normals is
    // a local side test that remains valid for globally self-touching Arachne
    // contours. Ignore repeated seam points when obtaining both tangents.
    const auto outgoing_it = std::find_if(
        path.points.begin() + 1, path.points.end(), [seam](const Point &point) { return point != seam; });
    const auto incoming_it = std::find_if(
        path.points.rbegin() + 1, path.points.rend(), [seam](const Point &point) { return point != seam; });
    if (outgoing_it == path.points.end() || incoming_it == path.points.rend())
        return {};

    const Vec2d outgoing = (*outgoing_it - seam).cast<double>().normalized();
    const Vec2d incoming = (seam - *incoming_it).cast<double>().normalized();
    const Vec2d material_direction =
        (Vec2d(-outgoing.y(), outgoing.x()) + Vec2d(-incoming.y(), incoming.x())) * preferred_dir;
    if (material_direction.norm() <= EPSILON)
        return {};

    Lines result;
    result.reserve(support_lines.size());
    for (const Line &line : support_lines) {
        Point closest;
        line.distance_to_squared(seam, &closest);
        if ((closest - seam).cast<double>().dot(material_direction) > SCALED_EPSILON)
            result.push_back(line);
    }
    return result;
}

static bool stays_on_material_side(
    const Polyline &path, Point path_start, const Vec2d &support_direction,
    AABBTreeLines::LinesDistancer<Line> &current_perimeter_distancer,
    double effective_offset, bool require_clearance)
{
    if (path.points.size() < 2 || support_direction.norm() <= EPSILON)
        return false;

    const Vec2d initial_offset = (path.points[1] - path_start).cast<double>();
    if (initial_offset.norm() <= SCALED_EPSILON ||
        initial_offset.normalized().dot(support_direction.normalized()) < min_support_alignment)
        return false;
    if (! require_clearance)
        return true;

    // Orca: after the connector has left the extrusion endpoint, an inward
    // offset must retain most of its requested clearance from the current
    // external wall. Otherwise a tight turn may send an initially correct path
    // back onto that wall, or make the opposite-side candidate look supported.
    const double clearance_tolerance = std::max(4. * SCALED_EPSILON, 0.25 * effective_offset);
    const double minimum_clearance = effective_offset - clearance_tolerance;
    const auto has_clearance = [&](const Point &point) {
        return current_perimeter_distancer.distance_from_lines<false>(point) +
            4. * SCALED_EPSILON >= minimum_clearance;
    };

    Point previous = path.points[1];
    if (! has_clearance(previous))
        return false;
    for (size_t index = 2; index < path.points.size(); ++index) {
        const Vec2d segment = (path.points[index] - previous).cast<double>();
        const size_t samples = std::max<size_t>(1, size_t(std::ceil(segment.norm() / effective_offset)));
        for (size_t sample = 1; sample <= samples; ++sample) {
            const Point point = (previous.cast<double>() +
                segment * (double(sample) / double(samples))).cast<coord_t>();
            if (! has_clearance(point))
                return false;
        }
        previous = path.points[index];
    }
    return true;
}

bool wipe_path_is_supported(const Polyline &polyline, Point wipe_start,
                            const Lines &other_perimeter_lines, const Lines &current_perimeter_lines,
                            double max_distance)
{
    if (other_perimeter_lines.empty())
        return false;

    AABBTreeLines::LinesDistancer<Line> target_distancer(other_perimeter_lines);
    Lines all_support_lines = other_perimeter_lines;
    all_support_lines.insert(all_support_lines.end(), current_perimeter_lines.begin(), current_perimeter_lines.end());
    AABBTreeLines::LinesDistancer<Line> all_support_distancer(std::move(all_support_lines));
    return wipe_path_support_score(
        polyline, wipe_start, target_distancer, all_support_distancer, max_distance).has_value();
}

bool offset_wipe_path_toward_support(Polyline &polyline, Point seam_start, Point seam_end, Point wipe_start,
                                     int preferred_dir, double offset_dist, double max_wipe_length,
                                     const Lines &target_perimeter_lines, const Lines &printed_perimeter_lines,
                                     const Lines &current_perimeter_lines,
                                     double max_support_distance)
{
    assert(preferred_dir == +1 || preferred_dir == -1);
    if (polyline.points.size() < 2 || target_perimeter_lines.empty() || current_perimeter_lines.empty() ||
        offset_dist <= SCALED_EPSILON ||
        max_wipe_length <= SCALED_EPSILON || max_support_distance <= SCALED_EPSILON)
        return false;

    Lines material_support_lines;
    const Lines *candidate_support_lines = &target_perimeter_lines;
    if (seam_start == seam_end) {
        // Orca: another contour may have a geometrically closer inner wall on
        // this loop's air side. Restrict zero-gap support using the local seam
        // normals before choosing the nearest wall.
        material_support_lines = material_side_support_lines(
            polyline, seam_start, preferred_dir, target_perimeter_lines);
        if (material_support_lines.empty())
            return false;
        candidate_support_lines = &material_support_lines;
    }

    AABBTreeLines::LinesDistancer<Line> support_distancer(*candidate_support_lines);
    const std::optional<Vec2d> support_offset = support_offset_at_start(
        polyline, seam_end, seam_start != seam_end,
        support_distancer, max_support_distance);
    if (! support_offset)
        return false;
    const Vec2d toward_support = *support_offset;
    const double local_support_distance = toward_support.norm();
    const double effective_offset = std::min(offset_dist, local_support_distance);
    if (effective_offset <= SCALED_EPSILON)
        return false;
    const Vec2d support_direction = toward_support / local_support_distance;

    // Orca: every candidate is validated against the same generated geometry.
    // Build these AABB trees once per loop instead of rebuilding them for each
    // preferred, alternate, translated, direct, or reversed candidate.
    Lines all_support_lines = printed_perimeter_lines;
    all_support_lines.insert(all_support_lines.end(), current_perimeter_lines.begin(), current_perimeter_lines.end());
    AABBTreeLines::LinesDistancer<Line> all_support_distancer(std::move(all_support_lines));
    AABBTreeLines::LinesDistancer<Line> current_perimeter_distancer(current_perimeter_lines);

    // Orca: allow only the contact needed to leave the extrusion endpoint. A
    // connector that meets the current wall again is a seam-gap retrace, even
    // if the rest of the non-extruding wipe remains over printed material.
    const double contact_tolerance = std::max(4. * SCALED_EPSILON, 0.1 * effective_offset);

    struct Candidate {
        Polyline path;
        // Orca: support score chooses the material-side path; length is used
        // only to replace a corner-truncated path with the reverse fallback.
        double support_score;
        double path_length;
    };

    const auto validate_candidate = [&](Polyline path, Point path_start,
                                        double path_contact_tolerance,
                                        const Vec2d &candidate_support_direction,
                                        bool require_clearance = true) -> std::optional<Candidate> {
        // Orca: backtracking indicates a wrong join only across a nonzero gap.
        // A closed zero-gap offset may initially turn back at its miter while
        // still remaining on the supported material side of the perimeter.
        const bool backtracks_across_gap = seam_start != seam_end && starts_by_backtracking(path, wipe_start);
        const bool material_side = seam_start != seam_end ||
            stays_on_material_side(path, path_start, candidate_support_direction,
                                   current_perimeter_distancer, effective_offset, require_clearance);
        const bool connector_clear = initial_connector_is_clear(
            path, wipe_start, path_start, current_perimeter_distancer, path_contact_tolerance);
        if (backtracks_across_gap || ! material_side || ! connector_clear)
            return std::nullopt;
        const std::optional<double> score = wipe_path_support_score(
            path, wipe_start, support_distancer, all_support_distancer, max_support_distance);
        if (! score)
            return std::nullopt;
        const double path_length = executable_path_length(path, wipe_start);
        return Candidate{std::move(path), *score, path_length};
    };

    const auto offset_candidate = [&](int dir) -> std::optional<Candidate> {
        Polyline path = polyline;
        if (! offset_wipe_path(path, seam_start, seam_end, wipe_start, dir,
                               effective_offset, max_wipe_length))
            return std::nullopt;
        return validate_candidate(std::move(path), seam_start, contact_tolerance, support_direction);
    };

    std::optional<Candidate> preferred = offset_candidate(preferred_dir);
    std::optional<Candidate> alternate = offset_candidate(-preferred_dir);

    // Orca: forward and reverse fallbacks share the same clamping, translation,
    // connector tolerance, and complete-path validation.
    const auto translated_candidate = [&](Polyline source, Point source_start, Point source_end,
                                          const Vec2d &candidate_support_offset) -> std::optional<Candidate> {
        const double support_distance = candidate_support_offset.norm();
        const double candidate_offset = std::min(offset_dist, support_distance);
        if (candidate_offset <= SCALED_EPSILON)
            return std::nullopt;

        const Vec2d candidate_translation = candidate_support_offset * (candidate_offset / support_distance);
        if (! translated_wipe_path(source, source_start, source_end, wipe_start,
                                   candidate_translation, max_wipe_length))
            return std::nullopt;
        const double candidate_tolerance = std::max(4. * SCALED_EPSILON, 0.1 * candidate_offset);
        return validate_candidate(std::move(source), source_start, candidate_tolerance,
                                  candidate_support_offset / support_distance);
    };

    std::optional<Candidate> translated = translated_candidate(polyline, seam_start, seam_end, toward_support);

    // Orca: if every full-length construction folds back onto the external
    // wall, retain a short direct inward move instead of accepting an outward
    // candidate or falling back to the standard wipe along the outer wall.
    const auto direct_candidate = [&](Point origin, const Vec2d &candidate_support_offset) -> std::optional<Candidate> {
        const double support_distance = candidate_support_offset.norm();
        const double candidate_offset = std::min(offset_dist, support_distance);
        if (candidate_offset <= SCALED_EPSILON)
            return std::nullopt;
        const Vec2d direction = candidate_support_offset / support_distance;
        const Point destination = (origin.cast<double>() + direction * candidate_offset).cast<coord_t>();
        if (destination == wipe_start)
            return std::nullopt;

        Polyline path{seam_start, destination};
        path.reset_to_linear_move();
        const double candidate_tolerance = std::max(4. * SCALED_EPSILON, 0.1 * candidate_offset);
        return validate_candidate(std::move(path), origin, candidate_tolerance, direction, false);
    };
    std::optional<Candidate> direct = direct_candidate(seam_end, toward_support);

    std::optional<Candidate> reversed;
    if (seam_start != seam_end && polyline.last_point() == seam_end) {
        // Orca: when a large gap straddles a sharp corner, connecting the
        // extrusion end to the forward offset may either reverse or leave only
        // a short local move. The already printed incoming wall is equally safe:
        // follow it backwards and determine its own material-side support.
        Polyline reversed_source = polyline;
        reversed_source.reverse();
        const std::optional<Vec2d> reversed_support_offset = support_offset_at_start(
            reversed_source, seam_end, true, support_distancer, max_support_distance);
        if (reversed_support_offset)
            reversed = translated_candidate(std::move(reversed_source), seam_end, seam_end,
                                             *reversed_support_offset);
    }

    // Orca: conventional offsets at a narrow cusp may form a bevel across the
    // cusp. Candidates pointing away from the actual inner wall are rejected
    // during validation; among the remaining paths, prefer the one whose first
    // point is materially closer to that wall.
    const double direction_change_margin = std::max(4. * SCALED_EPSILON, 0.1 * effective_offset);
    std::optional<Candidate> selected = std::move(preferred);
    if (alternate) {
        if (! selected || alternate->support_score + direction_change_margin < selected->support_score)
            selected = std::move(alternate);
    }
    if (translated) {
        if (! selected || translated->support_score + direction_change_margin < selected->support_score)
            selected = std::move(translated);
    }
    if (! selected)
        selected = std::move(direct);

    // Orca: prefer a complete reverse wipe over a forward fallback that had to
    // stop at the corner. Equal-length paths keep the normal forward behavior.
    const double length_margin = std::max(4. * SCALED_EPSILON, 0.1 * max_wipe_length);
    if (reversed && (! selected || reversed->path_length > selected->path_length + length_margin))
        selected = std::move(reversed);
    if (! selected)
        return false;

    polyline = std::move(selected->path);
    return true;
}

std::optional<Point> wipe_on_loops_destination(const ExtrusionPaths &paths, double nozzle_diam_scaled,
                                                 bool is_ccw, bool is_hole)
{
    assert(!paths.empty());
    assert(nozzle_diam_scaled > 0);
    if (paths.empty() || nozzle_diam_scaled <= 0)
        return std::nullopt;

    // Orca: clamp sample distance to L/4 so forward/backward samples cannot meet.
    double total_length = 0.;
    for (const ExtrusionPath &path : paths)
        total_length += path.length();
    const double sample_distance = std::min(nozzle_diam_scaled, total_length * 0.25);

    Point a = sample_path_at_distance(paths, true,  sample_distance);
    Point b = sample_path_at_distance(paths, false, sample_distance);

    const Point seam_start = paths.front().first_point();

    // Orca: skip the inward move for degenerate geometry.
    if (a == b || a == seam_start || b == seam_start)
        return std::nullopt;

    const bool reverse_turn = is_hole == is_ccw;
    if (reverse_turn)
        std::swap(a, b);

    double angle = seam_start.ccw_angle(a, b) / 3;

    // Orca: reject degenerate angles near 0 or 2π.
    static constexpr double angle_epsilon = 0.01;
    if (angle < angle_epsilon || angle > 2 * PI / 3 - angle_epsilon)
        return std::nullopt;

    if (reverse_turn)
        angle *= -1;

    Point pt = sample_path_at_distance(paths, true, std::min(0.2 * nozzle_diam_scaled, sample_distance));
    pt.rotate(angle, seam_start);
    return pt;
}

} // namespace Slic3r
