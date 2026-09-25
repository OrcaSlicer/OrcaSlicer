#include <catch2/catch_all.hpp>

#include "slic3r/GUI/SceneBenchmark.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using Slic3r::GUI::frame_time_stats;
using Slic3r::GUI::FrameTimeStats;

TEST_CASE("Frame time statistics take percentiles by nearest rank", "[SceneBenchmark]")
{
    // 1 to 100 ms in shuffled order: the p-th percentile is the p-th frame time.
    std::vector<double> frame_ms(100);
    std::iota(frame_ms.begin(), frame_ms.end(), 1.0);
    std::shuffle(frame_ms.begin(), frame_ms.end(), std::mt19937(42));

    const FrameTimeStats stats = frame_time_stats(frame_ms);
    CHECK_THAT(stats.average_ms, WithinAbs(50.5, 1e-9));
    CHECK_THAT(stats.fps, WithinAbs(1000.0 / 50.5, 1e-9));
    CHECK_THAT(stats.median_ms, WithinAbs(50.0, 1e-9));
    CHECK_THAT(stats.p95_ms, WithinAbs(95.0, 1e-9));
    CHECK_THAT(stats.p99_ms, WithinAbs(99.0, 1e-9));
    CHECK_THAT(stats.max_ms, WithinAbs(100.0, 1e-9));
}

TEST_CASE("Frame time percentiles are measured frame times", "[SceneBenchmark]")
{
    // 360 frames, as in a benchmark pass: rank 342 for p95 and rank 357 for p99.
    std::vector<double> frame_ms(360);
    std::iota(frame_ms.begin(), frame_ms.end(), 1.0);

    const FrameTimeStats stats = frame_time_stats(frame_ms);
    CHECK_THAT(stats.median_ms, WithinAbs(180.0, 1e-9));
    CHECK_THAT(stats.p95_ms, WithinAbs(342.0, 1e-9));
    CHECK_THAT(stats.p99_ms, WithinAbs(357.0, 1e-9));
}

TEST_CASE("Frame time statistics of a single frame are that frame", "[SceneBenchmark]")
{
    const FrameTimeStats stats = frame_time_stats({ 4.0 });
    CHECK_THAT(stats.fps, WithinAbs(250.0, 1e-9));
    CHECK_THAT(stats.median_ms, WithinAbs(4.0, 1e-9));
    CHECK_THAT(stats.p99_ms, WithinAbs(4.0, 1e-9));
    CHECK_THAT(stats.max_ms, WithinAbs(4.0, 1e-9));
}

TEST_CASE("Frame time statistics of no frames are zero", "[SceneBenchmark]")
{
    const FrameTimeStats stats = frame_time_stats({});
    CHECK_THAT(stats.fps, WithinAbs(0.0, 1e-9));
    CHECK_THAT(stats.average_ms, WithinAbs(0.0, 1e-9));
    CHECK_THAT(stats.max_ms, WithinAbs(0.0, 1e-9));
}
