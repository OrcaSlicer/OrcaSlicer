#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportLayer.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Narrow support fringes retain their original slabs", "[SupportMaterial][ExtruderLayerHeight]")
{
    const Polygon fringe = Polygon::new_scale({{0., 0.}, {10., 0.}, {10., 0.1}, {0., 0.1}});
    const Polygon column = Polygon::new_scale({{20., 0.}, {25., 0.}, {25., 5.}, {20., 5.}});
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayersPtr layers;
    for (int i = 0; i < 6; ++i) {
        auto &layer = storage.allocate(SupporLayerType::Base);
        layer.bottom_z = 0.15 + i * 0.05;
        layer.print_z = layer.bottom_z + 0.05;
        layer.height = 0.05;
        layer.polygons = {fringe, column};
        layers.push_back(&layer);
    }
    combine_support_layers(layers, storage, 0.3, 0.15, 0.6);
    size_t fringes = 0, columns = 0;
    for (const auto *layer : layers) {
        if (layer->height < 0.1) {
            REQUIRE_THAT(layer->height, WithinAbs(0.05, 1e-6));
            REQUIRE_THAT(layer->bottom_print_z(), WithinAbs(0.15 + fringes * 0.05, 1e-6));
            REQUIRE(diff(layer->polygons, Polygons{fringe}).empty());
            REQUIRE(diff(Polygons{fringe}, layer->polygons).empty());
            ++fringes;
        } else {
            REQUIRE_THAT(layer->height, WithinAbs(0.3, 1e-6));
            REQUIRE(diff(layer->polygons, Polygons{column}).empty());
            REQUIRE(diff(Polygons{column}, layer->polygons).empty());
            ++columns;
        }
    }
    REQUIRE(fringes == 6);
    REQUIRE(columns == 1);
}

TEST_CASE("Support columns combine without changing occupied volume", "[SupportMaterial][ExtruderLayerHeight]")
{
    const Polygon left = Polygon::new_scale({{0., 0.}, {5., 0.}, {5., 5.}, {0., 5.}});
    const Polygon right = Polygon::new_scale({{10., 0.}, {15., 0.}, {15., 5.}, {10., 5.}});
    struct Slab { double bottom, top; Polygons polygons; };
    std::vector<Slab> original{{0., 0.15, {left}}};
    for (int i = 3; i < 10; ++i) {
        Polygons polygons;
        if (i < 9) polygons.push_back(left);
        if (i >= 4) polygons.push_back(right);
        original.push_back({i * 0.05, (i + 1) * 0.05, polygons});
    }
    // A real vertical gap must survive the combination.
    for (int i = 11; i < 18; ++i)
        original.push_back({i * 0.05, (i + 1) * 0.05, {left}});
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayersPtr layers;
    for (const auto &slab : original) {
        auto &layer = storage.allocate(SupporLayerType::Base);
        layer.bottom_z = slab.bottom;
        layer.print_z = slab.top;
        layer.height = slab.top - slab.bottom;
        layer.polygons = slab.polygons;
        layers.push_back(&layer);
    }
    combine_support_layers(layers, storage, 0.3, 0.15, 0.6);
    REQUIRE(std::count_if(layers.begin(), layers.end(), [](const auto *layer) {
        return std::abs(layer->height - 0.3) < 1e-6;
    }) >= 3);
    REQUIRE_THAT(layers.front()->print_z, WithinAbs(0.15, 1e-6));
    REQUIRE_THAT(layers.front()->height, WithinAbs(0.15, 1e-6));
    for (int sample = 0; sample < 36; ++sample) {
        const double z = (sample + 0.5) * 0.025;
        CAPTURE(z);
        Polygons before, after;
        for (const auto &slab : original)
            if (slab.bottom < z && z < slab.top)
                before.insert(before.end(), slab.polygons.begin(), slab.polygons.end());
        for (const auto *layer : layers)
            if (layer->bottom_print_z() < z && z < layer->print_z)
                after.insert(after.end(), layer->polygons.begin(), layer->polygons.end());
        REQUIRE(diff(before, after).empty());
        REQUIRE(diff(after, before).empty());
        // Equal union and summed area also excludes duplicate deposited volume.
        REQUIRE_THAT(area(after), WithinAbs(area(union_(after)), 1.));
    }
}
