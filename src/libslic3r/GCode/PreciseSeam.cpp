// Precise Seam implementation. Design: docs/HLSD/precise-seam.md
#include "PreciseSeam.hpp"
#include "PreciseSeamInternal.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Model.hpp"
#include "SeamPlacer.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <boost/log/trivial.hpp>
#include <optional>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <cassert>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <tbb/parallel_for.h>
#include <vector>
#include <utility>

namespace Slic3r {
namespace PreciseSeam {

ModifierRegions prepare_modifier_regions(ExPolygons regions)
{
    ModifierRegions cached;
    cached.reserve(regions.size());
    // Each exterior is measured once; its holes remain in the same cached region.
    for (ExPolygon &region : regions)
        cached.emplace_back(std::move(region));
    return cached;
}

ModifierSlices prepare_modifier_slices(std::vector<ExPolygons> slices)
{
    ModifierSlices cached;
    cached.reserve(slices.size());
    // Keep empty layers so callers can index by the original object layer number.
    for (ExPolygons &layer : slices)
        cached.push_back(prepare_modifier_regions(std::move(layer)));
    return cached;
}

PreparedPerimeter::PreparedPerimeter(const Polygon &perimeter, coord_t surface_shift, bool is_hole)
    : polygon(perimeter), shift(surface_shift), hole(is_hole)
{
    const size_t count = polygon.size();
    if (count < 3)
        return;
    for (size_t edge = 0; edge < count; ++edge)
        if (polygon.points[edge] == polygon.points[(edge + 1) % count])
            return;
    bounds = BoundingBox(polygon.points);
    // Close an open polyline explicitly; prepare its storage once for all modifier queries.
    line.points.reserve(count + 1);
    line.points.insert(line.points.end(), polygon.points.begin(), polygon.points.end());
    line.points.push_back(polygon.points.front());
    valid = true;
}

PreparedPerimeter::~PreparedPerimeter() = default;

void PreparedPerimeter::set_target(const Polygon &noisy, coord_t noise_window)
{
    target = &noisy;
    window = noise_window;
    const size_t count = noisy.size();
    bool usable = count >= 3;
    for (size_t edge = 0; usable && edge < count; ++edge)
        usable = noisy.points[edge] != noisy.points[(edge + 1) % count];
    valid = valid && usable;
}

// Import EnforcedBlockedSeamPoint from SeamPlacerImpl namespace for convenience
using SeamPlacerImpl::EnforcedBlockedSeamPoint;

// Squared on-edge tolerance in coordinate units: Clipper truncates cuts to whole units, under 1 unit
// per axis, so under sqrt(2) from the edge (squared < 2); 2.5 keeps a margin. Independent of SCALING_FACTOR.
static constexpr double MACHINE_PRECISION_SQUARED = 2.5;

// Snapping radius for point insertion, shared by every rule that must match insertion. Deliberately in
// scaled units (1 um by default, 10 um on large beds), well above single-precision candidate steps.
static constexpr coord_t TOLERANCE_LINEAR = 1000;
static constexpr coord_t TOLERANCE_SQUARED = TOLERANCE_LINEAR * TOLERANCE_LINEAR;

// The surface is reconstructed this much inside the model surface, so a modifier face placed flush against
// a model face stays clear of it on every layer instead of touching it by rounding noise.
static constexpr coord_t SURFACE_MARGIN = 5 * TOLERANCE_LINEAR;
// Maximum sagitta of the chords that approximate the surface arcs around convex vertices, in units.
static constexpr double SURFACE_ARC_SAGITTA = 40.;

coord_t surface_shift(double line_width, double compensation)
{
    const double shift = scale_(0.5 * line_width - compensation) - double(SURFACE_MARGIN);
    // A shift within the snapping radius cannot change what insertion produces: keep the centerline path.
    return shift > double(TOLERANCE_LINEAR) ? coord_t(std::lround(shift)) : 0;
}

// Spacing of the measurements along a long edge: the step at which elephant foot compensation is resampled.
static constexpr double SHIFT_SAMPLING_STEP = 0.5;

ShiftProfile measure_shift_profile(const Polygon &perimeter, bool hole, const ShiftMeasurement &measurement)
{
    const double nominal = scale_(0.5 * measurement.line_width - measurement.compensation) - double(SURFACE_MARGIN);
    const double correction = scale_(measurement.outline_compensation) + double(SURFACE_MARGIN);
    const double min_depth = scale_(measurement.min_depth);
    const double max_depth = scale_(measurement.max_depth);
    // Normals towards air, as the surface line takes them.
    const size_t count = perimeter.size();
    const double side = (hole ? -1. : 1.) * (perimeter.is_counter_clockwise() ? 1. : -1.);
    std::vector<Vec2d> normals(count);
    for (size_t i = 0; i < count; ++i) {
        const Vec2d direction = (perimeter[(i + 1) % count] - perimeter[i]).cast<double>();
        normals[i] = side * Vec2d(direction.y(), -direction.x()) / direction.norm();
    }
    // The depth along the normals: the distance to the nearest outline point would be the corner's at an inner
    // corner, deeper than either side. A point not inside the material (signed distance not negative) is not measured.
    const auto shift_at = [&](const Point &point, const Vec2d &before, const Vec2d &after) {
        const auto [distance, line, nearest] = measurement.outline.distance_from_lines_extra<true>(point);
        const Vec2d offset = nearest - point.cast<double>();
        const double depth = distance < 0. ? std::max(offset.dot(before), offset.dot(after)) : -1.;
        const double shift = depth >= min_depth && depth <= max_depth ? depth - correction : nominal;
        return coord_t(std::lround(std::max(0., shift)));
    };
    const coord_t nominal_shift = coord_t(std::lround(std::max(0., nominal)));
    const auto same = [](double a, double b) { return std::abs(a - b) <= double(TOLERANCE_LINEAR); };

    ShiftProfile profile;
    profile.vertices.reserve(count);
    bool nominal_everywhere = true;
    for (size_t i = 0; i < count; ++i) {
        profile.vertices.push_back(shift_at(perimeter[i], normals[(i + count - 1) % count], normals[i]));
        nominal_everywhere = nominal_everywhere && same(double(profile.vertices.back()), double(nominal_shift));
    }
    // A long edge is measured in between as well. If every value there follows the line between its vertices, the
    // edge stays one part; otherwise all of them split it.
    const double step = scale_(SHIFT_SAMPLING_STEP);
    profile.interior.resize(count);
    bool split = false;
    for (size_t i = 0; i < count; ++i) {
        const size_t next = (i + 1) % count;
        const Vec2d a = perimeter[i].cast<double>();
        const Vec2d edge = perimeter[next].cast<double>() - a;
        const size_t intervals = size_t(std::ceil(edge.norm() / step));
        bool linear = true;
        std::vector<std::pair<double, coord_t>> samples;
        for (size_t k = 1; k < intervals; ++k) {
            const double t = double(k) / double(intervals);
            const Vec2d at = a + t * edge;
            const coord_t value = shift_at(Point(coord_t(std::lround(at.x())), coord_t(std::lround(at.y()))), normals[i], normals[i]);
            linear = linear && same(double(value), (1. - t) * double(profile.vertices[i]) + t * double(profile.vertices[next]));
            samples.emplace_back(t, value);
        }
        if (!linear) {
            profile.interior[i] = std::move(samples);
            split = true;
        }
    }
    if (nominal_everywhere && !split)
        return {};
    if (!split)
        profile.interior.clear();
    return profile;
}

PreparedPerimeter::PreparedPerimeter(const Polygon &perimeter, ShiftProfile shift_profile, bool is_hole)
    : PreparedPerimeter(perimeter, coord_t(0), is_hole)
{
    coord_t largest = 0;
    for (const coord_t value : shift_profile.vertices)
        largest = std::max(largest, value);
    for (const auto &edge : shift_profile.interior)
        for (const auto &sample : edge)
            largest = std::max(largest, sample.second);
    // Same snapping as surface_shift(): a surface this close cannot change what insertion produces.
    if (largest <= TOLERANCE_LINEAR)
        return;
    shift = largest;
    profile = std::move(shift_profile);
}

namespace detail {

// Use the existing clipping tolerance only to recover rounded coordinates, not to
// bridge gaps between intervals: even a small uncovered interval must remain a gap.
static std::optional<double> parameter_on_edge(const Point &point, const Point &a, const Point &b)
{
    // Original vertices need no floating-point projection.
    if (point == a) return 0.;
    if (point == b) return 1.;
    const Vec2d direction = b.cast<double>() - a.cast<double>();
    const Vec2d offset = point.cast<double>() - a.cast<double>();
    const double squared_length = direction.squaredNorm();
    if (squared_length == 0.)
        return std::nullopt;
    const double parameter = std::clamp(offset.dot(direction) / squared_length, 0., 1.);
    if ((offset - parameter * direction).squaredNorm() > MACHINE_PRECISION_SQUARED)
        return std::nullopt;
    return parameter;
}

static std::optional<ClippedEdgeInterval> interval_on_edge(
    const Point &first, const Point &last, size_t edge, const Polygon &perimeter)
{
    const Point &a = perimeter.points[edge];
    const Point &b = perimeter.points[(edge + 1) % perimeter.size()];
    const auto t0 = parameter_on_edge(first, a, b);
    // Reject this edge before projecting the second point when the first is outside.
    if (!t0)
        return std::nullopt;
    const auto t1 = parameter_on_edge(last, a, b);
    if (!t1 || *t0 == *t1)
        return std::nullopt;
    ClippedEdgeInterval interval{edge, *t0, *t1, first, last};
    if (interval.begin > interval.end) {
        std::swap(interval.begin, interval.end);
        std::swap(interval.first, interval.last);
    }
    // Canonical endpoints make adjacent original edges join at their shared vertex.
    if (interval.begin == 0.) interval.first = a;
    if (interval.end == 1.) interval.last = b;
    return interval;
}

// Interior clipping vertices normally retain the exact source coordinates. Only
// the two cut endpoints need projection; a failed sequence tries the next anchor.
static bool append_exact_fragment(const Polyline &fragment, const Polygon &perimeter,
                                  std::vector<ClippedEdgeInterval> &intervals)
{
    const size_t size = fragment.size();
    const size_t count = perimeter.size();
    assert(size >= 3); // bind_fragment() sends only fragments with interior points here.
    // A longer sequence cannot be one exact pass over this contour.
    if (size > count + 2)
        return false;
    for (size_t anchor = 0; anchor < count; ++anchor) {
        if (fragment.points[1] != perimeter.points[anchor])
            continue;
        // Clipping may return either direction, independent of contour winding.
        for (bool forward : {true, false}) {
            const auto next = [count, forward](size_t index) {
                return forward ? (index + 1) % count : (index + count - 1) % count;
            };
            size_t last = anchor;
            bool matches = true;
            for (size_t i = 2; i + 1 < size; ++i) {
                last = next(last);
                if (fragment.points[i] != perimeter.points[last]) {
                    matches = false;
                    break;
                }
            }
            if (!matches)
                continue;
            const size_t first_edge = forward ? (anchor + count - 1) % count : anchor;
            const size_t last_edge = forward ? last : (last + count - 1) % count;
            const auto first = interval_on_edge(fragment.points.front(), fragment.points[1], first_edge, perimeter);
            const auto end = interval_on_edge(fragment.points[size - 2], fragment.points.back(), last_edge, perimeter);
            if (!first || !end)
                continue;
            // Commit only a complete match, so rejected anchors leave no intervals.
            intervals.push_back(*first);
            size_t vertex = anchor;
            for (size_t i = 2; i + 1 < size; ++i) {
                const size_t adjacent = next(vertex);
                const size_t edge = forward ? vertex : adjacent;
                intervals.push_back({edge, 0., 1., perimeter.points[edge], perimeter.points[(edge + 1) % count]});
                vertex = adjacent;
            }
            intervals.push_back(*end);
            return true;
        }
    }
    return false;
}

// Find the first edge once, then follow the contour without restarting a global search.
bool append_projected_fragment(const Polyline &fragment, const Polygon &perimeter,
                               std::vector<ClippedEdgeInterval> &intervals,
                               FragmentBindingFailure &failure)
{
    const size_t original_size = intervals.size();
    bool forward = true;
    const size_t count = perimeter.size();
    for (size_t i = 1; i < fragment.size(); ++i) {
        if (fragment.points[i - 1] == fragment.points[i])
            continue;
        failure.pair_index = i - 1;
        std::optional<ClippedEdgeInterval> matched;
        const auto is_forward = [&](const ClippedEdgeInterval &candidate) {
            const Vec2d movement = fragment.points[i].cast<double>() - fragment.points[i - 1].cast<double>();
            const Vec2d edge = perimeter.points[(candidate.edge + 1) % count].cast<double>() -
                               perimeter.points[candidate.edge].cast<double>();
            return movement.dot(edge) > 0.;
        };
        if (intervals.size() == original_size) {
            failure.reason = "initial edge not found";
            for (size_t edge = 0; edge < count; ++edge) {
                matched = interval_on_edge(fragment.points[i - 1], fragment.points[i], edge, perimeter);
                if (matched) {
                    // The first suitable edge defines orientation; overlapping visits are unsupported.
                    forward = is_forward(*matched);
                    break;
                }
            }
        } else {
            failure.reason = "non-continuous binding";
            const ClippedEdgeInterval &previous = intervals.back();
            const double end = forward ? previous.end : previous.begin;
            matched = interval_on_edge(fragment.points[i - 1], fragment.points[i], previous.edge, perimeter);
            if (matched && is_forward(*matched) != forward)
                matched.reset();
            // The shared point is the previous pair's last point on the same edge, so continuity holds by
            // construction. Reuse its parameter rather than comparing a recomputed one: the two projections
            // may be compiled differently (e.g. FMA contraction) and differ by an ulp on some platforms.
            if (matched)
                (forward ? matched->begin : matched->end) = end;
            // Crossing to the neighbor is allowed only at their actual shared vertex.
            const size_t vertex = forward ? (previous.edge + 1) % count : previous.edge;
            if (!matched && end == (forward ? 1. : 0.) && fragment.points[i - 1] == perimeter.points[vertex]) {
                const size_t edge = forward ? vertex : (previous.edge + count - 1) % count;
                matched = interval_on_edge(fragment.points[i - 1], fragment.points[i], edge, perimeter);
                if (matched && is_forward(*matched) != forward)
                    matched.reset();
                // The shared point is exactly the neighbor's start (forward) or end (backward) vertex,
                // so parameter_on_edge() returns exactly 0 or 1 for it.
                assert(!matched || (forward ? matched->begin == 0. : matched->end == 1.));
            }
        }
        if (!matched) {
            // Never leave a partial fragment or discard earlier successful fragments.
            intervals.resize(original_size);
            return false;
        }
        intervals.push_back(*matched);
    }
    return intervals.size() > original_size;
}

// Exact binding first, projection as the general path; both leave intervals unchanged on failure.
bool bind_fragment(const Polyline &fragment, const Polygon &perimeter,
                   std::vector<ClippedEdgeInterval> &intervals, FragmentBindingFailure &failure)
{
    if (fragment.size() > 2 && append_exact_fragment(fragment, perimeter, intervals))
        return true;
    return append_projected_fragment(fragment, perimeter, intervals, failure);
}

// Rare-case repair, run only after binding has failed: Clipper may place an end cut a unit or two beside
// a vertex, so it lies on neither edge. Move such an end onto that vertex, taken from the fragment's own
// chain, and let the caller retry with the same strict rules, so a wrong move can only fail again.
// Returns false when nothing was changed.
static bool snap_cuts_to_adjacent_vertices(Polyline &fragment, const Polygon &perimeter)
{
    const size_t count = perimeter.size();
    // Strict comparison matches the vertex snapping in insert_point_into_perimeter.
    const auto close = [](const Point &a, const Point &b) {
        return (a - b).cast<double>().squaredNorm() < double(TOLERANCE_SQUARED);
    };
    const auto snap_target = [&](const Point &cut, const Point &neighbour) -> std::optional<Point> {
        if (cut == neighbour)
            return std::nullopt;
        bool neighbour_is_vertex = false;
        bool ambiguous = false;
        std::optional<Point> target;
        // Scan the whole perimeter before deciding: a cut that is a real vertex must be detected anywhere,
        // and an ambiguity must not pre-empt the neighbour rule below.
        for (size_t j = 0; j < count; ++j) {
            if (perimeter[j] == cut)
                return std::nullopt; // A real vertex, not a rounded cut.
            if (perimeter[j] != neighbour)
                continue;
            neighbour_is_vertex = true;
            for (const Point &candidate : {perimeter[(j + count - 1) % count], perimeter[(j + 1) % count]}) {
                if (!close(cut, candidate))
                    continue;
                if (target && *target != candidate)
                    ambiguous = true;
                target = candidate;
            }
        }
        // A cut between two arbitrary points of one edge is not this rounding case.
        if (!neighbour_is_vertex)
            return std::nullopt;
        // The neighbour itself takes precedence, even when both of its chain vertices are close too
        // (edges shorter than the radius on both sides): the cut is then just a rounded copy of it.
        if (close(cut, neighbour))
            return neighbour;
        // Two different edge-start candidates: keep the failure rather than guess.
        if (ambiguous)
            return std::nullopt;
        return target;
    };
    bool changed = false;
    if (fragment.size() >= 2)
        if (const auto target = snap_target(fragment.points.front(), fragment.points[1])) {
            if (*target == fragment.points[1])
                fragment.points.erase(fragment.points.begin());
            else
                fragment.points.front() = *target;
            changed = true;
        }
    if (fragment.size() >= 2)
        if (const auto target = snap_target(fragment.points.back(), fragment.points[fragment.size() - 2])) {
            if (*target == fragment.points[fragment.size() - 2])
                fragment.points.pop_back();
            else
                fragment.points.back() = *target;
            changed = true;
        }
    return changed;
}

// Location fields shared by the failure and recovery markers, enough to find the layer in a saved project.
static std::string fragment_location(const ExtractionContext &context, size_t fragment_index)
{
    const Layer *layer = context.layer;
    const ModelObject *object = layer && layer->object() ? layer->object()->model_object() : nullptr;
    return " object=" + std::to_string(object ? object->id().id : 0) +
           " modifier=" + std::to_string(context.modifier ? context.modifier->id().id : 0) +
           " layer=" + (layer ? std::to_string(layer->id()) : std::string("unknown")) +
           " z=" + (layer ? std::to_string(layer->slice_z) : std::string("unknown")) +
           " fragment=" + std::to_string(fragment_index);
}

bool append_fragment(const Polyline &fragment, const Polygon &perimeter,
                     std::vector<ClippedEdgeInterval> &intervals,
                     const ExtractionContext &context, size_t fragment_index)
{
    FragmentBindingFailure failure;
    if (bind_fragment(fragment, perimeter, intervals, failure))
        return true;
    // Rare-case fallback: repair the end cuts and retry once. A recovery is logged but is not a failure
    // (no failure count, no user warning).
    const char *outcome = nullptr;
    Polyline cleaned = fragment;
    if (snap_cuts_to_adjacent_vertices(cleaned, perimeter)) {
        FragmentBindingFailure retry_failure;
        // Only a contact shorter than the snapping distance may remain: nothing to bind, nothing lost.
        if (cleaned.size() < 2)
            outcome = "contact";
        else if (bind_fragment(cleaned, perimeter, intervals, retry_failure))
            outcome = "bound";
    }
    // A failed fragment shorter than the snapping radius is a contact: insertion would collapse it to one
    // point anyway, so accept it instead of reporting a failure.
    if (outcome == nullptr && fragment.length() < double(TOLERANCE_LINEAR))
        outcome = "contact";
    if (outcome != nullptr) {
        // Same log budget as failures, counted separately; SeamPlacer::init() reports the total.
        if (context.warnings == nullptr ||
            context.warnings->recovered_fragments.fetch_add(1, std::memory_order_relaxed) < failed_fragment_log_limit)
            BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamFragmentRecovered] Rare case resolved after a binding failure"
                << " outcome=" << outcome << fragment_location(context, fragment_index)
                << " original_pair=" << failure.pair_index << " original_reason=" << failure.reason
                << " fragment_points=" << fragment.size() << " cleaned_points=" << cleaned.size()
                << " perimeter_points=" << perimeter.size();
        return true;
    }
    // Reserve a log slot atomically before formatting; callers without shared state log every failure.
    if (context.warnings &&
        context.warnings->failed_fragments.fetch_add(1, std::memory_order_relaxed) >= failed_fragment_log_limit)
        return false;
    // Keep a small marker for investigating a saved project, not a full geometry dump.
    BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamIntersectionFailed] Unable to process intersection"
        << fragment_location(context, fragment_index) << " pair=" << failure.pair_index
        << " reason=" << failure.reason << " fragment_points=" << fragment.size()
        << " perimeter_points=" << perimeter.size();
    return false;
}

void cover_centerline(const Polygon &perimeter, CenterlinePoint from, const CenterlinePoint &to,
                      std::vector<ClippedEdgeInterval> &intervals)
{
    const size_t count = perimeter.size();
    if (from.edge == to.edge && from.parameter > to.parameter)
        return;
    for (size_t steps = 0; steps <= count; ++steps) {
        if (from.edge == to.edge && from.parameter <= to.parameter) {
            if (from.parameter < to.parameter)
                intervals.push_back({from.edge, from.parameter, to.parameter, from.point, to.point});
            return;
        }
        const size_t next = (from.edge + 1) % count;
        if (from.parameter < 1.)
            intervals.push_back({from.edge, from.parameter, 1., from.point, perimeter[next]});
        from = {next, 0., perimeter[next]};
    }
}

SurfaceLine build_surface_line(const Polygon &perimeter, coord_t shift, bool hole)
{
    ShiftProfile profile;
    profile.vertices.assign(perimeter.size(), shift);
    return build_surface_line(perimeter, profile, hole);
}

SurfaceLine build_surface_line(const Polygon &perimeter, const ShiftProfile &profile, bool hole)
{
    SurfaceLine surface;
    const size_t count = perimeter.size();
    assert(profile.vertices.size() == count);
    assert(profile.interior.empty() || profile.interior.size() == count);
    // Air lies to the right of a counter-clockwise contour and to the left of a counter-clockwise hole;
    // a clockwise input swaps the sides.
    const bool ccw = perimeter.is_counter_clockwise();
    const double side = (hole ? -1. : 1.) * (ccw ? 1. : -1.);
    const auto round_point = [](const Vec2d &p) { return Point(coord_t(std::lround(p.x())), coord_t(std::lround(p.y()))); };
    const auto cross = [](const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); };
    std::vector<Vec2d> normals(count);
    for (size_t i = 0; i < count; ++i) {
        const Vec2d direction = (perimeter[(i + 1) % count] - perimeter[i]).cast<double>();
        normals[i] = side * Vec2d(direction.y(), -direction.x()) / direction.norm();
    }

    // Every shifted edge is a chain of straight parts between the points where its shift is given: from the edge's
    // start vertex through the interior samples to its end vertex, each point moved along the edge's normal.
    // A part maps linearly onto its range of parameters on the source edge.
    struct Part {
        double begin;
        double end;
        Point a;
        Point b;
    };
    std::vector<Part> parts;
    std::vector<size_t> first_part(count + 1);
    surface.shifts.assign(count, Point(0, 0));
    surface.translated.assign(count, false);
    for (size_t i = 0; i < count; ++i) {
        const size_t next = (i + 1) % count;
        first_part[i] = parts.size();
        const bool interior = !profile.interior.empty() && !profile.interior[i].empty();
        Point from = perimeter[i] + round_point(double(profile.vertices[i]) * normals[i]);
        double begin = 0.;
        if (interior)
            for (const auto &[parameter, value] : profile.interior[i]) {
                const Vec2d at = perimeter[i].cast<double>() + parameter * (perimeter[next] - perimeter[i]).cast<double>();
                const Point to = round_point(at + double(value) * normals[i]);
                parts.push_back({begin, parameter, from, to});
                from = to;
                begin = parameter;
            }
        parts.push_back({begin, 1., from, perimeter[next] + round_point(double(profile.vertices[next]) * normals[i])});
        // One rounded shift for a constant edge keeps its shifted edge an exact translate of the source edge.
        if (!interior && profile.vertices[i] == profile.vertices[next]) {
            surface.shifts[i] = round_point(double(profile.vertices[i]) * normals[i]);
            surface.translated[i] = true;
        }
    }
    first_part[count] = parts.size();
    const auto last_part = [&first_part](size_t edge) { return first_part[edge + 1] - 1; };
    const auto direction = [&parts](size_t part) -> Vec2d { return (parts[part].b - parts[part].a).cast<double>(); };

    // A concave corner whose shifted edges cross within both of the parts meeting there is resolved exactly: the
    // shifted edges end at the crossing K, with no tails and no bridge, and the dead stretch around the vertex is
    // known at once. The trims of both corners of one part must leave a piece of it, so a fillet finer than the
    // shift keeps its tails and bridges. A candidate counts against its neighbours even when it is not resolved
    // itself.
    std::vector<double> end_at(count, 1.);   // Per vertex: fraction of the part before it where that part ends.
    std::vector<double> start_at(count, 0.); // Per vertex: fraction of the part after it where that part starts.
    std::vector<bool> candidate(count, false);
    std::vector<Vec2d> crossing(count);
    for (size_t v = 0; v < count; ++v) {
        const size_t before = (v + count - 1) % count;
        const Vec2d into = direction(last_part(before));
        const Vec2d out = direction(first_part[v]);
        const double turn = cross(normals[before], normals[v]);
        const double denominator = cross(into, out);
        if (side * turn >= 0. || denominator == 0.)
            continue; // Convex, collinear or a reversal.
        const Vec2d a = parts[last_part(before)].a.cast<double>();
        const Vec2d offset = parts[first_part[v]].a.cast<double>() - a;
        end_at[v] = cross(offset, out) / denominator;
        start_at[v] = cross(offset, into) / denominator;
        crossing[v] = a + end_at[v] * into;
        candidate[v] = end_at[v] > 0. && end_at[v] < 1. && start_at[v] > 0. && start_at[v] < 1.;
    }
    // A trimmed part keeps at least this many units, so its rounded ends stay distinct. The far corner trims the
    // same part only when the edge is a single part.
    const double min_part = 4.;
    const auto single = [&](size_t edge) { return first_part[edge] == last_part(edge); };
    std::vector<bool> resolved(count, false);
    for (size_t v = 0; v < count; ++v) {
        const size_t before = (v + count - 1) % count;
        const size_t after = (v + 1) % count;
        resolved[v] = candidate[v] &&
            (end_at[v] - (single(before) && candidate[before] ? start_at[before] : 0.)) *
                direction(last_part(before)).norm() > min_part &&
            ((single(v) && candidate[after] ? end_at[after] : 1.) - start_at[v]) *
                direction(first_part[v]).norm() > min_part;
    }

    Points &points = surface.polygon.points;
    std::vector<size_t> first_element(count, size_t(-1));
    std::vector<size_t> last_element(count, size_t(-1));
    // A point equal to the previous one ends a zero-length element: the new element replaces it.
    const auto push = [&](const Point &point, SurfaceElement element) {
        if (!points.empty() && points.back() == point) {
            surface.elements.back() = element;
            return;
        }
        points.push_back(point);
        surface.elements.push_back(element);
    };
    for (size_t i = 0; i < count; ++i) {
        const size_t next = (i + 1) % count;
        const Point &vertex = perimeter[next];
        for (size_t k = first_part[i]; k <= last_part(i); ++k) {
            const Part &part = parts[k];
            const bool first = k == first_part[i];
            // Fractions of the part kept between the crossings of resolved corners.
            const double from = first && resolved[i] ? start_at[i] : 0.;
            const double to = k == last_part(i) && resolved[next] ? end_at[next] : 1.;
            push(first && resolved[i] ? round_point(crossing[i]) : part.a,
                 {i, false, part.begin + from * (part.end - part.begin), part.begin + to * (part.end - part.begin)});
            if (first)
                first_element[i] = surface.elements.size() - 1;
        }
        last_element[i] = surface.elements.size() - 1;
        if (resolved[next])
            continue; // The next edge starts at the crossing that ends this one.
        const Point end = parts[last_part(i)].b;
        const Point resume = parts[first_part[next]].a;
        if (end == resume)
            continue; // Collinear edges or no shift at the vertex: the shifted edges meet exactly.
        push(end, {next, true});
        // The normal turns like the edge; a reversal turns by half a circle around the air side.
        const double turn = cross(normals[i], normals[next]);
        const double dot = normals[i].dot(normals[next]);
        const double sweep = turn == 0. && dot < 0. ? side * PI : std::atan2(turn, dot);
        // Concave towards air and not resolved: a straight bridge, carried onto the vertex like an arc.
        if (side * sweep <= 0.)
            continue;
        // An arc of the vertex's shift; both edges take that shift at the vertex, so the line stays continuous.
        const double radius = double(profile.vertices[next]);
        // Chord angle of an arc of this radius whose sagitta stays within the limit.
        const double max_step = 2. * std::acos(std::max(-1., 1. - SURFACE_ARC_SAGITTA / radius));
        const double start = std::atan2(normals[i].y(), normals[i].x());
        const size_t steps = size_t(std::ceil(std::abs(sweep) / max_step));
        for (size_t step = 1; step < steps; ++step) {
            const double angle = start + sweep * double(step) / double(steps);
            push(round_point(vertex.cast<double>() + radius * Vec2d(std::cos(angle), std::sin(angle))), {next, true});
        }
    }
    if (points.size() > 1 && points.back() == points.front()) {
        points.pop_back();
        surface.elements.pop_back();
    }
    if (points.size() < 3)
        return surface;
    // Resolved corners: the crossing ends the element of the edge before and starts the one after.
    for (size_t v = 0; v < count; ++v) {
        const size_t before = (v + count - 1) % count;
        if (resolved[v] && last_element[before] < points.size() && first_element[v] < points.size()) {
            const Point at = points[first_element[v]];
            surface.gaps.push_back({last_element[before], at, first_element[v], at});
        }
    }

    // Clipped as an open path closed at its start, like the centerline.
    surface.line.points = points;
    surface.line.points.push_back(points.front());
    // Point by point: a box built from an axis-aligned line counts as undefined.
    for (const Point &point : points)
        surface.bounds.merge(point);
    return surface;
}

Transfer transfer_point(const AABBTreeLines::LinesDistancer<Line> &target, const Point &point, const Point &surface,
                        double window)
{
    const std::vector<Line> &lines = target.get_lines();
    const size_t count = lines.size();
    // A position on the noisy loop; a vertex is represented by its outgoing edge.
    const auto position = [&lines, count](size_t index, double parameter, const Vec2d &at) -> CenterlinePoint {
        if (parameter >= 1.)
            return {(index + 1) % count, 0., lines[index].b};
        if (parameter <= 0.)
            return {index, 0., lines[index].a};
        return {index, parameter, Point(coord_t(std::lround(at.x())), coord_t(std::lround(at.y())))};
    };
    const Vec2d from = point.cast<double>();
    const Vec2d pair = (surface - point).cast<double>();
    if (pair.squaredNorm() >= double(TOLERANCE_SQUARED)) {
        // The line through the pair; a crossing within the window lies on an edge within the window of the point.
        const Vec2d direction = pair.normalized();
        std::optional<CenterlinePoint> crossing;
        double nearest = window;
        for (const size_t index : target.all_lines_in_radius(point, window)) {
            const Vec2d a = lines[index].a.cast<double>();
            const Vec2d ab = lines[index].b.cast<double>() - a;
            const Vec2d to_a = a - from;
            const double denominator = cross2(direction, ab);
            double parameter = 0.;
            double along = 0.;
            if (std::abs(denominator) <= EPSILON * ab.norm()) {
                // An edge along the line gives its point nearest to the point; one beside the line gives nothing.
                if (std::abs(cross2(direction, to_a)) > 1.)
                    continue;
                parameter = std::clamp(-to_a.dot(ab) / ab.squaredNorm(), 0., 1.);
                along = std::abs((to_a + parameter * ab).dot(direction));
            } else {
                parameter = cross2(to_a, direction) / denominator;
                if (parameter < 0. || parameter > 1.)
                    continue;
                along = std::abs(cross2(to_a, ab) / denominator);
            }
            // The nearest crossing; of equally near ones the first found, in the deterministic order of the tree.
            if (along > window || (crossing && along >= nearest))
                continue;
            nearest = along;
            crossing = position(index, parameter, a + parameter * ab);
        }
        if (crossing)
            return {TransferOutcome::Exact, *crossing};
    }
    const auto [distance, index, nearest] = target.distance_from_lines_extra<false>(point);
    if (index >= count || distance > window)
        return {TransferOutcome::Failed, {0, 0., point}};
    const Vec2d a = lines[index].a.cast<double>();
    const Vec2d ab = lines[index].b.cast<double>() - a;
    return {TransferOutcome::Approximate, position(index, std::clamp((nearest - a).dot(ab) / ab.squaredNorm(), 0., 1.), nearest)};
}
} // namespace detail

const detail::SurfaceLine &PreparedPerimeter::surface() const
{
    assert(valid && shift > 0);
    if (!m_surface)
        m_surface = std::make_unique<detail::SurfaceLine>(profile.empty() ? detail::build_surface_line(polygon, shift, hole) :
                                                                            detail::build_surface_line(polygon, profile, hole));
    return *m_surface;
}

const AABBTreeLines::LinesDistancer<Line> &PreparedPerimeter::target_lines() const
{
    assert(valid && target != nullptr);
    if (!m_target_lines)
        m_target_lines = std::make_unique<AABBTreeLines::LinesDistancer<Line>>(target->lines());
    return *m_target_lines;
}

// The edges of the surface polygon that make the arc or bridge of a vertex, in order; empty for a vertex without them.
static std::vector<size_t> vertex_chain(const detail::SurfaceLine &surface, size_t vertex)
{
    const size_t count = surface.elements.size();
    const auto of_vertex = [&surface, vertex](size_t i) { return surface.elements[i].vertex && surface.elements[i].source == vertex; };
    size_t first = 0;
    while (first < count && !of_vertex(first))
        ++first;
    std::vector<size_t> chain;
    if (first == count)
        return chain;
    // The run may wrap round the start of the polygon.
    for (size_t k = 0; k < count && of_vertex((first + count - 1) % count); ++k)
        first = (first + count - 1) % count;
    for (size_t k = 0, i = first; k < count && of_vertex(i); ++k, i = (i + 1) % count)
        chain.push_back(i);
    return chain;
}

// Arc length along a chain of surface polygon edges at the foot of a point, and the point at an arc length.
static double chain_position(const detail::SurfaceLine &surface, const std::vector<size_t> &chain, const Point &point)
{
    double best = std::numeric_limits<double>::max();
    double result = 0.;
    double start = 0.;
    for (const size_t i : chain) {
        const Vec2d a = surface.polygon[i].cast<double>();
        const Vec2d ab = surface.polygon[(i + 1) % surface.polygon.size()].cast<double>() - a;
        const double t = std::clamp((point.cast<double>() - a).dot(ab) / ab.squaredNorm(), 0., 1.);
        const double distance = (a + t * ab - point.cast<double>()).squaredNorm();
        if (distance < best) {
            best = distance;
            result = start + t * ab.norm();
        }
        start += ab.norm();
    }
    return result;
}

static Point chain_point(const detail::SurfaceLine &surface, const std::vector<size_t> &chain, double position)
{
    Vec2d at = surface.polygon[chain.front()].cast<double>();
    for (const size_t i : chain) {
        const Vec2d a = surface.polygon[i].cast<double>();
        const Vec2d ab = surface.polygon[(i + 1) % surface.polygon.size()].cast<double>() - a;
        const double length = ab.norm();
        at = a + ab * std::clamp(position / length, 0., 1.);
        if (position <= length)
            break;
        position -= length;
    }
    return Point(coord_t(std::lround(at.x())), coord_t(std::lround(at.y())));
}

static double chain_length(const detail::SurfaceLine &surface, const std::vector<size_t> &chain)
{
    double length = 0.;
    for (const size_t i : chain)
        length += (surface.polygon[(i + 1) % surface.polygon.size()] - surface.polygon[i]).cast<double>().norm();
    return length;
}

// The crossing of the shifted edges at a resolved concave vertex, where its dead stretch begins and ends.
static std::optional<Point> concave_crossing(const detail::SurfaceLine &surface, size_t vertex)
{
    for (const detail::SurfaceGap &gap : surface.gaps)
        if (!surface.elements[gap.start_element].vertex && surface.elements[gap.start_element].source == vertex)
            return gap.start;
    return std::nullopt;
}

// Where the shifted edge leaving a vertex starts: the surface point of a vertex without an arc or bridge (collinear
// edges or no shift there), with the shift profile at the vertex. Empty when that edge has no element left.
static std::optional<Point> edge_start(const detail::SurfaceLine &surface, size_t vertex)
{
    std::optional<size_t> first;
    for (size_t i = 0; i < surface.elements.size(); ++i)
        if (!surface.elements[i].vertex && surface.elements[i].source == vertex &&
            (!first || surface.elements[i].begin < surface.elements[*first].begin))
            first = i;
    if (!first)
        return std::nullopt;
    return surface.polygon[*first];
}

// The surface point paired with a Center point of the smooth centerline: on a shifted edge, the point of that edge at
// the same parameter; at a vertex, the middle of its arc or bridge (of the piece covered, for a zero-length segment),
// or where its shifted edges meet when it has neither; in the dead stretch of a concave corner, the crossing that
// makes it.
static Point center_surface_point(const PreparedPerimeter &prepared, const PerimeterSegment &segment, const Point &point,
                                  size_t edge)
{
    if (prepared.shift == 0)
        return point;
    const detail::SurfaceLine &surface = prepared.surface();
    const Polygon &perimeter = prepared.polygon;
    const size_t count = perimeter.size();
    std::optional<size_t> vertex;
    if (segment.edge_indices.empty())
        vertex = segment.begin.edge_index;
    else if (point == perimeter[edge])
        vertex = edge;
    else if (point == perimeter[(edge + 1) % count])
        vertex = (edge + 1) % count;
    if (vertex) {
        const std::vector<size_t> chain = vertex_chain(surface, *vertex);
        if (chain.empty()) {
            if (const std::optional<Point> crossing = concave_crossing(surface, *vertex))
                return *crossing;
            // The shifted edges meet at one point, the vertex itself where the shift there is zero. Without an element
            // of the edge the point itself is kept, an emergency fallback.
            return edge_start(surface, *vertex).value_or(point);
        }
        double from = 0.;
        double to = chain_length(surface, chain);
        if (segment.edge_indices.empty()) {
            from = chain_position(surface, chain, segment.surface_first);
            to = chain_position(surface, chain, segment.surface_last);
        }
        return chain_point(surface, chain, 0.5 * (from + to));
    }
    const Vec2d a = perimeter[edge].cast<double>();
    const Vec2d ab = perimeter[(edge + 1) % count].cast<double>() - a;
    const double t = std::clamp((point.cast<double>() - a).dot(ab) / ab.squaredNorm(), 0., 1.);
    double highest = 0.;
    for (size_t i = 0; i < surface.elements.size(); ++i) {
        const detail::SurfaceElement &element = surface.elements[i];
        if (element.vertex || element.source != edge)
            continue;
        highest = std::max(highest, element.end);
        // Each part of a shifted edge maps linearly onto its range of the source edge.
        if (t >= element.begin && t <= element.end && element.end > element.begin) {
            const Vec2d from = surface.polygon[i].cast<double>();
            const Vec2d at = from + (t - element.begin) / (element.end - element.begin) *
                                        (surface.polygon[(i + 1) % surface.polygon.size()].cast<double>() - from);
            return Point(coord_t(std::lround(at.x())), coord_t(std::lround(at.y())));
        }
    }
    // A dead stretch, next to the vertex beyond the edge's surface or before it.
    return concave_crossing(surface, t > highest ? (edge + 1) % count : edge).value_or(point);
}

// Finalize one joined strong segment; only Center needs temporary per-edge lengths.
static void prepare_strong_segment(PerimeterSegment &segment, ModelVolumeType mode, bool compare_lengths,
                                   const PreparedPerimeter &prepared)
{
    const bool center = mode == ModelVolumeType::PRECISE_SEAM_CENTER;
    std::vector<double> edge_lengths;
    if (center)
        edge_lengths.reserve(segment.edge_indices.size());
    // A single Left/Right segment needs only its endpoint; Center always needs arc length.
    if (center || compare_lengths) {
        for (size_t i = 0; i < segment.edge_indices.size(); ++i) {
            const double length = (segment.polyline.points[i + 1].cast<double>() -
                                   segment.polyline.points[i].cast<double>()).norm();
            segment.length += length;
            if (center)
                edge_lengths.push_back(length);
        }
    }
    if (mode == ModelVolumeType::PRECISE_SEAM_LEFT) {
        segment.strong_target = StrongSeamTarget{segment.polyline.points.front(), segment.begin.edge_index, segment.surface_first};
        return;
    }
    segment.strong_target = StrongSeamTarget{segment.polyline.points.back(), segment.end.edge_index, segment.surface_last};
    if (!center)
        return;
    // Only a point carried onto a noisy loop needs the pair of the middle.
    const auto pair_center = [&segment, &prepared]() {
        if (prepared.target != nullptr)
            segment.strong_target->surface = center_surface_point(prepared, segment, segment.strong_target->point,
                                                                  segment.strong_target->edge_index);
    };

    // The total is now known; reuse local lengths to locate its half without new square roots.
    double remaining = segment.length * 0.5;
    for (size_t i = 0; i < edge_lengths.size(); ++i) {
        const double length = edge_lengths[i];
        if (remaining <= length && length > 0.) {
            const Point &a = segment.polyline.points[i];
            const Point &b = segment.polyline.points[i + 1];
            segment.strong_target = StrongSeamTarget{
                a + ((remaining / length) * (b - a).cast<double>()).cast<coord_t>(), segment.edge_indices[i]};
            pair_center();
            return;
        }
        remaining -= length;
    }
    pair_center();
}

// Clipper returns a single point where a border only touches the line: nothing to bind.
static void append_fragments(Polylines &&clipped, Polylines &fragments)
{
    for (Polyline &fragment : clipped)
        if (fragment.size() >= 2)
            fragments.push_back(std::move(fragment));
}

// Centerline path, used when the surface coincides with the centerline: clip it as an open line, without
// offsets or contact rules, and bind the fragments to the perimeter itself. The open path keeps
// self-touching vertices apart.
static std::vector<detail::ClippedEdgeInterval> centerline_intervals(const PreparedPerimeter &prepared,
    const std::vector<const ModifierRegion*> &regions, const ExtractionContext &context, size_t &discarded)
{
    std::vector<detail::ClippedEdgeInterval> intervals;
    Polylines fragments;
    // Clip only accepted regions, with their holes still attached to the exterior.
    for (const ModifierRegion *region : regions)
        append_fragments(intersection_pl(prepared.line, region->polygon), fragments);
    for (size_t i = 0; i < fragments.size(); ++i)
        if (!detail::append_fragment(fragments[i], prepared.polygon, intervals, context, i))
            ++discarded;
    return intervals;
}

// Surface path: clip the surface line, bind the fragments to it, then carry them onto the centerline along
// the normal. A shifted edge keeps its parameters, an arc or bridge collapses to its vertex, and a dead stretch
// whose two ends are both covered covers the centerline between them.
static std::vector<detail::ClippedEdgeInterval> surface_intervals(const PreparedPerimeter &prepared,
    const std::vector<const ModifierRegion*> &regions, const ExtractionContext &context, size_t &discarded)
{
    using detail::ClippedEdgeInterval;
    std::vector<ClippedEdgeInterval> intervals;
    const detail::SurfaceLine &surface = prepared.surface();
    Polylines fragments;
    for (const ModifierRegion *region : regions)
        if (surface.bounds.defined && surface.bounds.overlap(region->bounds))
            append_fragments(intersection_pl(surface.line, region->polygon), fragments);
    std::vector<ClippedEdgeInterval> bound;
    for (size_t i = 0; i < fragments.size(); ++i)
        if (!detail::append_fragment(fragments[i], surface.polygon, bound, context, i))
            ++discarded;
    if (bound.empty())
        return intervals;

    const Polygon &perimeter = prepared.polygon;
    using detail::CenterlinePoint;
    // An edge with a constant shift is an exact translate, so a point moves back by the same integer shift. Where
    // the shift changes, the point on the source edge follows from the parameter. A shifted edge that ends at a
    // concave crossing covers only part of its source edge.
    const auto to_centerline = [&](size_t element, double parameter, const Point &point) -> CenterlinePoint {
        const detail::SurfaceElement &source = surface.elements[element];
        if (source.vertex)
            return {source.source, 0., perimeter[source.source]};
        const double t = source.begin + parameter * (source.end - source.begin);
        if (surface.translated[source.source])
            return {source.source, t, point - surface.shifts[source.source]};
        const Vec2d a = perimeter[source.source].cast<double>();
        const Vec2d at = a + t * (perimeter[(source.source + 1) % perimeter.size()].cast<double>() - a);
        return {source.source, t, Point(coord_t(std::lround(at.x())), coord_t(std::lround(at.y())))};
    };
    for (const ClippedEdgeInterval &interval : bound) {
        const CenterlinePoint first = to_centerline(interval.edge, interval.begin, interval.first);
        const CenterlinePoint last = to_centerline(interval.edge, interval.end, interval.last);
        // An arc gives a vertex point, a shifted edge the same interval on its source edge.
        intervals.push_back({first.edge, first.parameter, last.parameter, first.point, last.point, interval.first, interval.last});
    }

    // A stretch counts as reached when coverage ends at its end point on the same element; the snapping
    // radius absorbs a modifier boundary passing through the corner itself.
    const auto reaches = [](const Point &a, const Point &b) {
        return (a - b).cast<double>().squaredNorm() < double(TOLERANCE_SQUARED);
    };
    // Indexed by element: a hole has a gap at every vertex, a covered hole an interval on every element.
    std::unordered_multimap<size_t, const ClippedEdgeInterval*> by_element;
    by_element.reserve(bound.size());
    for (const ClippedEdgeInterval &interval : bound)
        by_element.emplace(interval.edge, &interval);
    for (const detail::SurfaceGap &gap : surface.gaps) {
        const ClippedEdgeInterval *before = nullptr;
        const ClippedEdgeInterval *after = nullptr;
        for (auto [it, end] = by_element.equal_range(gap.end_element); it != end; ++it)
            if (reaches(it->second->last, gap.end))
                before = it->second;
        for (auto [it, end] = by_element.equal_range(gap.start_element); it != end; ++it)
            if (reaches(it->second->first, gap.start))
                after = it->second;
        if (before != nullptr && after != nullptr)
            detail::cover_centerline(perimeter, to_centerline(before->edge, before->end, before->last),
                                     to_centerline(after->edge, after->begin, after->first), intervals);
    }
    return intervals;
}

SegmentExtraction extract_perimeter_segments(const PreparedPerimeter &prepared, const ModifierRegions &modifier,
                                             ModelVolumeType mode, const ExtractionContext &context)
{
    using detail::ClippedEdgeInterval;
    SegmentExtraction result;
    if (!prepared.valid) {
        result.valid = false;
        return result;
    }
    const Polygon &perimeter = prepared.polygon;
    const size_t count = perimeter.size();
    // The surface of an outer contour reaches the shift beyond the centerline's bounds.
    BoundingBox reach = prepared.bounds;
    if (prepared.shift > 0)
        reach.offset(coordf_t(prepared.shift));
    std::vector<const ModifierRegion*> nearby_regions;
    for (const ModifierRegion &region : modifier)
        if (!region.polygon.empty() && reach.overlap(region.bounds))
            nearby_regions.push_back(&region);
    if (nearby_regions.empty())
        return result;

    std::vector<ClippedEdgeInterval> intervals = prepared.shift > 0 ?
        surface_intervals(prepared, nearby_regions, context, result.discarded_fragments) :
        centerline_intervals(prepared, nearby_regions, context, result.discarded_fragments);

    std::sort(intervals.begin(), intervals.end(), [](const auto &a, const auto &b) {
        if (a.edge != b.edge) return a.edge < b.edge;
        if (a.begin != b.begin) return a.begin < b.begin;
        return a.end < b.end;
    });
    // Union overlaps on the same occurrence of an edge, never across equal coordinates.
    std::vector<ClippedEdgeInterval> merged;
    for (const ClippedEdgeInterval &interval : intervals) {
        // Fragments touching on an edge meet at the same integer point. Comparing that point as well keeps
        // them joined even if its two projections differ by an ulp on some platforms (e.g. FMA contraction).
        if (!merged.empty() && merged.back().edge == interval.edge &&
            (interval.begin <= merged.back().end || interval.first == merged.back().last)) {
            if (interval.end > merged.back().end) {
                merged.back().end = interval.end;
                merged.back().last = interval.last;
                merged.back().surface_last = interval.surface_last;
            } else if (prepared.target != nullptr && interval.begin == interval.end && merged.back().begin == merged.back().end &&
                       interval.surface_first && merged.back().surface_first) {
                // Separate pieces of one arc make one vertex point, from the piece nearest the arc's start to the
                // end of the farthest, whatever order sorting left them in.
                const detail::SurfaceLine &surface = prepared.surface();
                const std::vector<size_t> chain = vertex_chain(surface, interval.edge);
                const auto at = [&](const std::optional<Point> &point) { return chain_position(surface, chain, *point); };
                if (!chain.empty()) {
                    if (at(interval.surface_first) < at(merged.back().surface_first))
                        merged.back().surface_first = interval.surface_first;
                    if (at(interval.surface_last) > at(merged.back().surface_last))
                        merged.back().surface_last = interval.surface_last;
                }
            }
        } else
            merged.push_back(interval);
    }
    const auto position = [count](size_t edge, double parameter) {
        return parameter == 1. ? PerimeterPosition{(edge + 1) % count, 0.}
                               : PerimeterPosition{edge, parameter};
    };
    const auto same_position = [](const PerimeterPosition &a, const PerimeterPosition &b) {
        return a.edge_index == b.edge_index && a.parameter == b.parameter;
    };
    for (const ClippedEdgeInterval &interval : merged) {
        const auto begin = position(interval.edge, interval.begin);
        const auto end = position(interval.edge, interval.end);
        const bool continues = !result.segments.empty() && same_position(result.segments.back().end, begin);
        // A vertex point (surface arc) adds no edge: it only starts a zero-length segment if nothing reaches it.
        if (interval.begin == interval.end) {
            if (!continues) {
                PerimeterSegment segment;
                segment.begin = segment.end = begin;
                segment.polyline.points.push_back(interval.first);
                segment.surface_first = interval.surface_first.value_or(interval.first);
                segment.surface_last = interval.surface_last.value_or(interval.last);
                result.segments.push_back(std::move(segment));
            } else // The arc beyond the segment's last edge: its surface point pairs with the vertex the segment ends at.
                result.segments.back().surface_last = interval.surface_last.value_or(interval.last);
            continue;
        }
        if (!continues) {
            PerimeterSegment segment;
            segment.begin = begin;
            segment.polyline.points.push_back(interval.first);
            segment.surface_first = interval.surface_first.value_or(interval.first);
            result.segments.push_back(std::move(segment));
        }
        PerimeterSegment &segment = result.segments.back();
        segment.polyline.points.push_back(interval.last);
        segment.edge_indices.push_back(interval.edge);
        segment.end = end;
        segment.surface_last = interval.surface_last.value_or(interval.last);
    }
    if (result.segments.size() > 1 && same_position(result.segments.back().end, result.segments.front().begin)) {
        // Only the artificial cut at vertex zero can join the last and first intervals.
        PerimeterSegment tail = std::move(result.segments.back());
        result.segments.pop_back();
        PerimeterSegment &head = result.segments.front();
        tail.polyline.points.insert(tail.polyline.points.end(), head.polyline.points.begin() + 1, head.polyline.points.end());
        tail.edge_indices.insert(tail.edge_indices.end(), head.edge_indices.begin(), head.edge_indices.end());
        tail.end = head.end;
        tail.surface_last = head.surface_last;
        head = std::move(tail);
    }
    // A full loop must cover every original edge, not merely have equal endpoint coordinates. Vertex points
    // carry no coverage of their own.
    size_t covered_edges = 0;
    result.full_containment = true;
    for (const ClippedEdgeInterval &interval : merged)
        if (interval.begin != interval.end) {
            result.full_containment = result.full_containment && interval.edge == covered_edges &&
                                      interval.begin == 0. && interval.end == 1.;
            ++covered_edges;
        }
    result.full_containment = result.full_containment && covered_edges == count;
    // A touch on an inclined edge leaves a sub-micron gap. Treat the single segment as full containment
    // when the gap is shorter than the snapping radius, or spans one vertex with both ends within it
    // (exact for edges of 2 um and longer). The 2 um end distance is a cheap pre-filter. A zero-length
    // segment covers nothing, so it never qualifies.
    if (!result.full_containment && result.segments.size() == 1 && !result.segments.front().edge_indices.empty()) {
        const PerimeterSegment &only = result.segments.front();
        const Point &first = only.polyline.points.front();
        const Point &last = only.polyline.points.back();
        const double limit = double(TOLERANCE_LINEAR);
        if ((first - last).cast<double>().squaredNorm() < 4. * limit * limit) {
            const auto edge_length = [&perimeter, count](size_t edge) {
                return (perimeter[(edge + 1) % count] - perimeter[edge]).cast<double>().norm();
            };
            // Uncovered length from the segment's end forward to its begin; stops early once it exceeds the limit.
            const auto gap_length = [&](const PerimeterPosition &from, const PerimeterPosition &to) {
                if (from.edge_index == to.edge_index && from.parameter <= to.parameter)
                    return (to.parameter - from.parameter) * edge_length(from.edge_index);
                double gap = (1. - from.parameter) * edge_length(from.edge_index);
                for (size_t edge = (from.edge_index + 1) % count; gap < limit && edge != to.edge_index; edge = (edge + 1) % count)
                    gap += edge_length(edge);
                return gap + to.parameter * edge_length(to.edge_index);
            };
            // Strict comparisons match the vertex snapping in insert_point_into_perimeter.
            const auto within_snap = [limit](const Point &a, const Point &b) {
                return (a - b).cast<double>().squaredNorm() < limit * limit;
            };
            const Point &vertex = perimeter[only.begin.edge_index];
            const bool around_one_vertex = (only.end.edge_index + 1) % count == only.begin.edge_index &&
                                           within_snap(last, vertex) && within_snap(first, vertex);
            if (around_one_vertex || gap_length(only.end, only.begin) < limit)
                result.full_containment = true;
        }
    }
    // Strong skips full containment and weak handles it by type; neither needs strong data for it.
    if (!result.full_containment && is_precise_seam_strong(mode))
        for (PerimeterSegment &segment : result.segments)
            prepare_strong_segment(segment, mode, result.segments.size() > 1, prepared);
    return result;
}

void init_precise_seam_data(
    std::vector<const ModelVolume*>& strong_volumes_out,
    std::vector<const ModelVolume*>& weak_volumes_out,
    bool& has_strong_out,
    const ModelObject* model_object)
{
    // Clear output vectors
    strong_volumes_out.clear();
    weak_volumes_out.clear();

    if (model_object == nullptr) {
        has_strong_out = false;
        return;
    }

    // Collect and categorize precise seam modifiers
    for (const ModelVolume* volume : model_object->volumes) {
        if (volume->is_precise_seam()) {
            // Strong (Center/Left/Right) and weak (Enforced/Blocked/Neutral) go to separate lists.
            if (volume->is_precise_seam_strong())
                strong_volumes_out.push_back(volume);
            else
                weak_volumes_out.push_back(volume);
        }
    }

    has_strong_out = !strong_volumes_out.empty();

    // Collection already preserves model order, with higher-priority strong modifiers first.
    // Weak modifiers use last-write-wins, so apply the higher-priority ones last.
    std::reverse(weak_volumes_out.begin(), weak_volumes_out.end());
}

// Inserts a point on edge `edge_start_idx`, or reuses a vertex closer than TOLERANCE_LINEAR.
// Returns {final coordinates, index}. Requires at least three vertices (callers ensure it).
static std::pair<Point, size_t> insert_point_into_perimeter(
    const Point &point,
    size_t edge_start_idx,
    Polygon &perimeter_polygon
)
{
    assert(perimeter_polygon.points.size() >= 3);
    size_t perim_max = perimeter_polygon.points.size();

    // Determine edge start and end
    size_t vtx_start = edge_start_idx;
    size_t vtx_end = (edge_start_idx + 1) % perim_max;

    const Point &perim_p_start = perimeter_polygon.points[vtx_start];
    const Point &perim_p_end = perimeter_polygon.points[vtx_end];

    // Check proximity to edge vertices
    coord_t dist_sq_start = (point - perim_p_start).squaredNorm();
    if (dist_sq_start < TOLERANCE_SQUARED) {
        return std::make_pair(perim_p_start, vtx_start);
    }

    coord_t dist_sq_end = (point - perim_p_end).squaredNorm();
    if (dist_sq_end < TOLERANCE_SQUARED) {
        return std::make_pair(perim_p_end, vtx_end);
    }

    // On the closing edge, append instead of inserting at index 0: callers insert in descending source
    // order and rely on earlier indices staying valid.
    size_t insert_pos;
    if (edge_start_idx == perim_max - 1) {
        // Last edge: add to end of vector
        perimeter_polygon.points.push_back(point);
        insert_pos = perimeter_polygon.points.size() - 1;
    } else {
        // Regular edge: insert before end vertex
        insert_pos = vtx_end;
        perimeter_polygon.points.insert(
            perimeter_polygon.points.begin() + insert_pos,
            point
        );
    }

    return std::make_pair(perimeter_polygon.points[insert_pos], insert_pos);
}

// Adds a helper point TOLERANCE_LINEAR after (+1) or before (-1) vertex `point_idx`; skips edges shorter
// than 2 * TOLERANCE_LINEAR. Requires at least three vertices.
static void refine_at_vertex(
    size_t point_idx,
    int direction,
    Polygon &perimeter_polygon
)
{
    assert(perimeter_polygon.points.size() >= 3);
    assert(direction == 1 || direction == -1);

    size_t perim_max = perimeter_polygon.points.size();

    // Determine target edge based on direction
    size_t edge_start_idx, edge_end_idx;

    if (direction == 1) {
        // Direction +1: insertion AFTER point_idx (edge point_idx → point_idx+1)
        edge_start_idx = point_idx;
        edge_end_idx = (point_idx + 1) % perim_max;
    } else {
        // Direction -1: insertion BEFORE point_idx (edge point_idx-1 → point_idx)
        edge_start_idx = (point_idx + perim_max - 1) % perim_max;
        edge_end_idx = point_idx;
    }

    const Point &edge_start = perimeter_polygon.points[edge_start_idx];
    const Point &edge_end = perimeter_polygon.points[edge_end_idx];

    // Calculate edge length
    Vec2d edge_vector = (edge_end - edge_start).cast<double>();
    double edge_length = edge_vector.norm();

    // The helper needs TOLERANCE_LINEAR of edge on both sides.
    if (edge_length < 2.0 * TOLERANCE_LINEAR) {
        return;  // Edge too short - new point would be too close to end
    }

    // Calculate new point coordinates: edge_start + TOLERANCE_LINEAR * direction_normalized
    Vec2d direction_normalized = edge_vector / edge_length;
    // Place helper point near point_idx: after it for +1, before it for -1
    auto offset = (TOLERANCE_LINEAR * direction_normalized).cast<coord_t>();
    Point new_point = (direction == 1)
        ? Point(edge_start + offset)
        : Point(edge_end   - offset);

    // On the closing edge, append instead of inserting at index 0: callers insert in descending source
    // order and rely on earlier indices staying valid.
    if (edge_start_idx == perim_max - 1) {
        // Last edge: add to end of vector
        perimeter_polygon.points.push_back(new_point);
    } else {
        // Regular edge: insert before end vertex
        perimeter_polygon.points.insert(
            perimeter_polygon.points.begin() + edge_end_idx,
            new_point
        );
    }
}

// Records one extraction for the warnings: the type of a modifier with a discarded fragment, the
// evaluated/reached flags for "had no effect" (a discarded fragment counts as reached, a contact not).
static void record_extraction(PreciseSeamWarnings *warnings, const ModelVolume *modifier,
                              const SegmentExtraction &extracted)
{
    if (warnings == nullptr || !extracted.valid)
        return;
    if (extracted.discarded_fragments > 0)
        PreciseSeamWarnings::mark(warnings->failed_types, modifier->type());
    const auto it = warnings->modifier_usage.find(modifier);
    if (it == warnings->modifier_usage.end())
        return;
    PreciseSeamWarnings::ModifierUsage &usage = it->second;
    PreciseSeamWarnings::set(usage.checked);
    if (!extracted.segments.empty() || extracted.full_containment || extracted.discarded_fragments > 0)
        PreciseSeamWarnings::set(usage.reached);
}

// Carries a point found on the smooth loop onto the noisy loop it is inserted into, through the line to its surface
// point. Counts the approximate outcomes, and logs a failure like a discarded fragment, which it then counts as.
static detail::Transfer to_insertion(const PreparedPerimeter &prepared, const Point &point, const Point &surface,
                                     const ExtractionContext &context)
{
    const detail::Transfer moved = detail::transfer_point(prepared.target_lines(), point, surface, double(prepared.window));
    if (context.warnings != nullptr && moved.outcome == detail::TransferOutcome::Approximate)
        context.warnings->approximate_transfers.fetch_add(1, std::memory_order_relaxed);
    if (moved.outcome == detail::TransferOutcome::Failed) {
        if (context.warnings != nullptr && context.modifier != nullptr)
            PreciseSeamWarnings::mark(context.warnings->failed_types, context.modifier->type());
        if (context.warnings == nullptr ||
            context.warnings->failed_fragments.fetch_add(1, std::memory_order_relaxed) < failed_fragment_log_limit)
            BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamTransferFailed] No point of the fuzzy skin loop near the seam point"
                << detail::fragment_location(context, 0) << " window=" << prepared.window;
    }
    return moved;
}

// Strong priority is per modifier, never a global maximum across different modifiers.
std::optional<Point> insert_strong_seam_point(
    const std::vector<const ModelVolume*> &strong_volumes,
    Polygon &polygon,
    const PreparedPerimeter &prepared,
    const Layer *layer,
    const ModifierRegionsCache &slices_cache,
    PreciseSeamWarnings* warnings)
{
    assert(&prepared.insertion() == &polygon);
    if (strong_volumes.empty() || layer == nullptr || !prepared.valid)
        return std::nullopt;
    const size_t layer_id = layer->id() - layer->object()->slicing_parameters().raft_layers();
    for (const ModelVolume *modifier : strong_volumes) {
        const auto it = slices_cache.find(modifier);
        if (it == slices_cache.end() || layer_id >= it->second.size())
            continue;
        const SegmentExtraction extracted = extract_perimeter_segments(
            prepared, it->second[layer_id], modifier->type(), {layer, modifier, warnings});
        record_extraction(warnings, modifier, extracted);
        // Full containment leaves no intersection to place the point on: skipped with a warning.
        if (extracted.full_containment) {
            if (warnings)
                PreciseSeamWarnings::mark(warnings->full_containment, modifier->type());
            continue;
        }
        // No usable segment, even if all fragments were discarded: pass the turn to the next modifier.
        if (extracted.segments.empty())
            continue;
        if (warnings && extracted.segments.size() > 1)
            PreciseSeamWarnings::mark(warnings->multiple_intersections, modifier->type());

        // Lengths are compared exactly: for a symmetric modifier the chosen face may alternate between
        // layers, which is accepted (the user gets the multiple-intersections warning).
        StrongSeamTarget target{Point(0, 0), 0};
        // The winner's point on the smooth loop breaks ties, so the noise of a fuzzy skin loop never changes the winner.
        Point chosen(0, 0);
        double longest = -1.;
        const ExtractionContext context{layer, modifier, warnings};
        for (const PerimeterSegment &segment : extracted.segments) {
            assert(segment.strong_target.has_value());
            const StrongSeamTarget &candidate = *segment.strong_target;
            // Slice coordinates already include object rotation and retain bed axes.
            // Centering/instance translation cannot change rear (+Y), then left (-X) ordering.
            const bool farther_or_left = candidate.point.y() > chosen.y() ||
                (candidate.point.y() == chosen.y() && candidate.point.x() < chosen.x());
            if (!(segment.length > longest || (segment.length == longest && farther_or_left)))
                continue;
            StrongSeamTarget inserted = candidate;
            // Chosen on the smooth loop, inserted into the noisy one with fuzzy skin. A point that cannot be carried
            // is a discarded fragment: its segment takes no part.
            if (prepared.target != nullptr) {
                const detail::Transfer moved = to_insertion(prepared, candidate.point, candidate.surface.value_or(candidate.point), context);
                if (moved.outcome == detail::TransferOutcome::Failed)
                    continue;
                inserted = StrongSeamTarget{moved.position.point, moved.position.edge};
            }
            longest = segment.length;
            chosen = candidate.point;
            target = inserted;
        }
        // Every segment failed to be carried: the turn passes to the next modifier.
        if (longest < 0.)
            continue;
        const auto [seam_point, seam_index] = insert_point_into_perimeter(target.point, target.edge_index, polygon);
        // Preserve the insertion order: refining before first would shift the seam index.
        refine_at_vertex(seam_index, +1, polygon);
        refine_at_vertex(seam_index, -1, polygon);
        return seam_point; // No later strong modifier or weak processing for this perimeter.
    }
    return std::nullopt;
}

// Convert ModelVolumeType of weak modifier to EnforcedBlockedSeamPoint.
// Precondition: a weak precise-seam type; init_precise_seam_data() puts only those in the weak list.
// Exhaustive switch (no default) so -Wswitch flags any future PRECISE_SEAM_* additions.
static EnforcedBlockedSeamPoint convert_weak_modifier_type(ModelVolumeType type) {
    assert(is_precise_seam_weak(type));
    switch (type) {
        case ModelVolumeType::PRECISE_SEAM_ENFORCED:
            return EnforcedBlockedSeamPoint::Enforced;
        case ModelVolumeType::PRECISE_SEAM_BLOCKED:
            return EnforcedBlockedSeamPoint::Blocked;
        case ModelVolumeType::PRECISE_SEAM_NEUTRAL:
            return EnforcedBlockedSeamPoint::Neutral;
        // Non-weak types are unreachable by precondition; listed to keep the switch exhaustive.
        case ModelVolumeType::INVALID:
        case ModelVolumeType::MODEL_PART:
        case ModelVolumeType::NEGATIVE_VOLUME:
        case ModelVolumeType::PARAMETER_MODIFIER:
        case ModelVolumeType::SUPPORT_BLOCKER:
        case ModelVolumeType::SUPPORT_ENFORCER:
        case ModelVolumeType::PRECISE_SEAM_CENTER:
        case ModelVolumeType::PRECISE_SEAM_LEFT:
        case ModelVolumeType::PRECISE_SEAM_RIGHT:
        case ModelVolumeType::PAINTED_MODIFIER:
            break;
    }
    assert(false && "convert_weak_modifier_type called with non-weak type");
    return EnforcedBlockedSeamPoint::Neutral;
}

// Consume ready boundaries in modifier priority order; geometry extraction is separate.
static std::vector<WeakModifierSegment> prepare_weak_modifier_segments(
    std::vector<WeakModifierSegment> result, Polygon &polygon)
{
    // If no segments, return empty vector
    if (result.empty()) {
        return result;
    }

    // Source positions have the same order as arc length, without measuring the perimeter.
    struct PointToInsert {
        size_t segment_idx;
        bool is_left;
        PerimeterPosition position;
    };
    std::vector<PointToInsert> points_to_insert;
    points_to_insert.reserve(result.size() * 2);
    for (size_t seg_idx = 0; seg_idx < result.size(); ++seg_idx) {
        const WeakModifierSegment &seg = result[seg_idx];
        // A whole-perimeter zone has no boundaries to insert.
        if (seg.whole_perimeter)
            continue;
        points_to_insert.push_back({seg_idx, true, seg.left_position});
        points_to_insert.push_back({seg_idx, false, seg.right_position});
    }

    // Descending order preserves pending source indices; vertex zero is canonical (0, 0).
    // Only insertion events are reordered: modifier priority in result remains unchanged.
    std::sort(points_to_insert.begin(), points_to_insert.end(),
              [](const PointToInsert &a, const PointToInsert &b) {
                  if (a.position.edge_index != b.position.edge_index)
                      return a.position.edge_index > b.position.edge_index;
                  return a.position.parameter > b.position.parameter;
              });

    for (const PointToInsert &pt : points_to_insert) {
        WeakModifierSegment &seg = result[pt.segment_idx];
        Point &point_coords = pt.is_left ? seg.left_point : seg.right_point;
        // Take the final coordinates: a boundary close to an existing vertex snaps to it.
        point_coords = insert_point_into_perimeter(point_coords, pt.position.edge_index, polygon).first;
    }

    // Add helpers at boundary vertices, scanning original vertices backwards so insertions do not shift
    // pending indices.
    for (size_t poly_idx = polygon.size(); poly_idx-- > 0; ) {
        bool refine_before = false;
        bool refine_after = false;
        for (const WeakModifierSegment &segment : result) {
            // Its placeholder points must not match a real vertex.
            if (segment.whole_perimeter)
                continue;
            refine_before |= polygon[poly_idx] == segment.left_point;
            refine_after |= polygon[poly_idx] == segment.right_point;
        }
        // Insert after first: inserting before would shift the current vertex index.
        if (refine_after)
            refine_at_vertex(poly_idx, +1, polygon);
        if (refine_before)
            refine_at_vertex(poly_idx, -1, polygon);
    }

    // Split edges in enforced zones into segments ≤ enforcer_oversampling_distance
    // Determine type pattern for each polygon edge (sequential application of hierarchy)
    std::vector<EnforcedBlockedSeamPoint> edge_types(polygon.size(), EnforcedBlockedSeamPoint::Neutral);

    // Every boundary was inserted or snapped above and later steps only add vertices, so it is found.
    // A linear scan is fine: there are only a few zones per perimeter.
    auto find_point_index = [&](const Point &pt) -> size_t {
        const auto it = std::find(polygon.points.begin(), polygon.points.end(), pt);
        assert(it != polygon.points.end());
        return size_t(it - polygon.points.begin());
    };

    // Last write wins: zones are sorted lowest priority first.
    for (const auto &segment : result) {
        // A whole-perimeter zone types every edge, so Enforced subdivides the whole perimeter, as
        // painting it green all round would.
        if (segment.whole_perimeter) {
            std::fill(edge_types.begin(), edge_types.end(), segment.type);
            continue;
        }
        const size_t left_idx = find_point_index(segment.left_point);
        const size_t right_idx = find_point_index(segment.right_point);
        // Practically unreachable, but a miss would write past edge_types in Release builds: skip the zone.
        if (left_idx == polygon.size() || right_idx == polygon.size()) {
            BOOST_LOG_TRIVIAL(error) << "PreciseSeam: weak boundary not found in perimeter, skipping zone";
            continue;
        }

        // A zone covers edges [left_idx, right_idx): the edge from the right boundary is outside and must
        // not be subdivided. Edge types only drive subdivision; candidates are typed per point later.
        for (size_t idx = left_idx; idx != right_idx; idx = (idx + 1) % polygon.size())
            edge_types[idx] = segment.type;
    }

    // Boundaries and helpers are already in place; without enforced edges nothing is subdivided,
    // so skip rebuilding the polygon.
    if (std::find(edge_types.begin(), edge_types.end(), EnforcedBlockedSeamPoint::Enforced) == edge_types.end())
        return result;

    // Split enforced edges into small segments
    const double STEP_SCALED = scale_(SeamPlacer::enforcer_oversampling_distance);

    // Collect new list of polygon points with split enforced edges
    Points new_points;
    new_points.reserve(polygon.size() * 10); // Approximate estimate

    for (size_t i = 0; i < polygon.size(); ++i) {
        size_t next_i = (i + 1) % polygon.size();
        Point p_start = polygon[i];
        Point p_end = polygon[next_i];

        // Add current vertex
        new_points.push_back(p_start);

        // Check edge type [i, next_i]
        if (edge_types[i] != EnforcedBlockedSeamPoint::Enforced) {
            continue; // Not enforced - don't split
        }

        // Calculate subdivision parameters
        Vec2d edge_vec = (p_end - p_start).cast<double>();
        double edge_length = edge_vec.norm();

        if (edge_length <= STEP_SCALED) {
            continue; // Edge too short - don't split
        }

        // Number of segments: ceil(L / S)
        size_t num_segments = static_cast<size_t>(std::ceil(edge_length / STEP_SCALED));

        // Uniform step: L / num_segments (each segment ≤ S)
        double actual_step = edge_length / num_segments;
        Vec2d step_vec = edge_vec.normalized() * actual_step;

        // Add intermediate points incrementally
        Vec2d current_pos = p_start.cast<double>();
        for (size_t j = 1; j < num_segments; ++j) {
            current_pos += step_vec;
            new_points.push_back(current_pos.cast<coord_t>());
        }
    }

    // Replace polygon points with new ones (with split enforced edges)
    polygon.points = std::move(new_points);

    return result;
}

// Collect every modifier's ready segments before insertions can shift source edge indices.
std::vector<WeakModifierSegment> collect_weak_modifier_segments(
    const std::vector<const ModelVolume*> &weak_volumes,
    Polygon &polygon,
    const PreparedPerimeter &prepared,
    const Layer *layer,
    const ModifierRegionsCache &slices_cache,
    PreciseSeamWarnings* warnings)
{
    assert(&prepared.insertion() == &polygon);
    std::vector<WeakModifierSegment> result;
    if (weak_volumes.empty() || layer == nullptr || !prepared.valid)
        return result;
    // With fuzzy skin the zones are found on the smooth loop and their boundaries carried onto the noisy one; before
    // any insertion, a policy tells some zones whose boundaries came out in the reverse order.
    std::vector<double> source_arc;
    const auto arc_at = [&source_arc](const PerimeterPosition &position) {
        return source_arc[position.edge_index] +
               position.parameter * (source_arc[position.edge_index + 1] - source_arc[position.edge_index]);
    };
    // The length of the smooth loop from one position forward to another, across its start if need be.
    const auto forward = [&](const PerimeterPosition &from, const PerimeterPosition &to) {
        const double length = arc_at(to) - arc_at(from);
        return length >= 0. ? length : length + source_arc.back();
    };
    // Walks the noisy loop from one carried position forward to another: the first position, every vertex between them
    // across the start of the loop, then the second position. Positions on one edge with the second behind the first go
    // round the whole loop. Stops where `visit` returns false.
    const auto walk = [&prepared](const detail::CenterlinePoint &from, const detail::CenterlinePoint &to, const auto &visit) {
        const Polygon &noisy = *prepared.target;
        const size_t count = noisy.size();
        size_t vertices = (to.edge + count - from.edge) % count;
        if (vertices == 0 && to.parameter < from.parameter)
            vertices = count;
        if (!visit(from.point))
            return;
        for (size_t i = 1; i <= vertices; ++i)
            if (!visit(noisy[(from.edge + i) % count]))
                return;
        visit(to.point);
    };
    // Whether that noisy arc leaves the circle around `center`: a point of it lies farther than `radius`.
    const auto leaves = [&walk](const detail::CenterlinePoint &from, const detail::CenterlinePoint &to, const Point &center,
                                double radius) {
        bool outside = false;
        walk(from, to, [&outside, &center, limit = radius * radius](const Point &point) {
            outside = (point - center).cast<double>().squaredNorm() > limit;
            return !outside;
        });
        return outside;
    };
    // The length of that noisy arc.
    const auto noisy_length = [&walk](const detail::CenterlinePoint &from, const detail::CenterlinePoint &to) {
        double length = 0.;
        std::optional<Point> last;
        walk(from, to, [&length, &last](const Point &point) {
            if (last)
                length += (point - *last).cast<double>().norm();
            last = point;
            return true;
        });
        return length;
    };
    // A policy, not a proven guarantee. It assumes the points of a zone's true noisy arc lie within the window of the
    // smooth zone, as its boundaries do; the window is an estimate. Then for a zone shorter than four windows that arc
    // stays within the circle around its smooth left boundary whose radius is the zone's length plus a window, however
    // much the noise lengthens it, and the arc round the rest of the loop leaves the circle wherever the loop spreads
    // beyond it (its length tells nothing). So: the arc forward from the left boundary to the right one leaving the
    // circle reverses the zone; the other arc leaving it keeps the order; with both inside, a loop that lies within the
    // circle, the longer arc is taken for the rest of the loop, which may reduce a zone wrongly or miss a reversal.
    // Coinciding carried positions within the circle give two arcs of zero length and keep the order. The same holds
    // round the smooth right boundary for the short rest of an almost whole zone, with the arcs swapped.
    // Longer on both sides, the order is not checked: a transfer onto another part of the loop there is a limitation of
    // the direct transfer.
    const double window = double(prepared.window);
    const auto decide = [&](const detail::CenterlinePoint &from, const detail::CenterlinePoint &to, const Point &center,
                            double radius) {
        if (leaves(from, to, center, radius))
            return true;
        if (leaves(to, from, center, radius))
            return false;
        return noisy_length(from, to) > noisy_length(to, from);
    };
    const auto reversed = [&](const PerimeterSegment &segment, const detail::CenterlinePoint &left,
                              const detail::CenterlinePoint &right) {
        const double zone = forward(segment.begin, segment.end);
        const double rest = source_arc.back() - zone;
        if (zone < 4. * window)
            return decide(left, right, segment.polyline.points.front(), zone + window);
        if (rest < 4. * window)
            return decide(right, left, segment.polyline.points.back(), rest + window);
        return false;
    };
    // False when a boundary cannot be carried: the zone is then a discarded fragment.
    const auto carry = [&](const PerimeterSegment &segment, WeakModifierSegment &zone, const ExtractionContext &context) {
        if (source_arc.empty()) {
            source_arc.push_back(0.);
            const Polygon &smooth = prepared.polygon;
            for (size_t edge = 0; edge < smooth.size(); ++edge)
                source_arc.push_back(source_arc.back() + (smooth[(edge + 1) % smooth.size()] - smooth[edge]).cast<double>().norm());
        }
        const detail::Transfer left = to_insertion(prepared, segment.polyline.points.front(), segment.surface_first, context);
        if (left.outcome == detail::TransferOutcome::Failed)
            return false;
        detail::Transfer right = to_insertion(prepared, segment.polyline.points.back(), segment.surface_last, context);
        if (right.outcome == detail::TransferOutcome::Failed)
            return false;
        // A zone whose boundaries came out in the reverse order is reduced to its left boundary, a declared degradation.
        if (reversed(segment, left.position, right.position)) {
            right = left;
            if (context.warnings != nullptr)
                context.warnings->reversed_zones.fetch_add(1, std::memory_order_relaxed);
        }
        zone.left_point = left.position.point;
        zone.left_position = {left.position.edge, left.position.parameter};
        zone.right_point = right.position.point;
        zone.right_position = {right.position.edge, right.position.parameter};
        return true;
    };

    // Modifier cache indices exclude raft layers, unlike Layer::id().
    const size_t raft_layers = layer->object()->slicing_parameters().raft_layers();
    const size_t layer_id = layer->id() - raft_layers;
    for (const ModelVolume *modifier_volume : weak_volumes) {
        const auto it = slices_cache.find(modifier_volume);
        if (it == slices_cache.end() || layer_id >= it->second.size())
            continue;
        const ExtractionContext context{layer, modifier_volume, warnings};
        const SegmentExtraction extracted = extract_perimeter_segments(prepared, it->second[layer_id], modifier_volume->type(), context);
        record_extraction(warnings, modifier_volume, extracted);
        const auto type = convert_weak_modifier_type(modifier_volume->type());
        // Full containment works like seam painting all round: the whole perimeter takes the type.
        // A perimeter left whole Blocked also gets a warning (see apply_weak_modifiers_to_perimeter()).
        if (extracted.full_containment) {
            result.push_back({type, Point(), PerimeterPosition{0, 0.}, Point(), PerimeterPosition{0, 0.}, true, modifier_volume});
            continue;
        }
        for (const PerimeterSegment &segment : extracted.segments) {
            result.push_back({type, segment.polyline.points.front(), segment.begin,
                                   segment.polyline.points.back(), segment.end, false, modifier_volume});
            if (prepared.target != nullptr && !carry(segment, result.back(), context))
                result.pop_back();
        }
    }
    // Boundary insertion, refinement and last-write-wins priority use the established path.
    return prepare_weak_modifier_segments(std::move(result), polygon);
}

void apply_weak_modifiers_to_perimeter(
    const std::vector<WeakModifierSegment> &weak_segments,
    PrintObjectSeamData::LayerSeams &result,
    const SeamPlacerImpl::Perimeter &perimeter,
    bool &some_point_enforced,
    PreciseSeamWarnings *warnings)
{
    // Get z-coordinate for unscaling boundary points
    const float z_coord = result.points[perimeter.start_index].position.z();
    const size_t perimeter_size = perimeter.end_index - perimeter.start_index;

    // Helper lambda: search for point index in result.points by unscaled coordinates.
    // Linear scan is intentional — O(N×M) is acceptable for typical M ≤ 5 weak segments.
    auto find_point_index = [&](const Point &pt) -> std::optional<size_t> {
        Vec2f unscaled_pt = unscale(pt).cast<float>();
        Vec3f target(unscaled_pt.x(), unscaled_pt.y(), z_coord);
        for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i) {
            if (result.points[i].position == target) return i - perimeter.start_index;
        }
        return std::nullopt;
    };

    // Apply weak modifiers sequentially: sorted low-priority-first,
    // so higher-priority modifiers (higher in object tree) overwrite via last-write-wins.
    for (size_t seg_idx = 0; seg_idx < weak_segments.size(); ++seg_idx) {
        const auto &segment = weak_segments[seg_idx];

        // A whole-perimeter zone types every candidate, including painting's oversampled points.
        if (segment.whole_perimeter) {
            for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i)
                result.points[i].type = segment.type;
            if (segment.type == EnforcedBlockedSeamPoint::Enforced)
                some_point_enforced = true;
            continue;
        }

        // Find boundary point indices in result.points
        std::optional<size_t> left_idx = find_point_index(segment.left_point);
        std::optional<size_t> right_idx = find_point_index(segment.right_point);

        if (!left_idx.has_value() || !right_idx.has_value()) {
            BOOST_LOG_TRIVIAL(error) << "PreciseSeam: boundary point not found in perimeter, skipping segment";
            continue;
        }

        // Apply modifier type to range [left_idx, right_idx] with wraparound
        for (size_t idx = left_idx.value(); ; idx = (idx + 1) % perimeter_size) {
            result.points[perimeter.start_index + idx].type = segment.type;
            if (idx == right_idx.value()) break;
        }

        if (segment.type == EnforcedBlockedSeamPoint::Enforced) {
            some_point_enforced = true;
        }
    }

    // Only the highest whole zone is in effect, and a higher band may still unblock part of it.
    if (warnings == nullptr)
        return;
    const auto last_whole = std::find_if(weak_segments.rbegin(), weak_segments.rend(),
                                         [](const WeakModifierSegment &segment) { return segment.whole_perimeter; });
    if (last_whole == weak_segments.rend() || last_whole->type != EnforcedBlockedSeamPoint::Blocked)
        return;
    for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i)
        if (result.points[i].type != EnforcedBlockedSeamPoint::Blocked)
            return;
    if (const auto usage = warnings->modifier_usage.find(last_whole->modifier); usage != warnings->modifier_usage.end())
        PreciseSeamWarnings::set(usage->second.fully_blocked);
}

// Restore precise seam positions that may have been modified
void restore_precise_seam_positions(std::vector<PrintObjectSeamData::LayerSeams> &layers) {
  using SeamPlacerImpl::SeamCandidate;
  using SeamPlacerImpl::Perimeter;

  tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()),
    [&layers](tbb::blocked_range<size_t> r) {
      for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
        std::vector<SeamCandidate> &layer_perimeter_points = layers[layer_idx].points;
        // Iterate over perimeters (jump by end_index)
        for (size_t current = 0; current < layer_perimeter_points.size();
             current = layer_perimeter_points[current].perimeter.end_index) {
          Perimeter &perimeter = layer_perimeter_points[current].perimeter;
          if (perimeter.precise_seam_point.has_value()) {
            perimeter.final_seam_position = perimeter.precise_seam_point.value();
            perimeter.seam_index = perimeter.precise_seam_index;
          }
        }
      }
    });
}

} // namespace PreciseSeam
} // namespace Slic3r
