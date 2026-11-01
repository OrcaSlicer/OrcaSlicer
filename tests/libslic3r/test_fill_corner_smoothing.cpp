#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include "libslic3r/Point.hpp"
#include <cstddef>
#include "libslic3r/Polygon.hpp"
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Fill/FillCornerSmoothing.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

namespace {

// A right angle turn, with the outgoing leg ten times longer than the incoming one.
Polyline asymmetric_corner()
{
    return Polyline{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 100.) };
}

double max_turn_cosine(const Polyline &polyline)
{
    double sharpest = 1.;
    for (size_t i = 1; i + 1 < polyline.size(); ++i) {
        const Vec2d incoming = (polyline[i] - polyline[i - 1]).cast<double>().normalized();
        const Vec2d outgoing = (polyline[i + 1] - polyline[i]).cast<double>().normalized();
        sharpest = std::min(sharpest, incoming.dot(outgoing));
    }
    return sharpest;
}

// How far before the corner at (10, 0) the path leaves the straight leg along the x axis.
double departure_before_corner(const Polyline &polyline)
{
    double departure = 0.;
    for (const Point &point : polyline.points)
        if (point.y() == 0)
            departure = std::max(departure, unscale<double>(point.x()));
    return 10. - departure;
}

// The distance from a point to the nearest edge of a path.
double distance_to(const Polyline &polyline, const Point &point)
{
    double distance = std::numeric_limits<double>::max();
    for (size_t i = 1; i < polyline.size(); ++i)
        distance = std::min(distance, Line(polyline[i - 1], polyline[i]).distance_to(point));
    return distance;
}

const double tolerance = scaled<double>(0.0125);

// The smoothing at the given factor, flattened to the given resolution.
CornerSmoothing smoothing(double factor, double resolution = tolerance)
{
    return CornerSmoothing{ factor, resolution };
}

} // namespace

TEST_CASE("Corner smoothing replaces a sharp vertex by a curve", "[FillCornerSmoothing]")
{
    const Polyline sharp  = asymmetric_corner();
    Polyline       smooth = sharp;
    smooth_polyline_corners(smooth, smoothing(1.));

    REQUIRE(smooth.size() > sharp.size());
    REQUIRE(smooth.front() == sharp.front());
    REQUIRE(smooth.back() == sharp.back());
    // The right angle is gone, every remaining turn is a gentle one.
    REQUIRE(max_turn_cosine(sharp) < 0.1);
    REQUIRE(max_turn_cosine(smooth) > 0.9);
    REQUIRE(smooth.length() < sharp.length());
}

TEST_CASE("Corner smoothing keeps the path untouched at a zero factor", "[FillCornerSmoothing]")
{
    const Polyline sharp = asymmetric_corner();

    Polyline none = sharp;
    smooth_polyline_corners(none, smoothing(0.));
    REQUIRE(none.points == sharp.points);

    Polyline invalid = sharp;
    smooth_polyline_corners(invalid, smoothing(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE(invalid.points == sharp.points);
}

TEST_CASE("Corner smoothing consumes at most half of the shorter leg", "[FillCornerSmoothing]")
{
    // The curve must not reach beyond the middle of either adjoining segment, otherwise the curves of
    // two adjacent corners would overlap. The shorter leg is 10mm long, so the corner at (10, 0) is
    // rounded within 5mm of it on both legs, even though the other leg is 100mm long.
    Polyline smooth = asymmetric_corner();
    smooth_polyline_corners(smooth, smoothing(1.));

    REQUIRE(departure_before_corner(smooth) > 0.);
    REQUIRE(departure_before_corner(smooth) <= 5.);
    // The rounded corner is the polygon of the curve's tangents, which stays inside the box spanned by the
    // two legs, and every vertex of it is within 5mm of the corner along the outgoing leg.
    for (const Point &point : smooth.points) {
        REQUIRE(point.x() >= 0);
        REQUIRE(point.y() >= 0);
        REQUIRE(point.x() <= Point::new_scale(10., 0.).x());
        REQUIRE(point.y() <= Point::new_scale(0., 100.).y());
        if (point.x() == Point::new_scale(10., 0.).x() && point != smooth.back())
            REQUIRE(point.y() <= Point::new_scale(0., 5.).y());
    }
}

TEST_CASE("Corner smoothing scales the curve with the factor", "[FillCornerSmoothing]")
{
    Polyline half = asymmetric_corner();
    smooth_polyline_corners(half, smoothing(0.5));
    Polyline full = asymmetric_corner();
    smooth_polyline_corners(full, smoothing(1.));

    // Half of the factor leaves the 10mm leg closer to the corner.
    REQUIRE(departure_before_corner(half) < departure_before_corner(full));
    // A larger factor rounds a wider portion of the legs, cutting more of the corner off.
    REQUIRE(full.length() < half.length());
}

TEST_CASE("Corner smoothing leaves hairpins sharp", "[FillCornerSmoothing]")
{
    // Both ends of a curve replacing a nearly reversing turn coincide, which would round the hairpin
    // into a degenerate loop instead of a tip.
    Polyline hairpin{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(0., 0.5) };
    const Polyline sharp = hairpin;
    smooth_polyline_corners(hairpin, smoothing(1.));
    REQUIRE(hairpin == sharp);
}

TEST_CASE("Corner smoothing follows the flattening tolerance", "[FillCornerSmoothing]")
{
    Polyline coarse = asymmetric_corner();
    smooth_polyline_corners(coarse, smoothing(1., scaled<double>(0.2)));
    Polyline fine = asymmetric_corner();
    smooth_polyline_corners(fine, smoothing(1., scaled<double>(0.001)));

    REQUIRE(fine.size() > coarse.size());
    REQUIRE(fine.front() == coarse.front());
    REQUIRE(fine.back() == coarse.back());
}

TEST_CASE("Corner smoothing emits no zero length segments", "[FillCornerSmoothing]")
{
    // Fully smoothed adjacent corners meet at the midpoint of the segment they share.
    Polyline zigzag;
    for (int i = 0; i < 8; ++i)
        zigzag.points.emplace_back(Point::new_scale(i, i % 2 ? 1. : 0.));
    smooth_polyline_corners(zigzag, smoothing(1.));

    for (size_t i = 1; i < zigzag.size(); ++i)
        REQUIRE((zigzag[i] - zigzag[i - 1]).cast<double>().squaredNorm() > 0.);
}

TEST_CASE("Corner smoothing rounds every vertex of a polygon", "[FillCornerSmoothing]")
{
    // A polygon closes implicitly, so none of its corners may stay sharp, not even the first one.
    const Polygon square{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 10.),
                          Point::new_scale(0., 10.) };
    Polygons smooth{ square };
    smooth_polygons_corners(smooth, smoothing(1.));
    const Polyline rounded = smooth.front().split_at_first_point();

    REQUIRE(smooth.front().size() > square.size());
    REQUIRE(max_turn_cosine(rounded) > 0.9);
    // The turn from the closing segment back into the first one must be gentle as well.
    const Vec2d incoming = (rounded[rounded.size() - 1] - rounded[rounded.size() - 2]).cast<double>().normalized();
    const Vec2d outgoing = (rounded[1] - rounded[0]).cast<double>().normalized();
    REQUIRE(incoming.dot(outgoing) > 0.9);
    // None of the corners is cut by more than half of a 10mm side.
    for (const Point &point : smooth.front().points) {
        REQUIRE(point.x() >= 0);
        REQUIRE(point.y() >= 0);
        REQUIRE(point.x() <= Point::new_scale(10., 0.).x());
        REQUIRE(point.y() <= Point::new_scale(0., 10.).y());
    }
}

TEST_CASE("Corner smoothing keeps the ends of a path that returns to its start", "[FillCornerSmoothing][Regression]")
{
    // A branch of a lightning tree walks out and retraces its way back, ending where it started. Its
    // ends are two free ends that happen to coincide, and joining them would close it into a loop.
    Polyline retrace{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 10.),
                      Point::new_scale(5., 10.), Point::new_scale(0., 0.) };
    const Polyline sharp = retrace;
    smooth_polyline_corners(retrace, smoothing(1.));

    REQUIRE(retrace.size() > sharp.size());
    REQUIRE(retrace.front() == sharp.front());
    REQUIRE(retrace.back() == sharp.back());
}

TEST_CASE("Corner smoothing ignores vertices splitting a straight leg", "[FillCornerSmoothing][Regression]")
{
    // The triangular and grid infills emit a vertex halfway along the straight run joining two of
    // their corners. Measuring the legs up to that vertex instead of up to the next corner let the
    // rounding reach only half as far there as it did into the very same run elsewhere in the
    // pattern, so geometrically identical corners came out rounded to different radii.
    const Polyline plain{ Point::new_scale(0., 20.), Point::new_scale(10., 0.),
                          Point::new_scale(20., 0.), Point::new_scale(30., 20.) };
    Polyline split = plain;
    split.points.insert(split.points.begin() + 2, Point::new_scale(15., 0.));

    Polyline smooth_plain = plain;
    smooth_polyline_corners(smooth_plain, smoothing(1.));
    Polyline smooth_split = split;
    smooth_polyline_corners(smooth_split, smoothing(1.));

    REQUIRE(smooth_split.points == smooth_plain.points);
    // Both corners reach equally far into the 10mm run they share.
    double first = 20., last = 10.;
    for (const Point &point : smooth_plain.points)
        if (point.y() == 0) {
            first = std::min(first, unscale<double>(point.x()));
            last  = std::max(last, unscale<double>(point.x()));
        }
    REQUIRE(first - 10. == Catch::Approx(20. - last).margin(1e-5));
}

TEST_CASE("Corner smoothing turns by the same angle at every vertex of a corner", "[FillCornerSmoothing]")
{
    Polyline smooth = asymmetric_corner();
    smooth_polyline_corners(smooth, smoothing(1.));
    REQUIRE(smooth.size() > 4);
    double sharpest = 0., gentlest = PI;
    for (size_t i = 1; i + 1 < smooth.size(); ++i) {
        const Vec2d  incoming = (smooth[i] - smooth[i - 1]).cast<double>().normalized();
        const Vec2d  outgoing = (smooth[i + 1] - smooth[i]).cast<double>().normalized();
        const double turn     = std::atan2(std::abs(cross2(incoming, outgoing)), incoming.dot(outgoing));
        sharpest = std::max(sharpest, turn);
        gentlest = std::min(gentlest, turn);
    }
    // The vertices together make up the right angle of the corner.
    REQUIRE_THAT(sharpest * double(smooth.size() - 2), Catch::Matchers::WithinRel(0.5 * PI, 0.02));
    REQUIRE_THAT(gentlest, Catch::Matchers::WithinRel(sharpest, 0.02));
}

TEST_CASE("Corner smoothing keeps within the resolution of its curve", "[FillCornerSmoothing]")
{
    // The curve itself, up to 0.2 microns.
    Polyline fine = asymmetric_corner();
    smooth_polyline_corners(fine, smoothing(1., scaled<double>(0.0002)));
    auto vertices = [&fine](double resolution) {
        Polyline smooth = asymmetric_corner();
        smooth_polyline_corners(smooth, smoothing(1., scaled<double>(resolution)));
        for (const Point &point : smooth.points)
            REQUIRE(distance_to(fine, point) <= scaled<double>(resolution + 0.0002));
        return smooth.size() - 2;
    };
    // A coarser resolution takes fewer vertices.
    REQUIRE(vertices(0.05) < vertices(0.0125));
}
