// The end bands of the reduced toolpath set, decided by libvgcode's layer table from the roles
// each layer was printed with.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/src/Layers.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"

using libvgcode::EGCodeExtrusionRole;
using libvgcode::EMoveType;
using libvgcode::Interval;
using libvgcode::Layers;
using libvgcode::PathVertex;

namespace {

// One extrusion of each given role per layer, layers 0 to count - 1, 0.2 mm apart.
Layers layers_with(std::size_t count, const std::vector<std::vector<EGCodeExtrusionRole>>& roles_per_layer)
{
    Layers layers;
    uint32_t id = 0;
    for (std::size_t layer = 0; layer < count; ++layer) {
        const std::vector<EGCodeExtrusionRole>& roles = roles_per_layer[layer % roles_per_layer.size()];
        PathVertex v;
        v.type = EMoveType::Travel;
        v.layer_id = static_cast<uint32_t>(layer);
        v.position = { 0.0f, 0.0f, 0.2f * static_cast<float>(layer + 1) };
        layers.update(v, id++);
        for (EGCodeExtrusionRole role : roles) {
            v.type = EMoveType::Extrude;
            v.role = role;
            layers.update(v, id++);
        }
    }
    return layers;
}

const std::vector<EGCodeExtrusionRole> walls_and_infill = { EGCodeExtrusionRole::ExternalPerimeter, EGCodeExtrusionRole::Perimeter, EGCodeExtrusionRole::InternalInfill };
const std::vector<EGCodeExtrusionRole> walls_only = { EGCodeExtrusionRole::ExternalPerimeter, EGCodeExtrusionRole::Perimeter };
const std::vector<EGCodeExtrusionRole> support_only = { EGCodeExtrusionRole::SupportMaterial };

} // namespace

TEST_CASE("A range whose end layers have infill keeps just those two layers whole", "[Libvgcode]")
{
    Layers layers = layers_with(10, { walls_and_infill });
    layers.set_view_range(2, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 2, 7 });
}

TEST_CASE("A top layer with walls alone takes the layers down to the nearest infill along", "[Libvgcode]")
{
    // infill combination over three layers: the infill lands on layers 0, 3, 6 and 9
    Layers layers = layers_with(10, { walls_and_infill, walls_only, walls_only });
    layers.set_view_range(0, 8);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 6 });
    layers.set_view_range(0, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 6 });
    layers.set_view_range(0, 9);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 9 });
}

TEST_CASE("A bottom layer with walls alone takes the layers up to the nearest infill along", "[Libvgcode]")
{
    Layers layers = layers_with(10, { walls_and_infill, walls_only, walls_only });
    layers.set_view_range(4, 9);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 6, 9 });
}

TEST_CASE("A range from the first layer has no bottom band", "[Libvgcode]")
{
    // the bottom shells: no sparse infill on layers 0 to 2
    Layers layers = layers_with(10, { walls_only, walls_only, walls_only, walls_and_infill });
    layers.set_view_range(0, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 7 });
    layers.set_view_range(1, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 3, 7 });
}

TEST_CASE("A layer of support alone at the top of the range is drawn with the object layer below it", "[Libvgcode]")
{
    Layers layers = layers_with(6, { walls_and_infill, support_only });
    layers.set_view_range(0, 5);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 4 });
}

TEST_CASE("A layer with only solid infill ends the band on a print with no sparse infill in reach", "[Libvgcode]")
{
    Layers layers = layers_with(6, { { EGCodeExtrusionRole::ExternalPerimeter, EGCodeExtrusionRole::TopSolidInfill }, walls_only });
    layers.set_view_range(0, 5);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 4 });
}

TEST_CASE("Another object's solid infill on the layer does not end the band while sparse infill is in reach", "[Libvgcode]")
{
    // infill combined over two layers, and a second object topping out on the odd layers
    const std::vector<EGCodeExtrusionRole> walls_and_top = { EGCodeExtrusionRole::ExternalPerimeter, EGCodeExtrusionRole::TopSolidInfill };
    Layers layers = layers_with(8, { walls_and_infill, walls_only, walls_and_infill, walls_and_top });
    layers.set_view_range(0, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 6 });
    layers.set_view_range(3, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 4, 6 });
}

TEST_CASE("An end band reaches at most the given depth and otherwise stays on the end layer", "[Libvgcode]")
{
    // infill on layers 0, 10 and 20: from the top layer 29, layer 20 is the tenth layer down
    Layers layers = layers_with(30, { walls_and_infill, walls_only, walls_only, walls_only, walls_only,
                                      walls_only, walls_only, walls_only, walls_only, walls_only });
    layers.set_view_range(0, 29);
    CHECK(layers.get_whole_end_layers(10) == Interval{ 0, 20 });
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 29 });
}

TEST_CASE("A print with no infill keeps just the end layers whole", "[Libvgcode]")
{
    Layers layers = layers_with(30, { walls_only });
    layers.set_view_range(0, 29);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 29 });
}

TEST_CASE("The two end bands never cross", "[Libvgcode]")
{
    // infill on layers 0, 4 and 8: the top band of the range 4..7 reaches layer 4, the bottom layer
    Layers layers = layers_with(10, { walls_and_infill, walls_only, walls_only, walls_only });
    layers.set_view_range(4, 7);
    const Interval whole = layers.get_whole_end_layers(8);
    CHECK(whole[0] == 4);
    CHECK(whole[1] == 4);
}

TEST_CASE("Layers the caller marks as closed decide the bands over the layer's own roles", "[Libvgcode]")
{
    // sparse infill on every layer, but the caller knows layers 5 and 6 leave an object open
    Layers layers = layers_with(8, { walls_and_infill });
    layers.set_closed_layers({ 3, 3, 3, 3, 3, 0, 0, 3 });
    layers.set_view_range(0, 6);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 4 });
    layers.set_view_range(0, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 7 });
}

TEST_CASE("The top band reads the closure from above and the bottom band the one from below", "[Libvgcode]")
{
    // an object's bottom shells on layers 1 and 2, its top shells on 7 and 8
    Layers layers = layers_with(10, { walls_and_infill });
    layers.set_closed_layers({ 3, 2, 2, 3, 3, 3, 3, 1, 1, 3 });
    layers.set_view_range(1, 8);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 1, 8 });
    layers.set_view_range(2, 7);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 2, 7 });
}

TEST_CASE("A closed-layer list of the wrong size is ignored", "[Libvgcode]")
{
    Layers layers = layers_with(8, { walls_and_infill });
    layers.set_closed_layers({ 0, 0, 0 });
    layers.set_view_range(0, 6);
    CHECK(layers.get_whole_end_layers(8) == Interval{ 0, 6 });
}

TEST_CASE("A layer is closed when every object printed on it has infill", "[Libvgcode]")
{
    using libvgcode::LayersClosedByInfill;
    LayersClosedByInfill closed;
    const int a = 1, b = 2, none = -1;
    // layer 0: both objects print walls and sparse infill
    closed.add(0, a, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
    closed.add(0, a, EGCodeExtrusionRole::InternalInfill, EMoveType::Extrude);
    closed.add(0, b, EGCodeExtrusionRole::Perimeter, EMoveType::Extrude);
    closed.add(0, b, EGCodeExtrusionRole::InternalInfill, EMoveType::Extrude);
    // layer 1: a's infill was combined into another layer, b has its sparse infill
    closed.add(1, a, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
    closed.add(1, b, EGCodeExtrusionRole::Perimeter, EMoveType::Extrude);
    closed.add(1, b, EGCodeExtrusionRole::InternalInfill, EMoveType::Extrude);
    // layer 2: b tops out with solid infill, a has walls and the solid infill under its skin,
    // its sparse middle combined into layer 3
    closed.add(2, a, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
    closed.add(2, a, EGCodeExtrusionRole::SolidInfill, EMoveType::Extrude);
    closed.add(2, b, EGCodeExtrusionRole::Perimeter, EMoveType::Extrude);
    closed.add(2, b, EGCodeExtrusionRole::TopSolidInfill, EMoveType::Extrude);
    // layer 3: only a, with its sparse infill; a travel changes nothing
    closed.add(3, a, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
    closed.add(3, a, EGCodeExtrusionRole::InternalInfill, EMoveType::Extrude);
    closed.add(3, b, EGCodeExtrusionRole::InternalInfill, EMoveType::Travel);
    // layer 4: support alone
    closed.add(4, a, EGCodeExtrusionRole::SupportMaterial, EMoveType::Extrude);
    // layer 5: moves of no object, walls and infill
    closed.add(5, none, EGCodeExtrusionRole::Perimeter, EMoveType::Extrude);
    closed.add(5, none, EGCodeExtrusionRole::SolidInfill, EMoveType::Extrude);
    // layer 6: nothing at all
    const std::vector<uint8_t> result = closed.finish(7);
    REQUIRE(result.size() == 7);
    CHECK(result[0] == 3);
    CHECK(result[1] == 0);
    // b's top shell closes b from above, but a's sparse middle lies both above and below
    CHECK(result[2] == 0);
    CHECK(result[3] == 3);
    CHECK(result[4] == 0);
    CHECK(result[5] == 3);
    CHECK(result[6] == 0);
}

TEST_CASE("An object's top shells close it from above and its bottom shells from below", "[Libvgcode]")
{
    using libvgcode::LayersClosedByInfill;
    LayersClosedByInfill closed;
    // bottom shells on layers 0 and 1, sparse infill on 2 to 4, top shells on 5 and 6
    for (uint32_t layer = 0; layer < 7; ++layer) {
        closed.add(layer, 7, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
        closed.add(layer, 7, (layer >= 2 && layer <= 4) ? EGCodeExtrusionRole::InternalInfill : EGCodeExtrusionRole::SolidInfill, EMoveType::Extrude);
    }
    const std::vector<uint8_t> result = closed.finish(7);
    REQUIRE(result.size() == 7);
    CHECK(result[0] == 2);
    CHECK(result[1] == 2);
    CHECK(result[2] == 3);
    CHECK(result[3] == 3);
    CHECK(result[4] == 3);
    CHECK(result[5] == 1);
    CHECK(result[6] == 1);
}

TEST_CASE("An object with no sparse infill anywhere is closed by its solid infill", "[Libvgcode]")
{
    using libvgcode::LayersClosedByInfill;
    LayersClosedByInfill closed;
    closed.add(0, 7, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
    closed.add(0, 7, EGCodeExtrusionRole::SolidInfill, EMoveType::Extrude);
    closed.add(1, 7, EGCodeExtrusionRole::ExternalPerimeter, EMoveType::Extrude);
    closed.add(1, 7, EGCodeExtrusionRole::SolidInfill, EMoveType::Extrude);
    const std::vector<uint8_t> result = closed.finish(2);
    REQUIRE(result.size() == 2);
    CHECK(result[0] == 3);
    CHECK(result[1] == 3);
}

TEST_CASE("A tally with no labelled object yields no closed-layer list", "[Libvgcode]")
{
    using libvgcode::LayersClosedByInfill;
    LayersClosedByInfill closed;
    closed.add(0, -1, EGCodeExtrusionRole::Perimeter, EMoveType::Extrude);
    closed.add(0, -1, EGCodeExtrusionRole::InternalInfill, EMoveType::Extrude);
    closed.add(1, -1, EGCodeExtrusionRole::Perimeter, EMoveType::Extrude);
    CHECK(closed.finish(2).empty());
}
