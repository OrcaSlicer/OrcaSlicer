#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <string>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;

namespace {

// Paints the top face of `volume` with filament 2 and its bottom face with filament 3.
void paint_top_and_bottom(ModelVolume &volume)
{
    const TriangleMesh  &mesh = volume.mesh();
    const BoundingBoxf3  bbox = mesh.bounding_box();
    TriangleSelector     selector(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i) {
        bool top = true, bottom = true;
        for (int j = 0; j < 3; ++j) {
            const double z = mesh.its.vertices[mesh.its.indices[i][j]].z();
            top    = top && std::abs(z - bbox.max.z()) < EPSILON;
            bottom = bottom && std::abs(z - bbox.min.z()) < EPSILON;
        }
        if (top)
            selector.set_facet(i, EnforcerBlockerType::Extruder2);
        else if (bottom)
            selector.set_facet(i, EnforcerBlockerType::Extruder3);
    }
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

// Paints the +X side faces of `volume` with filament 2.
void paint_max_x_side(ModelVolume &volume)
{
    const TriangleMesh &mesh  = volume.mesh();
    const double        max_x = mesh.bounding_box().max.x();
    TriangleSelector    selector(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i) {
        bool on_side = true;
        for (int j = 0; j < 3; ++j)
            on_side = on_side && std::abs(mesh.its.vertices[mesh.its.indices[i][j]].x() - max_x) < EPSILON;
        if (on_side)
            selector.set_facet(i, EnforcerBlockerType::Extruder2);
    }
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

size_t layers_with_filament(const PrintObject &object, int filament_id)
{
    size_t count = 0;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            if (region->region().config().outer_wall_filament_id.value == filament_id && !region->slices.empty()) {
                ++count;
                break;
            }
    return count;
}

} // namespace

TEST_CASE("Painted top and bottom surfaces color their paint penetration layers", "[MultiMaterialSegmentation]")
{
    // 0 follows the shell layers; other values apply as set, below or above the shell.
    const auto [top_penetration, bottom_penetration, expected_top, expected_bottom] =
        GENERATE(table<int, int, size_t, size_t>({{0, 0, 5, 4}, {2, 3, 2, 3}, {7, 6, 7, 6}}));
    CAPTURE(top_penetration, bottom_penetration);

    // A 4 mm cube at 0.2 mm layers has 20 layers, enough to keep the top and bottom bands apart.
    DynamicPrintConfig config = Test::multifilament_config(3, {
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"top_shell_layers", "5"},
        {"bottom_shell_layers", "4"},
        {"top_color_penetration_layers", std::to_string(top_penetration)},
        {"bottom_color_penetration_layers", std::to_string(bottom_penetration)},
        {"gcode_comments", "1"}, // Match init_print so the second apply changes only the model.
    });

    Print print;
    Model model;
    Test::init_print({TriangleMesh(its_make_cube(20, 20, 4))}, print, model, config);
    paint_top_and_bottom(*model.objects.front()->volumes.front());
    print.apply(model, config);

    REQUIRE(print.objects().size() == 1);
    PrintObject &object = *print.get_object(0);
    object.slice();
    REQUIRE(object.layers().size() == 20);

    CHECK(layers_with_filament(object, 2) == expected_top);
    CHECK(layers_with_filament(object, 3) == expected_bottom);
}

TEST_CASE("The maximum width of a segmented region limits how deep side paint reaches into the model", "[MultiMaterialSegmentation]")
{
    // 0 lets the paint meet the rest of the layer halfway across the 20 mm wide cube.
    const auto [penetration, expected_depth] = GENERATE(table<double, double>({{0., 10.}, {2., 2.}, {5., 5.}}));
    CAPTURE(penetration);

    // A 4 mm cube at 0.2 mm layers has 20 layers; the 11th is away from the top and bottom shells.
    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"mmu_segmented_region_max_width", std::to_string(penetration)},
        {"gcode_comments", "1"}, // Match init_print so the second apply changes only the model.
    });

    Print print;
    Model model;
    Test::init_print({TriangleMesh(its_make_cube(20, 20, 4))}, print, model, config);
    paint_max_x_side(*model.objects.front()->volumes.front());
    print.apply(model, config);

    REQUIRE(print.objects().size() == 1);
    PrintObject &object = *print.get_object(0);
    object.slice();
    REQUIRE(object.layers().size() == 20);

    BoundingBox painted;
    for (const LayerRegion *region : object.get_layer(10)->regions())
        if (region->region().config().outer_wall_filament_id.value == 2 && !region->slices.empty())
            painted.merge(get_extents(region->slices.surfaces));
    REQUIRE(painted.defined);
    CHECK_THAT(unscale<double>(painted.size().x()), Catch::Matchers::WithinAbs(expected_depth, 0.1));
}
