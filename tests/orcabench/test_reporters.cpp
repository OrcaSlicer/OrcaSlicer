#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Compare.hpp"
#include "core/Document.hpp"
#include "core/Measurement.hpp"
#include "core/Policy.hpp"
#include "core/Reporters.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Runner.hpp"
#include "core/Sampler.hpp"
#include "core/Workload.hpp"
#include "orcabench_test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

constexpr std::uint64_t mib = 1024 * 1024;

Metrics sampled(double cpu_ms, double window_ms, std::uint64_t peak_mib)
{
    return {{SampledMetric::cpu_ns, cpu_ms * 1e6},
            {SampledMetric::cpu_window_ns, window_ms * 1e6},
            {SampledMetric::peak_rss_bytes, double(peak_mib * mib)}};
}

// One workload of each outcome, the one that ran over two iterations with a stage in each state.
Result canned()
{
    Result result;
    result.suite.duration = 6100ms;
    result.measurement    = {{MeasurementKey::policy, "quick"},
                             {MeasurementKey::warmup, "1"},
                             {MeasurementKey::iterations, "2"},
                             {MeasurementKey::threads, "20"}};
    result.build.revision         = "a83e6b7b74";
    result.build.dirty            = true;
    result.build.compiler         = "Clang";
    result.build.compiler_version = "22.1.3";
    result.build.config           = "Release";
    result.build.flags            = "/O2 /Ob2 /DNDEBUG";
    result.machine.host           = "BENCH-PC";
    result.machine.os             = "Linux 6.8.0";
    result.machine.cpu            = "13th Gen Intel(R) Core(TM) i5-13600K";
    result.machine.logical_cores  = 20;

    IterationResult first;
    first.wall           = 1000ms;
    first.cpu            = 7000ms;
    first.peak_rss_bytes = 400 * mib;
    first.timeline       = {span_of("posSlice", "object:0", 0ms, 12ms),
                            span_of("posInfill", "object:0", 20ms, 90ms, sampled(810, 90, 405)),
                            span_of("posSimplifyInfill", "object:0", 110ms, 5ms),
                            span_of("posContouring", "object:0", 115ms, 0ms),
                            span_of("psGCodeExport", "print", 150ms, 800ms, sampled(4000, 800, 400))};
    first.unfinished     = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 120ms}};
    first.not_run        = {{"posSimplifySupportPath", "object:0"}};

    IterationResult second;
    second.wall           = 1030ms;
    second.cpu            = 7210ms;
    second.peak_rss_bytes = 420 * mib;
    second.timeline       = {span_of("posSlice", "object:0", 0ms, 13ms),
                             span_of("posInfill", "object:0", 20ms, 96ms, sampled(864, 96, 410)),
                             span_of("posSimplifyInfill", "object:0", 116ms, 6ms),
                             span_of("posContouring", "object:0", 122ms, 0ms),
                             span_of("psGCodeExport", "print", 160ms, 810ms, sampled(4050, 810, 420))};
    second.unfinished = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 125ms}};
    second.not_run    = {{"posSimplifySupportPath", "object:0"}};

    WorkStats work;
    work.moves       = 606972;
    work.layers      = 190;
    result.workloads = {WorkloadResult::ran("slice/extruder-idler/standard-0.20", 0x3bbecdbaa64c8fe8, work, {first, second}),
                        WorkloadResult::skipped("slice/benchy/standard-0.20", "fixture not available: corpus 'external' not fetched"),
                        WorkloadResult::failed("slice/overhangs/tree-supports", "threw std::bad_alloc in posSupportMaterial")};
    return result;
}

std::string render(const std::string& name, const Result& result, const ReportOptions& options = {})
{
    std::ostringstream out;
    report(*make_reporter(name, out, options), result);
    return out.str();
}

const char* const canned_header = "orca_bench  quick  threads=20  Clang 22.1.3  Release  a83e6b7b74 dirty\n"
                                  "BENCH-PC  Linux 6.8.0  13th Gen Intel(R) Core(TM) i5-13600K  20 cores\n";

const char* const skipped_block = "\n"
                                  "slice/benchy/standard-0.20                                               SKIPPED\n"
                                  "  fixture not available: corpus 'external' not fetched\n";

} // namespace

TEST_CASE("the console lays out a run's rows, totals and outcomes", "[OrcaBench][Reporters]")
{
    CHECK(render("console", canned()) ==
          std::string(canned_header) +
              "\n"
              "slice/extruder-idler/standard-0.20                       1 warmup + 2 iterations\n"
              "                                   mean     min     CV   share     CPU  peak MiB\n"
              "  psGCodeExport                   805.0   800.0   0.9%   87.9%    5.0x       400\n"
              "  posInfill                        93.0    90.0   4.6%~  10.2%    9.0x       405\n"
              "  posSlice                         12.5    12.0   5.7%~   1.4%       -         -\n"
              "  posEstimateCurledExtrusions   never finished\n"
              "  other (3 stages, 1 not run)       5.5       -      -    0.6%\n"
              "  ------------------------------------------------------------------------------\n"
              "  summed work                     916.0\n"
              "  wall envelope                  1015.0\n"
              "  unaccounted                      99.0                   9.8%\n"
              "\n"
              "  peak RSS 400 MiB   CPU 7.0x   moves 606,972   layers 190   hash 3bbecdba\n" +
              skipped_block +
              "\n"
              "slice/overhangs/tree-supports                                             FAILED\n"
              "  threw std::bad_alloc in posSupportMaterial\n"
              "\n"
              "~ CV above the 3% significance bar, so a change that size there is noise\n"
              "\n"
              "1 ran, 1 skipped, 1 failed   6.1s\n");
}

TEST_CASE("under --verbose each scope has its own row and nothing folds", "[OrcaBench][Reporters]")
{
    ReportOptions options;
    options.verbose        = true;
    const std::string text = render("console", canned(), options);
    const std::string rows = "  psGCodeExport print             805.0   800.0   0.9%   87.9%    5.0x       400\n"
                             "  posInfill object:0               93.0    90.0   4.6%~  10.2%    9.0x       405\n"
                             "  posSlice object:0                12.5    12.0   5.7%~   1.4%       -         -\n"
                             "  posSimplifyInfill object:0        5.5     5.0  12.9%~   0.6%       -         -\n"
                             "  posContouring object:0        instant\n"
                             "  posEstimateCurledExtrusions object:0  never finished\n"
                             "  posSimplifySupportPath object:0  not run\n"
                             "  ---";
    CHECK(text.find(rows) != std::string::npos);
    CHECK(text.find("other (") == std::string::npos);
}

TEST_CASE("rows sort by mean, or by first start for pipeline order", "[OrcaBench][Reporters]")
{
    ReportOptions options;
    options.sort_by        = SortBy::Start;
    const std::string text = render("console", canned(), options);
    const auto        at   = [&text](const std::string& stage) { return text.find("  " + stage + " "); };
    CHECK(at("posSlice") < at("posInfill"));
    CHECK(at("posInfill") < at("posEstimateCurledExtrusions"));
    CHECK(at("posEstimateCurledExtrusions") < at("psGCodeExport"));
    CHECK(at("psGCodeExport") < text.find("  other ("));
}

TEST_CASE("the console marks shared readings and floored CPU, with a legend for each", "[OrcaBench][Reporters]")
{
    Result result = canned();
    result.machine.properties[MachineProperty::cpu_time_step_ns] = "15625000";
    for (IterationResult& iteration : result.workloads.front().iterations) {
        StageSpan& infill = iteration.timeline[1];
        infill.done_at -= 10ms;
        infill.started_at -= 10ms;
    }
    const std::string text = render("console", result);
    CHECK(text.find("  posInfill*                       93.0    90.0   4.6%~  10.2%       -       405\n") != std::string::npos);
    CHECK(text.find("  posSlice*                        12.5    12.0   5.7%~   1.4%       -         -\n") != std::string::npos);
    CHECK(text.find("  psGCodeExport                   805.0   800.0   0.9%   87.9%    5.0x       400\n") != std::string::npos);
    CHECK(text.find("\n* readings shared with stages that ran at the same time\n") != std::string::npos);
    CHECK(text.find("\nCPU left out where 15.6 ms steps in CPU time could skew it past 10%\n") != std::string::npos);
}

TEST_CASE("the console marks a stage that ran but also never finished, with a legend", "[OrcaBench][Reporters]")
{
    Result result = canned();
    result.workloads.front().iterations.front().unfinished.push_back({"posInfill", "object:1", Clock::time_point {} + 20ms});
    const std::string text = render("console", result);
    CHECK(text.find("  posInfill!                       93.0    90.0   4.6%~  10.2%    9.0x       405\n") != std::string::npos);
    CHECK(text.find("  posEstimateCurledExtrusions   never finished\n") != std::string::npos);
    CHECK(text.find("\n! started at least once without finishing, so some of its time is in unaccounted\n") != std::string::npos);
}

TEST_CASE("a console reused for another run explains only that run's marks", "[OrcaBench][Reporters]")
{
    std::ostringstream              out;
    const std::unique_ptr<Reporter> console = make_reporter("console", out, {});
    report(*console, canned());
    Result quiet    = canned();
    quiet.workloads = {quiet.workloads[1]};
    out.str("");
    report(*console, quiet);
    CHECK(out.str() == render("console", quiet));
}

TEST_CASE("a time too short to print as 0.1 prints as <0.1", "[OrcaBench][Reporters]")
{
    IterationResult iteration;
    iteration.wall     = 200ms;
    iteration.timeline = {span_of("posSlice", "object:0", 0ms, 30us), span_of("psGCodeExport", "print", 1ms, 100ms)};
    Result result      = canned();
    result.workloads   = {WorkloadResult::ran("slice/idler", std::nullopt, std::nullopt, {iteration})};
    ReportOptions options;
    options.verbose = true;
    CHECK(render("console", result, options).find("  posSlice object:0                <0.1    <0.1      -    0.0%       -         -\n") !=
          std::string::npos);
}

TEST_CASE("a figure too wide for its column keeps a space before it", "[OrcaBench][Reporters]")
{
    std::vector<IterationResult> iterations;
    for (const Clock::duration length : {Clock::duration(120000ms), Clock::duration(126913400us)}) {
        IterationResult iteration;
        iteration.wall     = length + 1s;
        iteration.timeline = {span_of("posSlice", "object:0", 0ms, length)};
        iterations.push_back(iteration);
    }
    Result result    = canned();
    result.workloads = {WorkloadResult::ran("slice/idler", std::nullopt, std::nullopt, iterations)};
    const std::string text = render("console", result);
    CHECK(text.find(" 123456.7 120000.0 ") != std::string::npos);
    CHECK(text.find("123456.7120000.0") == std::string::npos);
}

TEST_CASE("a pass count too wide for the console follows the workload's name after two spaces", "[OrcaBench][Reporters]")
{
    Result            result = canned();
    const std::string warmup(80, '9');
    result.measurement[MeasurementKey::warmup] = warmup;
    CHECK(render("console", result).find("\nslice/extruder-idler/standard-0.20  " + warmup + " warmup + 2 iterations\n") !=
          std::string::npos);
}

TEST_CASE("the header marks a build whose flags turn optimization off", "[OrcaBench][Reporters]")
{
    const auto [flags, marked] = GENERATE(table<std::string, bool>({
        {"/Zi /Od /Ob0 /DNDEBUG", true},
        {"-g -O0", true},
        {"/O2 /Ob2 /DNDEBUG", false},
        {"-O3 -march=native", false},
        {"/Od /O2", false},
        {"-O2 -O0", true},
        {"-g", true},
        {"-g -fsigned-char", true},
        {"", false},
    }));
    CAPTURE(flags);
    Result result = canned();
    result.build.config = "RelWithDebInfo";
    result.build.flags  = flags;
    CHECK((render("console", result).find("  RelWithDebInfo (unoptimized)  ") != std::string::npos) == marked);
}

TEST_CASE("a workload with no timed passes shows only what it recorded", "[OrcaBench][Reporters]")
{
    Result result    = canned();
    result.workloads = {WorkloadResult::ran("slice/extruder-idler/standard-0.20", 0x3bbecdbaa64c8fe8, std::nullopt, {}),
                        WorkloadResult::ran("slice/benchy/standard-0.20", std::nullopt, std::nullopt, {})};
    CHECK(render("console", result) == std::string(canned_header) +
                                           "\n"
                                           "slice/extruder-idler/standard-0.20\n"
                                           "  hash 3bbecdba\n"
                                           "\n"
                                           "slice/benchy/standard-0.20\n"
                                           "\n"
                                           "2 ran, 0 skipped, 0 failed   6.1s\n");
}

TEST_CASE("a run that collects the hash says when a workload produced none", "[OrcaBench][Reporters]")
{
    Result result                               = canned();
    result.measurement[MeasurementKey::metrics] = "wall,rss,hash";
    result.workloads = {WorkloadResult::ran("slice/extruder-idler/standard-0.20", 0x3bbecdbaa64c8fe8, std::nullopt, {}),
                        WorkloadResult::ran("slice/benchy/standard-0.20", std::nullopt, std::nullopt, {})};
    CHECK(render("console", result) == std::string(canned_header) +
                                           "\n"
                                           "slice/extruder-idler/standard-0.20\n"
                                           "  hash 3bbecdba\n"
                                           "\n"
                                           "slice/benchy/standard-0.20\n"
                                           "  hash none\n"
                                           "\n"
                                           "2 ran, 0 skipped, 0 failed   6.1s\n");
}

TEST_CASE("a workload whose passes took no time shows no unaccounted share", "[OrcaBench][Reporters]")
{
    Result result    = canned();
    result.workloads = {WorkloadResult::ran("slice/idler", std::nullopt, std::nullopt, {IterationResult {}})};
    const std::string text = render("console", result);
    const std::size_t at   = text.find("  unaccounted");
    REQUIRE(at != std::string::npos);
    CHECK(text.substr(at, text.find('\n', at) - at) == "  unaccounted" + std::string(23, ' ') + "0.0");
}

TEST_CASE("json writes the whole document once the run is done, whatever the console would fold", "[OrcaBench][Reporters]")
{
    ReportOptions options;
    options.collapse_below = 0.5;
    std::ostringstream              out;
    const std::unique_ptr<Reporter> json   = make_reporter("json", out, options);
    const Result                    result = canned();
    Result                          header = result;
    header.workloads.clear();
    json->started(header);
    for (const WorkloadResult& workload : result.workloads)
        json->workload(workload);
    CHECK(out.str().empty());
    json->finished(result);
    CHECK(out.str() == write_document(result));
}

TEST_CASE("null writes nothing", "[OrcaBench][Reporters]")
{
    CHECK(render("null", canned()).empty());
}

TEST_CASE("the reporters are console, json and null, and any other name is refused", "[OrcaBench][Reporters]")
{
    CHECK(reporter_names() == std::vector<std::string> {"console", "json", "null"});
    std::ostringstream out;
    CHECK_THROWS_AS(make_reporter("xml", out, {}), std::invalid_argument);
}

TEST_CASE("in a terminal the progress line is redrawn after each pass and erased on request", "[OrcaBench][Reporters]")
{
    std::ostringstream out;
    Clock::time_point  now = Clock::time_point {} + 1h;
    Progress           progress(out, true, [&now]() { return now; });
    progress.workload_started(2, 20, "slice/benchy/standard-0.20");
    // Each pass takes 1.5 s, so the time left is the passes left at that pace.
    const std::uint64_t   passes  = 4;
    const Clock::duration walls[] = {1210ms, 1190ms, 1200ms, 1200ms};
    for (std::uint64_t pass = 1; pass <= passes; ++pass) {
        now += 1500ms;
        progress.pass_done({pass, 1, passes, walls[pass - 1]});
    }
    progress.clear();

    const std::string name   = "[2/20] slice/benchy/standard-0.20";
    const std::string warmup = name + "  warmup 1/1  last 1.21 s  about 5 s left";
    const std::string first  = name + "  timed 1/3  last 1.19 s  about 3 s left";
    const std::string second = name + "  timed 2/3  last 1.20 s  about 2 s left";
    const std::string third  = name + "  timed 3/3  last 1.20 s";
    CHECK(out.str() == "\r" + name + "\r" + warmup + "\r" + first + " \r" + second + "\r" + third +
                           std::string(second.size() - third.size(), ' ') + "\r" + std::string(third.size(), ' ') + "\r");
}

TEST_CASE("in a terminal a progress line too wide for the view shortens the workload's name", "[OrcaBench][Reporters]")
{
    std::ostringstream out;
    Progress           progress(out, true);
    const std::string  name = "slice/" + std::string(70, 'x');
    progress.workload_started(1, 1, name);
    progress.pass_done({1, 0, 1, 1s});
    std::istringstream lines(out.str());
    std::size_t        drawn = 0;
    for (std::string line; std::getline(lines, line, '\r');) {
        CAPTURE(line);
        CHECK(line.size() < 80);
        drawn += line.empty() ? 0 : 1;
    }
    CHECK(drawn == 2);
    CHECK(out.str().find("...  timed 1/1  last 1.00 s") != std::string::npos);
}

TEST_CASE("in a log the progress line is printed once per workload", "[OrcaBench][Reporters]")
{
    std::ostringstream out;
    Progress           progress(out, false);
    progress.workload_started(1, 2, "slice/idler");
    progress.pass_done({1, 1, 2, 1s});
    progress.clear();
    progress.workload_started(2, 2, "slice/benchy");
    CHECK(out.str() == "[1/2] slice/idler\n[2/2] slice/benchy\n");
}

TEST_CASE("the report's events erase the progress line before a workload prints", "[OrcaBench][Reporters]")
{
    std::ostringstream              out;
    Progress                        progress(out, true);
    const std::unique_ptr<Reporter> console = make_reporter("console", out, {});
    const RunEvents                 events  = report_events(*console, &progress);
    const Result                    result  = canned();
    Result                          header  = result;
    header.workloads.clear();

    events.started(header);
    events.workload_started(2, 3, result.workloads[1].name);
    events.workload_done(result.workloads[1]);

    const std::string line = "[2/3] slice/benchy/standard-0.20";
    CHECK(out.str() == canned_header + ("\r" + line + "\r" + std::string(line.size(), ' ') + "\r") + skipped_block);
}

TEST_CASE("without a progress line only the reporter hears the run", "[OrcaBench][Reporters]")
{
    std::ostringstream              out;
    const std::unique_ptr<Reporter> console = make_reporter("console", out, {});
    const RunEvents                 events  = report_events(*console, nullptr);
    CHECK(static_cast<bool>(events.started));
    CHECK(static_cast<bool>(events.workload_done));
    CHECK(static_cast<bool>(events.finished));
    CHECK_FALSE(static_cast<bool>(events.workload_started));
    CHECK_FALSE(static_cast<bool>(events.pass_done));
}

TEST_CASE("a run reported as it goes prints what its finished result prints", "[OrcaBench][Reporters]")
{
    FakeHooks hooks;
    hooks.execute = [](unsigned, Measurement& measurement) {
        const Clock::time_point at = Clock::now();
        measurement.span("posSlice", Scope::object(0), at, at);
    };
    WorkloadKinds kinds;
    kinds.add("fake", [hooks](const CatalogEntry& entry) { return std::make_unique<FakeWorkload>(entry, hooks); });
    std::vector<CatalogEntry> entries(2);
    entries[0].name = "fake/cube";
    entries[0].kind = "fake";
    entries[1].name = "fake/cylinder";
    entries[1].kind = "fake";
    std::vector<std::string> calls;
    FakeEnvironment          environment(calls);

    std::ostringstream              live;
    const std::unique_ptr<Reporter> console = make_reporter("console", live, {});
    const RunEvents                 events  = report_events(*console, nullptr);
    const Result                    result  = run_suite(entries, Policy::resolve("quick", {}, 1), kinds, environment, host_reading, events);
    CHECK(live.str() == render("console", result));
}

namespace {

// A duration of `value` milliseconds, to the nanosecond.
std::chrono::nanoseconds ms_of(double value) { return std::chrono::nanoseconds(std::llround(value * 1e6)); }

// The CPU rate and peak MiB a stage's readings show in every pass of a run, where it has readings.
struct Readings
{
    std::optional<double>        cpu_rate;
    std::optional<std::uint64_t> peak_mib;
};

using StageReadings = std::map<std::string, Readings>;

// A pass whose stages ran one after another from its start, psGCodeExport on the print and the rest
// on object 0.
IterationResult pass_of(double wall_ms, double cpu_rate, std::optional<std::uint64_t> peak_mib,
                        const std::vector<std::pair<std::string, double>>& stages, const StageReadings& readings = {})
{
    IterationResult iteration;
    iteration.wall = ms_of(wall_ms);
    iteration.cpu  = std::chrono::nanoseconds(std::llround(cpu_rate * double(ms_of(wall_ms).count())));
    if (peak_mib)
        iteration.peak_rss_bytes = *peak_mib * mib;
    Clock::duration at {};
    for (const auto& [stage, length_ms] : stages) {
        const std::chrono::nanoseconds length = ms_of(length_ms);
        const auto                     found  = readings.find(stage);
        Metrics                        metrics;
        if (found != readings.end() && found->second.cpu_rate) {
            metrics[SampledMetric::cpu_ns]        = *found->second.cpu_rate * double(length.count());
            metrics[SampledMetric::cpu_window_ns] = double(length.count());
        }
        if (found != readings.end() && found->second.peak_mib)
            metrics[SampledMetric::peak_rss_bytes] = double(*found->second.peak_mib * mib);
        iteration.timeline.push_back(span_of(stage, stage == "psGCodeExport" ? "print" : "object:0", at, length, metrics));
        at += length;
    }
    return iteration;
}

const std::vector<std::string> idler_stages = {"posSlice",          "posPerimeters", "posPrepareInfill", "posInfill",
                                               "posSimplifyInfill", "posContouring", "psGCodeExport"};

// An idler pass of each stage's time in pipeline order, where curled extrusions never finish and
// support paths never start.
IterationResult idler_pass(double wall_ms, const std::vector<double>& stage_ms, std::uint64_t peak_mib, double cpu_rate,
                           const StageReadings& readings)
{
    std::vector<std::pair<std::string, double>> stages;
    for (std::size_t i = 0; i < idler_stages.size(); ++i)
        stages.emplace_back(idler_stages[i], stage_ms[i]);
    IterationResult iteration = pass_of(wall_ms, cpu_rate, peak_mib, stages, readings);
    iteration.unfinished      = {{"posEstimateCurledExtrusions", "object:0", Clock::time_point {} + 250ms}};
    iteration.not_run         = {{"posSimplifySupportPath", "object:0"}};
    return iteration;
}

IterationResult cube_pass(double wall_ms, double infill_ms) { return pass_of(wall_ms, 1.3, std::nullopt, {{"posInfill", infill_ms}}); }

IterationResult supports_pass(double wall_ms, double support_ms, double export_ms, std::uint64_t peak_mib, double cpu_rate)
{
    return pass_of(wall_ms, cpu_rate, peak_mib, {{"posSupportMaterial", support_ms}, {"psGCodeExport", export_ms}});
}

WorkStats work_of(std::uint64_t moves, std::uint64_t layers, double extrusion_mm3, Metrics metrics = {})
{
    WorkStats work;
    work.moves         = moves;
    work.layers        = layers;
    work.extrusion_mm3 = extrusion_mm3;
    work.metrics       = std::move(metrics);
    return work;
}

// A quick run of 1 warmup and 3 iterations at 20 threads on the bench machine.
Result bench_run(std::int64_t started_ms, std::string revision, bool dirty, std::string flags, std::vector<WorkloadResult> workloads)
{
    Result result;
    result.suite.started_at       = decltype(Suite::started_at)(std::chrono::milliseconds(started_ms));
    result.measurement            = {{MeasurementKey::policy, "quick"},
                                     {MeasurementKey::warmup, "1"},
                                     {MeasurementKey::iterations, "3"},
                                     {MeasurementKey::threads, "20"},
                                     {MeasurementKey::stages, "process,export"}};
    result.build.revision         = std::move(revision);
    result.build.dirty            = dirty;
    result.build.compiler         = "Clang";
    result.build.compiler_version = "22.1.3";
    result.build.config           = "Release";
    result.build.flags            = std::move(flags);
    result.machine.host           = "BENCH-PC";
    result.machine.os             = "Linux 6.8.0";
    result.machine.cpu            = "13th Gen Intel(R) Core(TM) i5-13600K";
    result.machine.logical_cores  = 20;
    result.workloads              = std::move(workloads);
    return result;
}

// The runs on either side of a change that speeds up voron-cube's infill and alters the tree
// supports' output, where the run after skips benchy.
Result before()
{
    const StageReadings readings = {{"posPrepareInfill", {12.1, 401}}, {"posInfill", {14.8, 398}}, {"psGCodeExport", {std::nullopt, 412}}};
    return bench_run(1790759640000, "a83e6b7b74", false, "/O2 /Ob2 /DNDEBUG",
                     {WorkloadResult::ran("slice/extruder-idler/standard-0.20", 0x3bbecdbaa64c8fe8, work_of(606972, 190, 12345.6),
                                          {idler_pass(1143.1, {12.4, 68.2, 83.0, 90.0, 3.0, 0.4, 804.6}, 412, 7.4, readings),
                                           idler_pass(1152.8, {13.1, 69.9, 84.1, 92.4, 3.1, 0.4, 810.2}, 420, 7.4, readings),
                                           idler_pass(1160.4, {13.3, 71.3, 85.9, 91.7, 3.0, 0.4, 812.0}, 415, 7.4, readings)}),
                      WorkloadResult::ran("slice/voron-cube/standard-0.20", 0x0a1b2c3d4e5f6071, std::nullopt,
                                          {cube_pass(412.7, 300.0), cube_pass(415.0, 301.2), cube_pass(418.1, 302.5)}),
                      WorkloadResult::ran("slice/overhangs/tree-supports", 0x5e2a90c100000001,
                                          work_of(1204331, 250, 30871.2, {{"moves.support", 88410}}),
                                          {supports_pass(2210.4, 512.0, 1210.9, 690, 3.1), supports_pass(2231.6, 518.4, 1222.3, 694, 3.1),
                                           supports_pass(2252.8, 524.8, 1233.7, 692, 3.1)}),
                      WorkloadResult::ran("slice/benchy/standard-0.20", 7, std::nullopt,
                                          {pass_of(100.0, 1.0, std::nullopt, {{"posSlice", 50.0}})})});
}

Result after()
{
    const StageReadings readings = {{"posPrepareInfill", {4.3, 402}}, {"posInfill", {15.0, 391}}, {"psGCodeExport", {std::nullopt, 415}}};
    return bench_run(1790766120000, "9f1c2d3e4a", true, "/O2 /Ob2 /DNDEBUG /GL",
                     {WorkloadResult::ran("slice/extruder-idler/standard-0.20", 0x3bbecdbaa64c8fe8, work_of(606972, 190, 12345.6),
                                          {idler_pass(1150.2, {12.9, 68.9, 89.6, 83.1, 3.0, 0.4, 812.3}, 415, 7.3, readings),
                                           idler_pass(1159.9, {13.8, 70.3, 90.8, 85.9, 3.1, 0.4, 815.0}, 419, 7.3, readings),
                                           idler_pass(1164.5, {14.2, 70.9, 92.0, 86.4, 3.0, 0.4, 818.1}, 418, 7.3, readings)}),
                      WorkloadResult::ran("slice/voron-cube/standard-0.20", 0x0a1b2c3d4e5f6071, std::nullopt,
                                          {cube_pass(395.1, 285.1), cube_pass(398.2, 286.0), cube_pass(399.0, 287.9)}),
                      WorkloadResult::ran("slice/overhangs/tree-supports", 0x91c02e7f00000002,
                                          work_of(1211904, 250, 31004.8, {{"moves.support", 95983}}),
                                          {supports_pass(2251.9, 540.3, 1219.0, 702, 3.0), supports_pass(2270.2, 545.1, 1230.0, 705, 3.0),
                                           supports_pass(2288.5, 549.9, 1241.0, 704, 3.0)}),
                      WorkloadResult::skipped("slice/benchy/standard-0.20", "fixture not available: corpus 'external' not fetched")});
}

// The runs with coarse CPU time steps, where posPrepareInfill overlaps posInfill in b and posPerimeters
// also started without finishing in a.
std::pair<Result, Result> marked_runs()
{
    Result a = before();
    Result b = after();
    for (Result* run : {&a, &b})
        run->machine.properties[MachineProperty::cpu_time_step_ns] = "15625000";
    for (IterationResult& iteration : b.workloads[0].iterations)
        for (StageSpan& span : iteration.timeline)
            if (span.stage == "posPrepareInfill") {
                span.started_at += 10ms;
                span.done_at += 10ms;
            }
    a.workloads[0].iterations[0].unfinished.push_back({"posPerimeters", "object:1", Clock::time_point {} + 20ms});
    return {a, b};
}

// The run after, where posSimplifyInfill no longer runs, posContouring takes no time and posIroning
// is new.
Result restaged_after()
{
    Result b = after();
    for (IterationResult& iteration : b.workloads[0].iterations) {
        Timeline&  timeline = iteration.timeline;
        const auto simplify = [](const StageSpan& span) { return span.stage == "posSimplifyInfill"; };
        timeline.erase(std::remove_if(timeline.begin(), timeline.end(), simplify), timeline.end());
        for (StageSpan& span : timeline)
            if (span.stage == "posContouring")
                span.done_at = span.started_at;
        timeline.push_back(span_of("posIroning", "object:0", 1100ms, 20ms));
    }
    return b;
}

// The run as if it had timed only its first iteration.
Result first_iteration_of(Result run)
{
    run.measurement[MeasurementKey::iterations] = "1";
    for (WorkloadResult& workload : run.workloads)
        if (workload.iterations.size() > 1)
            workload.iterations.resize(1);
    return run;
}

// The view the command line builds for --compare before.json after.json.
CompareView file_view()
{
    CompareView view;
    view.label_a = "before.json";
    view.label_b = "after.json";
    return view;
}

std::string compare_text(const Result& a, const Result& b, const CompareView& view = file_view(), bool allow_mismatch = false,
                         bool verbose = false)
{
    std::ostringstream out;
    write_comparison(out, compare(a, b, {allow_mismatch, verbose}), view);
    return out.str();
}

bool has_line(const std::string& text, const std::string& line) { return text.find("\n" + line + "\n") != std::string::npos; }

const std::string worse   = "\x1b[1;38;5;166m";
const std::string better  = "\x1b[1;38;5;32m";
const std::string inverse = "\x1b[7m";
const std::string strong  = "\x1b[1m";
const std::string faint   = "\x1b[2m";
const std::string reset   = "\x1b[0m";

std::string without_color(const std::string& text) { return std::regex_replace(text, std::regex("\x1b\\[[0-9;]*m"), ""); }

} // namespace

TEST_CASE("a comparison lays out changed output, the walls, each workload's table and what was not compared", "[OrcaBench][Reporters]")
{
    CHECK(compare_text(before(), after()) ==
          "orca_bench compare  quick  threads=20  1 warmup + 3 iterations  stages process,export\n"
          "a  2026-09-30 09:14 UTC  a83e6b7b74        Clang 22.1.3  Release  before.json\n"
          "b  2026-09-30 11:02 UTC  9f1c2d3e4a dirty  Clang 22.1.3  Release  after.json\n"
          "   a flags  /O2 /Ob2 /DNDEBUG\n"
          "   b flags  /O2 /Ob2 /DNDEBUG /GL\n"
          "   BENCH-PC  Linux 6.8.0  13th Gen Intel(R) Core(TM) i5-13600K  20 cores\n"
          "\n"
          "OUTPUT CHANGED in 1 workload, so its times below measure different work\n"
          "  slice/overhangs/tree-supports             hash 5e2a90c1 -> 91c02e7f\n"
          "                                            moves 1,204,331 -> 1,211,904  +0.6%\n"
          "                                            extrusion 30.9 -> 31.0 cm3  +0.4%\n"
          "                                            moves.support 88,410 -> 95,983  +8.6%\n"
          "\n"
          "                                             wall a  wall b  change                    diff  CV a  CV b   MiB a   MiB b\n"
          "  slice/extruder-idler/standard-0.20         1143.1  1150.2   +0.6%                    +7.1  0.8%  0.6%   412.0   415.0\n"
          "  slice/voron-cube/standard-0.20              412.7   395.1   -4.3%  faster           -17.6  0.7%  0.5%       -       -\n"
          "  slice/overhangs/tree-supports              2210.4  2251.9   +1.9%  output changed   +41.5  0.9%  0.8%   690.0   702.0\n"
          "  geometric mean over 2 workloads with unchanged output       -1.9%\n"
          "\n"
          "slice/extruder-idler/standard-0.20                                                                      output unchanged\n"
          "                                  min a   min b  change             diff by mean  CV a  CV b CPU a CPU b   MiB a   MiB b\n"
          "  psGCodeExport                   804.6   812.3   +1.0%             +7.7   +0.8%  0.5%  0.4%     -     -   412.0   415.0\n"
          "  posInfill                        90.0    83.1   -7.7%  faster     -6.9   -6.8%  1.4%  2.1% 14.8x 15.0x   398.0   391.0\n"
          "  posPrepareInfill                 83.0    89.6   +8.0%  slower     +6.6   +7.7%  1.7%  1.3% 12.1x  4.3x   401.0   402.0\n"
          "  posPerimeters                    68.2    68.9   +1.0%             +0.7   +0.3%  2.2%  1.5%     -     -       -       -\n"
          "  posSlice                         12.4    12.9   +4.0%~            +0.5   +5.4%  3.7%  4.9%     -     -       -       -\n"
          "  posEstimateCurledExtrusions  never finished in both\n"
          "  other (3 stages, 1 not run)       3.4     3.4   +0.0%             +0.0\n"
          "  ----------------------------------------------------------------------------------------------------------------------\n"
          "  summed work                    1061.6  1070.2   +0.8%             +8.6   +0.7%  0.8%  0.7%\n"
          "  unaccounted                      79.6    79.5   -0.1%             -0.1   -1.6%  2.0%  0.7%\n"
          "  wall                           1143.1  1150.2   +0.6%             +7.1   +0.5%  0.8%  0.6%  7.4x  7.3x\n"
          "  peak MiB                        412.0   415.0   +0.7%             +3.0   +0.4%  1.0%  0.5%\n"
          "\n"
          "slice/voron-cube/standard-0.20                                                                          output unchanged\n"
          "                                  min a   min b  change             diff by mean  CV a  CV b CPU a CPU b   MiB a   MiB b\n"
          "  posInfill                       300.0   285.1   -5.0%  faster    -14.9   -4.9%  0.4%  0.5%     -     -       -       -\n"
          "  ----------------------------------------------------------------------------------------------------------------------\n"
          "  summed work                     300.0   285.1   -5.0%  faster    -14.9   -4.9%  0.4%  0.5%\n"
          "  unaccounted                     112.7   110.0   -2.4%             -2.7   -2.6%  1.3%  1.0%\n"
          "  wall                            412.7   395.1   -4.3%  faster    -17.6   -4.3%  0.7%  0.5%  1.3x  1.3x\n"
          "\n"
          "slice/overhangs/tree-supports                                                                             OUTPUT CHANGED\n"
          "                                  min a   min b  change             diff by mean  CV a  CV b CPU a CPU b   MiB a   MiB b\n"
          "  psGCodeExport                  1210.9  1219.0   +0.7%             +8.1   +0.6%  0.9%  0.9%     -     -       -       -\n"
          "  posSupportMaterial              512.0   540.3   +5.5%  longer    +28.3   +5.2%  1.2%  0.9%     -     -       -       -\n"
          "  ----------------------------------------------------------------------------------------------------------------------\n"
          "  summed work                    1722.9  1759.3   +2.1%            +36.4   +2.0%  1.0%  0.9%\n"
          "  unaccounted                     487.5   492.6   +1.0%             +5.1   +0.9%  0.7%  0.5%\n"
          "  wall                           2210.4  2251.9   +1.9%            +41.5   +1.7%  0.9%  0.8%  3.1x  3.0x\n"
          "  per 1M moves                   1835.4  1858.2   +1.2%            +22.8   +1.1%  0.9%  0.8%\n"
          "  per layer                         8.8     9.0   +1.9%             +0.2   +1.7%  0.9%  0.8%\n"
          "  per cm3                          71.6    72.6   +1.4%             +1.0   +1.3%  0.9%  0.8%\n"
          "  peak MiB                        690.0   702.0   +1.7%            +12.0   +1.7%  0.3%  0.2%\n"
          "\n"
          "slice/benchy/standard-0.20                                                                                  NOT COMPARED\n"
          "  skipped in b: fixture not available: corpus 'external' not fetched\n"
          "\n"
          "min a, min b, wall a and wall b are minimums over the iterations, in ms unless the row says otherwise\n"
          "slower, faster    the minimum moved at least 3% and more than either run's CV\n"
          "longer, shorter   the same, where the output changed, so the work differs too\n"
          "~                 at least 3% but within a run's CV, so it may be noise\n"
          "\n"
          "3 compared, 1 with changed output, 1 not compared\n");
}

TEST_CASE("each column of a comparison's tables holds one unit at one decimal, flush with its heading", "[OrcaBench][Reporters]")
{
    const std::string variant = GENERATE(as<std::string> {}, "three iterations", "verbose", "one iteration", "a name as wide as its column");
    CAPTURE(variant);
    Result a = before();
    Result b = after();
    if (variant == "a name as wide as its column")
        a.workloads[1].name = b.workloads[1].name = "slice/voron-design-cube-v7/classic-normal";
    const std::string text = variant == "one iteration" ? compare_text(first_iteration_of(a), first_iteration_of(b))
                                                        : compare_text(a, b, file_view(), false, variant == "verbose");

    const std::string headings[] = {"wall a", "wall b", "min a", "min b", "change", "diff", "by mean",
                                    "CV a",   "CV b",   "CPU a", "CPU b", "MiB a",  "MiB b"};
    const std::regex  figure(R"(<0\.1|[+-]?[0-9][0-9,]*\.[0-9]+[%x]?)");
    std::map<std::size_t, std::string>     edges;
    std::map<std::string, std::set<char>>  units;
    std::size_t                            figures = 0;
    std::istringstream                     lines(text);
    for (std::string line; std::getline(lines, line);) {
        const std::size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos) {
            edges.clear();
            continue;
        }
        if (start >= 2 && (line.compare(start, 5, "min a") == 0 || line.compare(start, 6, "wall a") == 0)) {
            for (const std::string& heading : headings)
                if (line.find(heading) != std::string::npos)
                    edges[line.find(heading) + heading.size()] = heading;
            continue;
        }
        const std::size_t first = edges.empty() ? 0 : edges.begin()->first - 8;
        if (edges.empty() || line.size() <= first)
            continue;
        for (auto match = std::sregex_iterator(line.begin() + first, line.end(), figure); match != std::sregex_iterator(); ++match) {
            const std::string token = match->str();
            const std::size_t end   = first + match->position() + match->length();
            const char        unit  = token.back() == '%' || token.back() == 'x' ? token.back() : ' ';
            CAPTURE(line, token);
            REQUIRE(edges.count(end) == 1);
            units[edges[end]].insert(unit);
            CHECK(token.size() - token.find('.') == (unit == ' ' ? 2u : 3u));
            ++figures;
        }
    }
    CHECK(figures > 100);
    for (const auto& [heading, found] : units) {
        CAPTURE(heading);
        CHECK(found.size() == 1);
    }
}

TEST_CASE("runs compared although they measured differently say so before anything else", "[OrcaBench][Reporters]")
{
    Result b                                = after();
    b.measurement[MeasurementKey::threads] = "8";
    b.measurement[MeasurementKey::warmup]  = "0";
    CHECK(compare_text(before(), b, file_view(), true)
              .find("   BENCH-PC  Linux 6.8.0  13th Gen Intel(R) Core(TM) i5-13600K  20 cores\n"
                    "\n"
                    "MEASURED DIFFERENTLY, compared because that was allowed\n"
                    "  threads  20 -> 8\n"
                    "  warmup  1 -> 0\n"
                    "\n"
                    "OUTPUT CHANGED in 1 workload") != std::string::npos);
}

TEST_CASE("a changed hash with the same work stats says the values changed rather than the work", "[OrcaBench][Reporters]")
{
    Result b                    = after();
    b.workloads[0].output_hash = 0x1234567800000000;
    CHECK(compare_text(before(), b)
              .find("OUTPUT CHANGED in 2 workloads, so their times below measure different work\n"
                    "  slice/extruder-idler/standard-0.20        hash 3bbecdba -> 12345678\n"
                    "                                            every work stat the same, so values changed rather than the work\n"
                    "  slice/overhangs/tree-supports             hash 5e2a90c1 -> 91c02e7f\n") != std::string::npos);
}

TEST_CASE("a hash or work stat only one run recorded shows as none", "[OrcaBench][Reporters]")
{
    Result b                    = after();
    b.workloads[2].output_hash = std::nullopt;
    b.workloads[2].work->metrics.clear();
    CHECK(compare_text(before(), b)
              .find("  slice/overhangs/tree-supports             hash 5e2a90c1 -> none\n"
                    "                                            moves 1,204,331 -> 1,211,904  +0.6%\n"
                    "                                            extrusion 30.9 -> 31.0 cm3  +0.4%\n"
                    "                                            moves.support 88,410 -> none\n") != std::string::npos);
}

TEST_CASE("a comparison under --verbose gives each scope its own row and folds nothing", "[OrcaBench][Reporters]")
{
    const std::string text = compare_text(before(), after(), file_view(), false, true);
    const std::string rows =
        "  psGCodeExport print             804.6   812.3   +1.0%             +7.7   +0.8%  0.5%  0.4%     -     -   412.0   415.0\n"
        "  posInfill object:0               90.0    83.1   -7.7%  faster     -6.9   -6.8%  1.4%  2.1% 14.8x 15.0x   398.0   391.0\n"
        "  posPrepareInfill object:0        83.0    89.6   +8.0%  slower     +6.6   +7.7%  1.7%  1.3% 12.1x  4.3x   401.0   402.0\n"
        "  posPerimeters object:0           68.2    68.9   +1.0%             +0.7   +0.3%  2.2%  1.5%     -     -       -       -\n"
        "  posSlice object:0                12.4    12.9   +4.0%~            +0.5   +5.4%  3.7%  4.9%     -     -       -       -\n"
        "  posSimplifyInfill object:0        3.0     3.0   +0.0%             +0.0   +0.0%  1.9%  1.9%     -     -       -       -\n"
        "  posContouring object:0            0.4     0.4   +0.0%             +0.0   +0.0%  0.0%  0.0%     -     -       -       -\n"
        "  posEstimateCurledExtrusions object:0 never finished in both\n"
        "  posSimplifySupportPath object:0 not run in both\n"
        "  ---";
    CHECK(text.find(rows) != std::string::npos);
    CHECK(text.find("other (") == std::string::npos);
}

TEST_CASE("a comparison's rows sort by the larger minimum, or by first start for pipeline order", "[OrcaBench][Reporters]")
{
    CompareView view = file_view();
    view.sort_by     = SortBy::Start;
    const std::string text = compare_text(before(), after(), view);
    const auto        at   = [&text](const std::string& stage) { return text.find("  " + stage + " "); };
    CHECK(at("posSlice") < at("posPerimeters"));
    CHECK(at("posPerimeters") < at("posPrepareInfill"));
    CHECK(at("posPrepareInfill") < at("posInfill"));
    CHECK(at("posInfill") < at("posEstimateCurledExtrusions"));
    CHECK(at("posEstimateCurledExtrusions") < at("psGCodeExport"));
    CHECK(at("psGCodeExport") < text.find("  other ("));
}

TEST_CASE("a change over one iteration is shown but not judged", "[OrcaBench][Reporters]")
{
    const std::string text = compare_text(first_iteration_of(before()), first_iteration_of(after()));
    const std::string infill =
        "  posInfill                        90.0    83.1   -7.7%             -6.9   -7.7%     -     - 14.8x 15.0x   398.0   391.0";
    const std::string cube =
        "  slice/voron-cube/standard-0.20              412.7   395.1   -4.3%                   -17.6     -     -       -       -";
    CHECK(has_line(text, infill));
    CHECK(has_line(text, cube));
    CHECK(has_line(text, "a change is not judged where a run has no CV, as with one iteration or a mean of zero"));
    CHECK(text.find("slower, faster") == std::string::npos);
}

TEST_CASE("a comparison marks shared readings, stages that also never finished and floored CPU, with a legend for each",
          "[OrcaBench][Reporters]")
{
    const auto [a, b]        = marked_runs();
    const std::string text   = compare_text(a, b);
    const std::string infill =
        "  posInfill*                       90.0    83.1   -7.7%  faster     -6.9   -6.8%  1.4%  2.1%     -     -   398.0   391.0";
    const std::string prepare =
        "  posPrepareInfill*                83.0    89.6   +8.0%  slower     +6.6   +7.7%  1.7%  1.3%     -     -   401.0   402.0";
    const std::string perimeters =
        "  posPerimeters!                   68.2    68.9   +1.0%             +0.7   +0.3%  2.2%  1.5%     -     -       -       -";
    CHECK(has_line(text, infill));
    CHECK(has_line(text, prepare));
    CHECK(has_line(text, perimeters));
    CHECK(text.find("\n*                 readings shared with stages that ran at the same time\n"
                    "!                 started at least once without finishing, so some of its time is in unaccounted\n"
                    "CPU left out where a run's steps in CPU time could skew it past 10%\n") != std::string::npos);
}

TEST_CASE("a stage only one run has, or whose state changed, shows what each run did", "[OrcaBench][Reporters]")
{
    const std::string text = compare_text(before(), restaged_after());
    CHECK(has_line(text, "  posSimplifyInfill            only in a"));
    CHECK(has_line(text, "  posIroning                   only in b"));
    CHECK(has_line(text, "  posContouring                0.4 -> instant"));
}

TEST_CASE("the header shows both machines when they differ, and every build or machine property that differs", "[OrcaBench][Reporters]")
{
    Result a             = before();
    Result b             = after();
    b.build.flags        = "/Od /Ob0";
    b.build.properties   = {{"pgo", "on"}};
    b.machine.host       = "BENCH-PC-2";
    a.machine.properties = {{MachineProperty::cpu_time_step_ns, "15625000"}};
    b.machine.properties = {{MachineProperty::cpu_time_step_ns, "100"}};
    CHECK(compare_text(a, b).find("a  2026-09-30 09:14 UTC  a83e6b7b74        Clang 22.1.3  Release                before.json\n"
                                  "b  2026-09-30 11:02 UTC  9f1c2d3e4a dirty  Clang 22.1.3  Release (unoptimized)  after.json\n"
                                  "   a flags  /O2 /Ob2 /DNDEBUG\n"
                                  "   b flags  /Od /Ob0\n"
                                  "   pgo  - -> on\n"
                                  "   a  BENCH-PC  Linux 6.8.0  13th Gen Intel(R) Core(TM) i5-13600K  20 cores\n"
                                  "   b  BENCH-PC-2  Linux 6.8.0  13th Gen Intel(R) Core(TM) i5-13600K  20 cores\n"
                                  "   cpu_time_step_ns  15625000 -> 100\n") != std::string::npos);
}

TEST_CASE("a workload compared without timed passes shows only its title", "[OrcaBench][Reporters]")
{
    const auto untimed = [](Result run) {
        run.workloads = {WorkloadResult::ran("slice/idler", 1, std::nullopt, {})};
        return run;
    };
    const std::string text = compare_text(untimed(before()), untimed(after()));
    const std::size_t at   = text.find("\nslice/idler");
    REQUIRE(at != std::string::npos);
    CHECK(text.substr(at) == "\nslice/idler" + std::string(93, ' ') +
                                 "output unchanged\n"
                                 "  no timed passes\n"
                                 "\n"
                                 "1 compared, 0 with changed output, 0 not compared\n");
}

TEST_CASE("a stage whose minimum in a is zero shows both minimums and no change", "[OrcaBench][Reporters]")
{
    Result     a       = before();
    StageSpan& stalled = a.workloads[0].iterations[0].timeline[1];
    REQUIRE(stalled.stage == "posPerimeters");
    stalled.done_at = stalled.started_at;
    CHECK(has_line(compare_text(a, after()), "  posPerimeters                     0.0    68.9"));
}

TEST_CASE("a workload's CPU left out below the floor brings the floor's legend", "[OrcaBench][Reporters]")
{
    const auto cube_only = [](Result run) {
        run.machine.properties = {{MachineProperty::cpu_time_step_ns, "15625000"}};
        run.workloads          = {run.workloads[1]};
        return run;
    };
    const std::string text = compare_text(cube_only(before()), cube_only(after()));
    CHECK(has_line(text, "  wall                            412.7   395.1   -4.3%  faster    -17.6   -4.3%  0.7%  0.5%     -     -"));
    CHECK(has_line(text, "CPU left out where a run's steps in CPU time could skew it past 10%"));
}

TEST_CASE("a work stat too large for a count prints as a plain figure", "[OrcaBench][Reporters]")
{
    Result a                                      = before();
    Result b                                      = after();
    a.workloads[2].work->metrics["moves.support"] = std::ldexp(1.0, 64);
    b.workloads[2].work->metrics["moves.support"] = std::ldexp(1.0, 65);
    CHECK(has_line(compare_text(a, b), std::string(44, ' ') + "moves.support 18446744073709551616.0 -> 36893488147419103232.0  +100.0%"));
}

TEST_CASE("a comparison prints each control character in a document's text as a space or ?", "[OrcaBench][Reporters]")
{
    Result b               = after();
    b.build.flags          = "/O2 /DNDEBUG\a";
    b.workloads[3].reason  = "fixture\tnot available: \x1b[2J\xc2\x9bK";
    CompareView view       = file_view();
    view.label_b           = "after\x1b[31m.json";
    const std::string text = compare_text(before(), b, view);
    CHECK(has_line(text, "   b flags  /O2 /DNDEBUG?"));
    CHECK(has_line(text, "  skipped in b: fixture not available: ?[2J?K"));
    CHECK(text.find("Release  after?[31m.json\n") != std::string::npos);
    CHECK(text.find('\x1b') == std::string::npos);
}

TEST_CASE("an error message keeps its line breaks and prints every other control character as a space or ?", "[OrcaBench][Reporters]")
{
    CHECK(printable_lines("the runs measured differently:\n  key\x1b[2J  a\tb\n") == "the runs measured differently:\n  key?[2J  a b\n");
}

TEST_CASE("a comparison counts the workloads that failed only in b after the others", "[OrcaBench][Reporters]")
{
    Result b                = after();
    b.workloads[1]          = WorkloadResult::failed(b.workloads[1].name, "threw");
    const std::string text  = compare_text(before(), b);
    CHECK(text.find("\n2 compared, 1 with changed output, 2 not compared, 1 failed only in b\n") != std::string::npos);
}

TEST_CASE("color changes no character of either view", "[OrcaBench][Reporters]")
{
    SECTION("a run")
    {
        Result run      = canned();
        run.build.flags = "/Od";
        run.machine.properties[MachineProperty::cpu_time_step_ns] = "15625000";
        for (IterationResult& iteration : run.workloads.front().iterations) {
            iteration.timeline[1].started_at -= 10ms;
            iteration.timeline[1].done_at -= 10ms;
        }
        run.workloads.front().iterations.front().unfinished.push_back({"posInfill", "object:1", Clock::time_point {} + 20ms});
        ReportOptions options;
        options.color          = true;
        const std::string text = render("console", run, options);
        CHECK(text != render("console", run));
        CHECK(without_color(text) == render("console", run));
    }
    SECTION("a comparison")
    {
        const auto [a, b]      = marked_runs();
        Result     mismatched  = restaged_after();
        mismatched.build.flags = "/Od";
        mismatched.measurement[MeasurementKey::threads] = "8";
        const std::pair<Result, Result> runs[] = {
            {before(), after()}, {a, b}, {before(), mismatched}, {first_iteration_of(before()), first_iteration_of(after())}};
        for (const auto& [x, y] : runs)
            for (const bool verbose : {false, true}) {
                CompareView       view  = file_view();
                const std::string plain = compare_text(x, y, view, true, verbose);
                view.color              = true;
                const std::string text  = compare_text(x, y, view, true, verbose);
                CHECK(text != plain);
                CHECK(without_color(text) == plain);
            }
    }
}

TEST_CASE("a colored comparison paints verdicts, alarms and what fell below the bar", "[OrcaBench][Reporters]")
{
    CompareView view = file_view();
    view.color       = true;
    const std::string text = compare_text(before(), after(), view);
    CHECK_THAT(text, StartsWith(strong + "orca_bench compare" + reset + "  quick"));
    CHECK_THAT(text, ContainsSubstring(inverse + "OUTPUT CHANGED" + reset + " in 1 workload"));
    CHECK_THAT(text, ContainsSubstring(inverse + "output changed" + reset));
    CHECK_THAT(text, ContainsSubstring(strong + "slice/voron-cube/standard-0.20" + reset + " "));
    CHECK_THAT(text, ContainsSubstring(better + "-4.3%" + reset + "  " + better + "faster" + reset));
    CHECK_THAT(text, ContainsSubstring(faint + "output unchanged" + reset));
    CHECK_THAT(text, ContainsSubstring(faint + "   min a   min b  change"));
    CHECK_THAT(text, ContainsSubstring(strong + "posPrepareInfill" + reset));
    CHECK_THAT(text, ContainsSubstring(worse + "+8.0%" + reset + "  " + worse + "slower" + reset));
    CHECK_THAT(text, ContainsSubstring(better + "-7.7%" + reset + "  " + better + "faster" + reset));
    CHECK_THAT(text, ContainsSubstring(worse + "+5.5%" + reset + "  " + worse + "longer" + reset));
    CHECK_THAT(text, ContainsSubstring(faint + "+4.0%" + reset + faint + "~" + reset));
    CHECK_THAT(text, ContainsSubstring("68.9   " + faint + "+1.0%" + reset));
    CHECK_THAT(text, ContainsSubstring(worse + "never finished in both" + reset));
    CHECK_THAT(text, ContainsSubstring("  " + faint + std::string(118, '-') + reset));
    CHECK_THAT(text, ContainsSubstring(worse + "slower" + reset + ", " + better + "faster" + reset + "    " + faint + "the minimum moved"));
}

TEST_CASE("a colored run paints outcomes, stages that never finished and its marks", "[OrcaBench][Reporters]")
{
    Result run      = canned();
    run.build.flags = "/Od";
    ReportOptions options;
    options.color          = true;
    const std::string text = render("console", run, options);
    CHECK_THAT(text, StartsWith(strong + "orca_bench" + reset + "  quick"));
    CHECK_THAT(text, ContainsSubstring("Release " + worse + "(unoptimized)" + reset));
    CHECK_THAT(text, ContainsSubstring(strong + "slice/extruder-idler/standard-0.20" + reset));
    CHECK_THAT(text, ContainsSubstring(worse + "never finished" + reset));
    CHECK_THAT(text, ContainsSubstring("4.6%" + faint + "~" + reset));
    CHECK_THAT(text, ContainsSubstring(strong + "SKIPPED" + reset));
    CHECK_THAT(text, ContainsSubstring(inverse + "FAILED" + reset));
    CHECK_THAT(text, ContainsSubstring(faint + "~ CV above the 3% significance bar"));
}
