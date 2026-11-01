#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

#include <cstddef>
#include <utility>

using namespace Slic3r;

namespace {

// Two 20 x 20 x 6 mm boxes touching along X, one object, each part on its own filament.
// Layer height 0.2 mm everywhere gives 6 / 0.2 = 30 layers.
constexpr double box_height   = 6.;
constexpr double layer_height = 0.2;
constexpr size_t layer_count  = 30;

struct InterlockedBoxes
{
    Model model;
    Print print;

    InterlockedBoxes(int boundary_avoidance, int skip_bottom_layers)
    {
        // Beam settings from the report: 0.8 mm beams at 22.5 degrees, three layers high, one cell deep.
        const DynamicPrintConfig config = Test::multifilament_config(2, {
            { "layer_height",                    layer_height },
            { "initial_layer_print_height",      layer_height },
            { "elefant_foot_compensation",       0 },
            { "skirt_loops",                     0 },
            { "brim_type",                       "no_brim" },
            { "enable_prime_tower",              0 },
            { "interlocking_beam",               1 },
            { "interlocking_beam_width",         0.8 },
            { "interlocking_orientation",        22.5 },
            { "interlocking_beam_layer_count",   3 },
            { "interlocking_depth",              1 },
            { "interlocking_boundary_avoidance", boundary_avoidance },
            { "interlocking_skip_bottom_layers", skip_bottom_layers },
        });

        ModelObject *object = model.add_object();
        TriangleMesh second_mesh = make_cube(20., 20., box_height);
        second_mesh.translate(20.f, 0.f, 0.f);
        ModelVolume *first  = object->add_volume(make_cube(20., 20., box_height), ModelVolumeType::MODEL_PART, false);
        ModelVolume *second = object->add_volume(std::move(second_mesh), ModelVolumeType::MODEL_PART, false);
        first->config.set("extruder", 1);
        second->config.set("extruder", 2);
        object->add_instance();
        object->ensure_on_bed();

        print.apply(model, config);
        print.process();
    }

    const PrintObject &object() const { return *print.objects().front(); }
};

// Without interlocking, each part's slice on a layer is a plain axis-aligned rectangle that fills its bounding box.
// Beams poke fingers into the neighbouring part and leave notches behind, so the slice no longer fills its box.
bool has_beams(const Layer &layer)
{
    for (const LayerRegion *region : layer.regions()) {
        const ExPolygons slices = to_expolygons(region->slices.surfaces);
        if (slices.empty())
            continue;
        const BoundingBox bbox = get_extents(slices);
        const double      bbox_area = double(bbox.size().x()) * double(bbox.size().y());
        if (area(slices) < 0.995 * bbox_area)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("Beam interlocking leaves the skipped bottom layers with their original outlines", "[Interlocking]")
{
    const int boundary_avoidance = GENERATE(0, 1);
    const int skip_bottom_layers = GENERATE(2, 4, 7);
    INFO("boundary avoidance " << boundary_avoidance << ", skip bottom layers " << skip_bottom_layers);

    InterlockedBoxes boxes(boundary_avoidance, skip_bottom_layers);
    const PrintObject &object = boxes.object();
    REQUIRE(object.num_printing_regions() == 2);
    REQUIRE(object.layer_count() == layer_count);

    for (size_t layer_nr = 0; layer_nr < size_t(skip_bottom_layers); ++layer_nr) {
        INFO("layer " << layer_nr);
        CHECK_FALSE(has_beams(*object.get_layer(int(layer_nr))));
    }

    bool beams_above = false;
    for (size_t layer_nr = size_t(skip_bottom_layers); layer_nr < object.layer_count(); ++layer_nr)
        beams_above = beams_above || has_beams(*object.get_layer(int(layer_nr)));
    CHECK(beams_above);
}

TEST_CASE("Beam interlocking reaches the first layer when no bottom layers are skipped", "[Interlocking]")
{
    const int boundary_avoidance = GENERATE(0, 1);
    INFO("boundary avoidance " << boundary_avoidance);

    InterlockedBoxes boxes(boundary_avoidance, 0);
    const PrintObject &object = boxes.object();
    REQUIRE(object.layer_count() == layer_count);

    CHECK(has_beams(*object.get_layer(0)));
}

TEST_CASE("Skipping every layer leaves the parts without beams", "[Interlocking]")
{
    InterlockedBoxes boxes(0, int(layer_count));
    const PrintObject &object = boxes.object();
    REQUIRE(object.layer_count() == layer_count);

    for (size_t layer_nr = 0; layer_nr < object.layer_count(); ++layer_nr) {
        INFO("layer " << layer_nr);
        CHECK_FALSE(has_beams(*object.get_layer(int(layer_nr))));
    }
}
