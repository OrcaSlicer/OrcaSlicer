#include <catch2/catch_all.hpp>
#include "libslic3r/FilamentMixerModel.hpp"

TEST_CASE("Neutral pigment mixtures stay neutral and between their endpoints", "[FilamentMixerModel][Regression]")
{
    const auto a = GENERATE(0, 64, 180, 255);
    const auto b = GENERATE(0, 64, 180, 255);
    const auto t = GENERATE(0.0f, 0.2f, 0.5f, 0.8f, 1.0f);
    const auto mixed = filament_mixer::lerp(a, a, a, b, b, b, t);
    REQUIRE(mixed.r == mixed.g);
    REQUIRE(mixed.g == mixed.b);
    REQUIRE(mixed.r >= std::min(a, b));
    REQUIRE(mixed.r <= std::max(a, b));
}

TEST_CASE("Mixing identical pigment colors preserves the source color", "[FilamentMixerModel][Regression]")
{
    const auto t = GENERATE(0.2f, 0.5f, 0.8f);
    const auto mixed = filament_mixer::lerp(212, 161, 133, 212, 161, 133, t);
    REQUIRE(mixed.r == 212);
    REQUIRE(mixed.g == 161);
    REQUIRE(mixed.b == 133);
}

TEST_CASE("Chromatic pigment mixtures retain the regression estimate", "[FilamentMixerModel]")
{
    const auto mixed = filament_mixer::lerp(0, 0, 0, 212, 161, 133, 0.5f);
    REQUIRE(mixed.r == 83);
    REQUIRE(mixed.g == 82);
    REQUIRE(mixed.b == 88);
}
