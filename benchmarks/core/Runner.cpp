#include "core/Runner.hpp"

#include "core/BuildId.hpp"
#include "core/Host.hpp"
#include "core/Measurement.hpp"
#include "core/Policy.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Sampler.hpp"
#include "core/Workload.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

// Leaves the environment when the run ends, provided enter() returned.
class EnvironmentScope
{
public:
    EnvironmentScope(RunEnvironment& environment, const Policy& policy) : m_environment(environment)
    {
        m_environment.enter(policy);
    }
    ~EnvironmentScope() { m_environment.leave(); }

    EnvironmentScope(const EnvironmentScope&)            = delete;
    EnvironmentScope& operator=(const EnvironmentScope&) = delete;

private:
    RunEnvironment& m_environment;
};

BuildIdentity this_build()
{
    BuildIdentity build;
    build.revision         = build_revision;
    build.dirty            = build_dirty;
    build.compiler         = build_compiler;
    build.compiler_version = build_compiler_version;
    build.config           = build_config;
    build.flags            = build_flags;
    return build;
}

MachineIdentity this_machine()
{
    MachineIdentity machine;
    machine.host          = host_name();
    machine.os            = os_description();
    machine.cpu           = cpu_model();
    machine.logical_cores = logical_cores();
    if (const std::chrono::nanoseconds step = cpu_time_step(); step > std::chrono::nanoseconds::zero())
        machine.properties[MachineProperty::cpu_time_step_ns] = std::to_string(step.count());
    return machine;
}

bool same_work(const std::optional<WorkStats>& a, const std::optional<WorkStats>& b)
{
    if (!a || !b)
        return !a && !b;
    // Names every field, so one added to WorkStats does not compile here until it is compared.
    const auto& [moves, layers, gcode_bytes, travel_moves, extrusion_mm3, metrics] = *a;
    return std::tie(moves, layers, gcode_bytes, travel_moves, extrusion_mm3, metrics) ==
           std::tie(b->moves, b->layers, b->gcode_bytes, b->travel_moves, b->extrusion_mm3, b->metrics);
}

using StageKeys = std::vector<std::pair<std::string, std::string>>;

// The stage and scope of each report, sorted, so two passes compare in whatever order a workload
// reports.
template<class Reports> StageKeys keys_of(const Reports& reports)
{
    StageKeys keys;
    for (const auto& report : reports)
        keys.emplace_back(report.stage, report.scope);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// Which stages finished, stayed unfinished and never ran, in each scope.
using StageStates = std::tuple<StageKeys, StageKeys, StageKeys>;

StageStates stage_states(const Measurement& measurement)
{
    return {keys_of(measurement.timeline()), keys_of(measurement.unfinished_stages()), keys_of(measurement.stages_not_run())};
}

template<class Event, class... Args> void notify(const Event& event, Args&&... args)
{
    if (event)
        event(std::forward<Args>(args)...);
}

// "warmup pass 1" or "timed pass 2", where pass counts from 1 across both.
std::string pass_name(std::uint64_t pass, unsigned warmup)
{
    return pass <= warmup ? "warmup pass " + std::to_string(pass) : "timed pass " + std::to_string(pass - warmup);
}

// A failed query reads 0, which must not wrap around into a huge duration.
Clock::duration cpu_between(std::uint64_t before_ns, std::uint64_t after_ns)
{
    return std::chrono::nanoseconds(after_ns > before_ns ? static_cast<std::int64_t>(after_ns - before_ns) : 0);
}

WorkloadResult run_workload(const CatalogEntry& entry, const Policy& policy, const WorkloadKinds& kinds,
                            const RunContext& context, const ProcessProbe& probe, const RunEvents& events)
{
    const bool record_passes = policy.metrics.count(Metric::Wall) != 0;
    const bool record_hash   = policy.metrics.count(Metric::Hash) != 0;
    const bool record_work   = policy.metrics.count(Metric::Work) != 0;
    const bool record_memory = policy.metrics.count(Metric::Rss) != 0;
    const std::string first  = pass_name(1, policy.warmup);

    std::string call = "create()";
    std::string when;
    const auto  threw = [&](const std::string& what) {
        return WorkloadResult::failed(entry.name, call + " threw" + when + ": " + what);
    };
    std::optional<std::uint64_t> hash;
    std::optional<WorkStats>     work;
    std::vector<IterationResult> iterations;
    try {
        const std::unique_ptr<Workload> workload = kinds.create(entry);
        call = "setup()";
        if (const std::optional<std::string> skip = workload->setup(context)) {
            if (skip->empty())
                return WorkloadResult::failed(entry.name, "setup() gave an empty reason to skip");
            return WorkloadResult::skipped(entry.name, *skip);
        }

        std::optional<StageStates> states;
        // 64 bits, so the count cannot wrap whatever the policy's counts are.
        const std::uint64_t passes = std::uint64_t(policy.warmup) + policy.iterations;
        for (std::uint64_t pass = 1; pass <= passes; ++pass) {
            const std::string name = pass_name(pass, policy.warmup);
            when = " on " + name;
            call = "prepare()";
            workload->prepare(context);

            call = "execute()";
            const bool timed = pass > policy.warmup && record_passes;
            Sampler    sampler(probe);
            if (timed)
                sampler.start();
            const Reading before = probe();
            Measurement   measurement(before.at);
            workload->execute(context, measurement);
            const Reading        after    = probe();
            std::vector<Reading> readings = timed ? sampler.stop() : std::vector<Reading>();

            const std::optional<std::uint64_t> pass_hash   = record_hash ? measurement.hash() : std::nullopt;
            const std::optional<WorkStats>     pass_work   = record_work ? measurement.work_stats() : std::nullopt;
            const std::optional<StageStates>   pass_states = record_passes ? std::make_optional(stage_states(measurement)) : std::nullopt;
            if (pass == 1) {
                hash   = pass_hash;
                work   = pass_work;
                states = pass_states;
            } else if (pass_hash != hash) {
                return WorkloadResult::failed(entry.name, "the output hash of " + name + " differs from " + first);
            } else if (!same_work(pass_work, work)) {
                return WorkloadResult::failed(entry.name, "the work stats of " + name + " differ from " + first);
            } else if (pass_states != states) {
                return WorkloadResult::failed(entry.name, "the stages of " + name + " differ from " + first);
            }

            if (timed) {
                readings.push_back(before);
                readings.push_back(after);
                std::sort(readings.begin(), readings.end(), [](const Reading& a, const Reading& b) { return a.at < b.at; });
                IterationResult iteration;
                iteration.wall = after.at - before.at;
                iteration.cpu  = cpu_between(before.cpu_ns, after.cpu_ns);
                if (record_memory)
                    iteration.peak_rss_bytes = peak_rss(readings, before.at, after.at);
                iteration.timeline = measurement.timeline();
                for (StageSpan& span : iteration.timeline)
                    for (const auto& [key, value] : sampled_metrics(readings, span.started_at, span.done_at, record_memory))
                        span.metrics.emplace(key, value);
                iteration.unfinished = measurement.unfinished_stages();
                iteration.not_run    = measurement.stages_not_run();
                iteration.metrics    = measurement.metrics();
                iterations.push_back(std::move(iteration));
            }
            call = "pass_done";
            notify(events.pass_done, PassDone {pass, policy.warmup, passes, after.at - before.at});
        }
    } catch (const std::exception& error) {
        return threw(error.what());
    } catch (...) {
        return threw("something that is not a std::exception");
    }
    return WorkloadResult::ran(entry.name, hash, std::move(work), std::move(iterations));
}

} // namespace

Result run_suite(const std::vector<CatalogEntry>& entries, const Policy& policy, const WorkloadKinds& kinds,
                 RunEnvironment& environment, const ProcessProbe& probe, const RunEvents& events, const std::string& dump_dir)
{
    std::set<std::string> names;
    for (const CatalogEntry& entry : entries) {
        validate(entry);
        if (!names.insert(entry.name).second)
            throw WorkloadError("the catalog entry name '" + entry.name + "' appears twice");
    }

    Result result;
    result.suite.started_at = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const Clock::time_point started_at = Clock::now();
    result.measurement                 = policy.identity();
    result.build                       = this_build();
    result.machine                     = this_machine();

    RunContext context;
    context.timed    = policy.stages;
    context.dump_dir = dump_dir;
    std::vector<const CatalogEntry*> admitted;
    for (const CatalogEntry& entry : entries)
        if (!policy.pgo_eligible_only || is_pgo_eligible(entry))
            admitted.push_back(&entry);
    if (admitted.empty() && !entries.empty())
        throw WorkloadError(policy.name + " runs only the workloads profile training may, and none of those selected is one");
    {
        const EnvironmentScope scope(environment, policy);
        notify(events.started, result);
        for (std::size_t i = 0; i < admitted.size(); ++i) {
            notify(events.workload_started, i + 1, admitted.size(), admitted[i]->name);
            result.workloads.push_back(run_workload(*admitted[i], policy, kinds, context, probe, events));
            notify(events.workload_done, result.workloads.back());
        }
    }
    result.suite.duration = Clock::now() - started_at;
    notify(events.finished, result);
    return result;
}

}} // namespace Slic3r::Bench
