#include "core/Policy.hpp"

#include "core/Result.hpp"
#include "core/Sampler.hpp"
#include "core/Text.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

// Each enumerator beside its name, which in_enum_order() checks against the enum.
template<class Enum> using Named = std::pair<Enum, const char*>;

constexpr Named<Stage>  stage_names[]  = {{Stage::Load, "load"}, {Stage::Process, "process"}, {Stage::Export, "export"}};
constexpr Named<Metric> metric_names[] = {{Metric::Wall, "wall"}, {Metric::Rss, "rss"}, {Metric::Hash, "hash"}, {Metric::Work, "work"}};

// Whether every enumerator up to last has an entry, each at the index its value names.
template<class Enum, std::size_t N> constexpr bool in_enum_order(const Named<Enum> (&names)[N], Enum last)
{
    for (std::size_t i = 0; i < N; ++i)
        if (names[i].first != Enum(i))
            return false;
    return N == std::size_t(last) + 1;
}
static_assert(in_enum_order(stage_names, Stage::Export));
static_assert(in_enum_order(metric_names, Metric::Work));

// Recorded before either can vary, so every result has them and stays comparable once they can.
constexpr const char* fixture_providers = "embedded,handy";
constexpr const char* no_affinity       = "none";

struct Preset
{
    Policy policy;
    // Whether the preset refuses a warmup or iteration count from the run's options.
    bool   counts_fixed = false;
};

// One entry per preset, where 0 threads means every hardware thread.
const std::vector<Preset>& presets()
{
    static const MetricSet every_metric = {Metric::Wall, Metric::Rss, Metric::Hash, Metric::Work};
    // name, threads, warmup, iterations, stages, metrics, pgo_eligible_only; counts_fixed
    static const std::vector<Preset> table = {
        {{"quick", 0, 1, 3, {Stage::Load, Stage::Process, Stage::Export}, every_metric, false}, false},
        {{"precise", 1, 2, 10, {Stage::Load, Stage::Process, Stage::Export}, every_metric, false}, false},
        {{"verify", 0, 0, 1, {Stage::Load, Stage::Process, Stage::Export}, {Metric::Hash}, false}, false},
        {{"pgo", 0, 0, 1, {Stage::Load, Stage::Process, Stage::Export}, {}, true}, true},
    };
    return table;
}

// Joins what name() gives for each item with separator, in the range's order.
template<class Range, class Name> std::string joined(const Range& items, const char* separator, Name name)
{
    std::string list;
    for (const auto& item : items) {
        if (!list.empty())
            list += separator;
        list += name(item);
    }
    return list;
}

// The members' names in enum order, or "none" for an empty set.
template<class Enum, std::size_t N> std::string listed(const std::set<Enum>& members, const Named<Enum> (&names)[N])
{
    if (members.empty())
        return "none";
    return joined(members, ",", [&names](Enum member) { return names[std::size_t(member)].second; });
}

} // namespace

StageSet parse_stages(std::string_view list)
{
    StageSet stages;
    for (std::size_t begin = 0; begin <= list.size();) {
        const std::size_t end  = std::min(list.find(',', begin), list.size());
        const std::string name = trimmed(list.substr(begin, end - begin));
        const auto found       = std::find_if(std::begin(stage_names), std::end(stage_names),
                                              [&name](const Named<Stage>& known) { return name == known.second; });
        if (found == std::end(stage_names))
            throw PolicyError("unknown stage '" + name + "' in '" + std::string(list) + "', expected " +
                              joined(stage_names, ", ", [](const Named<Stage>& known) { return known.second; }));
        stages.insert(found->first);
        begin = end + 1;
    }
    return stages;
}

std::string to_string(const StageSet& stages) { return listed(stages, stage_names); }

std::string to_string(const MetricSet& metrics) { return listed(metrics, metric_names); }

Policy Policy::resolve(std::string_view preset, const PolicyOverrides& overrides, unsigned hardware_threads)
{
    const std::vector<Preset>& table = presets();
    const auto found = std::find_if(table.begin(), table.end(), [preset](const Preset& entry) { return entry.policy.name == preset; });
    if (found == table.end())
        throw PolicyError("unknown policy '" + std::string(preset) + "', expected " +
                          joined(table, ", ", [](const Preset& entry) { return entry.policy.name; }));
    if (hardware_threads == 0)
        throw PolicyError("the hardware thread count is 0");
    if (found->counts_fixed && (overrides.warmup || overrides.iterations))
        throw PolicyError(found->policy.name + " fixes its warmup and iteration counts");

    Policy policy     = found->policy;
    policy.threads    = overrides.threads.value_or(policy.threads);
    policy.warmup     = overrides.warmup.value_or(policy.warmup);
    policy.iterations = overrides.iterations.value_or(policy.iterations);
    policy.stages     = overrides.stages.value_or(policy.stages);
    if (policy.threads == 0)
        policy.threads = hardware_threads;
    if (policy.threads > hardware_threads)
        throw PolicyError("a run cannot use " + std::to_string(policy.threads) + " threads where the hardware runs " +
                          std::to_string(hardware_threads));
    if (policy.iterations == 0)
        throw PolicyError("a run needs at least one iteration");
    if (policy.stages.empty())
        throw PolicyError("a run needs at least one stage to time");
    // The work stats come from the G-code export writes, and a hash from that G-code or from the file load reads.
    if (policy.stages.count(Stage::Export) == 0) {
        std::size_t dropped = policy.metrics.erase(Metric::Work);
        if (policy.stages.count(Stage::Load) == 0)
            dropped += policy.metrics.erase(Metric::Hash);
        if (dropped != 0 && policy.metrics.empty())
            throw PolicyError(policy.name + " collects only what export or load produces, so it needs one of those stages");
    }
    return policy;
}

MeasurementIdentity Policy::identity() const
{
    // Naming every field makes a new one a compile error here until it is recorded or left out below.
    const auto& [policy_name, thread_count, warmup_count, iteration_count, timed_stages, collected_metrics, eligible_only] = *this;
    // Left out, since it only selects workloads and compare matches workloads by name.
    static_cast<void>(eligible_only);
    return {{MeasurementKey::policy, policy_name},
            {MeasurementKey::threads, std::to_string(thread_count)},
            {MeasurementKey::warmup, std::to_string(warmup_count)},
            {MeasurementKey::iterations, std::to_string(iteration_count)},
            {MeasurementKey::stages, to_string(timed_stages)},
            {MeasurementKey::metrics, to_string(collected_metrics)},
            {MeasurementKey::corpus, fixture_providers},
            {MeasurementKey::affinity, no_affinity},
            {MeasurementKey::sampling, std::to_string(sampling_interval.count()) + "ms"}};
}

}} // namespace Slic3r::Bench
