#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <math.h>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Support/OrganicToolpaths.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/Support/TreeSupport3D.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

TEST_CASE("Organic walls trimmed by fill are not printed as dashes", "[TreeSupport][WallTrim]")
{
    using namespace Slic3r;
    const coord_t  width = scaled<coord_t>(0.44);
    const Polyline axis{ Point(0, 0), Point::new_scale(10., 0.) };
    auto stretch = [](double from, double to) {
        return Polygon{ Point::new_scale(from, -1.), Point::new_scale(to, -1.), Point::new_scale(to, 1.), Point::new_scale(from, 1.) };
    };
    // Covered from 0.2 to 7 mm except for slits of 50 and 100 microns, and for 0.1 mm at 8.5 mm.
    const Polygons covered{ stretch(0.2, 3.), stretch(3.05, 4.5), stretch(4.6, 7.), stretch(8.5, 8.6) };
    std::vector<std::pair<double, double>> parts;
    for (const Polyline &part : organic_uncovered_wall_parts(axis, covered, width)) {
        const BoundingBox box = get_extents(part);
        parts.emplace_back(unscaled<double>(box.min.x()), unscaled<double>(box.max.x()));
    }
    std::sort(parts.begin(), parts.end());
    CAPTURE(parts.size());
    REQUIRE_FALSE(parts.empty());
    for (size_t i = 0; i < parts.size(); ++i) {
        CAPTURE(parts[i].first, parts[i].second);
        // Only a piece continuing the wall beyond an end of the axis may be shorter than a bead.
        if (parts[i].first > 0. && parts[i].second < 10.)
            CHECK(parts[i].second - parts[i].first >= 0.44);
        // Printed parts are not separated by a covered stretch shorter than a bead.
        if (i > 0)
            CHECK(parts[i].first - parts[i - 1].second >= 0.44);
    }
    // Uncovered stretches of a bead or more are printed, across the short covered one at 8.5 mm.
    CHECK_THAT(parts.front().first, Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT(parts.front().second, Catch::Matchers::WithinAbs(0.2, 1e-6));
    CHECK_THAT(parts.back().first, Catch::Matchers::WithinAbs(7., 1e-6));
    CHECK_THAT(parts.back().second, Catch::Matchers::WithinAbs(10., 1e-6));
}

TEST_CASE("Organic walls do not change their role for a sliver", "[TreeSupport][WallTrim]")
{
    using namespace Slic3r;
    const Polyline axis{ Point(0, 0), Point::new_scale(10., 0.) };
    auto stretch = [](double from, double to) {
        return Polygon{ Point::new_scale(from, -1.), Point::new_scale(to, -1.), Point::new_scale(to, 1.), Point::new_scale(from, 1.) };
    };
    auto extents = [](const Polylines &parts) {
        std::vector<std::pair<double, double>> result;
        for (const Polyline &part : parts) {
            const BoundingBox box = get_extents(part);
            result.emplace_back(unscaled<double>(box.min.x()), unscaled<double>(box.max.x()));
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    // Inside up to 6 mm except for a 50 micron slit at 3 mm, outside from there except for 50 microns
    // at 8 mm, and inside again for the last 30 microns of the axis.
    const Polygons region{ stretch(-1., 3.), stretch(3.05, 6.), stretch(8., 8.05), stretch(9.97, 11.) };
    const auto [inside, outside] = organic_split_wall_axis(axis, region, scaled<coord_t>(0.44));
    const auto in = extents(inside), out = extents(outside);
    CAPTURE(in.size(), out.size());
    REQUIRE(in.size() == 2);
    REQUIRE(out.size() == 1);
    CHECK_THAT(in.front().first, Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT(in.front().second, Catch::Matchers::WithinAbs(6., 1e-6));
    // A piece at an end of the axis continues the wall beyond it.
    CHECK_THAT(in.back().first, Catch::Matchers::WithinAbs(9.97, 1e-6));
    CHECK_THAT(in.back().second, Catch::Matchers::WithinAbs(10., 1e-6));
    CHECK_THAT(out.front().first, Catch::Matchers::WithinAbs(6., 1e-6));
    CHECK_THAT(out.front().second, Catch::Matchers::WithinAbs(9.97, 1e-6));
}

TEST_CASE("Organic inner tree loops are spliced only into the walls they are given", "[TreeSupport][WallTrim][Regression]")
{
    using namespace Slic3r;
    // A straight raft line runs past a tree inside a body, a wall of which lies farther away. Spliced into the line, the
    // loop of the tree would be printed as a part of the raft's fill.
    const bool walls_given = GENERATE(false, true);
    CAPTURE(walls_given);
    const Flow flow(.4f, .2f, .4f);
    ExtrusionEntityCollection fills;
    extrusion_entities_append_paths(fills.entities, Polylines{ Polyline({ Point::new_scale(-5., .45), Point::new_scale(5., .45) }) },
        erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
    extrusion_entities_append_paths(fills.entities, Polylines{ Polyline({ Point::new_scale(-5., -1.), Point::new_scale(5., -1.) }) },
        erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
    const ExtrusionEntitiesPtr walls{ fills.entities.back() };
    organic_generate_inner_tree_paths(fills.entities, Polygons{ make_circle_num_segments(scale_(.5), 64) }, Polygons{}, flow,
        walls_given ? &walls : nullptr);
    const auto *line = dynamic_cast<const ExtrusionPath*>(fills.entities.front());
    REQUIRE(line != nullptr);
    CHECK((line->as_polyline().size() == 2) == walls_given);
    // Not spliced, the loop is printed on its own.
    CHECK(fills.entities.size() == (walls_given ? 3 : 2));
}

TEST_CASE("Organic tree loops touching a wall are spliced into it", "[TreeSupport][WallTrim][Regression]")
{
    using namespace Slic3r;
    const coord_t width = scaled<coord_t>(0.22);
    Polylines     walls{ Polyline({ Point::new_scale(0., 0.), Point::new_scale(10., 0.) }) };
    auto square = [](double x, double y, double side) {
        return Polyline({ Point::new_scale(x, y), Point::new_scale(x + side, y), Point::new_scale(x + side, y + side),
                          Point::new_scale(x, y + side) });
    };
    // The loop of the bottom of a tree leaning into the wall, its axis 0.17 mm from the wall's, and a loop 3 mm away.
    const Polyline  touching = square(5., 0.17, 0.3), apart = square(5., 3., 0.3);
    const Polylines left     = organic_splice_loops_into_walls(walls, { touching, apart }, width);
    REQUIRE(left.size() == 1);
    CHECK(left.front().points == apart.points);
    REQUIRE(walls.size() == 1);
    // The wall keeps its ends and goes round the far side of the loop.
    const Polyline &wall = walls.front();
    CHECK(wall.first_point() == Point::new_scale(0., 0.));
    CHECK(wall.last_point() == Point::new_scale(10., 0.));
    for (const Point &point : { Point::new_scale(5.3, 0.17), Point::new_scale(5.3, 0.47), Point::new_scale(5., 0.47) })
        CHECK(std::find(wall.points.begin(), wall.points.end(), point) != wall.points.end());
    // It does not cross itself, going in and out at the same place over what it has just printed.
    const Lines lines = wall.lines();
    for (size_t i = 0; i < lines.size(); ++ i)
        for (size_t j = i + 2; j < lines.size(); ++ j) {
            Point crossing;
            CAPTURE(i, j);
            CHECK_FALSE(lines[i].intersection(lines[j], &crossing));
        }
}

TEST_CASE("Organic parts printed solid leave no gap a line could fill", "[TreeSupport][Solid][Regression]")
{
    using namespace Slic3r;
    // Lines 0.22 mm wide at layers 0.02 mm from a 0.4 mm nozzle. A tip printed solid, a strip 0.62 mm wide: a spiral of
    // loops a line wide lays one half a line inside its outline and leaves a slit about 0.18 mm wide along its middle,
    // over which the loop the layer above starts there, as the tip widens going up, would hang.
    const double            width = 0.22;
    const Flow              flow(float(width), 0.02f, 0.4f);
    const PrintConfig       print_config;
    const PrintObjectConfig object_config;
    const double            length = GENERATE(3., 6.);
    CAPTURE(length);
    const Polygon strip({ Point::new_scale(0., 0.), Point::new_scale(length, 0.), Point::new_scale(length, 0.62),
                          Point::new_scale(0., 0.62) });
    ExtrusionEntitiesPtr paths;
    const ScopeGuard     release([&paths] { for (ExtrusionEntity *entity : paths) delete entity; });
    // A strip all solid is printed with no wall.
    CHECK(organic_extrude_solid_islands(paths, { strip }, { strip }, flow, print_config, object_config).empty());
    // The beads, round at their ends as a nozzle lays them, so that the seam of a loop counts as closed.
    Polygons covered;
    for (const ExtrusionEntity *entity : paths)
        for_each_extrusion_path(*entity, [&covered](const ExtrusionPath &path) {
            polygons_append(covered, organic_bead(path.as_polyline(), scaled<double>(path.width)));
        });
    // What the beads leave of the strip is narrower than half a line everywhere.
    CHECK(opening(diff(Polygons{ strip }, covered), scaled<float>(0.25 * width)).empty());
}

TEST_CASE("Organic solid parts too small for the spiral are printed all the same", "[TreeSupport][Solid][Regression]")
{
    using namespace Slic3r;
    // Lines 0.22 mm wide at layers 0.1 mm from a 0.4 mm nozzle. A part printed solid a couple of lines across in the middle
    // of a body 3 mm square, as a body widening under a roof leaves in a notch of it: the trees inside the body take it for
    // material, so the layer above, wider there, stands on it.
    const double            width = 0.22;
    const Flow              flow(float(width), 0.1f, 0.4f);
    const PrintConfig       print_config;
    const PrintObjectConfig object_config;
    const Polygon body({ Point::new_scale(-1.5, -1.5), Point::new_scale(1.5, -1.5), Point::new_scale(1.5, 1.5), Point::new_scale(-1.5, 1.5) });
    const int     shape = GENERATE(0, 1, 2);
    CAPTURE(shape);
    const Polygon part = shape == 0 ? Polygon({ Point::new_scale(0., 0.), Point::new_scale(0.42, 0.), Point::new_scale(0.21, 0.36) }) :
                         shape == 1 ? Polygon({ Point::new_scale(0., 0.), Point::new_scale(0.3, 0.), Point::new_scale(0.3, 0.3), Point::new_scale(0., 0.3) }) :
                                      Polygon({ Point::new_scale(0., 0.), Point::new_scale(0.38, 0.12), Point::new_scale(0.33, 0.34),
                                                Point::new_scale(0.1, 0.3) });
    ExtrusionEntitiesPtr paths;
    const ScopeGuard     release([&paths] { for (ExtrusionEntity *entity : paths) delete entity; });
    organic_extrude_solid_islands(paths, { body }, { part }, flow, print_config, object_config);
    Polygons covered;
    for (const ExtrusionEntity *entity : paths)
        for_each_extrusion_path(*entity, [&covered](const ExtrusionPath &path) {
            polygons_append(covered, organic_bead(path.as_polyline(), scaled<double>(path.width)));
        });
    // The beads leave no part of it wider than half a line.
    REQUIRE_FALSE(covered.empty());
    CHECK(opening(diff(Polygons{ part }, covered), scaled<float>(0.25 * width)).empty());
}

TEST_CASE("Organic footing near a box keeps the edges crossing the box as they are", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    // A thin sloped outline, and one bent around its far end with a corner a fraction of a nanometre off its long edge. Cut
    // short out of the box, the bent outline crosses that edge out of the box, and the crossing rounds to the corner.
    const Polygon sloped{{-20000000, -2740000}, {20000000, 2740000}, {20000000, 3160000}, {-20000000, -2320000}};
    Polygon bent{{-800000, -1800000}, {2200001, 301400}, {22000000, 2540000}, {24000000, 2540000}, {24000000, 4000000},
        {22000000, 4000000}, {2300000, 735100}, {-3000000, 100000}, {-3000000, 300000}, {22000000, 4600000},
        {25000000, 4600000}, {25000000, 1950000}, {22000000, 1950000}, {-800000, -2200000}};
    bent.reverse();
    REQUIRE(sloped.is_counter_clockwise());
    REQUIRE(bent.is_counter_clockwise());
    // The box and the reach of a path of a bead 0.4 mm wide.
    BoundingBox box(Point(-1001, -202207), Point(998999, -65207));
    box.offset(500000.);
    const Polygons near = organic_footing_near_box({ sloped, bent }, box, 200200.f);
    // The long edge of the sloped outline crosses the box.
    const Point a = sloped.points[0], b = sloped.points[1];
    CHECK(std::any_of(near.begin(), near.end(), [&a, &b](const Polygon &polygon) {
        for (size_t i = 0; i < polygon.size(); ++ i) {
            const Point &p = polygon.points[i], &q = polygon.points[(i + 1) % polygon.size()];
            if ((p == a && q == b) || (p == b && q == a))
                return true;
        }
        return false;
    }));
}

TEST_CASE("Organic footing near a box grows near the box as the whole of it does", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    // offset() drops each vertex closer than a fraction of the offset to the last one it kept, going around a polygon from
    // its first vertex. Each footing has two vertices about 1 um apart out of the box, one of them where a long edge
    // along a path starts and so sets its slope: the path runs within a nanometre of the edge of the reach.
    struct Case { Polygons footing; coord_t y; double width; };
    const Polygon sloped{ {-3000000, 300000}, {-600000, 400000}, {-599500, 400700}, {3000000, 400800}, {3000000, 3000000},
        {-3000000, 3000000} };
    std::vector<Case> cases{ { { Polygon{ {-2000000, 500}, {-1999600, 0}, {3000000, 0}, {3000000, 420000}, {-3000000, 420000} } },
        -200050, .4 } };
    for (size_t start = 0; start < sloped.size(); ++ start) {
        Polygon rotated = sloped;
        std::rotate(rotated.points.begin(), rotated.points.begin() + start, rotated.points.end());
        cases.push_back({ { rotated }, 190400, .42 });
    }
    // The footing of a bead 0.2 mm wide, its start bent by nanometres.
    cases.push_back({ offset(organic_bead(Polyline(Points{ {-4000000, 30}, {-2000000, 2}, {-1999500, 0}, {3000000, 0} }),
        scaled<double>(.2)), 10000.f), -310199, .4 });
    // An outline 0.4 um thin, which offset() drops whole, and a hole listed apart from it, which offset() then grows into
    // nothing and a union would fill.
    cases.push_back({ { Polygon{ {-4000000, -200}, {4000000, -200}, {4000000, 200}, {-4000000, 200} },
        Polygon{ {-3000000, -100}, {0, 150}, {3000000, -100} } }, 100000, .4 });
    for (size_t i = 0; i < cases.size(); ++ i) {
        const Case &c = cases[i];
        CAPTURE(i);
        const Polyline path(Point(0, c.y), Point(1000000, c.y));
        BoundingBox box = get_extents(path);
        box.offset(scaled<double>(c.width + .1));
        const float delta = float(scaled<double>(.5 * c.width) + 2. * SCALED_EPSILON);
        const auto free_length = [&path, delta](const Polygons &footing) {
            double length = 0.;
            for (const Polyline &free : diff_pl(Polylines{ path }, offset(footing, delta, jtRound, SCALED_EPSILON)))
                length += free.length();
            return unscale<double>(length);
        };
        CHECK_THAT(free_length(organic_footing_near_box(c.footing, box, delta)), Catch::Matchers::WithinAbs(free_length(c.footing), 1e-6));
    }
}

TEST_CASE("Organic trees near the bed move no farther a layer than their walls print over the walls below", "[TreeSupport][Radius]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // The settings at a layer height, a line width and a branch angle, the foot growing by up to 40 degrees a layer.
    const auto settings = [](const double layer, const double line, const double angle) {
        TreeSupportSettings result{};
        result.support_line_width           = scaled<coord_t>(line);
        result.maximum_move_distance        = angle < 90. ? coord_t(std::tan(angle * M_PI / 180.) * scaled<double>(layer)) :
                                                            std::numeric_limits<coord_t>::max();
        result.bp_radius_increase_per_layer = std::min(std::tan(0.7) * scaled<double>(layer), 0.5 * scaled<double>(line));
        return result;
    };
    // Layers 0.2 mm and lines 0.42 mm, the defaults: the branch angle moves a tree by 0.168 mm a layer, and near the bed it
    // moves by half a line at most.
    const TreeSupportSettings coarse = settings(0.2, 0.42, 40.);
    CHECK(coarse.maximum_move_distance + coarse.foot_move_increase() <= coarse.support_line_width / 2);
    CHECK(coarse.foot_move_increase() > 0);
    // At fine layers the whole growth of the foot fits within half a line.
    for (const TreeSupportSettings &fine : { settings(0.02, 0.22, 35.), settings(0.04, 0.22, 40.) }) {
        CAPTURE(fine.maximum_move_distance);
        CHECK(fine.foot_move_increase() == coord_t(fine.bp_radius_increase_per_layer));
    }
    // A branch angle moving a tree by half a line or more leaves nothing to add.
    CHECK(settings(0.2, 0.42, 70.).foot_move_increase() == 0);
    CHECK(settings(0.2, 0.42, 90.).foot_move_increase() == 0);
}

// A tree is not given a base wider than it is tall, as in PrusaSlicer.
TEST_CASE("Organic base cone of a short tree is limited by its height", "[TreeSupport][Radius]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    TreeSupportSettings settings;
    settings.layer_height = scaled<coord_t>(0.2);
    settings.branch_radius = scaled<coord_t>(1.);
    settings.bp_radius = scaled<coord_t>(3.75);
    settings.bp_radius_increase_per_layer = scaled<double>(0.1);
    settings.layer_start_bp_radius = 27;
    // The tip of this tree is 2 mm above the bed: a base radius of 1 mm at the bed,
    CHECK(settings.recommendedMinRadius(0, 10) == scaled<coord_t>(1.));
    // less one radius increase for every layer above the bed.
    CHECK(settings.recommendedMinRadius(2, 8) == scaled<coord_t>(0.8));
    // A tree taller than the base diameter keeps the full base.
    CHECK(settings.recommendedMinRadius(0, 100) == scaled<coord_t>(3.75));
    // There is no base cone above its start.
    CHECK(settings.recommendedMinRadius(30, 70) == 0);
}

TEST_CASE("Organic bodies fill holes that the body above covers", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto disc = [](double x, double radius) {
        Polygon circle = make_circle_num_segments(scale_(radius), 64);
        circle.translate(Point::new_scale(x, 0.));
        return circle;
    };
    // A trunk 10 mm wide with a hole around a branch 2 mm wide.
    auto hollow = [&disc](double x, double hole_radius = 3.) {
        Polygon hole = disc(x, hole_radius);
        hole.reverse();
        Polygons trunk{disc(x, 5.), hole};
        if (hole_radius > 1.5)
            trunk.push_back(disc(x, 1.));
        return trunk;
    };
    // A trunk with a slit 0.3 mm wide, narrower than two lines.
    auto slit = [&disc](double x) {
        Polygon hole({Point::new_scale(x - 2., -0.15), Point::new_scale(x + 2., -0.15),
                      Point::new_scale(x + 2., 0.15), Point::new_scale(x - 2., 0.15)});
        hole.reverse();
        return Polygons{disc(x, 5.), hole};
    };
    // A trunk with a slit 0.3 mm wide from its centre out through its side.
    auto notched = [&disc](double x) {
        const Polygon cut({Point::new_scale(x, -0.15), Point::new_scale(x + 6., -0.15),
                           Point::new_scale(x + 6., 0.15), Point::new_scale(x, 0.15)});
        return diff(Polygons{disc(x, 5.)}, Polygons{cut});
    };
    // Trunk A merges into a body without a hole on the top layer, as a branch inside a hollow trunk
    // merges into it. The hole of trunk B goes on to the top. The model passes through trunk C. The hole
    // of trunk D narrows on the top layer to 1.5 mm. The slits of trunks E and F close on the top layer,
    // the one of trunk G goes on to the top.
    std::vector<SupportGeneratorLayer> storage(3);
    std::vector<Polygons> model(3);
    for (size_t i = 0; i < storage.size(); ++i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 1. + 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        const bool top = i + 1 == storage.size();
        layer.polygons = top ? Polygons{disc(0., 5.)} : hollow(0.);
        append(layer.polygons, hollow(20.));
        append(layer.polygons, top ? Polygons{disc(40., 5.)} : hollow(40.));
        append(layer.polygons, hollow(60., top ? 1.5 : 3.));
        append(layer.polygons, top ? Polygons{disc(80., 5.)} : slit(80.));
        append(layer.polygons, top ? Polygons{disc(100., 5.)} : notched(100.));
        append(layer.polygons, notched(120.));
        if (! top)
            model[i] = {disc(40., 0.5)};
    }
    SupportGeneratorLayersPtr bodies;
    for (SupportGeneratorLayer &layer : storage)
        bodies.push_back(&layer);
    organic_fill_covered_holes(bodies, model, scaled<coord_t>(0.22));
    for (size_t i = 0; i + 1 < storage.size(); ++i) {
        CAPTURE(i);
        const Polygons &body = storage[i].polygons;
        // Trunk A holds its branch and the hole around it as one solid body,
        CHECK_THAT(area(intersection(body, {disc(0., 5.)})), Catch::Matchers::WithinRel(area(disc(0., 5.)), 0.001));
        // trunk B keeps the hole that goes on above it,
        CHECK_FALSE(contains(body, Point::new_scale(22.5, 0.)));
        // and so does trunk C, through which the model passes.
        CHECK_FALSE(contains(body, Point::new_scale(42.5, 0.)));
        // The hole of trunk D narrows to the hole above it.
        CHECK(contains(body, Point::new_scale(62.5, 0.)));
        CHECK_FALSE(contains(body, Point::new_scale(61.25, 0.)));
        // Trunks E and F have no slit left,
        CHECK(contains(body, Point::new_scale(80., 0.)));
        CHECK(contains(body, Point::new_scale(102., 0.)));
        // trunk G keeps the slit that goes on above it.
        CHECK_FALSE(contains(body, Point::new_scale(122., 0.)));
    }
}

TEST_CASE("Organic bodies fill the closed bottom of a notch between their branches", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto disc = [](double x, double y, double radius) {
        Polygon circle = make_circle_num_segments(scale_(radius), 64);
        circle.translate(Point::new_scale(x, y));
        return circle;
    };
    // A hole of width mm in a trunk 10 mm wide, at 2 mm from its centre.
    auto holed = [&disc](double x, double width) {
        Polygon hole = disc(x, 2., width / 2.);
        hole.reverse();
        return Polygons{ disc(x, 0., 5.), hole };
    };
    // The same trunk with a notch as wide from the hole out through its side.
    auto notched = [&disc](double x, double width) {
        const Polygon cut({ Point::new_scale(x - width / 2., 2.), Point::new_scale(x + width / 2., 2.),
                            Point::new_scale(x + width / 2., 6.), Point::new_scale(x - width / 2., 6.) });
        return diff(Polygons{ disc(x, 0., 5.) }, union_(Polygons{ cut, disc(x, 2., width / 2.) }));
    };
    // Where two branches merge going down, the notch between them closes into a hole 0.3 mm wide, narrower than
    // two lines, under which trunk A is solid. In a hollow trunk the wall of that hole stands on nothing. In trunk E
    // the hole goes on down for two layers before it closes. The hole of trunk B instead goes on to the top, the
    // notch of trunk C goes on below it. The hole of trunk D, 1 mm wide, stays under the walls of the branches above
    // it, which would otherwise bridge over it, and so does the narrow hole of trunk F, which opens below into one. The
    // holes of trunk G, two layers deep, open below into a notch.
    std::vector<SupportGeneratorLayer> storage(4);
    std::vector<Polygons> model(4);
    for (size_t i = 0; i < storage.size(); ++i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 1. + 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        layer.polygons = i == 0 ? Polygons{ disc(0., 0., 5.) } : i == 1 ? holed(0., 0.3) : notched(0., 0.3);
        append(layer.polygons, i == 0 ? Polygons{ disc(20., 0., 5.) } : holed(20., 0.3));
        append(layer.polygons, i == 1 ? holed(40., 0.3) : notched(40., 0.3));
        append(layer.polygons, i == 0 ? Polygons{ disc(60., 0., 5.) } : i == 1 ? holed(60., 1.) : notched(60., 1.));
        append(layer.polygons, i == 0 ? Polygons{ disc(80., 0., 5.) } : i < 3 ? holed(80., 0.3) : notched(80., 0.3));
        append(layer.polygons, i == 0 ? holed(100., 1.) : i < 3 ? holed(100., 0.3) : notched(100., 0.3));
        append(layer.polygons, i == 1 || i == 2 ? holed(120., 0.3) : notched(120., 0.3));
    }
    SupportGeneratorLayersPtr bodies;
    for (SupportGeneratorLayer &layer : storage)
        bodies.push_back(&layer);
    organic_fill_covered_holes(bodies, model, scaled<coord_t>(0.22));
    for (size_t i = 1; i < 3; ++i) {
        CAPTURE(i);
        const Polygons &body = storage[i].polygons;
        // Trunk A has the hole filled and trunk E the holes over both layers,
        if (i == 1)
            CHECK(contains(body, Point::new_scale(0., 2.)));
        CHECK(contains(body, Point::new_scale(80., 2.)));
        // trunk B keeps the hole that goes on above it,
        CHECK_FALSE(contains(body, Point::new_scale(20., 2.)));
        // trunk C the hole over the notch below,
        CHECK_FALSE(contains(body, Point::new_scale(40., 2.)));
        // trunk D its wide hole, trunk F the narrow hole over a wide one and trunk G the holes over a notch.
        CHECK_FALSE(contains(body, Point::new_scale(60., 2.)));
        CHECK_FALSE(contains(body, Point::new_scale(100., 2.)));
        CHECK_FALSE(contains(body, Point::new_scale(120., 2.)));
    }
}

TEST_CASE("Organic bodies widen under a roof that bridges the gaps between them", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto disc = [](double x, double radius) {
        Polygon circle = make_circle_num_segments(scale_(radius), 64);
        circle.translate(Point::new_scale(x, 0.));
        return circle;
    };
    auto rectangle = [](double x, double y, double width, double depth) {
        return Polygon({Point::new_scale(x, y), Point::new_scale(x + width, y), Point::new_scale(x + width, y + depth),
                        Point::new_scale(x, y + depth)});
    };
    // Lines 0.22 mm wide, tips 0.8 mm across, layers 0.02 mm and a branch angle of 40 degrees, by which a wall may
    // overhang the one below it.
    const coord_t line_width = scaled<coord_t>(0.22), tip_radius = scaled<coord_t>(0.4);
    const double  step       = 0.11;
    const double  move       = std::tan(40. * M_PI / 180.) * 0.02;
    const double  reach      = GENERATE(0.235, 0.18);
    CAPTURE(reach);
    // Two rows of three tips 1.7 mm apart, 0.9 mm between their edges, under strips of roof as wide as the tips,
    // the second row with a sliver of model between two of its tips; a trunk 6 mm wide under a roof reaching
    // 1.5 mm beyond it.
    Polygons tips;
    for (double x : { -1.7, 0., 1.7, 58.3, 60., 61.7 })
        tips.push_back(disc(x, 0.4));
    const Polygons trunk{ disc(20., 3.) };
    const Polygons row{ rectangle(-2.3, -0.4, 4.6, 0.8) };
    const size_t   num_layers = 60, roof_layer = num_layers - 1;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer] = { row.front(), rectangle(16., -2., 8.5, 4.), rectangle(57.7, -0.4, 4.6, 0.8) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 1. + 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        layer.polygons = union_(tips, trunk);
        model[i]       = { rectangle(60.8, -1., 0.1, 2.) };
        bodies[i]      = &layer;
    }
    organic_widen_bodies_under_roofs(bodies, roofs, model, line_width, tip_radius, scaled<coord_t>(move), scaled<coord_t>(reach));
    const Polygons original = union_(tips, trunk);
    const Polygons &top     = storage.back().polygons;
    // The roof over the first row lies within reach of the tips below it, whose flares merge, being printed solid.
    CHECK(diff(row, offset(top, scaled<float>(reach + 0.005))).empty());
    CHECK(contains(top, Point::new_scale(0.6, 0.)));
    CHECK(contains(storage.back().solid_polygons, Point::new_scale(0.85, 0.)));
    // The trunk widens toward the roof beyond it by a band printed solid.
    CHECK(contains(top, Point::new_scale(23.3, 0.)));
    CHECK(contains(storage.back().solid_polygons, Point::new_scale(23.3, 0.)));
    for (size_t i = 0; i < storage.size(); ++ i) {
        CAPTURE(i);
        const Polygons &body  = storage[i].polygons;
        const Polygons  added = diff(body, original);
        // A layer overhangs the one below it by the branch angle at most.
        if (i > 0)
            CHECK(diff(body, offset(storage[i - 1].polygons, scaled<float>(move + 0.005))).empty());
        // Nothing grows beyond the roof or into the model.
        CHECK(diff(added, offset(roofs[roof_layer], scaled<float>(step + 0.005))).empty());
        CHECK(intersection(added, model[i]).empty());
        // The widened tips are printed solid, the trunk is not.
        if (! added.empty())
            CHECK(diff(intersection(body, offset(tips, scaled<float>(1.))), offset(storage[i].solid_polygons, SCALED_EPSILON)).empty());
        CHECK_FALSE(contains(storage[i].solid_polygons, Point::new_scale(20., 0.)));
    }
    // The tips widen over the layers the branch angle takes them, no more.
    CHECK(diff(storage.front().polygons, original).empty());
    CHECK(diff(original, storage.front().polygons).empty());
}

TEST_CASE("Organic bodies under a roof close the gap between them that the roof hangs over", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto rectangle = [](double x, double y, double width, double depth) {
        return Polygon({Point::new_scale(x, y), Point::new_scale(x + width, y), Point::new_scale(x + width, y + depth),
                        Point::new_scale(x, y + depth)});
    };
    // Lines 0.22 mm wide, tips 0.8 mm across, layers 0.02 mm and a branch angle of 40 degrees.
    const coord_t line_width = scaled<coord_t>(0.22), tip_radius = scaled<coord_t>(0.4);
    const double  move       = std::tan(40. * M_PI / 180.) * 0.02;
    const double  reach      = 0.235;
    // Two trunks 3 mm by 1.5 mm under a roof over both whose lines run along the gap between them: the line in the middle
    // of it passes over neither. Bodies widening under a roof keep a line and a half apart; 0.27 mm apart, they are
    // closer than that already, and 0.5 mm apart, they would stop with a channel that wide between them.
    const double   gap    = GENERATE(0.27, 0.5);
    CAPTURE(gap);
    const Polygons trunks{ rectangle(0., 0., 3., 1.5), rectangle(0., 1.5 + gap, 3., 1.5) };
    const size_t   num_layers = 20, roof_layer = num_layers - 1;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer] = { rectangle(0., 0., 3., 3. + gap) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 1. + 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        layer.polygons = trunks;
        bodies[i]      = &layer;
    }
    organic_widen_bodies_under_roofs(bodies, roofs, model, line_width, tip_radius, scaled<coord_t>(move), scaled<coord_t>(reach),
        { 0. });
    // The bodies close the gap under the roof with their bands: a line touching a body with its edge only, running along
    // a gap, would hang off it.
    const Polygons gap_area{ rectangle(0., 1.5, 3., gap) };
    CHECK(diff(gap_area, offset(storage.back().polygons, scaled<float>(0.005))).empty());
    const Point band = Point::new_scale(1.5, 1.52);
    CHECK(contains(storage.back().solid_polygons, band));
}

TEST_CASE("Organic bodies widen going up by half a line a layer at most", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto rect = [](double x0, double y0, double x1, double y1) {
        return Polygon({ Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1) });
    };
    // A body 2 mm deep that a model narrowing going up leaves 1 mm more room on layer 4, a branch leaning by 0.05 mm a
    // layer, another body beside the model top it may stand on, one with a sliver narrower than a line below the room,
    // an island in the air from layer 6, which a crumb check is for, and a body left room by 1 mm on two sides at its
    // corner. A branch angle moving a branch by less than half a line a layer limits the body over the model top where
    // it may not stand on it.
    const bool   model_roots = GENERATE(false, true);
    const double move        = GENERATE(0.11, 0.05);
    CAPTURE(model_roots, move);
    const size_t num_layers = 12, jump = 4;
    const double step = 0.11, line_width = 0.22;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers + 1), model(num_layers + 1);
    SupportGeneratorLayersPtr bodies(num_layers + 1, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        const double lean = 0.05 * double(i);
        layer.polygons = union_(Polygons{ rect(0., 0., 4., i < jump ? 2. : 3.), rect(6. + lean, 0., 8. + lean, 2.),
            rect(10., 0., 14., i < jump ? 2. : 3.), rect(16., 0., 18., i < jump ? 2. : 2.6) });
        if (i < jump)
            layer.polygons = union_(layer.polygons, Polygons{ rect(17.9, 2., 18., 2.5) });
        if (i >= 6)
            layer.polygons.push_back(rect(20., 0., 21., 1.));
        layer.polygons.push_back(i < jump ? rect(24., 0., 26., 2.) : rect(24., 0., 27., 3.));
        bodies[i] = &layer;
    }
    model[jump - 1] = { rect(10., 2., 14., 3.) };
    organic_limit_body_growth(bodies, roofs, model, model_roots, scaled<coord_t>(line_width), scaled<coord_t>(move));
    for (size_t i = 0; i < num_layers; ++ i) {
        CAPTURE(i);
        const Polygons &body = storage[i].polygons;
        // The body moves into the room by the step a layer, its walls each half on the wall below.
        const double edge = i < jump ? 2. : std::min(3., 2. + step * double(i + 1 - jump));
        CHECK(contains(body, Point::new_scale(2., edge - 0.01)));
        CHECK_FALSE(contains(body, Point::new_scale(2., edge + 0.01)));
        // The branch leans within the step.
        CHECK(contains(body, Point::new_scale(7.99 + 0.05 * double(i), 1.)));
        // Over the model top a body stands on it where it may, and widens by the branch angle where it may not.
        if (i >= jump) {
            const double over_model = model_roots ? 3. : 2. + move * double(i + 1 - jump);
            CHECK(contains(body, Point::new_scale(12., over_model - 0.01)));
            CHECK_FALSE(contains(body, Point::new_scale(12., over_model + 0.01)));
        }
        // A sliver narrower than a line prints no wall to stand on.
        if (i == jump)
            CHECK_FALSE(contains(body, Point::new_scale(17.95, 2.2)));
        CHECK(contains(body, Point::new_scale(20.5, 0.5)) == (i >= 6));
        // The corner moves by the step, not further along its diagonal.
        if (i == jump) {
            CHECK(contains(body, Point::new_scale(26.05, 2.05)));
            CHECK_FALSE(contains(body, Point::new_scale(26.1, 2.1)));
        }
    }
}

TEST_CASE("Organic hollow bodies narrow going up by half a line a layer at most", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto rect = [](double x0, double y0, double x1, double y1) {
        return Polygon({ Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1) });
    };
    // From layer 4 on: a body 2 mm deep narrows to 1 mm, a trunk 4 mm wide splits into two branches 1 mm apart, a body
    // narrows to 1 mm where the model comes within 1.2 mm, a body ends, a body narrows under a roof of its own layer; and
    // a branch leans by 0.05 mm a layer.
    const size_t num_layers = 16, jump = 4;
    const double step = 0.11;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers + 1), model(num_layers + 1);
    SupportGeneratorLayersPtr bodies(num_layers + 1, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        const double lean = 0.05 * double(i);
        layer.polygons = i < jump ?
            union_(Polygons{ rect(0., 0., 4., 2.), rect(10., 0., 14., 2.), rect(20., 0., 24., 2.), rect(30., 0., 32., 2.),
                rect(40., 0., 44., 2.) }) :
            union_(Polygons{ rect(0., 0., 4., 1.), rect(10., 0., 11.5, 2.), rect(12.5, 0., 14., 2.), rect(20., 0., 24., 1.),
                rect(40., 0., 44., 1.) });
        layer.polygons = union_(layer.polygons, Polygons{ rect(50. + lean, 0., 52. + lean, 2.) });
        if (i >= jump)
            model[i] = { rect(20., 1.2, 24., 3.) };
        bodies[i] = &layer;
    }
    roofs[jump] = { rect(40., 1., 44., 2.) };
    organic_limit_body_shrink(bodies, roofs, model, scaled<coord_t>(step));
    for (size_t i = jump; i < num_layers; ++ i) {
        CAPTURE(i);
        const Polygons &body = storage[i].polygons;
        const double    in   = step * double(i + 1 - jump);
        // The body narrows by the step a layer, its walls each half on the wall below.
        const double edge = std::max(1., 2. - in);
        CHECK(contains(body, Point::new_scale(2., edge - 0.01)));
        CHECK_FALSE(contains(body, Point::new_scale(2., edge + 0.01)));
        // The gap between the branches opens from its ends by the step a layer.
        CHECK_FALSE(contains(body, Point::new_scale(12., in - 0.01)));
        CHECK_FALSE(contains(body, Point::new_scale(12., 2. - in + 0.01)));
        CHECK(contains(body, Point::new_scale(12., 1.)) == (in < 1.));
        // Not into the model, from which it narrows a step a layer.
        const double cut = std::max(1., 1.2 - step * double(i - jump));
        CHECK(contains(body, Point::new_scale(22., cut - 0.01)));
        CHECK_FALSE(contains(body, Point::new_scale(22., cut + 0.01)));
        // A body ending stays ended, and a roof of the layer stands for the body.
        CHECK_FALSE(contains(body, Point::new_scale(31., 1.)));
        CHECK_FALSE(contains(body, Point::new_scale(42., 1.5)));
        // The branch leans within the step.
        CHECK_FALSE(contains(body, Point::new_scale(49.99 + 0.05 * double(i), 1.)));
    }
}

TEST_CASE("Organic bodies merged going up narrow down into their trunks by the branch angle", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto disc = [](double x, double y, double radius) {
        Polygon circle = make_circle_num_segments(scale_(radius), 128);
        circle.translate(Point::new_scale(x, y));
        return circle;
    };
    // Two trunks 2 mm across, 1 mm apart, merging going up into a body 5.2 mm across, and going down near the bed into
    // one 3.2 mm across, beside the model 0.6 mm across standing on the bed; and one trunk 5.2 mm across splitting going
    // up into two branches. A branch angle moving a wall by 0.1 mm a layer.
    const Polygons trunks{ disc(0., 0., 1.), disc(3., 0., 1.) };
    const Polygons merged{ disc(1.5, 0., 2.6) };
    const Polygons foot{ disc(1.5, 0., 1.6) };
    const Polygons standing{ disc(1.5, 2., 0.3) };
    const Polygons trunk{ disc(20., 0., 2.6) };
    const Polygons branches{ disc(18.5, 0., 1.), disc(21.5, 0., 1.) };
    const size_t   num_layers = 20, merge_layer = 10;
    const double   move = 0.1;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> model(num_layers + 1);
    SupportGeneratorLayersPtr bodies(num_layers + 1, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        layer.polygons = union_(i < 5 ? foot : i < merge_layer ? trunks : merged, i < merge_layer ? trunk : branches);
        if (i < 5)
            model[i] = standing;
        bodies[i] = &layer;
    }
    organic_keep_merged_bodies_whole(bodies, model, scaled<coord_t>(move));
    for (size_t i = 0; i < merge_layer; ++ i) {
        CAPTURE(i);
        const Polygons &body = storage[i].polygons;
        // Under the body the trunks merge into, the body goes on whole, whatever the trunks do below it, narrowing by
        // the branch angle into them: it holds the gap between the trunks while it reaches over it, away from the model
        // standing there, as the model stands under it on a layer below.
        const double radius = 2.6 - move * double(merge_layer - i);
        CHECK(contains(body, Point::new_scale(1.5, 0.05 - radius)));
        CHECK_FALSE(contains(body, Point::new_scale(1.5, - 0.05 - radius)));
        if (radius > 1.)
            CHECK(contains(body, Point::new_scale(1.5, 0.)));
        CHECK(intersection(body, standing).empty());
        CHECK(diff(intersection(body, offset(merged, scaled<float>(1.))), merged).empty());
        // A trunk splitting going up stays as it is.
        CHECK(diff(intersection(body, offset(trunk, scaled<float>(1.))), trunk).empty());
        CHECK(diff(trunk, body).empty());
    }
    for (size_t i = merge_layer; i < num_layers; ++ i)
        CHECK(diff(storage[i].polygons, union_(merged, branches)).empty());
}

TEST_CASE("Organic trees inside hollow bodies stop when the slicing is cancelled", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    TreeSupportSettings settings;
    settings.support_line_width         = scaled<coord_t>(0.22);
    settings.layer_height               = scaled<coord_t>(0.02);
    settings.min_radius                 = scaled<coord_t>(0.4);
    settings.maximum_move_distance      = scaled<coord_t>(0.01);
    settings.maximum_move_distance_slow = scaled<coord_t>(0.01);
    SupportGeneratorLayer layer;
    layer.print_z  = 0.02;
    layer.height   = 0.02;
    layer.polygons = { make_circle_num_segments(scale_(5.), 64) };
    SupportGeneratorLayersPtr bodies{ &layer };
    const std::vector<Polygons> roofs(1), model(1);
    const auto grow = [&] {
        organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(0.235), settings.support_line_width,
            {}, [] { throw CanceledException(); });
    };
    CHECK_THROWS_AS(grow(), CanceledException);
}

TEST_CASE("Organic roofs over the inside of a hollow body stand on trees grown inside it", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto disc = [](double x, double radius) {
        Polygon circle = make_circle_num_segments(scale_(radius), 128);
        circle.translate(Point::new_scale(x, 0.));
        return circle;
    };
    auto rectangle = [](double x, double y, double width, double depth) {
        return Polygon({Point::new_scale(x, y), Point::new_scale(x + width, y), Point::new_scale(x + width, y + depth),
                        Point::new_scale(x, y + depth)});
    };
    // Lines 0.22 mm wide, 0.25 mm apart in the base or 2.5 as by default, layers 0.02 mm, tips 0.8 mm across, a branch
    // angle of 40 degrees, a preferred one of 25 and a diameter angle of 5.
    const double spacing    = GENERATE(0.25, 2.5);
    const double line_width = 0.22, reach = 0.5 * (spacing + line_width), layer_height = 0.02;
    CAPTURE(spacing);
    const double move       = std::tan(40. * M_PI / 180.) * layer_height;
    const double grow       = std::tan(5. * M_PI / 180.) * layer_height;
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(move);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(grow);
    settings.support_rests_on_model           = false;
    // The known material is the perimeter and the reserved tree feet, not the whole sparse first-layer flange.
    // A second wall of a wide body, chosen by the toolpaths, cuts the trees when they are printed.
    auto walls = [&](const SupportGeneratorLayer &layer) {
        const Polygons outline = union_(layer.polygons);
        return diff(outline, offset(outline, - scaled<float>(line_width)));
    };
    // A trunk 6 mm across under a roof reaching 1 mm in from its wall and under a roof beside its wall; a trunk 12 mm across
    // under a roof 3 mm across in its middle, farther from its wall than a tree leaning by the branch angle gets above
    // the bed; a trunk 6 mm across with a hole the model passes through under a roof next to the hole; a trunk narrowing
    // from 7 mm across under the roof to 2 mm on the bed under a roof 1.8 mm across in its middle; two branches 3 mm
    // across, 0.6 mm apart, merging going down into a trunk 7 mm across, whose walls stand over its inside; a trunk 6 mm
    // across with a hole 3 mm across in its middle under a roof over the hole; a trunk 6 mm across under a roof in its
    // middle, whose path region is wide enough for a second wall up to halfway. The roof lies 2.4 mm above the bed.
    const Polygons small{ disc(0., 3.) };
    const Polygons near_wall{ rectangle(1.5, -0.6, 1.4, 1.2) };
    const Polygons beside{ rectangle(-2.7, -0.3, 0.1, 0.6) };
    const Polygons wide{ disc(20., 6.) };
    const Polygons middle{ disc(20., 1.5) };
    const Polygons pierced = diff(Polygons{ disc(40., 3.) }, Polygons{ disc(40.8, 0.8) });
    const Polygons passing{ disc(40.8, 0.5) };
    const Polygons beside_hole{ disc(39.2, 0.8) };
    const Polygons narrowing_middle{ disc(60., 0.9) };
    const Polygons branches{ disc(78.2, 1.5), disc(81.8, 1.5) };
    const Polygons ring = diff(Polygons{ disc(100., 3.) }, Polygons{ disc(100., 1.5) });
    const Polygons over_hole{ disc(100., 1.2) };
    const size_t   second_layer = 60;
    const Polygons single{ disc(120., 3.) }, doubled{ disc(120., 3.015) };
    const Polygons over_doubled{ disc(120., 1.2) };
    const Polygons merged{ disc(80., 3.5) };
    const size_t   num_layers = 121, roof_layer = num_layers - 1, merge_layer = 100;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer] = union_(Polygons{ near_wall.front(), beside.front(), middle.front(), beside_hole.front(),
                                         narrowing_middle.front(), over_hole.front(), over_doubled.front() });
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = union_(union_(small, wide), pierced);
        layer.polygons.push_back(disc(60., 1. + 2.5 * double(i) / double(roof_layer - 1)));
        append(layer.polygons, i < merge_layer ? merged : branches);
        append(layer.polygons, ring);
        append(layer.polygons, i < second_layer ? doubled : single);
        model[i]       = passing;
        bodies[i]      = &layer;
    }
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width);
    auto material = [&](size_t i) { return union_(walls(storage[i]), storage[i].inner_trees); };
    // Every point of the roof over the bodies lies within reach of the walls and the trees below it.
    const SupportGeneratorLayer &top = storage.back();
    CHECK_FALSE(top.inner_trees.empty());
    CHECK(diff(intersection(roofs[roof_layer], top.polygons), offset(material(roof_layer - 1), scaled<float>(reach + 0.005))).empty());
    // The trees inside a body wide enough for a second wall grow up to its first wall all the same, fusing with it by a
    // quarter of a line: the paths decide on the second wall by the outline they print, which may differ from the body,
    // and a tree kept a line farther in would end short of the only wall printed.
    const Polygons first_fuse = diff(offset(doubled, - scaled<float>(0.75 * line_width + 0.005)),
                                     offset(doubled, - scaled<float>(1.25 * line_width)));
    bool reached_first = false;
    for (size_t i = 0; i < second_layer; ++ i)
        reached_first = reached_first || ! intersection(storage[i].inner_trees, first_fuse).empty();
    CHECK(reached_first);
    // Every point of the roof over the hole in a body lies within reach of the walls and the trees below it: a hole is
    // as open as the inside of a body.
    CHECK(offset(diff(over_hole, offset(material(roof_layer - 1), scaled<float>(reach + 0.005))), - scaled<float>(0.005)).empty());
    // Nothing grows where the roof lies within reach of a wall.
    CHECK(intersection(top.inner_trees, offset(beside, scaled<float>(0.3))).empty());
    // Every bead of the walls of the branches over the inside of the trunk they merge into stands on the walls and the
    // trees below it, touching them at least: a bead does not bridge like a roof.
    CHECK_FALSE(storage[merge_layer - 1].inner_trees.empty());
    CHECK(diff(intersection(walls(storage[merge_layer]), storage[merge_layer - 1].polygons),
               offset(material(merge_layer - 1), scaled<float>(line_width + 0.005))).empty());
    for (size_t i = 0; i < storage.size(); ++ i) {
        CAPTURE(i);
        const Polygons &trees = storage[i].inner_trees;
        // Above the bed the trees fuse with the walls by a quarter of a line. On the first layer their feet replace
        // the flange in their area, including its perimeter where necessary. Neither can enter the model.
        if (i > 0)
            CHECK(intersection(trees, offset(walls(storage[i]), - scaled<float>(0.25 * line_width + 0.005))).empty());
        else
            CHECK(diff(trees, storage[i].polygons).empty());
        CHECK(offset(intersection(trees, model[i]), - scaled<float>(0.005)).empty());
        // No part of a tree hangs in the air: each stands on the trees or the walls below it.
        if (i > 0)
            for (const ExPolygon &part : union_ex(trees))
                CHECK_FALSE(intersection(to_polygons(part), union_(storage[i - 1].inner_trees, walls(storage[i - 1]))).empty());
        // Every part of a tree prints a loop half a line inside it, at least a line long once its seam is clipped by 0.15 of
        // a line: a shorter one is a dot, which the paths leave out, so that the part would hold nothing up.
        for (const ExPolygon &part : union_ex(trees)) {
            const Polygons loops = offset(part, - scaled<float>(0.5 * line_width));
            CHECK(std::any_of(loops.begin(), loops.end(), [&](const Polygon &loop) { return loop.length() >= scaled<double>(1.15 * line_width); }));
        }
        // A tree leans by the branch angle at most, and widens by the diameter angle, and where it ends, it stands on the
        // walls below it, each bead of it on them: a tree going into a wall ends in a bead beside it, fused with it. Slivers
        // of a hundredth left over by the offsets do not count.
        if (i > 0)
            CHECK(offset(diff(trees, union_(offset(storage[i - 1].inner_trees, scaled<float>(move + grow + 0.005)),
                                            offset(walls(storage[i - 1]), scaled<float>(line_width + 0.005)))),
                         - scaled<float>(0.005)).empty());
    }
    // With the base lines farther apart than a line, all of the roof over the small trunk lies within reach of its wall, and
    // nothing grows in the wide one but a few trees, the base pattern spacing apart.
    if (spacing > 1.)
        return;
    // The trees under the roof near the wall of the small trunk grow out of the wall: the farthest point of the roof lies
    // 1.5 mm from it, and a tree there leans, widening, until what is left of it inside the wall is too thin to print.
    const double lean = 1.5 + 0.4 - 1.75 * line_width;
    for (size_t i = 0; i + size_t(std::ceil(lean / (move - grow))) + 1 < roof_layer; ++ i)
        CHECK(intersection(storage[i].inner_trees, small).empty());
    // The trees in the middle of the wide trunk go down to the bed, merging into fewer as they go.
    const auto parts_in = [&](size_t i) { return union_ex(intersection(storage[i].inner_trees, wide)).size(); };
    size_t most = 0;
    for (size_t i = 1; i < storage.size(); ++ i)
        most = std::max(most, parts_in(i));
    CHECK(parts_in(1) > 0);
    CHECK(parts_in(1) < most);
}

namespace roof_tips_test {

using namespace Slic3r;

// Tips 0.8 mm across under 0.22 mm lines, which stay apart by their diameter and a line.
constexpr double tip_radius = 0.4, roof_line = 0.22, min_distance = 2. * tip_radius + roof_line;

// The interface under an 11 mm cube, with a notch cut out of a corner by the avoidance of a foot.
Polygons cube_interface(const Vec2d &shift = Vec2d::Zero())
{
    Polygon outline({Point::new_scale(-5.5, -3.65), Point::new_scale(-3.65, -3.65), Point::new_scale(-3.65, -5.5),
                     Point::new_scale(5.5, -5.5), Point::new_scale(5.5, 5.5), Point::new_scale(-5.5, 5.5)});
    outline.translate(Point::new_scale(shift.x(), shift.y()));
    return {outline};
}

Points tips_of(const Polygons &area, double connect_length)
{
    Points tips;
    for (const Polyline &line : Slic3r::TreeSupport3D::organic_roof_tips(area, scaled<coord_t>(roof_line), scaled<coord_t>(tip_radius),
                                                                          scaled<coord_t>(connect_length)))
        append(tips, line.points);
    return tips;
}

double nearest(const Points &tips, const Vec2d &p)
{
    double best = std::numeric_limits<double>::max();
    for (const Point &tip : tips)
        best = std::min(best, (unscaled(tip) - p).norm());
    return best;
}

// The largest distance from a point of the area inset by half a line, where the tips stand, to the nearest tip, sampled
// 0.05 mm apart.
double worst_cover(const Points &tips, const Polygons &area)
{
    const Polygons    inner = offset(area, -scaled<float>(roof_line / 2.), jtMiter, 3.);
    const BoundingBox box   = get_extents(inner);
    double            worst = 0.;
    for (double x = unscaled(box.min.x()); x <= unscaled(box.max.x()); x += 0.05)
        for (double y = unscaled(box.min.y()); y <= unscaled(box.max.y()); y += 0.05)
            if (contains(inner, Point::new_scale(x, y)))
                worst = std::max(worst, nearest(tips, Vec2d(x, y)));
    return worst;
}

} // namespace roof_tips_test

TEST_CASE("Organic trees under a roof over a hollow body hold every line of its lowest layer", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Lines 0.22 mm wide, 0.25 mm apart in the base, layers 0.02 mm, tips 0.8 mm across, a branch angle of 40 degrees, a
    // preferred one of 25 and a diameter angle of 5.
    const double line_width = 0.22, spacing = 0.25, reach = 0.5 * (spacing + line_width), layer_height = 0.02;
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(std::tan(40. * M_PI / 180.) * layer_height);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(std::tan(5. * M_PI / 180.) * layer_height);
    settings.support_rests_on_model           = false;
    // The lines of the roof run along Y, or at 45 degrees to the axes, as the support angle sets them.
    const double angle = GENERATE(0.5 * M_PI, 0.25 * M_PI);
    CAPTURE(angle);
    // A trunk 10 mm across under a roof 7 mm square in its middle, 1.2 mm above the bed.
    const Polygon trunk = make_circle_num_segments(scale_(5.), 256);
    const Polygon roof({ Point::new_scale(-3.5, -3.5), Point::new_scale(3.5, -3.5), Point::new_scale(3.5, 3.5),
                         Point::new_scale(-3.5, 3.5) });
    const size_t num_layers = 61, roof_layer = num_layers - 1;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer] = { roof };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = { trunk };
        bodies[i]      = &layer;
    }
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width,
        { angle });
    // A line of the lowest roof layer, wherever it lies, touches the trees and the wall it passes over by half a line at
    // least every line between two lines of the base, and its ends rest on them: a stretch ending in the air hangs once it
    // is longer than a line.
    const Polygons outline  = union_(storage.back().polygons);
    Polygons       touched  = offset(union_(diff(outline, offset(outline, - scaled<float>(line_width))), storage.back().inner_trees),
        scaled<float>(0.5 * line_width));
    Polygon        rotated  = roof;
    rotated.rotate(- angle);
    for (Polygon &polygon : touched)
        polygon.rotate(- angle);
    const BoundingBox box     = get_extents(rotated);
    const Polygons    bearing = offset(touched, scaled<float>(0.001));
    double            bridge = 0., end = 0.;
    for (coord_t y = box.min.y() + scaled<coord_t>(0.013); y < box.max.y(); y += scaled<coord_t>(0.07)) {
        const Polylines line = intersection_pl(Polylines{ Polyline({ Point(box.min.x() - 1, y), Point(box.max.x() + 1, y) }) },
            Polygons{ rotated });
        for (const Polyline &free : diff_pl(line, touched)) {
            const bool held = contains(bearing, free.first_point()) && contains(bearing, free.last_point());
            double    &longest = held ? bridge : end;
            longest = std::max(longest, unscaled<double>(free.length()));
        }
    }
    // The code holds them to these, with lines touching within 0.375 of a line; measured here outside half a line.
    const double touch = 0.375 * line_width, band = 0.5 * line_width - touch;
    CHECK(bridge <= 2. * (reach - touch - band) + 0.01);
    CHECK(end <= std::min(line_width, reach - touch) - band + 0.01);
}

TEST_CASE("Organic trees end on what their beads touch, not beside it", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Lines 0.22 mm wide, 0.25 mm apart in the base, layers 0.02 mm, tips 0.8 mm across, a branch angle of 40 degrees, a
    // preferred one of 25 and a diameter angle of 5.
    const double line_width = 0.22, spacing = 0.25, reach = 0.5 * (spacing + line_width), layer_height = 0.02;
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(std::tan(40. * M_PI / 180.) * layer_height);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(std::tan(5. * M_PI / 180.) * layer_height);
    settings.support_rests_on_model           = false;
    // A trunk 6 mm across and the last part of a tree on its top layer within a line and the move of a layer of the bead of
    // its wall all over, as the end of a tree going into the wall is. 0.135 mm from it, it does not touch it, as where the
    // model bars its way to the wall; 0.01 mm from it, as where the wall below lies a little farther out, the beads of the
    // two fuse all the same, the paths printing a tree a quarter of a line into the wall beside it.
    const double gap = GENERATE(0.135, 0.01);
    CAPTURE(gap);
    const double top    = 3. - 0.75 * line_width - gap;
    const Polygon trunk = make_circle_num_segments(scale_(3.), 256);
    const Polygon end({ Point::new_scale(-0.15, top - 0.1), Point::new_scale(0.15, top - 0.1), Point::new_scale(0.15, top), Point::new_scale(-0.15, top) });
    const size_t num_layers = 31;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = { trunk };
        bodies[i]      = &layer;
    }
    storage.back().inner_trees = { end };
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width);
    // A tree grows under an end beside the wall but not touching it, a bead of which would hang, and not under one that
    // touches it.
    CHECK(intersection(Polygons{ end }, storage[num_layers - 2].inner_trees).empty() == (gap < 0.25 * line_width));
}

TEST_CASE("Organic trees keep out of a hole of a body that opens to the outside below", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Lines 0.22 mm wide, 0.25 mm apart in the base, layers 0.02 mm, tips 0.8 mm across, a branch angle of 40 degrees, a
    // preferred one of 25 and a diameter angle of 5.
    const double line_width = 0.22, spacing = 0.25, reach = 0.5 * (spacing + line_width), layer_height = 0.02;
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(std::tan(40. * M_PI / 180.) * layer_height);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(std::tan(5. * M_PI / 180.) * layer_height);
    settings.support_rests_on_model           = false;
    // A body 6 mm square, 2 mm tall, with a slot 2 mm wide in it: in its upper half a hole of the body, in its lower half
    // open to the outside, as where two branches merge going up around a gap between them. A roof on the top layer over
    // the hole and the body on both sides of it: the trees under it lean toward the nearest walls, those of the hole among
    // them.
    const Polygon square({ Point::new_scale(-3., -3.), Point::new_scale(3., -3.), Point::new_scale(3., 3.), Point::new_scale(-3., 3.) });
    const Polygon hole({ Point::new_scale(-1., -1.), Point::new_scale(1., -1.), Point::new_scale(1., 2.), Point::new_scale(-1., 2.) });
    const Polygon slot({ Point::new_scale(-1., -1.), Point::new_scale(1., -1.), Point::new_scale(1., 3.5), Point::new_scale(-1., 3.5) });
    const Polygon roof({ Point::new_scale(-2.4, -0.4), Point::new_scale(2.4, -0.4), Point::new_scale(2.4, 1.4), Point::new_scale(-2.4, 1.4) });
    const size_t num_layers = 100, opening = 50;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = diff(Polygons{ square }, Polygons{ i < opening ? slot : hole });
        bodies[i]      = &layer;
    }
    roofs.back() = { roof };
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width);
    REQUIRE_FALSE(storage[num_layers - 2].inner_trees.empty());
    // What the trees print on the lowest layer of the hole lies within a line and the move of a layer of what the layer
    // below prints, as the end of a tree beside a wall does: none goes down the hole to hang over the slot.
    const Polygons below = union_(storage[opening - 1].polygons, storage[opening - 1].inner_trees);
    CHECK(area(diff(storage[opening].inner_trees, offset(below, float(settings.support_line_width + settings.maximum_move_distance)))) <
        scaled<double>(0.01) * scaled<double>(0.01));
}

TEST_CASE("Organic trees whose way to the wall the model bars go on down as one", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Lines 0.22 mm wide, 0.25 mm apart in the base, layers 0.02 mm, tips 0.8 mm across, a branch angle of 40 degrees, a
    // preferred one of 25 and a diameter angle of 5.
    const double line_width = 0.22, spacing = 0.25, reach = 0.5 * (spacing + line_width), layer_height = 0.02;
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(std::tan(40. * M_PI / 180.) * layer_height);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(std::tan(5. * M_PI / 180.) * layer_height);
    settings.support_rests_on_model           = false;
    // A trunk 6 mm across, 2 mm tall, a roof on its top layer over its inside, and the model, which the trees keep out of,
    // between the roof and the nearest wall all the way down: the trees under the roof leaning toward the wall run into it.
    const Polygon trunk = make_circle_num_segments(scale_(3.), 256);
    const Polygon roof({ Point::new_scale(-0.4, 1.2), Point::new_scale(0.4, 1.2), Point::new_scale(0.4, 1.8), Point::new_scale(-0.4, 1.8) });
    const Polygon wall_side({ Point::new_scale(-1., 2.2), Point::new_scale(1., 2.2), Point::new_scale(1., 2.6), Point::new_scale(-1., 2.6) });
    const size_t num_layers = 100;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers), model(num_layers, Polygons{ wall_side });
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = { trunk };
        bodies[i]      = &layer;
    }
    roofs.back() = { roof };
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width);
    // The trees under the roof start on the layer under it, and none further down: a tree the model stops goes on, rather
    // than a tree growing under its end, to run into the model in turn, and so on down.
    size_t trees = 0;
    size_t tips  = 0;
    for (size_t i = 0; i + 2 < num_layers; ++ i)
        for (const ExPolygon &part : union_ex(storage[i].inner_trees)) {
            ++ trees;
            if (intersection(to_polygons(part), storage[i + 1].inner_trees).empty())
                ++ tips;
        }
    REQUIRE(trees > 0);
    CHECK(tips == 0);
}

TEST_CASE("Organic trees that the model widens under go on down whole beside it", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Lines 0.22 mm wide, 0.25 mm apart in the base, layers 0.02 mm, tips 0.8 mm across, a branch angle of 40 degrees, a
    // preferred one of 25 and a diameter angle of 5.
    const double line_width = 0.22, spacing = 0.25, reach = 0.5 * (spacing + line_width), layer_height = 0.02;
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(std::tan(40. * M_PI / 180.) * layer_height);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(std::tan(5. * M_PI / 180.) * layer_height);
    settings.support_rests_on_model           = false;
    // A trunk 6 mm across, 2 mm tall, a roof on its top layer over its inside, and the model, which the trees keep out of,
    // between the roof and the nearest wall, its edge coming 0.025 mm nearer the roof a layer going down, more than the
    // branch angle moves a tree: the trees under the roof run into it, and it widens under them.
    const Polygon trunk = make_circle_num_segments(scale_(3.), 256);
    const Polygon roof({ Point::new_scale(-0.4, 1.2), Point::new_scale(0.4, 1.2), Point::new_scale(0.4, 1.8), Point::new_scale(-0.4, 1.8) });
    const size_t num_layers = 100;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = { trunk };
        bodies[i]      = &layer;
        const double edge = 2.1 - 0.025 * double(num_layers - 1 - i);
        model[i] = { Polygon({ Point::new_scale(-1., edge), Point::new_scale(1., edge), Point::new_scale(1., 2.9), Point::new_scale(-1., 2.9) }) };
    }
    roofs.back() = { roof };
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width);
    // The trees under the roof go on down whole, away from the model as it widens under them, rather than shrinking until
    // they end, each with a tree growing under its end, which the model cuts in turn, and so on down.
    double thinnest = std::numeric_limits<double>::max();
    for (size_t i = 1; i + 2 < num_layers; ++ i)
        for (const ExPolygon &part : union_ex(storage[i].inner_trees))
            thinnest = std::min(thinnest, part.area());
    REQUIRE(thinnest < std::numeric_limits<double>::max());
    CHECK(thinnest > 0.8 * M_PI * sqr(scaled<double>(0.4)));
}

TEST_CASE("Organic tips under an area interface cover it up to its corners", "[TreeSupport][Interface][Regression]")
{
    using namespace roof_tips_test;
    // Branch Density 20 % spaces the tips 0.22 mm * 100 / 20 + (0.8 mm - 0.22 mm) = 1.68 mm apart.
    const double   spacing = 1.68;
    const Points   tips    = tips_of(cube_interface(), spacing);
    REQUIRE_FALSE(tips.empty());
    // Up to the step between the points tested, every point lies within 0.7 spacing of a tip.
    CHECK(worst_cover(tips, cube_interface()) <= 0.7 * spacing + spacing / 8.);
    // Every convex corner of the interface, one of the notch too, has a tip at its corner inset by half a line.
    for (const Vec2d &corner : {Vec2d(-5.5, -3.65), Vec2d(-3.65, -5.5), Vec2d(5.5, -5.5), Vec2d(5.5, 5.5), Vec2d(-5.5, 5.5)}) {
        CAPTURE(corner.x(), corner.y());
        CHECK(nearest(tips, corner) <= roof_line / 2. * std::sqrt(2.) + 0.001);
    }
}

TEST_CASE("Organic tips under an area interface stay apart at any Branch Density", "[TreeSupport][Interface][Regression]")
{
    using namespace roof_tips_test;
    // Branch Density 20, 30 and 100 %. From about 25 % on, 0.7 of the spacing is less than the least distance between
    // tips, from 50 % on the spacing itself.
    const double connect_length = GENERATE(1.68, 0.22 * 100. / 30. + 0.58, 0.8);
    CAPTURE(connect_length);
    const Points tips = tips_of(cube_interface(), connect_length);
    REQUIRE(tips.size() > 1);
    double closest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < tips.size(); ++ i)
        for (size_t j = i + 1; j < tips.size(); ++ j)
            closest = std::min(closest, (unscaled(tips[i]) - unscaled(tips[j])).norm());
    CHECK(closest >= min_distance - 0.001);
    // They still cover the area, as closely as they may stand.
    const double spacing = std::max(connect_length, min_distance);
    CHECK(worst_cover(tips, cube_interface()) <= std::max(0.7 * spacing, min_distance) + spacing / 8.);
}

TEST_CASE("Organic tips under an area interface lie on a lattice fixed in the object's coordinates", "[TreeSupport][Interface][Regression]")
{
    using namespace roof_tips_test;
    // Tips of flat overhangs stacked above each other line up: an interface moved aside keeps its lattice tips.
    for (const Vec2d &shift : {Vec2d(0., 0.), Vec2d(0.37, 0.21)}) {
        CAPTURE(shift.x(), shift.y());
        const Points tips = tips_of(cube_interface(shift), 1.68);
        CHECK(std::find(tips.begin(), tips.end(), Point(0, 0)) != tips.end());
    }
}

TEST_CASE("Organic tips under an area interface stand at its corners reaching into a hole", "[TreeSupport][Interface][Regression]")
{
    using namespace roof_tips_test;
    // A 10 mm square interface around a 6 mm hole, into which a 2 mm tooth reaches 1.5 mm. The ends of the tooth are
    // convex corners of the interface, though the hole turns the other way there.
    const Polygon outer({Point::new_scale(-5., -5.), Point::new_scale(5., -5.), Point::new_scale(5., 5.), Point::new_scale(-5., 5.)});
    const Polygon hole({Point::new_scale(-3., -3.), Point::new_scale(3., -3.), Point::new_scale(3., 3.), Point::new_scale(-3., 3.)});
    const Polygon tooth({Point::new_scale(-1., 1.5), Point::new_scale(1., 1.5), Point::new_scale(1., 3.), Point::new_scale(-1., 3.)});
    const Polygons area = diff(Polygons{outer}, diff(Polygons{hole}, Polygons{tooth}));
    const Points   tips = tips_of(area, 1.68);
    for (const Vec2d &corner : {Vec2d(-1., 1.5), Vec2d(1., 1.5)}) {
        CAPTURE(corner.x(), corner.y());
        CHECK(nearest(tips, corner) <= roof_line / 2. * std::sqrt(2.) + 0.001);
    }
}

TEST_CASE("Organic tips under an area interface narrower than their least distance stand on its middle line", "[TreeSupport][Interface][Regression]")
{
    using namespace roof_tips_test;
    // A strip 1.1 mm wide and 5 mm long, turned by 30 degrees. Inset by half a line it is 0.88 mm wide, less than the
    // least distance between tips, so only one row of them fits across it.
    const Vec2d   along(std::cos(PI / 6.), std::sin(PI / 6.));
    const Vec2d   across(-along.y(), along.x());
    const auto    at = [&](double u, double v) { const Vec2d p = u * along + v * across; return Point::new_scale(p.x(), p.y()); };
    const Polygon strip({at(-2.5, -0.55), at(2.5, -0.55), at(2.5, 0.55), at(-2.5, 0.55)});
    const Points  tips = tips_of({strip}, 1.68);
    REQUIRE(tips.size() >= 2);
    for (const Point &tip : tips) {
        CAPTURE(unscaled(tip).x(), unscaled(tip).y());
        CHECK(std::abs(unscaled(tip).dot(across)) <= 0.001);
    }
    // The row reaches both ends of the strip inset by half a line, and its tips stay apart.
    for (const double end : {-2.5 + roof_line / 2., 2.5 - roof_line / 2.}) {
        CAPTURE(end);
        CHECK(nearest(tips, end * along) <= 0.001);
    }
    for (size_t i = 0; i < tips.size(); ++ i)
        for (size_t j = i + 1; j < tips.size(); ++ j)
            CHECK((unscaled(tips[i]) - unscaled(tips[j])).norm() >= min_distance - 0.001);
}

TEST_CASE("Organic tips along an overhang line stand no farther apart than the connect length", "[TreeSupport][Tips][Regression]")
{
    using namespace Slic3r;
    // A line 11.5 mm long, as along the edge of a box standing on its edge, or the outline of the sliver a layer of its
    // side overhangs by, 0.01 mm wide and jogging at its ends, sampled 1.313 mm apart, the connect length of tips 0.8 mm
    // across under 0.22 mm lines at a Branch Density of 30 %.
    const double length = 11.5, distance = 1.313;
    const bool   sliver = GENERATE(false, true);
    CAPTURE(sliver);
    const Polyline line = sliver ?
        Polyline(Points{ Point::new_scale(0., 0.01), Point::new_scale(0.06, 0.), Point::new_scale(length - 0.05, 0.),
                         Point::new_scale(length, 0.01), Point::new_scale(0., 0.01) }) :
        Polyline(Points{ Point::new_scale(0., 0.), Point::new_scale(length, 0.) });
    std::vector<double> at;
    for (const Polyline &tips : TreeSupport3D::ensure_maximum_distance_polyline({ line }, scaled<double>(distance), 1))
        for (const Point &tip : tips.points)
            at.push_back(unscaled(tip).x());
    std::sort(at.begin(), at.end());
    REQUIRE(at.size() >= 2);
    // Both ends get a tip, and no gap between two tips is longer than the distance, the last one included.
    CHECK_THAT(at.front(), Catch::Matchers::WithinAbs(0., 0.001));
    CHECK_THAT(at.back(), Catch::Matchers::WithinAbs(length, 0.001));
    for (size_t i = 1; i < at.size(); ++ i) {
        CAPTURE(at[i - 1], at[i]);
        CHECK(at[i] - at[i - 1] <= distance + 0.001);
    }
}

TEST_CASE("Organic tip sampling retains every line at the minimum step", "[TreeSupport][Tips][Regression]")
{
    using namespace Slic3r;
    const double distance = GENERATE(.055, .1, .12);
    const double length = GENERATE(.109, .15, .2, .3, 1., 10.05, 20.03);
    const size_t minimum = GENERATE(size_t(1), size_t(2));
    CAPTURE(distance, length, minimum);
    const Polylines lines{
        Polyline({Point::new_scale(0., 0.), Point::new_scale(length, 0.)}),
        Polyline({Point::new_scale(0., 2.), Point::new_scale(1., 3.)})};
    const auto sampled = TreeSupport3D::ensure_maximum_distance_polyline(lines, scaled<double>(distance), minimum);
    REQUIRE(sampled.size() == lines.size());
    for (size_t i = 0; i < sampled.size(); ++ i) {
        CAPTURE(i);
        REQUIRE(sampled[i].size() >= minimum);
        for (const Point &tip : sampled[i].points)
            CHECK((tip - tip.projection_onto(lines[i])).cast<double>().norm() <= SCALED_EPSILON);
    }
}

TEST_CASE("Organic roof pruning preserves printable tails outside continuing branches", "[TreeSupport][Interface][Regression]")
{
    using namespace Slic3r;
    const double overlap = GENERATE(-.1, 0., .001, .05, .2, 2., 5., 6.);
    const double width = GENERATE(.2, .5, 1.);
    CAPTURE(overlap, width);
    const auto rectangle = [](double x0, double y0, double x1, double y1) {
        return Polygons{Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0),
            Point::new_scale(x1, y1), Point::new_scale(x0, y1)})};
    };
    SupportGeneratorLayer lower, upper, roof;
    lower.print_z = roof.print_z = 1.;
    lower.bottom_z = roof.bottom_z = .9;
    upper.print_z = 1.1;
    upper.bottom_z = 1.;
    lower.height = upper.height = roof.height = .1;
    lower.polygons = upper.polygons = rectangle(-1., -1., overlap, 1.);
    roof.layer_type = SupporLayerType::TopContact;
    roof.polygons = rectangle(0., -width / 2., 5., width / 2.);
    const Polygons original = roof.polygons;
    SupportGeneratorLayersPtr tops{&roof}, interfaces, base_interfaces;
    TreeSupport3D::organic_remove_roofs_inside_branches({}, tops, interfaces, base_interfaces, {&lower, &upper}, scale_(.82));
    if (overlap < 5.) {
        REQUIRE(tops.size() == 1);
        CHECK(diff(original, roof.polygons).empty());
        CHECK(diff(roof.polygons, original).empty());
    } else {
        CHECK(tops.empty());
        CHECK(roof.polygons.empty());
    }
}
