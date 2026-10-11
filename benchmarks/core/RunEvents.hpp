#pragma once

#include "core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r { namespace Bench {

// One finished pass, counted from 1 across the warmup and timed passes.
struct PassDone
{
    std::uint64_t   pass    = 0;
    std::uint64_t   warmups = 0;
    std::uint64_t   passes  = 0;
    Clock::duration wall {};
};

// What the Runner reports as it runs, never while a pass is timed or sampled, so a listener cannot
// slow a measurement.
struct RunEvents
{
    // With the identities and no workloads, once the environment is entered.
    std::function<void(const Result&)> started;
    // Before a workload is created, counting from 1 among the entries the policy admits.
    std::function<void(std::size_t index, std::size_t count, const std::string& name)> workload_started;
    // After each pass, warmups included, where a listener that throws fails the workload.
    std::function<void(const PassDone&)> pass_done;
    // With the workload's result, whatever its outcome.
    std::function<void(const WorkloadResult&)> workload_done;
    // With the whole result, once the environment is left.
    std::function<void(const Result&)> finished;
};

}} // namespace Slic3r::Bench
