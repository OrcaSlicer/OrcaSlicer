#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <math.h>
#include <optional>
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
    // Vertical bodies widen the same whether the growth is limited or not, as on a belt.
    const bool    limited    = GENERATE(true, false);
    CAPTURE(reach, limited);
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
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, limited, line_width, tip_radius, scaled<coord_t>(move),
        scaled<coord_t>(reach));
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

TEST_CASE("Organic bodies widen under a part of a roof printed in loops too narrow for a loop", "[TreeSupport][Material][Regression]")
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
    // Lines 0.42 mm wide, layers 0.12 mm and a branch angle of 40 degrees. A tip 0.8 mm across stands under a strip of
    // roof printed in loops, 0.4 mm wide and reaching 1.1 mm beyond the tip: a little narrower than a line, the strip
    // gets no loop, see organic_roof_loops(), though the fill, taking its outermost loop in by half its spacing, may
    // print one there. Its end lying farther than reach from the tip, the tip widens toward it until every point of it
    // lies within reach.
    const coord_t line_width = scaled<coord_t>(0.42), tip_radius = scaled<coord_t>(0.4);
    const double  move       = std::tan(40. * M_PI / 180.) * 0.12;
    const double  reach      = 0.63;
    const size_t  num_layers = 20, roof_layer = num_layers - 1;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer] = { rectangle(0., -0.2, 1.5, 0.4) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 1. + 0.12 * double(i);
        layer.print_z  = layer.bottom_z + 0.12;
        layer.height   = 0.12;
        layer.polygons = { disc(0., 0.4) };
        bodies[i]      = &layer;
    }
    REQUIRE_FALSE(diff(roofs[roof_layer], offset(storage.back().polygons, scaled<float>(reach + 0.005))).empty());
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, line_width, tip_radius, scaled<coord_t>(move),
        scaled<coord_t>(reach));
    CHECK(diff(roofs[roof_layer], offset(storage.back().polygons, scaled<float>(reach + 0.005))).empty());
}

TEST_CASE("Organic bodies widen under a roof printed in loops but where a base interface under it prints lines", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto rectangle = [](double x, double y, double width, double depth) {
        return Polygon({Point::new_scale(x, y), Point::new_scale(x + width, y), Point::new_scale(x + width, y + depth),
                        Point::new_scale(x, y + depth)});
    };
    // Lines 0.22 mm wide, tips 0.8 mm across, layers 0.02 mm and a branch angle of 40 degrees. Two trunks 10 mm long,
    // 1 mm and 2 mm wide, 0.27 mm apart under a roof over both printed in loops, a quarter of a line apart from half a
    // line in, some of which run along the gap between them, over neither. Where a base interface prints the lowest
    // layer of the roof, it prints lines across the gap, which bridge it, or along it, which do not; where the model
    // cut the stack of the roof short before its base interface, the lowest layer prints loops, and the bodies widen
    // into the gap under them, but under a part of the roof a base interface prints.
    enum { Base, Loops, Along, Split };
    const int mode = GENERATE(Base, Loops, Along, Split);
    CAPTURE(mode);
    const coord_t  line_width = scaled<coord_t>(0.22), tip_radius = scaled<coord_t>(0.4);
    const double   move       = std::tan(40. * M_PI / 180.) * 0.02;
    const double   reach      = 0.235;
    const double   gap        = 0.27;
    const Polygons trunks{ rectangle(0., 0., 10., 1.), rectangle(0., 1. + gap, 10., 2.) };
    const size_t   num_layers = 20, roof_layer = num_layers - 1;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers), base_roofs(num_layers);
    roofs[roof_layer] = { rectangle(0., 0., 10., 3. + gap) };
    if (mode == Base || mode == Along)
        base_roofs[roof_layer] = roofs[roof_layer];
    else if (mode == Split)
        base_roofs[roof_layer] = { rectangle(0., 0., 5., 3. + gap) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 1. + 0.02 * double(i);
        layer.print_z  = layer.bottom_z + 0.02;
        layer.height   = 0.02;
        layer.polygons = trunks;
        bodies[i]      = &layer;
    }
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, line_width, tip_radius, scaled<coord_t>(move),
        scaled<coord_t>(reach), {}, {}, base_roofs, { mode == Along ? 0. : 0.5 * M_PI });
    // How much of the gap the bodies widen into by bands printed solid, along its middle or that of either half of
    // it, where loops run along it: most of the way across, as under lines along it.
    const auto widened = [&](const double x, const double length) {
        const Polygons gap_area{ rectangle(x, 1., length, gap) };
        return area(intersection(gap_area, storage.back().solid_polygons)) / area(gap_area);
    };
    const double middle = widened(3., 4.), left = widened(1.5, 2.), right = widened(6.5, 2.);
    CAPTURE(middle, left, right);
    if (mode == Split) {
        CHECK(left < 0.5);
        CHECK(right > 0.5);
    } else if (mode == Base)
        CHECK(middle < 0.5);
    else
        CHECK(middle > 0.5);
}

TEST_CASE("Organic bodies leaning under a roof widen beyond the layer below by no more than a wall overhangs it", "[TreeSupport][Material][Regression]")
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
    // Lines 0.82 mm wide, tips 1.5 mm across and a branch angle of 40 degrees: at layers 0.4 mm a move of 0.34 mm a
    // layer, and a wall overhangs the one below it by half a line, 0.41 mm, at most; at layers 0.6 mm by the move, 0.5
    // mm, as it is set to. Three tips 6 mm apart, leaning going up by the move a layer, under a strip of roof whose
    // middle between them lies farther than reach from them, with lines along the strip or none known.
    const double  height     = GENERATE(0.4, 0.6);
    const bool    lines      = GENERATE(false, true);
    CAPTURE(height, lines);
    const double  line_width = 0.82, reach = 1.64;
    const double  move       = std::tan(40. * M_PI / 180.) * height;
    const double  overhang   = std::max(0.5 * line_width, move);
    const size_t  num_layers = 13, roof_layer = num_layers - 1;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers), original(roof_layer);
    roofs[roof_layer] = { rectangle(- 1. + move * double(roof_layer), -0.75, 14., 1.5) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = height * double(i);
        layer.print_z  = layer.bottom_z + height;
        layer.height   = height;
        for (const double x : { 0., 6., 12. })
            original[i].push_back(disc(x + move * double(i), 0.75));
        layer.polygons = original[i];
        bodies[i]      = &layer;
    }
    const std::vector<double> roof_angles = lines ? std::vector<double>{ 0. } : std::vector<double>{};
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, scaled<coord_t>(line_width),
        scaled<coord_t>(0.75), scaled<coord_t>(move), scaled<coord_t>(reach), roof_angles);
    // The bodies widen under the roof, with no lines known until none of its loops hangs, which brings every point of
    // it within reach of them here: the side they lean to gains less a layer, and they widen by a step more,
    CHECK_FALSE(diff(storage.back().polygons, original.back()).empty());
    if (! lines)
        CHECK(diff(roofs[roof_layer], offset(storage.back().polygons, scaled<float>(reach + 0.005))).empty());
    // what they add on a layer lying within that overhang of the layer below, where the walls they print stand: the
    // side the tips lean to moves by the lean already, and the move more would take a wall there farther over nothing.
    for (size_t i = 1; i < storage.size(); ++ i) {
        CAPTURE(i);
        const Polygons added = diff(storage[i].polygons, original[i]);
        CHECK(diff(added, offset(storage[i - 1].polygons, scaled<float>(overhang + 0.01))).empty());
    }
}

TEST_CASE("Organic bodies leaning under a roof take no step more that leaves as much of it hanging", "[TreeSupport][Material][Regression]")
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
    // Lines 0.82 mm wide, layers 0.6 mm and a branch angle of 40 degrees: a move of 0.5 mm a layer, more than half a
    // line, so that a step is half a line and a wall overhangs the one below it by the move. A tip 1.5 mm across leans
    // going up by the move a layer toward a strip of roof beyond it, which two steps would hold. The side the tip leans
    // to gains nothing a layer, and no step more holds more of the roof. The strip prints lines along it, sampled a
    // quarter of a line apart, each touching what lies within half a line less half that pitch of it, and a line ending
    // in the air hangs no farther than a line beyond that.
    const double line_width = 0.82, reach = 1.64, step = 0.41;
    const double touch      = 0.5 * (line_width - 0.25 * line_width);
    const double hold       = touch + std::min(line_width, reach - touch);
    const double move       = std::tan(40. * M_PI / 180.) * 0.6;
    const size_t num_layers = 8, roof_layer = num_layers - 1;
    const double top        = move * double(roof_layer - 1);
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers), original(roof_layer);
    roofs[roof_layer] = { rectangle(top, -0.4, 0.75 + 2. * step + hold - 0.1, 0.8) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.6 * double(i);
        layer.print_z  = layer.bottom_z + 0.6;
        layer.height   = 0.6;
        original[i]    = { disc(move * double(i), 0.75) };
        layer.polygons = original[i];
        bodies[i]      = &layer;
    }
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, scaled<coord_t>(line_width),
        scaled<coord_t>(0.75), scaled<coord_t>(move), scaled<coord_t>(reach), { 0. });
    // The tip flares over the two layers the steps take, no lower.
    CHECK_FALSE(diff(storage[roof_layer - 1].polygons, original[roof_layer - 1]).empty());
    CHECK(diff(storage[roof_layer - 3].polygons, original[roof_layer - 3]).empty());
}

TEST_CASE("Organic bodies leaning under a roof go on past a step that holds no more of it", "[TreeSupport][Material][Regression]")
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
    // Lines 0.82 mm wide, layers 0.4 mm and a move of 0.5 mm a layer, more than half a line: a step is half a line, and
    // a wall overhangs the one below it by the move. A tip 1.5 mm across stands straight, then leans by the move over
    // its two top layers toward a strip of roof beyond it, which a step brings within reach. The side it leans to gains
    // nothing on those layers: a step more, down to the first of them, holds no more of the roof, and one more, down to
    // where the tip stands straight, carries a step up. The strip prints lines along it.
    const double line_width = 0.82, reach = 1.64, step = 0.41, move = 0.5034;
    const size_t num_layers = 9, roof_layer = num_layers - 1, straight = roof_layer - 3;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    const double top = 2. * move;
    roofs[roof_layer] = { rectangle(top, -0.4, 0.75 + step + reach - 0.1, 0.8) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.4 * double(i);
        layer.print_z  = layer.bottom_z + 0.4;
        layer.height   = 0.4;
        layer.polygons = { disc(move * double(i > straight ? i - straight : 0), 0.75) };
        bodies[i]      = &layer;
    }
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, scaled<coord_t>(line_width), scaled<coord_t>(0.75),
        scaled<coord_t>(move), scaled<coord_t>(reach), { 0. });
    CHECK(diff(roofs[roof_layer], offset(storage.back().polygons, scaled<float>(reach + 0.005))).empty());
}

TEST_CASE("Organic bodies over a support blocker widen under a roof as standing on nothing", "[TreeSupport][Material][Regression]")
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
    // Lines 0.42 mm wide, layers 0.2 mm and a branch angle of 40 degrees, a move of 0.17 mm a layer. A tip 1.5 mm
    // across three layers tall under a strip of roof reaching 3 mm to either side stands over a support blocker 0.6 mm
    // across, which the collision of the layer below holds but which is no material to stand on; a trunk far away
    // stands on every layer. The support may rest on the model, of which there is none under the tip.
    const double  line_width = 0.42, reach = 0.63;
    const double  move       = std::tan(40. * M_PI / 180.) * 0.2;
    const size_t  num_layers = 11, roof_layer = num_layers - 1, first_tip = roof_layer - 3;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer]    = { rectangle(-3., -0.75, 6., 1.5) };
    model[first_tip - 1] = { rectangle(-0.3, -0.3, 0.6, 0.6) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.2 * double(i);
        layer.print_z  = layer.bottom_z + 0.2;
        layer.height   = 0.2;
        layer.polygons = { disc(20., 2.) };
        if (i >= first_tip)
            layer.polygons.push_back(disc(0., 0.75));
        bodies[i] = &layer;
    }
    const std::function<Polygons(size_t, const BoundingBox &)> model_under = [](size_t, const BoundingBox &) {
        return Polygons();
    };
    organic_widen_bodies_under_roofs(bodies, roofs, model, model_under, true, scaled<coord_t>(line_width),
        scaled<coord_t>(0.75), scaled<coord_t>(move), scaled<coord_t>(reach));
    // The tip flares from its lowest layer on, five steps there and seven at the top, all around.
    for (const double side : { -1., 1. }) {
        CAPTURE(side);
        CHECK(contains(storage[first_tip].polygons, Point::new_scale(side * (0.75 + 4.5 * move), 0.)));
        CHECK(contains(storage.back().polygons, Point::new_scale(side * (0.75 + 6.5 * move), 0.)));
    }
}

TEST_CASE("Organic bodies widen under a roof over the model below only where the support may rest on it", "[TreeSupport][Material][Regression]")
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
    // Lines 0.42 mm wide, layers 0.2 mm and a branch angle of 40 degrees, a move of 0.17 mm a layer. A tip 1.5 mm
    // across three layers tall under a strip of roof reaching 3 mm to either side, farther than the flare of seven
    // steps of the move, 1.18 mm, and reach. It stands with its left half on a branch 0.8 mm across, and its right half
    // over the top of the model, which keeps the flare of the branch on its left.
    const bool    on_model   = GENERATE(false, true);
    CAPTURE(on_model);
    const double  line_width = 0.42, reach = 0.63;
    const double  move       = std::tan(40. * M_PI / 180.) * 0.2;
    const size_t  num_layers = 11, roof_layer = num_layers - 1, first_tip = roof_layer - 3;
    std::vector<SupportGeneratorLayer> storage(roof_layer);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer]    = { rectangle(-3., -0.75, 6., 1.5) };
    model[first_tip - 1] = { rectangle(0.3, -5., 5., 10.) };
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = 0.2 * double(i);
        layer.print_z  = layer.bottom_z + 0.2;
        layer.height   = 0.2;
        layer.polygons = { i < first_tip ? disc(-0.5, 0.4) : disc(0., 0.75) };
        bodies[i]      = &layer;
    }
    // The model under the tip is the model of the collision there, the support stands on it where it may rest on it.
    std::function<Polygons(size_t, const BoundingBox &)> model_under;
    if (on_model)
        model_under = [&model](size_t layer, const BoundingBox &) { return model[layer]; };
    organic_widen_bodies_under_roofs(bodies, roofs, model, model_under, true, scaled<coord_t>(line_width),
        scaled<coord_t>(0.75), scaled<coord_t>(move), scaled<coord_t>(reach));
    // The tip flares from its lowest layer on, five steps there and seven at the top, over the branch, and over the
    // model where the support may rest on it.
    CHECK(contains(storage[first_tip].polygons, Point::new_scale(-0.75 - 4.5 * move, 0.)));
    CHECK(contains(storage[first_tip].polygons, Point::new_scale(0.75 + 4.5 * move, 0.)) == on_model);
    CHECK(contains(storage.back().polygons, Point::new_scale(-0.75 - 6.5 * move, 0.)));
    CHECK(contains(storage.back().polygons, Point::new_scale(0.75 + 6.5 * move, 0.)) == on_model);
}

TEST_CASE("Organic bodies widen under a roof over the model the same when it is asked for near them only", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    auto rectangle = [](double x, double y, double width, double depth) {
        return Polygon({Point::new_scale(x, y), Point::new_scale(x + width, y), Point::new_scale(x + width, y + depth),
                        Point::new_scale(x, y + depth)});
    };
    // Lines 0.42 mm wide, layers 0.2 mm and a branch angle of 40 degrees, a move of 0.17 mm a layer. A tip three layers
    // tall, a wedge 1.5 mm across narrowing to a corner of 40 degrees, under a strip of roof reaching far past it,
    // stands on the top of the model, with a branch 0.8 mm across beside its wide end. Its flare widens with mitered
    // corners, which at that angle reach almost three times as far from the corner as its steps.
    const double  line_width = 0.42, reach = 0.63;
    const double  move       = std::tan(40. * PI / 180.) * 0.2;
    const size_t  num_layers = 11, roof_layer = num_layers - 1, first_tip = roof_layer - 3;
    const double  length     = 0.75 / std::tan(20. * PI / 180.);
    const Polygon wedge({Point::new_scale(0., -0.75), Point::new_scale(length, 0.), Point::new_scale(0., 0.75)});
    Polygon       branch = make_circle_num_segments(scale_(0.4), 64);
    branch.translate(Point::new_scale(-0.5, 0.));
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    roofs[roof_layer]    = { rectangle(-3., -2., 12., 4.) };
    model[first_tip - 1] = { rectangle(0.3, -5., 10., 10.) };
    // The bodies widened over all the model, and over the model within the box it is asked for near them.
    std::vector<Polygons> widened[2];
    for (const bool near_only : {false, true}) {
        std::vector<SupportGeneratorLayer> storage(roof_layer);
        SupportGeneratorLayersPtr          bodies(num_layers, nullptr);
        for (size_t i = 0; i < storage.size(); ++ i) {
            SupportGeneratorLayer &layer = storage[i];
            layer.bottom_z = 0.2 * double(i);
            layer.print_z  = layer.bottom_z + 0.2;
            layer.height   = 0.2;
            layer.polygons = { i < first_tip ? branch : wedge };
            bodies[i]      = &layer;
        }
        const std::function<Polygons(size_t, const BoundingBox &)> model_under =
            [&model, near_only](size_t layer, const BoundingBox &box) {
                return near_only ? intersection(model[layer], Polygons{ box.polygon() }) : model[layer];
            };
        organic_widen_bodies_under_roofs(bodies, roofs, model, model_under, true, scaled<coord_t>(line_width),
            scaled<coord_t>(0.75), scaled<coord_t>(move), scaled<coord_t>(reach));
        for (const SupportGeneratorLayer &layer : storage)
            widened[near_only ? 1 : 0].push_back(layer.polygons);
    }
    // The tip flares from its lowest layer on, five steps there, its corner reaching out more than twice as far as the
    // steps and half a line, and the same with the model asked for near it only.
    CHECK(contains(widened[0][first_tip], Point::new_scale(length + 10. * move + 0.5 * line_width, 0.)));
    for (size_t i = 0; i < roof_layer; ++ i) {
        CAPTURE(i);
        CHECK(diff(widened[0][i], offset(widened[1][i], scaled<float>(0.001))).empty());
        CHECK(diff(widened[1][i], offset(widened[0][i], scaled<float>(0.001))).empty());
    }
}

TEST_CASE("Organic islands clipped to a box widen within it as they do whole", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // A concave island reaching into a box 9 mm by 4 mm, whose clip to the box keeps far corners that join outside it
    // the other way round, widened by an XY distance of 0.35 mm.
    const ExPolygon island(Polygon({Point(5404922, 10355228), Point(5022691, 11330444), Point(1559138, 6809131),
        Point(-4696227, 9742406), Point(-6622603, 10931625), Point(-3695113, -12415881)}));
    REQUIRE(island.contour.is_counter_clockwise());
    const BoundingBox box(Point(-2640000, 10370000), Point(6359999, 14370000));
    const coord_t     delta = scaled<coord_t>(0.35);
    const Polygons    part  = ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, box);
    REQUIRE_FALSE(part.empty());
    // Within the box shrunk by the distance, the part widened is the island widened, though widened as it is clipped
    // it covers less.
    const Polygons inner = { box.inflated(- delta).polygon() };
    const Polygons whole =
        intersection(offset(to_polygons(island), float(delta), jtRound, scaled<double>(0.005)), inner);
    const std::optional<Polygons> widened =
        organic_widen_part(island, get_extents(island.contour), part, box, float(delta));
    REQUIRE(widened);
    const Polygons near = intersection(*widened, inner);
    REQUIRE_FALSE(whole.empty());
    REQUIRE_FALSE(
        diff(whole, offset(intersection(offset(part, float(delta), jtRound, scaled<double>(0.005)), inner),
            scaled<float>(0.001))).empty());
    CHECK(diff(whole, offset(near, scaled<float>(0.001))).empty());
    CHECK(diff(near, offset(whole, scaled<float>(0.001))).empty());
}

TEST_CASE("Organic island tips a box cuts short widen within it as they do whole", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Islands over a box 30 mm square, widened by an XY distance of 10 mm, whose widening drops a point closer than
    // 0.05 mm to the last one it keeps, with a sharp tip into the box less than 0.1 mm across its side. Cut to the box
    // as Clipper2 starts the cut, the widening drops all of the tips of the first two, leaves three points of the third
    // winding the other way round, the fourth twisted and winding the other way round, the next three twisted, though
    // winding as they do, the seventh next to a point of it within the box, and the last two, needles 20.6 mm long and
    // 0.01 and 0.2 mm across where they leave the island, all of them: the premise below checks that the tip cut
    // widened covers less. Cut by a box a little larger, the widening drops only one of the two points of the second
    // within the box, as it does of the island, and keeps the tips of the fourth, the sixth and the seventh as they
    // are; cut by a box as much larger as the widening, it keeps the wider needle as it is, where it is 0.1 mm across.
    // The first, the third and the fifth, all within that box, and the thinner needle are widened whole.
    const std::pair<ExPolygon, bool> tip = GENERATE(
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-0.02, 1.), Point::new_scale(0., -0.03), Point::new_scale(0.02, 1.),
            Point::new_scale(10., 1.)})), false),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-0.26, 1.), Point::new_scale(-0.015, -0.065), Point::new_scale(0., -0.02),
            Point::new_scale(2.55, 1.), Point::new_scale(10., 1.)})), true),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-0.02, 1.), Point::new_scale(0., -0.6), Point::new_scale(-0.002, -0.3),
            Point::new_scale(0.02, 1.), Point::new_scale(10., 1.)})), false),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-0.0427, 1.), Point::new_scale(-0.0177, -0.4431), Point::new_scale(-0.0075, -0.5305),
            Point::new_scale(-0.0176, -0.302), Point::new_scale(0.0332, 1.), Point::new_scale(10., 1.)})), true),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-0.02494, 1.), Point::new_scale(-0.00617, -0.32771), Point::new_scale(-0.00593, -0.27694),
            Point::new_scale(0.00841, -0.10896), Point::new_scale(0.04236, 1.), Point::new_scale(10., 1.)})), false),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-0.05999, 1.), Point::new_scale(-0.00054, -0.44774), Point::new_scale(-0.0013, -0.32835),
            Point::new_scale(0.01582, -0.25691), Point::new_scale(0.05683, 1.), Point::new_scale(10., 1.)})), true),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(0., 1.), Point::new_scale(-0.05, -0.04), Point::new_scale(-0.02, -0.15),
            Point::new_scale(-0.042, -0.06), Point::new_scale(1., 1.), Point::new_scale(10., 1.)})), true),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 25.), Point::new_scale(-10., 25.),
            Point::new_scale(-10., 20.), Point::new_scale(-0.005, 20.), Point::new_scale(0., -0.6),
            Point::new_scale(0.005, 20.), Point::new_scale(10., 20.)})), false),
        std::pair(ExPolygon(Polygon({Point::new_scale(10., 25.), Point::new_scale(-10., 25.),
            Point::new_scale(-10., 20.), Point::new_scale(-0.1, 20.), Point::new_scale(0., -0.6),
            Point::new_scale(0.1, 20.), Point::new_scale(10., 20.)})), true));
    const ExPolygon &island = tip.first;
    REQUIRE(island.contour.is_counter_clockwise());
    const BoundingBox box(Point::new_scale(-15., -30.), Point::new_scale(15., 0.));
    const coord_t     delta = scaled<coord_t>(10.);
    const Polygons    part  = ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, box);
    REQUIRE_FALSE(part.empty());
    // Within the box shrunk by the distance, the part widened, or the island widened whole where it is to be, is the
    // island widened, but for the points the widening drops, though the tip cut to the box widened covers less.
    const Polygons                inner   = { box.inflated(- delta).polygon() };
    const Polygons                whole   = offset(island, float(delta), jtRound, scaled<double>(0.005));
    const Polygons                cut     =
        offset(intersection(part, Polygons{ box.polygon() }), float(delta), jtRound, scaled<double>(0.005));
    const std::optional<Polygons> widened =
        organic_widen_part(island, get_extents(island.contour), part, box, float(delta));
    const Polygons               &near    = widened ? *widened : whole;
    REQUIRE_FALSE(diff(intersection(whole, inner), offset(cut, scaled<float>(0.005))).empty());
    CHECK(widened.has_value() == tip.second);
    CHECK(diff(intersection(whole, inner), offset(near, scaled<float>(0.1))).empty());
    CHECK(diff(intersection(near, inner), offset(whole, scaled<float>(0.1))).empty());
}

TEST_CASE("Organic island slivers widening whole drops keep the cut a box as much larger as the widening keeps", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // A sliver of an island 30 mm long over a box 40 mm wide, widened by an XY distance of 1 mm, whose widening drops a
    // point closer than 0.005 mm to the last one it keeps: its tip reaches 0.05 mm into the box, and its far side is
    // 0.004 mm long, so the sliver widened whole is dropped all of it. Cut by the box, or by a box a little larger, the
    // widening drops a point where the cut crosses it; cut by a box as much larger as the widening, the cut crosses it
    // 0.03 mm apart and is widened as it is, its points reported.
    const ExPolygon island(
        Polygon({Point::new_scale(0., -0.05), Point::new_scale(30., 2.), Point::new_scale(30., 2.004)}));
    REQUIRE(island.contour.is_counter_clockwise());
    const BoundingBox box(Point::new_scale(-5., -5.), Point::new_scale(35., 0.));
    const coord_t     delta = scaled<coord_t>(1.);
    REQUIRE(offset(island, float(delta), jtRound, scaled<double>(0.005)).empty());
    const Polygons    part = ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, box);
    const BoundingBox far  = box.inflated(delta);
    const Polygons    cut  = intersection(ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, far),
        Polygons{ far.polygon() });
    size_t                        points  = 0;
    const std::optional<Polygons> widened =
        organic_widen_part(island, get_extents(island.contour), part, box, float(delta), &points);
    REQUIRE(widened);
    CHECK(points == count_points(cut));
    CHECK_FALSE(intersection(*widened, Polygons{ box.inflated(- delta).polygon() }).empty());
}

TEST_CASE("Organic island tips a box a little larger cuts report no points of a cut reaching farther out", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // An island over a box 30 mm square, widened by an XY distance of 10 mm, whose widening drops a point closer than
    // 0.05 mm to the last one it keeps, with a sharp tip into the box: cut by the box, the widening drops both of its
    // points within it; cut by a box a little larger, it drops only one of them, as it does of the island, and no
    // points of a cut by the box as much larger as the widening are reported.
    const ExPolygon island(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
        Point::new_scale(-0.26, 1.), Point::new_scale(-0.015, -0.065), Point::new_scale(0., -0.02),
        Point::new_scale(2.55, 1.), Point::new_scale(10., 1.)}));
    REQUIRE(island.contour.is_counter_clockwise());
    const BoundingBox box(Point::new_scale(-15., -30.), Point::new_scale(15., 0.));
    const coord_t     delta = scaled<coord_t>(10.);
    const Polygons    part  = ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, box);
    REQUIRE_FALSE(organic_widens_cut_as_it_is(intersection(part, Polygons{ box.polygon() }), float(delta)));
    // The box a little larger, by four times the least edge the widening keeps along x and eight times along y.
    const auto        step = coord_t(std::ceil(4. * double(delta) * ClipperOffsetShortestEdgeFactor));
    const BoundingBox larger(box.min - Point(step, 2 * step), box.max + Point(step, 2 * step));
    const Polygons    cut  = intersection(ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, larger),
        Polygons{ larger.polygon() });
    REQUIRE(organic_widens_cut_as_it_is(cut, float(delta)));
    size_t                        points  = 0;
    const std::optional<Polygons> widened =
        organic_widen_part(island, get_extents(island.contour), part, box, float(delta), &points);
    REQUIRE(widened);
    CHECK(points == 0);
    const Polygons inner = { box.inflated(- delta).polygon() };
    const Polygons whole = offset(island, float(delta), jtRound, scaled<double>(0.005));
    CHECK(diff(intersection(whole, inner), offset(*widened, scaled<float>(0.1))).empty());
    CHECK(diff(intersection(*widened, inner), offset(whole, scaled<float>(0.1))).empty());
}

TEST_CASE("Organic islands a side of a box crosses next to a point are cut a little farther out", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // An island over a box 30 mm square, widened by an XY distance of 10 mm, whose widening drops a point closer than
    // 0.05 mm to the last one it keeps, with a blunt point 0.03 mm into the box: cut by the box, the widening drops all
    // of it.
    const ExPolygon island(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
        Point::new_scale(-1., 1.), Point::new_scale(0., -0.03), Point::new_scale(0.5, 1.), Point::new_scale(10., 1.)}));
    REQUIRE(island.contour.is_counter_clockwise());
    const BoundingBox box(Point::new_scale(-15., -30.), Point::new_scale(15., 0.));
    const coord_t     delta = scaled<coord_t>(10.);
    const Polygons    part  = ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, box);
    const Polygons    inner = { box.inflated(- delta).polygon() };
    const Polygons    whole = intersection(offset(island, float(delta), jtRound, scaled<double>(0.005)), inner);
    REQUIRE_FALSE(whole.empty());
    REQUIRE(intersection(offset(intersection(part, Polygons{ box.polygon() }), float(delta), jtRound,
        scaled<double>(0.005)), inner).empty());
    // Not widened whole, the island cut a little farther out widened is the island widened within the box shrunk by
    // the distance.
    const std::optional<Polygons> widened =
        organic_widen_part(island, get_extents(island.contour), part, box, float(delta));
    REQUIRE(widened);
    const Polygons near = intersection(*widened, inner);
    CHECK(diff(whole, offset(near, scaled<float>(0.001))).empty());
    CHECK(diff(near, offset(whole, scaled<float>(0.001))).empty());
}

TEST_CASE("Organic islands with points closer than the widening keeps within a box widen the part of them in it", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Islands over a box 30 mm square, widened by an XY distance of 10 mm, whose widening drops a point closer than
    // 0.05 mm to the last one it keeps, reaching 5 mm into the box with two points 0.01 mm apart there, as merging the
    // slices of regions leaves at a sharp corner, one of them on its contour and one on a hole in the box: the widening
    // drops one of them, as it does of the island.
    const ExPolygon island = GENERATE(
        ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
            Point::new_scale(-3., 1.), Point::new_scale(-2., -5.), Point::new_scale(2., -5.),
            Point::new_scale(2.01, -4.995), Point::new_scale(3., 1.), Point::new_scale(10., 1.)})),
        ExPolygon(Polygon({Point::new_scale(10., 5.), Point::new_scale(-10., 5.), Point::new_scale(-10., 1.),
                      Point::new_scale(-3., 1.), Point::new_scale(-2., -9.), Point::new_scale(2., -9.),
                      Point::new_scale(3., 1.), Point::new_scale(10., 1.)}),
            Polygon({Point::new_scale(-1., -7.), Point::new_scale(-1., -5.), Point::new_scale(1., -5.),
                Point::new_scale(1.01, -5.005)})));
    REQUIRE(island.contour.is_counter_clockwise());
    REQUIRE(std::all_of(island.holes.begin(), island.holes.end(), [](const Polygon &hole) {
        return hole.is_clockwise();
    }));
    const BoundingBox box(Point::new_scale(-15., -30.), Point::new_scale(15., 0.));
    const coord_t     delta = scaled<coord_t>(10.);
    const Polygons    part  = ClipperUtils::clip_clipper_polygons_with_subject_bbox(island, box);
    const Polygons    cut   = intersection(part, Polygons{ box.polygon() });
    REQUIRE(count_points(offset_input(cut, float(delta))) < count_points(cut));
    // Not widened whole, the part widened is the island widened within the box shrunk by the distance, but for the
    // points the widening drops; cut by the box itself, no points of a cut by a larger box are reported.
    size_t                        points  = 1;
    const std::optional<Polygons> widened =
        organic_widen_part(island, get_extents(island.contour), part, box, float(delta), &points);
    REQUIRE(widened);
    CHECK(points == 0);
    const Polygons inner = { box.inflated(- delta).polygon() };
    const Polygons whole = offset(island, float(delta), jtRound, scaled<double>(0.005));
    REQUIRE_FALSE(intersection(whole, inner).empty());
    CHECK(diff(intersection(whole, inner), offset(*widened, scaled<float>(0.1))).empty());
    CHECK(diff(intersection(*widened, inner), offset(whole, scaled<float>(0.1))).empty());
}

TEST_CASE("Organic cuts the widening twists where it joins past the points it drops are not widened as they are", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // A widening by 10000 units of the coordinates drops a point closer than 50 of them to the last one it keeps,
    // going around a contour from its first point. Past those it drops, it joins an edge that crosses or touches
    // another edge with an end between them and the edge it joins: one with its end there, one next to a point it
    // drops, one where the contour touches itself, and one with its start there. The last does not cross itself, and
    // the edge it joins crosses one ending between them; all 55 points it keeps lie as far along x as that edge, most
    // along a side with small teeth, and only three as far along y.
    const float   delta   = 10000.f;
    const Polygon twisted = GENERATE(
        Polygon({Point(100, 130), Point(40, 40), Point(100, 100), Point(50, 0), Point(120, 90)}),
        Polygon({Point(23, 75), Point(119, 42), Point(34, 83), Point(105, 54), Point(23, 98)}),
        Polygon({Point(100, 210), Point(50, 260), Point(160, 190), Point(180, 190), Point(50, 260), Point(0, 30)}),
        Polygon({Point(75, 71), Point(145, 68), Point(95, 65), Point(188, 66), Point(50, 112)}),
        [] {
            Polygon teeth({Point(0, 1000), Point(0, 951), Point(1000, 1010), Point(1000, 3000), Point(510, 3000),
                Point(500, 995), Point(490, 3000)});
            for (int k = 1; k < 50; ++ k)
                teeth.points.emplace_back(490 - 10 * k, k % 2 == 1 ? 3100 : 3000);
            return teeth;
        }());
    REQUIRE(twisted.area() > 0.);
    REQUIRE(count_points(offset_input(Polygons{ twisted }, delta)) < twisted.size());
    CHECK_FALSE(organic_widens_cut_as_it_is(Polygons{ twisted }, delta));
    // A square with a point next to a corner, which the widening drops, it keeps as it is otherwise.
    const Polygon square({Point(0, 0), Point(1000, 0), Point(1000, 1000), Point(0, 1000), Point(0, 980)});
    REQUIRE(count_points(offset_input(Polygons{ square }, delta)) < square.size());
    CHECK(organic_widens_cut_as_it_is(Polygons{ square }, delta));
}

TEST_CASE("Organic bodies under a roof widen as far in at most 128 steps at a tenth of a degree and not at all at a hundredth", "[TreeSupport][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Four round bodies 1.4 mm across, 6 mm apart, on 130 layers of 0.2 mm under a roof over all of them, bodies of
    // lines 0.42 mm wide that lean by a branch angle of 40 degrees, of 0.1 degrees, 0.35 um a layer, or of 0.01
    // degrees, 35 nm a layer.
    constexpr size_t num_layers = 130;
    const double     lean       = 0.2 * std::tan(0.1 * PI / 180.);
    Polygons         rounds;
    for (const double x : { 0., 6. })
        for (const double y : { 0., 6. }) {
            Polygon round;
            for (int k = 0; k < 64; ++ k)
                round.points.push_back(
                    Point::new_scale(x + 0.7 * std::cos(PI * k / 32.), y + 0.7 * std::sin(PI * k / 32.)));
            rounds.push_back(std::move(round));
        }
    const auto widened = [&rounds](const double angle, size_t &checks) {
        std::vector<SupportGeneratorLayer> storage(num_layers);
        SupportGeneratorLayersPtr          bodies(num_layers + 1, nullptr);
        for (size_t i = 0; i < num_layers; ++ i) {
            storage[i].bottom_z = 0.2 * double(i);
            storage[i].print_z  = 0.2 * double(i + 1);
            storage[i].polygons = rounds;
            bodies[i]           = &storage[i];
        }
        std::vector<Polygons> roofs(num_layers + 1), model(num_layers + 1);
        roofs.back() = { Polygon({Point::new_scale(-3., -3.), Point::new_scale(9., -3.), Point::new_scale(9., 9.),
            Point::new_scale(-3., 9.)}) };
        organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, scaled<coord_t>(0.42), scaled<coord_t>(0.4),
            scaled<coord_t>(0.2 * std::tan(angle * PI / 180.)), scaled<coord_t>(0.42), { 0. },
            [&checks] { ++ checks; });
        std::vector<Polygons> out;
        for (SupportGeneratorLayer &layer : storage)
            out.emplace_back(std::move(layer.polygons));
        return out;
    };
    size_t                      checks_wide = 0, checks_steep = 0;
    const std::vector<Polygons> wide        = widened(40., checks_wide);
    const std::vector<Polygons> steep       = widened(0.1, checks_steep);
    // The top bodies widen as far either way, but for the last step of the wide ones, of 0.17 mm, the steep ones in at
    // most 128 steps, the roof, each step and each layer a flare is tried on checked for cancelling; going down, the
    // steep ones are narrower by how far they lean, in stairs of 8 um: at most 3 um nine layers down, and all the way
    // down by 45 um less a stair at most.
    CHECK(diff(steep.back(), offset(wide.back(), scaled<float>(0.01))).empty());
    CHECK(diff(wide.back(), offset(steep.back(), scaled<float>(0.2 * std::tan(40. * PI / 180.) + 0.01))).empty());
    CHECK(diff(steep.back(), offset(steep[num_layers - 10], scaled<float>(0.005))).empty());
    CHECK(diff(steep.back(), offset(steep.front(), scaled<float>(double(num_layers - 1) * lean + 0.005))).empty());
    const double stair = 2.5 * 0.42 / 128.;
    CHECK_FALSE(diff(steep.back(), offset(steep.front(), scaled<float>(double(num_layers - 1) * lean - stair - 0.002)))
                    .empty());
    CHECK(checks_steep == 1 + 128 + num_layers);
    // Leaning less a layer than a step the widening tells apart, they do not widen at all, as at nought: every layer
    // keeps its bodies as they are, and nothing is checked for cancelling.
    size_t                      checks_still = 0;
    const std::vector<Polygons> still        = widened(0.01, checks_still);
    REQUIRE(still.size() == num_layers);
    CHECK(std::count_if(still.begin(), still.end(), [&rounds](const Polygons &layer) { return layer != rounds; }) == 0);
    CHECK(checks_still == 0);
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
    organic_widen_bodies_under_roofs(bodies, roofs, model, {}, true, line_width, tip_radius, scaled<coord_t>(move),
        scaled<coord_t>(reach), { 0. });
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

TEST_CASE("Organic hollow bodies narrow going up by the step a layer at most", "[TreeSupport][Material][Regression]")
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

TEST_CASE("Organic hollow bodies narrow by a layer height a layer at fine layers and by half a line at others", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    const auto step = [](const double layer_height, const double line_width, const double angle) {
        return unscaled<double>(organic_wall_step(scaled<coord_t>(layer_height), scaled<coord_t>(line_width),
            scaled<coord_t>(layer_height * std::tan(angle * PI / 180.))));
    };
    // Below a sixth of a line, 45 degrees, or the branch angle where it is steeper.
    CHECK_THAT(step(0.02, 0.22, 40.), Catch::Matchers::WithinAbs(0.02, 1e-6));
    CHECK_THAT(step(0.03, 0.22, 40.), Catch::Matchers::WithinAbs(0.03, 1e-6));
    CHECK_THAT(step(0.02, 0.22, 60.), Catch::Matchers::WithinAbs(0.02 * std::tan(PI / 3.), 1e-6));
    // Half a line at higher layers, at 0.2 mm layers and 0.42 mm lines among them.
    CHECK_THAT(step(0.04, 0.22, 40.), Catch::Matchers::WithinAbs(0.11, 1e-6));
    CHECK_THAT(step(0.08, 0.42, 40.), Catch::Matchers::WithinAbs(0.21, 1e-6));
    CHECK_THAT(step(0.2, 0.42, 40.), Catch::Matchers::WithinAbs(0.21, 1e-6));
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
        // the flange in their area, including its perimeter where necessary, but for slivers the unions leave, a few
        // microns wide at most, which the fill of the first layer clips off. Neither can enter the model.
        if (i > 0)
            CHECK(intersection(trees, offset(walls(storage[i]), - scaled<float>(0.25 * line_width + 0.005))).empty());
        else
            CHECK(offset(diff(trees, storage[i].polygons), - scaled<float>(0.005)).empty());
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
    // The lines of the roof run along Y, or at 45 degrees to the axes, as the support angle sets them: those of the
    // roof, or of a base interface under a roof printed in loops.
    const double angle = GENERATE(0.5 * M_PI, 0.25 * M_PI);
    const bool   based = GENERATE(false, true);
    CAPTURE(angle, based);
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
    if (based)
        organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach),
            settings.support_line_width, {}, {}, roofs, { angle });
    else
        organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach),
            settings.support_line_width, { angle });
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

TEST_CASE("Organic trees under a solid part of a hollow body hold every bead of it", "[TreeSupport][Material][Regression]")
{
    using namespace Slic3r;
    using namespace Slic3r::TreeSupport3D;
    // Lines 0.22 mm wide, layers 0.02 mm, tips 0.8 mm across, a branch angle of 40 degrees, a preferred one of 25 and a
    // diameter angle of 5. A body 6 mm square, hollow, whose top layer is printed solid over the inside of the layer
    // below: all of it, as a body widened under a roof is, or a core 2 mm square in it, with its walls. A bead touches
    // what is below where its axis lies within half a line of it, wherever the spiral runs, so the trees of the layer
    // below hold every point of the solid part that near, far from its walls. The reach a roof line bridges plays no part,
    // up to the two lines of a gap of the base of three.
    const double line_width = 0.22, layer_height = 0.02;
    const bool   core  = GENERATE(false, true);
    const double reach = GENERATE(0.22, 0.235, 0.44);
    CAPTURE(core, reach);
    TreeSupportSettings settings;
    settings.support_line_width               = scaled<coord_t>(line_width);
    settings.layer_height                     = scaled<coord_t>(layer_height);
    settings.min_radius                       = scaled<coord_t>(0.4);
    settings.maximum_move_distance            = scaled<coord_t>(std::tan(40. * M_PI / 180.) * layer_height);
    settings.maximum_move_distance_slow       = scaled<coord_t>(std::tan(25. * M_PI / 180.) * layer_height);
    settings.branch_radius_increase_per_layer = scaled<double>(std::tan(5. * M_PI / 180.) * layer_height);
    settings.support_rests_on_model           = false;
    const Polygon square({ Point::new_scale(-3., -3.), Point::new_scale(3., -3.), Point::new_scale(3., 3.), Point::new_scale(-3., 3.) });
    const Polygon middle({ Point::new_scale(-1., -1.), Point::new_scale(1., -1.), Point::new_scale(1., 1.), Point::new_scale(-1., 1.) });
    const size_t num_layers = 20;
    std::vector<SupportGeneratorLayer> storage(num_layers);
    std::vector<Polygons> roofs(num_layers), model(num_layers);
    SupportGeneratorLayersPtr bodies(num_layers, nullptr);
    for (size_t i = 0; i < storage.size(); ++ i) {
        SupportGeneratorLayer &layer = storage[i];
        layer.bottom_z = layer_height * double(i);
        layer.print_z  = layer.bottom_z + layer_height;
        layer.height   = layer_height;
        layer.polygons = { square };
        bodies[i]      = &layer;
    }
    const Polygons solid{ core ? middle : square };
    storage.back().solid_polygons = solid;
    organic_grow_trees_inside_hollow_bodies(bodies, roofs, model, settings, scaled<coord_t>(reach), settings.support_line_width);
    // The layer below prints its walls, a band three quarters of a line wide along its outline, and the trees inside it.
    const SupportGeneratorLayer &below = storage[num_layers - 2];
    REQUIRE_FALSE(below.inner_trees.empty());
    const float    half = 0.5f * float(settings.support_line_width);
    const Polygons held = union_(diff(Polygons{ square }, offset(Polygons{ square }, - 1.5f * half)), below.inner_trees);
    // But for specks where three tips just meet, their discs being polygons a few micrometres inside their circles, as the
    // trees leave them too.
    CHECK(area(opening(diff(solid, offset(held, half + float(SCALED_EPSILON))), scaled<float>(0.005))) <
        scaled<double>(0.01) * scaled<double>(0.01));
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
