#pragma once

#include "core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

// Where a span happened, either the whole print or one of its objects.
class Scope
{
public:
    static Scope print();
    static Scope object(std::size_t index);

    // "print" or "object:N", the form StageSpan::scope holds.
    const std::string& text() const { return m_text; }

private:
    explicit Scope(std::string text) : m_text(std::move(text)) {}

    std::string m_text;
};

// What one iteration of a workload reports, which the real workloads and the tests' fake both
// write through, so nothing between a workload and its result depends on slicing.
class Measurement
{
public:
    // The Runner builds one for each iteration with the time execute() starts.
    explicit Measurement(Clock::time_point iteration_start) : m_iteration_start(iteration_start) {}

    // Throws std::invalid_argument for a span without a stage, one that ends before it starts, starts
    // before its iteration or ends after it is reported, a stage already reported for its scope, or a
    // metric that is not finite or that the sampler writes.
    void span(std::string stage, const Scope& scope, Clock::time_point started_at, Clock::time_point done_at,
              Metrics metrics = {});

    // Throws std::invalid_argument for a step without a stage, one that starts before its iteration or
    // after it is reported, or a stage already reported for its scope.
    void unfinished(std::string stage, const Scope& scope, Clock::time_point started_at);

    // Throws std::invalid_argument for a step without a stage or a stage already reported for its scope.
    void not_run(std::string stage, const Scope& scope);

    // A measurement of the whole iteration, beside the wall time and CPU time the Runner takes,
    // refused with std::invalid_argument when it is not finite or its key was reported already.
    void metric(std::string key, double value);

    // Each throws std::invalid_argument when reported twice in one iteration, and work() also for a
    // number that is not finite. The hash leaves out whatever varies between runs of the same input,
    // such as the time the G-code header records, since every pass has to reproduce the first one's.
    void work(WorkStats work);
    void output_hash(std::uint64_t hash);

    const Timeline&                     timeline() const { return m_timeline; }
    const std::vector<UnfinishedStage>& unfinished_stages() const { return m_unfinished; }
    const std::vector<StageNotRun>&     stages_not_run() const { return m_not_run; }
    const Metrics&                      metrics() const { return m_metrics; }
    const std::optional<WorkStats>&     work_stats() const { return m_work; }
    const std::optional<std::uint64_t>& hash() const { return m_hash; }

private:
    void claim(const std::string& stage, const Scope& scope);

    Clock::time_point                             m_iteration_start;
    Timeline                                      m_timeline;
    std::vector<UnfinishedStage>                  m_unfinished;
    std::vector<StageNotRun>                      m_not_run;
    std::set<std::pair<std::string, std::string>> m_reported;
    Metrics                                       m_metrics;
    std::optional<WorkStats>                      m_work;
    std::optional<std::uint64_t>                  m_hash;
};

}} // namespace Slic3r::Bench
