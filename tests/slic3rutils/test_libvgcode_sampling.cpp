// The vertex that stands for a role when the solid model takes the toolpaths' colours: the
// length-weighted median of what the view grades by.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/src/Utils.hpp"

using libvgcode::collect_role_samples;
using libvgcode::EGCodeExtrusionRole;
using libvgcode::EMoveType;
using libvgcode::Interval;
using libvgcode::PathVertex;
using libvgcode::pick_sample_layers;
using libvgcode::sample_role_layers;
using libvgcode::weighted_median_index;
using libvgcode::XYBox;

namespace {

// An outer wall extrusion of the layer ending at x, y, printed at the given speed.
PathVertex wall_at(float x, float y, float speed)
{
    PathVertex v;
    v.type = EMoveType::Extrude;
    v.role = EGCodeExtrusionRole::ExternalPerimeter;
    v.layer_id = 0;
    v.position = { x, y, 0.2f };
    v.feedrate = speed;
    return v;
}

const auto speed_of = [](const PathVertex& v) { return std::optional<float>(v.feedrate); };

} // namespace

TEST_CASE("Samples of equal weight give the one in the middle by value", "[Libvgcode]")
{
    const std::vector<std::pair<float, float>> samples = { { 30.0f, 1.0f }, { 10.0f, 1.0f }, { 20.0f, 1.0f } };
    CHECK(weighted_median_index(samples) == 2);
}

TEST_CASE("A short ramp at the start does not outweigh the long run that follows", "[Libvgcode]")
{
    // an outer wall: a few short segments accelerating, then most of its length at full speed
    const std::vector<std::pair<float, float>> samples = { { 5.0f, 0.1f }, { 20.0f, 0.1f }, { 60.0f, 0.1f }, { 100.0f, 5.0f }, { 100.0f, 4.0f }, { 40.0f, 0.2f } };
    CHECK(samples[weighted_median_index(samples)].first == 100.0f);
}

TEST_CASE("Weight on the low side pulls the median down", "[Libvgcode]")
{
    const std::vector<std::pair<float, float>> samples = { { 100.0f, 1.0f }, { 20.0f, 3.0f }, { 50.0f, 1.0f } };
    CHECK(weighted_median_index(samples) == 1);
}

TEST_CASE("Samples with no weight between them fall back to the middle one by value", "[Libvgcode]")
{
    const std::vector<std::pair<float, float>> samples = { { 3.0f, 0.0f }, { 1.0f, 0.0f }, { 2.0f, 0.0f } };
    CHECK(weighted_median_index(samples) == 2);
}

TEST_CASE("No samples give the sample count", "[Libvgcode]")
{
    CHECK(weighted_median_index({}) == 0);
}

TEST_CASE("Only the extrusions that end inside the box are sampled", "[Libvgcode]")
{
    // two objects on one layer: a slow one around the origin, a fast one a hundred millimetres along
    const std::vector<PathVertex> vertices = { wall_at(0.0f, 0.0f, 20.0f), wall_at(10.0f, 0.0f, 20.0f), wall_at(10.0f, 10.0f, 20.0f),
                                               wall_at(100.0f, 0.0f, 200.0f), wall_at(110.0f, 0.0f, 200.0f), wall_at(110.0f, 10.0f, 200.0f) };
    const Interval range = { 0, vertices.size() - 1 };
    std::vector<std::pair<float, float>> samples;
    std::vector<std::size_t> ids;

    const XYBox fast_one{ { 99.0f, -1.0f }, { 111.0f, 11.0f } };
    CHECK(!collect_role_samples(vertices, range, 0, EGCodeExtrusionRole::ExternalPerimeter, fast_one, speed_of, samples, ids).has_value());
    CHECK(ids == std::vector<std::size_t>{ 3, 4, 5 });
    REQUIRE(samples.size() == 3);
    CHECK(samples[0].first == 200.0f);
    // the length of the segment that ends at the vertex, from the one before it
    CHECK(samples[1].second == 10.0f);

    samples.clear();
    ids.clear();
    CHECK(!collect_role_samples(vertices, range, 0, EGCodeExtrusionRole::ExternalPerimeter, XYBox(), speed_of, samples, ids).has_value());
    CHECK(ids.size() == 6);
}

TEST_CASE("A view that grades by nothing takes the first extrusion in the box", "[Libvgcode]")
{
    const std::vector<PathVertex> vertices = { wall_at(0.0f, 0.0f, 20.0f), wall_at(100.0f, 0.0f, 200.0f), wall_at(110.0f, 0.0f, 200.0f) };
    const Interval range = { 0, vertices.size() - 1 };
    std::vector<std::pair<float, float>> samples;
    std::vector<std::size_t> ids;
    const auto no_value = [](const PathVertex&) { return std::optional<float>(); };
    const XYBox fast_one{ { 99.0f, -1.0f }, { 111.0f, 11.0f } };
    CHECK(collect_role_samples(vertices, range, 0, EGCodeExtrusionRole::ExternalPerimeter, fast_one, no_value, samples, ids) == 1);
    CHECK(samples.empty());
}

TEST_CASE("Layers are picked among those that have the role, from the first to the last", "[Libvgcode]")
{
    // top surfaces on four layers of a hundred: spread evenly over all of them, a sample of eight
    // would land on layers 0, 14, 28, ... 99 and miss every one
    const std::vector<std::size_t> top = { 7, 8, 9, 10 };
    const auto has_top = [&top](std::size_t id) { return std::find(top.begin(), top.end(), id) != top.end(); };
    CHECK(pick_sample_layers(Interval{ 0, 99 }, 8, has_top) == top);
    CHECK(pick_sample_layers(Interval{ 9, 99 }, 8, has_top) == std::vector<std::size_t>{ 9, 10 });
    CHECK(pick_sample_layers(Interval{ 11, 99 }, 8, has_top).empty());
}

TEST_CASE("A role on every layer is sampled at evenly spread layers including both ends", "[Libvgcode]")
{
    const auto every = [](std::size_t) { return true; };
    CHECK(pick_sample_layers(Interval{ 0, 70 }, 8, every) == std::vector<std::size_t>{ 0, 10, 20, 30, 40, 50, 60, 70 });
    CHECK(pick_sample_layers(Interval{ 5, 7 }, 8, every) == std::vector<std::size_t>{ 5, 6, 7 });
    CHECK(pick_sample_layers(Interval{ 4, 4 }, 8, every) == std::vector<std::size_t>{ 4 });
}

TEST_CASE("Layers with the role among gaps are spread from the first to the last", "[Libvgcode]")
{
    // the role on every third layer from 2 to 59: twenty layers, of which eight are picked
    const auto every_third = [](std::size_t id) { return id >= 2 && id <= 59 && id % 3 == 2; };
    CHECK(pick_sample_layers(Interval{ 0, 70 }, 8, every_third) == std::vector<std::size_t>{ 2, 8, 17, 26, 32, 41, 50, 59 });
}

TEST_CASE("Where another object's layers take every pick, the other layers with the role are walked", "[Libvgcode]")
{
    // one object with top surfaces on layers 20 to 59, picked at 20, 25, 31, 36, 42, 47, 53 and 59,
    // and the other, inside the box, only on layer 21
    const auto has_top = [](std::size_t id) { return id >= 20 && id <= 59; };
    std::vector<std::size_t> visited;
    const auto first_in_box = [&visited](std::size_t id) {
        visited.push_back(id);
        return (id == 21) ? std::optional<std::size_t>(2100) : std::nullopt;
    };
    const auto no_samples = []() { return false; };
    CHECK(sample_role_layers(Interval{ 0, 70 }, 8, has_top, first_in_box, no_samples) == std::optional<std::size_t>(2100));
    // the eight picks, then the walk from the start of the interval, which skips the picked layer 20
    CHECK(visited == std::vector<std::size_t>{ 20, 25, 31, 36, 42, 47, 53, 59, 21 });
}

TEST_CASE("The walk stops at the first layer that yields samples, and never runs once the picks have", "[Libvgcode]")
{
    const auto every = [](std::size_t) { return true; };
    std::vector<std::size_t> visited;
    std::size_t sampled = 0;
    // only layer 33, which is not among the picks 0, 10, ... 70, has the role inside the box, and in
    // a graded view it yields samples
    const auto collect = [&](std::size_t id) {
        visited.push_back(id);
        sampled += (id == 33) ? 1 : 0;
        return std::optional<std::size_t>();
    };
    const auto has_samples = [&sampled]() { return sampled > 0; };
    CHECK(!sample_role_layers(Interval{ 0, 70 }, 8, every, collect, has_samples).has_value());
    CHECK(visited.back() == 33);
    // the eight picks and layers 1 to 33 less the picked 10, 20 and 30
    CHECK(visited.size() == 8 + 30);
    CHECK(sampled == 1);

    visited.clear();
    sampled = 0;
    // layer 10 is among the picks, so nothing past them is visited
    const auto collect_at_10 = [&](std::size_t id) {
        visited.push_back(id);
        sampled += (id == 10) ? 1 : 0;
        return std::optional<std::size_t>();
    };
    CHECK(!sample_role_layers(Interval{ 0, 70 }, 8, every, collect_at_10, has_samples).has_value());
    CHECK(visited == std::vector<std::size_t>{ 0, 10, 20, 30, 40, 50, 60, 70 });
}
