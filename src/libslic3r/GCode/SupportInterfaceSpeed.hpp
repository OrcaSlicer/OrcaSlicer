#pragma once

#include "../AABBTreeLines.hpp"
#include "../Circle.hpp"
#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../ExPolygon.hpp"
#include "../Layer.hpp"
#include "../Line.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../Print.hpp"
#include "../libslic3r.h"
#include "ExtrusionProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

// Project exposed upper interfaces onto the first model surface above them.
// Search only the Z interval which can contain a contact, including independent
// support-layer rounding. The cache below calculates max_support_height once.
inline ExPolygons support_interface_regions_below(const Layer &layer, double max_support_height)
{
    const PrintObject &object = *layer.object();
    const auto &supports = object.support_layers();
    const auto &model_layers = object.layers();
    const double bottom_z = layer.bottom_z();
    const double oldest_z = bottom_z - object.config().support_top_z_distance.value -
                            std::max(layer.height, max_support_height) - EPSILON;
    const auto first = std::lower_bound(supports.begin(), supports.end(), oldest_z,
        [](const SupportLayer *support, double z) { return support->print_z < z; });
    ExPolygons regions;
    for (auto contact = first; contact != supports.end() && (*contact)->print_z <= bottom_z + EPSILON; ++contact) {
        const SupportLayer *support = *contact;
        const double max_gap = object.config().support_top_z_distance.value + std::max(layer.height, support->height);
        if (bottom_z - support->print_z > max_gap + EPSILON || support->top_interface_polygons.empty())
            continue;
        ExPolygons visible = union_ex(support->top_interface_polygons);
        auto below = std::lower_bound(model_layers.begin(), model_layers.end(), support->print_z - EPSILON,
            [](const Layer *candidate, double z) { return candidate->print_z < z; });
        for (; below != model_layers.end() && (*below)->print_z <= bottom_z + EPSILON && !visible.empty(); ++below)
            visible = diff_ex(visible, (*below)->lslices);
        for (auto above = std::next(contact); above != supports.end() && (*above)->print_z <= bottom_z + EPSILON && !visible.empty(); ++above) {
            if ((*above)->print_z <= support->print_z + EPSILON)
                continue;
            visible = diff_ex(visible, (*above)->support_islands);
            visible = diff_ex(visible, (*above)->lslices);
            visible = diff_ex(visible, (*above)->base_areas);
            visible = diff_ex(visible, (*above)->top_interface_polygons);
        }
        append(regions, std::move(visible));
    }
    return union_ex(regions);
}

inline ExPolygons support_interface_regions_below(const Layer &layer)
{
    double max_height = 0.;
    for (const SupportLayer *support : layer.object()->support_layers())
        max_height = std::max(max_height, support->height);
    return support_interface_regions_below(layer, max_height);
}

class SupportInterfaceRegionCache
{
public:
    const ExPolygons &regions(const Layer &layer) {
        auto cached = m_regions.find(&layer);
        if (cached != m_regions.end())
            return cached->second;
        const PrintObject *object = layer.object();
        auto height = m_max_support_height.find(object);
        if (height == m_max_support_height.end()) {
            double max_height = 0.;
            for (const SupportLayer *support : object->support_layers())
                max_height = std::max(max_height, support->height);
            height = m_max_support_height.emplace(object, max_height).first;
        }
        return m_regions.emplace(&layer, support_interface_regions_below(layer, height->second)).first->second;
    }
    void clear() { m_regions.clear(); m_max_support_height.clear(); }
private:
    std::unordered_map<const Layer *, ExPolygons> m_regions;
    std::unordered_map<const PrintObject *, double> m_max_support_height;
};

struct SupportInterfaceLayerRegion
{
    ExPolygons regions;
    size_t     layer_offset { 0 };
};

inline std::vector<SupportInterfaceLayerRegion> support_interface_regions_for_slow_layers(
    const Layer &layer, size_t slow_layers, SupportInterfaceRegionCache &cache)
{
    std::vector<SupportInterfaceLayerRegion> result;
    const auto &layers = layer.object()->layers();
    const auto current = std::lower_bound(layers.begin(), layers.end(), layer.print_z,
        [](const Layer *candidate, double z) { return candidate->print_z < z; });
    if (current == layers.end() || *current != &layer)
        return result;
    const size_t current_idx = size_t(std::distance(layers.begin(), current));
    for (size_t offset = 0; offset < slow_layers && offset <= current_idx; ++offset) {
        const ExPolygons &regions = cache.regions(*layers[current_idx - offset]);
        if (!regions.empty())
            result.push_back({regions, offset});
    }
    return result;
}

// Clip all source segments in one Clipper operation, rather than rebuilding the
// polygon clipper for every move. An AABB index maps fragments back to source
// segments, including retraced segments, without changing travel order.
inline std::vector<std::vector<std::pair<double, double>>> support_interface_intervals(
    const Points3 &points, const ExPolygons &regions)
{
    std::vector<std::vector<std::pair<double, double>>> intervals(points.size() > 1 ? points.size() - 1 : 0);
    if (intervals.empty() || regions.empty())
        return intervals;
    Lines lines;
    std::vector<size_t> source_segments;
    Polylines subjects;
    lines.reserve(intervals.size());
    subjects.reserve(intervals.size());
    for (size_t i = 1; i < points.size(); ++i) {
        const Point a = points[i - 1].to_point();
        const Point b = points[i].to_point();
        if (a == b)
            continue;
        lines.emplace_back(a, b);
        source_segments.push_back(i - 1);
        subjects.emplace_back(a, b);
    }
    if (lines.empty())
        return intervals;
    const AABBTreeLines::LinesDistancer<Line> index(lines);
    for (const Polyline &fragment : intersection_pl(subjects, regions)) {
        const Vec2d a = fragment.first_point().cast<double>();
        const Vec2d b = fragment.last_point().cast<double>();
        const Point middle = ((a + b) * 0.5).cast<coord_t>();
        // Clipper rounds intersection coordinates to integers. Check both ends
        // as well as the midpoint so a crossing source segment is not mistaken
        // for the segment which produced this fragment.
        for (size_t i : index.all_lines_in_radius(middle, 2.)) {
            const Vec2d origin = lines[i].a.cast<double>();
            const Vec2d delta = lines[i].b.cast<double>() - origin;
            const double length_squared = delta.squaredNorm();
            if (length_squared == 0.)
                continue;
            double start = (a - origin).dot(delta) / length_squared;
            double end = (b - origin).dot(delta) / length_squared;
            if ((origin + start * delta - a).squaredNorm() > 4. ||
                (origin + end * delta - b).squaredNorm() > 4.)
                continue;
            if (start > end)
                std::swap(start, end);
            start = std::clamp(start, 0., 1.);
            end = std::clamp(end, 0., 1.);
            if (end > start)
                intervals[source_segments[i]].emplace_back(start, end);
        }
    }
    return intervals;
}

struct SupportFanSpan
{
    double start;
    double end;
    double blend; // -1 outside an interface region.
};

inline std::vector<SupportFanSpan> support_fan_spans(
    const std::vector<std::pair<std::pair<double, double>, double>> &intervals)
{
    std::vector<double> cuts{0., 1.};
    for (const auto &entry : intervals) {
        cuts.push_back(entry.first.first);
        cuts.push_back(entry.first.second);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<SupportFanSpan> result;
    for (size_t i = 1; i < cuts.size(); ++i) {
        const double middle = (cuts[i - 1] + cuts[i]) * 0.5;
        double blend = -1.;
        for (const auto &entry : intervals)
            if (middle >= entry.first.first && middle <= entry.first.second)
                blend = blend < 0. ? entry.second : std::min(blend, entry.second);
        if (!result.empty() && result.back().blend == blend)
            result.back().end = cuts[i];
        else
            result.push_back({cuts[i - 1], cuts[i], blend});
    }
    return result;
}

inline std::vector<std::vector<SupportFanSpan>> support_fan_spans(
    const Points3 &points, const std::vector<std::pair<ExPolygons, double>> &regions)
{
    std::vector<std::vector<std::pair<std::pair<double, double>, double>>> intervals(points.size() > 1 ? points.size() - 1 : 0);
    for (const auto &[polygons, blend] : regions) {
        const auto clipped = support_interface_intervals(points, polygons);
        for (size_t i = 0; i < clipped.size(); ++i)
            for (const auto &interval : clipped[i])
                intervals[i].push_back({interval, blend});
    }
    std::vector<std::vector<SupportFanSpan>> result;
    result.reserve(intervals.size());
    for (const auto &segment : intervals)
        result.push_back(support_fan_spans(segment));
    return result;
}

// Keep genuine G2/G3 moves for fan-only changes. Find exact circle/edge
// intersections, then classify each angular span against the interface polygons.
inline std::vector<SupportFanSpan> support_fan_spans(
    const ArcSegment &arc, const std::vector<std::pair<ExPolygons, double>> &regions)
{
    constexpr double two_pi = 2. * PI;
    const Vec2d center = arc.center.cast<double>();
    const Vec2d start = arc.start_point.cast<double>() - center;
    const double theta = std::atan2(start.y(), start.x());
    const double direction = arc.direction == ArcDirection::Arc_Dir_CCW ? 1. : -1.;
    const double sweep = arc.radius > 0. ? arc.length / arc.radius : 0.;
    if (sweep <= 0.)
        return {{0., 1., -1.}};
    std::vector<double> cuts{0., 1.};
    for (const auto &[polygons, blend] : regions)
        for (const Polygon &polygon : to_polygons(polygons))
            for (const Line &edge : polygon.lines()) {
                const Vec2d a = edge.a.cast<double>() - center;
                const Vec2d delta = (edge.b - edge.a).cast<double>();
                const double aa = delta.squaredNorm();
                if (aa == 0.)
                    continue;
                const double bb = 2. * a.dot(delta);
                const double cc = a.squaredNorm() - arc.radius * arc.radius;
                const double discriminant = bb * bb - 4. * aa * cc;
                if (discriminant < 0.)
                    continue;
                for (double sign : {-1., 1.}) {
                    const double t = (-bb + sign * std::sqrt(discriminant)) / (2. * aa);
                    if (t < 0. || t > 1.)
                        continue;
                    const Vec2d point = a + t * delta;
                    double angle = direction * (std::atan2(point.y(), point.x()) - theta);
                    angle = std::fmod(angle + two_pi, two_pi);
                    if (angle > 0. && angle < sweep)
                        cuts.push_back(angle / sweep);
                }
            }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) { return std::abs(a - b) < 1e-10; }), cuts.end());
    std::vector<std::pair<std::pair<double, double>, double>> intervals;
    for (size_t i = 1; i < cuts.size(); ++i) {
        const double angle = theta + direction * sweep * (cuts[i - 1] + cuts[i]) * 0.5;
        const Point point = (center + arc.radius * Vec2d(std::cos(angle), std::sin(angle))).cast<coord_t>();
        for (const auto &[polygons, blend] : regions)
            if (std::any_of(polygons.begin(), polygons.end(), [&point](const ExPolygon &polygon) { return polygon.contains(point); }))
                intervals.push_back({{cuts[i - 1], cuts[i]}, blend});
    }
    return support_fan_spans(intervals);
}

inline Point support_arc_point(const ArcSegment &arc, double fraction)
{
    if (fraction <= 0.)
        return arc.start_point;
    if (fraction >= 1.)
        return arc.end_point;
    const Vec2d start = (arc.start_point - arc.center).cast<double>();
    const double direction = arc.direction == ArcDirection::Arc_Dir_CCW ? 1. : -1.;
    const double angle = std::atan2(start.y(), start.x()) + direction * fraction * arc.length / arc.radius;
    return (arc.center.cast<double>() + arc.radius * Vec2d(std::cos(angle), std::sin(angle))).cast<coord_t>();
}

// ProcessedPoint::speed applies to the segment starting at that point. Preserve
// the original speed across passes so overlapping percentage limits never stack.
inline bool limit_speed_above_support(std::vector<ProcessedPoint> &points, const ExPolygons &regions,
                                     const ConfigOptionFloatOrPercent &limit, bool mark_for_fan = false,
                                     double blend_to_normal = 0.)
{
    if ((limit.value <= 0 && !mark_for_fan) || regions.empty() || points.size() < 2)
        return false;
    Points3 positions;
    positions.reserve(points.size());
    for (const ProcessedPoint &point : points)
        positions.push_back(point.p);
    const auto intervals = support_interface_intervals(positions, regions);
    std::vector<ProcessedPoint> result;
    bool changed = false;
    for (size_t i = 1; i < points.size(); ++i) {
        const ProcessedPoint &a = points[i - 1];
        const ProcessedPoint &b = points[i];
        const double original_speed = a.above_support_original_speed >= 0.f ? a.above_support_original_speed : a.speed;
        const double first_layer_speed = limit.value > 0 ? std::min(original_speed, limit.get_abs_value(original_speed)) : original_speed;
        const double capped_speed = first_layer_speed + std::clamp(blend_to_normal, 0., 1.) * (original_speed - first_layer_speed);
        std::vector<std::pair<std::pair<double, double>, double>> marked;
        if (capped_speed < a.speed || mark_for_fan)
            for (const auto &interval : intervals[i - 1])
                marked.push_back({interval, blend_to_normal});
        for (const SupportFanSpan &span : support_fan_spans(marked)) {
            ProcessedPoint point = a;
            const Vec3d position = a.p.cast<double>() + span.start * (b.p.cast<double>() - a.p.cast<double>());
            point.p = Point3(position.x(), position.y(), position.z());
            point.overlap = float(a.overlap + span.start * (b.overlap - a.overlap));
            point.above_support_original_speed = float(original_speed);
            if (span.blend >= 0.) {
                point.speed = float(std::min(double(a.speed), capped_speed));
                if (mark_for_fan && (!point.above_support_interface || blend_to_normal < point.above_support_blend_to_normal)) {
                    point.above_support_interface = true;
                    point.above_support_blend_to_normal = float(blend_to_normal);
                }
                changed = true;
            }
            result.push_back(point);
        }
    }
    if (changed) {
        result.push_back(points.back());
        points = std::move(result);
    }
    return changed;
}

} // namespace Slic3r
