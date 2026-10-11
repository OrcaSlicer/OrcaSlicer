#pragma once

#include "core/Result.hpp"

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Slic3r { namespace Bench {

// Thrown for a policy or an override a run cannot use.
class PolicyError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// The parts of the pipeline a run can time, in pipeline order.
enum class Stage { Load, Process, Export };

// What a run collects, where Wall includes CPU time and the step spans.
enum class Metric { Wall, Rss, Hash, Work };

// Ordered sets, so a list written from one follows its enum's order.
using StageSet  = std::set<Stage>;
using MetricSet = std::set<Metric>;

// Reads a comma-separated list such as "process, export" in any order, ignoring the spaces around
// each name and throwing PolicyError for a name it does not know.
StageSet parse_stages(std::string_view list);

// The set's names, or "none" when it is empty.
std::string to_string(const StageSet& stages);
std::string to_string(const MetricSet& metrics);

// What a run's options change on top of its preset, where an unset field keeps the preset's value.
struct PolicyOverrides
{
    // 0 for every hardware thread.
    std::optional<unsigned> threads;
    std::optional<unsigned> warmup;
    std::optional<unsigned> iterations;
    std::optional<StageSet> stages;
};

// How a run measures, resolved from a preset and the run's overrides.
struct Policy
{
    std::string name;
    unsigned    threads    = 0;
    unsigned    warmup     = 0;
    unsigned    iterations = 0;
    StageSet    stages;
    MetricSet   metrics;
    // Whether the run takes only the workloads marked for profile training.
    bool        pgo_eligible_only = false;

    // Drops the work stats when the stages leave out export, since they come from the G-code only
    // export writes, and the hash when they also leave out load, the other stage that makes one. Throws
    // PolicyError for anything a run cannot use, such as an unknown preset or more threads than the
    // hardware runs.
    static Policy resolve(std::string_view preset, const PolicyOverrides& overrides, unsigned hardware_threads);

    // Every setting compare enforces, written here and nowhere else.
    MeasurementIdentity identity() const;
};

}} // namespace Slic3r::Bench
