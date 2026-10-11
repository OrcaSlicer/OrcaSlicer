#pragma once

#include "core/Policy.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Sampler.hpp"
#include "core/Workload.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace Bench {

// What a run sets up around all its workloads, such as the thread cap, declared here so core
// stays free of the slicer's dependencies.
class RunEnvironment
{
public:
    virtual ~RunEnvironment() = default;

    // Called once before the first workload, where an exception ends the run.
    virtual void enter(const Policy& policy) = 0;

    // Called once after the last workload, and only when enter() returned.
    virtual void leave() noexcept = 0;
};

// Runs each entry the policy admits through setup() once, then prepare() and execute() for every
// warmup and timed pass, where a workload that throws, or whose pass differs from its first, fails
// with a reason and the run goes on. Throws WorkloadError before entering the environment for an
// entry name that validate() refuses or that repeats, since results are keyed by name, and for entries
// the policy admits none of, since a run of nothing would look like a pass. The probe reads
// the process, through Host unless a test scripts it, and a dump directory, when given, reaches each kind
// through RunContext.
Result run_suite(const std::vector<CatalogEntry>& entries, const Policy& policy, const WorkloadKinds& kinds,
                 RunEnvironment& environment, const ProcessProbe& probe = host_reading, const RunEvents& events = {},
                 const std::string& dump_dir = {});

}} // namespace Slic3r::Bench
