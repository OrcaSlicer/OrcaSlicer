#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <optional>

#include "slic3r/GUI/PreviewFrameRateMonitor.hpp"

using Slic3r::GUI::PreviewFrameRateMonitor;
using Catch::Matchers::WithinAbs;

namespace {

// Feeds `count` frames of `frame_ms` and returns how many of them reported a slow preview.
int count_reports(PreviewFrameRateMonitor& monitor, int count, double frame_ms)
{
    int reports = 0;
    for (int i = 0; i < count; ++i)
        if (monitor.add_frame(frame_ms))
            ++reports;
    return reports;
}

} // namespace

TEST_CASE("Fast frames never report a slow preview", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    // 10 ms frames are 100 fps.
    REQUIRE(count_reports(monitor, 1000, 10.) == 0);
}

TEST_CASE("Five slow seconds in a row report the average frame rate", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    // 100 ms frames are 10 fps: a one-second bucket closes every 10 frames, so the fifth closes on frame 50.
    REQUIRE(count_reports(monitor, 49, 100.) == 0);
    const std::optional<double> fps = monitor.add_frame(100.);
    REQUIRE(fps.has_value());
    REQUIRE_THAT(*fps, WithinAbs(10., 0.01));
}

TEST_CASE("A fast second resets the slow streak", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    REQUIRE(count_reports(monitor, 40, 100.) == 0); // four slow seconds
    REQUIRE(count_reports(monitor, 100, 10.) == 0); // one fast second
    REQUIRE(count_reports(monitor, 40, 100.) == 0); // four slow seconds again
}

TEST_CASE("Frames just faster than 15 fps never report a slow preview", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    // 66 ms is 15.15 fps.
    REQUIRE(count_reports(monitor, 1000, 66.) == 0);
}

TEST_CASE("Frames just slower than 15 fps report a slow preview", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    // 68 ms is 14.7 fps.
    REQUIRE(count_reports(monitor, 1000, 68.) > 0);
}

TEST_CASE("A single long frame counts as one slow second", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    // The first frame after a load can take seconds; on its own it must not report.
    REQUIRE_FALSE(monitor.add_frame(3000.).has_value());
    REQUIRE(count_reports(monitor, 100, 10.) == 0);
}

TEST_CASE("Reset drops a partial slow streak", "[PreviewFrameRateMonitor]")
{
    PreviewFrameRateMonitor monitor;
    REQUIRE(count_reports(monitor, 40, 100.) == 0);
    monitor.reset();
    REQUIRE(count_reports(monitor, 40, 100.) == 0);
}
