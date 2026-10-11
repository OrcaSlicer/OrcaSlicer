#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Result.hpp"

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Slic3r::Bench;
using namespace std::chrono_literals;

namespace {

StageSpan span_at(std::string stage, Clock::duration begin, Clock::duration length)
{
    StageSpan span;
    span.stage      = std::move(stage);
    span.scope      = "print";
    span.started_at = Clock::time_point {} + begin;
    span.done_at    = span.started_at + length;
    return span;
}

} // namespace

TEST_CASE("an iteration without spans or unfinished steps has the epoch as its origin", "[OrcaBench][Result]")
{
    CHECK(origin(IterationResult {}) == Clock::time_point {});
}

TEST_CASE("the origin is the earliest start in an unsorted timeline", "[OrcaBench][Result]")
{
    IterationResult iteration;
    iteration.timeline = { span_at("posInfill", 40ms, 10ms), span_at("posSlice", 10ms, 60ms) };

    CHECK(origin(iteration) == Clock::time_point {} + 10ms);
}

TEST_CASE("an unfinished step that started before every span is the origin", "[OrcaBench][Result]")
{
    IterationResult iteration;
    iteration.timeline   = { span_at("posSlice", 10ms, 60ms) };
    iteration.unfinished = { { "posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 5ms } };

    CHECK(origin(iteration) == Clock::time_point {} + 5ms);
}

TEST_CASE("each outcome is counted from the workload list", "[OrcaBench][Result]")
{
    Result result;
    result.workloads.push_back(WorkloadResult::ran("a", 1, {}, { IterationResult {} }));
    result.workloads.push_back(WorkloadResult::ran("b", 2, {}, { IterationResult {} }));
    result.workloads.push_back(WorkloadResult::skipped("c", "corpus not fetched"));
    result.workloads.push_back(WorkloadResult::failed("d", "threw std::bad_alloc"));

    CHECK(count_of(result, Outcome::Ran) == 2);
    CHECK(count_of(result, Outcome::Skipped) == 1);
    CHECK(count_of(result, Outcome::Failed) == 1);

    const std::size_t counted = count_of(result, Outcome::Ran) + count_of(result, Outcome::Skipped) +
                                count_of(result, Outcome::Failed);
    CHECK(counted == result.workloads.size());
}

TEST_CASE("a workload that did not run says why", "[OrcaBench][Result]")
{
    SECTION("skipped") {
        const auto workload = WorkloadResult::skipped("slice/benchy", "corpus not fetched");
        CHECK(workload.outcome == Outcome::Skipped);
        CHECK(workload.reason == "corpus not fetched");
    }
    SECTION("failed") {
        const auto workload = WorkloadResult::failed("slice/benchy", "threw std::bad_alloc");
        CHECK(workload.outcome == Outcome::Failed);
        CHECK(workload.reason == "threw std::bad_alloc");
    }
}

TEST_CASE("a workload that did not run and gives no reason is refused", "[OrcaBench][Result]")
{
    CHECK_THROWS_AS(WorkloadResult::skipped("slice/benchy", ""), std::invalid_argument);
    CHECK_THROWS_AS(WorkloadResult::failed("slice/benchy", ""), std::invalid_argument);
}

TEST_CASE("a skipped workload is not counted as ran and has no iterations", "[OrcaBench][Result]")
{
    Result result;
    result.workloads.push_back(WorkloadResult::skipped("slice/benchy", "corpus not fetched"));

    CHECK(count_of(result, Outcome::Ran) == 0);
    CHECK(result.workloads.front().iterations.empty());
}

TEST_CASE("a workload that ran has its measurements and no reason", "[OrcaBench][Result]")
{
    WorkStats work;
    work.moves = 606972;

    IterationResult iteration;
    iteration.wall = 1131ms;

    const auto workload = WorkloadResult::ran("slice/idler", 0x3bbecdba, work, { iteration });

    CHECK(workload.outcome == Outcome::Ran);
    CHECK(workload.reason.empty());
    REQUIRE(workload.output_hash.has_value());
    CHECK(*workload.output_hash == 0x3bbecdba);
    REQUIRE(workload.work.has_value());
    CHECK(workload.work->moves == 606972);
    REQUIRE(workload.iterations.size() == 1);
    CHECK(workload.iterations.front().wall == 1131ms);
}

TEST_CASE("named metrics are stored with the span or iteration they describe", "[OrcaBench][Result]")
{
    StageSpan span = span_at("posInfill", 0ms, 1ms);
    span.metrics["peak_rss_bytes"] = 431906816.0;

    IterationResult iteration;
    iteration.metrics["retired_instructions"] = 4.2e11;
    iteration.timeline.push_back(span);

    const auto workload = WorkloadResult::ran("slice/idler", 0, {}, { iteration });
    const auto& stored  = workload.iterations.front();

    CHECK_THAT(stored.metrics.at("retired_instructions"), Catch::Matchers::WithinRel(4.2e11));
    CHECK_THAT(stored.timeline.front().metrics.at("peak_rss_bytes"), Catch::Matchers::WithinRel(431906816.0));
}
