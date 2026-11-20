#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Two 10x20x6mm pillars 10mm apart in one filament, bridged at z=[5,6] by a 10x20mm deck in another, the way a
// painted underside of a bridge is segmented. The deck's first layer (print_z 5.2) lies entirely over the gap,
// next to the tops of the pillars on the same layer.
DynamicPrintConfig painted_bridge_config(const char *wall_generator, bool thick_bridges = false, bool extra_perimeters = false)
{
    return multifilament_config(2, {
        { "wall_generator",                wall_generator },
        { "layer_height",                  0.2 },
        { "initial_layer_print_height",    0.2 },
        { "wall_loops",                    2 },
        { "sparse_infill_density",         "15%" },
        { "bottom_shell_layers",           3 },
        { "outer_wall_line_width",         0.42 },
        { "inner_wall_line_width",         0.45 },
        { "detect_overhang_wall",          true },
        { "extra_perimeters_on_overhangs", extra_perimeters },
        { "thick_bridges",                 thick_bridges },
        { "enable_support",                false },
        { "enable_prime_tower",            false },
    });
}

void painted_bridge(Print &print, Model &model, const DynamicPrintConfig &config, bool reverse_filaments = false,
                    const DynamicPrintConfig &deck_config = DynamicPrintConfig(), double pillar_height = 6.)
{
    ModelObject *object = model.add_object();
    object->name = "painted_bridge.stl";
    TriangleMesh pillars = make_cube(10., 20., pillar_height);
    TriangleMesh far_pillar = make_cube(10., 20., pillar_height);
    far_pillar.translate(20.f, 0.f, 0.f);
    pillars.merge(far_pillar);
    object->add_volume(std::move(pillars))->config.set("extruder", reverse_filaments ? 2 : 1);
    TriangleMesh deck = make_cube(10., 20., 1.);
    deck.translate(10.f, 0.f, 5.f);
    ModelVolume *deck_volume = object->add_volume(std::move(deck));
    deck_volume->config.set("extruder", reverse_filaments ? 1 : 2);
    deck_volume->config.apply(deck_config);
    object->add_instance();
    object->ensure_on_bed();

    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// The first layer of the deck, with the regions of the deck and of the pillars on it.
struct DeckLayer
{
    const Layer       *layer   = nullptr;
    const LayerRegion *deck    = nullptr;
    const LayerRegion *pillars = nullptr;
};

DeckLayer deck_layer(const Print &print)
{
    DeckLayer out;
    for (const Layer *layer : print.objects().front()->layers())
        if (std::abs(layer->print_z - 5.2) < EPSILON)
            out.layer = layer;
    if (out.layer != nullptr)
        for (const LayerRegion *layerm : out.layer->regions())
            if (! layerm->slices.empty())
                (get_extents(layerm->slices.surfaces).size().x() < scaled(15.) ? out.deck : out.pillars) = layerm;
    return out;
}

// Length of the paths of `entities` running closer to the Y axis than to the X axis, the direction of the
// boundaries between the deck and the pillars, keeping only the lines `keep` accepts.
template<typename Keep>
double y_running_length(const ExtrusionEntityCollection &entities, Keep &&keep)
{
    double length = 0.;
    for_each_extrusion_path(entities, [&length, &keep](const ExtrusionPath &path) {
        for (const Line &line : path.polyline.to_polyline().lines())
            if (std::abs(line.b.y() - line.a.y()) > std::abs(line.b.x() - line.a.x()) && keep(line))
                length += unscale<double>(line.length());
    });
    return length;
}

} // namespace

TEST_CASE("A bridge in another filament is anchored on its neighbours", "[Bridge]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool thick_bridges = GENERATE(false, true);
    const bool extra_perimeters = GENERATE(false, true);
    const bool reverse_filaments = GENERATE(false, true);
    CAPTURE(wall_generator, thick_bridges, extra_perimeters, reverse_filaments);

    Print print;
    Model model;
    painted_bridge(print, model, painted_bridge_config(wall_generator, thick_bridges, extra_perimeters), reverse_filaments);
    const auto [layer, deck, pillars] = deck_layer(print);
    REQUIRE(layer != nullptr);
    REQUIRE(deck != nullptr);
    REQUIRE(pillars != nullptr);
    const BoundingBox gap        = get_extents(diff_ex(layer->lslices, layer->lower_layer->lslices));
    const double      line_width = pillars->flow(frExternalPerimeter).width();

    // The deck has no walls along the pillars, where they would be laid in mid air.
    CHECK(y_running_length(deck->perimeters, [](const Line &) { return true; }) < 1.);
    // The pillars keep their walls away from the deck.
    CHECK(y_running_length(pillars->perimeters, [&gap](const Line &line) {
        const coord_t x = (line.a.x() + line.b.x()) / 2;
        return x < gap.min.x() - scaled(5.) || x > gap.max.x() + scaled(5.);
    }) > 2. * 15.);

    // The deck takes a strip of the tops of the pillars to anchor on, which leaves the outer wall of the object to the
    // pillars.
    const ExPolygons anchors = intersection_ex(deck->slices.surfaces, layer->lower_layer->lslices);
    CHECK(area(anchors) > 0.75 * line_width * 2. * 15. / SCALING_FACTOR / SCALING_FACTOR);
    CHECK(area(diff_ex(anchors, offset(layer->lslices, float(scaled(-line_width)) + SCALED_EPSILON))) < sqr(scale_(0.01)));
    CHECK(area(intersection_ex(deck->fill_expolygons, pillars->fill_expolygons)) < sqr(scale_(0.01)));

    // The deck is bridged across the gap, and away from the outer surface the bridge reaches on over the strip.
    double along = 0.;
    double across = 0.;
    size_t anchored = 0;
    for_each_extrusion_path(deck->fills, [&](const ExtrusionPath &path) {
        if (path.role() != erBridgeInfill)
            return;
        for (const Line &line : path.polyline.to_polyline().lines()) {
            const bool along_x = std::abs(line.b.x() - line.a.x()) > std::abs(line.b.y() - line.a.y());
            (along_x ? along : across) += unscale<double>(line.length());
            if (! along_x || std::abs(line.a.y() - gap.center().y()) > gap.size().y() / 2 - scaled(2.))
                continue;
            INFO("span: " << unscale<double>(line.a.x()) << " -> " << unscale<double>(line.b.x()) << " at " << unscale<double>(line.a.y()));
            CHECK(unscale<double>(gap.min.x() - std::min(line.a.x(), line.b.x())) + path.width / 2. > 0.75 * line_width);
            CHECK(unscale<double>(std::max(line.a.x(), line.b.x()) - gap.max.x()) + path.width / 2. > 0.75 * line_width);
            ++ anchored;
        }
    });
    CHECK(along > 100.);
    CHECK(across < along / 4.);
    CHECK(anchored > 10);

    // The bridge does not run over the extrusions of the pillars.
    Polygons bridge_footprint;
    for_each_extrusion_path(deck->fills, [&bridge_footprint](const ExtrusionPath &path) {
        if (path.role() == erBridgeInfill)
            path.polygons_covered_by_width(bridge_footprint, 0.f);
    });
    Polygons pillar_footprint = pillars->perimeters.polygons_covered_by_width(0.f);
    polygons_append(pillar_footprint, pillars->fills.polygons_covered_by_width(0.f));
    CHECK(area(intersection_ex(union_ex(bridge_footprint), union_ex(pillar_footprint))) < 0.05 * 2. * 20. / SCALING_FACTOR / SCALING_FACTOR);
}

TEST_CASE("A bridge in another filament is anchored on a neighbour two walls thin", "[Bridge]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    // A wall as thin across the middle splits the lid into two bridges, with a line of the tube between them.
    const bool divided = GENERATE(false, true);
    CAPTURE(wall_generator, divided);

    // A 20x20x6mm square tube with walls two outer walls thick in one filament, closed at z=[5,6] by a lid in another,
    // the way a painted sacrificial layer over a hole is segmented.
    const double wall = 2. * 0.42;
    TriangleMesh tube = make_cube(wall, 20., 6.);
    // The other sides: x, y, size x, size y.
    const std::array<std::array<double, 4>, 4> sides{ { { 20. - wall, 0., wall, 20. }, { wall, 0., 20. - 2. * wall, wall },
                                                        { wall, 20. - wall, 20. - 2. * wall, wall }, { 10. - wall / 2., wall, wall, 20. - 2. * wall } } };
    for (size_t i = 0; i < (divided ? 4 : 3); ++ i) {
        TriangleMesh side = make_cube(sides[i][2], sides[i][3], 6.);
        side.translate(float(sides[i][0]), float(sides[i][1]), 0.f);
        tube.merge(side);
    }
    Model model;
    ModelObject *object = model.add_object();
    object->name = "painted_lid.stl";
    object->add_volume(std::move(tube))->config.set("extruder", 1);
    for (const double x : divided ? std::vector<double>{ wall, 10. + wall / 2. } : std::vector<double>{ wall }) {
        TriangleMesh lid = make_cube(divided ? 10. - 1.5 * wall : 20. - 2. * wall, 20. - 2. * wall, 1.);
        lid.translate(float(x), float(wall), 5.f);
        object->add_volume(std::move(lid))->config.set("extruder", 2);
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.apply(model, painted_bridge_config(wall_generator));
    print.set_status_silent();
    print.process();

    const Layer *layer = nullptr;
    for (const Layer *l : print.objects().front()->layers())
        if (std::abs(l->print_z - 5.2) < EPSILON)
            layer = l;
    REQUIRE(layer != nullptr);
    const auto contains = [](const ExPolygons &expolys, const Point &pt) {
        return std::any_of(expolys.begin(), expolys.end(), [&pt](const ExPolygon &expoly) { return expoly.contains(pt); });
    };
    // A point inside the lid, off the middle wall.
    const Point        in_lid      = get_extents(layer->lslices).center() + Point(scaled(5.), 0);
    const LayerRegion *lid_region  = nullptr;
    const LayerRegion *tube_region = nullptr;
    for (const LayerRegion *layerm : layer->regions())
        if (! layerm->slices.empty())
            (contains(to_expolygons(layerm->slices.surfaces), in_lid) ? lid_region : tube_region) = layerm;
    REQUIRE(lid_region != nullptr);
    REQUIRE(tube_region != nullptr);

    // The tube keeps its outer wall around the outline of the object, and the lid has no walls in mid air.
    double longest_loop = 0.;
    for (const ExtrusionEntity *entity : tube_region->perimeters.flatten().entities)
        if (entity->is_loop())
            longest_loop = std::max(longest_loop, unscale<double>(entity->length()));
    CHECK(longest_loop > 4. * 18.);
    double lid_walls = 0.;
    for_each_extrusion_path(lid_region->perimeters, [&lid_walls](const ExtrusionPath &path) { lid_walls += unscale<double>(path.length()); });
    CHECK(lid_walls < 1.);

    // The lid is bridged from the top of one wall of the tube to the top of another.
    size_t spans = 0;
    size_t anchored = 0;
    for_each_extrusion_path(lid_region->fills, [&](const ExtrusionPath &path) {
        if (path.role() != erBridgeInfill)
            return;
        for (const Line &line : path.polyline.to_polyline().lines())
            if (line.length() > scaled(8.)) {
                ++ spans;
                anchored += size_t(contains(layer->lower_layer->lslices, line.a) && contains(layer->lower_layer->lslices, line.b));
            }
    });
    CHECK(spans > 10);
    CHECK(anchored > spans * 9 / 10);
}

TEST_CASE("Regions sharing their walls keep them where a bridge of one is anchored on the other", "[Bridge]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    // Total length and open paths of the walls on the first layer of the deck, and the strip the deck took.
    const auto walls = [wall_generator](const DynamicPrintConfig &deck_config) {
        Print print;
        Model model;
        painted_bridge(print, model, painted_bridge_config(wall_generator), false, deck_config);
        const auto [layer, deck, pillars] = deck_layer(print);
        REQUIRE(layer != nullptr);
        double length = 0.;
        size_t open   = 0;
        for (const LayerRegion *layerm : layer->regions()) {
            for_each_extrusion_path(layerm->perimeters, [&length](const ExtrusionPath &path) { length += unscale<double>(path.length()); });
            for (const ExtrusionEntity *entity : layerm->perimeters.flatten().entities)
                open += size_t(! entity->is_loop());
        }
        const double anchor = deck == nullptr ? 0. : area(intersection_ex(deck->slices.surfaces, layer->lower_layer->lslices));
        return std::make_tuple(length, open, anchor);
    };

    // The deck differs only in its infill, so the walls are shared as on a single region.
    DynamicPrintConfig shared;
    shared.set_deserialize_strict({ { "extruder", "1" }, { "sparse_infill_density", "20%" } });
    DynamicPrintConfig single;
    single.set_deserialize_strict("extruder", "1");
    const auto [shared_length, shared_open, shared_anchor] = walls(shared);
    const auto [single_length, single_open, single_anchor] = walls(single);

    CHECK(shared_anchor > 0.);
    CHECK(shared_open == single_open);
    CHECK_THAT(shared_length, Catch::Matchers::WithinAbs(single_length, 0.01));
}

TEST_CASE("The anchor of a bridge in another filament replaces the outer wall of the neighbour", "[Bridge]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool  wider_deck     = GENERATE(false, true);
    CAPTURE(wall_generator, wider_deck);

    // The walls of the deck or of the pillars are wider than those of the other.
    DynamicPrintConfig wide;
    wide.set_deserialize_strict({ { "outer_wall_line_width", "0.6" }, { "inner_wall_line_width", "0.6" } });
    DynamicPrintConfig narrow;
    narrow.set_deserialize_strict({ { "outer_wall_line_width", "0.42" }, { "inner_wall_line_width", "0.45" } });
    DynamicPrintConfig config = painted_bridge_config(wall_generator);
    config.apply(wider_deck ? narrow : wide);
    const DynamicPrintConfig deck_config = wider_deck ? wide : narrow;
    Print print;
    Model model;
    painted_bridge(print, model, config, false, deck_config);
    const auto [layer, deck, pillars] = deck_layer(print);
    REQUIRE(layer != nullptr);
    REQUIRE(deck != nullptr);
    REQUIRE(pillars != nullptr);
    const double  mm2   = SCALING_FACTOR * SCALING_FACTOR;
    const coord_t outer = pillars->flow(frExternalPerimeter).scaled_width();
    const coord_t inner = pillars->flow(frPerimeter).scaled_width();

    // The strip is as wide as the outer wall of the pillars, along both sides of the gap but for the outline.
    const double outer_mm = unscale<double>(outer);
    const double strip    = area(intersection_ex(deck->slices.surfaces, layer->lower_layer->lslices)) * mm2;
    CHECK_THAT(strip, Catch::Matchers::WithinRel(2. * outer_mm * (20. - 2. * outer_mm), 0.05));

    // The tops of the pillars along the gap, as deep as their outer wall and half the inner one, are covered once.
    const ExPolygons gap  = diff_ex(layer->lslices, layer->lower_layer->lslices);
    const ExPolygons band = intersection_ex(diff_ex(offset_ex(gap, float(outer + inner / 2)), gap), offset(layer->lslices, float(- scale_(1.))));
    Polygons covered;
    for (const LayerRegion *layerm : layer->regions()) {
        layerm->perimeters.polygons_covered_by_width(covered, 0.f);
        layerm->thin_fills.polygons_covered_by_width(covered, 0.f);
        layerm->fills.polygons_covered_by_width(covered, 0.f);
    }
    Polygons bridge;
    for_each_extrusion_path(deck->fills, [&bridge](const ExtrusionPath &path) {
        if (path.role() == erBridgeInfill)
            path.polygons_covered_by_width(bridge, 0.f);
    });
    const double uncovered = area(diff_ex(band, covered)) * mm2;
    const double doubled   = area(intersection_ex(union_ex(bridge), union_ex(pillars->perimeters.polygons_covered_by_width(0.f)))) * mm2;
    CHECK(uncovered < 1.5);
    CHECK(doubled < 2.5);
}

TEST_CASE("A bridge in another filament takes no strip of a top surface of its neighbours", "[Bridge]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    // The pillars end on the first layer of the deck, which leaves their tops uncovered.
    Print print;
    Model model;
    painted_bridge(print, model, painted_bridge_config(wall_generator), false, DynamicPrintConfig(), 5.2);
    const auto [layer, deck, pillars] = deck_layer(print);
    REQUIRE(layer != nullptr);
    REQUIRE(deck != nullptr);
    REQUIRE(pillars != nullptr);
    CHECK(area(intersection_ex(deck->slices.surfaces, layer->lower_layer->lslices)) * SCALING_FACTOR * SCALING_FACTOR < 0.01);

    // The deck still has no walls in mid air along the pillars, and is bridged across the gap.
    CHECK(y_running_length(deck->perimeters, [](const Line &) { return true; }) < 1.);
    double along  = 0.;
    double across = 0.;
    for_each_extrusion_path(deck->fills, [&along, &across](const ExtrusionPath &path) {
        if (path.role() == erBridgeInfill)
            for (const Line &line : path.polyline.to_polyline().lines())
                (std::abs(line.b.x() - line.a.x()) > std::abs(line.b.y() - line.a.y()) ? along : across) += unscale<double>(line.length());
    });
    CHECK(along > 100.);
    CHECK(across < along / 4.);
}

TEST_CASE("The anchor of a bridge in another filament follows the settings it is reprocessed with", "[Bridge]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    // The first change reprocesses the walls, the second one only the infill.
    const auto change = GENERATE(std::make_pair("outer_wall_line_width", "0.6"), std::make_pair("bridge_angle", "45"));
    CAPTURE(wall_generator, change.first);

    const auto summary = [](const Print &print) {
        const auto [layer, deck, pillars] = deck_layer(print);
        REQUIRE(deck != nullptr);
        double bridge = 0.;
        for_each_extrusion_path(deck->fills, [&bridge](const ExtrusionPath &path) {
            if (path.role() == erBridgeInfill)
                bridge += unscale<double>(path.length());
        });
        return std::make_pair(unscale<double>(unscale<double>(area(to_expolygons(deck->slices.surfaces)))), bridge);
    };

    DynamicPrintConfig config = painted_bridge_config(wall_generator);
    Print print;
    Model model;
    painted_bridge(print, model, config);
    config.set_deserialize_strict(change.first, change.second);
    print.apply(model, config);
    print.process();

    Print fresh_print;
    Model fresh_model;
    painted_bridge(fresh_print, fresh_model, config);

    const auto [deck_area, bridge_length] = summary(print);
    const auto [fresh_deck_area, fresh_bridge_length] = summary(fresh_print);
    CHECK_THAT(deck_area, Catch::Matchers::WithinRel(fresh_deck_area, 1e-6));
    CHECK_THAT(bridge_length, Catch::Matchers::WithinRel(fresh_bridge_length, 1e-6));
}
