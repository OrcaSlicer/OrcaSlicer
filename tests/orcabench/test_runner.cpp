#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/BuildId.hpp"
#include "core/Host.hpp"
#include "core/Measurement.hpp"
#include "core/Policy.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Runner.hpp"
#include "core/Sampler.hpp"
#include "core/Workload.hpp"
#include "orcabench_test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;

namespace {

CatalogEntry entry_of(std::string name, std::string kind = "fake")
{
    CatalogEntry entry;
    entry.name = std::move(name);
    entry.kind = std::move(kind);
    return entry;
}

void add_fake(WorkloadKinds& kinds, const std::string& kind, const FakeHooks& hooks, std::vector<std::string>* calls = nullptr)
{
    kinds.add(kind, [hooks, calls](const CatalogEntry& entry) { return std::make_unique<FakeWorkload>(entry, hooks, calls); });
}

// A registry whose "fake" kind builds a FakeWorkload with these hooks, logging its calls.
WorkloadKinds fake_kinds(const FakeHooks& hooks, std::vector<std::string>& calls)
{
    WorkloadKinds kinds;
    add_fake(kinds, "fake", hooks, &calls);
    return kinds;
}

// One warmup and three timed passes, collecting everything.
Policy quick() { return Policy::resolve("quick", {}, 1); }

// quick without its warmup, so every pass is timed and sampled.
Policy sampled_quick()
{
    PolicyOverrides overrides;
    overrides.warmup = 0;
    return Policy::resolve("quick", overrides, 1);
}

// Reports a span at least one clock tick long, the pass as a metric, and the same output on every
// pass.
void report(unsigned pass, Measurement& measurement)
{
    const Clock::time_point started_at = Clock::now();
    while (Clock::now() == started_at) {}
    measurement.span("posSlice", Scope::print(), started_at, Clock::now());
    measurement.metric("pass", pass);
    WorkStats work;
    work.layers = 190;
    measurement.work(work);
    measurement.output_hash(0x3bbecdbaa64c8fe8);
}

std::vector<std::string> names_of(const Result& result)
{
    std::vector<std::string> names;
    for (const WorkloadResult& workload : result.workloads)
        names.push_back(workload.name);
    return names;
}

} // namespace

TEST_CASE("setup runs once, then prepare and execute run for every warmup and timed pass", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    run_suite({entry_of("fake/cube")}, quick(), fake_kinds({}, calls), environment);
    CHECK(calls == std::vector<std::string> {"enter", "setup", "prepare", "execute", "prepare", "execute", "prepare",
                                             "execute", "prepare", "execute", "leave"});
}

TEST_CASE("each timed pass is recorded with its spans and metrics, and a warmup pass is not", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    FakeHooks                hooks;
    hooks.execute       = report;
    const Result result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);

    REQUIRE(result.workloads.size() == 1);
    const WorkloadResult& workload = result.workloads.front();
    REQUIRE(workload.outcome == Outcome::Ran);
    CHECK(workload.output_hash == std::optional<std::uint64_t>(0x3bbecdbaa64c8fe8));
    REQUIRE(workload.work.has_value());
    CHECK(workload.work->layers == 190);
    REQUIRE(workload.iterations.size() == 3);
    Clock::duration timed {};
    for (std::size_t i = 0; i < workload.iterations.size(); ++i) {
        const IterationResult& iteration = workload.iterations[i];
        CHECK_THAT(iteration.metrics.at("pass"), Catch::Matchers::WithinAbs(i + 2.0, 0.0));
        REQUIRE(iteration.timeline.size() == 1);
        const StageSpan& span = iteration.timeline.front();
        CHECK(span.stage == "posSlice");
        CHECK(iteration.wall > Clock::duration::zero());
        CHECK(iteration.wall >= span.done_at - span.started_at);
        timed += iteration.wall;
    }
    CHECK(result.suite.duration >= timed);
}

TEST_CASE("a workload is told which stages to time", "[OrcaBench][Runner]")
{
    PolicyOverrides overrides;
    overrides.stages = StageSet {Stage::Export};

    StageSet  timed;
    FakeHooks hooks;
    hooks.setup = [&timed](const RunContext& context) {
        timed = context.timed;
        return std::optional<std::string>();
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    run_suite({entry_of("fake/cube")}, Policy::resolve("quick", overrides, 1), fake_kinds(hooks, calls), environment);
    CHECK(timed == StageSet {Stage::Export});
}

TEST_CASE("an entry name that is invalid or repeats ends the run before any workload", "[OrcaBench][Runner]")
{
    const std::vector<std::string> names =
        GENERATE(values<std::vector<std::string>>({{"fake/cube", "fake/cube"}, {"two words"}}));
    CAPTURE(names);
    std::vector<CatalogEntry> entries;
    for (const std::string& name : names)
        entries.push_back(entry_of(name));
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    CHECK_THROWS_AS(run_suite(entries, quick(), fake_kinds({}, calls), environment), WorkloadError);
    CHECK(calls.empty());
}

TEST_CASE("the environment is entered once before the first workload and left after the last", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Policy             policy = quick();
    run_suite({entry_of("fake/cube"), entry_of("fake/cylinder")}, policy, fake_kinds({}, calls), environment);
    REQUIRE(calls.size() > 2);
    CHECK(calls.front() == "enter");
    CHECK(calls.back() == "leave");
    CHECK(std::count(calls.begin(), calls.end(), "enter") == 1);
    CHECK(std::count(calls.begin(), calls.end(), "leave") == 1);
    CHECK(std::count(calls.begin(), calls.end(), "setup") == 2);
    CHECK(environment.threads == policy.threads);
}

TEST_CASE("an environment that fails to enter ends the run before any workload", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls, true);
    CHECK_THROWS_WITH(run_suite({entry_of("fake/cube")}, quick(), fake_kinds({}, calls), environment),
                      "the environment refused to enter");
    CHECK(calls == std::vector<std::string> {"enter"});
}

TEST_CASE("a skipped workload keeps its reason and is never prepared", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    FakeHooks                hooks;
    hooks.setup         = [](const RunContext&) { return std::optional<std::string>("fixture not available"); };
    const Result result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);
    REQUIRE(result.workloads.size() == 1);
    CHECK(result.workloads.front().outcome == Outcome::Skipped);
    CHECK(result.workloads.front().reason == "fixture not available");
    CHECK(std::count(calls.begin(), calls.end(), "prepare") == 0);
}

TEST_CASE("an empty reason to skip fails the workload", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    FakeHooks                hooks;
    hooks.setup         = [](const RunContext&) { return std::optional<std::string>(""); };
    const Result result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);
    REQUIRE(result.workloads.size() == 1);
    CHECK(result.workloads.front().outcome == Outcome::Failed);
    CHECK(result.workloads.front().reason == "setup() gave an empty reason to skip");
}

TEST_CASE("a throw fails only its own workload, naming the call and the pass", "[OrcaBench][Runner]")
{
    // quick's one warmup pass comes first, so the fake's pass 3 is timed pass 2.
    const auto [kind, reason] = GENERATE(table<std::string, std::string>({
        {"missing", "create() threw: fake/cube is of the kind 'missing', which no workload registered"},
        {"throws-in-setup", "setup() threw: no fixture"},
        {"throws-in-prepare", "prepare() threw on warmup pass 1: out of memory"},
        {"throws-in-execute", "execute() threw on timed pass 2: out of memory"},
        {"throws-an-int", "execute() threw on timed pass 2: something that is not a std::exception"},
    }));
    CAPTURE(kind);
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    WorkloadKinds            kinds = fake_kinds({}, calls);

    FakeHooks in_setup;
    in_setup.setup = [](const RunContext&) -> std::optional<std::string> { throw std::runtime_error("no fixture"); };
    add_fake(kinds, "throws-in-setup", in_setup);
    FakeHooks in_prepare;
    in_prepare.prepare = [](unsigned pass) {
        if (pass == 1)
            throw std::runtime_error("out of memory");
    };
    add_fake(kinds, "throws-in-prepare", in_prepare);
    FakeHooks in_execute;
    in_execute.execute = [](unsigned pass, Measurement&) {
        if (pass == 3)
            throw std::runtime_error("out of memory");
    };
    add_fake(kinds, "throws-in-execute", in_execute);
    FakeHooks an_int;
    an_int.execute = [](unsigned pass, Measurement&) {
        if (pass == 3)
            throw 42;
    };
    add_fake(kinds, "throws-an-int", an_int);

    const Result result = run_suite({entry_of("fake/cube", kind), entry_of("fake/cylinder")}, quick(), kinds, environment);
    REQUIRE(result.workloads.size() == 2);
    CHECK(result.workloads[0].outcome == Outcome::Failed);
    CHECK(result.workloads[0].reason == reason);
    CHECK(result.workloads[1].outcome == Outcome::Ran);
}

TEST_CASE("a workload that reports its first pass's span again is failed", "[OrcaBench][Runner]")
{
    Clock::time_point first_start;
    FakeHooks         hooks;
    hooks.execute = [&first_start](unsigned pass, Measurement& measurement) {
        if (pass == 1) {
            first_start = Clock::now();
            // Waits for the clock to tick, so the next pass starts after this span.
            while (Clock::now() == first_start) {}
        }
        measurement.span("posSlice", Scope::print(), first_start, first_start);
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result             result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);
    REQUIRE(result.workloads.size() == 1);
    CHECK(result.workloads.front().outcome == Outcome::Failed);
    CHECK(result.workloads.front().reason == "execute() threw on timed pass 1: the posSlice span starts before its iteration");
}

TEST_CASE("a pass whose output differs from the first pass fails the workload", "[OrcaBench][Runner]")
{
    const auto [differs, reason] = GENERATE(table<std::string, std::string>({
        {"hash", "the output hash of timed pass 3 differs from warmup pass 1"},
        {"work", "the work stats of timed pass 1 differ from warmup pass 1"},
        {"unfinished", "the stages of timed pass 1 differ from warmup pass 1"},
        {"not run", "the stages of timed pass 2 differ from warmup pass 1"},
    }));
    CAPTURE(differs);
    FakeHooks hooks;
    hooks.execute = [differs = differs](unsigned pass, Measurement& measurement) {
        const Clock::time_point at = Clock::now();
        if (differs == "unfinished" && pass == 2)
            measurement.unfinished("posSlice", Scope::print(), at);
        else if (differs == "not run" && pass == 3)
            measurement.not_run("posSlice", Scope::print());
        else
            measurement.span("posSlice", Scope::print(), at, at);
        WorkStats work;
        work.layers = (differs == "work" && pass == 2) ? 191 : 190;
        measurement.work(work);
        measurement.output_hash((differs == "hash" && pass == 4) ? 2 : 1);
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result             result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);
    REQUIRE(result.workloads.size() == 1);
    CHECK(result.workloads.front().outcome == Outcome::Failed);
    CHECK(result.workloads.front().reason == reason);
}

TEST_CASE("a pass may report its stages in another order than the first", "[OrcaBench][Runner]")
{
    FakeHooks hooks;
    hooks.execute = [](unsigned pass, Measurement& measurement) {
        const Clock::time_point at = Clock::now();
        const char*             stages[] {"posSlice", "posInfill"};
        if (pass % 2 == 0)
            std::swap(stages[0], stages[1]);
        measurement.span(stages[0], Scope::object(0), at, at);
        measurement.span(stages[1], Scope::object(0), at, at);
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result             result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);
    REQUIRE(result.workloads.size() == 1);
    CHECK(result.workloads.front().outcome == Outcome::Ran);
}

TEST_CASE("each timed pass records the steps that never finished or never ran", "[OrcaBench][Runner]")
{
    FakeHooks hooks;
    hooks.execute = [](unsigned, Measurement& measurement) {
        measurement.unfinished("posEstimateCurledExtrusions", Scope::object(0), Clock::now());
        measurement.not_run("posContouring", Scope::object(0));
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result             result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment);

    REQUIRE(result.workloads.size() == 1);
    REQUIRE(result.workloads.front().outcome == Outcome::Ran);
    const std::vector<IterationResult>& iterations = result.workloads.front().iterations;
    REQUIRE(iterations.size() == 3);
    for (const IterationResult& iteration : iterations) {
        REQUIRE(iteration.unfinished.size() == 1);
        CHECK(iteration.unfinished.front().stage == "posEstimateCurledExtrusions");
        CHECK(iteration.unfinished.front().scope == "object:0");
        REQUIRE(iteration.not_run.size() == 1);
        CHECK(iteration.not_run.front().stage == "posContouring");
        CHECK(iteration.not_run.front().scope == "object:0");
    }
}

TEST_CASE("what the policy does not collect is absent from the result", "[OrcaBench][Runner]")
{
    // The hash and the work stats change on every pass, which fails a workload only where the
    // policy collects them.
    FakeHooks hooks;
    hooks.execute = [](unsigned pass, Measurement& measurement) {
        const Clock::time_point started_at = Clock::now();
        measurement.span("posSlice", Scope::print(), started_at, Clock::now());
        WorkStats work;
        work.layers = 190 + pass;
        measurement.work(work);
        measurement.output_hash(pass);
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const WorkloadKinds      kinds = fake_kinds(hooks, calls);

    SECTION("verify keeps the hash of its one pass and records no iteration")
    {
        const Result result = run_suite({entry_of("fake/cube")}, Policy::resolve("verify", {}, 1), kinds, environment);
        REQUIRE(result.workloads.size() == 1);
        const WorkloadResult& workload = result.workloads.front();
        REQUIRE(workload.outcome == Outcome::Ran);
        CHECK(workload.output_hash == std::optional<std::uint64_t>(1));
        CHECK_FALSE(workload.work.has_value());
        CHECK(workload.iterations.empty());
    }
    SECTION("pgo keeps nothing")
    {
        const Result result = run_suite({entry_of("fake/cube")}, Policy::resolve("pgo", {}, 1), kinds, environment);
        REQUIRE(result.workloads.size() == 1);
        const WorkloadResult& workload = result.workloads.front();
        REQUIRE(workload.outcome == Outcome::Ran);
        CHECK_FALSE(workload.output_hash.has_value());
        CHECK_FALSE(workload.work.has_value());
        CHECK(workload.iterations.empty());
    }
    SECTION("quick without export drops the hash and the work stats, so their changes do not fail it")
    {
        PolicyOverrides overrides;
        overrides.stages    = StageSet {Stage::Process};
        const Result result = run_suite({entry_of("fake/cube")}, Policy::resolve("quick", overrides, 1), kinds, environment);
        REQUIRE(result.workloads.size() == 1);
        const WorkloadResult& workload = result.workloads.front();
        REQUIRE(workload.outcome == Outcome::Ran);
        CHECK_FALSE(workload.output_hash.has_value());
        CHECK_FALSE(workload.work.has_value());
        CHECK(workload.iterations.size() == 3);
    }
}

TEST_CASE("under pgo an entry that may not train is left out of the run", "[OrcaBench][Runner]")
{
    CatalogEntry micro = entry_of("fake/micro");
    micro.tier         = Tier::Micro;

    CatalogEntry opted_in = entry_of("fake/micro-opted-in");
    opted_in.tier         = Tier::Micro;
    opted_in.pgo_eligible = true;

    const std::vector<CatalogEntry> entries {micro, entry_of("fake/macro"), opted_in};
    std::vector<std::string>        calls;
    FakeEnvironment                 environment(calls);
    const WorkloadKinds             kinds = fake_kinds({}, calls);
    CHECK(names_of(run_suite(entries, Policy::resolve("pgo", {}, 1), kinds, environment)) ==
          std::vector<std::string> {"fake/macro", "fake/micro-opted-in"});
    CHECK(names_of(run_suite(entries, quick(), kinds, environment)) ==
          std::vector<std::string> {"fake/micro", "fake/macro", "fake/micro-opted-in"});
}

TEST_CASE("under pgo a selection with no entry that may train is refused before the run", "[OrcaBench][Runner]")
{
    CatalogEntry micro = entry_of("fake/micro");
    micro.tier         = Tier::Micro;

    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    CHECK_THROWS_AS(run_suite({micro}, Policy::resolve("pgo", {}, 1), fake_kinds({}, calls), environment), WorkloadError);
    CHECK(calls.empty());
}

TEST_CASE("the result records the policy's identity, this build and this machine", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Policy             policy  = quick();
    const auto               before  = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const Clock::time_point  started = Clock::now();
    const Result             result  = run_suite({}, policy, fake_kinds({}, calls), environment);
    const Clock::duration    elapsed = Clock::now() - started;
    const auto               after   = std::chrono::system_clock::now();

    CHECK(result.measurement == policy.identity());
    CHECK(result.build.revision == build_revision);
    CHECK(result.build.dirty == build_dirty);
    CHECK(result.build.compiler == build_compiler);
    CHECK(result.build.compiler_version == build_compiler_version);
    CHECK(result.build.config == build_config);
    CHECK(result.build.flags == build_flags);
    CHECK(result.machine.host == host_name());
    CHECK(result.machine.os == os_description());
    CHECK(result.machine.cpu == cpu_model());
    CHECK(result.machine.logical_cores == logical_cores());
    const std::chrono::nanoseconds step = cpu_time_step();
    CHECK(result.machine.properties == (step > std::chrono::nanoseconds::zero()
                                            ? Properties {{MachineProperty::cpu_time_step_ns, std::to_string(step.count())}}
                                            : Properties {}));
    CHECK(result.suite.started_at >= before);
    CHECK(result.suite.started_at <= after);
    CHECK(result.suite.duration <= elapsed);
    CHECK(result.workloads.empty());
}

TEST_CASE("a timed pass records its sampled peak memory and each span's sampled readings", "[OrcaBench][Runner]")
{
    ScriptedProcess process;
    process.rate      = 3;
    process.rss_bytes = 100;
    FakeHooks hooks;
    hooks.execute = [&process](unsigned, Measurement& measurement) {
        // Three more reads leave at least two taken wholly inside each span.
        process.rss_bytes              = 500;
        const Clock::time_point infill = Clock::now();
        process.await(3);
        measurement.span("posInfill", Scope::print(), infill, Clock::now());
        process.rss_bytes                  = 700;
        const Clock::time_point perimeters = Clock::now();
        process.await(3);
        measurement.span("posPerimeters", Scope::print(), perimeters, Clock::now());
        process.rss_bytes = 200;
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result result = run_suite({entry_of("fake/cube")}, sampled_quick(), fake_kinds(hooks, calls), environment, process.probe());

    REQUIRE(result.workloads.size() == 1);
    REQUIRE(result.workloads.front().outcome == Outcome::Ran);
    const std::vector<IterationResult>& iterations = result.workloads.front().iterations;
    REQUIRE(iterations.size() == 3);
    for (const IterationResult& iteration : iterations) {
        CHECK(iteration.peak_rss_bytes == std::optional<std::uint64_t>(700));
        CHECK(iteration.cpu == 3 * iteration.wall);
        REQUIRE(iteration.timeline.size() == 2);
        const double peaks[] = {500.0, 700.0};
        for (std::size_t i = 0; i < 2; ++i) {
            const Metrics& metrics = iteration.timeline[i].metrics;
            CHECK_THAT(metrics.at(SampledMetric::peak_rss_bytes), Catch::Matchers::WithinAbs(peaks[i], 0.0));
            CHECK_THAT(metrics.at(SampledMetric::cpu_ns), Catch::Matchers::WithinAbs(3 * metrics.at(SampledMetric::cpu_window_ns), 0.0));
        }
    }
}

TEST_CASE("a run that does not collect memory still records each span's sampled CPU", "[OrcaBench][Runner]")
{
    ScriptedProcess process;
    process.rate = 2;
    FakeHooks hooks;
    hooks.execute = [&process](unsigned, Measurement& measurement) {
        const Clock::time_point infill = Clock::now();
        process.await(3);
        measurement.span("posInfill", Scope::print(), infill, Clock::now());
    };
    Policy policy = sampled_quick();
    policy.metrics.erase(Metric::Rss);
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result result = run_suite({entry_of("fake/cube")}, policy, fake_kinds(hooks, calls), environment, process.probe());

    REQUIRE(result.workloads.size() == 1);
    REQUIRE(result.workloads.front().outcome == Outcome::Ran);
    for (const IterationResult& iteration : result.workloads.front().iterations) {
        CHECK_FALSE(iteration.peak_rss_bytes.has_value());
        REQUIRE(iteration.timeline.size() == 1);
        const Metrics& metrics = iteration.timeline.front().metrics;
        CHECK(metrics.count(SampledMetric::peak_rss_bytes) == 0);
        CHECK_THAT(metrics.at(SampledMetric::cpu_ns), Catch::Matchers::WithinAbs(2 * metrics.at(SampledMetric::cpu_window_ns), 0.0));
    }
}

TEST_CASE("a warmup pass and a run that records no timing are not sampled", "[OrcaBench][Runner]")
{
    ScriptedProcess          process;
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);

    SECTION("a warmup pass")
    {
        bool      unsampled = false;
        FakeHooks hooks;
        hooks.execute = [&process, &unsampled](unsigned pass, Measurement&) {
            if (pass != 1)
                return;
            const std::uint64_t before = process.reads;
            std::this_thread::sleep_for(10 * sampling_interval);
            unsampled = process.reads == before;
        };
        run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment, process.probe());
        CHECK(unsampled);
    }
    SECTION("verify, which reads only at its one pass's two ends")
    {
        run_suite({entry_of("fake/cube")}, Policy::resolve("verify", {}, 1), fake_kinds({}, calls), environment, process.probe());
        CHECK(process.reads == 2);
    }
}

TEST_CASE("the run reports each workload, each pass after its last reading, and the whole result last", "[OrcaBench][Runner]")
{
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    WorkloadKinds            kinds = fake_kinds({}, calls);
    FakeHooks                skip;
    skip.setup = [](const RunContext&) { return std::optional<std::string>("fixture not available"); };
    add_fake(kinds, "skips", skip, &calls);

    // Logs the Runner's own readings, since the sampler's thread would race the log.
    const std::thread::id runner = std::this_thread::get_id();
    const ProcessProbe    probe  = [&calls, runner]() {
        if (std::this_thread::get_id() == runner)
            calls.push_back("read");
        return host_reading();
    };
    Result    header, whole;
    RunEvents events;
    events.started = [&calls, &header](const Result& result) {
        calls.push_back("started");
        header = result;
    };
    events.workload_started = [&calls](std::size_t index, std::size_t count, const std::string& name) {
        calls.push_back("workload " + std::to_string(index) + " of " + std::to_string(count) + ": " + name);
    };
    events.pass_done = [&calls](const PassDone& done) {
        calls.push_back("pass " + std::to_string(done.pass) + " of " + std::to_string(done.passes));
    };
    events.workload_done = [&calls](const WorkloadResult& workload) { calls.push_back("done: " + workload.name); };
    events.finished      = [&calls, &whole](const Result& result) {
        calls.push_back("finished");
        whole = result;
    };

    PolicyOverrides overrides;
    overrides.iterations = 1;
    const Policy policy  = Policy::resolve("quick", overrides, 1);
    const Result result  = run_suite({entry_of("fake/cube"), entry_of("fake/benchy", "skips")}, policy, kinds, environment, probe, events);

    CHECK(calls == std::vector<std::string> {"enter", "started", "workload 1 of 2: fake/cube", "setup", "prepare", "read", "execute",
                                             "read", "pass 1 of 2", "prepare", "read", "execute", "read", "pass 2 of 2",
                                             "done: fake/cube", "workload 2 of 2: fake/benchy", "setup", "done: fake/benchy", "leave",
                                             "finished"});
    CHECK(header.measurement == policy.identity());
    CHECK(header.workloads.empty());
    CHECK(whole.workloads.size() == 2);
    CHECK(whole.suite.duration == result.suite.duration);
}

TEST_CASE("a pass's event carries the time its execute() took", "[OrcaBench][Runner]")
{
    std::vector<Clock::duration> walls;
    RunEvents                    events;
    events.pass_done = [&walls](const PassDone& done) {
        if (done.pass > done.warmups)
            walls.push_back(done.wall);
    };
    FakeHooks hooks;
    hooks.execute = report;
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result result = run_suite({entry_of("fake/cube")}, quick(), fake_kinds(hooks, calls), environment, host_reading, events);

    REQUIRE(result.workloads.size() == 1);
    const std::vector<IterationResult>& iterations = result.workloads.front().iterations;
    REQUIRE(walls.size() == iterations.size());
    for (std::size_t i = 0; i < walls.size(); ++i)
        CHECK(walls[i] == iterations[i].wall);
}

TEST_CASE("a run counts only the workloads its policy admits", "[OrcaBench][Runner]")
{
    CatalogEntry micro = entry_of("fake/micro");
    micro.tier         = Tier::Micro;
    std::vector<std::string> started;
    RunEvents                events;
    events.workload_started = [&started](std::size_t index, std::size_t count, const std::string& name) {
        started.push_back(std::to_string(index) + " of " + std::to_string(count) + ": " + name);
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    run_suite({micro, entry_of("fake/macro")}, Policy::resolve("pgo", {}, 1), fake_kinds({}, calls), environment, host_reading, events);
    CHECK(started == std::vector<std::string> {"1 of 1: fake/macro"});
}

TEST_CASE("a listener that throws after a pass fails only that workload", "[OrcaBench][Runner]")
{
    bool      thrown = false;
    RunEvents events;
    events.pass_done = [&thrown](const PassDone&) {
        if (!std::exchange(thrown, true))
            throw std::runtime_error("the console is gone");
    };
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);
    const Result result = run_suite({entry_of("fake/cube"), entry_of("fake/cylinder")}, quick(), fake_kinds({}, calls), environment,
                                    host_reading, events);

    REQUIRE(result.workloads.size() == 2);
    CHECK(result.workloads[0].outcome == Outcome::Failed);
    CHECK(result.workloads[0].reason == "pass_done threw on warmup pass 1: the console is gone");
    CHECK(result.workloads[1].outcome == Outcome::Ran);
}
