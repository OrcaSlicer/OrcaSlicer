#include <catch2/catch_all.hpp>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include <cstddef>
#include "libslic3r/libslic3r.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Print.hpp"

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

// What is printed in one support base area.
struct BaseAreaPaths
{
    // Paths cross the inside of the area, away from its walls.
    bool infilled = false;
    // Paths run along most of the outline of the area.
    bool walled = false;
};

void collect_base_polylines(const ExtrusionEntity &entity, Polylines &out, bool &other_roles)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection &>(entity).entities)
            collect_base_polylines(*child, out, other_roles);
    } else if (entity.role() == erSupportMaterial) {
        entity.collect_polylines(out);
    } else {
        other_roles = true;
    }
}

// The base areas of the support layers but the first, which is a flange with a sheath of its own. The
// paths are measured against the areas rather than told apart by kind, since the walls of hollow
// branches are printed as paths clipped at their seam, just like infill.
std::vector<BaseAreaPaths> base_areas(const Slic3r::Print &print)
{
    const float                width = float(scale_(0.4));
    std::vector<BaseAreaPaths> out;
    const auto                &layers = print.objects().front()->support_layers();
    for (size_t layer_idx = 1; layer_idx < layers.size(); ++ layer_idx) {
        const SupportLayer &layer = *layers[layer_idx];
        Polylines           paths;
        bool                other_roles = false;
        collect_base_polylines(layer.support_fills, paths, other_roles);
        // Organic supports keep only the islands, which take in the interfaces of the layers that have any.
        if (paths.empty() || (layer.base_areas.empty() && other_roles))
            continue;
        for (const ExPolygon &area : layer.base_areas.empty() ? layer.support_islands : layer.base_areas) {
            const double outline = unscaled(area.contour.length());
            if (outline < 3.)
                continue;
            BaseAreaPaths paths_in_area;
            paths_in_area.infilled = total_length(intersection_pl(paths, offset_ex(area, -3.f * width))) > scale_(2.);
            const ExPolygons band  = diff_ex(ExPolygons{ area }, offset_ex(area, -width));
            paths_in_area.walled   = unscaled(total_length(intersection_pl(paths, band))) > 0.75 * outline;
            out.emplace_back(paths_in_area);
        }
    }
    return out;
}

size_t count_areas(const std::vector<BaseAreaPaths> &areas, bool infilled)
{
    return std::count_if(areas.begin(), areas.end(), [infilled](const BaseAreaPaths &paths) {
        return paths.walled && paths.infilled == infilled;
    });
}

// A tall support trunk, merged from many branches, under a wide plate.
TriangleMesh trunk_mesh()
{
    TriangleMesh column = make_cube(6, 6, 40);
    TriangleMesh plate  = make_cube(40, 40, 3);
    column.translate(17.f, 17.f, 0.f);
    plate.translate(0.f, 0.f, 39.f);
    TriangleMesh mesh = column;
    mesh.merge(plate);
    return mesh;
}

std::vector<BaseAreaPaths> trunk_base_areas(const char *style, const char *pattern, int wall_count)
{
    Slic3r::Print print;
    slice_with_tree_support(trunk_mesh(), print, style, 30, 1, 0, {
        { "support_base_pattern",    pattern },
        { "tree_support_wall_count", wall_count },
    });
    return base_areas(print);
}

} // namespace

// Support wall loops is auto (-1) unless set, and a value of -1 is kept as it is when loaded.
TEST_CASE("Tree support wall loops default to auto", "[TreeSupport]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK(config.opt_int("tree_support_wall_count") == -1);
    config.set_deserialize_strict({ { "tree_support_wall_count", 0 } });
    config.set_deserialize_strict({ { "tree_support_wall_count", "-1" } });
    CHECK(config.opt_int("tree_support_wall_count") == -1);
}

// With Support wall loops at -1 (auto), a support area that gets infill keeps a wall around it.
// Without one, the infill lines of each layer stand on those below as separate plates with nothing
// tying them together, and a tall support folds over.
TEST_CASE("Tree support infill keeps a wall around it when the wall count is auto", "[TreeSupport]")
{
    const char *style   = GENERATE("tree_slim", "tree_strong", "tree_hybrid", "organic");
    const char *pattern = GENERATE("rectilinear", "default");
    CAPTURE(style, pattern);

    size_t infilled = 0;
    for (const BaseAreaPaths &paths : trunk_base_areas(style, pattern, -1))
        if (paths.infilled) {
            ++ infilled;
            CHECK(paths.walled);
        }
    // Default leaves tree branches and trunks hollow; only the normal support of Tree Hybrid gets infill with it.
    if (std::string(pattern) == "rectilinear" || std::string(style) == "tree_hybrid")
        REQUIRE(infilled > 0);
}

// With Support wall loops at 0, support thick enough may still be printed as infill alone, as auto did
// before -1 was added.
TEST_CASE("Tree support infill may go without a wall when the wall count is 0", "[TreeSupport]")
{
    const char *style = GENERATE("tree_slim", "tree_strong");
    CAPTURE(style);

    const std::vector<BaseAreaPaths> areas = trunk_base_areas(style, "rectilinear", 0);
    CHECK(std::any_of(areas.begin(), areas.end(), [](const BaseAreaPaths &paths) { return paths.infilled && ! paths.walled; }));
}

// Default and Hollow print no infill in tree branches or trunks, so saved projects slice as before.
TEST_CASE("Tree support stays hollow with the Default and Hollow base patterns", "[TreeSupport]")
{
    const char *style      = GENERATE("tree_slim", "tree_strong", "organic");
    const char *pattern    = GENERATE("default", "hollow");
    const int   wall_count = GENERATE(-1, 0);
    CAPTURE(style, pattern, wall_count);

    const std::vector<BaseAreaPaths> areas = trunk_base_areas(style, pattern, wall_count);
    CHECK(count_areas(areas, false) > 0);
    CHECK(std::none_of(areas.begin(), areas.end(), [](const BaseAreaPaths &paths) { return paths.infilled; }));
}
