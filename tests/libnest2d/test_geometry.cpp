#include <catch2/catch_all.hpp>

#include "libnest2d_test_utils.hpp"
#include "printer_parts.hpp"

using namespace libnest2d;

namespace {

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Geometry values round-trip through floating point, so compare with a small
// tolerance that works both near and away from zero.
void require_close(double value, double expected) {
    REQUIRE_THAT(value, WithinRel(expected, 1e-9) || WithinAbs(expected, 1e-9));
}

// The printer parts as nestable items, computed once.
const std::vector<Item> &prusa_parts() {
    static const std::vector<Item> parts = [] {
        std::vector<Item> ret;
        ret.reserve(PRINTER_PART_POLYGONS.size());
        for (auto &inp : PRINTER_PART_POLYGONS) {
            auto inp_cpy = inp;
            if (ClosureTypeV<PathImpl> == Closure::OPEN)
                inp_cpy.points.pop_back();
            if constexpr (!is_clockwise<PathImpl>())
                std::reverse(inp_cpy.begin(), inp_cpy.end());
            ret.emplace_back(inp_cpy);
        }
        return ret;
    }();
    return parts;
}

} // namespace

TEST_CASE("Degree and radian conversion round-trips", "[Geometry]") {
    Degrees deg(180);
    Radians rad(deg);

    require_close(rad, Pi);
    require_close(deg, 180);
    require_close(Degrees(rad), 180);
    require_close(rad, Radians(deg));
    require_close(Degrees(rad), deg);
    REQUIRE(rad == deg);
}

TEST_CASE("Segment angle to the X axis", "[Geometry]") {
    auto quadrant = [](Point to) { return Degrees(Segment({0, 0}, to).angleToXaxis()); };

    REQUIRE(quadrant({12, -10}) > 270);  REQUIRE(quadrant({12, -10}) < 360);
    REQUIRE(quadrant({12, 10})  > 0);    REQUIRE(quadrant({12, 10})  < 90);
    REQUIRE(quadrant({-12, 10}) > 90);   REQUIRE(quadrant({-12, 10}) < 180);
    REQUIRE(quadrant({-12, -10}) > 180); REQUIRE(quadrant({-12, -10}) < 270);

    require_close(quadrant({1, 0}), 0);
    require_close(quadrant({0, 1}), 90);
    require_close(quadrant({-1, 0}), 180);
    require_close(quadrant({0, -1}), 270);
}

TEST_CASE("Point to segment distance", "[Geometry]") {
    Point   p2 = {10, 0};
    Segment seg({0, 0}, {10, 10});

    auto check = [](TCompute<Coord> val, TCompute<Coord> expected) {
        if (std::is_floating_point<TCompute<Coord>>::value)
            require_close(double(val), double(expected));
        else
            REQUIRE(val == expected);
    };

    auto h = pointlike::horizontalDistance(p2, seg);
    REQUIRE(h.second);
    check(h.first, 10);

    auto v = pointlike::verticalDistance(p2, seg);
    REQUIRE(v.second);
    check(v.first, -10);

    v = pointlike::verticalDistance(Point{10, 20}, seg);
    REQUIRE(v.second);
    check(v.first, 10);

    Point   p4 = {80, 0};
    Segment seg2({0, 0}, {0, 40});

    h = pointlike::horizontalDistance(p4, seg2);
    REQUIRE(h.second);
    check(h.first, 80);

    v = pointlike::verticalDistance(p4, seg2);
    REQUIRE_FALSE(v.second);   // the point does not project onto the segment
}

TEST_CASE("Item area", "[Geometry]") {
    require_close(RectangleItem(10, 10).area(), 100);
    require_close(RectangleItem(100, 100).area(), 10000);

    Item item = {
        {61, 97}, {70, 151}, {176, 151}, {189, 138},
        {189, 59}, {70, 59}, {61, 77}, {61, 97}
    };
    REQUIRE(std::abs(shapelike::area(item.transformedShape())) > 0);
}

TEST_CASE("Point inside polygon", "[Geometry]") {
    RectangleItem rect(10, 10);

    REQUIRE(rect.isInside(Point{1, 1}));
    REQUIRE(rect.isInside(Point{3, 3}));
    REQUIRE_FALSE(rect.isInside(Point{11, 11}));
    REQUIRE_FALSE(rect.isInside(Point{11, 12}));
}

TEST_CASE("Bounding circle of the printer parts", "[Geometry]") {
    PolygonImpl p = {{{0, 10}, {10, 0}, {0, -10}, {0, 10}}, {}};
    Circle      c = placers::boundingCircle(p);

    require_close(getX(c.center()), 0);
    require_close(getY(c.center()), 0);
    require_close(c.radius(), 10);

    shapelike::translate(p, PointImpl{10, 10});
    c = placers::boundingCircle(p);
    require_close(getX(c.center()), 10);
    require_close(getY(c.center()), 10);
    require_close(c.radius(), 10);

    for (auto &part : prusa_parts()) {
        c = placers::boundingCircle(part.transformedShape());
        REQUIRE_FALSE(std::isnan(c.radius()));
        for (auto v : shapelike::contour(part.transformedShape())) {
            auto d = pointlike::distance(v, c.center());
            if (d > c.radius())
                REQUIRE(std::abs(1.0 - d / c.radius()) <= 1e-3);  // on the circle
        }
    }
}

TEST_CASE("Convex hull of a printer part", "[Geometry]") {
    PathImpl poly  = PRINTER_PART_POLYGONS[0];
    auto     chull = sl::convexHull(poly);

    REQUIRE(chull.size() == poly.size());  // the part is already convex
}

namespace {

using Unit  = int64_t;
using Ratio = boost::rational<boost::multiprecision::int128_t>;

// Reference minimum-area bounding box, found by brute force over every edge
// direction, to validate the rotating-calipers implementation.
long double ref_min_area_box(const PolygonImpl &p) {
    long double min_area = std::numeric_limits<long double>::max();

    auto update_min = [&](const Point &a, const Point &b) {
        PolygonImpl rotated = p;
        sl::rotate(rotated, -Segment(a, b).angleToXaxis());
        min_area = std::min(min_area, cast<long double>(sl::area(sl::boundingBox(rotated))));
    };

    auto it = sl::cbegin(p), itx = std::next(it);
    while (itx != sl::cend(p)) { update_min(*it, *itx); ++it; ++itx; }
    update_min(*std::prev(sl::cend(p)), *sl::cbegin(p));

    return min_area;
}

} // namespace

TEST_CASE("Minimum-area bounding box via rotating calipers", "[Geometry]") {
    const long double tolerance = 500e6l;

    for (const PathImpl &part : PRINTER_PART_POLYGONS) {
        auto area = cast<long double>(minAreaBoundingBox<PathImpl, Unit, Ratio>(part).area());
        REQUIRE(std::abs(ref_min_area_box(PolygonImpl(part)) - area) < tolerance);
    }

    for (PathImpl part : STEGOSAUR_POLYGONS) {
        std::reverse(part.begin(), part.end());
        PolygonImpl poly(removeCollinearPoints<PathImpl, PointImpl, Unit>(part, 1000000));
        auto area = cast<long double>(minAreaBoundingBox<PolygonImpl, Unit, Ratio>(poly).area());
        REQUIRE(std::abs(ref_min_area_box(poly) - area) < tolerance);
    }
}
