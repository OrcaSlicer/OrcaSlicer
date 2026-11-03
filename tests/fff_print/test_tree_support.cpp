#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include <cstddef>
#include "libslic3r/libslic3r.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ExPolygon.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

// The upper plate overhangs both the lower plate and open air, so branches land on the model and on
// the bed in the same slice.
TriangleMesh two_tier_mesh()
{
    TriangleMesh lower  = make_cube(30, 30, 3);
    TriangleMesh column = make_cube(8, 8, 15);
    TriangleMesh upper  = make_cube(50, 50, 3);
    // Each part overlaps the one below rather than resting on it; a coplanar join slices ambiguously.
    column.translate(11.f, 11.f, 2.f);
    upper.translate(-10.f, -10.f, 16.f);
    TriangleMesh mesh = lower;
    mesh.merge(column);
    mesh.merge(upper);
    return mesh;
}

TriangleMesh scaled(TestMesh id, float scale)
{
    TriangleMesh mesh = Slic3r::Test::mesh(id);
    mesh.scale(scale);
    return mesh;
}

// `extra` is applied last, so a caller can add or override any key.
void slice_with_tree_support(const TriangleMesh &mesh, Slic3r::Print &print, const char *style,
                             int threshold_angle = 30, int build_plate_only = 0, int raft_layers = 0,
                             std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                "tree(auto)" },
        { "support_style",               style },
        { "support_on_build_plate_only", build_plate_only },
        { "support_threshold_angle",     threshold_angle },
        { "raft_layers",                 raft_layers },
        { "layer_height",                0.2 },
    });
    config.set_deserialize_strict(extra);
    Slic3r::Test::init_and_process_print({ mesh }, print, config);
}

Points support_points(const Slic3r::Print &print)
{
    Points points;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        layer->support_fills.collect_points(points);
    return points;
}

size_t support_point_count(const TriangleMesh &mesh, const char *style, int threshold_angle = 30,
                           int build_plate_only = 0)
{
    Slic3r::Print print;
    slice_with_tree_support(mesh, print, style, threshold_angle, build_plate_only);
    return support_points(print).size();
}

// Index of the first differing point, or the common length when they match. An index keeps a
// failure readable; comparing the vectors themselves dumps thousands of points.
size_t first_difference(const Points &a, const Points &b)
{
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i)
        if (a[i] != b[i])
            return i;
    return common;
}

// Slice `mesh` twice and require an identical support point sequence. Point counts and total
// length are order insensitive, so the sequence is what a reordering shows up in.
void sliced_twice_matches(const TriangleMesh &mesh, int build_plate_only, const char *style = "tree_slim",
                          std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    Slic3r::Print first_print, second_print;
    slice_with_tree_support(mesh, first_print, style, 30, build_plate_only, 0, extra);
    slice_with_tree_support(mesh, second_print, style, 30, build_plate_only, 0, extra);
    const Points first  = support_points(first_print);
    const Points second = support_points(second_print);
    REQUIRE(first.size() > 1000); // without support the comparison below passes vacuously
    REQUIRE(second.size() == first.size());
    REQUIRE(first_difference(first, second) == first.size());
}

} // namespace

TEST_CASE("Tree support is generated for an overhang and not for a plain cube", "[TreeSupport]")
{
    REQUIRE(support_point_count(scaled(TestMesh::overhang, 2.f), "tree_slim") > 1000);
    REQUIRE(support_point_count(Slic3r::Test::cube(20), "tree_slim") == 0);
}

TEST_CASE("Restricting tree support to the build plate changes what is generated", "[TreeSupport]")
{
    const TriangleMesh mesh = two_tier_mesh();
    const size_t anywhere    = support_point_count(mesh, "tree_slim", 30, 0);
    const size_t plate_only  = support_point_count(mesh, "tree_slim", 30, 1);
    REQUIRE(anywhere > 1000);
    REQUIRE(plate_only > 1000);
    // The upper plate overhangs the lower one, so some branches would land on the model.
    REQUIRE(plate_only != anywhere);
}

TEST_CASE("Tree support layers rise monotonically within the layer height limits", "[TreeSupport]")
{
    Slic3r::Print print;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), print, "tree_slim");
    const double nozzle = print.config().nozzle_diameter.values.front();

    size_t checked = 0;
    double previous = 0;
    bool   previous_was_adjacent = false;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        if (layer->print_z <= 0 || layer->height <= 0) {
            // Layers with no nodes are left at zero. Skipping one leaves a hole, so the next pair
            // spans more than one layer and its gap says nothing about the layer height limit.
            previous_was_adjacent = false;
            continue;
        }
        if (previous > 0) {
            CAPTURE(previous, layer->print_z);
            REQUIRE(layer->print_z > previous);
            if (previous_was_adjacent)
                REQUIRE(layer->print_z - previous <= nozzle + EPSILON);
        }
        previous = layer->print_z;
        previous_was_adjacent = true;
        ++checked;
    }
    REQUIRE(checked > 10);
}

TEST_CASE("A raft is still generated under tree support", "[TreeSupport]")
{
    // The mesh supports itself, so a layer count alone passes with no raft at all.
    Slic3r::Print rafted, unrafted;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), rafted, "tree_slim", 30, 0, 3);
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), unrafted, "tree_slim", 30, 0, 0);
    const PrintObject *rafted_object   = rafted.objects().front();
    const PrintObject *unrafted_object = unrafted.objects().front();
    REQUIRE(rafted_object->support_layers().size() > unrafted_object->support_layers().size());
    // The raft goes under the object.
    REQUIRE(rafted_object->layers().front()->print_z > unrafted_object->layers().front()->print_z);
}

// drop_nodes() decides the node merges and spawns the next layer's nodes in parallel. Every one of
// those decisions has to be applied in a fixed order, or the same model gives different branches on
// each slice.
TEST_CASE("Tree support toolpaths do not depend on thread scheduling", "[TreeSupport][Regression]")
{
    // Scaled up so that a layer holds enough nodes for the parallel range to be split. At stock
    // size it stays in one chunk and the order never varies.
    SECTION("overhang")            { sliced_twice_matches(scaled(TestMesh::overhang, 2.f), 0); }
    SECTION("bridge with hole")    { sliced_twice_matches(scaled(TestMesh::bridge_with_hole, 3.f), 0); }
    // Dropping every branch that cannot reach the bed leaves the survivors dense enough that the
    // neighbour merge fires in bulk.
    SECTION("on the build plate")  { sliced_twice_matches(scaled(TestMesh::overhang, 4.f), 1); }
    // Branches resting on the model are what put nodes in a part group other than 0, which is the
    // only way to reach the prune in the second pass. tree_hybrid additionally builds polygon
    // nodes, so it is the only style that exercises the overhang merge.
    SECTION("resting on the model") { sliced_twice_matches(two_tier_mesh(), 0); }
    SECTION("hybrid on the model")  { sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid"); }
}

// Prim breaks equal-distance ties by heap address. A 1 mm branch diameter puts neighbours close
// enough to tie, and an explicit line width pins max_move_dist, so the moved tie winner reaches
// the support toolpaths.
TEST_CASE("Tree support toolpaths do not depend on the MST tie order", "[TreeSupport][Regression]")
{
    sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid", {
        { "tree_support_branch_diameter", 1.0 },
        { "tree_support_branch_distance", 5.0 },
        { "tree_support_branch_angle",    40 },
        { "support_line_width",           0.4 },
    });
}

namespace {

// Slices two_tier_mesh() with organic tree support and a variable layer height: thin layers below z = 10
// and thick ones above it, so the upper plate overhang is supported from thick layers and the branches
// cross into thin ones on their way down.
std::string slice_organic_with_variable_layer_height(Slic3r::Print &print, Slic3r::Model &model)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                "tree(auto)" },
        { "support_style",               "organic" },
        { "support_threshold_angle",     30 },
        { "layer_height",                0.2 },
        { "initial_layer_print_height",  0.2 },
        { "support_top_z_distance",      0.2 },
        { "support_bottom_z_distance",   0.2 },
    });
    ModelObject *object = model.add_object();
    object->add_volume(two_tier_mesh());
    // Every layer range must carry a layer_height (see layer_height_profile_from_ranges).
    for (const auto &[range, height] : { std::pair<t_layer_height_range, double>{ { 2., 10. }, 0.1 }, { { 10., 19. }, 0.28 } }) {
        DynamicPrintConfig range_config;
        range_config.set_key_value("layer_height", new ConfigOptionFloat(height));
        object->layer_config_ranges[range].assign_config(std::move(range_config));
    }
    object->add_instance()->set_offset(Vec3d(90., 90., 0.));
    object->ensure_on_bed();
    print.apply(model, config);
    std::string error = print.validate().string;
    if (error.empty())
        print.process();
    return error;
}

} // namespace

TEST_CASE("Organic tree support is generated on the layers of an object with variable layer height", "[TreeSupport]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    const std::string error = slice_organic_with_variable_layer_height(print, model);
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(support_points(print).size() > 1000);

    const PrintObject &object = *print.objects().front();
    double min_height = std::numeric_limits<double>::max(), max_height = 0.;
    for (const Layer *layer : object.layers())
        if (layer->print_z > 3.) {
            min_height = std::min(min_height, layer->height);
            max_height = std::max(max_height, layer->height);
        }
    // The object really is sliced with variable layer height.
    REQUIRE(max_height - min_height > 0.1);

    // Every support layer is printed together with an object layer and is as thick as it.
    size_t checked = 0;
    for (const SupportLayer *support_layer : object.support_layers()) {
        if (support_layer->support_fills.entities.empty())
            continue;
        const auto it = std::find_if(object.layers().begin(), object.layers().end(),
            [support_layer](const Layer *layer) { return std::abs(layer->print_z - support_layer->print_z) < EPSILON; });
        CAPTURE(support_layer->print_z, support_layer->height);
        REQUIRE(it != object.layers().end());
        CHECK_THAT(support_layer->height, Catch::Matchers::WithinAbs((*it)->height, EPSILON));
        ++checked;
    }
    REQUIRE(checked > 20);
}

TEST_CASE("Organic tree support keeps the top Z distance with variable layer height", "[TreeSupport]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    REQUIRE(slice_organic_with_variable_layer_height(print, model).empty());

    const PrintObject &object = *print.objects().front();
    // The bottom of the first layer of the 50 x 50 mm upper plate.
    double plate_bottom = 0.;
    for (const Layer *layer : object.layers())
        if (layer->print_z > 10. && area(layer->lslices) > 0.9 * sqr(scaled<double>(50.))) {
            plate_bottom = layer->print_z - layer->height;
            break;
        }
    REQUIRE(plate_bottom > 15.);

    // The highest support below the plate is where the branches supporting it end.
    double support_top = 0.;
    for (const SupportLayer *support_layer : object.support_layers())
        if (! support_layer->support_fills.entities.empty() && support_layer->print_z < plate_bottom + EPSILON)
            support_top = std::max(support_top, support_layer->print_z);
    const double gap = plate_bottom - support_top;
    CAPTURE(plate_bottom, support_top);
    // 0.2 mm rounds to a single empty 0.28 mm layer.
    CHECK_THAT(gap, Catch::Matchers::WithinAbs(0.28, EPSILON));
}
