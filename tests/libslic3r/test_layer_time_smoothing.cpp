#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/LayerTimeSmoothing.hpp"

#include <cfloat>
#include <cstddef>
#include <vector>

using namespace Slic3r;

namespace {

void check_variation(const std::vector<float> &t, float v, size_t first)
{
    for (size_t i = first + 1; i < t.size(); ++ i) {
        REQUIRE(t[i] >= t[i - 1] * (1.f - v) - 1e-2f);
        REQUIRE(t[i - 1] >= t[i] * (1.f - v) - 1e-2f);
    }
}

} // namespace

TEST_CASE("Layer time smoothing: target times obey the variation limit", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 10.f, 100.f, 200.f, 50.f, 10.f };
    const std::vector<float> caps(times.size(), FLT_MAX);
    const std::vector<float> t = LayerTimeSmoother::solve_target_times(times, caps, 0.2f, 0);

    check_variation(t, 0.2f, 0);
    // Layers are only ever slowed down.
    for (size_t i = 0; i < t.size(); ++ i)
        REQUIRE(t[i] >= times[i]);
    // The slowest layer is not changed, its neighbours are raised to exactly 80% of it.
    CHECK(t[2] == Catch::Approx(200.f));
    CHECK(t[1] == Catch::Approx(160.f));
    CHECK(t[3] == Catch::Approx(160.f));
    CHECK(t[0] == Catch::Approx(128.f));
    CHECK(t[4] == Catch::Approx(128.f));
}

TEST_CASE("Layer time smoothing: layers before first_layer are untouched and do not constrain", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 300.f, 10.f, 10.f, 10.f };
    const std::vector<float> caps(times.size(), FLT_MAX);
    const std::vector<float> t = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 1);
    for (size_t i = 0; i < t.size(); ++ i)
        CHECK(t[i] == Catch::Approx(times[i]));
}

TEST_CASE("Layer time smoothing: caps stop the propagation", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 6.f, 6.f, 6.f, 40.f, 40.f };
    // Each layer may at most triple.
    std::vector<float> caps;
    for (float t : times)
        caps.emplace_back(t * 3.f);
    const std::vector<float> t = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 0);
    // The layer below the step hits its cap, the layers below it ramp down with 25% per layer.
    CHECK(t[2] == Catch::Approx(18.f));
    CHECK(t[1] == Catch::Approx(13.5f));
    CHECK(t[0] == Catch::Approx(10.125f));
    CHECK(t[3] == Catch::Approx(40.f));
    CHECK(t[4] == Catch::Approx(40.f));
}

TEST_CASE("Layer time smoothing: total time increase limit relaxes the variation", "[LayerTimeSmoothing]")
{
    const std::vector<float> times = { 5.f, 5.f, 5.f, 5.f, 50.f, 5.f, 5.f, 5.f, 5.f };
    const std::vector<float> caps(times.size(), FLT_MAX);
    float effective = 0.f;

    // Without a budget the increase is large.
    const std::vector<float> unlimited = LayerTimeSmoother::solve_target_times(times, caps, 0.25f, 100.f, 0, effective);
    CHECK(effective == Catch::Approx(0.25f));
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

TEST_CASE("Layer time smoothing: solve applies the per layer slow down and the minimum speed cap", "[LayerTimeSmoothing]")
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
    CHECK(t[0] == Catch::Approx(40.f));
    // Layer 2 is limited to twice its time by the per layer cap, layer 1 follows it with 25% variation.
    CHECK(t[2] == Catch::Approx(8.f));
    CHECK(t[1] == Catch::Approx(6.f));
    CHECK(t[4] == Catch::Approx(5.f));
}

TEST_CASE("Layer time smoothing: the diagnostic comment reports the layer times", "[LayerTimeSmoothing]")
{
    LayerTimeSmoother smoother{LayerTimeSmoother::Params{}};
    const std::vector<float> times = { 10.f, 4.f };
    smoother.solve(times, { FLT_MAX, FLT_MAX }, 1);
    smoother.set_time_after(1, 8.f);
    CHECK(smoother.layer_comment(1) == "; LAYER_TIME_SMOOTHING factor=0.500 t_raw=4.00 t_out=8.00\n");
    CHECK(smoother.layer_comment(0) == "; LAYER_TIME_SMOOTHING factor=1.000 t_raw=10.00 t_out=10.00\n");
    CHECK(smoother.layer_comment(5).empty());
}
