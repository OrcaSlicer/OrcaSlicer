#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/LayerTimeSmoothing.hpp"

#include <cfloat>
#include <cstddef>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

void check_variation(const std::vector<float> &t, float v, size_t first)
{
    for (size_t i = first + 1; i < t.size(); ++ i) {
        REQUIRE(t[i] >= t[i - 1] * (1.f - v) - 1e-2f);
        REQUIRE(t[i - 1] >= t[i] * (1.f - v) - 1e-2f);
    }
}

} // namespace

TEST_CASE("Layer time targets obey the variation limit", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 10.f, 100.f, 200.f, 50.f, 10.f };
    const std::vector<float> caps(times.size(), FLT_MAX);
    const std::vector<float> t = LayerTimeSmoother::solve_target_times(times, caps, 0.2f, 0);

    check_variation(t, 0.2f, 0);
    // Layers are only ever slowed down.
    for (size_t i = 0; i < t.size(); ++ i)
        REQUIRE(t[i] >= times[i]);
    // The slowest layer is not changed, its neighbours are raised to exactly 80% of it.
    CHECK_THAT(t[2], WithinAbs(200.f, 1e-3));
    CHECK_THAT(t[1], WithinAbs(160.f, 1e-3));
    CHECK_THAT(t[3], WithinAbs(160.f, 1e-3));
    CHECK_THAT(t[0], WithinAbs(128.f, 1e-3));
    CHECK_THAT(t[4], WithinAbs(128.f, 1e-3));
}

TEST_CASE("Layer time smoothing leaves the layers before the first layer untouched", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 300.f, 10.f, 10.f, 10.f };
    const std::vector<float> caps(times.size(), FLT_MAX);
    const std::vector<float> t = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 1);
    for (size_t i = 0; i < t.size(); ++ i)
        CHECK_THAT(t[i], WithinAbs(times[i], 1e-3));
}

TEST_CASE("Layer time caps stop the propagation of the variation limit", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 6.f, 6.f, 6.f, 40.f, 40.f };
    // Each layer may at most triple.
    std::vector<float> caps;
    for (float t : times)
        caps.emplace_back(t * 3.f);
    const std::vector<float> t = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 0);
    // The layer below the step hits its cap, the layers below it ramp down with 25% per layer.
    CHECK_THAT(t[2], WithinAbs(18.f, 1e-3));
    CHECK_THAT(t[1], WithinAbs(13.5f, 1e-3));
    CHECK_THAT(t[0], WithinAbs(10.125f, 1e-3));
    CHECK_THAT(t[3], WithinAbs(40.f, 1e-3));
    CHECK_THAT(t[4], WithinAbs(40.f, 1e-3));
}

TEST_CASE("The total time increase limit relaxes the layer time variation limit", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 5.f, 5.f, 5.f, 5.f, 50.f, 5.f, 5.f, 5.f, 5.f };
    const std::vector<float> caps(times.size(), FLT_MAX);
    float effective = 0.f;

    // Without a budget the increase is large.
    const std::vector<float> unlimited = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 100.f, 0, effective);
    CHECK_THAT(effective, WithinAbs(0.25f, 1e-3));
    double base = 0., increase_unlimited = 0.;
    for (size_t i = 0; i < times.size(); ++ i) {
        base += times[i];
        increase_unlimited += unlimited[i] - times[i];
    }
    CHECK(increase_unlimited > 0.5 * base);

    // With a 20% budget the variation is relaxed until the increase fits.
    const std::vector<float> limited = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 0.2f, 0, effective);
    CHECK(effective > 0.25f);
    CHECK(effective <= 1.f);
    double increase_limited = 0.;
    for (size_t i = 0; i < times.size(); ++ i) {
        REQUIRE(limited[i] >= times[i]);
        increase_limited += limited[i] - times[i];
    }
    CHECK(increase_limited <= 0.2 * base + 1e-2);
    check_variation(limited, effective, 0);
}

TEST_CASE("Layer time smoothing applies the per layer slow down and the minimum speed cap", "[LayerTimeSmoothing]")
{
    LayerTimeSmoother::Params params;
    params.max_variation      = 0.25f;
    params.max_layer_slowdown = 1.f;   // a layer may at most double
    params.max_total_increase = 100.f;
    LayerTimeSmoother smoother(params);

    const std::vector<float> times     = { 40.f, 4.f, 4.f, 40.f, 4.f };
    // The last layer cannot be slowed down further than 5 s by the minimum print speed.
    const std::vector<float> max_times = { 40.f, FLT_MAX, FLT_MAX, 40.f, 5.f };
    const std::vector<float> t = smoother.solve(times, max_times, 1);

    REQUIRE(t.size() == times.size());
    CHECK_THAT(t[0], WithinAbs(40.f, 1e-3));
    // Layer 2 is limited to twice its time by the per layer cap, layer 1 follows it with 25% variation.
    CHECK_THAT(t[2], WithinAbs(8.f, 1e-3));
    CHECK_THAT(t[1], WithinAbs(6.f, 1e-3));
    CHECK_THAT(t[4], WithinAbs(5.f, 1e-3));
}

TEST_CASE("The layer time smoothing comment reports the layer times", "[LayerTimeSmoothing]")
{
    LayerTimeSmoother smoother{LayerTimeSmoother::Params{}};
    const std::vector<float> times = { 10.f, 4.f };
    smoother.solve(times, { FLT_MAX, FLT_MAX }, 1);
    smoother.set_time_after(1, 8.f);
    CHECK(smoother.layer_comment(1) == "; LAYER_TIME_SMOOTHING factor=0.500 t_raw=4.00 t_out=8.00\n");
    CHECK(smoother.layer_comment(0) == "; LAYER_TIME_SMOOTHING factor=1.000 t_raw=10.00 t_out=10.00\n");
    CHECK(smoother.layer_comment(5).empty());
}
