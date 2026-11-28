#include "core/Result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

WorkloadResult did_not_run(std::string name, Outcome outcome, std::string reason)
{
    if (reason.empty())
        throw std::invalid_argument(name + " did not run and gives no reason");
    WorkloadResult result;
    result.name    = std::move(name);
    result.outcome = outcome;
    result.reason  = std::move(reason);
    return result;
}

} // namespace

Clock::time_point origin(const IterationResult& iteration)
{
    std::optional<Clock::time_point> earliest;
    const auto                       consider = [&earliest](Clock::time_point at) {
        if (!earliest || at < *earliest)
            earliest = at;
    };
    for (const StageSpan& span : iteration.timeline)
        consider(span.started_at);
    for (const UnfinishedStage& stage : iteration.unfinished)
        consider(stage.started_at);
    return earliest.value_or(Clock::time_point {});
}

WorkloadResult WorkloadResult::ran(std::string name, std::optional<std::uint64_t> output_hash, std::optional<WorkStats> work,
                                   std::vector<IterationResult> iterations)
{
    WorkloadResult result;
    result.name        = std::move(name);
    result.outcome     = Outcome::Ran;
    result.output_hash = output_hash;
    result.work        = std::move(work);
    result.iterations  = std::move(iterations);
    return result;
}

WorkloadResult WorkloadResult::skipped(std::string name, std::string reason)
{
    return did_not_run(std::move(name), Outcome::Skipped, std::move(reason));
}

WorkloadResult WorkloadResult::failed(std::string name, std::string reason)
{
    return did_not_run(std::move(name), Outcome::Failed, std::move(reason));
}

std::size_t count_of(const Result& result, Outcome outcome)
{
    return static_cast<std::size_t>(std::count_if(result.workloads.begin(), result.workloads.end(),
        [outcome](const WorkloadResult& workload) { return workload.outcome == outcome; }));
}

}} // namespace Slic3r::Bench
