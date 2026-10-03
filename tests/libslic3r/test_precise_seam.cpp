#include <catch2/catch_all.hpp>
#include "libslic3r/GCode/PreciseSeam.hpp"
#include "libslic3r/GCode/PreciseSeamInternal.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include <algorithm>
#include <cmath>

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
    // Inputs found by a randomized search against this Clipper: random perimeters clipped as open
    // lines against a half-plane whose border passes a few nanometres from a vertex, keeping fragments
    // that fail bind_fragment() but are bound by append_fragment(). Here the border crosses the
    // perimeter at a vertex, and Clipper rounds the cut to the vertex height but 1-2 nm beside it.
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
        {{nm(120701295, -128335579), nm(120586940, -128250619), nm(120582237, -128248720), nm(120594985, -128327864),
          nm(120504023, -128324788), nm(120782884, -128380054), nm(120729765, -128355229)},
         {nm(157083968, -426144373), nm(84375562, 169433907), nm(-213413578, 133079704), nm(-140705172, -462498576)},
         nm(120729764, -128355229), nm(120729765, -128355229)},
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
