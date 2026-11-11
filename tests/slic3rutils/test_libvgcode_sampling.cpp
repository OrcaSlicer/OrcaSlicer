// The vertex that stands for a role when the solid model takes the toolpaths' colours: the
// length-weighted median of what the view grades by.
#include <catch2/catch_test_macros.hpp>

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
