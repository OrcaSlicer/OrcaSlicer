#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/AABBTreeLines.hpp"

#include "test_helpers.hpp"

#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Object layer heights", "[PrintObject]") {
    GIVEN("A 20mm cube") {
        WHEN("sliced with a 2mm layer height and a 3mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               2 },
                { "nozzle_diameter",            3 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
            THEN("The output vector has 10 entries") {
                REQUIRE(layers.size() == 10);
            }
            AND_THEN("Each layer is approximately 2mm above the previous Z") {
                coordf_t last = 0.0;
                for (size_t i = 0; i < layers.size(); ++ i) {
                    REQUIRE_THAT(layers[i]->print_z - last, Catch::Matchers::WithinAbs(2.0, 1e-4));
                    last = layers[i]->print_z;
                }
            }
        }
        WHEN("sliced with a 10mm layer height and an 11mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               10 },
                { "nozzle_diameter",            11 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 3 entries") {
                REQUIRE(layers.size() == 3);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE_THAT(layers.front()->print_z, Catch::Matchers::WithinAbs(2.0, 1e-4));
            }
            AND_THEN("Layer 1 is at 12mm") {
                REQUIRE_THAT(layers[1]->print_z, Catch::Matchers::WithinAbs(12.0, 1e-4));
            }
        }
        WHEN("sliced with a 15mm layer height and a 16mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               15 },
                { "nozzle_diameter",            16 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 2 entries") {
                REQUIRE(layers.size() == 2);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE_THAT(layers[0]->print_z, Catch::Matchers::WithinAbs(2.0, 1e-4));
            }
            AND_THEN("Layer 1 is at 17mm") {
                REQUIRE_THAT(layers[1]->print_z, Catch::Matchers::WithinAbs(17.0, 1e-4));
            }
        }
        WHEN("layer height exceeds the nozzle diameter") {
            // Orca does not clamp an over-large layer height to the nozzle; it
            // rejects the slice during flow computation. Pin that behavior.
            THEN("Slicing is rejected") {
                Slic3r::Print print;
                REQUIRE_THROWS(Slic3r::Test::init_and_process_print({cube(20)}, print, {
                    { "initial_layer_print_height", 0.3 },
                    { "layer_height",               0.5 },
                    { "nozzle_diameter",            0.4 }
                }));
            }
        }
    }
}

SCENARIO("Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, { { "sparse_infill_density", 0 } });
			const PrintObject &object = *print.objects().front();
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
        }
        WHEN("wall_loops is set to 3")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "sparse_infill_density", 0 },
                { "wall_loops",            3 }
            });
            const PrintObject &object = *print.objects().front();
            THEN("Every layer in region 0 has 3 perimeter loops") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

TEST_CASE("Initial layer height is honored", "[PrintObject]")
{
    const std::string gcode = Slic3r::Test::slice({cube(20)}, {
        { "initial_layer_print_height", 0.3 },
        { "layer_height",               0.2 },
        { "z_hop",                      0 } // keep recorded Z equal to the printed layer height
    });

    std::set<double> layer_zs;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&layer_zs] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (line.extruding(self) && line.dist_XY(self) > 0)
            layer_zs.insert(self.z());
    });

    REQUIRE(layer_zs.size() > 1);
    REQUIRE_THAT(*layer_zs.begin(),            Catch::Matchers::WithinAbs(0.3, 1e-4));
    REQUIRE_THAT(*std::next(layer_zs.begin()), Catch::Matchers::WithinAbs(0.5, 1e-4));
}

static TriangleMesh internal_bridge_step()
{
    // Orca: The smaller tower leaves a shoulder whose solid skin needs internal bridges
    // over the sparse infill in the base, without relying on an external model file.
    TriangleMesh mesh = make_cube(30, 24, 3);
    TriangleMesh tower = make_cube(14, 10, 1);
    tower.translate(8, 7, 3);
    mesh.merge(tower);
    return mesh;
}

static DynamicPrintConfig internal_bridge_config(const std::string &pattern, int multiline)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"fill_multiline", multiline},
                                   {"sparse_infill_density", "15%"},
                                   {"sparse_infill_smooth_factor", "100%"},
                                   {"infill_direction", 45},
                                   {"internal_bridge_angle", 0},
                                   {"thick_internal_bridges", true},
                                   {"top_shell_layers", 3},
                                   {"bottom_shell_layers", 2},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});
    return config;
}

TEST_CASE("Internal bridge angles follow the lower infill layer and model rotation", "[PrintObject][InternalBridge][Regression]")
{
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral");
    // Orca: Cover both a central line (odd counts) and offset pairs (even counts).
    const int multiline = GENERATE(1, 2, 3);
    CAPTURE(multiline);
    const double rotation = GENERATE(23., -123.);
    const std::vector<double> cycle{10., 30., 70.};
    auto config = internal_bridge_config(pattern, multiline);
    config.set_deserialize_strict({{"sparse_infill_rotate_template", "10,30,70"},
                                   {"align_infill_direction_to_model", true},
                                   {"separated_infills", false}});
    Print print;
    Model model;
    init_print({internal_bridge_step()}, print, model, config, nullptr, false);
    model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(rotation)));
    print.apply(model, config);
    print.process();
    const PrintObject &object = *print.objects().front();
    size_t bridges = 0;
    for (size_t i = 1; i < object.layer_count(); ++i) {
        // Orca: The support is one layer below the bridge. Check the template and model
        // rotation together, including normalization when the resulting angle is negative.
        double expected = std::fmod(cycle[(i - 1) % cycle.size()] + 90. + rotation, 180.);
        if (expected < 0.) expected += 180.;
        for (const LayerRegion *region : object.get_layer(i)->regions())
            for (const Surface *surface : region->fill_surfaces.filter_by_type(stInternalBridge)) {
                CAPTURE(pattern, rotation, i);
                CHECK_THAT(Geometry::rad2deg(surface->bridge_angle), Catch::Matchers::WithinAbs(expected, 0.001));
                ++bridges;
            }
    }
    REQUIRE(bridges > 0);
}

TEST_CASE("Turning infill does not replace the anchors of another region", "[PrintObject][InternalBridge][Regression]")
{
    // Orca: Keep the right-hand region fixed while changing the left-hand pattern in the
    // same object. Its bridge areas must be independent of a previous candidate's anchors.
    const int multiline = GENERATE(1, 2, 3);
    CAPTURE(multiline);
    auto right_bridges = [multiline](const std::string &left_pattern) {
        auto config = internal_bridge_config(left_pattern, multiline);
        Print print;
        Model model;
        init_print({internal_bridge_step()}, print, model, config, nullptr, false);
        TriangleMesh right = internal_bridge_step();
        right.translate(50, 0, 0);
        ModelVolume *volume = model.objects.front()->add_volume(std::move(right));
        volume->config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        volume->config.set_key_value("infill_direction", new ConfigOptionFloat(17.));
        print.apply(model, config);
        print.process();
        std::map<size_t, Polygons> result;
        const PrintObject &object = *print.objects().front();
        for (size_t i = 0; i < object.layer_count(); ++i)
            for (const LayerRegion *region : object.get_layer(i)->regions())
                if (region->region().config().infill_direction == 17.)
                    polygons_append(result[i], to_polygons(region->fill_surfaces.filter_by_type(stInternalBridge)));
        return result;
    };
    const auto baseline = right_bridges("rectilinear");
    const auto actual = right_bridges(GENERATE("hilbertcurve", "octagramspiral"));
    REQUIRE(actual.size() == baseline.size());
    double total_area = 0.;
    for (const auto &[layer, expected] : baseline) {
        CAPTURE(layer);
        const auto &polys = actual.at(layer);
        CHECK(area(diff(expected, polys)) < scaled<double>(1.) * scaled<double>(1.) * 1e-6);
        CHECK(area(diff(polys, expected)) < scaled<double>(1.) * scaled<double>(1.) * 1e-6);
        total_area += area(expected);
    }
    REQUIRE(total_area > 0.);
}

TEST_CASE("Rounded internal bridges end on printed support", "[PrintObject][InternalBridge][Regression]")
{
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral");
    const bool separated = GENERATE(false, true);
    CAPTURE(pattern, separated);
    auto config = internal_bridge_config(pattern, 1);
    config.set_deserialize_strict({{"infill_wall_overlap", "0%"}, {"separated_infills", separated}});
    TriangleMesh mesh = internal_bridge_step();
    if (separated) {
        TriangleMesh second = internal_bridge_step();
        second.translate(50, 0, 0);
        mesh.merge(second);
    }
    Print print;
    Model model;
    init_print({mesh}, print, model, config, nullptr, false);
    print.process();

    // Orca: Check final extrusion endpoints after polygon cleanup and fill generation.
    // A correct bridge angle and correct sparse anchors alone do not guarantee contact.
    const PrintObject &object = *print.objects().front();
    size_t checked = 0;
    for (size_t i = 1; i < object.layer_count(); ++i) {
        Polygons support;
        Polylines walls;
        for (const LayerRegion *region : object.get_layer(i - 1)->regions()) {
            region->perimeters.polygons_covered_by_width(support, 0.f);
            region->fills.polygons_covered_by_width(support, 0.f);
            region->perimeters.collect_polylines(walls);
        }
        REQUIRE_FALSE(support.empty());
        const AABBTreeLines::LinesDistancer<Line> support_tree(to_lines(union_(support)));
        const AABBTreeLines::LinesDistancer<Line> wall_tree(to_lines(walls));
        for (const LayerRegion *region : object.get_layer(i)->regions())
            for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
                if (entity->role() != erInternalBridgeInfill)
                    continue;
                const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
                REQUIRE(path != nullptr);
                for (const Line &line : path->polyline.to_polyline().lines()) {
                    // Orca: Sample span ends, excluding short connectors and wall overlap.
                    if (line.length() < scale_(std::max(0.7, 3. * path->width)))
                        continue;
                    for (const Point &point : {line.a, line.b}) {
                        if (wall_tree.distance_from_lines<false>(point) <= scale_(0.5))
                            continue;
                        CAPTURE(i, point.x(), point.y());
                        const double gap = unscale<double>(support_tree.distance_from_lines<true>(point)) - 0.5 * path->width;
                        CHECK(gap <= 0.1);
                        ++checked;
                    }
                }
            }
    }
    REQUIRE(checked > 0);
}

TEST_CASE("Enabling separated infill recomputes body origins", "[PrintObject][InternalBridge][Regression]")
{
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral", "archimedeanchords");
    CAPTURE(pattern);
    auto footprint = [&](bool reslice) {
        auto config = internal_bridge_config(pattern, 2);
        config.set_deserialize_strict({{"separated_infills", !reslice}});
        TriangleMesh mesh = internal_bridge_step();
        TriangleMesh second = internal_bridge_step();
        second.translate(50, 0, 0);
        mesh.merge(second);
        Print print;
        Model model;
        init_print({mesh}, print, model, config, nullptr, false);
        print.process();
        if (reslice) {
            // Orca: Enabling centering after a completed slice must rebuild the body
            // origins now shared by bridge preparation and printed infill.
            config.set_deserialize_strict({{"separated_infills", true}});
            print.apply(model, config);
            print.process();
        }
        Polygons result;
        for (const LayerRegion *region : print.objects().front()->get_layer(4)->regions())
            region->fills.polygons_covered_by_width(result, 0.f);
        return union_(result);
    };
    const Polygons fresh = footprint(false);
    const Polygons resliced = footprint(true);
    REQUIRE_FALSE(fresh.empty());
    CHECK(area(diff(fresh, resliced)) < scaled<double>(1.) * scaled<double>(1.) * 1e-6);
    CHECK(area(diff(resliced, fresh)) < scaled<double>(1.) * scaled<double>(1.) * 1e-6);
}

TEST_CASE("Surface centering survives changes to separated infill settings", "[PrintObject][SurfaceInfill][Regression]")
{
    const std::string pattern = GENERATE("archimedeanchords", "octagramspiral");
    const std::string initial_center = GENERATE("each_surface", "each_model", "each_assembly");
    const std::string final_center = GENERATE("each_surface", "each_model", "each_assembly");
    const bool separated = GENERATE(false, true);
    const std::string top_order = GENERATE("default", "outward", "inward");
    const std::string bottom_order = top_order == "outward" ? "inward" : top_order == "inward" ? "outward" : "default";
    const std::string density = GENERATE("80%", "100%");
    const bool change_center = initial_center != final_center;
    CAPTURE(pattern, initial_center, final_center, separated, top_order, bottom_order, density);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"top_surface_pattern", pattern},
                                   {"bottom_surface_pattern", pattern},
                                   {"top_surface_fill_order", top_order},
                                   {"bottom_surface_fill_order", bottom_order},
                                   {"top_surface_density", density},
                                   {"bottom_surface_density", density},
                                   {"center_of_surface_pattern", initial_center},
                                   {"separated_infills", change_center ? separated : !separated},
                                   {"sparse_infill_pattern", "rectilinear"},
                                   {"sparse_infill_density", "15%"},
                                   {"top_shell_layers", 2},
                                   {"bottom_shell_layers", 2},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});

    // Orca: Two disconnected bodies exercise per-body centering. The offset tower also
    // makes each-surface and each-model centering differ on the top surfaces.
    TriangleMesh mesh = make_cube(30, 24, 2);
    TriangleMesh tower = make_cube(12, 10, 1);
    tower.translate(4, 3, 2);
    mesh.merge(tower);
    TriangleMesh second = mesh;
    second.translate(50, 0, 0);
    mesh.merge(second);

    // Orca: Equal footprints can hide reordered or reversed paths. Retain their point
    // sequences and ordering protection to cover the directional surface behavior too.
    struct SurfaceFillSnapshot {
        std::map<bool, std::vector<Points>> paths;
        bool protected_order = true;
    };
    auto surface_fills = [](const Print &print) {
        std::map<std::pair<size_t, ExtrusionRole>, SurfaceFillSnapshot> result;
        const PrintObject &object = *print.objects().front();
        for (size_t i = 0; i < object.layer_count(); ++i) {
            auto collect = [&](const auto &self, const ExtrusionEntity &entity, bool no_sort) -> void {
                if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                    for (const ExtrusionEntity *child : collection->entities)
                        self(self, *child, no_sort || collection->no_sort);
                } else if (entity.role() == erTopSolidInfill || entity.role() == erBottomSurface) {
                    const auto *path = dynamic_cast<const ExtrusionPath *>(&entity);
                    REQUIRE(path != nullptr);
                    auto &snapshot = result[{i, entity.role()}];
                    // Orca: The centered test model has one body on either side of X=0.
                    // Their traversal order may vary; preserve path order within each body.
                    Points points = path->polyline.to_polyline().points;
                    REQUIRE_FALSE(points.empty());
                    snapshot.paths[points.front().x() > 0].push_back(std::move(points));
                    snapshot.protected_order &= no_sort && !path->can_reverse();
                }
            };
            for (const LayerRegion *region : object.get_layer(i)->regions())
                collect(collect, region->fills, false);
        }
        return result;
    };

    Print print;
    Model model;
    init_print({mesh}, print, model, config, nullptr, false);
    print.process();
    const auto initial = surface_fills(print);
    config.set_deserialize_strict({{"center_of_surface_pattern", final_center}, {"separated_infills", separated}});
    print.apply(model, config);
    // Orca: Preparation owns the body origins, and its invalidation must also force
    // regeneration of top/bottom extrusion paths, even when sparse infill is unchanged.
    CHECK_FALSE(print.objects().front()->is_step_done(posPrepareInfill));
    CHECK_FALSE(print.objects().front()->is_step_done(posInfill));
    print.process();
    const auto resliced = surface_fills(print);

    Print fresh_print;
    Model fresh_model;
    init_print({mesh}, fresh_print, fresh_model, config, nullptr, false);
    fresh_print.process();
    const auto fresh = surface_fills(fresh_print);
    REQUIRE_FALSE(fresh.empty());
    REQUIRE(resliced.size() == fresh.size());
    std::set<ExtrusionRole> roles;
    bool changed_paths = false;
    for (const auto &entry : fresh) {
        CAPTURE(entry.first.first, entry.first.second);
        REQUIRE_FALSE(entry.second.paths.empty());
        roles.insert(entry.first.second);
        REQUIRE(resliced.count(entry.first) == 1);
        REQUIRE(initial.count(entry.first) == 1);
        const auto &actual = resliced.at(entry.first);
        const auto &expected = entry.second;
        const auto &before = initial.at(entry.first);
        CHECK((actual.paths == expected.paths));
        if (!change_center)
            CHECK((actual.paths == before.paths));
        if (top_order != "default") {
            CHECK(expected.protected_order);
            CHECK(actual.protected_order);
            CHECK(before.protected_order);
        }
        changed_paths |= expected.paths != before.paths;
    }
    CHECK(roles.count(erTopSolidInfill) == 1);
    CHECK(roles.count(erBottomSurface) == 1);
    // Orca: Guard against a vacuous comparison: changing surface centering must change
    // the printed pattern, while toggling separated sparse infill must leave it alone.
    CHECK(changed_paths == change_center);
}

TEST_CASE("Separated infill keeps fragmented and nested bodies independent", "[PrintObject][SurfaceInfill][Regression]")
{
    constexpr size_t grid_size = 8;
    TriangleMesh mesh;
    auto add_box = [&](double x, double y, double width, double depth) {
        TriangleMesh box = make_cube(width, depth, 0.6);
        box.translate(x, y, 0);
        mesh.merge(box);
    };
    // Orca: Many small islands exercise spatial pruning and the tree's original
    // island indices. A pillar inside a frame also overlaps its bounding box,
    // but must remain a separate body because it lies entirely inside the hole.
    for (size_t x = 0; x < grid_size; ++ x)
        for (size_t y = 0; y < grid_size; ++ y)
            add_box(6 * x, 6 * y, 3, 3);
    add_box(54, 0, 20, 4);
    add_box(54, 16, 20, 4);
    add_box(54, 0, 4, 20);
    add_box(70, 0, 4, 20);
    add_box(62, 8, 4, 4);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"separated_infills", true},
                                   {"center_of_surface_pattern", "each_surface"},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"elefant_foot_compensation", 0},
                                   {"wall_loops", 1}});
    Print print;
    Model model;
    init_print({mesh}, print, model, config, nullptr, false);
    // Orca: Prepare body bounds through the public pipeline, then inspect the object read-only.
    print.process();
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() > 1);
    for (const Layer *layer : object.layers()) {
        REQUIRE(layer->lslices.size() == grid_size * grid_size + 2);
        REQUIRE(layer->lslices_separated_component_bboxes.size() == layer->lslices.size());
        size_t holes = 0;
        for (size_t i = 0; i < layer->lslices.size(); ++ i) {
            const BoundingBox &body = layer->lslices_separated_component_bboxes[i];
            const BoundingBox &island = layer->lslices_bboxes[i];
            CHECK(body.min == island.min);
            CHECK(body.max == island.max);
            holes += layer->lslices[i].holes.size();
        }
        CHECK(holes == 1);
    }
}

TEST_CASE("Body centering survives islands merging and splitting between layers", "[PrintObject][SurfaceInfill][Regression]")
{
    const bool separated = GENERATE(false, true);
    CAPTURE(separated);
    // Orca: Four posts join through horizontal then vertical rails, creating a
    // cycle of overlaps before splitting into four islands again. This exercises
    // redundant connections and indexing either adjacent layer. A fifth post
    // stays separate at every height.
    TriangleMesh mesh;
    for (int x : {0, 8})
        for (int y : {0, 8}) {
            TriangleMesh post = make_cube(4, 4, 1);
            post.translate(x, y, 0);
            mesh.merge(post);
        }
    for (int y : {0, 8}) {
        TriangleMesh rail = make_cube(12, 4, 0.2);
        rail.translate(0, y, 0.2);
        mesh.merge(rail);
    }
    for (int x : {0, 8}) {
        TriangleMesh rail = make_cube(4, 12, 0.2);
        rail.translate(x, 0, 0.4);
        mesh.merge(rail);
    }
    TriangleMesh isolated = make_cube(4, 4, 1);
    isolated.translate(20, 0, 0);
    mesh.merge(isolated);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"separated_infills", separated},
                                   {"center_of_surface_pattern", separated ? "each_surface" : "each_model"},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"elefant_foot_compensation", 0},
                                   {"wall_loops", 1}});
    Print print;
    Model model;
    init_print({mesh}, print, model, config, nullptr, false);
    // Orca: Prepare body bounds through the public pipeline, then inspect the object read-only.
    print.process();
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() == 5);
    REQUIRE(object.get_layer(0)->lslices.size() == 5);
    REQUIRE(object.get_layer(1)->lslices.size() == 3);
    REQUIRE(object.get_layer(2)->lslices.size() == 3);
    REQUIRE(object.get_layer(4)->lslices.size() == 5);

    BoundingBox isolated_bbox = object.get_layer(0)->lslices_bboxes.front();
    for (const BoundingBox &bbox : object.get_layer(0)->lslices_bboxes)
        if (bbox.min.x() > isolated_bbox.min.x())
            isolated_bbox = bbox;
    BoundingBox connected_bbox;
    for (const Layer *layer : object.layers())
        for (const BoundingBox &bbox : layer->lslices_bboxes)
            if (bbox.min.x() < isolated_bbox.min.x())
                connected_bbox.merge(bbox);
    for (const Layer *layer : object.layers()) {
        REQUIRE(layer->lslices_separated_component_bboxes.size() == layer->lslices.size());
        for (size_t i = 0; i < layer->lslices.size(); ++ i) {
            const BoundingBox &expected = layer->lslices_bboxes[i].min.x() < isolated_bbox.min.x() ? connected_bbox : isolated_bbox;
            const BoundingBox &actual = layer->lslices_separated_component_bboxes[i];
            CHECK(actual.min == expected.min);
            CHECK(actual.max == expected.max);
        }
    }
}

// Config carrying just the filament-scope keys PrintObject::resolve_auto_support_filament reads.
static DynamicPrintConfig auto_support_config(std::vector<std::string>  types,
                                              std::vector<unsigned char> soluble,
                                              std::vector<std::string>  colours,
                                              std::vector<unsigned char> is_support = {})
{
    if (is_support.empty())
        is_support.assign(types.size(), false);
    DynamicPrintConfig config;
    config.set_key_value("filament_type",                  new ConfigOptionStrings(std::move(types)));
    config.set_key_value("filament_soluble",               new ConfigOptionBools(std::move(soluble)));
    config.set_key_value("filament_is_support",            new ConfigOptionBools(std::move(is_support)));
    config.set_key_value("filament_colour",                new ConfigOptionStrings(std::move(colours)));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    return config;
}

// A single-volume object printed with `extruder` (1-based).
static ModelObject& auto_support_object(Model &model, int extruder)
{
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    return *object;
}

TEST_CASE("Auto support filament picks a filament that does not bond to the object", "[PrintObject]")
{
    Model              model;
    const ModelObject &object = auto_support_object(model, 1); // printed in PLA

    SECTION("a non-bonding material is preferred over the object's own") {
        // PLA does not bond to PET, so the PETG filament is the one that detaches cleanly.
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG"}, {false, false}, {"#FFFFFF", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 2, config, true) == 2);
    }
    SECTION("a soluble filament wins over a merely non-bonding one") {
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG", "PVA"}, {false, false, true}, {"#FFFFFF", "#000000", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true) == 3);
    }
    SECTION("a support material wins over a plain filament of the same compatibility") {
        // Both candidates are PETG, so they are equally incompatible with the PLA object and only the support
        // flag separates them. Filament 2 has the object's exact colour, so colour alone would pick it.
        const bool               soluble = GENERATE(true, false); // flagged soluble, or flagged a support filament
        const DynamicPrintConfig config  = auto_support_config({"PLA", "PETG", "PETG"},
                                                              {false, false, soluble},
                                                              {"#FFFFFF", "#FFFFFF", "#000000"},
                                                              {false, false, !soluble});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true) == 3);
    }
    SECTION("the excluded filament is not picked") {
        // "Avoid interface filament for base" keeps the base off the interface's filament.
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG", "PVA"}, {false, false, true}, {"#FFFFFF", "#000000", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true, 3) == 2);
    }
    SECTION("only bonding filaments available falls back to the object's own filament") {
        // Both filaments are PLA, so no filament would detach: reuse the object's to avoid mixing colours.
        const DynamicPrintConfig config = auto_support_config({"PLA", "PLA"}, {false, false}, {"#FFFFFF", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 2, config, true) == 1);
    }
    SECTION("a single filament resolves to Default") {
        const DynamicPrintConfig config = auto_support_config({"PLA"}, {false}, {"#FFFFFF"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 1, config, true) == 0);
    }
    SECTION("support disabled resolves to Default") {
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG"}, {false, false}, {"#FFFFFF", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 2, config, false) == 0);
    }
}

TEST_CASE("Auto support filament makes a single-material plate use two filaments", "[PrintObject]")
{
    // Regression guard: the auto-picked interface filament has to show up in Print::extruders(), otherwise the
    // print looks single-filament and the prime tower gets normalized away.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "enable_support",             true },
        { "support_interface_filament", SUPPORT_FILAMENT_AUTO },
        // "Auto" only applies to printers with one nozzle per filament.
        { "single_extruder_multi_material", false },
        { "filament_type",              "PLA;PETG" },
        { "nozzle_diameter",            "0.4,0.4" },
        { "filament_diameter",          "1.75,1.75" },
        { "filament_soluble",           "0,0" },
        { "filament_colour",            "#FFFFFF;#000000" }
    });

    REQUIRE(print.objects().front()->config().support_interface_filament.value == 2);
    REQUIRE(print.extruders().size() == 2);
}

TEST_CASE("Auto support filament is re-resolved when the filament properties change", "[PrintObject]")
{
    // Regression guard: flagging a filament soluble makes Auto pick it, and clearing the flag again has to
    // hand the support back to the next best material instead of keeping the stale pick.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",                 true },
        { "support_interface_filament",     SUPPORT_FILAMENT_AUTO },
        { "single_extruder_multi_material", false },
        { "filament_type",                  "ABS;PLA;PETG" },
        { "nozzle_diameter",                "0.4,0.4,0.4" },
        { "filament_diameter",              "1.75,1.75,1.75" },
        // The object prints in ABS; the PLA is much closer to its colour than the PETG.
        { "filament_colour",                "#800080;#2F26A6;#C9C9C9" },
        { "filament_soluble",               "0,0,1" }
    });

    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, config);
    // Soluble beats a closer colour, so the PETG wins.
    REQUIRE(print.objects().front()->config().support_interface_filament.value == 3);

    config.set_deserialize_strict({ { "filament_soluble", "0,0,0" } });
    print.apply(model, config);
    // Both are now plain filaments, equally incompatible with the ABS object: the closer colour decides.
    REQUIRE(print.objects().front()->config().support_interface_filament.value == 2);
}
