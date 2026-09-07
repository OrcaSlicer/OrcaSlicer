#ifndef slic3r_ExtrusionProcessor_hpp_
#define slic3r_ExtrusionProcessor_hpp_

// This algorithm is copied from PrusaSlicer, original author is Pavel Mikus(pavel.mikus.mail@seznam.cz)

#include "../AABBTreeLines.hpp"
//#include "../SupportSpotsGenerator.hpp"
#include "../libslic3r.h"
#include "../ExtrusionEntity.hpp"
#include "../Layer.hpp"
#include "../Point.hpp"
#include "../SVG.hpp"
#include "../BoundingBox.hpp"
#include "../Polygon.hpp"
#include "../ClipperUtils.hpp"
#include "../Flow.hpp"
#include "../Config.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "../Circle.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

template<int Dim> struct ExtendedPoint
{
    Eigen::Matrix<double, Dim, 1, Eigen::DontAlign> position;
    float distance;
    float curvature;
};

// A KNOWN_DISTANCES functor that knows no distances, so every input point is queried.
struct NoKnownDistances
{
    template<typename P> const double *operator()(const P &) const { return nullptr; }
};

template<bool SCALED_INPUT, bool ADD_INTERSECTIONS, bool PREV_LAYER_BOUNDARY_OFFSET, bool SIGNED_DISTANCE, bool CURVATURE = true,
         typename POINTS, typename L, typename KNOWN_DISTANCES = NoKnownDistances>
std::vector<ExtendedPoint<L::Dim>> estimate_points_properties(const POINTS&                           input_points,
                                                              const AABBTreeLines::LinesDistancer<L>& unscaled_prev_layer,
                                                              float                                   flow_width,
                                                              float                                   max_line_length = -1.0f,
                                                              float                                   min_distance    = -1.0f,
                                                              // Speed an overhang distance prints at. Without it, interior sampling
                                                              // is skipped and every line over 4mm is split.
                                                              const std::function<float(float)>&      distance_to_speed = {},
                                                              // Overlap (1 - distance / flow_width) at or below which the overhang
                                                              // fan switches on; negative when the fan does not depend on overlap.
                                                              float                                   fan_overlap_threshold = -1.0f,
                                                              // Returns an input point's signed distance if already known, else nullptr.
                                                              const KNOWN_DISTANCES&                  known_distance = KNOWN_DISTANCES{})
{
    bool   looped     = input_points.front() == input_points.back();
    std::function<size_t(size_t,size_t)> get_prev_index = [](size_t idx, size_t count) {
        if (idx > 0) {
            return idx - 1;
        } else
            return idx;
    };
    if (looped) {
        get_prev_index = [](size_t idx, size_t count) {
            if (idx == 0)
                idx = count;
            return --idx;
        };
    };
    std::function<size_t(size_t,size_t)> get_next_index = [](size_t idx, size_t size) {
        if (idx + 1 < size) {
            return idx + 1;
        } else
            return idx;
    };
    if (looped) {
        get_next_index = [](size_t idx, size_t count) {
            if (++idx == count)
                idx = 0;
            return idx;
        };
    };

    using P = typename POINTS::value_type;
    // ORCA:
    // minimum spacing threshold for any newly generated points
    // Setting the minimum spacing to be 25% of the flow width ensures the points are spaced far enough apart
    // to avoid micro stutters while the movement of the print head is still fine-grained enough to maintain
    // print quality.
    double min_spacing = flow_width*0.25;

    using AABBScalar = typename AABBTreeLines::LinesDistancer<L>::Scalar;
    using Vec        = Eigen::Matrix<double, L::Dim, 1, Eigen::DontAlign>;
    if (input_points.empty())
        return {};
    float boundary_offset = PREV_LAYER_BOUNDARY_OFFSET ? 0.5 * flow_width : 0.0f;
    auto  maybe_unscale   = [](const P& p) -> Vec { return SCALED_INPUT ? unscaled(p) : p.template cast<double>(); };

    using Distance = typename AABBTreeLines::LinesDistancer<L>::Floating;
    auto input_distance = [&unscaled_prev_layer, &known_distance](const P &input, const Vec &position) -> Distance {
        if (const double *known = known_distance(input))
            return Distance(*known);
        auto [distance, nearest_line, x] = unscaled_prev_layer.template distance_from_lines_extra<SIGNED_DISTANCE>(
            position.template cast<AABBScalar>());
        return distance;
    };

    std::vector<ExtendedPoint<L::Dim>> points;
    points.reserve(input_points.size() * (ADD_INTERSECTIONS ? 1.5 : 1));

    {
        ExtendedPoint<L::Dim> start_point{maybe_unscale(input_points.front())};
        start_point.distance = input_distance(input_points.front(), start_point.position) + boundary_offset;
        points.push_back(start_point);
    }
    for (size_t i = 1; i < input_points.size(); i++) {
        ExtendedPoint<L::Dim> next_point{maybe_unscale(input_points[i])};
        next_point.distance = input_distance(input_points[i], next_point.position) + boundary_offset;

        // Intersection handling
        if (ADD_INTERSECTIONS &&
            ((points.back().distance > boundary_offset + EPSILON) != (next_point.distance > boundary_offset + EPSILON))) {
            const ExtendedPoint<L::Dim>& prev_point    = points.back();
            auto                         intersections = unscaled_prev_layer.template intersections_with_line<true>(
                L{prev_point.position.template cast<AABBScalar>(), next_point.position.template cast<AABBScalar>()});
            for (const auto &intersection : intersections) {
                ExtendedPoint<L::Dim> p{};
                p.position = intersection.first.template cast<double>();
                p.distance = boundary_offset;
                // ORCA: Filter out points that are introduced at intersections if their distance from the previous or next point is not meaningful
                if ((p.position - prev_point.position).norm() > min_spacing &&
                    (next_point.position - p.position).norm() > min_spacing) {
                    points.push_back(p);
                }
            }
        }
        points.push_back(next_point);
    }

    // ORCA: How an overhang distance prints, which is what the passes below compare. A segment is printed at the lower
    // of the speeds at its two ends, and with the overhang fan on if the overlap at either end turns it on. A point
    // added to a path can therefore only change the G-code where it prints at a different speed or fan state from the
    // points either side of it, and the passes below add points there and nowhere else.
    const float width_inv = 1.f / flow_width;
    // Whether an overhang distance turns the overhang fan on. The overlap test check_overhang_fan applies in GCode.cpp.
    auto fan_on = [fan_overlap_threshold, width_inv](float distance) {
        return fan_overlap_threshold >= 0.f && 1.f - distance * width_inv <= fan_overlap_threshold;
    };
    // Whether two overhang distances are interchangeable ie have the same speed (beyond a 1mm/sec threshold that gcode.cpp filters out on) 
    // and the same fan state.
    auto same_speed_and_fan = [&distance_to_speed, &fan_on](float a, float b) {
        return std::abs(distance_to_speed(a) - distance_to_speed(b)) <= 1.f && fan_on(a) == fan_on(b);
    };
    // Whether the first overhang distance prints slower than the second, beyond the 1mm/sec gcode.cpp tolerance, or turns the overhang
    // fan on where the second does not. Against a supported point (overhang distance 0) it tells whether an end is
    // affected by the overhang.
    auto slower_or_cooled = [&distance_to_speed, &fan_on](float a, float b) {
        return distance_to_speed(a) < distance_to_speed(b) - 1.f || (fan_on(a) && !fan_on(b));
    };

    // ORCA: Interior sampling
    // The passes below infer the support under a span from its endpoints alone, so a part that is supported
    // differently from both ends is invisible to them. The outer perimeter of an overhang whose ends are supported by
    // reads as supported along its whole length. Probe the interior, keep the samples the endpoint interpolation fails to predict, 
    // and bisect either side of each one, so a span that is only partly unsupported gets points where its support actually changes 
    // instead of one reading being spread across all of it.
    // Skipped where there is nothing to find: min_distance <= 0 when no overhang can slow this path down, and
    // fan_overlap_threshold < 0 when no overhang switches the fan on. It also needs distance_to_speed to tell which of
    // the points it could add would change the G-code.
    if (PREV_LAYER_BOUNDARY_OFFSET && ADD_INTERSECTIONS && distance_to_speed && (min_distance > 0 || fan_overlap_threshold >= 0.f)) {
        // Probe at least this densely before treating matching samples as evidence that a span is uniform. The
        // segmentation pass below only splits lines of 2mm or more, and every pass here drops points closer
        // together than min_spacing, so finer discovery would not produce a more precise transition.
        const double max_probe_spacing = std::max(2., 4. * min_spacing);
        // A backstop for that length test, which on a non-finite length would never be met.
        constexpr int max_bisection_depth = 10;

        // Part of a segment still to bisect: its positions along the segment and bisections left.
        struct Subspan { double t0, t1; int depth; };

        std::vector<ExtendedPoint<L::Dim>>    sampled_points; // Populated lazily, on the first insertion
        std::vector<std::pair<double, float>> interior;       // Samples of one segment, keyed by position along it
        std::vector<Subspan>                  pending;

        for (size_t point_idx = 0; point_idx + 1 < points.size(); ++point_idx) {
            const ExtendedPoint<L::Dim>& curr     = points[point_idx];
            const ExtendedPoint<L::Dim>& next     = points[point_idx + 1];
            const Vec                    step     = next.position - curr.position;
            const double                 line_len = step.norm();

            interior.clear();
            if (line_len >= max_probe_spacing)
                pending.push_back({0., 1., max_bisection_depth});

            while (!pending.empty()) {
                const Subspan subspan = pending.back();
                pending.pop_back();
                if (subspan.depth <= 0 || (subspan.t1 - subspan.t0) * line_len < max_probe_spacing)
                    continue;

                const double t = 0.5 * (subspan.t0 + subspan.t1);
                auto [distance, nearest_line, x] = unscaled_prev_layer.template distance_from_lines_extra<SIGNED_DISTANCE>(
                    (curr.position + t * step).template cast<AABBScalar>());
                const float sampled = float(distance + boundary_offset);

                interior.emplace_back(t, sampled);
                pending.push_back({subspan.t0, t, subspan.depth - 1});
                pending.push_back({t, subspan.t1, subspan.depth - 1});
            }

            if (!interior.empty()) {
                std::sort(interior.begin(), interior.end(),
                          [](const std::pair<double, float>& l, const std::pair<double, float>& r) { return l.first < r.first; });
                // Coarse probing keeps every sample it took until this pass can see which ones bracket a speed or
                // fan transition. Matching samples cannot be discarded during discovery: one may be the last
                // supported point before a narrow unsupported pocket found by a later probe.
                size_t kept = 0;
                for (size_t i = 0; i < interior.size(); ++i) {
                    const float sample    = interior[i].second;
                    const bool  at_start  = kept == 0;                     // Nothing kept yet, so the segment's own start precedes it
                    const bool  at_end    = i + 1 == interior.size();      // And nothing follows the last sample but the segment's end
                    const float before    = at_start ? curr.distance : interior[kept - 1].second;
                    const float after     = at_end ? next.distance : interior[i + 1].second;
                    // A sample is worth a point in the path only where it prints differently, in speed or fan state,
                    // from the points either side of it. Differing from one of the segment's own ends is not enough on
                    // its own where the sample is not the slower or cooled of the two: the segmentation pass below
                    // already confines the slowdown and cooling at an end, at a distance taken from how far out that
                    // end is rather than from wherever bisection happened to stop, and a point here would leave the
                    // span beside the end too short for that pass to run at all. Support an end cannot account for,
                    // where the interior is the slower or cooled of the two, is exactly what this pass is here to find.
                    const bool worth_before = !same_speed_and_fan(sample, before) && (!at_start || slower_or_cooled(sample, before));
                    const bool worth_after  = !same_speed_and_fan(sample, after) && (!at_end || slower_or_cooled(sample, after));
                    if (worth_before || worth_after)
                        interior[kept++] = interior[i];
                }
                interior.resize(kept);
            }

            if (!interior.empty() && sampled_points.empty()) {
                sampled_points.reserve(points.size() + 8);
                sampled_points.assign(points.begin(), points.begin() + point_idx + 1);
            }
            if (!sampled_points.empty()) {
                // Only a sub-span of max_probe_spacing or more is ever bisected, so these sit at least
                // 2 * min_spacing apart, and need none of the filtering the passes either side of this one do.
                for (const auto& [t, distance] : interior)
                    sampled_points.push_back({curr.position + t * step, distance});
                sampled_points.push_back(next);
            }
        }
        if (!sampled_points.empty())
            points = std::move(sampled_points);
    }

    // Segmentation handling
    if (PREV_LAYER_BOUNDARY_OFFSET && ADD_INTERSECTIONS) {
        std::vector<ExtendedPoint<L::Dim>> new_points;
        new_points.reserve(points.size() * 2);
        new_points.push_back(points.front());
        for (int point_idx = 0; point_idx < int(points.size()) - 1; ++point_idx) {
            const ExtendedPoint<L::Dim>& curr = points[point_idx];
            const ExtendedPoint<L::Dim>& next = points[point_idx + 1];

            if ((curr.distance > -boundary_offset && curr.distance < boundary_offset + 2.0f) ||
                (next.distance > -boundary_offset && next.distance < boundary_offset + 2.0f)) {
                double line_len = (next.position - curr.position).norm();

                // ORCA: A line prints as slow as its slower end and is cooled if either end is, so an overhang at one
                // end would otherwise slow down or cool the whole line. Split the line so that only the part beside
                // that end prints that way, if the line is reasonably long (2mm or more) and at least one end prints
                // slower or cooled compared with a supported point (overhang distance 0). Deciding on how the end
                // prints, rather than on its overhang distance against min_distance, also catches an end whose overhang
                // distance is exactly where the slowdown begins, such as an outline crossing at half a line width.
                // Without distance_to_speed, split every line over 4mm.
                const bool split_line = distance_to_speed ?
                    line_len >= 2.f && (slower_or_cooled(curr.distance, 0.f) || slower_or_cooled(next.distance, 0.f)) :
                    line_len > 4.0f;
                if (split_line) {
                    // Each end's piece is that end's overhang distance plus 1.5 line widths (3 * boundary_offset) long:
                    // a0 ends the piece beside curr, a1 starts the piece beside next.
                    double a0 = std::clamp((curr.distance + 3 * boundary_offset) / line_len, 0.0, 1.0);
                    double a1 = std::clamp(1.0f - (next.distance + 3 * boundary_offset) / line_len, 0.0, 1.0);
                    double t0 = std::min(a0, a1);
                    double t1 = std::max(a0, a1);

                    // Up to two cut points, in order along the line. Each takes its own overhang distance, so every
                    // piece prints by the overhang distances at its own two ends. t0 >= 1 or t1 <= 0 falls on the
                    // line's own end, so there is no cut. A cut closer than min_spacing to either end of the line is
                    // not meaningful and is filtered out (#6714).
                    ExtendedPoint<L::Dim> cut[2]{};
                    bool                  keep[2] = {false, false};
                    for (int k = 0; k < 2; ++k) {
                        const double t = k == 0 ? t0 : t1;
                        if (k == 0 ? t >= 1.0 : t <= 0.0)
                            continue;
                        const Vec p                  = curr.position + t * (next.position - curr.position);
                        auto [p_dist, p_near_l, p_x] = unscaled_prev_layer.template distance_from_lines_extra<SIGNED_DISTANCE>(
                            p.template cast<AABBScalar>());
                        cut[k].position = p;
                        cut[k].distance = float(p_dist + boundary_offset);
                        keep[k] = (p - curr.position).norm() > min_spacing && (next.position - p).norm() > min_spacing;
                    }
                    if (distance_to_speed) {
                        // Only keep a cut that changes the G-code: one that prints differently from at least one of the
                        // points either side of it, which are the line's ends or the other cut where that is kept. A cut
                        // that prints like both would only split a move into two identical ones.
                        if (keep[0])
                            keep[0] = !same_speed_and_fan(cut[0].distance, curr.distance) ||
                                      !same_speed_and_fan(cut[0].distance, keep[1] ? cut[1].distance : next.distance);
                        if (keep[1])
                            keep[1] = !same_speed_and_fan(cut[1].distance, keep[0] ? cut[0].distance : curr.distance) ||
                                      !same_speed_and_fan(cut[1].distance, next.distance);
                        // Two cuts closer together than min_spacing would leave a micro segment between them, so only the
                        // first is kept.
                        if (keep[0] && keep[1] && (cut[1].position - cut[0].position).norm() <= min_spacing)
                            keep[1] = false;
                    }
                    for (int k = 0; k < 2; ++k)
                        if (keep[k])
                            new_points.push_back(cut[k]);
                }
            }
            new_points.push_back(next);
        }
        points = std::move(new_points);
    }

    // Maximum line length handling
    if (max_line_length > 0) {
        std::vector<ExtendedPoint<L::Dim>> new_points;
        new_points.reserve(points.size() * 2);
        {
            for (size_t i = 0; i + 1 < points.size(); i++) {
                const ExtendedPoint<L::Dim>& curr = points[i];
                const ExtendedPoint<L::Dim>& next = points[i + 1];
                new_points.push_back(curr);
                double len             = (next.position - curr.position).squaredNorm();
                double t               = sqrt((max_line_length * max_line_length) / len);
                size_t new_point_count = 1.0 / t;
                for (size_t j = 1; j < new_point_count + 1; j++) {
                    Vec pos    = curr.position * (1.0 - j * t) + next.position * (j * t);
                    auto [p_dist, p_near_l,
                          p_x] = unscaled_prev_layer.template distance_from_lines_extra<SIGNED_DISTANCE>(pos.template cast<AABBScalar>());
                    ExtendedPoint<L::Dim> new_p{};
                    new_p.position = pos;
                    new_p.distance = float(p_dist + boundary_offset);
                    
                    // ORCA: Filter out points that are introduced if their distance from the previous or next point is not meaningful
                    if ((pos - curr.position).norm() > min_spacing && (next.position - pos).norm() > min_spacing) {
                        new_points.push_back(new_p);
                    }
                }
            }
            new_points.push_back(points.back());
        }
        points = std::move(new_points);
    }

    if constexpr (!CURVATURE)
        return points;

    // Curvature calculation
    float accumulated_distance = 0;
    std::vector<float> distances_for_curvature(points.size());
    for (size_t point_idx = 0; point_idx < points.size(); ++point_idx) {
        const ExtendedPoint<L::Dim>& a = points[point_idx];
        const ExtendedPoint<L::Dim>& b = points[get_prev_index(point_idx, points.size())];

        distances_for_curvature[point_idx] = (b.position - a.position).norm();
        accumulated_distance += distances_for_curvature[point_idx];
    }

    if (accumulated_distance > EPSILON)
        for (float window_size : {3.0f, 9.0f, 16.0f}) {
            for (int point_idx = 0; point_idx < int(points.size()); ++point_idx) {
                ExtendedPoint<L::Dim>& current = points[point_idx];

                Vec back_position = current.position;
                {
                    size_t back_point_index = point_idx;
                    float  dist_backwards   = 0;
                    while (dist_backwards < window_size * 0.5 && back_point_index != get_prev_index(back_point_index, points.size())) {
                        float line_dist = distances_for_curvature[get_prev_index(back_point_index, points.size())];
                        if (dist_backwards + line_dist > window_size * 0.5) {
                            back_position = points[back_point_index].position +
                                            (window_size * 0.5 - dist_backwards) *
                                                (points[get_prev_index(back_point_index, points.size())].position -
                                                 points[back_point_index].position)
                                                    .normalized();
                            dist_backwards += window_size * 0.5 - dist_backwards + EPSILON;
                        } else {
                            dist_backwards += line_dist;
                            back_point_index = get_prev_index(back_point_index, points.size());
                        }
                    }
                }

                Vec front_position = current.position;
                {
                    size_t front_point_index = point_idx;
                    float  dist_forwards     = 0;
                    while (dist_forwards < window_size * 0.5 && front_point_index != get_next_index(front_point_index, points.size())) {
                        float line_dist = distances_for_curvature[front_point_index];
                        if (dist_forwards + line_dist > window_size * 0.5) {
                            front_position = points[front_point_index].position +
                                             (window_size * 0.5 - dist_forwards) *
                                                 (points[get_next_index(front_point_index, points.size())].position -
                                                  points[front_point_index].position)
                                                     .normalized();
                            dist_forwards += window_size * 0.5 - dist_forwards + EPSILON;
                        } else {
                            dist_forwards += line_dist;
                            front_point_index = get_next_index(front_point_index, points.size());
                        }
                    }
                }

                float new_curvature = angle((current.position - back_position).template head<2>(),
                                            (front_position - current.position).template head<2>()) /
                                      window_size;
                if (abs(current.curvature) < abs(new_curvature)) {
                    current.curvature = new_curvature;
                }
            }
        }

    return points;
}

struct Point3Hash
{
    size_t operator()(const Vec3crd &pt) const noexcept { return size_t(((89 * 31 + int64_t(pt.x())) * 31 + pt.y()) * 31 + pt.z()); }
};

// The trees of the layer below an object layer, and the signed distances from the layer's perimeter and bridge
// vertices to that layer's outline, computed for ExtrusionQualityEstimator ahead of the G-code generator.
struct PrecomputedOverhangLayer
{
    const PrintObject                                               *object{nullptr};
    const Layer                                                     *layer{nullptr};
    std::shared_ptr<const AABBTreeLines::LinesDistancer<Linef3>>     lower_boundaries;
    std::shared_ptr<const AABBTreeLines::LinesDistancer<CurledLine>> lower_curled_lines;
    std::unordered_map<Point3, double, Point3Hash>                   distances;
};

// `layer` must have a layer below it; leave out `curled_lines` only when no region of `layer` slows down for curled
// perimeters.
inline PrecomputedOverhangLayer precompute_overhang_layer(const PrintObject *object, const Layer &layer, bool curled_lines = true)
{
    PrecomputedOverhangLayer out{object, &layer,
                                 std::make_shared<const AABBTreeLines::LinesDistancer<Linef3>>(to_unscaled_linesf3(layer.lower_layer->lslices)),
                                 curled_lines ? std::make_shared<const AABBTreeLines::LinesDistancer<CurledLine>>(layer.lower_layer->curled_lines) :
                                                nullptr,
                                 {}};
    const AABBTreeLines::LinesDistancer<Linef3> &lower = *out.lower_boundaries;
    auto add_path = [&out, &lower](const ExtrusionPath &path) {
        if (!is_bridge(path.role()) && !is_perimeter(path.role()))
            return;
        for (const Point3 &point : path.polyline.points)
            if (auto [it, inserted] = out.distances.try_emplace(point, 0.); inserted) {
                const Eigen::Matrix<double, 3, 1, Eigen::DontAlign> position = unscaled(point);
                auto [distance, nearest_line, x] = lower.distance_from_lines_extra<true>(position.cast<double>());
                it->second = distance;
            }
    };
    for (const LayerRegion *region : layer.regions()) {
        for_each_extrusion_path(region->perimeters, add_path);
        for_each_extrusion_path(region->fills, add_path);
    }
    return out;
}

struct ProcessedPoint
{
    Point3 p;
    float speed = 1.0f;
    float overlap = 1.0f;
    // Orca: Preview geometry must not include the artificial curled-edge distance used for speed and cooling.
    float overhang_percentage = 0.0f;
};

class ExtrusionQualityEstimator
{
    using Boundaries   = AABBTreeLines::LinesDistancer<Linef3>;
    using CurledLines  = AABBTreeLines::LinesDistancer<CurledLine>;
    std::unordered_map<const PrintObject*, std::shared_ptr<const Boundaries>>  prev_layer_boundaries;
    std::unordered_map<const PrintObject*, std::shared_ptr<const Boundaries>>  next_layer_boundaries;
    std::unordered_map<const PrintObject*, std::shared_ptr<const CurledLines>> prev_curled_extrusions;
    // The layers the trees above are built from, and the layers prepared last.
    std::unordered_map<const PrintObject*, const Layer*>                      prev_layer_sources;
    std::unordered_map<const PrintObject*, const Layer*>                      last_prepared_layers;
    std::vector<PrecomputedOverhangLayer>                                     precomputed_layers;
    const PrintObject                                                        *current_object;

    const PrecomputedOverhangLayer *precomputed_for(const PrintObject *object) const
    {
        auto it = std::find_if(precomputed_layers.begin(), precomputed_layers.end(),
                               [object](const PrecomputedOverhangLayer &layer) { return layer.object == object; });
        return it == precomputed_layers.end() ? nullptr : &*it;
    }

    template<typename T> static const T &or_empty(const std::shared_ptr<const T> &tree)
    {
        static const T empty;
        return tree ? *tree : empty;
    }

    // Build the current contour only when opt-in preview metadata needs it.
    const Boundaries &current_boundaries()
    {
        auto &tree = next_layer_boundaries[current_object];
        const Layer *layer = last_prepared_layers[current_object];
        if (!tree && layer != nullptr)
            tree = std::make_shared<const Boundaries>(to_unscaled_linesf3(layer->lslices));
        return or_empty(tree);
    }

    struct BoundaryProjection
    {
        double signed_distance;
        size_t line_id;
        Vec3d position;
    };

    // Orca: Resolve corner ties in favor of a contour segment parallel to the extrusion. Pure nearest
    // distance may associate a horizontal move with an adjacent vertical edge and report that edge's
    // layer-to-layer displacement instead of the displacement of the wall being printed.
    BoundaryProjection boundary_projection_at(const AABBTreeLines::LinesDistancer<Linef3> &boundaries,
                                               const Vec3d &position, float width,
                                               const Vec2d &path_direction)
    {
        auto [signed_distance, line_id, boundary_position] =
            boundaries.distance_from_lines_extra<true>(position);
        constexpr size_t no_line = size_t(-1);
        const double direction_length = path_direction.norm();
        if (line_id == no_line || direction_length <= EPSILON)
            return {signed_distance, line_id, boundary_position};

        const Vec2d unit_direction = path_direction / direction_length;
        double best_score = std::numeric_limits<double>::infinity();
        const int side = boundaries.outside(position);
        for (const size_t candidate_id : boundaries.all_lines_in_radius(position, 1.1 * width)) {
            const Linef3 &line = boundaries.get_line(candidate_id);
            const Vec2d line_direction = (line.b - line.a).head<2>();
            const double line_length = line_direction.norm();
            if (line_length <= EPSILON)
                continue;
            const double alignment = std::abs(unit_direction.dot(line_direction / line_length));
            Vec3d candidate_position;
            const double candidate_distance =
                std::sqrt(line_alg::distance_to_squared(line, position, &candidate_position));
            const double score = candidate_distance + width * (1.0 - alignment);
            if (score < best_score) {
                best_score = score;
                signed_distance = candidate_distance * side;
                line_id = candidate_id;
                boundary_position = candidate_position;
            }
        }
        return {signed_distance, line_id, boundary_position};
    }

    // Orca: Measure displacement from the current contour instead of assuming every path center is
    // exactly half a width inside it. Arachne width and placement changes otherwise make identical
    // consecutive contours look unsupported; fitted arcs need the same correction for their curves.
    float overhang_percentage_at(Vec3d position, float width, const Vec2d &path_direction,
                                 bool associate_current_contour)
    {
        // Slice boundaries lie in XY. Z-contoured paths carry an extrusion-height offset which
        // must not become part of the horizontal unsupported-width measurement.
        position.z() = 0.0;
        const auto [previous_distance, previous_line_id, previous_position] =
            or_empty(prev_layer_boundaries[current_object]).distance_from_lines_extra<true>(position);
        const float area_percentage = float(100.0 * std::clamp((previous_distance + 0.5 * width) / width, 0.0, 1.0));
        // Orca: Bridges may run through a current-layer solid region whose outer contour says nothing
        // about support below the bridge. Keep their original area-based unsupported-width formula.
        if (!associate_current_contour)
            return area_percentage;
        const BoundaryProjection current = boundary_projection_at(
            current_boundaries(), position, width, path_direction);
        constexpr size_t no_line = size_t(-1);
        // Orca: An opposite-facing lower boundary belongs to the other side of a thin wall or a
        // nearby hole, not the current wall's support contour. Its unsupported area must survive
        // even if the current outer boundary itself is unchanged between layers.
        if (current.line_id != no_line && previous_line_id != no_line) {
            const Linef3 &current_line = current_boundaries().get_line(current.line_id);
            const Linef3 &previous_line = or_empty(prev_layer_boundaries[current_object]).get_line(previous_line_id);
            if ((current_line.b - current_line.a).dot(previous_line.b - previous_line.a) < 0.0) {
                // Orca: Retain placement correction against the nearest current boundary so an
                // identical thin contour still reads zero, but do not cap by outer-wall displacement.
                const double current_distance = current_boundaries().distance_from_lines<true>(position);
                const double inset_error = std::isfinite(current_distance) ?
                    std::max(0.0, current_distance + 0.5 * width) : 0.0;
                return float(100.0 * std::clamp((previous_distance + 0.5 * width - inset_error) / width, 0.0, 1.0));
            }
        }
        // Orca: Correct only a path center closer than half a width to its own contour. Deeper inner
        // walls need no correction, and a missing current contour retains the conservative estimate.
        const double current_inset_error = std::isfinite(current.signed_distance) ?
            std::max(0.0, current.signed_distance + 0.5 * width) : 0.0;
        double unsupported_width = previous_distance + 0.5 * width - current_inset_error;
        // Orca: Near an outer contour, cap the unsupported width by the displacement of the associated
        // current boundary point. This rejects a perpendicular neighboring edge as the lower layer's
        // nearest support at a corner. Deeper paths, including bridge interiors, keep the area-based
        // distance because their support cannot be inferred from the outer contour displacement.
        if (current.line_id != no_line && std::isfinite(current.signed_distance) &&
            current.signed_distance >= -0.55 * width) {
            const BoundaryProjection previous = boundary_projection_at(
                or_empty(prev_layer_boundaries[current_object]), current.position, width, path_direction);
            if (std::isfinite(previous.signed_distance))
                unsupported_width = std::min(unsupported_width, std::max(0.0, previous.signed_distance));
        }
        return float(100.0 * std::clamp(unsupported_width / width, 0.0, 1.0));
    }

    float segment_overhang_percentage(const Vec3d &start, const Vec3d &end, float width,
                                      bool associate_current_contour)
    {
        if (width <= EPSILON)
            return 0.0f;
        const Vec2d direction = (end - start).head<2>();
        const double probe_spacing = std::max(0.1, double(width));
        const size_t intervals = std::max<size_t>(1, size_t(std::ceil(direction.norm() / probe_spacing)));
        float maximum = 0.0f;
        // Use cell centres to keep shared endpoints from attributing a neighboring wall's overhang
        // to this span. Sample the interior even if speed-based splitting retained a long segment.
        for (size_t sample = 0; sample < intervals && maximum < 100.0f; ++sample)
            maximum = std::max(maximum, overhang_percentage_at(
                start + (end - start) * ((double(sample) + 0.5) / intervals), width, direction,
                associate_current_contour));
        return maximum;
    }

public:
    void set_current_object(const PrintObject *object) { current_object = object; }

    // Takes the data computed ahead for the layer about to be generated, replacing the previous layer's.
    void set_precomputed_layers(std::vector<PrecomputedOverhangLayer> &&layers) { precomputed_layers = std::move(layers); }

    // Measures the layer against its actual lower layer.
    void prepare_for_new_layer(const PrintObject *obj, const Layer *layer)
    {
        if (layer == nullptr) return;
        const PrintObject *object = obj;
        // Always measure the actual lower layer, including after skipped preparations.
        const Layer *prev = layer->lower_layer;
        last_prepared_layers[object] = layer;
        next_layer_boundaries[object].reset();
        prev_layer_sources[object] = prev;
        const PrecomputedOverhangLayer *precomputed = precomputed_for(object);
        if (prev == nullptr) {
            prev_layer_boundaries[object]  = nullptr;
            prev_curled_extrusions[object] = nullptr;
        } else if (precomputed != nullptr && precomputed->layer == layer && layer->lower_layer == prev) {
            prev_layer_boundaries[object]  = precomputed->lower_boundaries;
            prev_curled_extrusions[object] = precomputed->lower_curled_lines;
        } else {
            prev_layer_boundaries[object]  = std::make_shared<const Boundaries>(to_unscaled_linesf3(prev->lslices));
            prev_curled_extrusions[object] = std::make_shared<const CurledLines>(prev->curled_lines);
        }
    }

    // Orca: Return the unsupported part of the extrusion width for every original path segment, in percent.
    // This uses the same signed-distance calculation as the overhang speed estimator. Probe interiors too:
    // supported endpoints can hide a recessed support contour. Each unchanged segment gets its largest
    // sampled percentage; collecting metadata must not split printer moves.
    std::vector<float> estimate_overhang_percentages(const ExtrusionPath &path)
    {
        const size_t segments_count = path.polyline.points.size() > 1 ? path.polyline.points.size() - 1 : 0;
        std::vector<float> percentages(segments_count, 0.0f);
        if (segments_count == 0 || path.width <= EPSILON)
            return percentages;

        for (size_t i = 0; i < percentages.size(); ++i) {
            percentages[i] = segment_overhang_percentage(unscaled(path.polyline.points[i]),
                unscaled(path.polyline.points[i + 1]), path.width, is_perimeter(path.role()));
        }
        return percentages;
    }

    // Orca: Sample the fitted circle itself, not its source chords. Samples include both endpoints
    // and are uniformly spaced in arc progress, so they can be mapped onto any preview tessellation.
    // The caller bounds the interval count; sampling must never modify the fitted printer move.
    std::vector<float> estimate_overhang_arc_percentages(const ArcSegment &arc, float width, size_t intervals,
                                                         bool associate_current_contour = true)
    {
        if (!arc.is_valid() || width <= EPSILON || intervals == 0)
            return {};
        std::vector<float> percentages(intervals + 1);
        const Vec2d center = unscaled(arc.center);
        const double radius = arc.radius * SCALING_FACTOR;
        // Orca: The shared current-contour baseline also cancels the apparent protrusion caused by
        // replacing the current layer's polygonal chords with the fitted curve.
        for (size_t i = 0; i <= intervals; ++i) {
            const double angle = arc.polar_start_theta + arc.angle_radians * (double(i) / intervals);
            const Vec3d position(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle), 0.0);
            const Vec2d tangent(-std::sin(angle), std::cos(angle));
            percentages[i] = overhang_percentage_at(position, width, tangent, associate_current_contour);
        }
        return percentages;
    }

    std::vector<ProcessedPoint> estimate_extrusion_quality(const ExtrusionPath                &path,
                                                           const ConfigOptionPercents         &overlaps,
                                                           const ConfigOptionFloatsOrPercents &speeds,
                                                           float                               ext_perimeter_speed,
                                                           float                               original_speed,
                                                           bool								   slowdown_for_curled_edges,
                                                           // Overlap at or below which the overhang fan switches on; negative when the fan
                                                           // does not depend on overlap.
                                                           float                               fan_overlap_threshold = -1.0f,
                                                           bool                                estimate_overhang_metadata = false)
    {
        size_t                               speed_sections_count = std::min(overlaps.values.size(), speeds.values.size());
        std::vector<std::pair<float, float>> speed_sections;
        
        
        
        for (size_t i = 0; i < speed_sections_count; i++) {
            float distance = path.width * (1.0 - (overlaps.get_at(i) / 100.0));
            float speed    = speeds.get_at(i).percent ? (ext_perimeter_speed * speeds.get_at(i).value / 100.0) : speeds.get_at(i).value;
            speed_sections.push_back({distance, speed});
        }
        std::sort(speed_sections.begin(), speed_sections.end(),
                  [](const std::pair<float, float> &a, const std::pair<float, float> &b) { 
                    if (a.first == b.first) {
                        return a.second > b.second;
                    }
                    return a.first < b.first; });

        std::pair<float, float> last_section{INFINITY, 0};
        for (auto &section : speed_sections) {
            if (section.first == last_section.first) {
                section.second = last_section.second;
            } else {
                last_section = section;
            }
        }
        
        // Orca: Find the smallest overhang distance where speed adjustments begin
        float smallest_distance_with_lower_speed = std::numeric_limits<float>::infinity(); // Initialize to a large value
        bool found = false;
        for (const auto& section : speed_sections) {
            if (section.second <= original_speed) {
                if (section.first < smallest_distance_with_lower_speed) {
                    smallest_distance_with_lower_speed = section.first;
                    found = true;
                }
            }
        }

        // If no overhang distance slows this path down, -1 turns interior sampling off unless the overhang fan can switch.
        // Lines are only split where an end prints slower or cooled, so here only a fan switch splits them.
        if (!found)
            smallest_distance_with_lower_speed=-1.f;

        // Orca: Pass to the point properties estimator the smallest ovehang distance that triggers a slowdown (smallest_distance_with_lower_speed)
        auto calculate_speed = [&speed_sections, &original_speed](float distance) {
            float final_speed;
            if (distance <= speed_sections.front().first) {
                final_speed = original_speed;
            } else if (distance >= speed_sections.back().first) {
                final_speed = speed_sections.back().second;
            } else {
                size_t section_idx = 0;
                while (distance > speed_sections[section_idx + 1].first) {
                    section_idx++;
                }
                float t = (distance - speed_sections[section_idx].first) /
                          (speed_sections[section_idx + 1].first - speed_sections[section_idx].first);
                t           = std::clamp(t, 0.0f, 1.0f);
                final_speed = (1.0f - t) * speed_sections[section_idx].second + t * speed_sections[section_idx + 1].second;
            }
            return round(final_speed);
        };

        // ORCA: The speed sections are built from ext_perimeter_speed, which can be above the speed this path prints at
        // (original_speed, e.g. held down by resonance avoidance). Every segment is capped at original_speed below, so
        // overhang distances whose speeds differ only above it print the same and must not count as a speed change when
        // the path is split.
        auto effective_speed = [&calculate_speed, original_speed](float distance) {
            return std::min(calculate_speed(distance), original_speed);
        };

        // Precomputed distances hold only if they were measured against the layer prev_layer_boundaries is built from.
        const std::unordered_map<Point3, double, Point3Hash> *known = nullptr;
        if (const PrecomputedOverhangLayer *precomputed = precomputed_for(current_object);
            precomputed != nullptr && precomputed->layer->lower_layer == prev_layer_sources[current_object])
            known = &precomputed->distances;
        const Boundaries  &prev_boundaries = or_empty(prev_layer_boundaries[current_object]);
        const CurledLines &prev_curled     = or_empty(prev_curled_extrusions[current_object]);
        auto known_distance = [known](const Point3 &point) -> const double * {
            if (known == nullptr)
                return nullptr;
            auto it = known->find(point);
            return it == known->end() ? nullptr : &it->second;
        };

        std::vector<ExtendedPoint<3>> extended_points =
            estimate_points_properties<true, true, true, true, false>(path.polyline.points, prev_boundaries, path.width, -1,
                                                               smallest_distance_with_lower_speed, effective_speed, fan_overlap_threshold,
                                                               known_distance);
        const auto width_inv = 1.0f / path.width;
        std::vector<ProcessedPoint> processed_points;
        processed_points.reserve(extended_points.size());
        for (size_t i = 0; i < extended_points.size(); i++) {
            const ExtendedPoint<3>& curr = extended_points[i];
            const ExtendedPoint<3>& next = extended_points[i + 1 < extended_points.size() ? i + 1 : i];

            float artificial_distance_to_curled_lines = 0.0;
            if(slowdown_for_curled_edges) {
            	// The following code artifically increases the distance to provide slowdown for extrusions that are over curled lines
            	const double dist_limit = 10.0 * path.width;
				{
                    Vec3d middle       = 0.5 * (curr.position + next.position);
                    auto  line_indices = prev_curled.all_lines_in_radius(Point::new_scale(middle),
                                                                                                    scale_(dist_limit));
                    if (!line_indices.empty()) {
                        double len = (next.position - curr.position).norm();
                        // For long lines, there is a problem with the additional slowdown. If by accident, there is small curled line near the middle of this long line
                    	//  The whole segment gets slower unnecesarily. For these long lines, we do additional check whether it is worth slowing down.
                    	// NOTE that this is still quite rough approximation, e.g. we are still checking lines only near the middle point
                    	// TODO maybe split the lines into smaller segments before running this alg? but can be demanding, and GCode will be huge
                    	if (len > 2) {
                            Vec2d dir   = Vec2d(next.position.head<2>() - curr.position.head<2>()) / len;
                            Vec2d right = Vec2d(-dir.y(), dir.x());

                            Polygon box_of_influence = {
                                scaled(Vec2d(curr.position.head<2>() + right * dist_limit)),
                                scaled(Vec2d(next.position.head<2>() + right * dist_limit)),
                                scaled(Vec2d(next.position.head<2>() - right * dist_limit)),
                                scaled(Vec2d(curr.position.head<2>() - right * dist_limit)),
                            };

                            double projected_lengths_sum = 0;
                            for (size_t idx : line_indices) {
                                const CurledLine& line   = prev_curled.get_line(idx);
                                Lines             inside = intersection_ln({{line.a, line.b}}, {box_of_influence});
                            	if (inside.empty())
                                	continue;
                            	double projected_length = abs(dir.dot(unscaled(Vec2d((inside.back().b - inside.back().a).cast<double>()))));
                            	projected_lengths_sum += projected_length;
                            }
                            if (projected_lengths_sum < 0.4 * len) {
                                line_indices.clear();
                            }
                        }
                    
                    	for (size_t idx : line_indices) {
                        	const CurledLine &line                 = prev_curled.get_line(idx);
                        	float             distance_from_curled = unscaled(line_alg::distance_to(line, Point::new_scale(middle)));
                        	float             dist                 = path.width * (1.0 - (distance_from_curled / dist_limit)) *
                                     (1.0 - (distance_from_curled / dist_limit)) *
                                     (line.curled_height / (path.height * 10.0f)); // max_curled_height_factor from SupportSpotGenerator
                        	artificial_distance_to_curled_lines = std::max(artificial_distance_to_curled_lines, dist);
                    	}
                    }
                }
            }	

            float extrusion_speed = std::min(calculate_speed(curr.distance), calculate_speed(next.distance));
            // ORCA: Clamp resulting speed to lowest of calculated speed based on the overhang values and the current speed
            // Fixes bug where resulting overhang speed is higher than the current speed due to (for example) volumetric flow limits.
            extrusion_speed = std::min(extrusion_speed, original_speed);
            
            if(slowdown_for_curled_edges) {
                float curled_speed = calculate_speed(artificial_distance_to_curled_lines);
            	extrusion_speed       = std::min(curled_speed, extrusion_speed); // adjust extrusion speed based on what is smallest - the calculated overhang speed or the artificial curled speed
            }
            
            float overlap = std::min(1 - (curr.distance+artificial_distance_to_curled_lines) * width_inv, 1 - (next.distance+artificial_distance_to_curled_lines) * width_inv);

            // Orca: Keep the existing speed/fan overlap untouched. Only pay for current-contour queries
            // when opt-in preview metadata is requested, then normalize away Arachne placement and curls.
            // Speed splitting can omit shallow pockets that do not change feedrate. Apply the same
            // interior sampling as fixed-speed metadata without adding or moving extrusion points.
            const float overhang_percentage = estimate_overhang_metadata ?
                segment_overhang_percentage(curr.position, next.position, path.width, is_perimeter(path.role())) : 0.0f;
            processed_points.push_back({Point3(scaled(curr.position)), extrusion_speed, overlap, overhang_percentage});
        }
        return processed_points;
    }
};

} // namespace Slic3r

#endif // slic3r_ExtrusionProcessor_hpp_
