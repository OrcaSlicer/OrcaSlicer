#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"

#include "test_helpers.hpp"

#include <limits>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;

// 20mm cube in spiral mode: 0.25mm layers (80 total), 3 bottom shell layers, so the
// fillet base sits at z = 0.75. Explicit line widths keep the wall spacing, and with
// it the expected fillet wall counts, independent of default changes.
static Slic3r::DynamicPrintConfig spiral_vase_config(double bottom_fillet_radius)
{
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "spiral_mode",                      1 },
        { "spiral_mode_bottom_fillet_radius", bottom_fillet_radius },
        { "wall_loops",                       1 },
        { "top_shell_layers",                 0 },
        { "sparse_infill_density",            0 },
        { "bottom_shell_layers",              3 },
        { "bottom_shell_thickness",           0 },
        { "layer_height",                     0.25 },
        { "initial_layer_print_height",       0.25 },
        { "line_width",                       0.45 },
        { "inner_wall_line_width",            0.45 },
        { "skirt_loops",                      0 },
        { "brim_type",                        "no_brim" }
    });
    return config;
}

// A move that changes X/Y, ramps Z and extrudes all at once only occurs inside a spiral.
static double min_spiral_move_z(const std::string &gcode, bool &found_any)
{
    double min_z = std::numeric_limits<double>::max();
    found_any = false;
    std::istringstream stream(gcode);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("G1 ", 0) != 0)
            continue;
        if (size_t comment = line.find(';'); comment != std::string::npos)
            line.erase(comment);
        size_t z_pos = line.find('Z');
        size_t e_pos = line.find('E');
        if (line.find('X') == std::string::npos || line.find('Y') == std::string::npos ||
            z_pos == std::string::npos || e_pos == std::string::npos)
            continue;
        if (atof(line.c_str() + e_pos + 1) > 0.) {
            found_any = true;
            min_z = std::min(min_z, atof(line.c_str() + z_pos + 1));
        }
    }
    return min_z;
}

// First layer eligible for spiraling: a single perimeter loop and no fill extrusions.
static const Layer *first_spiral_candidate(const PrintObject &object)
{
    for (const Layer *layer : object.layers()) {
        const LayerRegion *region = layer->regions().front();
        if (region->perimeters.items_count() == 1 && region->fills.items_count() == 0)
            return layer;
    }
    return nullptr;
}

TEST_CASE("Spiral layers keep a single perimeter and no fill without a bottom fillet", "[SpiralVase]") {
    Slic3r::Print print;
    Slic3r::Model model;
    init_print({cube(20)}, print, model, spiral_vase_config(0.));
    print.process();
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() == 80);
    for (const Layer *layer : object.layers()) {
        const LayerRegion *region = layer->regions().front();
        if (layer->id() >= 3) {
            CHECK(region->perimeters.items_count() == 1);
            CHECK(region->fills.items_count() == 0);
        } else {
            CHECK(region->fills.items_count() > 0);
        }
    }
}

TEST_CASE("Bottom fillet adds inner walls that taper away with height", "[SpiralVase]") {
    Slic3r::Print print;
    Slic3r::Model model;
    init_print({cube(20)}, print, model, spiral_vase_config(2.));
    print.process();
    // Fillet base z = 3 * 0.25 = 0.75; a 2mm fillet spans layers whose bottom z is in
    // [0.75, 2.75), i.e. layer ids 3..10.
    const PrintObject &object = *print.objects().front();
    // The first fillet layers carry extra inner walls (id 3 holds the full 2mm band).
    CHECK(object.get_layer(3)->regions().front()->perimeters.items_count() >= 3);
    CHECK(object.get_layer(4)->regions().front()->perimeters.items_count() >= 2);
    // The wall count tapers monotonically through the fillet.
    size_t previous = std::numeric_limits<size_t>::max();
    for (int i = 3; i <= 11; ++ i) {
        size_t count = object.get_layer(i)->regions().front()->perimeters.items_count();
        CHECK(count <= previous);
        previous = count;
    }
    // Above the fillet the spiral wall is back to a single loop without fill.
    for (const Layer *layer : object.layers())
        if (layer->id() >= 11) {
            const LayerRegion *region = layer->regions().front();
            CHECK(region->perimeters.items_count() == 1);
            CHECK(region->fills.items_count() == 0);
        }
    // The bottom shell layers are still fully filled.
    for (int i = 0; i < 3; ++ i)
        CHECK(object.get_layer(i)->regions().front()->fills.items_count() > 0);
}

TEST_CASE("Spiral Z ramp starts right above the last non-spiral layer", "[SpiralVase]") {
    // Without a fillet the spiral starts right above the 3 bottom shell layers; a 2mm
    // fillet (layer ids 3..10, minus taper quantization) delays it into id 7..11.
    auto [fillet_radius, min_transition_id, max_transition_id] =
        GENERATE(table<double, size_t, size_t>({ {0., 3, 3}, {2., 7, 11} }));
    Slic3r::Print print;
    Slic3r::Model model;
    init_print({cube(20)}, print, model, spiral_vase_config(fillet_radius));
    print.process();
    const Layer *transition = first_spiral_candidate(*print.objects().front());
    REQUIRE(transition != nullptr);
    CHECK(transition->id() >= min_transition_id);
    CHECK(transition->id() <= max_transition_id);
    // Spiral moves never dip below the top of the last non-spiral layer.
    bool found_spiral_moves = false;
    double min_z = min_spiral_move_z(Slic3r::Test::gcode(print), found_spiral_moves);
    CHECK(found_spiral_moves);
    CHECK(min_z > transition->print_z - transition->height - 0.01);
}
