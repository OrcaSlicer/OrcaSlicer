// The vertex that stands for a role when the solid model takes the toolpaths' colours: the
// length-weighted median of what the view grades by.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include "libvgcode/src/Utils.hpp"

using libvgcode::weighted_median_index;

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
