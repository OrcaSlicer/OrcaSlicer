#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Result.hpp"
#include "core/Sampler.hpp"
#include "orcabench_test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;
using namespace std::chrono_literals;

namespace {

const Clock::time_point base = Clock::time_point {} + 1s;

// Four readings 5 ms apart, whose memory peaks at the second and whose CPU time only grows.
std::vector<Reading> readings()
{
    return {{base, 100, 0}, {base + 5ms, 300, 10}, {base + 10ms, 200, 30}, {base + 15ms, 250, 60}};
}

double value_of(const Metrics& metrics, const char* key)
{
    REQUIRE(metrics.count(key) == 1);
    return metrics.at(key);
}

} // namespace

TEST_CASE("the peak memory of a window is its highest reading, ends included, and a window without one has none", "[OrcaBench][Sampler]")
{
    CHECK(peak_rss(readings(), base, base + 15ms) == std::optional<std::uint64_t>(300));
    CHECK(peak_rss(readings(), base + 6ms, base + 14ms) == std::optional<std::uint64_t>(200));
    CHECK(peak_rss(readings(), base + 10ms, base + 10ms) == std::optional<std::uint64_t>(200));
    CHECK(peak_rss(readings(), base + 11ms, base + 15ms) == std::optional<std::uint64_t>(250));
    CHECK_FALSE(peak_rss(readings(), base + 11ms, base + 14ms).has_value());
}

TEST_CASE("a span's CPU is what the process used between the first and last readings inside it", "[OrcaBench][Sampler]")
{
    const Metrics metrics = sampled_metrics(readings(), base + 4ms, base + 16ms, true);
    CHECK_THAT(value_of(metrics, SampledMetric::peak_rss_bytes), Catch::Matchers::WithinAbs(300.0, 0.0));
    CHECK_THAT(value_of(metrics, SampledMetric::cpu_ns), Catch::Matchers::WithinAbs(50.0, 0.0));
    CHECK_THAT(value_of(metrics, SampledMetric::cpu_window_ns), Catch::Matchers::WithinAbs(10e6, 0.0));
}

TEST_CASE("a span with one reading inside has no CPU, and one with none has nothing", "[OrcaBench][Sampler]")
{
    const Metrics one = sampled_metrics(readings(), base + 4ms, base + 6ms, true);
    CHECK(one.size() == 1);
    CHECK_THAT(value_of(one, SampledMetric::peak_rss_bytes), Catch::Matchers::WithinAbs(300.0, 0.0));
    CHECK(sampled_metrics(readings(), base + 6ms, base + 9ms, true).empty());
}

TEST_CASE("a span's memory is left out when the run does not collect it", "[OrcaBench][Sampler]")
{
    const Metrics metrics = sampled_metrics(readings(), base, base + 15ms, false);
    CHECK(metrics.count(SampledMetric::peak_rss_bytes) == 0);
    CHECK(metrics.count(SampledMetric::cpu_ns) == 1);
}

TEST_CASE("a span whose CPU time went backwards has no CPU", "[OrcaBench][Sampler]")
{
    std::vector<Reading> backwards = readings();
    backwards.back().cpu_ns        = 5;
    const Metrics metrics          = sampled_metrics(backwards, base + 4ms, base + 16ms, true);
    CHECK(metrics.count(SampledMetric::cpu_ns) == 0);
    CHECK(metrics.count(SampledMetric::cpu_window_ns) == 0);
    CHECK(metrics.count(SampledMetric::peak_rss_bytes) == 1);
}

TEST_CASE("the sampler reads on its own thread until stopped, in the order it read", "[OrcaBench][Sampler]")
{
    ScriptedProcess process;
    Sampler         sampler(process.probe());
    sampler.start();
    process.await(3);
    const std::vector<Reading> taken = sampler.stop();
    CHECK(taken.size() >= 3);
    CHECK(std::is_sorted(taken.begin(), taken.end(), [](const Reading& a, const Reading& b) { return a.at < b.at; }));

    const std::uint64_t after_stop = process.reads;
    std::this_thread::sleep_for(10 * sampling_interval);
    CHECK(process.reads == after_stop);
}

TEST_CASE("a probe that throws on the sampler's thread is rethrown by stop()", "[OrcaBench][Sampler]")
{
    ScriptedProcess process;
    process.fail = true;
    Sampler sampler(process.probe());
    sampler.start();
    process.await(1);
    CHECK_THROWS_WITH(sampler.stop(), "the probe failed");
}

TEST_CASE("a reading from Host is taken now and finds memory in use", "[OrcaBench][Sampler]")
{
    const Clock::time_point before  = Clock::now();
    const Reading           reading = host_reading();
    const Clock::time_point after   = Clock::now();
    CHECK(reading.at >= before);
    CHECK(reading.at <= after);
    CHECK(reading.rss_bytes > 0);
}
