#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "test_helpers.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <string>
#include <utility>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {
constexpr int default_walls = 2;

// Paints the facets of a painted modifier's mesh that face along `direction`.
void paint_facing(ModelVolume &painted_modifier, const Vec3f &direction)
{
    const indexed_triangle_set &its = painted_modifier.mesh().its;
    TriangleSelector selector(painted_modifier.mesh());
    for (int facet_idx = 0; facet_idx < int(its.indices.size()); ++facet_idx)
        if (its_face_normal(its, facet_idx).dot(direction) > 0.99f)
            selector.set_facet(facet_idx, EnforcerBlockerType::ENFORCER);
    painted_modifier.painted_modifier_facets.set(selector);
}

// A 20 mm cube sliced at 0.2 mm, with modifiers added in object list order.
struct Scene {
    Model              model;
    Print              print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ModelObject       *object;

    Scene()
    {
        config.set_deserialize_strict({{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}, {"wall_loops", std::to_string(default_walls)}});
        object = model.add_object();
        object->add_volume(Test::cube(20));
        object->add_instance();
    }

    ModelVolume &part() { return *object->volumes.front(); }

    ModelVolume &add_painted_modifier(int walls, float depth, const Vec3f &direction = Vec3f::UnitX())
    {
        ModelVolume *painted_modifier = object->add_painted_modifier(part());
        painted_modifier->painted_modifier_depth = depth;
        painted_modifier->config.set_key_value("wall_loops", new ConfigOptionInt(walls));
        paint_facing(*painted_modifier, direction);
        return *painted_modifier;
    }

    // A modifier enclosing the whole part.
    ModelVolume &add_enclosing_modifier(int walls)
    {
        ModelVolume *modifier = object->add_volume(Test::cube(30), ModelVolumeType::PARAMETER_MODIFIER);
        modifier->set_offset(part().get_offset());
        modifier->config.set_key_value("wall_loops", new ConfigOptionInt(walls));
        modifier->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(50));
        return *modifier;
    }

    void slice()
    {
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
    }

    const Layer &layer_at(double z) const
    {
        const PrintObject &print_object = *print.objects().front();
        const auto it = std::find_if(print_object.layers().begin(), print_object.layers().end(), [z](const Layer *l) { return l->slice_z > z; });
        REQUIRE(it != print_object.layers().end());
        return **it;
    }
};

// Area in mm² of the layer regions printed with the given number of walls.
double area_with_walls(const Layer &layer, int walls)
{
    double area = 0.;
    for (const LayerRegion *region : layer.regions())
        if (region->region().config().wall_loops.value == walls)
            for (const Surface &surface : region->slices.surfaces)
                area += surface.expolygon.area();
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

const LayerRegion *region_with_walls(const Layer &layer, int walls)
{
    for (const LayerRegion *region : layer.regions())
        if (region->region().config().wall_loops.value == walls && ! region->slices.empty())
            return region;
    return nullptr;
}

// Painting one side of a 20 mm square claims the triangle of the square nearest to that side.
constexpr double nearest_side_area = 20. * 10. / 2.;
// A 3 mm deep band of that triangle is a trapezoid with parallel sides of 20 and 14 mm.
constexpr double band_area = 3. * (20. + 14.) / 2.;
constexpr double tolerance = 4.;
} // namespace

TEST_CASE("A painted modifier applies its settings only near the painted side of its part", "[PaintedModifier]")
{
    Scene scene;
    scene.add_painted_modifier(5, 0.f);
    scene.slice();
    const Layer &layer = scene.layer_at(10.);

    CHECK_THAT(area_with_walls(layer, 5), WithinAbs(nearest_side_area, tolerance));
    CHECK_THAT(area_with_walls(layer, default_walls), WithinAbs(400. - nearest_side_area, tolerance));
    const LayerRegion *painted = region_with_walls(layer, 5);
    REQUIRE(painted != nullptr);
    // The +X side was painted, so the region lies in the +X half of the layer.
    const BoundingBox painted_bbox = get_extents(painted->slices.surfaces);
    const BoundingBox layer_bbox   = get_extents(layer.lslices);
    CHECK(painted_bbox.min.x() >= layer_bbox.center().x() - scaled<coord_t>(0.5));
    CHECK(painted_bbox.max.x() >= layer_bbox.max.x() - scaled<coord_t>(0.5));
}

TEST_CASE("Painted modifier depth limits how far the paint reaches into the part", "[PaintedModifier]")
{
    Scene scene;
    scene.add_painted_modifier(5, 3.f);
    scene.slice();
    const Layer &layer = scene.layer_at(10.);

    CHECK_THAT(area_with_walls(layer, 5), WithinAbs(band_area, tolerance));
    const LayerRegion *painted = region_with_walls(layer, 5);
    REQUIRE(painted != nullptr);
    CHECK_THAT(unscaled(get_extents(painted->slices.surfaces).size().x()), WithinAbs(3., 0.2));
}

TEST_CASE("A painted top face reaches down by the painted modifier depth", "[PaintedModifier]")
{
    Scene scene;
    scene.add_painted_modifier(5, 1.f, Vec3f::UnitZ());
    scene.slice();

    CHECK_THAT(area_with_walls(scene.layer_at(19.8), 5), WithinAbs(400., tolerance));
    CHECK_THAT(area_with_walls(scene.layer_at(19.2), 5), WithinAbs(400., tolerance));
    CHECK(area_with_walls(scene.layer_at(18.), 5) == 0.);
    CHECK(area_with_walls(scene.layer_at(10.), 5) == 0.);
}

TEST_CASE("A painted modifier and a mesh modifier apply the settings of the later one in the object list", "[PaintedModifier]")
{
    Scene scene;
    scene.add_enclosing_modifier(3);
    scene.add_painted_modifier(5, 0.f);
    scene.slice();
    {
        const Layer &layer = scene.layer_at(10.);
        CHECK_THAT(area_with_walls(layer, 5), WithinAbs(nearest_side_area, tolerance));
        CHECK_THAT(area_with_walls(layer, 3), WithinAbs(400. - nearest_side_area, tolerance));
        // The painted modifier overrides only the walls, the mesh modifier still sets the infill below it.
        const LayerRegion *painted = region_with_walls(layer, 5);
        REQUIRE(painted != nullptr);
        CHECK_THAT(painted->region().config().sparse_infill_density.value, WithinAbs(50., 1e-9));
    }

    // Moving the mesh modifier after the painted one in the list lets it win everywhere.
    std::swap(scene.object->volumes[1], scene.object->volumes[2]);
    scene.slice();
    {
        const Layer &layer = scene.layer_at(10.);
        CHECK(area_with_walls(layer, 5) == 0.);
        CHECK_THAT(area_with_walls(layer, 3), WithinAbs(400., tolerance));
    }
}

TEST_CASE("Overlapping painted modifiers apply the settings of the later one in the object list", "[PaintedModifier]")
{
    const bool deep_first = GENERATE(true, false);
    CAPTURE(deep_first);
    Scene scene;
    if (deep_first) {
        scene.add_painted_modifier(5, 0.f);
        scene.add_painted_modifier(4, 3.f);
    } else {
        scene.add_painted_modifier(4, 3.f);
        scene.add_painted_modifier(5, 0.f);
    }
    scene.slice();
    const Layer &layer = scene.layer_at(10.);

    if (deep_first) {
        CHECK_THAT(area_with_walls(layer, 4), WithinAbs(band_area, tolerance));
        CHECK_THAT(area_with_walls(layer, 5), WithinAbs(nearest_side_area - band_area, tolerance));
    } else {
        CHECK(area_with_walls(layer, 4) == 0.);
        CHECK_THAT(area_with_walls(layer, 5), WithinAbs(nearest_side_area, tolerance));
    }
}

TEST_CASE("Repainting a painted modifier reslices its object", "[PaintedModifier]")
{
    Scene scene;
    ModelVolume &painted_modifier = scene.add_painted_modifier(5, 0.f);
    scene.slice();
    const coord_t center_x = get_extents(scene.layer_at(10.).lslices).center().x();
    REQUIRE(get_extents(region_with_walls(scene.layer_at(10.), 5)->slices.surfaces).min.x() >= center_x - scaled<coord_t>(0.5));

    paint_facing(painted_modifier, -Vec3f::UnitX());
    scene.slice();
    const LayerRegion *painted = region_with_walls(scene.layer_at(10.), 5);
    REQUIRE(painted != nullptr);
    CHECK(get_extents(painted->slices.surfaces).max.x() <= center_x + scaled<coord_t>(0.5));
}

TEST_CASE("A painted modifier without paint changes nothing", "[PaintedModifier]")
{
    Scene scene;
    ModelVolume *painted_modifier = scene.object->add_painted_modifier(scene.part());
    painted_modifier->config.set_key_value("wall_loops", new ConfigOptionInt(5));
    scene.slice();

    const Layer &layer = scene.layer_at(10.);
    CHECK(area_with_walls(layer, 5) == 0.);
    CHECK_THAT(area_with_walls(layer, default_walls), WithinAbs(400., tolerance));
}
