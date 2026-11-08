#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <boost/filesystem.hpp>
#include <cstdlib>
#include <fstream>
#include <limits>

#include "test_helpers.hpp"

using namespace Slic3r;

// The segment support cascade.
//
// A segment's support terminates on its base plane, which is a cut face of its PARENT. Wherever the
// parent has no material under that contact, the load has nowhere to go, so each segment hands what
// it could not carry up to its parent as forced overhang. These tests read the demand off the
// PrintObjects directly rather than inferring it from G-code: the transfer is the behaviour under
// test, and G-code adds an un-rotation whose per-move Z is not a usable invariant for a tilted
// segment.

namespace {

DynamicPrintConfig cascade_config(const char *support_type = "normal(auto)", const char *support_style = "default")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",   "1" },
        { "print_sequence",     "by group" },
        { "layer_change_gcode", "G92 E0" },
        { "enable_support",     "1" },
        { "support_type",       support_type },
        { "support_style",      support_style },
    });
    return config;
}

const PrintObject *segment_with_index(const Slic3r::Print &print, int index)
{
    for (const PrintObject *object : print.objects())
        if (object->is_segment() && object->segment_index() == index)
            return object;
    return nullptr;
}

// Layers carrying inherited load of either kind: the child's support (support-on-support) or the
// child's body over this segment's empty space.
size_t layers_with_demand(const PrintObject *object)
{
    const std::vector<Polygons> &support = object->segment_support_demand();
    const std::vector<Polygons> &body    = object->segment_body_demand();
    size_t n = 0;
    for (size_t i = 0; i < std::max(support.size(), body.size()); ++ i)
        if ((i < support.size() && ! support[i].empty()) || (i < body.size() && ! body[i].empty()))
            ++ n;
    return n;
}

} // namespace

SCENARIO("Support demand cascades up a chain of segments", "[cutplanes][cascade]") {
    GIVEN("a sphere on a pillar, cut into three stacked segments") {
        // The sphere overhangs the pillar on every side, so the top segment cannot land all of its
        // support on its own floor. What it cannot carry must reach the root -- through the middle
        // segment, which is the part a single-hop cascade would get wrong.
        Slic3r::Print print;
        Model         model;
        ModelObject  *object = model.add_object();
        object->name = "sphere_on_pillar";
        ModelVolume  *cube   = object->add_volume(Test::cube(20.),
                                                  ModelVolumeType::MODEL_PART, false);
        ModelVolume  *sphere = object->add_volume(Test::mesh(Test::TestMesh::sphere_50mm),
                                                  ModelVolumeType::MODEL_PART, false);
        const BoundingBoxf3 cb = cube->mesh().bounding_box();
        const BoundingBoxf3 sb = sphere->mesh().bounding_box();
        sphere->set_offset(Vec3d(cb.center().x() - sb.center().x(),
                                 cb.center().y() - sb.center().y(),
                                 cb.max.z()      - sb.min.z()));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));

        CutPlaneStack stack;
        stack.enabled = true;
        for (double z : { 7., 14. }) {
            CutPlane cp;
            cp.point  = Vec3d(cb.center().x(), cb.center().y(), z);
            cp.normal = Vec3d::UnitZ();
            stack.planes.push_back(cp);
        }
        object->cut_planes = stack;
        object->ensure_on_bed();

        print.apply(model, cascade_config());
        REQUIRE(print.objects().size() == 3);
        print.process();

        const PrintObject *root = segment_with_index(print, 0);
        const PrintObject *mid  = segment_with_index(print, 1);
        const PrintObject *top  = segment_with_index(print, 2);
        REQUIRE(root != nullptr);
        REQUIRE(mid  != nullptr);
        REQUIRE(top  != nullptr);

        THEN("the chain is root -> mid -> top") {
            CHECK(root->is_root_segment());
            CHECK(mid->segment()->parent == 0);
            CHECK(top->segment()->parent == 1);
        }

        THEN("demand reaches the root through the middle segment") {
            // The topmost segment is a leaf: nothing hands anything to it.
            CHECK(layers_with_demand(top) == 0);
            INFO("mid layers with demand: "  << layers_with_demand(mid)
                 << ", root layers with demand: " << layers_with_demand(root));
            CHECK(layers_with_demand(mid)  > 0);
            CHECK(layers_with_demand(root) > 0);
        }

        THEN("every segment's demand is indexed within its own layer range") {
            // Demand is consumed by index against object layers; an entry past the end would be
            // silently ignored by the generator rather than reported.
            for (const PrintObject *segment : { root, mid }) {
                INFO("segment " << segment->segment_index());
                CHECK(segment->segment_support_demand().size() <= segment->layer_count());
            }
        }

        THEN("the parent actually generates support from the inherited load") {
            // Demand that never becomes support would satisfy the checks above and still be inert.
            CHECK(! root->support_layers().empty());
        }

        THEN("what reaches the root is the top segment's support, handed down as support-on-support") {
            // The sphere starts well above the top segment's floor, so its overhang is carried by
            // the top segment's own support, which ends on the plane; the body footprint at that
            // floor is the pillar's cross-section, resting on material all the way down. The two
            // kinds are kept apart because only support-on-support may end flush with the plane.
            size_t body = 0, support = 0;
            for (const Polygons &layer : root->segment_body_demand())    body    += ! layer.empty();
            for (const Polygons &layer : root->segment_support_demand()) support += ! layer.empty();
            INFO("root body-demand layers " << body << ", support-demand layers " << support);
            CHECK(support > 0);
            CHECK(body    == 0);
        }
    }
}

SCENARIO("A segment floating above its base plane is supported", "[cutplanes][cascade]") {
    GIVEN("two disconnected shells with a cut plane through the gap between them") {
        // The plane passes through empty space, so it produces no cut face and the upper segment's
        // first layer is suspended in mid-air. The stock generators treat layer 0 as resting on the
        // ground, so without explicit seeding nothing supports it -- and every ordinary
        // touching-plane fixture would still pass.
        Slic3r::Print print;
        Model         model;
        ModelObject  *object = model.add_object();
        object->name = "two_shells";
        ModelVolume  *lower = object->add_volume(Test::cube(20.),
                                                 ModelVolumeType::MODEL_PART, false);
        ModelVolume  *upper = object->add_volume(Test::cube(20.),
                                                 ModelVolumeType::MODEL_PART, false);
        // Lower shell spans z 0..20; upper shell spans z 30..50, leaving a 10mm gap.
        lower->set_offset(Vec3d(0., 0., 0.));
        upper->set_offset(Vec3d(0., 0., 30.));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));

        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        cp.point  = Vec3d(10., 10., 25.);       // in the gap: touches neither shell
        cp.normal = Vec3d::UnitZ();
        stack.planes.push_back(cp);
        object->cut_planes = stack;
        object->ensure_on_bed();

        print.apply(model, cascade_config());
        REQUIRE(print.objects().size() == 2);
        print.process();

        const PrintObject *root  = segment_with_index(print, 0);
        const PrintObject *child = segment_with_index(print, 1);
        REQUIRE(root  != nullptr);
        REQUIRE(child != nullptr);

        THEN("the child is recognised as floating, not resting, on its base plane") {
            CHECK_FALSE(child->segment()->touches_base_plane());
            INFO("gap below the child's material: " << child->segment()->base_plane_distance_below_material);
            CHECK(child->segment()->base_plane_distance_below_material > 1.);
        }

        THEN("the floating first layer is supported down to its base plane") {
            // The child's suspended first layer is seeded as demand and carried down to the base
            // plane -- the cut face it nominally rests on.
            REQUIRE(! child->support_layers().empty());
            double lowest = std::numeric_limits<double>::max();
            for (const SupportLayer *layer : child->support_layers())
                if (layer != nullptr && (! layer->support_islands.empty() || ! layer->base_areas.empty()))
                    lowest = std::min(lowest, layer->print_z);
            INFO("lowest support layer in the child's frame: " << lowest << " (base plane at -5)");
            CHECK(lowest < -4.);
        }

        THEN("the root carries the column across the open span below the plane") {
            // The plane sits 5mm above the root's material. The child's support ends at the plane,
            // so the root has to build the rest -- above its own material, in layers that exist
            // only for that (segment_slice_top()). The column is carved against the root's top
            // face, so once it reaches material it stops asking.
            INFO("root layers with demand: " << layers_with_demand(root));
            CHECK(layers_with_demand(root) > 0);
            double root_material_top = 0., root_support_top = 0.;
            for (const Layer *layer : root->layers())
                if (! layer->lslices.empty())
                    root_material_top = std::max(root_material_top, layer->print_z);
            for (const SupportLayer *layer : root->support_layers())
                if (! layer->support_islands.empty() || ! layer->base_areas.empty())
                    root_support_top = std::max(root_support_top, layer->print_z);
            INFO("root material top " << root_material_top << ", support top " << root_support_top);
            CHECK(root_support_top > root_material_top + 3.);
        }
    }
}

SCENARIO("Tree support styles cascade and stay inside their segment", "[cutplanes][cascade][tree]") {
    // Both tree generators reach the segment through different code than normal support: classic
    // injects the demand band in detect_overhangs and clips its area vectors before area_groups is
    // built, organic seeds tips before precalculate and clips the assembled layers. Exporting the
    // G-code is part of the test on purpose: the classic clip has a hard ordering constraint --
    // area_groups holds raw ExPolygon pointers into the very vectors being clipped, so clipping
    // after it is built dangles them and crashes inside generate_toolpaths, which is reached only
    // by actually producing toolpaths.
    struct Style { const char *name; const char *type; const char *style; };
    const std::vector<Style> styles = {
        { "classic tree", "tree(auto)", "tree_hybrid" },
        { "organic tree", "tree(auto)", "organic"     },
    };

    for (const Style &s : styles) {
        DYNAMIC_SECTION("style " << s.name) {
            Slic3r::Print print;
            Model         model;
            ModelObject  *object = model.add_object();
            object->name = "sphere_on_cube";
            ModelVolume  *cube   = object->add_volume(Test::cube(20.),
                                                      ModelVolumeType::MODEL_PART, false);
            ModelVolume  *sphere = object->add_volume(Test::mesh(Test::TestMesh::sphere_50mm),
                                                      ModelVolumeType::MODEL_PART, false);
            const BoundingBoxf3 cb = cube->mesh().bounding_box();
            const BoundingBoxf3 sb = sphere->mesh().bounding_box();
            sphere->set_offset(Vec3d(cb.center().x() - sb.center().x(),
                                     cb.center().y() - sb.center().y(),
                                     cb.max.z()      - sb.min.z()));
            object->add_instance()->set_offset(Vec3d(100., 100., 0.));

            CutPlaneStack stack;
            stack.enabled = true;
            CutPlane cp;
            cp.point  = Vec3d(cb.center().x(), cb.center().y(), 10.);
            cp.normal = Vec3d::UnitZ();
            stack.planes.push_back(cp);
            object->cut_planes = stack;
            object->ensure_on_bed();

            print.apply(model, cascade_config(s.type, s.style));
            REQUIRE(print.objects().size() == 2);
            REQUIRE_NOTHROW(print.process());

            const PrintObject *root  = segment_with_index(print, 0);
            const PrintObject *child = segment_with_index(print, 1);
            REQUIRE(root  != nullptr);
            REQUIRE(child != nullptr);

            THEN("the child's load reaches the root and both segments carry support") {
                INFO("root layers with demand: " << layers_with_demand(root));
                CHECK(layers_with_demand(root) > 0);
                CHECK(! child->support_layers().empty());
                CHECK(! root->support_layers().empty());
            }

            THEN("toolpaths generate without dangling the area_groups aliases") {
                const auto out_path = boost::filesystem::temp_directory_path()
                                    / boost::filesystem::unique_path("cutplanes-tree-%%%%%%%%.gcode");
                GCodeProcessorResult result;
                REQUIRE_NOTHROW(print.export_gcode(out_path.string(), &result, nullptr));
                std::ifstream in(out_path.string());
                const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close(); // Windows refuses to remove a file that is still open
                boost::filesystem::remove(out_path);
                REQUIRE(! gcode.empty());
                CHECK(gcode.find(";TYPE:Support") != std::string::npos);
            }
        }
    }
}

SCENARIO("Editing a segmented object does not regenerate unrelated objects", "[cutplanes][cascade]") {
    // What can and cannot be asserted here is bounded by how Print::apply works on this fork. A
    // cut-plane edit is treated as a solid-geometry change and invalidates every PrintObject of
    // that ModelObject -- and since every splitting plane becomes a clip on every segment, "move
    // one plane" is inherently a family-wide edit, not a per-segment one. So the honest acceptance
    // rule is: the affected family may regenerate, unrelated ModelObjects must not. Regeneration is
    // read from the step's timestamp, because equal output would not prove a step did not re-run.
    GIVEN("a segmented object and an unrelated plain object on the same plate") {
        Slic3r::Print print;
        Model         model;

        ModelObject *segmented = model.add_object();
        segmented->name = "segmented";
        segmented->add_volume(Test::cube(20.));
        segmented->add_instance()->set_offset(Vec3d(80., 80., 0.));
        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        cp.point  = Vec3d(10., 10., 10.);
        cp.normal = Vec3d::UnitZ();
        stack.planes.push_back(cp);
        segmented->cut_planes = stack;
        segmented->ensure_on_bed();

        ModelObject *plain = model.add_object();
        plain->name = "plain";
        plain->add_volume(Test::cube(20.));
        plain->add_instance()->set_offset(Vec3d(160., 160., 0.));
        plain->ensure_on_bed();

        const DynamicPrintConfig config = cascade_config();
        print.apply(model, config);
        print.process();

        auto plain_object = [&print]() -> const PrintObject* {
            for (const PrintObject *object : print.objects())
                if (! object->is_segment())
                    return object;
            return nullptr;
        };
        const PrintObject *plain_po = plain_object();
        REQUIRE(plain_po != nullptr);
        const auto plain_before = plain_po->step_state_with_timestamp(posSupportMaterial);
        REQUIRE(plain_before.state == PrintStateBase::DONE);

        WHEN("a cut plane on the segmented object is moved") {
            model.objects.front()->cut_planes.planes[0].point.z() = 13.;
            print.apply(model, config);
            print.process();

            THEN("the unrelated object's support is not regenerated") {
                const PrintObject *plain_after_po = plain_object();
                REQUIRE(plain_after_po != nullptr);
                // Same PrintObject, same step, same timestamp: it was never re-run.
                CHECK(plain_after_po == plain_po);
                const auto plain_after = plain_after_po->step_state_with_timestamp(posSupportMaterial);
                CHECK(plain_after.state == PrintStateBase::DONE);
                CHECK(plain_after.timestamp == plain_before.timestamp);
            }

            THEN("the segmented family still slices and supports correctly") {
                const PrintObject *root = segment_with_index(print, 0);
                REQUIRE(root != nullptr);
                CHECK(! root->layers().empty());
                // The root's MATERIAL ends at the cut; a few empty layers may follow it (support
                // reach, see segment_slice_top()).
                double root_material_top = 0.;
                for (const Layer *layer : root->layers())
                    if (! layer->lslices.empty())
                        root_material_top = std::max(root_material_top, layer->print_z);
                CHECK_THAT(root_material_top, Catch::Matchers::WithinAbs(13., 0.3));
                CHECK(root->layers().back()->print_z <= 14.);
            }
        }
    }
}

SCENARIO("Parent material permanently stops an inherited demand column", "[cutplanes][cascade]") {
    GIVEN("a parent that catches the child's overhang on a shelf and narrows again below it") {
        // Stack, bottom to top: a narrow foot, a wide shelf, a narrow neck, then the child.
        //
        //        ####### child (wide, overhangs the neck)
        //   - - -|-----|- - -  cut plane
        //          | |         neck   (narrow)
        //        |-----|       shelf  (wide)  <- catches the overhang column
        //          | |         foot   (narrow)
        //
        // The child's overhang has no material under it at the interface, so it enters the demand
        // column and descends. At the shelf it lands on solid parent material and is caught. Below
        // the shelf the parent narrows again, and the question is whether the caught load stays
        // caught: carving only the per-layer output leaves the accumulated column intact, so demand
        // reappears beside the foot and asks for support under load the shelf already carries.
        Slic3r::Print print;
        Model         model;
        ModelObject  *object = model.add_object();
        object->name = "shelf_stack";
        auto add_box = [object](double sx, double sy, double sz, const Vec3d &offset) {
            ModelVolume *v = object->add_volume(TriangleMesh(its_make_cube(sx, sy, sz)),
                                               ModelVolumeType::MODEL_PART, false);
            v->set_offset(offset);
            return v;
        };
        // Centred on (15,15) so every piece shares an axis.
        add_box(10., 10., 3.,  Vec3d(10., 10.,  0.));   // foot   z 0..3
        add_box(30., 30., 2.,  Vec3d( 0.,  0.,  3.));   // shelf  z 3..5
        add_box(10., 10., 5.,  Vec3d(10., 10.,  5.));   // neck   z 5..10
        add_box(30., 30., 10., Vec3d( 0.,  0., 10.));   // child  z 10..20
        object->add_instance()->set_offset(Vec3d(120., 120., 0.));

        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        cp.point  = Vec3d(15., 15., 10.);
        cp.normal = Vec3d::UnitZ();
        stack.planes.push_back(cp);
        object->cut_planes = stack;
        object->ensure_on_bed();

        print.apply(model, cascade_config());
        REQUIRE(print.objects().size() == 2);
        print.process();

        const PrintObject *root = segment_with_index(print, 0);
        REQUIRE(root != nullptr);

        THEN("the child's overhang produces demand above the shelf") {
            // Non-vacuity: without demand here the assertion below proves nothing.
            INFO("root layers with demand: " << layers_with_demand(root));
            REQUIRE(layers_with_demand(root) > 0);
        }

        THEN("no demand survives below the shelf that caught it") {
            // The child's overhang past the shelf is its BODY over the root's empty space.
            size_t offending = 0;
            double lowest_with_demand = std::numeric_limits<double>::max();
            for (const std::vector<Polygons> *demand : { &root->segment_body_demand(), &root->segment_support_demand() })
                for (size_t i = 0; i < demand->size() && i < root->layer_count(); ++ i) {
                    if ((*demand)[i].empty())
                        continue;
                    const double z = root->get_layer(int(i))->print_z;
                    lowest_with_demand = std::min(lowest_with_demand, z);
                    if (z < 2.5)        // below the shelf (z 3..5), where only the narrow foot remains
                        ++ offending;
                }
            INFO("lowest layer carrying demand: " << lowest_with_demand
                 << ", layers with demand below the shelf: " << offending);
            CHECK(offending == 0);
        }
    }
}

SCENARIO("Tree support descends to a floating segment's base plane", "[cutplanes][cascade][tree]") {
    // The tree generators' node graphs are indexed by object layer and stop at layer 0, so branches
    // could never reach a floor below that -- and a floating segment's base plane is below it
    // everywhere. Their feet are now carried down as a column, seeded with the layer-0 demand the
    // node graphs cannot consume, so the two failures are fixed together: without either half this
    // fixture produces no support below the segment's own zero at all.
    struct Style { const char *name; const char *type; const char *style; };
    const std::vector<Style> styles = {
        { "classic tree", "tree(auto)", "tree_hybrid" },
        { "organic tree", "tree(auto)", "organic"     },
        { "normal",       "normal(auto)", "default"   },   // already worked; guards against regressing it
    };

    for (const Style &s : styles) {
        DYNAMIC_SECTION("style " << s.name) {
            Slic3r::Print print;
            Model         model;
            ModelObject  *object = model.add_object();
            object->name = "two_shells";
            ModelVolume  *lower = object->add_volume(Test::cube(20.),
                                                     ModelVolumeType::MODEL_PART, false);
            ModelVolume  *upper = object->add_volume(Test::cube(20.),
                                                     ModelVolumeType::MODEL_PART, false);
            lower->set_offset(Vec3d(0., 0., 0.));    // z 0..20
            upper->set_offset(Vec3d(0., 0., 30.));   // z 30..50, a 10mm gap
            object->add_instance()->set_offset(Vec3d(100., 100., 0.));

            CutPlaneStack stack;
            stack.enabled = true;
            CutPlane cp;
            cp.point  = Vec3d(10., 10., 25.);        // in the gap, 5mm under the upper shell
            cp.normal = Vec3d::UnitZ();
            stack.planes.push_back(cp);
            object->cut_planes = stack;
            object->ensure_on_bed();

            print.apply(model, cascade_config(s.type, s.style));
            REQUIRE(print.objects().size() == 2);
            REQUIRE_NOTHROW(print.process());

            const PrintObject *child = segment_with_index(print, 1);
            REQUIRE(child != nullptr);
            REQUIRE_FALSE(child->segment()->touches_base_plane());

            THEN("its support reaches down to the base plane, below the segment's own zero") {
                REQUIRE(! child->support_layers().empty());
                // Report creation and extrusion separately: a layer can be installed below zero and
                // still emit nothing if toolpath generation cannot pair it with anything.
                double lowest_any = std::numeric_limits<double>::max();
                double lowest_fill = std::numeric_limits<double>::max();
                size_t below_zero = 0, below_zero_with_fills = 0;
                for (const SupportLayer *layer : child->support_layers()) {
                    if (layer == nullptr)
                        continue;
                    const bool has_area = ! layer->support_islands.empty() || ! layer->base_areas.empty();
                    const bool has_fill = ! layer->support_fills.empty();
                    if (has_area || has_fill)
                        lowest_any = std::min(lowest_any, layer->print_z);
                    if (has_fill)
                        lowest_fill = std::min(lowest_fill, layer->print_z);
                    if (layer->print_z < -EPSILON) {
                        ++ below_zero;
                        if (has_fill)
                            ++ below_zero_with_fills;
                    }
                }
                INFO("support layers below zero: " << below_zero << " (with extrusions: " << below_zero_with_fills
                     << "), lowest layer with geometry: " << lowest_any
                     << ", lowest with extrusions: " << lowest_fill << " -- base plane sits at -5");
                CHECK(below_zero > 0);
                CHECK(below_zero_with_fills > 0);
            }
        }
    }
}

namespace {
// Lowest support layer of `object` with extrusions, and how many such layers sit below its own zero.
std::pair<double, size_t> lowest_support_fill(const PrintObject &object)
{
    double lowest = std::numeric_limits<double>::max();
    size_t below  = 0;
    for (const SupportLayer *layer : object.support_layers())
        if (layer != nullptr && ! layer->support_fills.empty()) {
            lowest = std::min(lowest, layer->print_z);
            if (layer->print_z < - EPSILON)
                ++ below;
        }
    return { lowest, below };
}
} // namespace

SCENARIO("A root lifted off the plate is supported down to the plate", "[cutplanes][cascade][floating_root]") {
    // 2026-11-03, sample.3MF: "supports still dont work on parts floating off the build plate".
    // An object with auto-drop off, 8.6 mm above the plate, with cut planes: its root segment slices
    // from its own bottom and keeps the plate at a negative slice z, but nothing treated that first
    // layer as suspended -- the cascade seeded floating NON-root segments only, the natural scan
    // skipped layer 0, and the tree's descent ran for tilted floors only -- so the whole print,
    // support included, started 8.6 mm in the air.
    struct Style { const char *name; const char *type; const char *style; };
    const std::vector<Style> styles = {
        { "classic tree", "tree(auto)",   "tree_hybrid" },
        { "organic tree", "tree(auto)",   "organic"     },
        { "normal",       "normal(auto)", "default"     },
    };
    const double lift = 8.;
    for (const Style &s : styles)
        for (const char *axes : { "bc", "virtual" }) {
            DYNAMIC_SECTION("style " << s.name << ", axes " << axes) {
                Slic3r::Print print;
                Model         model;
                ModelObject  *object = model.add_object();
                object->name = "lifted_cube";
                object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
                object->add_instance()->set_offset(Vec3d(100., 100., lift));   // auto_drop is off: stays up

                CutPlaneStack stack;
                stack.enabled = true;
                CutPlane cp;
                cp.point  = Vec3d(10., 10., 10.);
                cp.normal = Vec3d::UnitZ();
                stack.planes.push_back(cp);
                object->cut_planes = stack;

                DynamicPrintConfig config = cascade_config(s.type, s.style);
                config.set_deserialize_strict({ { "axis_selection", axes } });
                print.apply(model, config);
                REQUIRE_NOTHROW(print.process());

                const PrintObject *root = segment_with_index(print, 0);
                REQUIRE(root != nullptr);
                REQUIRE(root->is_root_segment());
                REQUIRE(root->root_floats_above_plate());

                THEN("its support reaches the plate, a lift below the root's own zero") {
                    const auto [lowest, below] = lowest_support_fill(*root);
                    INFO("lowest support layer with extrusions: " << lowest << ", layers below zero: " << below
                         << " -- the plate sits at " << -lift);
                    CHECK(below > 0);
                    CHECK(lowest < - lift + 0.5);
                }
            }
        }
}

SCENARIO("A root resting on the plate does not float", "[cutplanes][cascade][floating_root]") {
    Slic3r::Print print;
    Model         model;
    ModelObject  *object = model.add_object();
    object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane cp;
    cp.point  = Vec3d(10., 10., 10.);
    cp.normal = Vec3d::UnitZ();
    stack.planes.push_back(cp);
    object->cut_planes = stack;
    print.apply(model, cascade_config("tree(auto)", "tree_hybrid"));
    REQUIRE_NOTHROW(print.process());
    const PrintObject *root = segment_with_index(print, 0);
    REQUIRE(root != nullptr);
    CHECK_FALSE(root->root_floats_above_plate());
    // A cube on the plate needs no support at all.
    CHECK(lowest_support_fill(*root).second == 0);
}

// Hidden: slice a real project with its own settings and supports on, and report each segment's
// lowest support. UNLAYERED_REPRO_3MF=<path> fff_print_tests "[.repro_floating]"
SCENARIO("Repro: floating project support per segment", "[.repro_floating]") {
    const char *path = std::getenv("UNLAYERED_REPRO_3MF");
    if (path == nullptr)
        return;
    DynamicPrintConfig        config = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext subs(ForwardCompatibilitySubstitutionRule::Enable);
    Model model = Model::read_from_file(path, &config, &subs, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    config.set_deserialize_strict({ { "enable_support", "1" }, { "layer_change_gcode", "G92 E0" } });
    Slic3r::Print print;
    print.apply(model, config);
    print.process();
    for (const PrintObject *object : print.objects()) {
        const auto [lowest, below] = lowest_support_fill(*object);
        WARN("segment " << (object->is_segment() ? object->segment_index() : -1)
             << (object->is_root_segment() ? " (root)" : "") << " floats_root=" << object->root_floats_above_plate()
             << " slice_z_base=" << (object->is_segment() ? object->segment()->slice_z_base() : 0.)
             << " lowest support fill " << lowest << " layers below zero " << below);
    }
}

SCENARIO("A column crossing a sibling's slab is handed to that sibling, not only to the parent", "[cutplanes][cascade]") {
    GIVEN("a sphere on a pillar cut by two planes through one point, so a wedge sits between them") {
        // Both planes pass through the same point in the pillar. Between them lies a thin wedge
        // whose ceiling is the steeper plane -- the same plane the top segment rests on. The top
        // segment's parent is the ROOT (its base plane mostly borders the root), so a cascade
        // that hands the column only to the parent skips the wedge: the root clips its support
        // at its ceilings, which is exactly the wedge's slab, and the sphere's overhang above
        // the wedge's empty space is left unsupported there.
        Slic3r::Print print;
        Model         model;
        ModelObject  *object = model.add_object();
        object->name = "wedge_between_planes";
        ModelVolume  *cube   = object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
        ModelVolume  *sphere = object->add_volume(Test::mesh(Test::TestMesh::sphere_50mm), ModelVolumeType::MODEL_PART, false);
        const BoundingBoxf3 cb = cube->mesh().bounding_box();
        const BoundingBoxf3 sb = sphere->mesh().bounding_box();
        sphere->set_offset(Vec3d(cb.center().x() - sb.center().x(),
                                 cb.center().y() - sb.center().y(),
                                 cb.max.z()      - sb.min.z()));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));

        // The planes cross 5mm off the pillar's centre, on the -X side, and the stack is user
        // ordered so the flat plane peels first: the wedge is then the -X sliver above the flat
        // plane and below the steep one, and the top segment is everything above the steep plane.
        // The steep plane's cut face borders the root over the wider +X part, so the ROOT is the
        // top's parent. The sphere overhangs the wedge's empty slab beyond the pillar, so the top's
        // support has to be carried through the wedge.
        CutPlaneStack stack;
        stack.enabled      = true;
        stack.user_ordered = true;
        const Vec3d through(cb.center().x() - 5., cb.center().y(), 10.);
        for (const Vec3d &n : { Vec3d(0.42, 0., 0.91), Vec3d(-0.10, 0., 0.995) }) {   // first peels LAST
            CutPlane cp;
            cp.point  = through;
            cp.normal = n.normalized();
            stack.planes.push_back(cp);
        }
        object->cut_planes = stack;
        object->ensure_on_bed();

        print.apply(model, cascade_config());
        REQUIRE(print.objects().size() == 3);
        print.process();

        // The wedge is the non-root segment bounded by BOTH a floor and a ceiling.
        const PrintObject *wedge = nullptr;
        const PrintObject *top   = nullptr;
        for (const PrintObject *segment : print.objects()) {
            if (segment->is_root_segment())
                continue;
            bool has_ceiling = false;
            for (const CutSegmentClip &clip : segment->segment()->clips)
                has_ceiling |= clip.role == CutClipRole::Ceiling;
            (has_ceiling ? wedge : top) = segment;
        }
        REQUIRE(wedge != nullptr);
        REQUIRE(top   != nullptr);

        THEN("the wedge is a sibling of the top segment, both resting on the root") {
            INFO("wedge index " << wedge->segment_index() << " parent " << wedge->segment()->parent
                 << ", top index " << top->segment_index() << " parent " << top->segment()->parent);
            CHECK(wedge->segment()->parent == 0);
            CHECK(top->segment()->parent   == 0);
            CHECK(wedge->segment_index()   <  top->segment_index());
        }

        THEN("the top segment's load reaches the wedge as well as the root") {
            INFO("wedge layers with demand: " << layers_with_demand(wedge));
            CHECK(layers_with_demand(wedge) > 0);
            CHECK(layers_with_demand(segment_with_index(print, 0)) > 0);
        }

        THEN("the wedge actually generates support from that load") {
            CHECK(! wedge->support_layers().empty());
        }
    }
}

SCENARIO("Section support overlap lets a receiver's column reach past the plane", "[cutplanes][cascade]") {
    GIVEN("the wedge fixture sliced with and without a one-layer overlap") {
        auto build = [](double overlap, Slic3r::Print &print, Model &model) {
            ModelObject  *object = model.add_object();
            object->name = "wedge_between_planes";
            ModelVolume  *cube   = object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
            ModelVolume  *sphere = object->add_volume(Test::mesh(Test::TestMesh::sphere_50mm), ModelVolumeType::MODEL_PART, false);
            const BoundingBoxf3 cb = cube->mesh().bounding_box();
            const BoundingBoxf3 sb = sphere->mesh().bounding_box();
            sphere->set_offset(Vec3d(cb.center().x() - sb.center().x(), cb.center().y() - sb.center().y(), cb.max.z() - sb.min.z()));
            object->add_instance()->set_offset(Vec3d(100., 100., 0.));
            CutPlaneStack stack;
            stack.enabled      = true;
            stack.user_ordered = true;
            const Vec3d through(cb.center().x() - 5., cb.center().y(), 10.);
            for (const Vec3d &n : { Vec3d(0.42, 0., 0.91), Vec3d(-0.10, 0., 0.995) }) {
                CutPlane cp; cp.point = through; cp.normal = n.normalized(); stack.planes.push_back(cp);
            }
            object->cut_planes = stack;
            object->ensure_on_bed();
            DynamicPrintConfig config = cascade_config();
            config.set_deserialize_strict({ { "support_section_overlap", overlap } });
            print.apply(model, config);
            REQUIRE(print.objects().size() == 3);
            print.process();
        };
        // The highest support in the wedge relative to its own ceiling-bound layers: with the
        // overlap the column is allowed to end one layer higher, so its top rises by about a layer.
        auto wedge_support_top = [](const Slic3r::Print &print) {
            double top = -1e9;
            for (const PrintObject *segment : print.objects()) {
                if (segment->is_root_segment()) continue;
                bool has_ceiling = false;
                for (const CutSegmentClip &clip : segment->segment()->clips) has_ceiling |= clip.role == CutClipRole::Ceiling;
                if (! has_ceiling) continue;
                for (const SupportLayer *layer : segment->support_layers())
                    if (! layer->support_islands.empty())
                        top = std::max(top, layer->print_z);
            }
            return top;
        };
        Slic3r::Print plain, overlapped;
        Model         model_plain, model_overlapped;
        build(0.,  plain,      model_plain);
        build(0.2, overlapped, model_overlapped);
        const double top_plain = wedge_support_top(plain), top_overlapped = wedge_support_top(overlapped);
        INFO("wedge support top: plain " << top_plain << ", with 0.2 overlap " << top_overlapped);
        REQUIRE(top_plain > -1e8);
        THEN("the wedge's column ends higher with the overlap, by no more than the overlap plus a layer") {
            CHECK(top_overlapped >= top_plain + 0.15);
            CHECK(top_overlapped <= top_plain + 0.45);
        }
    }
}
