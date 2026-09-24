#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/WipePathHelpers.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Slic3r;
using Slic3r::AABBTreeLines::LinesDistancer;

TEST_CASE("Stored wipe path retains its length around a curved wall after a seam gap", "[WipePath][Regression]")
{
    const int mirror = GENERATE(1, -1);
    const double wipe_length = GENERATE(0.8, 1.0);
    CAPTURE(mirror, wipe_length);
    // A 0.02 mm seam gap on a curved 0.24 mm wall leaves a short outgoing
    // segment whose inward offset backtracks. Coordinates use internal scaling
    // from the affected loop; the adjacent inner wall is already printed.
    const auto point = [mirror](coord_t x, coord_t y) { return Point(mirror * x, y); };
    const Polyline original{
        point(671861, 7772276), point(688586, 7765098), point(781082, 7687014),
        point(852059, 7608861), point(889773, 7556912), point(958919, 7382963),
        point(977048, 7259018), point(977325, 7173839), point(944250, 7039370),
        point(911230, 6952087), point(880323, 6894944), point(760243, 6763860),
        point(598587, 6641719), point(492626, 6593610), point(362533, 6543938),
        point(170173, 6513482), point(114917, 6509380), point(18418, 6513666),
        point(-145550, 6537251), point(-259087, 6580797), point(-413987, 6690495),
        point(-485220, 6767022), point(-561189, 6893573), point(-576897, 6965717),
        point(-595201, 7089303), point(-597977, 7164172), point(-590614, 7239553),
        point(-574031, 7305533), point(-539668, 7383293), point(-442552, 7550465),
        point(-332173, 7659644), point(-257819, 7717153), point(-209522, 7749563),
        point(-121695, 7793438), point(399, 7844160), point(228137, 7880068),
        point(363721, 7881585), point(431909, 7865985), point(569648, 7816149),
        point(653482, 7780164),
    };
    const Polyline inner{
        point(739251, 7241332), point(739377, 7202281), point(716708, 7110113),
        point(694416, 7051190), point(685088, 7033943), point(599527, 6940541),
        point(476243, 6847393), point(400952, 6813209), point(300851, 6774988),
        point(142724, 6749952), point(111379, 6747625), point(40681, 6750765),
        point(-85281, 6768883), point(-146010, 6792175), point(-256555, 6870462),
        point(-295251, 6912034), point(-336173, 6978125), point(-357996, 7111200),
        point(-359693, 7156985), point(-355612, 7198777), point(-348288, 7227916),
        point(-327412, 7275156), point(-252784, 7403618), point(-175201, 7480358),
        point(-89616, 7543582), point(-22802, 7576960), point(65459, 7613627),
        point(248096, 7642423), point(338175, 7643431), point(364673, 7637369),
        point(482204, 7594844), point(562221, 7560499), point(615604, 7515433),
        point(679764, 7441323), point(727601, 7320981), point(739251, 7241332),
    };
    const Point seam_start = original.first_point();
    const Point seam_end = original.last_point();
    const double offset = scale_(0.239999);
    Polyline forward = original;
    REQUIRE_FALSE(offset_wipe_path(forward, seam_start, seam_end, seam_end,
        -mirror, offset, scale_(wipe_length)));

    Polyline path = original;
    REQUIRE(offset_wipe_path_toward_support(path, seam_start, seam_end, seam_end,
        -mirror, offset, scale_(wipe_length), inner.lines(), inner.lines(), original.lines(), offset));
    REQUIRE(path.first_point() == seam_start);
    REQUIRE(path.points.size() > 2);
    // Wipe::wipe replaces the sentinel with the actual extrusion endpoint.
    path.points.front() = seam_end;
    CHECK_THAT(unscale_(path.length()), Catch::Matchers::WithinAbs(wipe_length, 0.0004));
    CHECK(mirror * (path.points[1].x() - seam_end.x()) < 0);
    CHECK(path.points[1].y() < seam_end.y());
    Lines support = inner.lines();
    const Lines current = original.lines();
    support.insert(support.end(), current.begin(), current.end());
    REQUIRE(wipe_path_support_score(path, seam_end, LinesDistancer<Line>(inner.lines()),
        LinesDistancer<Line>(support), offset).has_value());
}

TEST_CASE("Stored wipe path retains its length after a loop pre-move at a curved seam", "[WipePath][Regression]")
{
    const int mirror = GENERATE(1, -1);
    const bool pre_move = GENERATE(false, true);
    CAPTURE(mirror, pre_move);
    const auto point = [mirror](coord_t x, coord_t y) { return Point(mirror * x, y); };
    // A 0.02 mm seam gap on a curved 0.24 mm wall, with the adjacent inner
    // wall already printed. The loop pre-move advances the nozzle near the seam.
    const Polyline original{
        point(686772, 7813199), point(516334, 7887188), point(411017, 7915098),
        point(346190, 7926015), point(246957, 7925330), point(-16945, 7881611),
        point(-116443, 7842513), point(-255907, 7773886), point(-378126, 7681053),
        point(-499258, 7552879), point(-574414, 7438250), point(-613558, 7370259),
        point(-626313, 7341394), point(-650774, 7263424), point(-669964, 7169919),
        point(-666798, 7058662), point(-631336, 6876547), point(-624410, 6852946),
        point(-577832, 6762704), point(-517493, 6693143), point(-455794, 6631765),
        point(-315549, 6531304), point(-169627, 6468424), point(-1908, 6443726),
        point(143562, 6438395), point(314277, 6465470), point(380448, 6480795),
        point(519922, 6526617), point(673990, 6611110), point(801581, 6705610),
        point(927505, 6840928), point(969616, 6912718), point(1004269, 7009155),
        point(1044144, 7171737), point(1046617, 7228598), point(1028598, 7358360),
        point(950280, 7560914), point(867133, 7675444), point(732946, 7788889),
        point(706168, 7804780), point(705118, 7805235),
    };
    const Polyline inner{
        point(808905, 7211140), point(796801, 7298317), point(739603, 7446245),
        point(691575, 7512401), point(595745, 7593416), point(438069, 7661865),
        point(360694, 7682370), point(327119, 7688024), point(266586, 7687607),
        point(53302, 7653657), point(-20276, 7624745), point(-130307, 7570601),
        point(-218687, 7503470), point(-311907, 7404832), point(-371722, 7313599),
        point(-401113, 7262547), point(-420207, 7203763), point(-431422, 7149118),
        point(-429596, 7084952), point(-401187, 6939055), point(-379519, 6897073),
        point(-343546, 6855603), point(-301665, 6813940), point(-197879, 6739595),
        point(-104131, 6699197), point(19838, 6680942), point(129154, 6676936),
        point(268758, 6699077), point(316366, 6710103), point(424812, 6745731),
        point(545430, 6811879), point(642396, 6883697), point(735572, 6983824),
        point(753259, 7013976), point(776223, 7077887), point(808905, 7211140),
    };
    const Point seam_start = original.first_point();
    const Point seam_end = original.last_point();
    const Point wipe_start = pre_move ? point(652751, 7792162) : seam_end;
    const double offset = scale_(0.239999);
    Polyline path = original;
    REQUIRE(offset_wipe_path_toward_support(path, seam_start, seam_end, wipe_start,
        mirror, offset, scale_(0.8), inner.lines(), inner.lines(), original.lines(), offset));
    REQUIRE(path.first_point() == seam_start);
    path.points.front() = wipe_start;
    CHECK_THAT(unscale_(path.length()), Catch::Matchers::WithinAbs(0.8, 0.0004));
    Lines support = inner.lines();
    const Lines current = original.lines();
    support.insert(support.end(), current.begin(), current.end());
    REQUIRE(wipe_path_support_score(path, wipe_start, LinesDistancer<Line>(inner.lines()),
        LinesDistancer<Line>(support), offset).has_value());
}

// Orca: helpers for constructing the extrusion geometry used by wipe tests.

static ExtrusionPath make_path(const std::vector<Point> &pts, ExtrusionRole role = erExternalPerimeter,
                               float width = 0.4f, float height = 0.2f)
{
    ExtrusionPath p(role, 0.5, width, height);
    for (const Point &pt : pts)
        p.polyline.append(Point3(pt.x(), pt.y(), coord_t(0)));
    return p;
}

static ExtrusionPaths make_paths(const std::vector<Point> &pts, ExtrusionRole role = erExternalPerimeter,
                                 float width = 0.4f)
{
    ExtrusionPaths paths;
    paths.push_back(make_path(pts, role, width));
    return paths;
}

TEST_CASE("Inward wipe support recognizes an inner wall starting on an overhang", "[WipePath][Regression]")
{
    const bool overhang_first = GENERATE(false, true);
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    ExtrusionPaths paths{
        make_path({point(0.4, 0.4), point(0.4, 2.)}, erOverhangPerimeter),
        make_path({point(0.4, 2.), point(0.4, 9.6), point(5.6, 9.6), point(5.6, 0.4), point(0.4, 0.4)}, erPerimeter)
    };
    if (!overhang_first)
        std::rotate(paths.begin(), paths.begin() + 1, paths.end());
    const ExtrusionLoop inner(paths);
    REQUIRE(inner.role() == (overhang_first ? erOverhangPerimeter : erPerimeter));

    WipeInwardSupport support;
    support.append(inner);
    REQUIRE(support.inner_lines.size() == inner.as_polyline().lines().size());
    // The overhanging portion itself is already printed and can support the wipe.
    const LinesDistancer<Line> inner_distancer(support.inner_lines);
    CHECK_THAT(inner_distancer.distance_from_lines<false>(point(0.4, 1.)),
               Catch::Matchers::WithinAbs(0., SCALED_EPSILON));
    const Polyline original{point(0., 0.), point(0., 10.), point(6., 10.), point(6., 0.), point(0., 0.)};
    Polyline wipe = original;
    REQUIRE(offset_wipe_path_toward_support(wipe, original.first_point(), original.first_point(),
        original.first_point(), -1, scale_(0.2), scale_(2.), support.inner_lines,
        support.printed_lines, original.lines(), scale_(0.6)));
    CHECK(wipe.points[1].x() > original.first_point().x());
}

TEST_CASE("Inward wipe support accumulates earlier walls without treating outer walls as targets", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    WipeInwardSupport support;
    const ExtrusionPath inner = make_path({point(0.4, 0.), point(0.4, 5.)}, erPerimeter);
    support.append(inner);
    const ExtrusionLoop outer(ExtrusionPaths{
        make_path({point(0., 0.), point(0., 5.)}, erOverhangPerimeter),
        make_path({point(0., 5.), point(-5., 5.), point(-5., 0.), point(0., 0.)})
    });
    support.append(outer);
    REQUIRE(support.inner_lines.size() == 1);
    REQUIRE(support.printed_lines.size() == 5);
    const LinesDistancer<Line> targets(support.inner_lines);
    CHECK_THAT(targets.distance_from_lines<false>(point(0., 2.)),
               Catch::Matchers::WithinAbs(scale_(0.4), SCALED_EPSILON));
}

static ExtrusionPaths make_loop_paths(const std::vector<Point> &contour_pts, float width = 0.4f)
{
    ExtrusionPaths paths;
    size_t mid = contour_pts.size() / 2;
    ExtrusionPath first(erExternalPerimeter, 0.5, width, 0.2f);
    for (size_t i = 0; i <= mid; ++i)
        first.polyline.append(Point3(contour_pts[i].x(), contour_pts[i].y(), coord_t(0)));
    ExtrusionPath second(erExternalPerimeter, 0.5, width, 0.2f);
    for (size_t i = mid; i < contour_pts.size(); ++i)
        second.polyline.append(Point3(contour_pts[i].x(), contour_pts[i].y(), coord_t(0)));
    second.polyline.append(Point3(contour_pts[0].x(), contour_pts[0].y(), coord_t(0)));
    paths.push_back(std::move(first));
    paths.push_back(std::move(second));
    return paths;
}

// Orca: sample_path_at_distance coverage.

TEST_CASE("sample_path_at_distance forward returns start for zero target", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s)});
    REQUIRE(sample_path_at_distance(paths, true, 0.0) == Point(0, 0));
}

TEST_CASE("sample_path_at_distance forward samples along path", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s)});

    Point result = sample_path_at_distance(paths, true, 50 * s);
    REQUIRE_THAT(result.x(), Catch::Matchers::WithinAbs(50 * s, 2));
    REQUIRE_THAT(result.y(), Catch::Matchers::WithinAbs(0, 2));
}

TEST_CASE("sample_path_at_distance forward crosses segment boundary", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s)});

    Point result = sample_path_at_distance(paths, true, 150 * s);
    REQUIRE_THAT(result.x(), Catch::Matchers::WithinAbs(100 * s, 2));
    REQUIRE_THAT(result.y(), Catch::Matchers::WithinAbs(50 * s, 2));
}

TEST_CASE("sample_path_at_distance backward from end", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s)});

    Point result = sample_path_at_distance(paths, false, 50 * s);
    REQUIRE_THAT(result.x(), Catch::Matchers::WithinAbs(100 * s, 2));
    REQUIRE_THAT(result.y(), Catch::Matchers::WithinAbs(50 * s, 2));
}

TEST_CASE("sample_path_at_distance on short path returns reachable point", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(0, 0), Point(10 * s, 0)});

    Point result = sample_path_at_distance(paths, true, 1000 * s);
    REQUIRE(result == Point(10 * s, 0));
}

TEST_CASE("sample_path_at_distance on zero-length path returns start", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(50 * s, 50 * s)});

    REQUIRE(sample_path_at_distance(paths, true, 100 * s) == Point(50 * s, 50 * s));
    REQUIRE(sample_path_at_distance(paths, false, 100 * s) == Point(50 * s, 50 * s));
}

TEST_CASE("Wipe offset direction follows the material side", "[WipePath]")
{
    REQUIRE(wipe_offset_direction(true,  false) == +1);
    REQUIRE(wipe_offset_direction(false, false) == -1);
    REQUIRE(wipe_offset_direction(true,  true) == -1);
    REQUIRE(wipe_offset_direction(false, true) == +1);
}

TEST_CASE("Stored wipe path leaves source crossings to support validation", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(10 * s, 0), Point(100 * s, 0), Point(coord_t(13.4 * s), coord_t(50 * s))};

    // Orca: crossing the just-printed wall is harmless for a non-extruding wipe.
    // The caller decides whether the result is supported by printed geometry.
    REQUIRE(offset_wipe_path(path, Point(10 * s, 0), Point(0, 0), Point(0, 0), +1, 5 * s, 1000 * s));
}

TEST_CASE("Stored wipe path builds the join after a nonzero seam gap", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(10 * s, 0), Point(10 * s, 0), Point(10 * s, 100 * s)};

    REQUIRE(offset_wipe_path(path, Point(10 * s, 0), Point(0, 0), Point(0, 0), +1, 5 * s, 1000 * s));
    REQUIRE(path.points.size() == 3);
    REQUIRE(path.points[1] == Point(5 * s, 5 * s));
    REQUIRE(path.points[2] == Point(5 * s, 100 * s));
    REQUIRE(path.fitting_result.size() == 1);
    REQUIRE(path.fitting_result.front().end_point_index == path.points.size() - 1);
}

TEST_CASE("Stored wipe path rejects an offset seam join that turns backward", "[WipePath][Regression]")
{
    const coord_t s = scale_(1.0);
    const Point seam_start(s, s);
    const Point seam_end(0, 0);
    Polyline path{seam_start, Point(s, -10 * s), Point(s, -20 * s)};
    const Polyline original = path;

    REQUIRE_FALSE(offset_wipe_path(path, seam_start, seam_end, seam_end, +1, s, 5 * s));
    REQUIRE(path.points == original.points);
}

TEST_CASE("Stored wipe path continues after an inward pre-move", "[WipePath][Regression]")
{
    const coord_t s = scale_(1.0);
    const Point seam_start(s, s);
    const Point seam_end(0, 0);
    const Point wipe_start(2 * s, 2 * s);
    Polyline path{seam_start, Point(s, -10 * s), Point(s, -20 * s)};

    REQUIRE(offset_wipe_path(path, seam_start, seam_end, wipe_start, +1, s, 5 * s));
    REQUIRE(path.points.size() >= 3);
    path.points.front() = wipe_start; // Orca: reproduce Wipe::wipe()'s executable representation.
    CHECK_THAT(path.length(), Catch::Matchers::WithinAbs(5. * s, 2.));
}

TEST_CASE("Stored wipe path does not retrace a translated seam gap", "[WipePath][Regression]")
{
    const coord_t s = scale_(1.0);
    const Point seam_start(s, 0);
    const Point seam_end(0, 0);
    Polyline path{seam_start, seam_end, Point(-10 * s, 0)};
    const Polyline original = path;
    const Lines support{Line(Point(-10 * s, s), Point(10 * s, s))};

    // Orca: the exact reversal at seam_start forces the translated fallback.
    // The seam gap supplies its incoming direction but must not become an
    // inward-outward-inward detour in the executable path.
    REQUIRE(offset_wipe_path_toward_support(
        path, seam_start, seam_end, seam_end, +1, s, 5 * s,
        support, support, original.lines(), s));
    REQUIRE(path.points.size() == 2);
    CHECK(path.points[1].y() > seam_end.y());
}

TEST_CASE("Stored wipe path keeps its first offset point when seam gap is zero", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s),
                  Point(0, 100 * s), Point(0, 0)};

    REQUIRE(offset_wipe_path(path, Point(0, 0), Point(0, 0), Point(0, 0), +1, 5 * s, 20 * s));
    REQUIRE(path.points.size() >= 3);
    REQUIRE(path.points[0] == Point(0, 0));
    REQUIRE_THAT(path.points[1].x(), Catch::Matchers::WithinAbs(5 * s, 2));
    REQUIRE_THAT(path.points[1].y(), Catch::Matchers::WithinAbs(5 * s, 2));
}

TEST_CASE("Stored wipe path ignores unsafe geometry beyond the used prefix", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(1000 * s, 0), Point(1000 * s, 20 * s),
                  Point(900 * s, 20 * s), Point(0, 20 * s), Point(0, 0)};

    REQUIRE(offset_wipe_path(path, Point(0, 0), Point(0, 0), Point(0, 0), +1, 30 * s, 10 * s));
    REQUIRE(path.points.size() == 2);
    REQUIRE_THAT(path.length(), Catch::Matchers::WithinAbs(10 * s, 2));
}

TEST_CASE("Stored wipe path grows its source until the offset reaches the requested length", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s), Point(0, 100 * s)};
    const double wipe_length = 250 * s;

    // Orca: two inward corners shorten this offset by more than 2 * offset_dist.
    REQUIRE(offset_wipe_path(path, Point(0, 0), Point(0, 0), Point(0, 0),
                             +1, 10 * s, wipe_length));
    REQUIRE_THAT(path.length(), Catch::Matchers::WithinAbs(wipe_length, 2));
}

TEST_CASE("Stored wipe path is unchanged when wipe distance is zero", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(100 * s, 0)};
    const Polyline orig = path;

    REQUIRE_FALSE(offset_wipe_path(path, Point(0, 0), Point(0, 0), Point(0, 0), +1, 5 * s, 0));
    REQUIRE(path.points == orig.points);
}

TEST_CASE("Stored wipe path defers actual-start crossings to support validation", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(100 * s, 0), Point(100 * s, 100 * s),
                  Point(0, 100 * s), Point(0, 0)};
    const Lines current = path.lines();
    const Lines remote{Line(Point(0, 50 * s), Point(100 * s, 50 * s))};
    const Point wipe_start(50 * s, -10 * s);

    REQUIRE(offset_wipe_path(path, Point(0, 0), Point(0, 0), wipe_start, +1, 5 * s, 100 * s));
    Lines all_support = remote;
    all_support.insert(all_support.end(), current.begin(), current.end());
    REQUIRE_FALSE(wipe_path_support_score(path, wipe_start,
        LinesDistancer<Line>(remote), LinesDistancer<Line>(all_support), 5 * s).has_value());
}

TEST_CASE("Stored wipe path keeps the closing join when its prefix ends at the closing vertex", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(0, 100 * s), Point(100 * s, 100 * s),
                  Point(100 * s, 0), Point(0, 0)};

    REQUIRE(offset_wipe_path(path, Point(0, 0), Point(0, 0), Point(0, 0),
                             +1, 5 * s, 300 * s));
    REQUIRE(path.points.size() >= 2);
    REQUIRE(path.points[1] == Point(-5 * s, -5 * s));
}

TEST_CASE("Stored wipe path rejects a two-point zero-gap loop", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    Polyline path{Point(0, 0), Point(100 * s, 0), Point(0, 0)};
    const Polyline original = path;

    REQUIRE_FALSE(offset_wipe_path(path, Point(0, 0), Point(0, 0), Point(0, 0),
                                   +1, 5 * s, 100 * s));
    REQUIRE(path.points == original.points);
}

TEST_CASE("Stored wipe path tolerates quantized contact at its actual start", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    const coord_t quantization = coord_t(SCALED_EPSILON / 2);
    Polyline path{Point(0, quantization), Point(100 * s, quantization),
                  Point(100 * s, 100 * s + quantization), Point(0, 100 * s + quantization),
                  Point(0, quantization)};

    // Orca: the executable transition starts within the geometry epsilon of the
    // source endpoint. Treat this as the allowed start contact, while contacts
    // farther along the transition remain unsafe.
    REQUIRE(offset_wipe_path(path, Point(0, quantization), Point(0, quantization),
                             Point(0, 0), +1, 5 * s, 20 * s));
}

TEST_CASE("Stored wipe path requires nearby generated perimeter geometry", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    const Polyline path{Point(0, 0), Point(0, 2 * s), Point(10 * s, 2 * s)};
    const Lines adjacent{Line(Point(0, 4 * s), Point(10 * s, 4 * s))};
    const Lines remote{Line(Point(0, 20 * s), Point(10 * s, 20 * s))};
    const Lines current = path.lines();

    const LinesDistancer<Line> adjacent_distancer(adjacent);
    const LinesDistancer<Line> remote_distancer(remote);
    Lines all_support = remote;
    all_support.insert(all_support.end(), current.begin(), current.end());
    const LinesDistancer<Line> all_support_distancer(all_support);

    const auto score = wipe_path_support_score(path, Point(0, 2 * s), adjacent_distancer, adjacent_distancer, 3 * s);
    REQUIRE(score.has_value());
    CHECK_THAT(*score, Catch::Matchers::WithinAbs(2. * s, 2.));
    REQUIRE_FALSE(wipe_path_support_score(path, Point(0, 2 * s), adjacent_distancer, adjacent_distancer, 0).has_value());
    REQUIRE_FALSE(wipe_path_support_score(path, Point(0, 2 * s), remote_distancer, remote_distancer, 3 * s).has_value());
    REQUIRE_FALSE(wipe_path_support_score(path, Point(0, 2 * s), remote_distancer, all_support_distancer, 3 * s).has_value());
    REQUIRE_FALSE(wipe_path_support_score(path, Point(0, 2 * s), LinesDistancer<Line>(Lines{}),
                                          all_support_distancer, 3 * s).has_value());
}

TEST_CASE("Stored wipe path checks the first segment from its actual start", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    const Polyline path{Point(0, 0), Point(10 * s, 0)};
    const Lines support_near_ends{
        Line(Point(0, -s), Point(0, s)),
        Line(Point(10 * s, -s), Point(10 * s, s))
    };

    // Orca: both endpoints are supported, but the middle of the executable segment
    // from wipe_start is not. The dummy path[0] must not hide that segment.
    const LinesDistancer<Line> support_distancer(support_near_ends);
    REQUIRE_FALSE(wipe_path_support_score(path, Point(0, 0), support_distancer, support_distancer, 2 * s).has_value());
}

TEST_CASE("Stored wipe path rejects unsupported gaps between nearby samples", "[WipePath][Regression]")
{
    const Point start = Point::new_scale(0., 0.);
    const Point end = Point::new_scale(0.8, 0.);
    const Polyline path{start, end};
    const double support_y = GENERATE(0.8, 0.95);
    const Lines support{
        Line(Point::new_scale(0., support_y), Point::new_scale(0., 2.)),
        Line(Point::new_scale(0.8, support_y), Point::new_scale(0.8, 2.))
    };

    // Both endpoints are within 1 mm of support and the move is shorter than
    // the old sampling interval. Only the 0.8 mm case supports its midpoint.
    const LinesDistancer<Line> support_distancer(support);
    const bool supported = wipe_path_support_score(path, start, support_distancer, support_distancer, scale_(1.)).has_value();
    CHECK(supported == (support_y < 0.9));
}

TEST_CASE("Stored wipe path checks support at the actual nozzle position", "[WipePath][Regression]")
{
    const Point end = Point::new_scale(0., 0.);
    const Polyline path{end, end};
    const Lines support{Line(Point::new_scale(-1., 0.), Point::new_scale(1., 0.))};

    const LinesDistancer<Line> support_distancer(support);
    REQUIRE_FALSE(wipe_path_support_score(path, Point::new_scale(0., -2.),
        support_distancer, support_distancer, scale_(1.)).has_value());
}

TEST_CASE("Direct inward fallback respects a short wipe distance before validation", "[WipePath][Regression]")
{
    const Point seam = Point::new_scale(0., 0.);
    Polyline path{seam, Point::new_scale(10., 0.), Point::new_scale(10., 10.),
                  Point::new_scale(0., 10.), seam};
    const Lines current = path.lines();
    const Lines support{Line(Point::new_scale(0.4, 0.4), Point::new_scale(9.6, 0.4))};
    const bool pre_move = GENERATE(false, true);
    const Point wipe_start = pre_move ? Point::new_scale(0.02, 0.02) : seam;
    const double wipe_length = scale_(0.05);

    REQUIRE(offset_wipe_path_toward_support(
        path, seam, seam, wipe_start, +1, scale_(0.2), wipe_length,
        support, support, current, scale_(0.4)));
    REQUIRE(path.points.size() == 2);
    path.points.front() = wipe_start;
    CHECK_THAT(path.length(), Catch::Matchers::WithinAbs(wipe_length, 2.));
    CHECK(path.last_point().x() > wipe_start.x());
    CHECK(path.last_point().y() > wipe_start.y());
}

TEST_CASE("Stored wipe path uses a stable zero-gap join for nearly parallel segments", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(58.777, 61.985);
    Polyline path{
        seam, point(58.822, 61.918), point(58.900, 61.789), point(58.980, 61.641),
        point(59.054, 61.480), point(59.260, 60.980), point(58.412, 62.485),
        point(58.631, 62.202), seam,
    };

    REQUIRE(offset_wipe_path(path, seam, seam, seam, -1, scale_(0.23), scale_(0.8)));
    REQUIRE(path.points.size() >= 3);

    const Vec2d first = (path.points[1] - seam).cast<double>();
    const Vec2d second = (path.points[2] - path.points[1]).cast<double>();
    CHECK(first.dot(second) >= 0.);
}

TEST_CASE("Stored wipe path follows the inner wall at a narrow external cusp", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(55.139, 60.077);
    Polyline path{
        seam, point(55.156, 60.010), point(55.205, 59.961), point(55.237, 59.934),
        point(55.304, 59.907), point(55.392, 59.872), point(55.630, 59.791),
        point(56.564, 59.430), point(55.061, 59.956), point(55.108, 60.008), seam,
    };
    const Polyline original = path;
    const Lines target_support{
        Line(point(54.983, 59.648), point(55.121, 59.745)),
    };
    Lines printed_support = target_support;
    printed_support.emplace_back(point(54.75, 60.25), point(55.50, 60.10));

    REQUIRE(offset_wipe_path_toward_support(
        path, seam, seam, seam, -1, scale_(0.270341), scale_(0.8),
        target_support, printed_support, original.lines(), scale_(0.4)));
    REQUIRE(path.points.size() >= 2);
    CHECK(path.points[1].y() < seam.y() - scale_(0.2));
    CHECK(std::abs(path.points[1].x() - seam.x()) < scale_(0.1));
}

TEST_CASE("Stored wipe path keeps a supported zero-gap join that initially backtracks", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(56.737, 62.049);
    Polyline path{
        seam, point(56.759, 62.142), point(56.727, 62.294), point(56.682, 62.447),
        point(56.631, 62.570), point(56.581, 62.669), point(56.512, 62.776),
        point(54.0, 64.0), point(50.0, 60.0), point(54.0, 58.0),
        point(56.773, 62.031), seam,
    };
    const Polyline original = path;
    const Lines target_support{
        Line(point(56.546, 62.012), point(56.534, 62.104)),
        Line(point(56.534, 62.104), point(56.506, 62.238)),
        Line(point(56.506, 62.238), point(56.467, 62.371)),
        Line(point(56.467, 62.371), point(56.424, 62.474)),
        Line(point(56.424, 62.474), point(56.382, 62.556)),
    };

    Polyline inward = path;
    REQUIRE(offset_wipe_path(inward, seam, seam, seam, +1, scale_(0.23), scale_(0.8)));
    REQUIRE(inward.points.size() >= 3);
    const Vec2d connector = (inward.points[1] - seam).cast<double>();
    const Vec2d outgoing = (inward.points[2] - inward.points[1]).cast<double>();
    REQUIRE(connector.dot(outgoing) < 0.);

    REQUIRE(offset_wipe_path_toward_support(
        path, seam, seam, seam, +1, scale_(0.23), scale_(0.8),
        target_support, target_support, original.lines(), scale_(0.4)));
    CHECK(path.points[1].x() < seam.x() - scale_(0.1));
}

TEST_CASE("Stored wipe path leaves a narrow cusp directly after a seam gap", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam_start = point(55.139, 60.077);
    const Point seam_end = point(55.141, 60.067);
    Polyline path{
        seam_start, point(55.107, 60.008), point(55.061, 59.956), point(55.027, 59.943),
        point(54.982, 59.924), point(54.922, 59.879), point(54.868, 59.845),
        point(54.754, 59.783), point(54.391, 59.635), point(54.053, 59.471),
    };
    const Polyline original = path;
    const Lines target_support{
        Line(point(55.132, 59.744), point(55.121, 59.745)),
        Line(point(55.121, 59.745), point(54.983, 59.648)),
        Line(point(54.983, 59.648), point(54.938, 59.623)),
        Line(point(54.938, 59.623), point(54.866, 59.584)),
        Line(point(54.866, 59.584), point(54.483, 59.427)),
        Line(point(54.483, 59.427), point(54.157, 59.268)),
    };

    REQUIRE(offset_wipe_path_toward_support(
        path, seam_start, seam_end, seam_end, -1, scale_(0.270341), scale_(0.8),
        target_support, target_support, original.lines(), scale_(0.4)));
    REQUIRE(path.points.size() >= 2);
    CHECK(path.points[1].y() < seam_end.y() - scale_(0.2));
    CHECK(std::abs(path.points[1].x() - seam_end.x()) < scale_(0.05));

    // Orca: the inward connector must not run back through the first extruded
    // point after the gap, which would put the wipe on the external wall.
    const Line connector(seam_end, path.points[1]);
    CHECK(connector.distance_to(original.points[1]) > scale_(0.02));
}

TEST_CASE("Stored wipe path does not reverse after an inward pre-move at a wide gap", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam_start = point(55.139, 60.077);
    const Point seam_end = point(55.163, 60.002);
    const Point wipe_start = point(55.142, 60.037);
    Polyline path{
        seam_start, point(55.107, 60.008), point(55.061, 59.956), point(55.027, 59.943),
        point(54.982, 59.924), point(54.922, 59.879), point(54.868, 59.845),
        point(54.754, 59.783), point(54.391, 59.635), point(54.053, 59.471),
        point(50.2, 55.0), point(50.2, 50.0), point(60.8, 50.0), point(60.8, 55.0),
        point(56.564, 59.430), point(55.824, 59.708), point(55.392, 59.872),
        point(55.237, 59.934), point(55.205, 59.961), seam_end,
    };
    const Polyline original = path;
    const Lines target_support{
        Line(point(55.132, 59.744), point(55.121, 59.745)),
        Line(point(55.121, 59.745), point(54.983, 59.648)),
        Line(point(54.983, 59.648), point(54.866, 59.584)),
        Line(point(54.866, 59.584), point(54.483, 59.427)),
        Line(point(54.483, 59.427), point(54.157, 59.268)),
    };

    REQUIRE(offset_wipe_path_toward_support(
        path, seam_start, seam_end, wipe_start, -1, scale_(0.270341), scale_(0.8),
        target_support, target_support, original.lines(), scale_(0.4)));
    REQUIRE(path.points.size() >= 3);

    const Vec2d connector = (path.points[1] - wipe_start).cast<double>();
    const Vec2d outgoing = (path.points[2] - path.points[1]).cast<double>();
    CHECK(connector.dot(outgoing) >= 0.);
    path.points.front() = wipe_start;
    CHECK_THAT(path.length(), Catch::Matchers::WithinAbs(scale_(0.8), 2.));
}

TEST_CASE("Stored wipe path follows the incoming wall when a corner gap truncates the forward path",
          "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam_start = point(46.047, 61.988);
    const Point seam_end = point(46.118, 61.917);
    Polyline path{
        seam_start, point(39.139, 55.080), point(46.047, 48.171),
        point(52.956, 55.080), seam_end,
    };
    const Polyline original = path;
    const Lines target_support{
        Line(point(46.047, 61.672), point(39.461, 55.080)),
        Line(point(39.461, 55.080), point(46.047, 48.493)),
        Line(point(46.047, 48.493), point(52.633, 55.080)),
        Line(point(52.633, 55.080), point(46.047, 61.672)),
    };

    REQUIRE(offset_wipe_path_toward_support(
        path, seam_start, seam_end, seam_end, +1, scale_(0.23), scale_(0.8),
        target_support, target_support, original.lines(), scale_(0.4)));
    path.points.front() = seam_end;
    CHECK_THAT(path.length(), Catch::Matchers::WithinAbs(scale_(0.8), 2.));
    REQUIRE(path.points.size() >= 3);
    CHECK(path.points[1].x() < seam_end.x());
    CHECK(path.points[1].y() < seam_end.y());
}

TEST_CASE("Stored wipe path prefers support on the material side of a seam gap", "[WipePath][Regression]")
{
    const coord_t s = scale_(1.0);
    const Point seam_start(s, 0);
    const Point seam_end(0, 0);
    Polyline path{seam_start, Point(s, 10 * s), Point(s, 20 * s)};
    const Polyline original = path;
    const Lines target_support{
        Line(Point(0, s), Point(0, 3 * s)),
        Line(Point(s / 2, -s / 10), Point(3 * s / 2, -s / 10)),
    };

    // Orca: the lower line is closest at the cusp and the preferred winding
    // points toward it, but the outgoing wall is adjacent to the upper line.
    REQUIRE(offset_wipe_path_toward_support(
        path, seam_start, seam_end, seam_end, -1, s, 5 * s,
        target_support, target_support, original.lines(), 2 * s));
    CHECK(path.points[1].y() > seam_end.y());
}

TEST_CASE("Stored wipe path rejects an outward offset at a reflex seam gap", "[WipePath][Regression]")
{
    const double offset = GENERATE(0.2, 0.4); // 50% and 100% of a 0.4 mm wall.
    const double mirror = GENERATE(1., -1.);
    CAPTURE(offset, mirror);
    const auto point = [mirror](double x, double y) { return Point::new_scale(mirror * x, y); };
    const Point seam_start = point(0., 0.);
    const double gap_component = 0.04 / std::sqrt(2.); // Default 10% seam gap for a 0.4 mm nozzle.
    const Point seam_end = point(-gap_component, -gap_component);
    const Polyline original{seam_start, point(0., -10.)};
    const Lines support{Line(point(0.4, -10.), point(0.4, 1.))};
    const int preferred_dir = mirror > 0. ? +1 : -1;

    // The inward miter backtracks. The opposite offset can still be supported
    // by the outer bead, so support alone must not make it an inward candidate.
    Polyline outward = original;
    REQUIRE(offset_wipe_path(outward, seam_start, seam_end, seam_end,
                             -preferred_dir, scale_(offset), scale_(2.)));
    Lines all_support = support;
    const Lines current = original.lines();
    all_support.insert(all_support.end(), current.begin(), current.end());
    REQUIRE(wipe_path_support_score(outward, seam_end,
        LinesDistancer<Line>(support), LinesDistancer<Line>(all_support), scale_(0.4)).has_value());
    REQUIRE(mirror * outward.points[1].x() < 0.);

    Polyline path = original;
    if (offset_wipe_path_toward_support(path, seam_start, seam_end, seam_end,
            preferred_dir, scale_(offset), scale_(2.), support, support, current, scale_(0.4))) {
        REQUIRE(path.points.size() >= 2);
        CHECK(mirror * path.points[1].x() > 0.);
    } else {
        CHECK(path.points == original.points);
    }

    // An inward pre-move provides a clear connector to the direct fallback.
    // The fix must retain this usable inward path, rather than reject all wipes.
    const Point wipe_start = point(0.05, -0.04);
    path = original;
    REQUIRE(offset_wipe_path_toward_support(path, seam_start, seam_end, wipe_start,
        preferred_dir, scale_(offset), scale_(2.), support, support, current, scale_(0.4)));
    REQUIRE(path.points.size() == 2);
    CHECK(mirror * path.points[1].x() > mirror * wipe_start.x());
}

TEST_CASE("Direct inward wipes respect the nozzle position and intervening walls", "[WipePath][Regression]")
{
    const int mirror = GENERATE(1, -1);
    const bool crossing_wall = GENERATE(false, true);
    CAPTURE(mirror, crossing_wall);
    const auto point = [mirror](double x, double y) { return Point::new_scale(mirror * x, y); };
    const Point seam_start = point(0., 0.);
    const double gap_component = 0.04 / std::sqrt(2.);
    const Point seam_end = point(-gap_component, -gap_component);
    const Polyline original{seam_start, point(0., -10.)};
    const Lines support{Line(point(0.4, -10.), point(0.4, 1.))};
    Lines current = original.lines();
    // The direct destination is near x=0.172. A nozzle already farther inward
    // must not return toward the wall. An inward connector from x=0.05 must
    // still be rejected when another wall lies between it and the destination.
    const Point wipe_start = point(crossing_wall ? 0.05 : 0.3, -0.04);
    if (crossing_wall)
        current.emplace_back(point(0.1, -0.2), point(0.1, 0.2));
    Polyline path = original;
    REQUIRE_FALSE(offset_wipe_path_toward_support(path, seam_start, seam_end, wipe_start,
        mirror, scale_(0.2), scale_(2.), support, support, current, scale_(0.4)));
    CHECK(path.points == original.points);
}

TEST_CASE("Inward wipe checks the material side after leaving an open wall endpoint", "[WipePath][Regression]")
{
    const bool require_clearance = GENERATE(false, true);
    CAPTURE(require_clearance);
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(0., 0.);
    const LinesDistancer<Line> current(Lines{Line(seam, point(2., 0.))});
    const LinesDistancer<Line> support(Lines{Line(point(0., 0.4), point(2., 0.4))});
    Polyline path{seam, point(0.1, 0.2), point(0.5, 0.2), point(-0.2, 0.2)};
    REQUIRE(wipe_path_stays_on_material_side(
        path, seam, Vec2d(0., 1.), support, current, scale_(0.2), require_clearance));

    // Rounding the open endpoint keeps 0.2 mm of unsigned clearance while
    // moving to the air side. Checking only the first direction cannot catch it.
    path.points.push_back(point(-0.2, -0.2));
    path.points.push_back(point(0.5, -0.2));
    REQUIRE_FALSE(wipe_path_stays_on_material_side(
        path, seam, Vec2d(0., 1.), support, current, scale_(0.2), require_clearance));
}

TEST_CASE("Direct inward fallbacks check the material side without requiring clearance", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(0., 0.);
    const LinesDistancer<Line> current(Lines{Line(point(-2., 0.), point(2., 0.))});
    const LinesDistancer<Line> support(Lines{Line(point(-2., 0.4), point(2., 0.4))});
    REQUIRE(wipe_path_stays_on_material_side(
        Polyline{seam, point(0., 0.05)}, seam, Vec2d(0., 1.), support, current, scale_(0.2), false));

    // Even if the construction's initial direction points outward, the nearby
    // inner wall still identifies the material side independently of that hint.
    REQUIRE_FALSE(wipe_path_stays_on_material_side(
        Polyline{seam, point(0., -0.05)}, seam, Vec2d(0., -1.), support, current, scale_(0.2), false));
}

TEST_CASE("Stored wipe path may return to the current wall after reaching an earlier wall", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    const Polyline path{Point(0, 0), Point(0, 2 * s), Point(10 * s, 0)};
    const Lines earlier{Line(Point(0, 2 * s), Point(10 * s, 2 * s))};
    const Lines current{Line(Point(0, 0), Point(10 * s, 0))};

    Lines all_support = earlier;
    all_support.insert(all_support.end(), current.begin(), current.end());
    REQUIRE(wipe_path_support_score(path, Point(0, 0),
        LinesDistancer<Line>(earlier), LinesDistancer<Line>(all_support), s).has_value());
}

TEST_CASE("Stored wipe path tolerates compounded coordinate quantization", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    const coord_t rounding = coord_t(3.5 * SCALED_EPSILON);
    const Point destination(0, 2 * s + rounding);
    const Polyline path{Point(0, 0), destination};
    const Lines earlier{Line(Point(-s, 0), Point(s, 0))};

    const LinesDistancer<Line> support_distancer(earlier);
    REQUIRE(wipe_path_support_score(path, destination, support_distancer, support_distancer, 2 * s).has_value());
}

TEST_CASE("Stored wipe path stays on the inner side of a short external loop", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(55.270, 41.666);
    Polyline path{
        seam, point(55.241, 41.568), point(55.210, 41.518), point(55.195, 41.506),
        point(55.173, 41.496), point(55.141, 41.479), point(55.126, 41.473),
        point(55.068, 41.421), point(55.055, 41.416), point(55.006, 41.382),
        point(54.808, 41.231), point(54.687, 41.153), point(54.590, 41.069),
        point(54.529, 41.027), point(54.441, 40.949), point(54.299, 40.803),
        point(54.219, 40.674), point(54.183, 40.581), point(54.172, 40.511),
        point(54.182, 40.450), point(54.225, 40.358), point(54.256, 40.318),
        point(54.341, 40.251), point(54.418, 40.211), point(54.499, 40.176),
        point(54.675, 40.124), point(54.797, 40.106), point(54.978, 40.092),
        point(55.245, 40.093), point(55.443, 40.103), point(55.591, 40.128),
        point(55.771, 40.164), point(55.962, 40.217), point(56.103, 40.264),
        point(56.167, 40.295), point(56.246, 40.341), point(56.338, 40.412),
        point(56.382, 40.469), point(56.396, 40.527), point(56.386, 40.609),
        point(56.313, 40.740), point(56.208, 40.867), point(56.071, 40.991),
        point(55.946, 41.094), point(55.812, 41.198), point(55.722, 41.262),
        point(55.665, 41.294), point(55.556, 41.398), point(55.520, 41.414),
        point(55.495, 41.424), point(55.478, 41.437), point(55.442, 41.469),
        point(55.407, 41.505), point(55.386, 41.510), point(55.367, 41.516),
        point(55.335, 41.535), point(55.292, 41.575), seam,
    };
    const Polyline original = path;
    const Polyline inner{
        point(55.111, 41.176), point(54.946, 41.050), point(54.824, 40.970),
        point(54.733, 40.892), point(54.668, 40.846), point(54.598, 40.784),
        point(54.480, 40.662), point(54.424, 40.572), point(54.403, 40.516),
        point(54.420, 40.479), point(54.465, 40.443), point(54.577, 40.390),
        point(54.723, 40.347), point(54.823, 40.333), point(54.986, 40.320),
        point(55.239, 40.321), point(55.418, 40.330), point(55.550, 40.352),
        point(55.718, 40.386), point(55.896, 40.435), point(56.017, 40.476),
        point(56.061, 40.497), point(56.118, 40.530), point(56.154, 40.558),
        point(56.124, 40.611), point(56.043, 40.709), point(55.921, 40.819),
        point(55.804, 40.916), point(55.676, 41.015), point(55.600, 41.069),
        point(55.526, 41.114), point(55.426, 41.207), point(55.379, 41.235),
        point(55.349, 41.259), point(55.291, 41.317), point(55.252, 41.293),
        point(55.192, 41.238), point(55.111, 41.176),
    };
    Lines target_support = inner.lines();
    // Orca: a different contour has a slightly closer inner wall on the air
    // side of this short loop. It must not override the loop's material side.
    target_support.emplace_back(point(55.159, 42.147), point(55.299, 42.011));

    REQUIRE(offset_wipe_path_toward_support(
        path, seam, seam, seam, +1, scale_(0.293166), scale_(0.8),
        target_support, target_support, original.lines(), scale_(0.4)));
    REQUIRE(path.points.size() >= 2);

    // Orca: the nearest inner wall is below the seam; accepting the opposite
    // offset would send the wipe into air outside this small contour.
    CHECK(path.points[1].y() < seam.y());
}

TEST_CASE("Stored wipe path does not return to the external wall after moving inward", "[WipePath][Regression]")
{
    const auto point = [](double x, double y) { return Point::new_scale(x, y); };
    const Point seam = point(47.451, 54.647);
    Polyline path{
        seam, point(47.370, 54.634), point(47.345, 54.619), point(47.333, 54.604),
        point(47.322, 54.572), point(47.312, 54.518), point(47.315, 54.445),
        point(47.345, 54.257), point(47.357, 54.206), point(47.380, 54.135),
        point(47.418, 54.065), point(47.514, 53.917), point(47.537, 53.886),
        point(47.597, 53.834), point(47.705, 53.769), point(47.747, 53.748),
        point(47.785, 53.734), point(47.825, 53.735), point(47.862, 53.746),
        point(47.889, 53.763), point(47.939, 53.817), point(47.964, 53.856),
        point(47.979, 53.897), point(47.986, 53.943), point(47.986, 54.005),
        point(47.977, 54.075), point(47.949, 54.188), point(47.902, 54.321),
        point(47.871, 54.388), point(47.835, 54.444), point(47.765, 54.521),
        point(47.741, 54.542), point(47.675, 54.589), point(47.615, 54.620),
        point(47.518, 54.642), seam,
    };
    const Polyline original = path;
    const Polyline inner{
        point(47.541, 54.281), point(47.545, 54.258), point(47.560, 54.212),
        point(47.577, 54.181), point(47.682, 54.017), point(47.707, 53.995),
        point(47.789, 53.946), point(47.791, 53.958), point(47.791, 53.992),
        point(47.785, 54.039), point(47.762, 54.132), point(47.721, 54.249),
        point(47.700, 54.294), point(47.680, 54.324), point(47.628, 54.382),
        point(47.574, 54.422), point(47.548, 54.435), point(47.513, 54.443),
        point(47.541, 54.281),
    };
    const double offset = scale_(0.229999);

    REQUIRE(offset_wipe_path_toward_support(
        path, seam, seam, seam, +1, offset, scale_(0.8),
        inner.lines(), inner.lines(), original.lines(), scale_(0.4)));

    // Orca: after reaching the inner wall, a full-width inward wipe must not
    // collapse back onto the external perimeter at a tight turn.
    for (size_t index = 1; index < path.points.size(); ++index) {
        double clearance = std::numeric_limits<double>::infinity();
        for (const Line &line : original.lines())
            clearance = std::min(clearance, line.distance_to(path.points[index]));
        CHECK(clearance >= 0.75 * offset);
    }
}

// Orca: wipe_on_loops_destination coverage for every orientation.

TEST_CASE("wipe_on_loops destination is on the material side for every orientation", "[WipePath]")
{
    const auto [is_ccw, is_hole] = GENERATE(
        table<bool, bool>({{true, false}, {false, false}, {false, true}, {true, true}}));
    INFO("is_ccw=" << is_ccw << ", is_hole=" << is_hole);
    const double nozzle_diameter = GENERATE(0.4, 0.8);
    const bool subdivided = GENERATE(false, true);
    INFO("nozzle diameter=" << nozzle_diameter << ", subdivided=" << subdivided);

    const coord_t s = scale_(1.0);
    std::vector<Point> contour = {Point(0, 0), Point(20 * s, 0), Point(20 * s, 20 * s), Point(0, 20 * s)};
    if (subdivided) {
        // The same square, with path boundaries inside both sampling distances near the seam.
        contour = {Point(0, 0), Point(scale_(0.03), 0.), Point(scale_(0.2), 0.),
                   Point(20 * s, 0), Point(20 * s, 20 * s), Point(0, 20 * s),
                   Point(0., scale_(0.2)), Point(0., scale_(0.03))};
    }
    if (!is_ccw)
        for (Point &point : contour)
            std::swap(point.x(), point.y());
    ExtrusionPaths paths;
    if (subdivided) {
        for (size_t i = 0; i < contour.size(); ++i)
            paths.push_back(make_path({contour[i], contour[(i + 1) % contour.size()]}));
    } else {
        paths = make_loop_paths(contour);
    }

    const std::optional<Point> destination =
        wipe_on_loops_destination(paths, scale_(nozzle_diameter), is_ccw, is_hole);
    REQUIRE(destination.has_value());

    const Point seam_start = paths.front().first_point();
    const Vec2d first_edge = (paths.front().polyline.points[1].to_point() - seam_start).cast<double>();
    Vec2d material_normal(-first_edge.y(), first_edge.x());
    if (is_ccw == is_hole)
        material_normal = -material_normal;

    // Orca: contours use their winding's inside; holes use the opposite side.
    const Vec2d move = destination->cast<double>() - seam_start.cast<double>();
    REQUIRE(move.dot(material_normal) > 0.);
    // Move 20% of the nozzle diameter, turning through one third of the material-side
    // corner: 90 degrees for a contour, 270 degrees for a hole.
    const double distance = scale_(0.2 * nozzle_diameter);
    const double angle = is_hole ? PI / 2. : PI / 6.;
    CHECK_THAT(move.dot(first_edge.normalized()), Catch::Matchers::WithinAbs(distance * std::cos(angle), 2.));
    CHECK_THAT(move.dot(material_normal.normalized()), Catch::Matchers::WithinAbs(distance * std::sin(angle), 2.));
}

TEST_CASE("wipe_on_loops returns destination for small but nonzero loop", "[WipePath]")
{
    // Orca: a 0.5 mm square is tight for a 0.4 mm nozzle but remains valid.
    const coord_t s = scale_(1.0);
    auto paths = make_loop_paths({Point(0, 0), Point(s / 2, 0), Point(s / 2, s / 2), Point(0, s / 2)});

    auto dest = wipe_on_loops_destination(paths, scale_(0.4), true, false);
    REQUIRE(dest.has_value());
}

TEST_CASE("wipe_on_loops destination is nullopt for degenerate single-point path", "[WipePath]")
{
    const coord_t s = scale_(1.0);
    auto paths = make_paths({Point(50 * s, 50 * s)});

    auto dest = wipe_on_loops_destination(paths, scale_(0.4), true, false);
    REQUIRE_FALSE(dest.has_value());
}
