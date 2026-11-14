#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include "libslic3r/GCode/PreciseSeam.hpp"
#include "libslic3r/GCode/PreciseSeamInternal.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Line.hpp"
#include <memory>
#include "libslic3r/ExtrusionEntity.hpp"

#include <algorithm>
#include <initializer_list>
#include <cmath>
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include <utility>
#include <cstddef>
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Model.hpp"
#include <vector>
#include "libslic3r/libslic3r.h"
#include "libslic3r/Polyline.hpp"

using namespace Slic3r;

namespace {
Point mm(double x, double y) { return Point(scale_(x), scale_(y)); }

Polygon rectangle(double x0, double y0, double x1, double y1)
{
    // Modifier exteriors are CCW; holes are explicitly reversed in their fixtures.
    return Polygon(Points{mm(x0, y0), mm(x1, y0), mm(x1, y1), mm(x0, y1)});
}

void check_provenance(const Polygon &perimeter, const PreciseSeam::SegmentExtraction &result, bool measured = true)
{
    REQUIRE(result.valid);
    CHECK(result.discarded_fragments == 0);
    for (const auto &segment : result.segments) {
        REQUIRE(segment.polyline.size() >= 2);
        REQUIRE(segment.edge_indices.size() + 1 == segment.polyline.size());
        // Full coverage retains geometry, but consumers skip it before using strong data.
        if (result.full_containment)
            CHECK_FALSE(segment.strong_target.has_value());
        else {
            REQUIRE(segment.strong_target.has_value());
            REQUIRE(segment.strong_target->edge_index < perimeter.size());
        }
        if (measured && !result.full_containment) {
            CHECK_THAT(segment.polyline.length(), Catch::Matchers::WithinRel(segment.length, 1e-12));
            CHECK(segment.length > 0.);
        } else
            CHECK_THAT(segment.length, Catch::Matchers::WithinAbs(0., 1e-12));
        const std::pair<PreciseSeam::PerimeterPosition, Point> endpoints[] = {
            {segment.begin, segment.polyline.points.front()}, {segment.end, segment.polyline.points.back()}};
        for (const auto &[position, point] : endpoints) {
            REQUIRE(position.edge_index < perimeter.size());
            CHECK(position.parameter >= 0.);
            CHECK(position.parameter < 1.);
            const Vec2d a = perimeter.points[position.edge_index].cast<double>();
            const Vec2d b = perimeter.points[(position.edge_index + 1) % perimeter.size()].cast<double>();
            const Vec2d reconstructed = a + position.parameter * (b - a);
            CHECK((point.cast<double>() - reconstructed).squaredNorm() <= 2.5);
        }
        for (size_t i = 0; i < segment.edge_indices.size(); ++i) {
            const size_t edge = segment.edge_indices[i];
            REQUIRE(edge < perimeter.size());
            const Vec2d a = perimeter.points[edge].cast<double>();
            const Vec2d direction = perimeter.points[(edge + 1) % perimeter.size()].cast<double>() - a;
            double previous = -1.;
            for (const Point &point : {segment.polyline.points[i], segment.polyline.points[i + 1]}) {
                const Vec2d offset = point.cast<double>() - a;
                const double t = offset.dot(direction) / direction.squaredNorm();
                CHECK(t >= 0.);
                CHECK(t <= 1.);
                CHECK(t >= previous);
                CHECK((offset - t * direction).squaredNorm() <= 2.5);
                previous = t;
            }
        }
    }
}
} // namespace

TEST_CASE("Cached modifier bounds preserve holes and layer slots across perimeter queries", "[PreciseSeam][SegmentExtraction]")
{
    ExPolygon nearby(rectangle(2, -2, 8, 2));
    nearby.holes.push_back(rectangle(4, -1, 6, 1));
    nearby.holes.back().reverse();
    const ExPolygon distant(rectangle(102, -2, 108, 2));
    const auto cached = PreciseSeam::prepare_modifier_slices({
        {ExPolygon{}, nearby, distant}, {}, {ExPolygon(rectangle(2, 30, 8, 32))}});
    REQUIRE(cached.size() == 3);
    REQUIRE(cached[0].size() == 3);
    CHECK_FALSE(cached[0][0].bounds.defined);
    CHECK(cached[1].empty());
    REQUIRE(cached[2].size() == 1);
    CHECK(cached[2][0].bounds.min == mm(2, 30));
    CHECK(cached[0][1].bounds.min == mm(2, -2));
    CHECK(cached[0][1].bounds.max == mm(8, 2));
    REQUIRE(cached[0][1].polygon.holes.size() == 1);
    CHECK(cached[0][1].polygon.holes.front().points == nearby.holes.front().points);

    // Reuse one immutable layer cache; each perimeter sees only its nearby region.
    const Polygon first = rectangle(0, 0, 20, 20);
    const Polygon second = rectangle(100, 0, 120, 20);
    const PreciseSeam::PreparedPerimeter first_prepared(first), second_prepared(second);
    const auto left = PreciseSeam::extract_perimeter_segments(first_prepared, cached[0], ModelVolumeType::PRECISE_SEAM_CENTER);
    const auto right = PreciseSeam::extract_perimeter_segments(second_prepared, cached[0], ModelVolumeType::PRECISE_SEAM_CENTER);
    const auto repeated = PreciseSeam::extract_perimeter_segments(first_prepared, cached[0], ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(first, left);
    check_provenance(second, right);
    REQUIRE(left.segments.size() == 2);
    REQUIRE(right.segments.size() == 1);
    REQUIRE(repeated.segments.size() == left.segments.size());
    CHECK(left.segments[0].polyline.points.front() == mm(2, 0));
    CHECK(left.segments[0].polyline.points.back() == mm(4, 0));
    CHECK(left.segments[1].polyline.points.front() == mm(6, 0));
    CHECK(left.segments[1].polyline.points.back() == mm(8, 0));
    CHECK(right.segments[0].polyline.points.front() == mm(102, 0));
    CHECK(right.segments[0].polyline.points.back() == mm(108, 0));
    for (size_t i = 0; i < left.segments.size(); ++i) {
        CHECK(repeated.segments[i].polyline.points == left.segments[i].polyline.points);
        CHECK(repeated.segments[i].edge_indices == left.segments[i].edge_indices);
    }
    CHECK(cached[0][1].polygon.contour.points == nearby.contour.points);
}

TEST_CASE("Weak extraction preserves geometry and bindings without preparing strong data", "[PreciseSeam][SegmentExtraction]")
{
    const int scenario = GENERATE(0, 1, 2);
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    // Cover separate intervals, joining across vertex zero, and full containment.
    const ExPolygons modifier{ExPolygon(scenario == 0 ? rectangle(8, -2, 12, 22) :
                                        scenario == 1 ? rectangle(-2, -2, 4, 4) : rectangle(-2, -2, 22, 22))};
    const auto measured = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions(modifier), ModelVolumeType::PRECISE_SEAM_CENTER);
    const auto weak_mode = GENERATE(ModelVolumeType::PRECISE_SEAM_ENFORCED,
                                    ModelVolumeType::PRECISE_SEAM_BLOCKED,
                                    ModelVolumeType::PRECISE_SEAM_NEUTRAL);
    const auto unmeasured = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions(modifier), weak_mode);
    check_provenance(perimeter, measured);
    CHECK(unmeasured.valid == measured.valid);
    CHECK(unmeasured.full_containment == measured.full_containment);
    CHECK(unmeasured.discarded_fragments == measured.discarded_fragments);
    REQUIRE(unmeasured.segments.size() == measured.segments.size());
    for (size_t i = 0; i < measured.segments.size(); ++i) {
        const auto &expected = measured.segments[i];
        const auto &actual = unmeasured.segments[i];
        CHECK(actual.polyline.points == expected.polyline.points);
        CHECK(actual.edge_indices == expected.edge_indices);
        CHECK(actual.begin.edge_index == expected.begin.edge_index);
        CHECK(actual.end.edge_index == expected.end.edge_index);
        CHECK_THAT(actual.begin.parameter, Catch::Matchers::WithinAbs(expected.begin.parameter, 1e-12));
        CHECK_THAT(actual.end.parameter, Catch::Matchers::WithinAbs(expected.end.parameter, 1e-12));
        CHECK_FALSE(actual.strong_target.has_value());
        CHECK_THAT(actual.length, Catch::Matchers::WithinAbs(0., 1e-12));
    }
}

TEST_CASE("Strong extraction prepares the mode point on the complete joined segment", "[PreciseSeam][SegmentExtraction]")
{
    const auto mode = GENERATE(ModelVolumeType::PRECISE_SEAM_LEFT,
                               ModelVolumeType::PRECISE_SEAM_RIGHT,
                               ModelVolumeType::PRECISE_SEAM_CENTER);
    const bool reverse = GENERATE(false, true);
    const double width = GENERATE(4., 6.);
    Polygon perimeter = rectangle(0, 0, 20, 20);
    if (reverse)
        std::reverse(perimeter.points.begin(), perimeter.points.end());
    // The forward contour crosses vertex zero; Center is either that vertex or inside an edge.
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-2, -2, width, 4))}), mode);
    const bool center = mode == ModelVolumeType::PRECISE_SEAM_CENTER;
    check_provenance(perimeter, result, center);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    CHECK_THAT(segment.length, Catch::Matchers::WithinAbs(center ? scale_(width + 4.) : 0., 1e-6));

    Point expected_point = mm((width - 4.) / 2., 0);
    size_t expected_edge = reverse ? 2 : (width == 4. ? 3 : 0);
    if (mode != ModelVolumeType::PRECISE_SEAM_CENTER) {
        const bool vertical = (mode == ModelVolumeType::PRECISE_SEAM_LEFT) != reverse;
        expected_point = vertical ? mm(0, 4) : mm(width, 0);
        expected_edge = vertical ? 3 : (reverse ? 2 : 0);
    }
    REQUIRE(segment.strong_target.has_value());
    CHECK(segment.strong_target->point == expected_point);
    CHECK(segment.strong_target->edge_index == expected_edge);
}

TEST_CASE("Strong extraction measures competing segments but skips fully contained targets", "[PreciseSeam][SegmentExtraction]")
{
    const auto mode = GENERATE(ModelVolumeType::PRECISE_SEAM_LEFT,
                               ModelVolumeType::PRECISE_SEAM_RIGHT,
                               ModelVolumeType::PRECISE_SEAM_CENTER);
    const bool full = GENERATE(false, true);
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    // Left/Right still require lengths when two segments compete; full coverage skips every mode.
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(full ? rectangle(-2, -2, 22, 22) : rectangle(8, -2, 12, 22))}), mode);
    CHECK(result.full_containment == full);
    REQUIRE(result.segments.size() == (full ? 1 : 2));
    check_provenance(perimeter, result);
    for (const auto &segment : result.segments) {
        CHECK_THAT(segment.length, Catch::Matchers::WithinAbs(full ? 0. : scale_(4.), 1e-6));
        if (full)
            CHECK(segment.polyline.points.front() == segment.polyline.points.back());
        else {
            REQUIRE(segment.strong_target.has_value());
            if (mode == ModelVolumeType::PRECISE_SEAM_LEFT)
                CHECK(segment.strong_target->point == segment.polyline.points.front());
            else if (mode == ModelVolumeType::PRECISE_SEAM_RIGHT)
                CHECK(segment.strong_target->point == segment.polyline.points.back());
        }
    }
}

TEST_CASE("Projection binding follows the same edge and its neighbor in either direction", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    const bool wrap = GENERATE(false, true);
    const Polygon perimeter = rectangle(0, 0, 10, 10);
    Polyline fragment;
    fragment.points = wrap ? Points{mm(0, 3), mm(0, 0), mm(3, 0), mm(6, 0)}
                           : Points{mm(2, 0), mm(4, 0), mm(7, 0), mm(10, 0), mm(10, 3)};
    std::vector<size_t> expected = wrap ? std::vector<size_t>{3, 0, 0} : std::vector<size_t>{0, 0, 0, 1};
    if (reverse) {
        std::reverse(fragment.points.begin(), fragment.points.end());
        std::reverse(expected.begin(), expected.end());
    }
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    PreciseSeam::detail::FragmentBindingFailure failure;
    REQUIRE(PreciseSeam::detail::append_projected_fragment(fragment, perimeter, intervals, failure));
    REQUIRE(intervals.size() == expected.size());
    for (size_t i = 0; i < intervals.size(); ++i) {
        CHECK(intervals[i].edge == expected[i]);
        CHECK(intervals[i].first == fragment.points[reverse ? i + 1 : i]);
        CHECK(intervals[i].last == fragment.points[reverse ? i : i + 1]);
    }
}

TEST_CASE("Projection binding rolls back a fragment that reverses or leaves the contour", "[PreciseSeam][SegmentExtraction]")
{
    const int scenario = GENERATE(0, 1, 2);
    const Polygon perimeter = rectangle(0, 0, 10, 10);
    Polyline fragment;
    fragment.points = {mm(2, 0), mm(7, 0)};
    // Every failure follows a successful pair, exercising rollback rather than an empty result.
    fragment.points.push_back(scenario == 0 ? mm(4, 0) : scenario == 1 ? mm(7, 3) : mm(10, 3));
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals{{2, 0., 1., mm(10, 10), mm(0, 10)}};
    PreciseSeam::detail::FragmentBindingFailure failure;
    CHECK_FALSE(PreciseSeam::detail::append_projected_fragment(fragment, perimeter, intervals, failure));
    REQUIRE(intervals.size() == 1);
    CHECK(intervals[0].edge == 2);
    CHECK(intervals[0].first == mm(10, 10));
    CHECK(intervals[0].last == mm(0, 10));
    CHECK(failure.pair_index == 1);
}

TEST_CASE("Projection binding does not jump to a distant edge at a repeated vertex", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter(Points{mm(0, 0), mm(4, 0), mm(4, 4), mm(0, 0), mm(-4, 0), mm(-4, -4)});
    Polyline fragment;
    // The last pair belongs to edge zero, but edge three is the required continuation.
    fragment.points = {mm(4, 1), mm(4, 4), mm(0, 0), mm(2, 0)};
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    PreciseSeam::detail::FragmentBindingFailure failure;
    CHECK_FALSE(PreciseSeam::detail::append_projected_fragment(fragment, perimeter, intervals, failure));
    CHECK(intervals.empty());
    CHECK(failure.pair_index == 2);
}

TEST_CASE("A rejected intersection warns without removing successful fragments", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 10, 10);
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    Polyline fragment;
    fragment.points = {mm(1, 0), mm(2, 0)};
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    CHECK(warnings.failed_fragments.load() == 0);
    // An out-and-back path must be discarded, not salvaged by clipping source edges again.
    fragment.points = {mm(3, 0), mm(7, 0), mm(4, 0)};
    CHECK_FALSE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 1));
    REQUIRE(intervals.size() == 1);
    CHECK(intervals[0].first == mm(1, 0));
    CHECK(intervals[0].last == mm(2, 0));
    CHECK(warnings.failed_fragments.load() == 1);
    fragment.points = {mm(10, 2), mm(10, 4)};
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 2));
    REQUIRE(intervals.size() == 2);
    CHECK(intervals.back().edge == 1);
}

TEST_CASE("A cut rounded past its adjacent source vertex is snapped instead of discarding the fragment", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    const Point corner = mm(20, 0);
    const bool at_start = GENERATE(false, true);
    const bool reversed = GENERATE(false, true);
    // 1 nm: the cut projects to the corner's parameter, giving a zero-length pair.
    // 5 nm: the cut lies on neither neighbouring edge within clipping precision.
    const coord_t offset = GENERATE(coord_t(1), coord_t(5));
    CAPTURE(at_start, reversed, offset);
    Polyline fragment;
    if (at_start)
        fragment.points = {Point(corner.x() + offset, corner.y()), corner, mm(20, 20), mm(10, 20)};
    else
        fragment.points = {mm(10, 0), corner, Point(corner.x(), corner.y() - offset)};
    if (reversed)
        fragment.reverse();
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    CHECK(warnings.failed_fragments.load() == 0);
    CHECK(warnings.recovered_fragments.load() == 1);
    // The rest of the fragment is kept, and the dropped cut leaves the boundary exactly at the corner.
    // Canonical vertex parameters 0 and 1 are exact by contract, hence a zero margin.
    std::sort(intervals.begin(), intervals.end(), [](const auto &a, const auto &b) { return a.edge < b.edge; });
    if (at_start) {
        REQUIRE(intervals.size() == 2);
        CHECK(intervals[0].edge == 1);
        CHECK_THAT(intervals[0].begin, Catch::Matchers::WithinAbs(0., 0.));
        CHECK_THAT(intervals[0].end, Catch::Matchers::WithinAbs(1., 0.));
        CHECK(intervals[0].first == corner);
        CHECK(intervals[1].edge == 2);
        CHECK_THAT(intervals[1].begin, Catch::Matchers::WithinAbs(0., 0.));
        CHECK_THAT(intervals[1].end, Catch::Matchers::WithinAbs(0.5, 1e-12));
    } else {
        REQUIRE(intervals.size() == 1);
        CHECK(intervals[0].edge == 0);
        CHECK_THAT(intervals[0].begin, Catch::Matchers::WithinAbs(0.5, 1e-12));
        CHECK_THAT(intervals[0].end, Catch::Matchers::WithinAbs(1., 0.));
        CHECK(intervals[0].last == corner);
    }

    // A two-point contact shorter than the snapping distance leaves nothing to bind and is not a failure.
    Polyline contact;
    contact.points = {Point(corner.x() + offset, corner.y()), corner};
    if (reversed)
        contact.reverse();
    intervals.clear();
    CHECK(PreciseSeam::detail::append_fragment(contact, perimeter, intervals, context, 1));
    CHECK(intervals.empty());
    CHECK(warnings.failed_fragments.load() == 0);

    // Beyond the 1 um snapping distance the cleanup does not apply: the fragment is still a failure.
    Polyline distant;
    distant.points = {Point(corner.x() + coord_t(scale_(0.002)), corner.y()), corner, mm(20, 20), mm(10, 20)};
    if (reversed)
        distant.reverse();
    CHECK_FALSE(PreciseSeam::detail::append_fragment(distant, perimeter, intervals, context, 2));
    CHECK(intervals.empty());
    CHECK(warnings.failed_fragments.load() == 1);
}

TEST_CASE("A cut beside the start of its chain's edge is replaced by that vertex only", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    const Point corner = mm(20, 0);
    const bool reversed = GENERATE(false, true);
    CAPTURE(reversed);
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;

    // The cut stands 2 nm beside the corner, off both edges, while the fragment continues from the
    // next vertex (20, 20): the corner shares an edge with that neighbour, so the cut becomes the corner.
    Polyline fragment;
    fragment.points = {Point(corner.x() + 2, corner.y() - 2), mm(20, 20), mm(10, 20)};
    if (reversed)
        fragment.reverse();
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    CHECK(warnings.failed_fragments.load() == 0);
    std::sort(intervals.begin(), intervals.end(), [](const auto &a, const auto &b) { return a.edge < b.edge; });
    REQUIRE(intervals.size() == 2);
    CHECK(intervals[0].edge == 1);
    CHECK_THAT(intervals[0].begin, Catch::Matchers::WithinAbs(0., 0.));
    CHECK_THAT(intervals[0].end, Catch::Matchers::WithinAbs(1., 0.));
    CHECK(intervals[0].first == corner);
    CHECK(intervals[1].edge == 2);

    // The same cut next to a vertex that is not on the fragment's chain is never snapped there:
    // (0, 20) does not share an edge with the corner, so the fragment stays a failure.
    Polyline detached;
    detached.points = {Point(corner.x() + 2, corner.y() - 2), mm(0, 20), mm(0, 10)};
    if (reversed)
        detached.reverse();
    intervals.clear();
    CHECK_FALSE(PreciseSeam::detail::append_fragment(detached, perimeter, intervals, context, 1));
    CHECK(intervals.empty());
    CHECK(warnings.failed_fragments.load() == 1);
}

TEST_CASE("A cut close to its neighbour and to both of the neighbour's chain vertices snaps to the neighbour", "[PreciseSeam][SegmentExtraction]")
{
    // Edges P -> N and N -> Q are 0.5 um long, so a cut 2 nm beside N is within 1 um of P, N and Q.
    // P and Q are ambiguous edge-start candidates, but the cut is a rounded copy of N, which wins.
    const Point n = mm(20, 0);
    const coord_t half_um = coord_t(scale_(0.0005));
    const Point p(n.x() - half_um, n.y());
    const Point q(n.x(), n.y() + half_um);
    const Polygon perimeter(Points{mm(0, 0), p, n, q, mm(20, 20), mm(0, 20)});
    const bool reversed = GENERATE(false, true);
    CAPTURE(reversed);
    Polyline fragment;
    fragment.points = {Point(n.x() + 2, n.y() - 2), n, q, mm(20, 20), mm(10, 20)};
    if (reversed)
        fragment.reverse();
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    CHECK(warnings.failed_fragments.load() == 0);
    CHECK(warnings.recovered_fragments.load() == 1);
    // The dropped cut leaves the boundary exactly at N: edges N -> Q and Q -> (20, 20) are whole.
    std::sort(intervals.begin(), intervals.end(), [](const auto &a, const auto &b) { return a.edge < b.edge; });
    REQUIRE(intervals.size() == 3);
    CHECK(intervals[0].edge == 2);
    CHECK(intervals[0].first == n);
    CHECK_THAT(intervals[0].begin, Catch::Matchers::WithinAbs(0., 0.));
    CHECK_THAT(intervals[0].end, Catch::Matchers::WithinAbs(1., 0.));
    CHECK(intervals[1].edge == 3);
    CHECK_THAT(intervals[1].begin, Catch::Matchers::WithinAbs(0., 0.));
    CHECK_THAT(intervals[1].end, Catch::Matchers::WithinAbs(1., 0.));
    CHECK(intervals[2].edge == 4);
    CHECK_THAT(intervals[2].begin, Catch::Matchers::WithinAbs(0., 0.));
    CHECK_THAT(intervals[2].end, Catch::Matchers::WithinAbs(0.5, 1e-12));
}

TEST_CASE("A fragment end that is itself a source vertex is never treated as a rounded cut", "[PreciseSeam][SegmentExtraction]")
{
    // The closing edge v4 -> v0 is only 0.5 um long. The fragment starts with a cut 2 nm beside v3 (so
    // the fallback runs) and ends at the real vertex v0, the start of the open clipping line. v0 lies
    // within 1 um of its neighbour v4, but it is no rounded cut and must keep the closing edge.
    const Point v3 = mm(0, 20);
    const Point v4(coord_t(0), coord_t(scale_(0.0005)));
    const Polygon perimeter(Points{mm(0, 0), mm(20, 0), mm(20, 20), v3, v4});
    const bool reversed = GENERATE(false, true);
    CAPTURE(reversed);
    Polyline fragment;
    fragment.points = {Point(v3.x() + 2, v3.y() + 2), v4, mm(0, 0)};
    if (reversed)
        fragment.reverse();
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    CHECK(warnings.failed_fragments.load() == 0);
    CHECK(warnings.recovered_fragments.load() == 1);
    // Both whole edges v3 -> v4 and v4 -> v0 are bound; v0 was not dropped.
    std::sort(intervals.begin(), intervals.end(), [](const auto &a, const auto &b) { return a.edge < b.edge; });
    REQUIRE(intervals.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(intervals[i].edge == 3 + i);
        CHECK_THAT(intervals[i].begin, Catch::Matchers::WithinAbs(0., 0.));
        CHECK_THAT(intervals[i].end, Catch::Matchers::WithinAbs(1., 0.));
    }
}

TEST_CASE("A failed fragment shorter than the snapping distance is accepted as a contact", "[PreciseSeam][SegmentExtraction]")
{
    // Two cuts in the middle of a 45-degree edge, 1.4 nm apart and within clipping precision of it,
    // project to the same parameter. No vertex is near, so only the contact rule applies.
    const Polygon diagonal(Points{Point(0, 0), Point(1000000, 1000000), Point(0, 1000000)});
    Polyline contact;
    contact.points = {Point(500000, 500000), Point(500001, 499999)};
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    PreciseSeam::detail::FragmentBindingFailure failure;
    REQUIRE_FALSE(PreciseSeam::detail::bind_fragment(contact, diagonal, intervals, failure));
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    CHECK(PreciseSeam::detail::append_fragment(contact, diagonal, intervals, context, 0));
    CHECK(intervals.empty());
    CHECK(warnings.failed_fragments.load() == 0);
    CHECK(warnings.recovered_fragments.load() == 1);

    // A failed fragment longer than 1 um stays a failure.
    Polyline longer;
    longer.points = {Point(500000, 500000), Point(502000, 498000)};
    CHECK_FALSE(PreciseSeam::detail::append_fragment(longer, diagonal, intervals, context, 1));
    CHECK(warnings.failed_fragments.load() == 1);
}

TEST_CASE("Recovered fragments are counted beyond the diagnostic limit", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    const Point corner = mm(20, 0);
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    // The same recoverable fragment repeats, as it would on every layer of a prismatic model.
    Polyline fragment;
    fragment.points = {Point(corner.x() + 1, corner.y()), corner, mm(20, 20), mm(10, 20)};
    const size_t repeats = PreciseSeam::failed_fragment_log_limit + 2;
    for (size_t i = 0; i < repeats; ++i) {
        std::vector<PreciseSeam::detail::ClippedEdgeInterval> layer;
        CHECK(PreciseSeam::detail::append_fragment(fragment, perimeter, layer, context, i));
        CHECK(layer.size() == 2);
    }
    // Every recovery is counted past the log budget; none is a failure.
    CHECK(warnings.recovered_fragments.load() == repeats);
    CHECK(warnings.failed_fragments.load() == 0);
}

TEST_CASE("Real clipping that rounds a cut beside a vertex is recovered on the fragment's own chain", "[PreciseSeam][SegmentExtraction]")
{
    // Inputs found by a randomized search against Clipper2: random perimeters clipped as open
    // lines against a half-plane whose border passes a few nanometres from a vertex, keeping fragments
    // that fail bind_fragment() but are bound by append_fragment(). Here the border crosses the
    // perimeter at a vertex, and Clipper places the cut at the vertex height but 1-2 nm beside it.
    // If a Clipper change stops producing these cuts, the case only warns that it no longer exercises
    // the fallback; the general extraction checks below remain valid and still apply.
    const auto nm = [](coord_t x, coord_t y) { return Point(x, y); };
    struct Case {
        Points perimeter;
        Points modifier;
        Point cut;
        Point vertex;
    };
    const Case cases[] = {
        // Cut 1 nm beside its neighbour in the fragment: dropped as a rounded copy of that vertex.
        {{nm(-24603214, 112634156), nm(-24432144, 112478995), nm(-24498846, 112431640), nm(-24506812, 112403974),
          nm(-24547188, 111960677), nm(-24140867, 112210640)},
         {nm(-264026532, 293042572), nm(215012909, -68234624), nm(395651507, 171285096), nm(-83387934, 532562293)},
         nm(-24506811, 112403974), nm(-24506812, 112403974)},
        // Cut 2 nm beside the vertex before its neighbour: replaced by that chain vertex.
        {{nm(42606838, 119780952), nm(42638884, 119963549), nm(42573013, 119810910), nm(42578384, 120014121),
          nm(42272085, 119667500), nm(42436134, 119614299), nm(42534337, 119521867), nm(42780062, 119646533)},
         {nm(331854259, 41295522), nm(-247310081, 198039476), nm(-325682058, -91542694), nm(253482282, -248286648)},
         nm(42272083, 119667500), nm(42272085, 119667500)},
    };
    const size_t index = GENERATE(size_t(0), size_t(1));
    CAPTURE(index);
    const Case &c = cases[index];
    const Polygon perimeter(c.perimeter);
    const ExPolygon modifier{Polygon(c.modifier)};
    const PreciseSeam::PreparedPerimeter prepared(perimeter);
    REQUIRE(prepared.valid);

    // The case is relevant only while Clipper still produces the special cut (beside the vertex, not
    // on it) and both regular binding paths still reject that fragment. Otherwise warn instead of
    // failing: the input went stale, while the synthetic tests above still cover the fallback.
    const Polylines fragments = intersection_pl(prepared.line, modifier);
    const auto special = std::find_if(fragments.begin(), fragments.end(), [&c](const Polyline &fragment) {
        return fragment.points.front() == c.cut || fragment.points.back() == c.cut;
    });
    const bool cut_present = special != fragments.end();
    bool reproduces = cut_present;
    if (reproduces) {
        std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
        PreciseSeam::detail::FragmentBindingFailure failure;
        reproduces = !PreciseSeam::detail::bind_fragment(*special, perimeter, intervals, failure);
    }
    if (reproduces) {
        // Deterministic proof that the fallback itself binds this real fragment.
        std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
        PreciseSeam::PreciseSeamWarnings fallback_warnings;
        PreciseSeam::ExtractionContext fallback_context;
        fallback_context.warnings = &fallback_warnings;
        CHECK(PreciseSeam::detail::append_fragment(*special, perimeter, intervals, fallback_context, 0));
        CHECK_FALSE(intervals.empty());
        CHECK(fallback_warnings.recovered_fragments.load() == 1);
        CHECK(fallback_warnings.failed_fragments.load() == 0);
    } else if (!cut_present)
        WARN("Case " << index << ": Clipper no longer returns the rounded cut beside a vertex, so the snapping "
             "fallback is not exercised here. Refresh the inputs by clipping perimeters against half-planes "
             "whose border passes a few nanometers from a vertex.");
    else
        WARN("Case " << index << ": Clipper still returns the rounded cut, but regular binding now accepts it, "
             "so the snapping fallback is not exercised here. Check whether binding changed on purpose.");

    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    const auto result = PreciseSeam::extract_perimeter_segments(
        prepared, PreciseSeam::prepare_modifier_regions({modifier}), ModelVolumeType::PRECISE_SEAM_CENTER, context);
    check_provenance(perimeter, result);
    CHECK(warnings.failed_fragments.load() == 0);
    // Nothing is lost: the extracted coverage equals Clipper's within the snapping distance.
    double clipped = 0., extracted = 0.;
    for (const Polyline &fragment : fragments)
        clipped += fragment.length();
    for (const auto &segment : result.segments)
        extracted += segment.polyline.length();
    CHECK(std::abs(clipped - extracted) < scale_(0.001));
    // While the case reproduces, the recovered boundary lies exactly on the vertex of the fragment's chain.
    if (reproduces) {
        const bool on_vertex = std::any_of(result.segments.begin(), result.segments.end(), [&c](const auto &segment) {
            return segment.polyline.points.front() == c.vertex || segment.polyline.points.back() == c.vertex;
        });
        CHECK(on_vertex);
    }
}

TEST_CASE("Rejected fragments remain counted beyond the diagnostic limit", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 10, 10);
    PreciseSeam::PreciseSeamWarnings warnings;
    PreciseSeam::ExtractionContext context;
    context.warnings = &warnings;
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    Polyline fragment;
    // Keep a valid fragment intact while repeated out-and-back failures exhaust the log budget.
    fragment.points = {mm(1, 0), mm(2, 0)};
    REQUIRE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    fragment.points = {mm(3, 0), mm(7, 0), mm(4, 0)};
    const size_t failures = PreciseSeam::failed_fragment_log_limit + 2;
    for (size_t i = 0; i < failures; ++i)
        CHECK_FALSE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, i + 1));
    CHECK(warnings.failed_fragments.load() == failures);
    REQUIRE(intervals.size() == 1);
    CHECK(intervals.front().first == mm(1, 0));
    CHECK(intervals.front().last == mm(2, 0));

    // Standalone callers still reject safely, and a new processing pass gets its own budget.
    CHECK_FALSE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, {}, 0));
    CHECK(warnings.failed_fragments.load() == failures);
    PreciseSeam::PreciseSeamWarnings next_pass;
    context.warnings = &next_pass;
    CHECK_FALSE(PreciseSeam::detail::append_fragment(fragment, perimeter, intervals, context, 0));
    CHECK(next_pass.failed_fragments.load() == 1);
}

TEST_CASE("A crossing modifier extracts both perimeter intervals without a body chord", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    Polygon perimeter = rectangle(0, 0, 20, 20);
    if (reverse) perimeter.reverse();
    const Points original = perimeter.points;
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(8, -2, 12, 22))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    CHECK(perimeter.points == original);
    REQUIRE(result.segments.size() == 2);
    CHECK_FALSE(result.full_containment);
    std::vector<coord_t> sides;
    for (const auto &segment : result.segments) {
        CHECK_THAT(unscale<double>(segment.length), Catch::Matchers::WithinAbs(4., 1e-6));
        CHECK(segment.polyline.points.front().y() == segment.polyline.points.back().y());
        sides.push_back(segment.polyline.points.front().y());
    }
    std::sort(sides.begin(), sides.end());
    CHECK(sides == std::vector<coord_t>{mm(0, 0).y(), mm(0, 20).y()});
}

TEST_CASE("A corner interval remains connected when the contour origin changes", "[PreciseSeam][SegmentExtraction]")
{
    const size_t origin = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3));
    Polygon perimeter = rectangle(0, 0, 20, 20);
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-2, -2, 4, 4))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    CHECK(segment.polyline.points.front() == mm(0, 4));
    CHECK(segment.polyline.points.back() == mm(4, 0));
    CHECK_THAT(unscale<double>(segment.length), Catch::Matchers::WithinAbs(8., 1e-6));
    CHECK_FALSE(result.full_containment);
}

TEST_CASE("Collinear perimeter vertices retain their original edge provenance", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter(Points{mm(0, 0), mm(2, 0), mm(4, 0), mm(8, 0), mm(20, 0), mm(20, 20), mm(0, 20)});
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(1, -2, 13, 2))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    CHECK(segment.polyline.points.front() == mm(1, 0));
    CHECK(segment.polyline.points.back() == mm(13, 0));
    CHECK(segment.edge_indices == std::vector<size_t>{0, 1, 2, 3});
    CHECK_THAT(unscale<double>(segment.length), Catch::Matchers::WithinAbs(12., 1e-6));
}

TEST_CASE("Interior vertices retain their sequence between two cut endpoints", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    const size_t origin = GENERATE(size_t(0), size_t(5));
    Polygon perimeter;
    // A zigzag makes skipped or misbound interior edges observable in the arc length.
    for (int x = 0; x <= 10; ++x)
        perimeter.points.push_back(mm(x, x % 2));
    perimeter.points.push_back(mm(10, 10));
    perimeter.points.push_back(mm(0, 10));
    if (reverse) perimeter.reverse();
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(2.5, -2, 8.5, 3))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    Points expected{mm(2.5, .5)};
    for (int x = 3; x <= 8; ++x)
        expected.push_back(mm(x, x % 2));
    expected.push_back(mm(8.5, .5));
    if (reverse) std::reverse(expected.begin(), expected.end());
    CHECK(segment.polyline.points == expected);
    CHECK_THAT(unscale<double>(segment.length), Catch::Matchers::WithinAbs(6. * std::sqrt(2.), 1e-6));
}

TEST_CASE("Neighboring vertices distinguish repeated anchors on different lobes", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    // Both lobes visit the origin, but their adjacent edges lead to different vertices.
    Polygon perimeter(Points{mm(0, 0), mm(4, 0), mm(4, 4), mm(0, 4),
                             mm(0, 0), mm(-4, 0), mm(-4, -4), mm(0, -4)});
    if (reverse) perimeter.reverse();
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-1, -5, 1, 5))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 2);
    std::vector<size_t> edges;
    for (const auto &segment : result.segments) {
        CHECK_THAT(unscale<double>(segment.length), Catch::Matchers::WithinAbs(6., 1e-6));
        edges.insert(edges.end(), segment.edge_indices.begin(), segment.edge_indices.end());
    }
    std::sort(edges.begin(), edges.end());
    CHECK(edges == std::vector<size_t>{0, 2, 3, 4, 6, 7});
}

TEST_CASE("Modifier holes subtract coverage while separate components add intervals", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    ExPolygon area(rectangle(1, -3, 10, 3));
    area.holes.push_back(rectangle(4, -1, 7, 1));
    area.holes.back().reverse();
    const auto result = PreciseSeam::extract_perimeter_segments(PreciseSeam::PreparedPerimeter(perimeter),
        PreciseSeam::prepare_modifier_regions({area, ExPolygon(rectangle(14, -3, 18, 3))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 3);
    const std::vector<Point> starts{mm(1, 0), mm(7, 0), mm(14, 0)};
    const std::vector<Point> ends{mm(4, 0), mm(10, 0), mm(18, 0)};
    for (size_t i = 0; i < 3; ++i) {
        CHECK(result.segments[i].polyline.points.front() == starts[i]);
        CHECK(result.segments[i].polyline.points.back() == ends[i]);
    }
    CHECK_FALSE(result.full_containment);
}

TEST_CASE("Full coverage is distinct from an empty or point-only intersection", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    const size_t origin = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3));
    Polygon perimeter = rectangle(0, 0, 20, 20);
    // Coverage depends on traversed edges, not winding or the arbitrary contour origin.
    if (reverse)
        perimeter.reverse();
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    const int scenario = GENERATE(0, 1, 2, 3, 4);
    ExPolygons modifier;
    if (scenario == 0) modifier = {ExPolygon(rectangle(-2, -2, 22, 22))};
    if (scenario == 1) modifier = {ExPolygon(rectangle(2, 2, 4, 4))};
    if (scenario == 2) modifier = {ExPolygon(rectangle(30, 30, 40, 40))};
    if (scenario == 3) modifier = {ExPolygon(rectangle(20, 20, 25, 25))};
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions(modifier), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    CHECK(result.full_containment == (scenario == 0));
    if (scenario == 0) {
        REQUIRE(result.segments.size() == 1);
        CHECK_THAT(unscale<double>(result.segments.front().polyline.length()), Catch::Matchers::WithinAbs(80., 1e-6));
        CHECK(result.segments.front().edge_indices.size() == 4);
    } else
        CHECK(result.segments.empty());
}

TEST_CASE("Full coverage survives a modifier boundary touching the perimeter at one point", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    const size_t origin = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3));
    const int scenario = GENERATE(0, 1, 2);
    CAPTURE(reverse, origin, scenario);
    Polygon perimeter = rectangle(0, 0, 20, 20);
    if (reverse)
        perimeter.reverse();
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    ExPolygon modifier(rectangle(-2, -2, 22, 22));
    if (scenario == 0) {
        // A hole vertex touches the middle of an edge from inside the body.
        modifier.holes.push_back(Polygon(Points{mm(10, 0), mm(12, 2), mm(10, 4), mm(8, 2)}));
        modifier.holes.back().reverse();
    } else if (scenario == 1) {
        // A notch in the exterior touches the same point from outside the body.
        modifier = ExPolygon(Polygon(Points{mm(-2, -2), mm(8, -2), mm(10, 0), mm(12, -2),
                                            mm(22, -2), mm(22, 22), mm(-2, 22)}));
    } else {
        // A hole vertex touches a corner, which is vertex zero for some origins.
        modifier.holes.push_back(Polygon(Points{mm(0, 0), mm(3, 1), mm(1, 3)}));
        modifier.holes.back().reverse();
    }
    // A single contact point leaves no uncovered length, even if clipping splits the line there:
    // the split pieces meet at one source position and merge back into complete edges.
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({modifier}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    CHECK(result.full_containment);
    REQUIRE(result.segments.size() == 1);
    CHECK_THAT(unscale<double>(result.segments.front().polyline.length()), Catch::Matchers::WithinAbs(80., 1e-6));
    CHECK(result.segments.front().edge_indices.size() >= 4);
}

TEST_CASE("A touch poking nanometres through an inclined edge is full containment", "[PreciseSeam][SegmentExtraction]")
{
    // Square of side 20 mm rotated by atan(3/4), integer vertices. A hole tip near the middle of the first
    // side pokes outward along the normal (0.6, -0.8): by about 2.2 nm (a touch the integer grid cannot
    // represent) or 2.2 um (a real gap). Contour origin and winding place the gap inside edge 0, inside
    // the closing edge or in between.
    const bool micro = GENERATE(true, false);
    const bool reverse = GENERATE(false, true);
    const size_t origin = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3));
    CAPTURE(micro, reverse, origin);
    Polygon perimeter(Points{mm(0, 0), mm(16, 12), mm(4, 28), mm(-12, 16)});
    if (reverse)
        perimeter.reverse();
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    const coord_t step = micro ? 1 : 1000;
    const Point tip(mm(8, 6).x() + step, mm(8, 6).y() - 2 * step);
    ExPolygon modifier(rectangle(-20, -10, 25, 40));
    modifier.holes.push_back(Polygon(Points{tip, mm(8.2, 7.4), mm(6.6, 6.2)}));
    modifier.holes.back().reverse();
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({modifier}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.full_containment == micro);
}

TEST_CASE("A gap around one vertex is full containment exactly when both ends snap to it", "[PreciseSeam][SegmentExtraction]")
{
    // A hole bounded by x + y = cut takes the corner (0, 0) off the square. Each end lies `cut` from the
    // vertex and the uncovered length is 2 * cut: below 1 um at 300 nm; 1.4 um at 700 nm, yet both ends
    // still snap onto the vertex on insertion; at 1200 nm neither does.
    const coord_t cut = GENERATE(coord_t(300), coord_t(700), coord_t(1200));
    CAPTURE(cut);
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    ExPolygon modifier(rectangle(-2, -2, 22, 22));
    const coord_t reach = coord_t(scale_(1.));
    modifier.holes.push_back(Polygon(Points{Point(-reach, -reach), Point(reach + cut, -reach), Point(-reach, reach + cut)}));
    modifier.holes.back().reverse();
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({modifier}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.full_containment == (cut < 1000));
}

TEST_CASE("A sub-micron gap that contains a short edge is full containment", "[PreciseSeam][SegmentExtraction]")
{
    // The closing edge v4 -> v0 is 500 nm long. The hole boundary through (100, 0) and (0, 600) leaves
    // 100 + 500 + 100 = 700 nm uncovered across two vertices; the segment does not touch edge 4.
    const Polygon perimeter(Points{mm(0, 0), mm(20, 0), mm(20, 20), mm(0, 20), Point(coord_t(0), coord_t(500))});
    ExPolygon modifier(rectangle(-2, -2, 22, 22));
    modifier.holes.push_back(Polygon(Points{Point(coord_t(100100), coord_t(-600000)), Point(coord_t(-100000), coord_t(600600)),
                                            Point(coord_t(-1000000), coord_t(-1000000))}));
    modifier.holes.back().reverse();
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({modifier}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.full_containment);
}

TEST_CASE("Segments whose ends meet only in space stay ordinary segments", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    const bool slit = GENERATE(false, true);
    CAPTURE(reverse, slit);
    Polygon perimeter;
    ExPolygon modifier;
    if (slit) {
        // A slit 500 nm wide enters the contour from the left; the hole leaves its inner 9 mm uncovered,
        // so the ends face each other across the slit while about 18 mm of perimeter lies between them.
        const coord_t half = 250;
        perimeter = Polygon(Points{mm(0, 0), mm(20, 0), mm(20, 10), mm(0, 10), Point(coord_t(0), mm(0, 5).y() + half),
                                   Point(mm(10, 0).x(), mm(0, 5).y() + half), Point(mm(10, 0).x(), mm(0, 5).y() - half),
                                   Point(coord_t(0), mm(0, 5).y() - half)});
        modifier = ExPolygon(rectangle(-2, -2, 22, 12));
        modifier.holes.push_back(rectangle(1, 4, 11, 6));
        modifier.holes.back().reverse();
    } else {
        // A sharp spike 800 nm wide at its base: cutting off its 1 mm tip brings the ends within about
        // 40 nm while the uncovered tip is about 2 mm long.
        perimeter = Polygon(Points{mm(20, 0), mm(0, 0), Point(coord_t(0), coord_t(800))});
        modifier = ExPolygon(rectangle(-1, -1, 19, 1));
    }
    if (reverse)
        perimeter.reverse();
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({modifier}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    CHECK((segment.polyline.points.front() - segment.polyline.points.back()).cast<double>().norm() < scale_(0.001));
    CHECK_FALSE(result.full_containment);
}

TEST_CASE("Repeated visits to a coordinate stay on their original perimeter edges", "[PreciseSeam][SegmentExtraction]")
{
    // Two visits to the origin belong to different lobes, not to one shared vertex.
    const Polygon perimeter(Points{mm(0, 0), mm(4, 0), mm(4, 4), mm(0, 0), mm(-4, 0), mm(-4, -4)});
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-1, -1, 1, 1))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 2);
    std::vector<size_t> edges;
    for (const auto &segment : result.segments)
        edges.insert(edges.end(), segment.edge_indices.begin(), segment.edge_indices.end());
    std::sort(edges.begin(), edges.end());
    CHECK(edges == std::vector<size_t>{0, 2, 3, 5});
}

TEST_CASE("Two-point fragments accept the first matching perimeter edge", "[PreciseSeam][SegmentExtraction]")
{
    // Policy: do not search for duplicate bindings on overlapping source edges.
    const Polygon perimeter(Points{mm(0, 0), mm(10, 0), mm(0, 0), mm(0, 10)});
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(2, -1, 8, 1))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.segments[0].edge_indices == std::vector<size_t>{0});
    CHECK(result.segments[0].polyline.points.front() == mm(2, 0));
    CHECK(result.segments[0].polyline.points.back() == mm(8, 0));
    CHECK_THAT(unscale<double>(result.segments[0].length), Catch::Matchers::WithinAbs(6., 1e-6));
}

TEST_CASE("Unnormalized perimeter input is reported instead of silently losing coverage", "[PreciseSeam][SegmentExtraction]")
{
    const int scenario = GENERATE(0, 1, 2, 3);
    Polygon perimeter;
    if (scenario == 1) perimeter.points = {mm(0, 0), mm(20, 0)};
    if (scenario == 2) perimeter.points = {mm(0, 0), mm(20, 0), mm(20, 0), mm(0, 20)};
    if (scenario == 3) perimeter.points = {mm(0, 0), mm(20, 0), mm(0, 20), mm(0, 0)};
    // Invalid geometry is rejected once, before allocating its clipping line.
    const PreciseSeam::PreparedPerimeter prepared(perimeter);
    CHECK_FALSE(prepared.valid);
    CHECK(prepared.line.points.empty());
    CHECK_FALSE(prepared.bounds.defined);
    const auto result = PreciseSeam::extract_perimeter_segments(
        prepared, PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-2, -2, 22, 22))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    CHECK_FALSE(result.valid);
    CHECK(result.segments.empty());
    CHECK_FALSE(result.full_containment);
}

TEST_CASE("Segment lengths measure diagonal arcs rather than squared distances", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter(Points{mm(0, 0), mm(10, 10), mm(0, 10)});
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(2, -1, 6, 11))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 2);
    // The cuts lie on the grid, but Clipper computes them on the inclined edge in floating point and
    // may land one unit off; allow one grid step per axis.
    const auto &diagonal = result.segments[0];
    CAPTURE(diagonal.polyline.points.front(), diagonal.polyline.points.back(), diagonal.length);
    CHECK((diagonal.polyline.points.front() - mm(2, 2)).cast<double>().squaredNorm() < 2.);
    CHECK((diagonal.polyline.points.back() - mm(6, 6)).cast<double>().squaredNorm() < 2.);
    CHECK(diagonal.edge_indices == std::vector<size_t>{0});
    CHECK_THAT(unscale<double>(diagonal.length), Catch::Matchers::WithinAbs(std::sqrt(32.), 3e-6));
    CHECK_THAT(unscale<double>(result.segments[1].length), Catch::Matchers::WithinAbs(4., 1e-6));
}

TEST_CASE("An endpoint at an original vertex uses its outgoing edge including vertex zero", "[PreciseSeam][SegmentExtraction]")
{
    const size_t origin = GENERATE(size_t(0), size_t(1));
    Polygon perimeter(Points{mm(0, 0), mm(5, 5), mm(0, 10)});
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-1, -1, 6, 5))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    CHECK(segment.polyline.points.back() == mm(5, 5));
    CHECK(segment.end.edge_index == 1 - origin);
    CHECK_THAT(segment.end.parameter, Catch::Matchers::WithinAbs(0., 1e-12));
}

TEST_CASE("A one-unit uncovered gap is not bridged by the projection tolerance", "[PreciseSeam][SegmentExtraction]")
{
    // These are scaled integer units, deliberately smaller than the projection tolerance.
    const Polygon perimeter(Points{Point(0, 0), Point(20, 0), Point(20, 20), Point(0, 20)});
    const ExPolygons modifier{
        ExPolygon(Polygon(Points{Point(3, -2), Point(8, -2), Point(8, 2), Point(3, 2)})),
        ExPolygon(Polygon(Points{Point(9, -2), Point(14, -2), Point(14, 2), Point(9, 2)}))};
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions(modifier), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 2);
    CHECK(result.segments[0].polyline.points.back() == Point(8, 0));
    CHECK(result.segments[1].polyline.points.front() == Point(9, 0));
}

TEST_CASE("Distant modifier areas do not change nearby coverage or detach its holes", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse_areas = GENERATE(false, true);
    const Polygon perimeter = rectangle(0, 0, 20, 20);
    ExPolygon nearby(rectangle(1, -3, 10, 3));
    nearby.holes.push_back(rectangle(4, -1, 7, 1));
    nearby.holes.back().reverse();
    // A rejected area keeps its own hole; neither may affect the nearby area.
    ExPolygon distant(rectangle(100, 100, 120, 120));
    distant.holes.push_back(rectangle(105, 105, 115, 115));
    distant.holes.back().reverse();
    ExPolygons modifier{distant, nearby, ExPolygon(rectangle(14, -3, 18, 3))};
    if (reverse_areas) std::reverse(modifier.begin(), modifier.end());
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions(modifier), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 3);
    const Points starts{mm(1, 0), mm(7, 0), mm(14, 0)};
    const Points ends{mm(4, 0), mm(10, 0), mm(18, 0)};
    for (size_t i = 0; i < starts.size(); ++i) {
        CHECK(result.segments[i].polyline.points.front() == starts[i]);
        CHECK(result.segments[i].polyline.points.back() == ends[i]);
        CHECK(result.segments[i].edge_indices == std::vector<size_t>{0});
    }
    CHECK_FALSE(result.full_containment);
}

TEST_CASE("Rounded intersections on an inclined edge retain their original edge", "[PreciseSeam][SegmentExtraction]")
{
    const bool reverse = GENERATE(false, true);
    // The exact cuts (3, 0.9) and (7, 2.1) are off the integer grid, so Clipper moves them to a nearby
    // grid point; how it rounds is the library's business. The far vertices keep the contour
    // realistically long: a contour shorter than 1 um would be below the snapping distance as a whole,
    // and its remaining uncovered part would count as full containment.
    Polygon perimeter(Points{Point(0, 0), Point(10, 3), Point(coord_t(10), mm(0, 20).y()), Point(coord_t(0), mm(0, 20).y())});
    if (reverse) perimeter.reverse();
    const ExPolygon area(Polygon(Points{Point(3, -2), Point(7, -2), Point(7, 5), Point(3, 5)}));
    const auto result = PreciseSeam::extract_perimeter_segments(
        PreciseSeam::PreparedPerimeter(perimeter), PreciseSeam::prepare_modifier_regions({area}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const auto &segment = result.segments.front();
    // The rounded cuts must stay bound to the original inclined edge, within one grid step of the exact
    // cuts on each axis, whatever rounding the clipping library uses.
    const Vec2d first = reverse ? Vec2d(7., 2.1) : Vec2d(3., 0.9);
    const Vec2d last = reverse ? Vec2d(3., 0.9) : Vec2d(7., 2.1);
    CAPTURE(segment.polyline.points.front(), segment.polyline.points.back(), segment.length);
    CHECK((segment.polyline.points.front().cast<double>() - first).squaredNorm() < 2.);
    CHECK((segment.polyline.points.back().cast<double>() - last).squaredNorm() < 2.);
    CHECK(segment.edge_indices == std::vector<size_t>{reverse ? size_t(2) : size_t(0)});
    CHECK_THAT(segment.length, Catch::Matchers::WithinAbs((last - first).norm(), 2. * std::sqrt(2.)));
    CHECK_FALSE(result.full_containment);
}

namespace {
// 0.2 mm, the shift of a 0.4 mm line without margin or compensation; keeps the expected points round.
const coord_t surface_distance = coord_t(scale_(0.2));

// The L-shaped contour has its only concave vertex at (10, 10): edge 2 arrives along y = 10, edge 3 leaves
// along x = 10, and their shifted copies cross at (10.2, 10.2).
Polygon l_shape()
{
    return Polygon(Points{mm(0, 0), mm(20, 0), mm(20, 10), mm(10, 10), mm(10, 20), mm(0, 20)});
}

PreciseSeam::SegmentExtraction surface_extraction(const Polygon &perimeter, ExPolygons regions, bool hole = false,
                                                  ModelVolumeType mode = ModelVolumeType::PRECISE_SEAM_CENTER,
                                                  coord_t shift = surface_distance)
{
    const PreciseSeam::PreparedPerimeter prepared(perimeter, shift, hole);
    return PreciseSeam::extract_perimeter_segments(prepared, PreciseSeam::prepare_modifier_regions(std::move(regions)), mode);
}

PreciseSeam::SegmentExtraction surface_extraction(const Polygon &perimeter, const Polygon &region, bool hole = false,
                                                  ModelVolumeType mode = ModelVolumeType::PRECISE_SEAM_CENTER,
                                                  coord_t shift = surface_distance)
{
    return surface_extraction(perimeter, ExPolygons{ExPolygon(region)}, hole, mode, shift);
}

bool contains_point(const Polyline &polyline, const Point &point)
{
    return std::find(polyline.points.begin(), polyline.points.end(), point) != polyline.points.end();
}
} // namespace

TEST_CASE("The surface line shifts edges towards air, follows convex corners and bridges concave ones", "[PreciseSeam][SegmentExtraction]")
{
    // A convex contour has no dead stretches; the clipping line closes the polygon at its start.
    const Polygon square = rectangle(0, 0, 20, 20);
    const auto convex = PreciseSeam::detail::build_surface_line(square, surface_distance, false);
    REQUIRE(convex.shifts.size() == 4);
    CHECK(convex.shifts[0] == Point(0, -surface_distance));
    CHECK(convex.shifts[1] == Point(surface_distance, 0));
    REQUIRE(convex.elements.size() == convex.polygon.size());
    size_t arc_chords = 0;
    for (size_t i = 0; i < convex.polygon.size(); ++i) {
        CHECK(convex.polygon.points[i] != convex.polygon.points[(i + 1) % convex.polygon.size()]);
        arc_chords += convex.elements[i].vertex ? 1 : 0;
    }
    CHECK(arc_chords > 4); // Every corner is an arc of several chords.
    REQUIRE(convex.line.size() == convex.polygon.size() + 1);
    CHECK(convex.line.first_point() == convex.line.last_point());
    CHECK(convex.gaps.empty());

    // One concave corner leaves one dead stretch whose ends meet near the crossing of the shifted edges.
    const auto concave = PreciseSeam::detail::build_surface_line(l_shape(), surface_distance, false);
    REQUIRE(concave.gaps.size() == 1);
    CHECK((concave.gaps.front().end - mm(10.2, 10.2)).cast<double>().norm() < scale_(0.001));
    CHECK((concave.gaps.front().start - mm(10.2, 10.2)).cast<double>().norm() < scale_(0.001));

    // The wall of a hole is shifted into the hole, where its corners are concave: the shifted edges end at their
    // crossings, with a dead stretch at every corner.
    const auto hole = PreciseSeam::detail::build_surface_line(rectangle(0, 0, 10, 10), surface_distance, true);
    CHECK(hole.shifts[0] == Point(0, surface_distance));
    CHECK(hole.gaps.size() == 4);
    CHECK(hole.polygon.points == Points{mm(0.2, 0.2), mm(9.8, 0.2), mm(9.8, 9.8), mm(0.2, 9.8)});
    CHECK(hole.bounds.defined);
}

TEST_CASE("A modifier crossing the surface acts without reaching the centerline", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // 0.1 mm past the surface, 0.1 mm short of the centerline.
    const Polygon shallow = rectangle(8, -1, 12, -0.1);
    CHECK(surface_extraction(square, shallow, false, ModelVolumeType::PRECISE_SEAM_CENTER, 0).segments.empty());
    const auto result = surface_extraction(square, shallow);
    check_provenance(square, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.segments.front().polyline.points.front() == mm(8, 0));
    CHECK(result.segments.front().polyline.points.back() == mm(12, 0));
    CHECK_FALSE(result.full_containment);

    // An inclined side maps along the normal: it crosses the surface y = -0.2 at x = 7 + 0.8 / 0.9.
    const Polygon inclined(Points{mm(7, -1), mm(12, -1), mm(12, -0.1), mm(8, -0.1)});
    const auto slanted = surface_extraction(square, inclined);
    check_provenance(square, slanted);
    REQUIRE(slanted.segments.size() == 1);
    CHECK_THAT(unscale<double>(slanted.segments.front().polyline.points.front().x()),
               Catch::Matchers::WithinAbs(7. + 0.8 / 0.9, 1e-5));
    CHECK(slanted.segments.front().polyline.points.front().y() == 0);
}

TEST_CASE("A modifier that does not cross the surface has no effect even across the centerline", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // Stops 0.1 mm short of the surface.
    const auto short_of_surface = surface_extraction(square, rectangle(8, -1, 12, -0.3));
    CHECK(short_of_surface.segments.empty());
    CHECK_FALSE(short_of_surface.full_containment);
    // Lies inside the body: crosses the centerline but never reaches the surface.
    const auto inside = surface_extraction(square, rectangle(8, -0.15, 12, 0.5));
    CHECK(inside.segments.empty());
    CHECK_FALSE(inside.full_containment);
    CHECK(inside.discarded_fragments == 0);
}

TEST_CASE("Covering only the arc of a convex corner gives a zero-length segment at its vertex", "[PreciseSeam][SegmentExtraction]")
{
    const auto mode = GENERATE(ModelVolumeType::PRECISE_SEAM_LEFT, ModelVolumeType::PRECISE_SEAM_CENTER,
                               ModelVolumeType::PRECISE_SEAM_RIGHT, ModelVolumeType::PRECISE_SEAM_ENFORCED);
    const Polygon square = rectangle(0, 0, 20, 20);
    // Around the arc point at -45 degrees from (20, 0), clear of both shifted edges.
    const auto arc = surface_extraction(square, rectangle(20.11, -0.17, 20.17, -0.11), false, mode);
    REQUIRE(arc.valid);
    CHECK(arc.discarded_fragments == 0);
    REQUIRE(arc.segments.size() == 1);
    const auto &point = arc.segments.front();
    CHECK(point.edge_indices.empty());
    CHECK(point.polyline.points == Points{mm(20, 0)});
    CHECK(point.begin.edge_index == 1);
    CHECK(point.begin.parameter == 0.);
    CHECK(point.end.edge_index == 1);
    CHECK(point.end.parameter == 0.);
    CHECK_FALSE(arc.full_containment);
    if (is_precise_seam_strong(mode)) {
        REQUIRE(point.strong_target.has_value());
        CHECK(point.strong_target->point == mm(20, 0));
        CHECK(point.strong_target->edge_index == 1);
    } else
        CHECK_FALSE(point.strong_target.has_value());

    // The arc together with part of the next shifted edge is an ordinary segment starting at the vertex.
    const auto longer = surface_extraction(square, rectangle(20.11, -0.17, 20.5, 3), false, mode);
    // Only strong modes prepare the targets the provenance check expects; only Center measures one segment.
    if (is_precise_seam_strong(mode))
        check_provenance(square, longer, mode == ModelVolumeType::PRECISE_SEAM_CENTER);
    REQUIRE(longer.segments.size() == 1);
    CHECK(longer.segments.front().polyline.points.front() == mm(20, 0));
    CHECK(longer.segments.front().polyline.points.back() == mm(20, 3));
    CHECK(longer.segments.front().begin.edge_index == 1);
    CHECK(longer.segments.front().begin.parameter == 0.);
}

TEST_CASE("Covering the crossing of a concave corner covers its dead stretch of the centerline", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = l_shape();
    // The crossing (10.2, 10.2) inside: one segment through the vertex.
    const auto across = surface_extraction(perimeter, rectangle(10.1, 10.1, 10.4, 10.4));
    check_provenance(perimeter, across);
    REQUIRE(across.segments.size() == 1);
    CHECK(across.segments.front().polyline.points == Points{mm(10.4, 10), mm(10, 10), mm(10, 10.4)});
    CHECK(across.segments.front().edge_indices == std::vector<size_t>{2, 3});

    // On one side only: the segment ends under the modifier, outside the dead stretch.
    const auto one_side = surface_extraction(perimeter, rectangle(10.5, 10.1, 11, 10.4));
    check_provenance(perimeter, one_side);
    REQUIRE(one_side.segments.size() == 1);
    CHECK(one_side.segments.front().polyline.points == Points{mm(11, 10), mm(10.5, 10)});

    // Within the shift of both walls but clear of the surface: across the bridge, and across the tail
    // of a shifted edge past the crossing.
    CHECK(surface_extraction(perimeter, rectangle(10.02, 10.02, 10.15, 10.15)).segments.empty());
    CHECK(surface_extraction(perimeter, rectangle(10.05, 10.05, 10.15, 10.3)).segments.empty());
}

TEST_CASE("The surface of a hole lies inside the hole", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon perimeter = rectangle(0, 0, 10, 10);
    const Polygon inside_hole = rectangle(4, 0.1, 6, 1);
    const auto hole = surface_extraction(perimeter, inside_hole, true);
    check_provenance(perimeter, hole);
    REQUIRE(hole.segments.size() == 1);
    CHECK(hole.segments.front().polyline.points == Points{mm(4, 0), mm(6, 0)});
    // The same modifier is inside the body of a contour.
    CHECK(surface_extraction(perimeter, inside_hole, false).segments.empty());
    // The corners of a hole are concave: covering the crossing (9.8, 0.2) covers the corner of the centerline.
    const auto corner = surface_extraction(perimeter, rectangle(9.5, 0.1, 9.9, 0.5), true);
    check_provenance(perimeter, corner);
    REQUIRE(corner.segments.size() == 1);
    CHECK(corner.segments.front().polyline.points == Points{mm(9.5, 0), mm(10, 0), mm(10, 0.5)});
}

TEST_CASE("A modifier touching the surface at one point has no effect", "[PreciseSeam][SegmentExtraction]")
{
    // Counter-clockwise diamond whose top vertex lies exactly on the surface.
    const Polygon diamond(Points{mm(10, -0.2), mm(9.5, -0.7), mm(10, -1.2), mm(10.5, -0.7)});
    const auto result = surface_extraction(rectangle(0, 0, 20, 20), diamond);
    CHECK(result.segments.empty());
    CHECK_FALSE(result.full_containment);
}

TEST_CASE("A modifier face flush with the model surface stays clear of the reconstructed surface", "[PreciseSeam][SegmentExtraction]")
{
    // The model surface lies half a 0.4 mm line outside the centerline; the margin keeps the
    // reconstructed surface inside it.
    const coord_t shift = PreciseSeam::surface_shift(0.4, 0.);
    REQUIRE(shift > 0);
    REQUIRE(shift < surface_distance);
    CHECK(surface_extraction(rectangle(0, 0, 20, 20), rectangle(8, -1, 12, -0.2), false,
                             ModelVolumeType::PRECISE_SEAM_CENTER, shift).segments.empty());

    // Inclined wall x + y = 20, flush face 0.2 mm outside it.
    const Polygon triangle(Points{mm(0, 0), mm(20, 0), mm(0, 20)});
    const Vec2d normal = Vec2d(1., 1.).normalized();
    const auto at = [](const Vec2d &p) { return mm(p.x(), p.y()); };
    const Vec2d a = Vec2d(11., 9.) + 0.2 * normal;
    const Vec2d b = Vec2d(9., 11.) + 0.2 * normal;
    Polygon flush(Points{at(a), at(a + 2. * normal), at(b + 2. * normal), at(b)});
    flush.make_counter_clockwise();
    CHECK(surface_extraction(triangle, flush, false, ModelVolumeType::PRECISE_SEAM_CENTER, shift).segments.empty());

    // A tessellated cylinder inside a modifier whose hole is the model surface.
    Polygon circle;
    for (int i = 0; i < 360; ++i)
        circle.points.push_back(mm(10. * std::cos(i * PI / 180.), 10. * std::sin(i * PI / 180.)));
    const auto around = [&](double distance) {
        Polygons model_surface = offset(circle, float(scale_(distance)), jtRound, 10.);
        REQUIRE(model_surface.size() == 1);
        ExPolygon region(rectangle(-20, -20, 20, 20));
        region.holes.push_back(model_surface.front());
        region.holes.back().make_clockwise();
        return surface_extraction(circle, ExPolygons{region}, false, ModelVolumeType::PRECISE_SEAM_CENTER, shift);
    };
    const auto flush_ring = around(0.2);
    CHECK(flush_ring.segments.empty());
    CHECK_FALSE(flush_ring.full_containment);
    // 10 um deeper the modifier covers the whole surface.
    CHECK(around(0.19).full_containment);
}

TEST_CASE("The surface shift subtracts compensation and margin and drops within the snapping radius", "[PreciseSeam][SegmentExtraction]")
{
    // d = w/2 - c - margin, kept only above the snapping radius; the margin is 5000 units and the snapping radius
    // 1000 units. The line width is 0.4 mm throughout.
    const auto expected = [](double distance) { return coord_t(std::lround(scale_(distance))) - 5000; };
    // Positive compensation moves the wall towards air, closer to the model surface, so it shortens the shift.
    CHECK(PreciseSeam::surface_shift(0.4, 0.) == expected(0.2));
    CHECK(PreciseSeam::surface_shift(0.4, 0.1) == expected(0.1));
    CHECK(PreciseSeam::surface_shift(0.4, -0.1) == expected(0.3));
    // Two units either side of the snapping radius.
    CHECK(PreciseSeam::surface_shift(0.4, 0.2 - 6002 * SCALING_FACTOR) == 1002);
    CHECK(PreciseSeam::surface_shift(0.4, 0.2 - 5998 * SCALING_FACTOR) == 0);
    // Compensation beyond half the width would put the surface on the body side: no shift, not a negative one.
    CHECK(PreciseSeam::surface_shift(0.4, 0.3) == 0);
}

TEST_CASE("Full containment means the whole surface, not the whole centerline", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    CHECK(surface_extraction(square, rectangle(-1, -1, 21, 21)).full_containment);
    // Covers the centerline all round but the surface only under the bump on the front face.
    const ExPolygons partial = union_ex(Polygons{rectangle(-0.1, -0.1, 20.1, 20.1), rectangle(8, -1, 12, 0)});
    const auto result = surface_extraction(square, partial);
    CHECK_FALSE(result.full_containment);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.segments.front().polyline.points == Points{mm(8, 0), mm(12, 0)});
}

TEST_CASE("A concave fillet finer than the shift binds without failures", "[PreciseSeam][SegmentExtraction]")
{
    // The concave corner of the L shape rounded with radius 0.1 mm in 20 edges: none of them is resolved, so the
    // surface line keeps their shifted edges and bridges, crossing near the corner.
    Points points{mm(0, 0), mm(20, 0), mm(20, 10)};
    for (int i = 0; i <= 20; ++i) {
        const double angle = 0.5 * PI * i / 20.;
        points.push_back(mm(10.1 - 0.1 * std::sin(angle), 10.1 - 0.1 * std::cos(angle)));
    }
    points.insert(points.end(), {mm(10, 20), mm(0, 20)});
    const Polygon perimeter(points);
    // A modifier covering the whole corner, tails and bridges of the surface line included.
    const auto across = surface_extraction(perimeter, rectangle(9.9, 9.9, 10.4, 10.4));
    check_provenance(perimeter, across);
    REQUIRE(across.segments.size() == 1);
    CHECK(across.segments.front().polyline.points.front() == mm(10.4, 10));
    CHECK(across.segments.front().polyline.points.back() == mm(10, 10.4));
    // Every fillet vertex lies in the covered dead stretch.
    for (size_t i = 3; i <= 23; ++i)
        CHECK(contains_point(across.segments.front().polyline, perimeter.points[i]));
    const auto one_side = surface_extraction(perimeter, rectangle(10.5, 10.1, 11, 10.4));
    check_provenance(perimeter, one_side);
    REQUIRE(one_side.segments.size() == 1);
    CHECK(one_side.segments.front().polyline.points == Points{mm(11, 10), mm(10.5, 10)});
}

TEST_CASE("Surface extraction does not depend on the contour origin or direction", "[PreciseSeam][SegmentExtraction]")
{
    const int origin = GENERATE(0, 1, 2, 3, 4, 5);
    const bool reverse = GENERATE(false, true);
    CAPTURE(origin, reverse);
    Polygon perimeter = l_shape();
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    if (reverse)
        perimeter.reverse();
    // Across the concave corner, and around the convex corner (20, 0) as one segment through its vertex.
    const struct {
        Polygon region;
        Point first;
        Point last;
    } cases[] = {{rectangle(10.1, 10.1, 10.4, 10.4), mm(10.4, 10), mm(10, 10.4)},
                 {rectangle(19, -1, 21, 1), mm(19, 0), mm(20, 1)}};
    for (const auto &expected : cases) {
        const auto result = surface_extraction(perimeter, expected.region);
        check_provenance(perimeter, result);
        REQUIRE(result.segments.size() == 1);
        const Polyline &polyline = result.segments.front().polyline;
        CHECK(polyline.points.front() == (reverse ? expected.last : expected.first));
        CHECK(polyline.points.back() == (reverse ? expected.first : expected.last));
        CHECK(polyline.size() == 3);
    }
}

namespace {
// Concave circular wall of `count` vertices per full turn with a vertex at `vertex`, its centre on the air side:
// a hole of the given radius, or a notch of that radius cut into the right side of an 80 mm square.
Polygon concave_wall(double radius, int count, bool hole, Point &vertex)
{
    Polygon polygon;
    if (hole) {
        for (int i = 0; i < count; ++i)
            polygon.points.push_back(mm(radius * std::cos(2. * PI * i / count), radius * std::sin(2. * PI * i / count)));
        vertex = mm(radius, 0);
        return polygon;
    }
    polygon.points = {mm(-40, -40), mm(40, -40)};
    // The notch runs clockwise around its centre (40, 0), from angle -90 through 180 degrees to 90.
    for (int j = 0; j <= count / 2; ++j) {
        const double angle = -0.5 * PI - 2. * PI * j / count;
        polygon.points.push_back(mm(40. + radius * std::cos(angle), radius * std::sin(angle)));
    }
    polygon.points.insert(polygon.points.end(), {mm(40, 40), mm(-40, 40)});
    vertex = mm(40. - radius, 0);
    return polygon;
}
} // namespace

TEST_CASE("A boundary swept across a shallow concave vertex moves the single segment end monotonically", "[PreciseSeam][SegmentExtraction]")
{
    const int count = GENERATE(120, 180, 360, 720);
    const double radius = GENERATE(2., 10.);
    const bool hole = GENERATE(true, false);
    CAPTURE(count, radius, hole);
    Point vertex;
    const Polygon perimeter = concave_wall(radius, count, hole, vertex);
    const PreciseSeam::PreparedPerimeter prepared(perimeter, surface_distance, hole);
    const double x = unscale<double>(vertex.x());
    // The wall runs up (+y) through the vertex in both cases; the crossing of the shifted edges lies at y = 0.
    // The modifier's top face sweeps from 6 um below it to 6 um above in 0.2 um steps.
    double previous = -1.;
    for (int step = -30; step <= 30; ++step) {
        CAPTURE(step);
        const auto regions = PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(x - 0.5, -0.5, x + 0.5, step * 0.0002))});
        const auto result = PreciseSeam::extract_perimeter_segments(prepared, regions, ModelVolumeType::PRECISE_SEAM_CENTER);
        CHECK(result.discarded_fragments == 0);
        REQUIRE(result.segments.size() == 1);
        // The segment starts at the fixed bottom face, so its length follows the top face along the wall.
        const double length = result.segments.front().length;
        CHECK(length >= previous - 2.);
        previous = length;
        if (step == 30)
            CHECK(contains_point(result.segments.front().polyline, vertex));
    }
}

TEST_CASE("Covering an arc and the end of the previous shifted edge ends the segment at the vertex", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // The front shifted edge from x = 17 and the arc around (20, 0) up to about -33 degrees.
    const auto result = surface_extraction(square, rectangle(17, -0.5, 20.17, -0.11));
    check_provenance(square, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.segments.front().polyline.points == Points{mm(17, 0), mm(20, 0)});
    CHECK(result.segments.front().end.edge_index == 1);
    CHECK(result.segments.front().end.parameter == 0.);
}

TEST_CASE("A vertex point at vertex zero forms or closes a segment across the contour origin", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // Only the arc around vertex zero (0, 0), at 225 degrees.
    const auto point = surface_extraction(square, rectangle(-0.17, -0.17, -0.11, -0.11));
    REQUIRE(point.segments.size() == 1);
    CHECK(point.segments.front().polyline.points == Points{mm(0, 0)});
    CHECK(point.segments.front().begin.edge_index == 0);
    CHECK(point.segments.front().edge_indices.empty());
    CHECK_FALSE(point.full_containment);
    // The left shifted edge down to the vertex and part of its arc: the vertex point heads the sorted intervals,
    // and the tail on the closing edge joins it.
    const auto joined = surface_extraction(square, rectangle(-0.5, -0.17, -0.11, 3));
    check_provenance(square, joined);
    REQUIRE(joined.segments.size() == 1);
    CHECK(joined.segments.front().polyline.points == Points{mm(0, 3), mm(0, 0)});
    CHECK(joined.segments.front().edge_indices == std::vector<size_t>{3});
    CHECK(joined.segments.front().end.edge_index == 0);
    CHECK(joined.segments.front().end.parameter == 0.);
}

TEST_CASE("A vertex point on a hole does not depend on the contour origin or direction", "[PreciseSeam][SegmentExtraction]")
{
    const int origin = GENERATE(0, 1, 2, 3, 4, 5);
    const bool reverse = GENERATE(false, true);
    CAPTURE(origin, reverse);
    Polygon perimeter = l_shape();
    std::rotate(perimeter.points.begin(), perimeter.points.begin() + origin, perimeter.points.end());
    if (reverse)
        perimeter.reverse(); // A hole as the loop gives it, clockwise.
    // As a hole, the L has air inside: its reflex vertex (10, 10) is convex from the air, wrapped by an arc
    // of radius 0.2 mm into the hole. The modifier covers only that arc, around its point at 225 degrees.
    const auto result = surface_extraction(perimeter, rectangle(9.83, 9.83, 9.89, 9.89), true);
    REQUIRE(result.valid);
    CHECK(result.discarded_fragments == 0);
    CHECK_FALSE(result.full_containment);
    REQUIRE(result.segments.size() == 1);
    const auto &point = result.segments.front();
    CHECK(point.polyline.points == Points{mm(10, 10)});
    CHECK(point.edge_indices.empty());
    // The position is the vertex's outgoing edge, wherever the rotation and reversal moved it.
    const size_t vertex = size_t(std::find(perimeter.points.begin(), perimeter.points.end(), mm(10, 10)) - perimeter.points.begin());
    REQUIRE(vertex < perimeter.size());
    CHECK(point.begin.edge_index == vertex);
    CHECK(point.begin.parameter == 0.);
    CHECK(point.end.edge_index == vertex);
    CHECK(point.end.parameter == 0.);
    REQUIRE(point.strong_target.has_value());
    CHECK(point.strong_target->point == mm(10, 10));
}

TEST_CASE("Leaving part of one arc uncovered is still full containment", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // The modifier covers everything but a small hole on the arc around (20, 0), clear of both shifted edges.
    ExPolygon region(rectangle(-1, -1, 21, 21));
    region.holes.push_back(rectangle(20.12, -0.16, 20.16, -0.12));
    region.holes.back().reverse();
    const auto result = surface_extraction(square, ExPolygons{region});
    CHECK(result.full_containment);
    CHECK(result.discarded_fragments == 0);
}

TEST_CASE("A reversal of direction is wrapped by a half-circle arc", "[PreciseSeam][SegmentExtraction]")
{
    // A needle from (20, 5) to the tip (26, 5) and back: the surface runs along both sides and round the tip.
    const Polygon needle(Points{mm(0, 0), mm(20, 0), mm(20, 5), mm(26, 5), mm(20, 5), mm(20, 20), mm(0, 20)});
    const auto tip = surface_extraction(needle, rectangle(26.1, 4, 27, 6));
    CHECK(tip.discarded_fragments == 0);
    REQUIRE(tip.segments.size() == 1);
    CHECK(tip.segments.front().polyline.points == Points{mm(26, 5)});
    const auto around = surface_extraction(needle, rectangle(25, 4, 27, 6));
    check_provenance(needle, around);
    REQUIRE(around.segments.size() == 1);
    CHECK(around.segments.front().polyline.points == Points{mm(25, 5), mm(26, 5), mm(25, 5)});
}

TEST_CASE("Nearly collinear vertices whose rounded shifts differ by one unit bind without failures", "[PreciseSeam][SegmentExtraction]")
{
    // The second edge rises (or drops) 30 units over 10 mm: 0.6 units of its 0.2 mm shift point along x, so
    // the rounded shifts of the two edges differ by one unit; the vertex is convex or concave.
    const coord_t rise = GENERATE(30, -30);
    CAPTURE(rise);
    const Polygon perimeter(Points{mm(0, 0), mm(10, 0), Point(mm(20, 0).x(), rise), mm(20, 20), mm(0, 20)});
    const auto surface = PreciseSeam::detail::build_surface_line(perimeter, surface_distance, false);
    CHECK(surface.shifts[0] != surface.shifts[1]);
    const auto result = surface_extraction(perimeter, rectangle(9, -1, 11, -0.1));
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    CHECK(result.segments.front().polyline.points.front() == mm(9, 0));
    CHECK(contains_point(result.segments.front().polyline, mm(10, 0)));
}

TEST_CASE("Covering the centerline between two positions never goes round the whole loop", "[PreciseSeam][SegmentExtraction]")
{
    using PreciseSeam::detail::CenterlinePoint;
    const Polygon square = rectangle(0, 0, 20, 20);
    std::vector<PreciseSeam::detail::ClippedEdgeInterval> intervals;
    // Backwards on one edge: nothing.
    PreciseSeam::detail::cover_centerline(square, CenterlinePoint{2, 0.6, mm(12, 20)}, CenterlinePoint{2, 0.3, mm(14, 20)}, intervals);
    CHECK(intervals.empty());
    // Forwards on one edge, and across the closing edge to the next one.
    PreciseSeam::detail::cover_centerline(square, CenterlinePoint{2, 0.3, mm(14, 20)}, CenterlinePoint{2, 0.6, mm(8, 20)}, intervals);
    REQUIRE(intervals.size() == 1);
    CHECK(intervals[0].begin == 0.3);
    CHECK(intervals[0].end == 0.6);
    intervals.clear();
    PreciseSeam::detail::cover_centerline(square, CenterlinePoint{3, 0.5, mm(0, 10)}, CenterlinePoint{1, 0.5, mm(20, 10)}, intervals);
    REQUIRE(intervals.size() == 3);
    CHECK(intervals[0].edge == 3);
    CHECK(intervals[0].last == mm(0, 0));
    CHECK(intervals[1].edge == 0);
    CHECK(intervals[2].edge == 1);
    CHECK(intervals[2].end == 0.5);
}

namespace {
// A profile with the given shift in mm at each vertex and no interior samples.
PreciseSeam::ShiftProfile vertex_profile(std::initializer_list<double> shifts)
{
    PreciseSeam::ShiftProfile profile;
    for (const double shift : shifts)
        profile.vertices.push_back(coord_t(scale_(shift)));
    return profile;
}

bool within_unit(const Point &a, const Point &b) { return (a - b).cast<double>().squaredNorm() <= 2.; }

bool contains_near(const Polygon &polygon, const Point &point)
{
    return std::any_of(polygon.points.begin(), polygon.points.end(), [&point](const Point &p) { return within_unit(p, point); });
}
} // namespace

TEST_CASE("A variable shift moves every vertex by its own distance and keeps the line continuous", "[PreciseSeam][SegmentExtraction]")
{
    // The front edge goes from 0.1 mm below (0, 0) to 0.3 mm below (20, 0); the corner (20, 20) has no shift.
    const Polygon square = rectangle(0, 0, 20, 20);
    const auto surface = PreciseSeam::detail::build_surface_line(square, vertex_profile({0.1, 0.3, 0., 0.2}), false);
    REQUIRE(surface.elements.size() == surface.polygon.size());
    CHECK(contains_near(surface.polygon, mm(0, -0.1)));
    CHECK(contains_near(surface.polygon, mm(20, -0.3)));
    CHECK(contains_near(surface.polygon, mm(20.3, 0)));
    // Edges with different shifts at their ends are not translates; the right edge reaches the corner itself.
    CHECK_FALSE(surface.translated[0]);
    CHECK(contains_near(surface.polygon, mm(20, 20)));
    // Every arc point lies at its vertex's shift from the vertex, so the shifted edges join the arcs without gaps.
    const double shifts[] = {0.1, 0.3, 0., 0.2};
    for (size_t i = 0; i < surface.polygon.size(); ++i) {
        const auto &element = surface.elements[i];
        if (!element.vertex)
            continue;
        CAPTURE(i, element.source);
        const double distance = (surface.polygon.points[i] - square.points[element.source]).cast<double>().norm();
        CHECK_THAT(distance, Catch::Matchers::WithinAbs(scale_(shifts[element.source]), 1.5));
    }
    CHECK(surface.gaps.empty());

    // A modifier on the front face finds the centerline under its sides, whatever the shift there.
    const PreciseSeam::PreparedPerimeter prepared(square, vertex_profile({0.1, 0.3, 0., 0.2}), false);
    REQUIRE(prepared.shift == coord_t(scale_(0.3)));
    const auto result = PreciseSeam::extract_perimeter_segments(prepared,
        PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(4, -1, 16, -0.25))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(square, result);
    REQUIRE(result.segments.size() == 1);
    // The surface runs y = -0.1 - 0.01 x: below y = -0.25 from x = 15.
    CHECK(within_unit(result.segments.front().polyline.points.front(), mm(15, 0)));
    CHECK(within_unit(result.segments.front().polyline.points.back(), mm(16, 0)));
}

TEST_CASE("A concave corner with different shifts on its edges is resolved at their crossing", "[PreciseSeam][SegmentExtraction]")
{
    // Shifts 0.2 at (20, 10), 0.3 at the concave vertex (10, 10) and 0.1 at (10, 20): the shifted edges run
    // y = 10.2 + 0.01 (20 - x) and x = 10.3 - 0.02 (y - 10), crossing at about (10.29406, 10.29706).
    const Polygon perimeter = l_shape();
    const auto profile = vertex_profile({0.2, 0.2, 0.2, 0.3, 0.1, 0.2});
    const auto surface = PreciseSeam::detail::build_surface_line(perimeter, profile, false);
    REQUIRE(surface.gaps.size() == 1);
    const Vec2d crossing(10.3 - 0.02 * 0.297 / 0.9998, 10. + 0.297 / 0.9998);
    CHECK((unscale(surface.gaps.front().end) - crossing).norm() < 0.001);

    // Covering the crossing covers the dead stretch: one segment through the vertex, its ends under the modifier.
    const PreciseSeam::PreparedPerimeter prepared(perimeter, profile, false);
    const auto result = PreciseSeam::extract_perimeter_segments(prepared,
        PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(10.25, 10.25, 10.5, 10.5))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    check_provenance(perimeter, result);
    REQUIRE(result.segments.size() == 1);
    const Points &points = result.segments.front().polyline.points;
    REQUIRE(points.size() == 3);
    CHECK(within_unit(points[0], mm(10.5, 10)));
    CHECK(points[1] == mm(10, 10));
    CHECK(within_unit(points[2], mm(10, 10.5)));
}

TEST_CASE("A shift changing along a long edge splits its element and moves the segment end monotonically", "[PreciseSeam][SegmentExtraction]")
{
    // The front edge keeps 0.2 mm at its vertices and 0.3 mm in its middle: its surface bends at (10, -0.3).
    const Polygon square = rectangle(0, 0, 20, 20);
    auto profile = vertex_profile({0.2, 0.2, 0.2, 0.2});
    profile.interior.resize(4);
    profile.interior[0].emplace_back(0.5, coord_t(scale_(0.3)));
    const auto surface = PreciseSeam::detail::build_surface_line(square, profile, false);
    std::vector<std::pair<double, double>> front;
    for (const auto &element : surface.elements)
        if (element.source == 0 && !element.vertex)
            front.emplace_back(element.begin, element.end);
    REQUIRE(front.size() == 2);
    const std::pair<double, double> expected[] = {{0., 0.5}, {0.5, 1.}};
    for (size_t i = 0; i < 2; ++i) {
        CHECK_THAT(front[i].first, Catch::Matchers::WithinAbs(expected[i].first, 1e-12));
        CHECK_THAT(front[i].second, Catch::Matchers::WithinAbs(expected[i].second, 1e-12));
    }
    CHECK(contains_near(surface.polygon, mm(10, -0.3)));
    // The other edges keep a constant shift and stay translates.
    CHECK(surface.translated[1]);

    // A side of the modifier swept across the bend: the segment end follows it along the centerline.
    const PreciseSeam::PreparedPerimeter prepared(square, profile, false);
    double previous = -1.;
    for (int step = -20; step <= 20; ++step) {
        CAPTURE(step);
        const double x = 10. + step * 0.05;
        const auto result = PreciseSeam::extract_perimeter_segments(prepared,
            PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(2, -1, x, -0.1))}), ModelVolumeType::PRECISE_SEAM_CENTER);
        REQUIRE(result.segments.size() == 1);
        const Point &end = result.segments.front().polyline.points.back();
        CHECK(within_unit(end, mm(x, 0)));
        CHECK(double(end.x()) >= previous);
        previous = double(end.x());
    }
}

TEST_CASE("A profile within the snapping radius keeps the centerline", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    PreciseSeam::ShiftProfile profile;
    profile.vertices = {1000, 0, 500, 1000};
    const PreciseSeam::PreparedPerimeter prepared(square, profile, false);
    CHECK(prepared.shift == 0);
    CHECK(prepared.profile.empty());
    profile.vertices[1] = 1001;
    CHECK(PreciseSeam::PreparedPerimeter(square, profile, false).shift == 1001);
}

namespace {
// Measurement of a 0.4 mm line without compensation, accepting depths from 0.15 to 0.6 mm.
PreciseSeam::ShiftMeasurement measurement(const AABBTreeLines::LinesDistancer<Line> &outline, double compensation = 0.,
                                          double outline_compensation = 0.)
{
    return {outline, 0.4, compensation, outline_compensation, 0.15, 0.6};
}

coord_t shift_mm(double distance) { return coord_t(std::lround(scale_(distance))) - 5000; }
} // namespace

TEST_CASE("A centerline at its nominal depth keeps the constant shift", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    const AABBTreeLines::LinesDistancer<Line> nominal(to_lines(ExPolygon(rectangle(-0.2, -0.2, 20.2, 20.2))));
    CHECK(PreciseSeam::measure_shift_profile(square, false, measurement(nominal)).empty());
    // Every depth outside the accepted range takes the nominal shift as well: the outline is not this wall's own.
    const AABBTreeLines::LinesDistancer<Line> far(to_lines(ExPolygon(rectangle(-5, -5, 25, 25))));
    CHECK(PreciseSeam::measure_shift_profile(square, false, measurement(far)).empty());
    // XY compensation that the outline does not contain is in the nominal depth: still the constant shift.
    const AABBTreeLines::LinesDistancer<Line> uncompensated(to_lines(ExPolygon(rectangle(-0.3, -0.3, 20.3, 20.3))));
    CHECK(PreciseSeam::measure_shift_profile(square, false, measurement(uncompensated, -0.1, 0.)).empty());
    // At the inner corner of an L the nearest outline point is the corner, about 0.28 mm away; the depth along the
    // normals is still 0.2 mm.
    const Polygons l_outline = offset(l_shape(), float(scale_(0.2)), jtMiter, 3.);
    REQUIRE(l_outline.size() == 1);
    const AABBTreeLines::LinesDistancer<Line> l_nominal(to_lines(ExPolygon(l_outline.front())));
    CHECK(PreciseSeam::measure_shift_profile(l_shape(), false, measurement(l_nominal)).empty());
    // The same for the corners of a hole, all of them inner corners seen from the material.
    ExPolygon around_hole(rectangle(-10, -10, 30, 30));
    around_hole.holes.push_back(rectangle(0.2, 0.2, 19.8, 19.8));
    around_hole.holes.back().reverse();
    const AABBTreeLines::LinesDistancer<Line> hole_nominal(to_lines(around_hole));
    CHECK(PreciseSeam::measure_shift_profile(square, true, measurement(hole_nominal)).empty());
}

TEST_CASE("A centerline deeper than nominal takes its measured shift", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // Elephant foot compensation of 0.15 mm all round: the outline lies 0.35 mm from the centerline.
    const AABBTreeLines::LinesDistancer<Line> foot(to_lines(ExPolygon(rectangle(-0.35, -0.35, 20.35, 20.35))));
    const auto profile = PreciseSeam::measure_shift_profile(square, false, measurement(foot));
    REQUIRE(profile.vertices.size() == 4);
    for (const coord_t value : profile.vertices)
        CHECK(std::abs(value - shift_mm(0.35)) <= 1);
    // Constant along every edge: no interior samples.
    CHECK(profile.interior.empty());
    // A compensation the outline contains is subtracted; one it does not contain is not.
    const auto compensated = PreciseSeam::measure_shift_profile(square, false, measurement(foot, 0.1, 0.1));
    REQUIRE(compensated.vertices.size() == 4);
    CHECK(std::abs(compensated.vertices.front() - shift_mm(0.25)) <= 1);
    const auto excluded = PreciseSeam::measure_shift_profile(square, false, measurement(foot, -0.1, 0.));
    REQUIRE(excluded.vertices.size() == 4);
    CHECK(std::abs(excluded.vertices.front() - shift_mm(0.35)) <= 1);
    // A depth within the margin would put the surface on the body side: zero, not negative.
    const AABBTreeLines::LinesDistancer<Line> shallow(to_lines(ExPolygon(rectangle(-0.003, -0.003, 20.003, 20.003))));
    const auto clamped = PreciseSeam::measure_shift_profile(square, false, {shallow, 0.4, 0., 0., 0., 0.6});
    REQUIRE(clamped.vertices.size() == 4);
    CHECK(clamped.vertices.front() == 0);
}

TEST_CASE("A shift changing in the middle of a long edge is sampled along it", "[PreciseSeam][SegmentExtraction]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // The outline steps out to 0.3 mm below the front edge between x = 8 and 12 mm.
    const Polygon stepped(Points{mm(-0.2, -0.2), mm(8, -0.2), mm(8, -0.3), mm(12, -0.3), mm(12, -0.2), mm(20.2, -0.2),
                                 mm(20.2, 20.2), mm(-0.2, 20.2)});
    const AABBTreeLines::LinesDistancer<Line> outline(to_lines(ExPolygon(stepped)));
    const auto profile = PreciseSeam::measure_shift_profile(square, false, measurement(outline));
    REQUIRE(profile.vertices.size() == 4);
    CHECK(std::abs(profile.vertices.front() - shift_mm(0.2)) <= 1);
    REQUIRE(profile.interior.size() == 4);
    // Every 0.5 mm along the 20 mm edge; only the front edge changes. Next to the step its corner is nearer.
    REQUIRE(profile.interior[0].size() == 39);
    for (const auto &[parameter, value] : profile.interior[0]) {
        CAPTURE(parameter);
        const double x = 20. * parameter;
        CHECK(std::abs(value - shift_mm(x > 8.2 && x < 11.8 ? 0.3 : 0.2)) <= 1);
    }
    for (size_t edge = 1; edge < 4; ++edge)
        CHECK(profile.interior[edge].empty());
}

namespace {
// The edges of a noisy loop as Precise Seam indexes them.
AABBTreeLines::LinesDistancer<Line> noisy_lines(const Polygon &noisy) { return AABBTreeLines::LinesDistancer<Line>(noisy.lines()); }

// A point within a unit of `expected`.
void check_point(const Point &point, const Point &expected)
{
    CAPTURE(point.x(), point.y(), expected.x(), expected.y());
    CHECK((point - expected).cast<double>().norm() <= 1.5);
}
} // namespace

TEST_CASE("A point moves onto the noisy edge that the line to its surface point crosses", "[PreciseSeam][Fuzzy]")
{
    // Smooth edge along y = 0; its surface point lies 0.195 mm towards air at y < 0. The noisy edge from (9, -0.1) to
    // (11, 0.1) crosses x = 10 at (10, 0), halfway along.
    const Polygon noisy(Points{mm(-0.1, -0.1), mm(9, -0.1), mm(11, 0.1), mm(20.1, -0.1), mm(20.1, 20.1), mm(-0.1, 20.1)});
    const auto moved = PreciseSeam::detail::transfer_point(noisy_lines(noisy), mm(10, 0), mm(10, -0.195), scale_(0.4));
    CHECK(moved.outcome == PreciseSeam::detail::TransferOutcome::Exact);
    CHECK(moved.position.edge == 1);
    CHECK_THAT(moved.position.parameter, Catch::Matchers::WithinAbs(0.5, 1e-9));
    check_point(moved.position.point, mm(10, 0));
}

TEST_CASE("Of several crossings the one nearest to the point is taken", "[PreciseSeam][Fuzzy]")
{
    // A fold of the noisy loop: x = 10 crosses it at y = 0.3, 0.175 and -0.05.
    const Polygon noisy(Points{mm(0, 0.3), mm(11, 0.3), mm(9, 0.05), mm(11, -0.15), mm(20, -0.15), mm(20, 20), mm(0, 20)});
    const auto moved = PreciseSeam::detail::transfer_point(noisy_lines(noisy), mm(10, 0), mm(10, -0.195), scale_(0.4));
    CHECK(moved.outcome == PreciseSeam::detail::TransferOutcome::Exact);
    CHECK(moved.position.edge == 2);
    check_point(moved.position.point, mm(10, -0.05));
}

TEST_CASE("At an acute corner the point stays on its own side of the noisy loop", "[PreciseSeam][Fuzzy]")
{
    // A 20 degree wedge from the origin: one side along y = 0, the other along (cos 20, sin 20). The noisy loop lies
    // 0.1 mm outside both. Across the first side at x = 1 the line also crosses the other side's noise, 0.47 mm away.
    const double angle = 20. * PI / 180.;
    const Vec2d along(std::cos(angle), std::sin(angle));
    const Vec2d outward(-std::sin(angle), std::cos(angle));
    const auto other = [&](double distance) {
        const Vec2d point = distance * along + 0.1 * outward;
        return mm(point.x(), point.y());
    };
    const Polygon noisy(Points{mm(-0.29, -0.1), mm(0.5, -0.1), mm(1.5, -0.1), mm(5, -0.1), other(5), other(1.5), other(0.5)});
    const auto moved = PreciseSeam::detail::transfer_point(noisy_lines(noisy), mm(1, 0), mm(1, -0.195), scale_(0.6));
    CHECK(moved.outcome == PreciseSeam::detail::TransferOutcome::Exact);
    CHECK(moved.position.edge == 1);
    check_point(moved.position.point, mm(1, -0.1));
}

TEST_CASE("A transfer is exact, approximate or fails", "[PreciseSeam][Fuzzy]")
{
    using Outcome = PreciseSeam::detail::TransferOutcome;
    // The noisy edge near the point runs from (8, -0.1) to (9.9, -0.1): x = 10 misses it.
    const Polygon noisy(Points{mm(0, -0.1), mm(8, -0.1), mm(9.9, -0.1), mm(9.9, 20), mm(0, 20)});
    const auto lines = noisy_lines(noisy);
    // No crossing within the window: the nearest point of the loop, (9.9, 0) on the edge up from the corner.
    const auto missed = PreciseSeam::detail::transfer_point(lines, mm(10, 0), mm(10, -0.195), scale_(0.4));
    CHECK(missed.outcome == Outcome::Approximate);
    CHECK(missed.position.edge == 2);
    check_point(missed.position.point, mm(9.9, 0));
    // A pair that coincides, as on the centerline path, has no line: the same nearest point.
    const auto no_line = PreciseSeam::detail::transfer_point(lines, mm(10, 0), mm(10, 0), scale_(0.4));
    CHECK(no_line.outcome == Outcome::Approximate);
    CHECK(no_line.position.edge == 2);
    check_point(no_line.position.point, mm(9.9, 0));
    // Nothing within the window.
    CHECK(PreciseSeam::detail::transfer_point(lines, mm(10, 0), mm(10, -0.195), scale_(0.05)).outcome == Outcome::Failed);
    // An edge along the line gives its point nearest to the point: (9.9, 0) itself for a line along x = 9.9.
    const auto collinear = PreciseSeam::detail::transfer_point(lines, mm(9.9, 0.05), mm(9.9, 0.2), scale_(0.4));
    CHECK(collinear.outcome == Outcome::Exact);
    CHECK(collinear.position.edge == 2);
    check_point(collinear.position.point, mm(9.9, 0.05));
}

TEST_CASE("Segments keep the surface points their ends were carried from", "[PreciseSeam][Fuzzy]")
{
    const auto mode = GENERATE(ModelVolumeType::PRECISE_SEAM_LEFT, ModelVolumeType::PRECISE_SEAM_CENTER,
                               ModelVolumeType::PRECISE_SEAM_RIGHT);
    const Polygon square = rectangle(0, 0, 20, 20);
    const auto extracted = surface_extraction(square, rectangle(4, -1, 6, 0.1), false, mode);
    REQUIRE(extracted.segments.size() == 1);
    const auto &segment = extracted.segments.front();
    CHECK(segment.polyline.points.front() == mm(4, 0));
    CHECK(segment.polyline.points.back() == mm(6, 0));
    CHECK(segment.surface_first == Point(mm(4, 0).x(), -surface_distance));
    CHECK(segment.surface_last == Point(mm(6, 0).x(), -surface_distance));
    REQUIRE(segment.strong_target.has_value());
    // Center's pair is worked out only for a point carried onto a noisy loop.
    if (mode == ModelVolumeType::PRECISE_SEAM_CENTER)
        CHECK_FALSE(segment.strong_target->surface.has_value());
    else
        CHECK(segment.strong_target->surface == (mode == ModelVolumeType::PRECISE_SEAM_LEFT ? segment.surface_first : segment.surface_last));
    // On the centerline path the ends are their own pairs.
    const auto centerline = surface_extraction(square, rectangle(4, -1, 6, 0.1), false, mode, 0);
    REQUIRE(centerline.segments.size() == 1);
    CHECK(centerline.segments.front().surface_first == mm(4, 0));
    CHECK(centerline.segments.front().surface_last == mm(6, 0));
}

TEST_CASE("A piece of a convex corner's arc keeps both its ends", "[PreciseSeam][Fuzzy]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    // Around the arc point at -45 degrees from (20, 0): one vertex point, two different surface points on the arc.
    const auto arc = surface_extraction(square, rectangle(20.11, -0.17, 20.17, -0.11), false, ModelVolumeType::PRECISE_SEAM_ENFORCED);
    REQUIRE(arc.segments.size() == 1);
    const auto &segment = arc.segments.front();
    CHECK(segment.polyline.points == Points{mm(20, 0)});
    CHECK(segment.surface_first != segment.surface_last);
    // Both on the arc of radius 0.2 mm around the vertex, within the sagitta of its chords; the first one nearer the
    // bottom edge, where the arc starts.
    for (const Point &point : {segment.surface_first, segment.surface_last})
        CHECK(std::abs((point - mm(20, 0)).cast<double>().norm() - double(surface_distance)) <= 41.);
    CHECK(segment.surface_first.y() < segment.surface_last.y());
}

TEST_CASE("Center pairs with the middle of its edge, arc or concave crossing when carried", "[PreciseSeam][Fuzzy]")
{
    // Any usable noisy loop: only the pairs worked out for carrying are checked here.
    const auto center = [](const Polygon &perimeter, const Polygon &region) {
        PreciseSeam::PreparedPerimeter prepared(perimeter, surface_distance, false);
        prepared.set_target(perimeter, coord_t(scale_(0.5)));
        auto extracted = PreciseSeam::extract_perimeter_segments(prepared, PreciseSeam::prepare_modifier_regions({ExPolygon(region)}),
                                                                 ModelVolumeType::PRECISE_SEAM_CENTER);
        REQUIRE(extracted.segments.size() == 1);
        REQUIRE(extracted.segments.front().strong_target.has_value());
        REQUIRE(extracted.segments.front().strong_target->surface.has_value());
        return *extracted.segments.front().strong_target;
    };
    const Polygon square = rectangle(0, 0, 20, 20);
    const double half = double(surface_distance) / std::sqrt(2.);
    // Inside an edge: the point of the shifted edge at the same parameter.
    const auto edge = center(square, rectangle(4, -1, 6, 0.1));
    CHECK(edge.point == mm(5, 0));
    CHECK(*edge.surface == Point(mm(5, 0).x(), -surface_distance));
    // At a vertex the segment passes: the middle of the whole arc, at -45 degrees.
    const auto vertex = center(square, rectangle(19, -1, 21, 1));
    CHECK(vertex.point == mm(20, 0));
    CHECK((*vertex.surface - Point(mm(20, 0).x() + coord_t(half), -coord_t(half))).cast<double>().norm() <= 41.);
    // A zero-length segment: the middle of its piece of arc, about -45 degrees too for this symmetric piece.
    const auto piece = center(square, rectangle(20.11, -0.17, 20.17, -0.11));
    CHECK(piece.point == mm(20, 0));
    CHECK((*piece.surface - Point(mm(20, 0).x() + coord_t(half), -coord_t(half))).cast<double>().norm() <= 41.);
    // In the dead stretch of the concave corner (10, 10) of the L, at its vertex and beside it: the crossing (10.2, 10.2).
    const auto at_corner = center(l_shape(), rectangle(10.1, 10.1, 10.4, 10.4));
    CHECK(at_corner.point == mm(10, 10));
    check_point(*at_corner.surface, mm(10.2, 10.2));
    const auto beside = center(l_shape(), rectangle(10.1, 10.1, 10.6, 10.4));
    check_point(beside.point, mm(10.1, 10));
    check_point(*beside.surface, mm(10.2, 10.2));
}

TEST_CASE("Center at a vertex between collinear edges pairs with the point where their shifted edges meet", "[PreciseSeam][Fuzzy]")
{
    // The smooth loop has the vertex (10, 0) between two collinear edges, without an arc; Center across x = 9..11 mm
    // falls on it. Turned, the vertex is vertex zero. With a shift profile of 0.3 mm along the front and 0.2 mm on
    // the other sides, the surface lies 0.3 mm below it.
    const auto [turned, profiled] = GENERATE(table<bool, bool>({{false, false}, {true, false}, {false, true}}));
    CAPTURE(turned, profiled);
    Points points{mm(0, 0), mm(10, 0), mm(20, 0), mm(20, 20), mm(0, 20)};
    if (turned)
        std::rotate(points.begin(), points.begin() + 1, points.end());
    const Polygon smooth(points);
    // The noisy edge near the vertex runs from (9, -0.1) to (11, 0.3) mm; the rest of the noisy loop lies over 1 mm
    // away. The line x = 10 mm crosses that edge halfway, at (10, 0.1).
    const Polygon noisy(Points{mm(-0.1, -0.1), mm(9, -0.1), mm(11, 0.3), mm(20.1, -0.1), mm(20.1, 20.1), mm(-0.1, 20.1)});
    PreciseSeam::ShiftProfile profile;
    profile.vertices = {coord_t(scale_(0.3)), coord_t(scale_(0.3)), coord_t(scale_(0.3)), surface_distance, surface_distance};
    const auto prepared = profiled ? std::make_unique<PreciseSeam::PreparedPerimeter>(smooth, profile, false) :
                                     std::make_unique<PreciseSeam::PreparedPerimeter>(smooth, surface_distance, false);
    prepared->set_target(noisy, coord_t(scale_(0.5)));
    REQUIRE(prepared->valid);
    const auto extracted = PreciseSeam::extract_perimeter_segments(
        *prepared, PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(9, -1, 11, 0.1))}), ModelVolumeType::PRECISE_SEAM_CENTER);
    REQUIRE(extracted.segments.size() == 1);
    REQUIRE(extracted.segments.front().strong_target.has_value());
    const auto &target = *extracted.segments.front().strong_target;
    CHECK(target.point == mm(10, 0));
    REQUIRE(target.surface.has_value());
    check_point(*target.surface, mm(10, profiled ? -0.3 : -0.2));
    const auto moved = PreciseSeam::detail::transfer_point(noisy_lines(noisy), target.point, *target.surface, scale_(0.5));
    CHECK(moved.outcome == PreciseSeam::detail::TransferOutcome::Exact);
    check_point(moved.position.point, mm(10, 0.1));
}

TEST_CASE("Separate pieces of one arc make one vertex point from the first piece's start to the last one's end", "[PreciseSeam][Fuzzy]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    PreciseSeam::PreparedPerimeter prepared(square, surface_distance, false);
    prepared.set_target(square, coord_t(scale_(0.5)));
    // Two boxes on the arc of radius 0.2 mm around the corner (20, 0), which runs from -90 to 0 degrees: the first
    // covers it from x = 20.0384 mm (about -79 degrees) to about -61 degrees, the second from about -29 degrees to
    // y = -0.0384 mm (about -11 degrees). Neither touches the shifted edges.
    const ExPolygons regions{ExPolygon(rectangle(20.0384, -0.2179, 20.0984, -0.1579)),
                             ExPolygon(rectangle(20.1579, -0.0984, 20.2179, -0.0384))};
    const auto extracted = PreciseSeam::extract_perimeter_segments(prepared, PreciseSeam::prepare_modifier_regions(regions),
                                                                   ModelVolumeType::PRECISE_SEAM_ENFORCED);
    REQUIRE(extracted.segments.size() == 1);
    const auto &segment = extracted.segments.front();
    CHECK(segment.polyline.points == Points{mm(20, 0)});
    CAPTURE(segment.surface_first.x(), segment.surface_first.y(), segment.surface_last.x(), segment.surface_last.y());
    CHECK(std::abs(segment.surface_first.x() - mm(20.0384, 0).x()) <= 2);
    CHECK(std::abs(segment.surface_last.y() - mm(0, -0.0384).y()) <= 2);
    // On the arc, within the sagitta of its chords.
    for (const Point &point : {segment.surface_first, segment.surface_last})
        CHECK(std::abs((point - mm(20, 0)).cast<double>().norm() - double(surface_distance)) <= 41.);
}

TEST_CASE("A segment across the start of the loop keeps the surface points of its outer ends", "[PreciseSeam][Fuzzy]")
{
    // The square starts at its corner (0, 0); the zone runs from y = 1 mm on the left side through the corner to
    // x = 1 mm on the front, whose surface points lie 0.2 mm outside.
    const Polygon square = rectangle(0, 0, 20, 20);
    REQUIRE(square[0] == mm(0, 0));
    PreciseSeam::PreparedPerimeter prepared(square, surface_distance, false);
    prepared.set_target(square, coord_t(scale_(0.5)));
    const auto extracted = PreciseSeam::extract_perimeter_segments(
        prepared, PreciseSeam::prepare_modifier_regions({ExPolygon(rectangle(-1, -1, 1, 1))}), ModelVolumeType::PRECISE_SEAM_ENFORCED);
    REQUIRE(extracted.segments.size() == 1);
    const auto &segment = extracted.segments.front();
    check_point(segment.polyline.points.front(), mm(0, 1));
    check_point(segment.polyline.points.back(), mm(1, 0));
    check_point(segment.surface_first, mm(-0.2, 1));
    check_point(segment.surface_last, mm(1, -0.2));
}

TEST_CASE("A loop keeps its untextured centerline when copied, reversed or turned", "[PreciseSeam][Fuzzy]")
{
    const Polygon square = rectangle(0, 0, 20, 20);
    ExtrusionPath path(erExternalPerimeter);
    path.polyline = Polyline3(square.split_at_first_point());
    ExtrusionLoop loop(std::move(path));
    const auto untextured = std::make_shared<const Polygon>(square);
    loop.untextured = untextured;
    const ExtrusionLoop copy = loop;
    CHECK(copy.untextured == untextured);
    const std::unique_ptr<ExtrusionEntity> clone(loop.clone());
    CHECK(static_cast<const ExtrusionLoop *>(clone.get())->untextured == untextured);
    loop.reverse();
    CHECK(loop.untextured == untextured);
    loop.make_counter_clockwise();
    CHECK(loop.untextured == untextured);
    CHECK(loop.split_at_vertex(mm(20, 0)));
    CHECK(loop.untextured == untextured);
    // The geometry it holds is never changed through the loop.
    CHECK(untextured->points == square.points);
}
