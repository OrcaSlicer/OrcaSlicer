#include <catch2/catch_all.hpp>

#include <numeric>
#include <iostream>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;

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
                REQUIRE(result == Polygons {
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 155, 145 }, { 145, 145 }, { 145, 155 }, { 155, 155 } } });
            }
        }
        WHEN("offset_ex") {
            ExPolygons result = Slic3r::offset_ex(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(result == ExPolygons { {
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 145, 145 }, { 145, 155 }, { 155, 155 }, { 155, 145 } } } } );
            }
        }
        WHEN("offset2_ex") {
            ExPolygons result = Slic3r::offset2_ex({ square_with_hole }, 5.f, -2.f);
            THEN("offset matches") {
                REQUIRE(result == ExPolygons { {
                    { { 203, 203 }, { 97, 203 }, { 97, 97 }, { 203, 97 } },
                    { { 143, 143 }, { 143, 157 }, { 157, 157 }, { 157, 143 } } } } );
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

template<e_ordering o = e_ordering::OFF, class P, class Tree, class Alloc>
double polytree_area(const Tree &tree, std::vector<P, Alloc> *out)
{
    traverse_pt<o>(tree, out);

    return std::accumulate(out->begin(), out->end(), 0.0,
                           [](double a, const P &p) { return a + p.area(); });
}

size_t count_polys(const ExPolygons& expolys)
{
    size_t c = 0;
    for (auto &ep : expolys) c += ep.holes.size() + 1;

    return c;
}

TEST_CASE("Traversing Clipper PolyTree", "[ClipperUtils]") {
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

    Polygons reference = union_({box_frame, hole_left, hole_right, inner_left, inner_right});

    ClipperLib::PolyTree tree = union_pt(reference);
    double area_sum = box_frame.area() + hole_left.area() +
                      hole_right.area() + inner_left.area() +
                      inner_right.area();

    REQUIRE(area_sum > 0);

    SECTION("Traverse into Polygons WITHOUT spatial ordering") {
        Polygons output;
        REQUIRE(area_sum == Catch::Approx(polytree_area(tree.GetFirst(), &output)));
        REQUIRE(output.size() == reference.size());
    }

    SECTION("Traverse into ExPolygons WITHOUT spatial ordering") {
        ExPolygons output;
        REQUIRE(area_sum == Catch::Approx(polytree_area(tree.GetFirst(), &output)));
        REQUIRE(count_polys(output) == reference.size());
    }

    SECTION("Traverse into Polygons WITH spatial ordering") {
        Polygons output;
        REQUIRE(area_sum == Catch::Approx(polytree_area<e_ordering::ON>(tree.GetFirst(), &output)));
        REQUIRE(output.size() == reference.size());
    }

    SECTION("Traverse into ExPolygons WITH spatial ordering") {
        ExPolygons output;
        REQUIRE(area_sum == Catch::Approx(polytree_area<e_ordering::ON>(tree.GetFirst(), &output)));
        REQUIRE(count_polys(output) == reference.size());
    }
}
