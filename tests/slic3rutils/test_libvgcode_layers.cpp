// The end bands of the reduced toolpath set, decided by libvgcode's layer table from the roles
// each layer was printed with.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/src/Layers.hpp"

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
