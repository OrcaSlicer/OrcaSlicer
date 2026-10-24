#include <algorithm>
#include <cmath>
#include <random>
#include <catch2/catch_all.hpp>

#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include <cstddef>
#include "libslic3r/libslic3r.h"
#include <numeric>
#include <iostream>
#include <utility>
#include <boost/filesystem.hpp>
#include <utility>
#include <vector>
#include <cstdlib>
#include <optional>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/MultiPoint.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;

// Clipper may start an output polygon at any of its vertices.
static Polygon start_at_min(Polygon polygon)
{
    std::rotate(polygon.points.begin(), std::min_element(polygon.points.begin(), polygon.points.end()), polygon.points.end());
    return polygon;
}
static Polygons start_at_min(Polygons polygons)
{
    for (Polygon &polygon : polygons)
        polygon = start_at_min(std::move(polygon));
    return polygons;
}
static ExPolygons start_at_min(ExPolygons expolygons)
{
    for (ExPolygon &expolygon : expolygons) {
        expolygon.contour = start_at_min(std::move(expolygon.contour));
        expolygon.holes   = start_at_min(std::move(expolygon.holes));
    }
    return expolygons;
}

SCENARIO("Various Clipper operations - xs/t/11_clipper.t", "[ClipperUtils]") {
    // CCW oriented contour
    Slic3r::Polygon   square{ { 200, 100 }, {200, 200}, {100, 200}, {100, 100} };
    // CW oriented contour
    Slic3r::Polygon   hole_in_square{ { 160, 140 }, { 140, 140 }, { 140, 160 }, { 160, 160 } };
    Slic3r::ExPolygon square_with_hole(square, hole_in_square);
    GIVEN("square_with_hole") {
        WHEN("offset") {
            Polygons result = Slic3r::offset(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(start_at_min(result) == start_at_min(Polygons {
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 155, 145 }, { 145, 145 }, { 145, 155 }, { 155, 155 } } }));
            }
        }
        WHEN("offset_ex") {
            ExPolygons result = Slic3r::offset_ex(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(start_at_min(result) == start_at_min(ExPolygons { {
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 145, 145 }, { 145, 155 }, { 155, 155 }, { 155, 145 } } } }));
            }
        }
        WHEN("offset2_ex") {
            ExPolygons result = Slic3r::offset2_ex({ square_with_hole }, 5.f, -2.f);
            THEN("offset matches") {
                REQUIRE(start_at_min(result) == start_at_min(ExPolygons { {
                    { { 203, 203 }, { 97, 203 }, { 97, 97 }, { 203, 97 } },
                    { { 143, 143 }, { 143, 157 }, { 157, 157 }, { 157, 143 } } } }));
            }
        }
    }
    GIVEN("square_with_hole 2") {
        Slic3r::ExPolygon square_with_hole(
            { { 20000000, 20000000 }, { 0, 20000000 }, { 0, 0 }, { 20000000, 0 } },
            { { 5000000, 15000000 }, { 15000000, 15000000 }, { 15000000, 5000000 }, { 5000000, 5000000 } });
        WHEN("offset2_ex") {
            Slic3r::ExPolygons result = Slic3r::offset2_ex(ExPolygons { square_with_hole }, -1.f, 1.f);
            THEN("offset matches") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("square and hole") {
        WHEN("diff_ex") {
            ExPolygons result = Slic3r::diff_ex(Polygons{ square }, Polygons{ hole_in_square });
            THEN("hole is created") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("polyline") {
        Polyline polyline { { 50, 150 }, { 300, 150 } };
        WHEN("intersection_pl") {
            Polylines result = Slic3r::intersection_pl({ polyline }, Polygons{ square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 2);
            }
            THEN("result lines have correct length") {
                // results are in no particular order
                REQUIRE(result[0].length() == 40);
                REQUIRE(result[1].length() == 40);
            }
        }
        WHEN("diff_pl") {
            Polylines result = Slic3r::diff_pl({ polyline }, Polygons{ square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 3);
            }
            // results are in no particular order
            THEN("the left result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 50; }) == 1);
            }
            THEN("the right result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 100; }) == 1);
            }
            THEN("the central result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 20; }) == 1);
            }
        }
    }
    GIVEN("Clipper bug #96 / Slic3r issue #2028") {
        Slic3r::Polyline subject{
            { 44735000, 31936670 }, { 55270000, 31936670 }, { 55270000, 25270000 }, { 74730000, 25270000 }, { 74730000, 44730000 }, { 68063296, 44730000 }, { 68063296, 55270000 }, { 74730000, 55270000 },
            { 74730000, 74730000 }, { 55270000, 74730000 }, { 55270000, 68063296 }, { 44730000, 68063296 }, { 44730000, 74730000 }, { 25270000, 74730000 }, { 25270000, 55270000 }, { 31936670, 55270000 },
            { 31936670, 44730000 }, { 25270000, 44730000 }, { 25270000, 25270000 }, { 44730000, 25270000 }, { 44730000, 31936670 } };
        Slic3r::Polygon clip { {75200000, 45200000}, {54800000, 45200000}, {54800000, 24800000}, {75200000, 24800000} };
        Slic3r::Polylines result = Slic3r::intersection_pl(subject, Polygons{ clip });
        THEN("intersection_pl - result is not empty") {
            REQUIRE(result.size() == 1);
        }
    }
    GIVEN("Clipper bug #122") {
        Slic3r::Polyline subject { { 1975, 1975 }, { 25, 1975 }, { 25, 25 }, { 1975, 25 }, { 1975, 1975 } };
        Slic3r::Polygons clip { { { 2025, 2025 }, { -25, 2025 } , { -25, -25 }, { 2025, -25 } },
                                { { 525, 525 }, { 525, 1475 }, { 1475, 1475 }, { 1475, 525 } } };
        Slic3r::Polylines result = Slic3r::intersection_pl({ subject }, clip);
        THEN("intersection_pl - result is not empty") {
            REQUIRE(result.size() == 1);
            REQUIRE(result.front().points.size() == 5);
        }
    }
    GIVEN("Clipper bug #126") {
        Slic3r::Polyline subject { { 200000, 19799999 }, { 200000, 200000 }, { 24304692, 200000 }, { 15102879, 17506106 }, { 13883200, 19799999 }, { 200000, 19799999 } };
        Slic3r::Polygon clip { { 15257205, 18493894 }, { 14350057, 20200000 }, { -200000, 20200000 }, { -200000, -200000 }, { 25196917, -200000 } };
        Slic3r::Polylines result = Slic3r::intersection_pl(subject, Polygons{ clip });
        THEN("intersection_pl - result is not empty") {
            REQUIRE(result.size() == 1);
        }
        THEN("intersection_pl - result has same length as subject polyline") {
            REQUIRE(result.front().length() == Catch::Approx(subject.length()));
        }
    }

#if 0
    {
        # Clipper does not preserve polyline orientation
        my $polyline = Slic3r::Polyline->new([50, 150], [300, 150]);
        my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
        is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
        is_deeply $result->[0]->pp, [[100, 150], [200, 150]], 'clipped line orientation is preserved';
    }
    {
        # Clipper does not preserve polyline orientation
        my $polyline = Slic3r::Polyline->new([300, 150], [50, 150]);
        my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
        is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
        is_deeply $result->[0]->pp, [[200, 150], [100, 150]], 'clipped line orientation is preserved';
    }
    {
        # Disabled until Clipper bug #127 is fixed
        my $subject = [
            Slic3r::Polyline->new([-90000000, -100000000], [-90000000, 100000000]), # vertical
                Slic3r::Polyline->new([-100000000, -10000000], [100000000, -10000000]), # horizontal
                Slic3r::Polyline->new([-100000000, 0], [100000000, 0]), # horizontal
                Slic3r::Polyline->new([-100000000, 10000000], [100000000, 10000000]), # horizontal
        ];
        my $clip = Slic3r::Polygon->new(# a circular, convex, polygon
            [99452190, 10452846], [97814760, 20791169], [95105652, 30901699], [91354546, 40673664], [86602540, 50000000],
            [80901699, 58778525], [74314483, 66913061], [66913061, 74314483], [58778525, 80901699], [50000000, 86602540],
            [40673664, 91354546], [30901699, 95105652], [20791169, 97814760], [10452846, 99452190], [0, 100000000],
            [-10452846, 99452190], [-20791169, 97814760], [-30901699, 95105652], [-40673664, 91354546],
            [-50000000, 86602540], [-58778525, 80901699], [-66913061, 74314483], [-74314483, 66913061],
            [-80901699, 58778525], [-86602540, 50000000], [-91354546, 40673664], [-95105652, 30901699],
            [-97814760, 20791169], [-99452190, 10452846], [-100000000, 0], [-99452190, -10452846],
            [-97814760, -20791169], [-95105652, -30901699], [-91354546, -40673664], [-86602540, -50000000],
            [-80901699, -58778525], [-74314483, -66913061], [-66913061, -74314483], [-58778525, -80901699],
            [-50000000, -86602540], [-40673664, -91354546], [-30901699, -95105652], [-20791169, -97814760],
            [-10452846, -99452190], [0, -100000000], [10452846, -99452190], [20791169, -97814760],
            [30901699, -95105652], [40673664, -91354546], [50000000, -86602540], [58778525, -80901699],
            [66913061, -74314483], [74314483, -66913061], [80901699, -58778525], [86602540, -50000000],
            [91354546, -40673664], [95105652, -30901699], [97814760, -20791169], [99452190, -10452846], [100000000, 0]
            );
        my $result = Slic3r::Geometry::Clipper::intersection_pl($subject, [$clip]);
        is scalar(@$result), scalar(@$subject), 'intersection_pl - expected number of polylines';
        is sum(map scalar(@$_), @$result), scalar(@$subject) * 2, 'intersection_pl - expected number of points in polylines';
    }
#endif
}

SCENARIO("Various Clipper operations - t/clipper.t", "[ClipperUtils]") {
    GIVEN("square with hole") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polygon   square2 { { 5, 12 }, { 25, 12 }, { 25, 18 }, { 5, 18 } };
        // CW oriented contour
        Slic3r::Polygon   hole_in_square { { 14, 14 }, { 14, 16 }, { 16, 16 }, { 16, 14 } };
        WHEN("intersection_ex with another square") {
            ExPolygons intersection = Slic3r::intersection_ex(Polygons{ square, hole_in_square }, Polygons{ square2 });
            THEN("intersection area matches (hole is preserved)") {
                ExPolygon match({ { 20, 18 }, { 10, 18 }, { 10, 12 }, { 20, 12 } },
                                { { 14, 16 }, { 16, 16 }, { 16, 14 }, { 14, 14 } });
                REQUIRE(intersection.size() == 1);
                REQUIRE(intersection.front().area() == Catch::Approx(match.area()));
            }
        }
    }
    GIVEN("square with hole 2") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 0, 0 }, { 40, 0 }, { 40, 40 }, { 0, 40 } };
        Slic3r::Polygon   square2 { { 10, 10 }, { 30, 10 }, { 30, 30 }, { 10, 30 } };
        // CW oriented contour
        Slic3r::Polygon   hole { { 15, 15 }, { 15, 25 }, { 25, 25 }, {25, 15 } };
        WHEN("union_ex with another square") {
            ExPolygons union_ = Slic3r::union_ex({ square, square2, hole });
            THEN("union of two ccw and one cw is a contour with no holes") {
                REQUIRE(union_.size() == 1);
                REQUIRE(union_.front() == ExPolygon { { 40, 40 }, { 0, 40 }, { 0, 0 }, { 40, 0 } } );
            }
        }
        WHEN("diff_ex with another square") {
            ExPolygons diff = Slic3r::diff_ex(Polygons{ square, square2 }, Polygons{ hole });
            THEN("difference of a cw from two ccw is a contour with one hole") {
                REQUIRE(diff.size() == 1);
                REQUIRE(diff.front().area() == Catch::Approx(ExPolygon({ {40, 40}, {0, 40}, {0, 0}, {40, 0} }, { {15, 25}, {25, 25}, {25, 15}, {15, 15} }).area()));
            }
        }
    }
    GIVEN("yet another square") {
        Slic3r::Polygon  square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polyline square_pl = square.split_at_first_point();
        WHEN("no-op diff_pl") {
            Slic3r::Polylines res = Slic3r::diff_pl({ square_pl }, Polygons{});
            THEN("returns the right number of polylines") {
                REQUIRE(res.size() == 1);
            }
            THEN("returns the unmodified input polyline") {
                REQUIRE(res.front().points.size() == square_pl.points.size());
            }
        }
    }
}

TEST_CASE("Top level expolygons of an even-odd union", "[ClipperUtils]") {
    // Create a polygon representing unit box
    Polygon unitbox;
    const auto UNIT = coord_t(1. / SCALING_FACTOR);
    unitbox.points = { Vec2crd{0, 0}, Vec2crd{UNIT, 0}, Vec2crd{UNIT, UNIT}, Vec2crd{0, UNIT}};

    Polygon box_frame = unitbox;
    box_frame.scale(20, 10);

    Polygon hole_left = unitbox;
    hole_left.scale(8);
    hole_left.translate(UNIT, UNIT);
    hole_left.reverse();

    Polygon hole_right = hole_left;
    hole_right.translate(UNIT * 10, 0);

    Polygon inner_left = unitbox;
    inner_left.scale(4);
    inner_left.translate(UNIT * 3, UNIT * 3);

    Polygon inner_right = inner_left;
    inner_right.translate(UNIT * 10, 0);

    ExPolygons reference;
    for (const Polygon &polygon : union_({box_frame, hole_left, hole_right, inner_left, inner_right}))
        reference.emplace_back(polygon);

    double area_sum = box_frame.area() + hole_left.area() +
                      hole_right.area() + inner_left.area() +
                      inner_right.area();

    REQUIRE(area_sum > 0);

    ExPolygons nested;
    ExPolygons top_level = top_level_expolygons(reference, &nested);
    auto area = [](const ExPolygons &expolys) {
        return std::accumulate(expolys.begin(), expolys.end(), 0.0, [](double a, const ExPolygon &e) { return a + e.area(); });
    };
    REQUIRE(top_level.size() == 1);
    REQUIRE(top_level.front().holes.size() == 2);
    REQUIRE(nested.size() == 2);
    REQUIRE(area_sum == Catch::Approx(area(top_level) + area(nested)));
    REQUIRE(top_level_expolygons(reference).size() == 1);
}

// Rings flattened to x,y,x,y... and sorted, with each ring rotated to start at its lowest point: two
// encodings of the same geometry compare equal however the pieces came back or wherever a ring started.
static std::vector<std::vector<coord_t>> canonical_rings(const ExPolygons &expolygons)
{
    std::vector<std::vector<coord_t>> rings;
    const auto add = [&rings](const Polygon &poly) {
        if (poly.points.empty())
            return;
        Points pts = poly.points;
        std::rotate(pts.begin(),
                    std::min_element(pts.begin(), pts.end(), [](const Point &a, const Point &b) {
                        return std::make_pair(a.x(), a.y()) < std::make_pair(b.x(), b.y());
                    }),
                    pts.end());
        std::vector<coord_t> flat;
        flat.reserve(pts.size() * 2);
        for (const Point &p : pts) {
            flat.emplace_back(p.x());
            flat.emplace_back(p.y());
        }
        rings.emplace_back(std::move(flat));
    };
    for (const ExPolygon &expoly : expolygons) {
        add(expoly.contour);
        for (const Polygon &hole : expoly.holes)
            add(hole);
    }
    std::sort(rings.begin(), rings.end());
    return rings;
}

// The same rings, every coordinate within `tolerance`.
static bool same_rings(const ExPolygons &a, const ExPolygons &b, coord_t tolerance)
{
    const std::vector<std::vector<coord_t>> ra = canonical_rings(a), rb = canonical_rings(b);
    return std::equal(ra.begin(), ra.end(), rb.begin(), rb.end(), [tolerance](const std::vector<coord_t> &x, const std::vector<coord_t> &y) {
        return std::equal(x.begin(), x.end(), y.begin(), y.end(), [tolerance](coord_t u, coord_t v) { return std::abs(u - v) <= tolerance; });
    });
}

TEST_CASE("Tiled diff and intersection return the same polygons as the plain calls", "[ClipperUtils]") {
    // A grid of disjoint framed squares, enough of them to be split into several tiles.
    const int  n    = 40;
    const coord_t cell = scaled<coord_t>(2.), side = scaled<coord_t>(1.5), frame = scaled<coord_t>(0.3);
    ExPolygons subject;
    for (int y = 0; y < n; ++ y)
        for (int x = 0; x < n; ++ x) {
            const Point o(x * cell, y * cell);
            ExPolygon square(Polygon({ o, o + Point(side, 0), o + Point(side, side), o + Point(0, side) }));
            Polygon hole({ o + Point(frame, frame), o + Point(frame, side - frame), o + Point(side - frame, side - frame), o + Point(side - frame, frame) });
            square.holes.emplace_back(std::move(hole));
            subject.emplace_back(std::move(square));
        }
    // Clip polygons crossing many squares, one of them large with holes of its own.
    Polygons clip;
    const coord_t span = n * cell;
    for (int i = 0; i < 8; ++ i) {
        const coord_t y0 = coord_t(i) * span / 8, y1 = y0 + scaled<coord_t>(0.9);
        clip.emplace_back(Polygon({ Point(- cell, y0), Point(span, y0 + cell * 3), Point(span, y1 + cell * 3), Point(- cell, y1) }));
    }
    ExPolygon big(Polygon({ Point(span / 4, span / 4), Point(3 * span / 4, span / 4), Point(3 * span / 4, 3 * span / 4), Point(span / 4, 3 * span / 4) }));
    for (int i = 0; i < 4; ++ i) {
        const Point o(span / 4 + scaled<coord_t>(3.1) + i * scaled<coord_t>(9.7), span / 4 + scaled<coord_t>(5.3));
        big.holes.emplace_back(Polygon({ o, o + Point(0, scaled<coord_t>(20.)), o + Point(scaled<coord_t>(5.), scaled<coord_t>(20.)), o + Point(scaled<coord_t>(5.), 0) }));
    }
    polygons_append(clip, to_polygons(big));

    const ApplySafetyOffset safety = GENERATE(ApplySafetyOffset::No, ApplySafetyOffset::Yes);

    // The point of the fixture: below 128 pieces the helpers fall back to a single tile and the tiled
    // path under test is never taken.
    REQUIRE(ClipperUtils::tile_expolygons(subject, 32).size() > 1);

    // With the safety offset a tile unites fewer clip polygons, so Clipper2 can round a crossing 1 unit differently.
    const coord_t tolerance = safety == ApplySafetyOffset::Yes ? 1 : 0;

    const ExPolygons diff_plain = diff_ex(subject, clip, safety);
    const ExPolygons diff_tiled = diff_ex_by_piece(subject, clip, safety);
    REQUIRE(area(diff_plain) > 0.);
    CHECK(same_rings(diff_tiled, diff_plain, tolerance));

    const ExPolygons intersection_plain = intersection_ex(subject, clip, safety);
    const ExPolygons intersection_tiled = intersection_ex_by_piece(subject, clip, safety);
    REQUIRE(area(intersection_plain) > 0.);
    CHECK(same_rings(intersection_tiled, intersection_plain, tolerance));
}

TEST_CASE("Subtracting a far expansion gives the same polygons as diff with expand", "[ClipperUtils]") {
    // As bridge_over_infill uses it: the lower layer's solids, each shrunk by the solid infill spacing on its own, expanded by
    // two or four spacings.
    const float spacing = scaled<float>(0.45);
    const float delta   = GENERATE(2.f, 4.f) * spacing;
    const auto  expanded_diff = [delta](const Polygons &subject, const Polygons &clip) { return diff(subject, expand(clip, delta)); };
    // Whether the smaller round expansion that diff_expanded tries first covers the subject on its own.
    const auto  coarse_covers = [delta](const Polygons &subject, const Polygons &coarse) {
        return diff(subject, expand(coarse, 0.7f * delta, jtRound, 0.01 * delta)).empty();
    };
    const auto  rectangle = [](double x0, double y0, double x1, double y1) {
        return Polygon::new_scale({ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } });
    };
    // A rectangle whose sides are sawteeth 12 µm deep, as the slices of a relief are.
    const auto  sawtooth = [](double x0, double y0, double x1, double y1) {
        const Vec2d corners[] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        Polygon     out;
        for (size_t i = 0; i < 4; ++ i) {
            const Vec2d a = corners[i], b = corners[(i + 1) % 4], outward = Vec2d((b - a).y(), (a - b).x()).normalized();
            const int   teeth = int(std::round((b - a).norm() / 0.024));
            for (int t = 0; t < teeth; ++ t) {
                out.points.emplace_back(Point::new_scale(a + (b - a) * (t / double(teeth))));
                out.points.emplace_back(Point::new_scale(a + (b - a) * ((t + 0.5) / teeth) + 0.012 * outward));
            }
        }
        return out;
    };

    SECTION("A crack between solids shrunk apart is covered") {
        const Polygons solids = shrink(Polygons{ rectangle(0., 0., 10., 10.), rectangle(10., 0., 20., 10.) }, spacing);
        const Polygons subject{ rectangle(2., 2., 18., 8.) };
        REQUIRE_FALSE(diff(subject, solids).empty());
        CHECK(coarse_covers(subject, polygons_simplify(solids, delta / 8.)));
        CHECK(expanded_diff(subject, solids).empty());
        CHECK(diff_expanded(subject, solids, delta).empty());
    }
    SECTION("A crack between detailed solids shrunk apart is covered") {
        const Polygons solids = shrink(Polygons{ sawtooth(0., 0., 10., 10.), sawtooth(10., 0., 20., 10.) }, spacing);
        const Polygons subject{ rectangle(2., 2., 18., 8.) };
        REQUIRE_FALSE(diff(subject, solids).empty());
        // The shrink's miters cut steep teeth about 1.4 spacings deep, which widens the crack to about what the coarse
        // expansion reaches at two spacings. So only the default reach, four spacings, is sure to take the early-out.
        if (delta > 3.f * spacing)
            CHECK(coarse_covers(subject, polygons_simplify(solids, delta / 8.)));
        CHECK(expanded_diff(subject, solids).empty());
        CHECK(diff_expanded(subject, solids, delta).empty());
    }
    SECTION("Strips too thin to survive the shrink leave the subject unsupported") {
        const Polygons solids = shrink(Polygons{ rectangle(0., 0., 10., 10.), rectangle(10., 0., 10.6, 10.), rectangle(11.2, 0., 11.8, 10.),
                                                 rectangle(12.4, 0., 13., 10.) },
                                       spacing);
        const Polygons subject{ rectangle(1., 1., 14., 9.) };
        const Polygons expected = expanded_diff(subject, solids);
        REQUIRE_FALSE(expected.empty());
        CHECK(diff_expanded(subject, solids, delta) == expected);
    }
    SECTION("A hole wider than the expansion leaves the subject over it") {
        Polygon hole = rectangle(5., 5., 15., 15.);
        hole.reverse();
        const Polygons solids{ rectangle(0., 0., 20., 20.), hole };
        const Polygons subject{ rectangle(2., 2., 18., 18.) };
        const Polygons expected = expanded_diff(subject, solids);
        REQUIRE_FALSE(expected.empty());
        CHECK(diff_expanded(subject, solids, delta) == expected);
    }
    SECTION("A hole that simplification removes is covered") {
        Polygon hole = rectangle(5., 9.975, 10., 10.025);
        hole.reverse();
        const Polygons solids{ rectangle(0., 0., 20., 20.), hole };
        const Polygons subject{ rectangle(2., 2., 18., 18.) };
        REQUIRE(polygons_simplify({ hole }, delta / 8.).empty());
        CHECK(expanded_diff(subject, solids).empty());
        CHECK(diff_expanded(subject, solids, delta).empty());
    }
    SECTION("A thin C that simplification would turn into a hole is kept as it is") {
        // A C 100 µm wide over a 10 mm radius at a tolerance of 0.5 mm, scaled with the tolerance. Arcs of 3,200 and 2,500
        // points leave 0.05 radians open at each end, and the path starts 4,500 points in.
        const double scale = delta / 8. / scaled<double>(0.5);
        const auto   arc   = [](double radius, int n, Points &out) {
            for (int i = 0; i < n; ++ i) {
                const double a = 0.05 + (2. * PI - 0.1) * i / (n - 1);
                out.emplace_back(coord_t(std::round(radius * std::cos(a))), coord_t(std::round(radius * std::sin(a))));
            }
        };
        Points outer, inner;
        arc(std::round(scale * scaled<double>(10.)) + std::round(scale * scaled<double>(0.1)), 3200, outer);
        arc(std::round(scale * scaled<double>(10.)), 2500, inner);
        Polygon c(std::move(outer));
        c.points.insert(c.points.end(), inner.rbegin(), inner.rend());
        std::rotate(c.points.begin(), c.points.begin() + 4500, c.points.end());
        REQUIRE(c.is_counter_clockwise());
        REQUIRE(area(polygons_simplify({ c }, delta / 8.)) < 0.);
        // A square beside the C, halfway round, which its own round expansion covers.
        const coord_t  x = coord_t(-scale * scaled<double>(10.05)), half = coord_t(scale * scaled<double>(0.2));
        const Polygons subject{ Polygon({ { x - half, -half }, { x + half, -half }, { x + half, half }, { x - half, half } }) };
        CHECK(coarse_covers(subject, { c }));
        CHECK(expanded_diff(subject, { c }).empty());
        CHECK(diff_expanded(subject, { c }, delta).empty());
    }
    SECTION("A path that simplifies into several contours and holes keeps its sign") {
        // A band 1 mm wide winding once and a radian more, its end 20 µm outside its start, then on as a strand 40 µm wide for
        // two radians. Simplified, the end crosses the start, closing a ring around holes, and the strand's sides cross each other.
        const double radius = scaled<double>(5.), width = scaled<double>(1.), pitch = width + scaled<double>(0.02), band_end = 2. * PI + 1.,
                     end = band_end + 2.;
        const int    n      = 6000;
        Points       outer, inner;
        for (int i = 0; i < n; ++ i) {
            const double t = end * i / (n - 1), centre = radius + pitch * t / (2. * PI), half = t <= band_end ? width / 2. : scaled<double>(0.02);
            outer.emplace_back(coord_t(std::round((centre + half) * std::cos(t))), coord_t(std::round((centre + half) * std::sin(t))));
            inner.emplace_back(coord_t(std::round((centre - half) * std::cos(t))), coord_t(std::round((centre - half) * std::sin(t))));
        }
        Polygon band(std::move(outer));
        band.points.insert(band.points.end(), inner.rbegin(), inner.rend());
        REQUIRE(band.is_counter_clockwise());
        Points path = MultiPoint::_douglas_peucker(to_polyline(band).points, delta / 8.);
        path.pop_back();
        const Polygons simplified = polygons_simplify({ band }, delta / 8.);
        const size_t   contours   = size_t(std::count_if(simplified.begin(), simplified.end(), [](const Polygon &p) { return p.is_counter_clockwise(); }));
        CHECK(contours >= 2);
        CHECK(simplified.size() - contours >= 1);
        CHECK((area(simplified) > 0.) == (Polygon(path).area() > 0.));
        const Polygons subject{ rectangle(-8., -8., 8., 8.) };
        CHECK(diff_expanded(subject, { band }, delta) == expanded_diff(subject, { band }));
    }
    SECTION("Random solids give the same polygons") {
        // Overlapping rectangles, some too thin to survive the shrink, and jittered rings, under the region they cover and on
        // every other seed a rectangle more.
        size_t empty = 0, unsupported = 0;
        for (unsigned seed = 1; seed <= 24; ++ seed) {
            std::mt19937 rng(seed);
            const auto   uniform = [&rng](double from, double to) { return from + (to - from) * double(rng()) / double(std::mt19937::max()); };
            Polygons     solids;
            for (int i = 0; i < 6; ++ i) {
                const double x = uniform(0., 40.), y = uniform(0., 40.);
                solids.emplace_back(rectangle(x, y, x + uniform(0.3, 12.), y + uniform(0.3, 12.)));
            }
            for (int i = 0; i < 2; ++ i) {
                const Vec2d  centre(uniform(5., 45.), uniform(5., 45.));
                const double r = uniform(2., 8.), r_hole = r * uniform(0.3, 0.7);
                Polygon      ring, hole;
                for (int k = 0; k < 96; ++ k) {
                    const Vec2d direction(std::cos(2. * PI * k / 96), std::sin(2. * PI * k / 96));
                    ring.points.emplace_back(Point::new_scale(centre + r * uniform(0.97, 1.03) * direction));
                    hole.points.emplace_back(Point::new_scale(centre + r_hole * uniform(0.97, 1.03) * direction));
                }
                hole.reverse();
                solids.emplace_back(std::move(ring));
                solids.emplace_back(std::move(hole));
            }
            Polygons covered = solids;
            if (seed % 2 == 0) {
                const double x = uniform(0., 40.), y = uniform(0., 40.);
                covered.emplace_back(rectangle(x, y, x + uniform(2., 10.), y + uniform(2., 10.)));
            }
            const Polygons subject  = shrink(union_(covered), spacing);
            const Polygons shrunk   = shrink(solids, spacing);
            const Polygons expected = expanded_diff(subject, shrunk);
            CHECK(diff_expanded(subject, shrunk, delta) == expected);
            ++ (expected.empty() ? empty : unsupported);
        }
        // The seeds give both outcomes: subjects the solids support entirely, and subjects with unsupported parts left.
        CHECK(empty > 0);
        CHECK(unsupported > 0);
    }
}

// An outline with a small clockwise hole well inside it, cut along its edge by three clip polygons that never reach the
// hole. Clipper2 puts the hole at the top level of its PolyTree, beside the outline rather than under it. The coordinates
// come from a sliced layer reduced to the few paths that still do this; most changes to them don't.
static const Polygons misnesting_subject{
    Polygon({ { 23000000, 31000000 }, { 7500000, 25900000 }, { 200000, 26450004 }, { 0, 26450004 }, { 0, 26550004 },
              { -100000, 26550004 }, { -300000, 26600000 }, { 13000000, 0 } }),
    Polygon({ { 10200000, 24250000 }, { 10500000, 24250000 }, { 9800000, 24000000 } }),
    Polygon({ { 4500000, 22500000 }, { 5000000, 22000000 }, { 4000000, 22000000 } }),
};
static const Polygons misnesting_clip{
    Polygon({ { 0, 26550004 }, { -200000, 26550004 }, { -200000, 26000000 } }),
    Polygon({ { 0, 26450004 }, { 100000, 26450004 }, { 0, 26000000 } }),
    Polygon({ { 10000000, 24000000 }, { 10199999, 24250000 }, { 10500000, 24250000 }, { 8000000, 26500000 } }),
};

// The case above inside a frame, a square outline with a square hole around the case, and with an island in its hole.
// Clipper2 then puts the hole beside the outline inside the frame's hole, where the frame and the outline both enclose
// it, and the island under the misplaced hole.
static Polygons misnesting_framed_subject()
{
    Polygons subject = misnesting_subject;
    subject.emplace_back(Polygon({ { -2000000, -2000000 }, { 25000000, -2000000 }, { 25000000, 33000000 }, { -2000000, 33000000 } }));
    subject.emplace_back(Polygon({ { -1000000, -1000000 }, { -1000000, 32000000 }, { 24000000, 32000000 }, { 24000000, -1000000 } }));
    subject.emplace_back(Polygon({ { 4400000, 22100000 }, { 4600000, 22100000 }, { 4500000, 22300000 } }));
    return subject;
}
// In the hole of the framed case, beside its island.
static const Point in_framed_hole(4500000, 22400000);
// In the island of the framed case.
static const Point in_framed_island(4500000, 22150000);

static void check_orientation(const ExPolygons &expolygons)
{
    for (const ExPolygon &expoly : expolygons) {
        CHECK(expoly.contour.is_counter_clockwise());
        for (const Polygon &hole : expoly.holes)
            CHECK(hole.is_clockwise());
    }
}

// How many of the expolygons contain the point, each on its own, as a per-piece operation sees them.
static size_t pieces_containing(const ExPolygons &expolygons, const Point &pt)
{
    return std::count_if(expolygons.begin(), expolygons.end(), [&pt](const ExPolygon &expoly) { return expoly.contains(pt); });
}

TEST_CASE("Every hole of a boolean result belongs to the expolygon around it", "[ClipperUtils]") {
    const Polygon &hole = misnesting_subject.back();
    REQUIRE(hole.is_clockwise());
    const ExPolygons result = diff_ex(misnesting_subject, misnesting_clip);
    check_orientation(result);
    CHECK(pieces_containing(result, hole.centroid()) == 0);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(misnesting_subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A misplaced hole goes to the innermost contour around it, an island in it stays filled", "[ClipperUtils]") {
    const Polygons   subject = misnesting_framed_subject();
    const ExPolygons result  = diff_ex(subject, misnesting_clip);
    check_orientation(result);
    // Given to the frame instead of the outline, the hole would leave the outline covering it.
    CHECK(pieces_containing(result, in_framed_hole) == 0);
    CHECK(pieces_containing(result, in_framed_island) == 1);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A misplaced hole touching the contour around it goes to that contour", "[ClipperUtils]") {
    // The first two vertices of the hole the subject's triangle cuts round a fraction of a unit outside the outline, the
    // third is well inside it. Reduced from a sliced layer; Clipper2 puts the hole at the top level of its PolyTree.
    const Polygons subject{
        Polygon({ { 1000000, 5000000 }, { 1000000, 4000000 }, { 6000000, 0 } }),
        Polygon({ { 1024999, 4500000 }, { 1100011, 4574998 }, { 1100011, 4450000 } }),
    };
    const Polygons clip{
        Polygon({ { 1500000, 5000000 }, { 1062508, 4612507 }, { 1212486, 4462504 } }),
        Polygon({ { 1100000, 4430000 }, { 1024998, 4500001 }, { 2000000, 3000000 } }),
    };
    const ExPolygons result = diff_ex(subject, clip);
    check_orientation(result);
    CHECK(pieces_containing(result, subject.back().centroid()) == 0);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, clip)), 1e-9));
}

TEST_CASE("A misplaced island comes out as an expolygon of its own", "[ClipperUtils]") {
    // Clipper2 puts the island of this result directly under the outer contour, at the level of its holes, instead of
    // under the hole around it. Reduced from a sliced layer.
    const Polygons subject{
        Polygon({ { 5200000, 700000 }, { 0, 5000000 }, { 0, 0 } }),
        Polygon({ { 3100000, 825000 }, { 3250000, 900001 }, { 3250000, 1000000 }, { 4000000, 1500000 }, { 3000000, 500000 } }),
    };
    const Polygons clip{
        Polygon({ { 4500000, 1200000 }, { 4000000, 1000000 }, { 5000000, 800000 } }),
        Polygon({ { 3250000, 900001 }, { 3250000, 1000000 }, { 3000000, 1000000 }, { 3500000, 1100000 }, { 3100000, 1000000 }, { 3183343, 866672 } }),
        Polygon({ { 4000000, 1000000 }, { 4000000, 1000000 }, { 4000000, 1000000 } }),
    };
    const ExPolygons result = diff_ex(subject, clip);
    check_orientation(result);
    // In the island.
    CHECK(pieces_containing(result, Point(3230000, 1020000)) == 1);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, clip)), 1e-9));
}

TEST_CASE("Nested islands of an even-odd union keep their misplaced holes", "[ClipperUtils]") {
    // The framed case as one even-odd union: the frame is the top level, everything in its hole is nested, and Clipper2
    // puts the case's hole beside the outline among the nested islands.
    ExPolygons input;
    for (const Polygon &polygon : misnesting_framed_subject())
        input.emplace_back(polygon);
    for (const Polygon &polygon : misnesting_clip)
        input.emplace_back(polygon);
    ExPolygons       nested;
    const ExPolygons top_level = top_level_expolygons(input, &nested);
    REQUIRE(top_level.size() == 1);
    check_orientation(nested);
    CHECK(pieces_containing(nested, in_framed_hole) == 0);
    CHECK(pieces_containing(nested, in_framed_island) == 1);
}

TEST_CASE("Loops chained outside in keep their nesting where Clipper2 misplaces one", "[ClipperUtils]") {
    // An even-odd union of the case's paths leaves the hole at the top level of the PolyTree, among the outer loops.
    Polygons loops = misnesting_subject;
    polygons_append(loops, misnesting_clip);
    const Polygons chained = union_pt_chained_outside_in(loops);
    REQUIRE(chained.size() == 7);
    for (const Polygon &loop : chained)
        CHECK(loop.is_counter_clockwise());
    // The hole's loop comes right after the outline's, which has no other loop inside it.
    const auto outline = std::max_element(chained.begin(), chained.end(), [](const Polygon &a, const Polygon &b) { return a.area() < b.area(); });
    REQUIRE(outline + 1 != chained.end());
    CHECK((outline + 1)->area() == Catch::Approx(- misnesting_subject.back().area()));
}

TEST_CASE("A misplaced hole hugging an island inside it still goes to the outline", "[ClipperUtils]") {
    // The case's hole with an island filling most of it, 20 units from it: the hole's vertices all lie close to the
    // island and its middle inside it, yet the island is inside the hole, not around it.
    Polygons subject = misnesting_subject;
    subject.emplace_back(Polygon({ { 4000020, 22000010 }, { 4500000, 22150000 }, { 4999980, 22000010 },
                                   { 4650000, 22160000 }, { 4500000, 22499980 }, { 4350000, 22160000 } }));
    const ExPolygons result = diff_ex(subject, misnesting_clip);
    check_orientation(result);
    // In the hole below the island, and in the island.
    CHECK(pieces_containing(result, Point(4500000, 22050000)) == 0);
    CHECK(pieces_containing(result, Point(4500000, 22300000)) == 1);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A concave misplaced hole touching the outline at every vertex goes to it", "[ClipperUtils]") {
    // The case with the outline drawn in to two more vertices and the hole replaced by a concave pentagon, each of its
    // vertices within 3 units of the outline; the middle of its bounding box is outside both.
    Polygons subject = misnesting_subject;
    subject.front().points.insert(subject.front().points.end(), { { 7000000, 20000000 }, { 20000000, 22000000 } });
    subject.back() = Polygon({ { 1500002, 22999999 }, { 20333332, 22999999 }, { 19999999, 22000001 }, { 6999998, 20000001 }, { 12999995, 13 } });
    const ExPolygons result = diff_ex(subject, misnesting_clip);
    check_orientation(result);
    CHECK(pieces_containing(result, Point(10000000, 22500000)) == 0);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A point strictly inside a polygon, where there is one", "[ClipperUtils]") {
    // A notch with a horizontal edge at the middle of its bounding box, so a line through that middle runs along the edge;
    // two lobes joined by a one unit neck and a square with a one unit spike, where the widest band between vertex heights
    // runs through the thin part.
    const Polygons wide{
        Polygon({ { 0, 0 }, { 2000000, 0 }, { 2000000, 5000000 }, { 8000000, 5000000 }, { 8000000, 0 }, { 10000000, 0 },
                  { 10000000, 10000000 }, { 0, 10000000 } }),
        Polygon({ { 0, 0 }, { 100, 0 }, { 100, 5 }, { 51, 5 }, { 51, 25 }, { 100, 25 }, { 100, 30 }, { 0, 30 }, { 0, 25 },
                  { 50, 25 }, { 50, 5 }, { 0, 5 } }),
        Polygon({ { 0, 0 }, { 1000000, 0 }, { 1000000, 1000000 }, { 500001, 1000000 }, { 500001, 5000000 }, { 500000, 5000000 },
                  { 500000, 1000000 }, { 0, 1000000 } }),
    };
    for (Polygon polygon : wide)
        for (int winding = 0; winding < 2; ++ winding, polygon.reverse()) {
            const std::optional<Point> pt = ClipperUtils::interior_point(polygon);
            REQUIRE(pt);
            CHECK(contains(polygon, *pt, false));
        }
    // One unit thick everywhere, these hold no grid point strictly inside: a strip, and rows one unit high joined at the
    // left.
    CHECK_FALSE(ClipperUtils::interior_point(Polygon({ { 0, 0 }, { 1000, 0 }, { 1000, 1 }, { 0, 1 } })));
    Polygon rows({ { 0, 0 }, { 100, 0 } });
    for (coord_t y = 1; y < 100; y += 2)
        rows.points.insert(rows.points.end(), { { 100, y }, { 1, y }, { 1, y + 1 }, { 100, y + 1 } });
    rows.points.emplace_back(0, 100);
    CHECK_FALSE(ClipperUtils::interior_point(rows));
}

TEST_CASE("A correctly nested result converts in the order of its tree", "[ClipperUtils]") {
    // Two squares, one with a hole holding an island, the other with two holes and a corner cut off. Clipper2 nests this
    // correctly, so the expolygons follow its tree, each path as Clipper2 returned it: an outline with its holes in tree
    // order, then the islands of those holes, outline by outline.
    const Polygons subject{
        Polygon({ { 0, 0 }, { 10000000, 0 }, { 10000000, 10000000 }, { 0, 10000000 } }),
        Polygon({ { 2000000, 2000000 }, { 2000000, 8000000 }, { 8000000, 8000000 }, { 8000000, 2000000 } }),
        Polygon({ { 4000000, 4000000 }, { 6000000, 4000000 }, { 6000000, 6000000 }, { 4000000, 6000000 } }),
        Polygon({ { 20000000, 0 }, { 32000000, 0 }, { 32000000, 12000000 }, { 20000000, 12000000 } }),
        Polygon({ { 21000000, 1000000 }, { 21000000, 4000000 }, { 24000000, 4000000 }, { 24000000, 1000000 } }),
        Polygon({ { 26000000, 6000000 }, { 26000000, 10000000 }, { 30000000, 10000000 }, { 30000000, 6000000 } }),
    };
    const Polygons clip{ Polygon({ { 31000000, -1000000 }, { 33000000, -1000000 }, { 33000000, 1000000 }, { 31000000, 1000000 } }) };
    ExPolygons expected(3);
    expected[0].contour = Polygon({ { 31000000, 1000000 }, { 32000000, 1000000 }, { 32000000, 12000000 }, { 20000000, 12000000 },
                                    { 20000000, 0 }, { 31000000, 0 } });
    expected[0].holes   = { Polygon({ { 26000000, 10000000 }, { 30000000, 10000000 }, { 30000000, 6000000 }, { 26000000, 6000000 } }),
                            Polygon({ { 21000000, 4000000 }, { 24000000, 4000000 }, { 24000000, 1000000 }, { 21000000, 1000000 } }) };
    expected[1].contour = Polygon({ { 10000000, 10000000 }, { 0, 10000000 }, { 0, 0 }, { 10000000, 0 } });
    expected[1].holes   = { Polygon({ { 2000000, 8000000 }, { 8000000, 8000000 }, { 8000000, 2000000 }, { 2000000, 2000000 } }) };
    expected[2].contour = Polygon({ { 6000000, 6000000 }, { 4000000, 6000000 }, { 4000000, 4000000 }, { 6000000, 4000000 } });
    CHECK(diff_ex(subject, clip) == expected);
}

TEST_CASE("Loops of a correctly nested union chain in the order of its tree", "[ClipperUtils]") {
    // Three nested squares and two more beside them, as concentric fill hands them over: chained outside in, depth first,
    // each loop as Clipper2 returned it, the holes reversed to run counter-clockwise.
    const Polygons loops{
        Polygon({ { 0, 0 }, { 10000000, 0 }, { 10000000, 10000000 }, { 0, 10000000 } }),
        Polygon({ { 1000000, 1000000 }, { 9000000, 1000000 }, { 9000000, 9000000 }, { 1000000, 9000000 } }),
        Polygon({ { 2000000, 2000000 }, { 8000000, 2000000 }, { 8000000, 8000000 }, { 2000000, 8000000 } }),
        Polygon({ { 20000000, 0 }, { 32000000, 0 }, { 32000000, 12000000 }, { 20000000, 12000000 } }),
        Polygon({ { 21000000, 1000000 }, { 31000000, 1000000 }, { 31000000, 11000000 }, { 21000000, 11000000 } }),
    };
    const Polygons expected{
        Polygon({ { 10000000, 10000000 }, { 0, 10000000 }, { 0, 0 }, { 10000000, 0 } }),
        Polygon({ { 9000000, 1000000 }, { 9000000, 9000000 }, { 1000000, 9000000 }, { 1000000, 1000000 } }),
        Polygon({ { 8000000, 8000000 }, { 2000000, 8000000 }, { 2000000, 2000000 }, { 8000000, 2000000 } }),
        Polygon({ { 32000000, 12000000 }, { 20000000, 12000000 }, { 20000000, 0 }, { 32000000, 0 } }),
        Polygon({ { 31000000, 1000000 }, { 31000000, 11000000 }, { 21000000, 11000000 }, { 21000000, 1000000 } }),
    };
    CHECK(union_pt_chained_outside_in(loops) == expected);
}

TEST_CASE("A clockwise sliver with nothing around it comes back as Clipper2 returned it", "[ClipperUtils]") {
    // Rounding leaves a clockwise sliver of half a square unit beside the result's one piece, outside it. Reduced from a
    // sliced layer. It is no hole of anything, so it stays a contour of its own, neither dropped nor given to the piece.
    const Polygons subject{ Polygon({ { 875000, 200000 }, { 900002, 175002 }, { 950001, 150004 } }) };
    const Polygons clip{ Polygon({ { 879409, 185299 }, { 950000, 150004 }, { 1000000, 200000 } }) };
    const ExPolygons result = diff_ex(subject, clip);
    REQUIRE(result.size() == 2);
    CHECK(std::count_if(result.begin(), result.end(), [](const ExPolygon &expoly) { return expoly.contour.is_clockwise() && expoly.holes.empty(); }) == 1);
    CHECK(std::all_of(result.begin(), result.end(), [](const ExPolygon &expoly) { return expoly.holes.empty(); }));
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, clip)), 1e-9));
}
