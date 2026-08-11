#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "libslic3r/Fill/FillCornerSmoothing.hpp"
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

bool contains(const Polyline &polyline, const Point &point)
{
    return std::find(polyline.points.begin(), polyline.points.end(), point) != polyline.points.end();
}

const double tolerance = scaled<double>(0.0125);

} // namespace

TEST_CASE("Corner smoothing replaces a sharp vertex by a curve", "[FillCornerSmoothing]")
{
    const Polyline sharp  = asymmetric_corner();
    Polyline       smooth = sharp;
    smooth_polyline_corners(smooth, 1., tolerance);

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
    smooth_polyline_corners(none, 0., tolerance);
    REQUIRE(none.points == sharp.points);

    Polyline invalid = sharp;
    smooth_polyline_corners(invalid, std::numeric_limits<double>::quiet_NaN(), tolerance);
    REQUIRE(invalid.points == sharp.points);
}

TEST_CASE("Corner smoothing consumes at most half of the shorter leg", "[FillCornerSmoothing]")
{
    // The curve must not reach beyond the middle of either adjoining segment, otherwise the curves of
    // two adjacent corners would overlap. The shorter leg is 10mm long, so the corner at (10, 0) is
    // left 5mm before it and rejoined 5mm past it, even though the other leg is 100mm long.
    Polyline smooth = asymmetric_corner();
    smooth_polyline_corners(smooth, 1., tolerance);

    REQUIRE(contains(smooth, Point::new_scale(5., 0.)));
    REQUIRE(contains(smooth, Point::new_scale(10., 5.)));
    // A Bezier curve stays within the convex hull of its control points, so the rounded path stays
    // inside the box spanned by the two legs.
    for (const Point &point : smooth.points) {
        REQUIRE(point.x() >= 0);
        REQUIRE(point.y() >= 0);
        REQUIRE(point.x() <= Point::new_scale(10., 0.).x());
        REQUIRE(point.y() <= Point::new_scale(0., 100.).y());
    }
}

TEST_CASE("Corner smoothing scales the curve with the factor", "[FillCornerSmoothing]")
{
    Polyline half = asymmetric_corner();
    smooth_polyline_corners(half, 0.5, tolerance);
    Polyline full = asymmetric_corner();
    smooth_polyline_corners(full, 1., tolerance);

    // Half of the factor leaves the 10mm leg half as far from the corner.
    REQUIRE(contains(half, Point::new_scale(7.5, 0.)));
    REQUIRE(contains(full, Point::new_scale(5., 0.)));
    // A larger factor rounds a wider portion of the legs, cutting more of the corner off.
    REQUIRE(full.length() < half.length());
}

TEST_CASE("Corner smoothing leaves hairpins sharp", "[FillCornerSmoothing]")
{
    // Both ends of a curve replacing a nearly reversing turn coincide, which would round the hairpin
    // into a degenerate loop instead of a tip.
    Polyline hairpin{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(0., 0.5) };
    const Polyline sharp = hairpin;
    smooth_polyline_corners(hairpin, 1., tolerance);
    REQUIRE(hairpin == sharp);
}

TEST_CASE("Corner smoothing follows the flattening tolerance", "[FillCornerSmoothing]")
{
    Polyline coarse = asymmetric_corner();
    smooth_polyline_corners(coarse, 1., scaled<double>(0.2));
    Polyline fine = asymmetric_corner();
    smooth_polyline_corners(fine, 1., scaled<double>(0.001));

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
    smooth_polyline_corners(zigzag, 1., tolerance);

    for (size_t i = 1; i < zigzag.size(); ++i)
        REQUIRE((zigzag[i] - zigzag[i - 1]).cast<double>().squaredNorm() > 0.);
}

TEST_CASE("Corner smoothing rounds the seam of a closed loop", "[FillCornerSmoothing]")
{
    // A loop has no free ends, so none of its corners may stay sharp, not even the one it starts at.
    Polyline square{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 10.),
                     Point::new_scale(0., 10.), Point::new_scale(0., 0.) };
    const Polyline sharp = square;
    smooth_polyline_corners(square, 1., tolerance);

    REQUIRE(square.front() == square.back());
    REQUIRE(square.size() > sharp.size());
    REQUIRE(max_turn_cosine(square) > 0.9);
    // The turn from the last segment back into the first one closes the loop and must be gentle too.
    const Vec2d incoming = (square[square.size() - 1] - square[square.size() - 2]).cast<double>().normalized();
    const Vec2d outgoing = (square[1] - square[0]).cast<double>().normalized();
    REQUIRE(incoming.dot(outgoing) > 0.9);
    // None of the corners is cut by more than half of a 10mm side.
    for (const Point &point : square.points) {
        REQUIRE(point.x() >= 0);
        REQUIRE(point.y() >= 0);
        REQUIRE(point.x() <= Point::new_scale(10., 0.).x());
        REQUIRE(point.y() <= Point::new_scale(0., 10.).y());
    }
}
