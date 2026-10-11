#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Result.hpp"
#include "core/Sampler.hpp"
#include "core/Summary.hpp"
#include "orcabench_test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

IterationResult iteration_of(Clock::duration wall, Timeline timeline)
{
    IterationResult iteration;
    iteration.wall     = wall;
    iteration.timeline = std::move(timeline);
    return iteration;
}

WorkloadResult ran(std::vector<IterationResult> iterations)
{
    return WorkloadResult::ran("slice/idler", std::nullopt, std::nullopt, std::move(iterations));
}

// A header recorded at `threads` on a machine of 20 cores that advances CPU time in `step`, where
// zero records no step.
Result recorded_with(Clock::duration step, std::string threads = "20")
{
    Result header;
    header.machine.logical_cores                = 20;
    header.measurement[MeasurementKey::threads] = std::move(threads);
    if (step > Clock::duration::zero())
        header.machine.properties[MachineProperty::cpu_time_step_ns] = std::to_string(step.count());
    return header;
}

const Result fine_grained = recorded_with(Clock::duration::zero());

const StageRow& row_of(const WorkloadSummary& summary, const std::string& stage, const std::string& scope = "")
{
    const auto found = std::find_if(summary.rows.begin(), summary.rows.end(),
                                    [&](const StageRow& row) { return row.stage == stage && row.scope == scope; });
    REQUIRE(found != summary.rows.end());
    return *found;
}

// Infill on two objects and the export, over two iterations.
WorkloadResult two_objects()
{
    return ran({iteration_of(120ms, {span_of("posInfill", "object:0", 0ms, 10ms), span_of("posInfill", "object:1", 0ms, 20ms),
                                     span_of("psGCodeExport", "print", 30ms, 70ms)}),
                iteration_of(120ms, {span_of("posInfill", "object:0", 0ms, 15ms), span_of("posInfill", "object:1", 0ms, 25ms),
                                     span_of("psGCodeExport", "print", 40ms, 60ms)})});
}

StageRow row_with(std::string stage, double share, StageState state = StageState::Ran)
{
    StageRow row;
    row.stage      = std::move(stage);
    row.state      = state;
    row.unfinished = state == StageState::Unfinished;
    row.share      = share;
    row.mean       = Millis(100 * share);
    return row;
}

std::vector<std::string> stages_of(const std::vector<StageRow>& rows)
{
    std::vector<std::string> stages;
    for (const StageRow& row : rows)
        stages.push_back(row.stage);
    return stages;
}

} // namespace

TEST_CASE("a row sums its stage across objects in each iteration, then takes the mean and min", "[OrcaBench][Summary]")
{
    const WorkloadSummary summary = summarize(two_objects(), fine_grained, false);
    REQUIRE(summary.rows.size() == 2);
    CHECK_THAT(summary.summed_work.count(), WithinAbs(100.0, 1e-9));

    const StageRow& infill = row_of(summary, "posInfill");
    CHECK_THAT(infill.mean.count(), WithinAbs(35.0, 1e-9));
    CHECK_THAT(infill.min.count(), WithinAbs(30.0, 1e-9));
    REQUIRE(infill.cv.has_value());
    CHECK_THAT(*infill.cv, WithinRel(std::sqrt(50.0) / 35, 1e-12));
    CHECK_THAT(infill.share, WithinAbs(0.35, 1e-12));

    const StageRow& gcode = row_of(summary, "psGCodeExport");
    CHECK_THAT(gcode.mean.count(), WithinAbs(65.0, 1e-9));
    CHECK_THAT(gcode.min.count(), WithinAbs(60.0, 1e-9));
    CHECK_THAT(gcode.share, WithinAbs(0.65, 1e-12));
}

TEST_CASE("under --verbose each scope of a stage has its own row", "[OrcaBench][Summary]")
{
    const WorkloadSummary summary = summarize(two_objects(), fine_grained, true);
    REQUIRE(summary.rows.size() == 3);
    CHECK_THAT(row_of(summary, "posInfill", "object:0").mean.count(), WithinAbs(12.5, 1e-9));
    CHECK_THAT(row_of(summary, "posInfill", "object:1").mean.count(), WithinAbs(22.5, 1e-9));
    CHECK_THAT(row_of(summary, "psGCodeExport", "print").mean.count(), WithinAbs(65.0, 1e-9));
}

TEST_CASE("a row has no CV with one iteration", "[OrcaBench][Summary]")
{
    const WorkloadSummary summary = summarize(ran({iteration_of(10ms, {span_of("posSlice", "object:0", 0ms, 5ms)})}), fine_grained, false);
    CHECK_FALSE(row_of(summary, "posSlice").cv.has_value());
}

TEST_CASE("the wall envelope is the mean wall time, and unaccounted the time no span covers", "[OrcaBench][Summary]")
{
    // Two objects side by side, so the spans add up to more time than they cover.
    const Timeline spans {span_of("posInfill", "object:0", 0ms, 30ms), span_of("posPerimeters", "object:1", 10ms, 40ms),
                          span_of("psGCodeExport", "print", 60ms, 30ms)};
    const WorkloadSummary summary = summarize(ran({iteration_of(100ms, spans), iteration_of(120ms, spans)}), fine_grained, false);
    CHECK_THAT(summary.summed_work.count(), WithinAbs(100.0, 1e-9));
    CHECK_THAT(summary.wall.count(), WithinAbs(110.0, 1e-9));
    CHECK_THAT(summary.unaccounted.count(), WithinAbs(30.0, 1e-9));
}

TEST_CASE("each stage is in one of four states", "[OrcaBench][Summary]")
{
    IterationResult iteration =
        iteration_of(10ms, {span_of("posSlice", "object:0", 0ms, 5ms), span_of("posContouring", "object:0", 5ms, 0ms)});
    iteration.unfinished      = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 6ms}};
    iteration.not_run         = {{"posSimplifySupportPath", "object:0"}};
    const WorkloadSummary summary = summarize(ran({iteration}), fine_grained, false);
    CHECK(row_of(summary, "posSlice").state == StageState::Ran);
    CHECK(row_of(summary, "posContouring").state == StageState::Instant);
    CHECK(row_of(summary, "posEstimateCurledExtrusions").state == StageState::Unfinished);
    CHECK(row_of(summary, "posEstimateCurledExtrusions").unfinished);
    CHECK(row_of(summary, "posSimplifySupportPath").state == StageState::NotRun);
    CHECK_FALSE(row_of(summary, "posSlice").unfinished);
}

TEST_CASE("a stage that ran in one scope and never finished in another is a stage that ran, marked unfinished", "[OrcaBench][Summary]")
{
    IterationResult iteration     = iteration_of(10ms, {span_of("posEstimateCurledExtrusions", "object:1", 0ms, 3ms)});
    iteration.unfinished          = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 1ms}};
    const WorkloadSummary summary = summarize(ran({iteration}), fine_grained, false);
    const StageRow&       row     = row_of(summary, "posEstimateCurledExtrusions");
    CHECK(row.state == StageState::Ran);
    CHECK(row.unfinished);
}

TEST_CASE("a row's figures come from the iterations with a span of it, and its share from every iteration", "[OrcaBench][Summary]")
{
    const auto curled = [](Clock::duration length) {
        return iteration_of(50ms,
                            {span_of("posSlice", "object:0", 0ms, 32ms), span_of("posEstimateCurledExtrusions", "object:0", 32ms, length)});
    };
    IterationResult unfinished       = iteration_of(50ms, {span_of("posSlice", "object:0", 0ms, 32ms)});
    unfinished.unfinished            = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 32ms}};
    const WorkloadSummary summary    = summarize(ran({curled(10ms), unfinished, curled(14ms)}), fine_grained, false);
    const StageRow&       curled_row = row_of(summary, "posEstimateCurledExtrusions");
    CHECK(curled_row.unfinished);
    CHECK_THAT(curled_row.mean.count(), WithinAbs(12.0, 1e-9));
    CHECK_THAT(curled_row.min.count(), WithinAbs(10.0, 1e-9));
    REQUIRE(curled_row.cv.has_value());
    CHECK_THAT(*curled_row.cv, WithinRel(std::sqrt(8.0) / 12, 1e-12));
    CHECK_THAT(curled_row.share, WithinAbs(0.2, 1e-12));
}

TEST_CASE("a stage is instant only when every span of it took no time", "[OrcaBench][Summary]")
{
    const WorkloadSummary summary = summarize(ran({iteration_of(10ms, {span_of("posContouring", "object:0", 0ms, 1us)}),
                                                   iteration_of(10ms, {span_of("posContouring", "object:0", 0ms, 0ms)})}),
                                              fine_grained, false);
    CHECK(row_of(summary, "posContouring").state == StageState::Ran);
}

TEST_CASE("a stage that ran in one scope is a stage that ran, though each scope keeps its own state", "[OrcaBench][Summary]")
{
    IterationResult iteration = iteration_of(10ms, {span_of("posSupportMaterial", "object:1", 0ms, 3ms)});
    iteration.not_run         = {{"posSupportMaterial", "object:0"}};
    const WorkloadResult workload = ran({iteration});

    const WorkloadSummary merged = summarize(workload, fine_grained, false);
    CHECK(row_of(merged, "posSupportMaterial").state == StageState::Ran);
    CHECK_THAT(row_of(merged, "posSupportMaterial").mean.count(), WithinAbs(3.0, 1e-9));

    const WorkloadSummary verbose = summarize(workload, fine_grained, true);
    CHECK(row_of(verbose, "posSupportMaterial", "object:0").state == StageState::NotRun);
    CHECK(row_of(verbose, "posSupportMaterial", "object:1").state == StageState::Ran);
}

TEST_CASE("a row's first start is its earliest offset from its iteration's origin", "[OrcaBench][Summary]")
{
    IterationResult first = iteration_of(200ms, {span_of("posSlice", "object:0", 0ms, 10ms), span_of("posInfill", "object:0", 30ms, 10ms)});
    first.not_run         = {{"posContouring", "object:0"}};
    IterationResult second =
        iteration_of(200ms, {span_of("posSlice", "object:0", 100ms, 10ms), span_of("posInfill", "object:0", 125ms, 10ms)});
    second.unfinished = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 160ms}};
    second.not_run    = {{"posContouring", "object:0"}};
    const WorkloadSummary summary = summarize(ran({first, second}), fine_grained, false);
    CHECK(row_of(summary, "posSlice").first_start == 0ms);
    CHECK(row_of(summary, "posInfill").first_start == 25ms);
    CHECK(row_of(summary, "posEstimateCurledExtrusions").first_start == 60ms);
    CHECK(row_of(summary, "posContouring").first_start == Clock::duration::max());
}

TEST_CASE("a row's CPU is its CPU time over its windows, and its peak the lowest iteration's highest reading", "[OrcaBench][Summary]")
{
    const Metrics first  = {{SampledMetric::cpu_ns, 30e6}, {SampledMetric::cpu_window_ns, 10e6}, {SampledMetric::peak_rss_bytes, 500}};
    const Metrics second = {{SampledMetric::cpu_ns, 50e6}, {SampledMetric::cpu_window_ns, 10e6}, {SampledMetric::peak_rss_bytes, 700}};
    const Metrics other  = {{SampledMetric::peak_rss_bytes, 600}};
    const WorkloadSummary summary = summarize(ran({iteration_of(20ms, {span_of("posInfill", "object:0", 0ms, 12ms, first),
                                                                       span_of("posInfill", "object:1", 0ms, 12ms, other)}),
                                                   iteration_of(20ms, {span_of("posInfill", "object:0", 0ms, 12ms, second)})}),
                                              fine_grained, false);
    const StageRow& infill = row_of(summary, "posInfill");
    REQUIRE(infill.cpu.has_value());
    CHECK_THAT(*infill.cpu, WithinAbs(4.0, 1e-12));
    CHECK(infill.peak_rss_bytes == std::optional<std::uint64_t>(600));
}

TEST_CASE("a row leaves out a peak reading that no count of bytes can be", "[OrcaBench][Summary]")
{
    const double peak = GENERATE(-1.0, 1e20);
    CAPTURE(peak);
    const Metrics         readings = {{SampledMetric::peak_rss_bytes, peak}};
    const WorkloadSummary summary =
        summarize(ran({iteration_of(20ms, {span_of("posInfill", "object:0", 0ms, 12ms, readings)})}), fine_grained, false);
    CHECK_FALSE(row_of(summary, "posInfill").peak_rss_bytes.has_value());
}

TEST_CASE("a row without sampled readings has no CPU and no peak", "[OrcaBench][Summary]")
{
    const WorkloadSummary summary =
        summarize(ran({iteration_of(20ms, {span_of("posInfill", "object:0", 0ms, 12ms)})}), fine_grained, false);
    const StageRow&       infill  = row_of(summary, "posInfill");
    CHECK_FALSE(infill.cpu.has_value());
    CHECK_FALSE(infill.cpu_below_floor);
    CHECK_FALSE(infill.peak_rss_bytes.has_value());
}

TEST_CASE("nothing is divided by a total of no time", "[OrcaBench][Summary]")
{
    SECTION("spans that all took no time give their rows no share")
    {
        const WorkloadSummary summary =
            summarize(ran({iteration_of(10ms, {span_of("posContouring", "object:0", 0ms, 0ms)})}), fine_grained, false);
        CHECK_THAT(row_of(summary, "posContouring").share, WithinAbs(0.0, 0.0));
    }
    SECTION("CPU time read over no time gives no CPU")
    {
        const Metrics         readings = {{SampledMetric::cpu_ns, 1e6}, {SampledMetric::cpu_window_ns, 0.0}};
        const WorkloadSummary summary =
            summarize(ran({iteration_of(10ms, {span_of("posInfill", "object:0", 0ms, 5ms, readings)})}), fine_grained, false);
        CHECK_FALSE(row_of(summary, "posInfill").cpu.has_value());
    }
    SECTION("passes that took no time give the workload no CPU")
    {
        IterationResult iteration = iteration_of(0ms, {});
        iteration.cpu             = 5ms;
        CHECK_FALSE(summarize(ran({iteration}), fine_grained, false).cpu.has_value());
    }
}

TEST_CASE("a row whose span overlaps a span outside it is marked shared", "[OrcaBench][Summary]")
{
    SECTION("spans of different stages side by side")
    {
        const WorkloadSummary summary = summarize(ran({iteration_of(100ms, {span_of("posInfill", "object:0", 0ms, 30ms),
                                                                            span_of("posPerimeters", "object:1", 10ms, 30ms),
                                                                            span_of("psGCodeExport", "print", 40ms, 20ms)})}),
                                                  fine_grained, false);
        CHECK(row_of(summary, "posInfill").shared);
        CHECK(row_of(summary, "posPerimeters").shared);
        // It starts as posPerimeters ends, which is not an overlap.
        CHECK_FALSE(row_of(summary, "psGCodeExport").shared);
    }
    SECTION("one stage on two objects, which only --verbose splits")
    {
        const WorkloadResult workload =
            ran({iteration_of(100ms, {span_of("posInfill", "object:0", 0ms, 30ms), span_of("posInfill", "object:1", 5ms, 20ms)})});
        CHECK_FALSE(row_of(summarize(workload, fine_grained, false), "posInfill").shared);
        CHECK(row_of(summarize(workload, fine_grained, true), "posInfill", "object:0").shared);
    }
    SECTION("a long span that outlasts the spans after it, recorded as each one finished")
    {
        const WorkloadSummary summary = summarize(ran({iteration_of(100ms, {span_of("posPerimeters", "object:1", 10ms, 10ms),
                                                                            span_of("posSupportMaterial", "object:1", 50ms, 10ms),
                                                                            span_of("posInfill", "object:0", 0ms, 100ms)})}),
                                                  fine_grained, false);
        CHECK(row_of(summary, "posPerimeters").shared);
        CHECK(row_of(summary, "posSupportMaterial").shared);
        CHECK(row_of(summary, "posInfill").shared);
    }
    SECTION("a span that took no time as another starts, which shares nothing")
    {
        const WorkloadSummary summary = summarize(
            ran({iteration_of(100ms, {span_of("posInfill", "object:0", 0ms, 30ms), span_of("posContouring", "object:0", 0ms, 0ms)})}),
            fine_grained, false);
        CHECK_FALSE(row_of(summary, "posInfill").shared);
        CHECK_FALSE(row_of(summary, "posContouring").shared);
    }
}

TEST_CASE("a recorded CPU time step is read only from a whole count of nanoseconds", "[OrcaBench][Summary]")
{
    const auto [text, nanoseconds] = GENERATE(table<std::string, long long>({
        {"15625000", 15625000},
        {"15625000 ns", 0},
        {"-15625000", 0},
        {"", 0},
    }));
    CAPTURE(text);
    MachineIdentity machine;
    machine.properties[MachineProperty::cpu_time_step_ns] = text;
    CHECK(recorded_cpu_time_step(machine) == std::chrono::nanoseconds(nanoseconds));
}

TEST_CASE("CPU shows only when the recorded step's rounding is at most a tenth of it", "[OrcaBench][Summary]")
{
    // The run's threads and the sampler's each round by a step, 328.125 ms at 20 threads and 31.25 ms at one.
    const auto [step, threads, cpu_ms, shown] = GENERATE(table<Clock::duration, std::string, double, bool>({
        {15625us, "1", 400.0, true},
        {15625us, "1", 200.0, false},
        {15625us, "20", 4000.0, true},
        {15625us, "20", 3200.0, false},
        {15625us, "", 2000.0, false},
        {0us, "20", 1.0, true},
    }));
    CAPTURE(step, threads, cpu_ms);
    const Metrics readings = {{SampledMetric::cpu_ns, cpu_ms * 1e6}, {SampledMetric::cpu_window_ns, 1e9}};
    const WorkloadSummary summary =
        summarize(ran({iteration_of(2s, {span_of("posInfill", "object:0", 0ms, 1s, readings)})}), recorded_with(step, threads), false);
    const StageRow& infill = row_of(summary, "posInfill");
    CHECK(infill.cpu.has_value() == shown);
    CHECK(infill.cpu_below_floor == !shown);
}

TEST_CASE("a workload's CPU is its CPU time over its wall time, floored the same way", "[OrcaBench][Summary]")
{
    const auto run = [](Clock::duration first_cpu, Clock::duration second_cpu) {
        IterationResult first = iteration_of(1s, {});
        first.cpu             = first_cpu;
        IterationResult second = iteration_of(1s, {});
        second.cpu             = second_cpu;
        return ran({first, second});
    };
    const WorkloadSummary fine_grained_summary = summarize(run(8s, 6s), fine_grained, false);
    REQUIRE(fine_grained_summary.cpu.has_value());
    CHECK_THAT(*fine_grained_summary.cpu, WithinAbs(7.0, 1e-12));

    const Result stepped = recorded_with(15625us);
    CHECK(summarize(run(8s, 6s), stepped, false).cpu.has_value());
    const WorkloadSummary floored = summarize(run(2s, 2s), stepped, false);
    CHECK_FALSE(floored.cpu.has_value());
    CHECK(floored.cpu_below_floor);
}

TEST_CASE("a workload's peak memory is its lowest iteration's", "[OrcaBench][Summary]")
{
    std::vector<IterationResult> iterations(3, iteration_of(1s, {}));
    iterations[0].peak_rss_bytes = 200;
    iterations[1].peak_rss_bytes = 100;
    iterations[2].peak_rss_bytes = 300;
    CHECK(summarize(ran(iterations), fine_grained, false).peak_rss_bytes == std::optional<std::uint64_t>(100));
    CHECK_FALSE(summarize(ran({iteration_of(1s, {})}), fine_grained, false).peak_rss_bytes.has_value());
}

TEST_CASE("a row under the threshold folds into the other row, and one at it stays", "[OrcaBench][Summary]")
{
    std::vector<StageRow>         rows {row_with("posSlice", 0.009), row_with("posPerimeters", 0.01), row_with("posInfill", 0.981)};
    const std::optional<OtherRow> other = collapse(rows, 0.01);
    REQUIRE(other.has_value());
    CHECK(other->stages == 1);
    CHECK(other->not_run == 0);
    CHECK_THAT(other->mean.count(), WithinAbs(0.9, 1e-12));
    CHECK_THAT(other->share, WithinAbs(0.009, 1e-12));
    CHECK(stages_of(rows) == std::vector<std::string> {"posPerimeters", "posInfill"});
}

TEST_CASE("the other row counts the stages that never ran", "[OrcaBench][Summary]")
{
    std::vector<StageRow> rows {row_with("posContouring", 0, StageState::NotRun),
                                row_with("posSimplifySupportPath", 0, StageState::Instant), row_with("posSlice", 0.005),
                                row_with("posInfill", 0.995)};
    const std::optional<OtherRow> other = collapse(rows, 0.01);
    REQUIRE(other.has_value());
    CHECK(other->stages == 3);
    CHECK(other->not_run == 1);
}

TEST_CASE("a row that never finished somewhere never folds", "[OrcaBench][Summary]")
{
    StageRow partly   = row_with("posSupportMaterial", 0.002);
    partly.unfinished = true;
    std::vector<StageRow> rows {row_with("posEstimateCurledExtrusions", 0, StageState::Unfinished), partly,
                                row_with("posPerimeters", 0.002), row_with("posInfill", 0.996)};
    const std::optional<OtherRow> other = collapse(rows, 0.01);
    REQUIRE(other.has_value());
    CHECK(other->stages == 1);
    CHECK(stages_of(rows) == std::vector<std::string> {"posEstimateCurledExtrusions", "posSupportMaterial", "posInfill"});
}

TEST_CASE("no other row is made when nothing is under the threshold", "[OrcaBench][Summary]")
{
    std::vector<StageRow> rows {row_with("posInfill", 0.4), row_with("psGCodeExport", 0.6)};
    CHECK_FALSE(collapse(rows, 0.01).has_value());
    CHECK(rows.size() == 2);
}
