#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Compare.hpp"
#include "core/Result.hpp"
#include "core/Summary.hpp"
#include "orcabench_test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

using Stages = std::vector<std::pair<std::string, Clock::duration>>;

// An iteration of `wall` whose stages ran one after another on object 0 from its start.
IterationResult iteration_of(Clock::duration wall, const Stages& stages)
{
    IterationResult iteration;
    iteration.wall = wall;
    Clock::duration at {};
    for (const auto& [stage, length] : stages) {
        iteration.timeline.push_back(span_of(stage, "object:0", at, length));
        at += length;
    }
    return iteration;
}

// The same iteration three times, so every figure has a CV of zero.
std::vector<IterationResult> thrice(const IterationResult& iteration) { return {iteration, iteration, iteration}; }

WorkloadResult ran(std::string name, std::vector<IterationResult> iterations, std::optional<std::uint64_t> hash = 1,
                   std::optional<WorkStats> work = std::nullopt)
{
    return WorkloadResult::ran(std::move(name), hash, std::move(work), std::move(iterations));
}

// A quick run at `threads` on a machine of 20 cores.
Result run_of(std::vector<WorkloadResult> workloads, std::string threads = "20")
{
    Result result;
    result.measurement           = {{MeasurementKey::policy, "quick"}, {MeasurementKey::threads, std::move(threads)}};
    result.machine.logical_cores = 20;
    result.workloads             = std::move(workloads);
    return result;
}

WorkloadComparison only_workload(const Comparison& comparison)
{
    REQUIRE(comparison.workloads.size() == 1);
    return comparison.workloads.front();
}

StagePair pair_of(const WorkloadComparison& workload, const std::string& stage)
{
    const auto found = std::find_if(workload.stages.begin(), workload.stages.end(),
                                    [&stage](const StagePair& pair) { return pair.stage == stage; });
    REQUIRE(found != workload.stages.end());
    return *found;
}

std::vector<std::string> stages_of(const std::vector<StagePair>& pairs)
{
    std::vector<std::string> stages;
    for (const StagePair& pair : pairs)
        stages.push_back(pair.stage);
    return stages;
}

} // namespace

TEST_CASE("runs that measured differently are refused, naming every key that differs", "[OrcaBench][Compare]")
{
    Result a = run_of({});
    Result b = run_of({}, "8");
    a.measurement[MeasurementKey::corpus]              = "embedded,handy";
    b.measurement["a setting compare has no name for"] = "on";
    CHECK_THROWS_AS(compare(a, b, {}), CompareError);
    CHECK_THROWS_WITH(compare(a, b, {}), ContainsSubstring("threads  20 -> 8") && ContainsSubstring(MeasurementKey::corpus) &&
                                             ContainsSubstring("a setting compare has no name for"));
}

TEST_CASE("a mismatch is compared when allowed, with every difference listed", "[OrcaBench][Compare]")
{
    const IterationResult pass = iteration_of(100ms, {{"posSlice", 50ms}});
    CompareOptions        options;
    options.allow_mismatch      = true;
    const Comparison comparison = compare(run_of({ran("slice/idler", {pass})}), run_of({ran("slice/idler", {pass})}, "8"), options);
    REQUIRE(comparison.measurement.size() == 1);
    CHECK(comparison.measurement.front().key == MeasurementKey::threads);
    CHECK(comparison.measurement.front().a == std::optional<std::string>("20"));
    CHECK(comparison.measurement.front().b == std::optional<std::string>("8"));
    CHECK(comparison.workloads.size() == 1);
}

TEST_CASE("the runs may come from different builds and machines", "[OrcaBench][Compare]")
{
    Result b           = run_of({});
    b.build.revision   = "9f1c2d3e4a";
    b.build.compiler   = "MSVC";
    b.machine.host     = "OTHER-PC";
    b.machine.cpu      = "AMD Ryzen 9 7950X";
    CHECK_NOTHROW(compare(run_of({}), b, {}));
}

TEST_CASE("a workload is compared only when both runs ran it", "[OrcaBench][Compare]")
{
    const std::vector<IterationResult> pass = {iteration_of(100ms, {{"posSlice", 50ms}})};
    const Result a = run_of({ran("slice/idler", pass), ran("slice/benchy", pass), WorkloadResult::failed("slice/cube", "threw"),
                             ran("slice/gone", pass), WorkloadResult::skipped("slice/voron", "fixture not available"),
                             WorkloadResult::failed("slice/tall", "threw")});
    const Result b = run_of({ran("slice/idler", pass), WorkloadResult::skipped("slice/benchy", "fixture not available"),
                             ran("slice/cube", pass), ran("slice/new", pass),
                             WorkloadResult::skipped("slice/voron", "fixture not available"),
                             WorkloadResult::skipped("slice/tall", "fixture not available")});
    const Comparison                                 comparison = compare(a, b, {});
    std::vector<std::pair<std::string, std::string>> seen;
    for (const WorkloadComparison& workload : comparison.workloads)
        seen.emplace_back(workload.name, workload.not_compared);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"slice/idler", ""},
        {"slice/benchy", "skipped in b: fixture not available"},
        {"slice/cube", "failed in a: threw"},
        {"slice/gone", "only in a"},
        {"slice/voron", "skipped in both: fixture not available"},
        {"slice/tall", "failed in a: threw; skipped in b: fixture not available"},
        {"slice/new", "only in b"},
    };
    CHECK(seen == expected);
}

TEST_CASE("changed output lists the hash and every work stat that differs", "[OrcaBench][Compare]")
{
    WorkStats work_a;
    work_a.moves   = 1000;
    work_a.layers  = 10;
    work_a.metrics = {{"moves.perimeter", 600}, {"moves.support", 100}};
    WorkStats work_b                = work_a;
    work_b.moves                    = 1010;
    work_b.metrics["moves.support"] = 110;
    const IterationResult    pass  = iteration_of(100ms, {{"posSlice", 50ms}});
    const WorkloadComparison idler = only_workload(compare(run_of({ran("slice/idler", {pass}, 1, work_a)}),
                                                           run_of({ran("slice/idler", {pass}, 2, work_b)}), {}));
    CHECK(idler.output_changed);
    CHECK(idler.hash_a == std::optional<std::uint64_t>(1));
    CHECK(idler.hash_b == std::optional<std::uint64_t>(2));
    std::vector<std::string> changed;
    for (const WorkDifference& difference : idler.work_differences)
        changed.push_back(difference.name);
    CHECK(changed == std::vector<std::string> {"moves", "moves.support"});
}

TEST_CASE("output both runs share is unchanged, and a hash or work stats only one run collected say nothing", "[OrcaBench][Compare]")
{
    const IterationResult pass   = iteration_of(100ms, {{"posSlice", 50ms}});
    const Result          hashed = run_of({ran("slice/idler", {pass}, 7, WorkStats {})});
    CHECK_FALSE(only_workload(compare(hashed, hashed, {})).output_changed);
    const WorkloadComparison uncollected = only_workload(compare(hashed, run_of({ran("slice/idler", {pass}, std::nullopt)}), {}));
    CHECK_FALSE(uncollected.output_changed);
    CHECK(uncollected.work_differences.empty());
}

TEST_CASE("a comparison counts the workloads compared whose output changed", "[OrcaBench][Compare]")
{
    const std::vector<IterationResult> pass = {iteration_of(100ms, {{"posSlice", 50ms}})};
    const Result a = run_of({ran("slice/idler", pass, 1), ran("slice/cube", pass, 1), ran("slice/benchy", pass, 1)});
    const Result b = run_of({ran("slice/idler", pass, 2), ran("slice/cube", pass, 1),
                             WorkloadResult::skipped("slice/benchy", "fixture not available")});
    CHECK(compare(a, b, {}).changed_outputs == 1);
}

TEST_CASE("a comparison counts the workloads that failed in b and not in a", "[OrcaBench][Compare]")
{
    const std::vector<IterationResult> pass = {iteration_of(100ms, {{"posSlice", 50ms}})};
    const Result a = run_of({ran("slice/idler", pass), ran("slice/cube", pass), WorkloadResult::skipped("slice/voron", "missing"),
                             WorkloadResult::failed("slice/tall", "threw"), ran("slice/gone", pass)});
    const Result b = run_of({WorkloadResult::failed("slice/idler", "threw"), ran("slice/cube", pass),
                             WorkloadResult::failed("slice/voron", "threw"), WorkloadResult::failed("slice/tall", "threw"),
                             WorkloadResult::failed("slice/new", "threw")});
    CHECK(compare(a, b, {}).failed_only_in_b == 2);
}

TEST_CASE("a change is the change of the minimum, with the mean's beside it", "[OrcaBench][Compare]")
{
    const auto run = [](std::vector<Clock::duration> times) {
        std::vector<IterationResult> iterations;
        for (const Clock::duration time : times)
            iterations.push_back(iteration_of(200ms, {{"posInfill", time}}));
        return run_of({ran("slice/idler", std::move(iterations))});
    };
    const StagePair infill = pair_of(only_workload(compare(run({100ms, 100ms, 100ms}), run({90ms, 130ms, 130ms}), {})), "posInfill");
    REQUIRE(infill.time.has_value());
    CHECK_THAT(infill.time->change, WithinAbs(-0.1, 1e-12));
    CHECK_THAT(infill.time->mean_change, WithinAbs(350.0 / 300 - 1, 1e-12));
    // Ten percent is inside b's CV of about twenty.
    CHECK(infill.time->verdict == Verdict::Noise);
}

TEST_CASE("a change counts when it clears the 3% bar and both runs' CV", "[OrcaBench][Compare]")
{
    const auto [change, cv_a, cv_b, verdict] = GENERATE(table<double, std::optional<double>, std::optional<double>, Verdict>({
        {0.029, 0.01, 0.01, Verdict::Unchanged},
        {0.03, 0.01, 0.01, Verdict::Worse},
        {-0.03, 0.01, 0.01, Verdict::Better},
        {0.04, 0.01, 0.05, Verdict::Noise},
        {0.04, 0.04, 0.01, Verdict::Noise},
        {0.05, std::nullopt, 0.01, Verdict::Unjudged},
    }));
    CAPTURE(change, cv_a, cv_b);
    CHECK(judge(change, cv_a, cv_b) == verdict);
}

TEST_CASE("a stage that changed state, or that one run lacks, is significant", "[OrcaBench][Compare]")
{
    const IterationResult both    = iteration_of(200ms, {{"posSlice", 50ms}, {"posSupportMaterial", 60ms}});
    IterationResult       changed = iteration_of(200ms, {{"posSlice", 50ms}});
    bool                  timed   = false;
    SECTION("ran in one run and never ran in the other") { changed.not_run = {{"posSupportMaterial", "object:0"}}; }
    SECTION("ran in one run and is missing from the other") {}
    SECTION("ran in both, and in one also started without finishing")
    {
        changed            = both;
        changed.unfinished = {{"posSupportMaterial", "object:1", Clock::time_point {} + 1ms}};
        timed              = true;
    }
    const WorkloadComparison idler =
        only_workload(compare(run_of({ran("slice/idler", thrice(both))}), run_of({ran("slice/idler", thrice(changed))}), {}));
    const StagePair support = pair_of(idler, "posSupportMaterial");
    CHECK(support.significant);
    CHECK(support.time.has_value() == timed);
    CHECK_FALSE(pair_of(idler, "posSlice").significant);
}

TEST_CASE("a stage under the resolvable time counts as instant, and a change under it is not judged", "[OrcaBench][Compare]")
{
    const auto run = [](std::vector<Clock::duration> contouring) {
        std::vector<IterationResult> iterations;
        for (const Clock::duration length : contouring)
            iterations.push_back(iteration_of(200ms, {{"posSlice", 50ms}, {"posContouring", length}}));
        return run_of({ran("slice/idler", std::move(iterations))});
    };
    const auto tick = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(100));
    SECTION("instant in one run and a tick in the other")
    {
        const StagePair contouring = pair_of(only_workload(compare(run({0ns, 0ns, 0ns}), run({tick, 0ns, 0ns}), {})), "posContouring");
        CHECK_FALSE(contouring.significant);
    }
    SECTION("a tick in every pass of one run and a tick in some of the other's")
    {
        const StagePair contouring =
            pair_of(only_workload(compare(run({tick, tick, tick}), run({tick, 0ns, tick}), {})), "posContouring");
        REQUIRE(contouring.time.has_value());
        CHECK(contouring.time->verdict == Verdict::Unchanged);
        CHECK_FALSE(contouring.significant);
    }
}

TEST_CASE("a stage that never finished in some iterations compares the iterations it finished in", "[OrcaBench][Compare]")
{
    const auto finished = [](Clock::duration length) {
        return iteration_of(200ms, {{"posSlice", 50ms}, {"posEstimateCurledExtrusions", length}});
    };
    IterationResult unfinished      = iteration_of(200ms, {{"posSlice", 50ms}});
    unfinished.unfinished           = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 50ms}};
    const WorkloadComparison idler  = only_workload(compare(run_of({ran("slice/idler", {finished(40ms), unfinished, finished(40ms)})}),
                                                            run_of({ran("slice/idler", thrice(finished(50ms)))}), {}));
    const StagePair          curled = pair_of(idler, "posEstimateCurledExtrusions");
    REQUIRE(curled.time.has_value());
    CHECK_THAT(curled.time->change, WithinAbs(0.25, 1e-12));
    CHECK(curled.time->verdict == Verdict::Worse);
}

TEST_CASE("under verbose a comparison pairs each scope of a stage apart, and records that it did", "[OrcaBench][Compare]")
{
    IterationResult pass = iteration_of(200ms, {{"posSlice", 50ms}});
    pass.timeline.push_back(span_of("posSlice", "object:1", 50ms, 30ms));
    const Result   run = run_of({ran("slice/idler", thrice(pass))});
    CompareOptions options;
    options.verbose          = true;
    const Comparison verbose = compare(run, run, options);
    CHECK(verbose.verbose);
    CHECK(only_workload(verbose).stages.size() == 2);
    const Comparison merged = compare(run, run, {});
    CHECK_FALSE(merged.verbose);
    CHECK(only_workload(merged).stages.size() == 1);
}

TEST_CASE("a pair folds only when small in both runs and unchanged, so a stage that grew surfaces", "[OrcaBench][Compare]")
{
    // posX is 0.5% of a's work and 1.25% of b's, posY stays under 1% in both, posZ doubles while small, and posW
    // runs in neither.
    IterationResult a = iteration_of(1100ms, {{"posInfill", 993ms}, {"posX", 5ms}, {"posY", 1ms}, {"posZ", 1ms}});
    IterationResult b = iteration_of(500ms, {{"posInfill", 392ms}, {"posX", 5ms}, {"posY", 1ms}, {"posZ", 2ms}});
    a.not_run = b.not_run = {{"posW", "object:0"}};
    WorkloadComparison idler = only_workload(compare(run_of({ran("slice/idler", thrice(a))}), run_of({ran("slice/idler", thrice(b))}), {}));
    const std::optional<OtherPair> other = collapse(idler.stages, collapse_share);
    REQUIRE(other.has_value());
    CHECK(other->stages == 2);
    CHECK(other->not_run == 1);
    CHECK_THAT(other->a.count(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(other->b.count(), WithinAbs(1.0, 1e-9));
    CHECK(stages_of(idler.stages) == std::vector<std::string> {"posInfill", "posX", "posZ"});
}

TEST_CASE("the wall, summed work, unaccounted time and peak memory compare their minimums", "[OrcaBench][Compare]")
{
    const auto run = [](std::vector<std::tuple<Clock::duration, Clock::duration, std::uint64_t>> passes) {
        std::vector<IterationResult> iterations;
        for (const auto& [wall, work, peak] : passes) {
            iterations.push_back(iteration_of(wall, {{"posInfill", work}}));
            iterations.back().peak_rss_bytes = peak;
        }
        return run_of({ran("slice/idler", std::move(iterations))});
    };
    const WorkloadComparison idler = only_workload(compare(run({{100ms, 60ms, 400}, {110ms, 60ms, 420}, {120ms, 60ms, 410}}),
                                                           run({{90ms, 55ms, 380}, {130ms, 55ms, 390}, {95ms, 55ms, 385}}), {}));
    REQUIRE(idler.wall.has_value());
    REQUIRE(idler.summed_work.has_value());
    REQUIRE(idler.unaccounted.has_value());
    REQUIRE(idler.peak_rss_bytes.has_value());
    CHECK_THAT(idler.wall->change, WithinAbs(90.0 / 100 - 1, 1e-12));
    CHECK_THAT(idler.summed_work->change, WithinAbs(55.0 / 60 - 1, 1e-12));
    CHECK_THAT(idler.unaccounted->change, WithinAbs(35.0 / 40 - 1, 1e-12));
    CHECK_THAT(idler.peak_rss_bytes->change, WithinAbs(380.0 / 400 - 1, 1e-12));
}

TEST_CASE("a run's CPU left out below the floor is marked as such", "[OrcaBench][Compare]")
{
    IterationResult pass           = iteration_of(100ms, {{"posSlice", 50ms}});
    pass.cpu                       = 100ms;
    const Result b                 = run_of({ran("slice/idler", thrice(pass))});
    Result       a                 = b;
    a.machine.properties           = {{MachineProperty::cpu_time_step_ns, "15625000"}};
    const WorkloadComparison idler = only_workload(compare(a, b, {}));
    CHECK(idler.cpu_below_floor_a);
    CHECK_FALSE(idler.cpu_a.has_value());
    CHECK_FALSE(idler.cpu_below_floor_b);
    REQUIRE(idler.cpu_b.has_value());
    CHECK_THAT(*idler.cpu_b, WithinAbs(1.0, 1e-12));
}

TEST_CASE("the wall per million moves, per layer and per cm3 compare the work each run did", "[OrcaBench][Compare]")
{
    WorkStats work_a;
    work_a.moves         = 1000000;
    work_a.layers        = 100;
    work_a.extrusion_mm3 = 20000;
    WorkStats work_b     = work_a;
    work_b.moves         = 1100000;
    work_b.extrusion_mm3 = 22000;
    const auto run       = [](Clock::duration wall, std::uint64_t hash, const WorkStats& work) {
        return run_of({ran("slice/idler", thrice(iteration_of(wall, {{"posInfill", wall / 2}})), hash, work)});
    };
    const WorkloadComparison idler = only_workload(compare(run(2000ms, 1, work_a), run(2100ms, 2, work_b), {}));
    REQUIRE(idler.per_million_moves.has_value());
    CHECK_THAT(idler.per_million_moves->a.min, WithinAbs(2000.0, 1e-9));
    CHECK_THAT(idler.per_million_moves->b.min, WithinAbs(2100.0 / 1.1, 1e-9));
    REQUIRE(idler.per_layer.has_value());
    CHECK_THAT(idler.per_layer->change, WithinAbs(0.05, 1e-12));
    REQUIRE(idler.per_cm3.has_value());
    CHECK_THAT(idler.per_cm3->b.min, WithinAbs(2100.0 / 22, 1e-9));

    work_b.layers = 0;
    CHECK_FALSE(only_workload(compare(run(2000ms, 1, work_a), run(2100ms, 2, work_b), {})).per_layer.has_value());
}

TEST_CASE("the geometric mean covers the workloads compared with unchanged output, a timed stage and a wall in b, and says "
          "how many",
          "[OrcaBench][Compare]")
{
    const auto pass    = [](Clock::duration wall) { return thrice(iteration_of(wall, {{"posInfill", wall / 2}})); };
    const auto untimed = [](Clock::duration wall) { return thrice(iteration_of(wall, {})); };
    const Result a     = run_of({ran("slice/idler", pass(100ms)), ran("slice/cube", pass(100ms)), ran("slice/supports", pass(100ms), 1),
                             ran("slice/benchy", pass(100ms)), ran("slice/instant", pass(100ms)), ran("load/cube", untimed(1ms))});
    const Result b     = run_of({ran("slice/idler", pass(110ms)), ran("slice/cube", pass(90ms)), ran("slice/supports", pass(150ms), 2),
                             ran("slice/instant", pass(0ms)), ran("load/cube", untimed(2ms))});
    const Comparison comparison = compare(a, b, {});
    CHECK(comparison.geometric_mean_of == 2);
    REQUIRE(comparison.wall_geometric_mean.has_value());
    CHECK_THAT(*comparison.wall_geometric_mean, WithinAbs(std::sqrt(1.1 * 0.9) - 1, 1e-12));
}
