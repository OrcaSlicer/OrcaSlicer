#include "core/Summary.hpp"

#include "core/Result.hpp"
#include "core/Sampler.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ratio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

using RowKey = std::pair<std::string, std::string>;

// What summarize() gathers for one row across the iterations.
struct Tally
{
    bool                         ran        = false;
    bool                         instant    = true;
    bool                         unfinished = false;
    bool                         shared     = false;
    double                       cpu_ns     = 0.0;
    double                       window_ns  = 0.0;
    std::size_t                  windows    = 0;
    Clock::duration              first_start = Clock::duration::max();
    // Absent for an iteration with no span of the row, such as one where it never finished, whose time is unknown.
    std::vector<std::optional<Clock::duration>> per_iteration;
    // The highest reading of the row's spans in each iteration, absent where none has one.
    std::vector<std::optional<std::uint64_t>> peak_per_iteration;
};

template<typename T> std::optional<T> lowest(const std::vector<std::optional<T>>& values)
{
    std::optional<T> low;
    for (const std::optional<T>& value : values)
        if (value)
            low = std::min(low.value_or(*value), *value);
    return low;
}

double nanoseconds(Clock::duration duration) { return std::chrono::duration<double, std::nano>(duration).count(); }

// Whether a CPU time from this many pairs of readings could be off by more than cpu_error_limit of
// itself, given that each of the run's threads and the sampler's can round by a step per pair.
bool below_floor(const Result& header, std::size_t pairs, double cpu_ns)
{
    const std::chrono::nanoseconds step = recorded_cpu_time_step(header.machine);
    if (step == std::chrono::nanoseconds::zero())
        return false;
    unsigned   threads = 0;
    const auto found   = header.measurement.find(MeasurementKey::threads);
    if (found != header.measurement.end())
        std::from_chars(found->second.data(), found->second.data() + found->second.size(), threads);
    if (threads == 0)
        threads = std::max(header.machine.logical_cores, 1u);
    return double(pairs) * (double(threads) + 1) * nanoseconds(step) > cpu_error_limit * cpu_ns;
}

} // namespace

Spread spread_of(const std::vector<double>& values)
{
    Spread spread;
    if (values.empty())
        return spread;
    spread.min  = *std::min_element(values.begin(), values.end());
    spread.mean = std::accumulate(values.begin(), values.end(), 0.0) / double(values.size());
    if (values.size() > 1 && spread.mean > 0) {
        double squares = 0.0;
        for (const double value : values)
            squares += (value - spread.mean) * (value - spread.mean);
        spread.cv = std::sqrt(squares / double(values.size() - 1)) / spread.mean;
    }
    return spread;
}

Clock::duration covered(const Timeline& timeline)
{
    std::vector<std::pair<Clock::time_point, Clock::time_point>> spans;
    for (const StageSpan& span : timeline)
        spans.emplace_back(span.started_at, span.done_at);
    std::sort(spans.begin(), spans.end());
    Clock::duration   total {};
    Clock::time_point reached = Clock::time_point::min();
    for (const auto& [started_at, done_at] : spans) {
        const Clock::time_point from = std::max(started_at, reached);
        if (done_at > from)
            total += done_at - from;
        reached = std::max(reached, done_at);
    }
    return total;
}

std::chrono::nanoseconds recorded_cpu_time_step(const MachineIdentity& machine)
{
    const auto found = machine.properties.find(MachineProperty::cpu_time_step_ns);
    if (found == machine.properties.end())
        return {};
    const std::string& text  = found->second;
    std::int64_t       count = 0;
    const auto [end, error]  = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc() || end != text.data() + text.size() || count < 0)
        return {};
    return std::chrono::nanoseconds(count);
}

WorkloadSummary summarize(const WorkloadResult& workload, const Result& header, bool verbose)
{
    WorkloadSummary   summary;
    const std::size_t count = workload.iterations.size();
    if (count == 0)
        return summary;

    std::map<RowKey, Tally> tallies;
    const auto              key_of = [verbose](const std::string& stage, const std::string& scope) {
        return RowKey(stage, verbose ? scope : std::string());
    };
    const auto tally_of = [&tallies, count](const RowKey& key) -> Tally& {
        Tally& tally = tallies[key];
        tally.per_iteration.resize(count);
        tally.peak_per_iteration.resize(count);
        return tally;
    };

    Clock::duration                           summed {}, wall {}, uncovered {}, cpu {};
    std::vector<std::optional<std::uint64_t>> peaks;
    for (std::size_t i = 0; i < count; ++i) {
        const IterationResult&  iteration = workload.iterations[i];
        const Clock::time_point zero      = origin(iteration);
        std::vector<Tally*>     span_tallies;
        for (const StageSpan& span : iteration.timeline) {
            Tally& tally = tally_of(key_of(span.stage, span.scope));
            span_tallies.push_back(&tally);
            const Clock::duration length = span.done_at - span.started_at;
            tally.per_iteration[i]       = tally.per_iteration[i].value_or(Clock::duration::zero()) + length;
            tally.ran                    = true;
            tally.instant                = tally.instant && length == Clock::duration::zero();
            tally.first_start            = std::min(tally.first_start, span.started_at - zero);
            const auto used              = span.metrics.find(SampledMetric::cpu_ns);
            const auto window            = span.metrics.find(SampledMetric::cpu_window_ns);
            if (used != span.metrics.end() && window != span.metrics.end()) {
                tally.cpu_ns += used->second;
                tally.window_ns += window->second;
                ++tally.windows;
            }
            // The cast is undefined for a reading no count of bytes can be, which a hand-edited result may hold.
            const auto peak = span.metrics.find(SampledMetric::peak_rss_bytes);
            if (peak != span.metrics.end() && peak->second >= 0 && peak->second < double(std::numeric_limits<std::uint64_t>::max()))
                tally.peak_per_iteration[i] = std::max(tally.peak_per_iteration[i].value_or(0), static_cast<std::uint64_t>(peak->second));
            summed += length;
        }
        std::vector<std::size_t> by_start(span_tallies.size());
        std::iota(by_start.begin(), by_start.end(), std::size_t(0));
        std::sort(by_start.begin(), by_start.end(), [&iteration](std::size_t a, std::size_t b) {
            return iteration.timeline[a].started_at < iteration.timeline[b].started_at;
        });
        for (std::size_t a = 0; a < by_start.size(); ++a) {
            const StageSpan& one = iteration.timeline[by_start[a]];
            for (std::size_t b = a + 1; b < by_start.size(); ++b) {
                const StageSpan& other = iteration.timeline[by_start[b]];
                if (other.started_at >= one.done_at)
                    break;
                Tally* const first  = span_tallies[by_start[a]];
                Tally* const second = span_tallies[by_start[b]];
                if (first != second && one.started_at < other.done_at)
                    first->shared = second->shared = true;
            }
        }
        for (const UnfinishedStage& stage : iteration.unfinished) {
            Tally& tally      = tally_of(key_of(stage.stage, stage.scope));
            tally.unfinished  = true;
            tally.first_start = std::min(tally.first_start, stage.started_at - zero);
        }
        for (const StageNotRun& stage : iteration.not_run)
            tally_of(key_of(stage.stage, stage.scope));
        wall += iteration.wall;
        uncovered += iteration.wall - covered(iteration.timeline);
        cpu += iteration.cpu;
        peaks.push_back(iteration.peak_rss_bytes);
    }
    summary.peak_rss_bytes = lowest(peaks);

    summary.summed_work = Millis(summed) / double(count);
    summary.wall        = Millis(wall) / double(count);
    summary.unaccounted = Millis(uncovered) / double(count);
    if (wall > Clock::duration::zero()) {
        if (below_floor(header, count, nanoseconds(cpu)))
            summary.cpu_below_floor = true;
        else
            summary.cpu = nanoseconds(cpu) / nanoseconds(wall);
    }

    for (const auto& [key, tally] : tallies) {
        StageRow row;
        row.stage       = key.first;
        row.scope       = key.second;
        row.first_start = tally.first_start;
        row.unfinished  = tally.unfinished;
        if (!tally.ran) {
            row.state = tally.unfinished ? StageState::Unfinished : StageState::NotRun;
            summary.rows.push_back(std::move(row));
            continue;
        }
        row.state = tally.instant ? StageState::Instant : StageState::Ran;
        Clock::duration     total {};
        std::vector<double> times;
        for (const std::optional<Clock::duration>& length : tally.per_iteration) {
            if (!length)
                continue;
            total += *length;
            times.push_back(Millis(*length).count());
        }
        const Spread spread = spread_of(times);
        row.mean            = Millis(spread.mean);
        row.min             = Millis(spread.min);
        row.cv              = spread.cv;
        if (summary.summed_work.count() > 0)
            row.share = Millis(total) / double(count) / summary.summed_work;
        if (tally.windows > 0) {
            if (below_floor(header, tally.windows, tally.cpu_ns))
                row.cpu_below_floor = true;
            else if (tally.window_ns > 0)
                row.cpu = tally.cpu_ns / tally.window_ns;
        }
        row.peak_rss_bytes = lowest(tally.peak_per_iteration);
        row.shared         = tally.shared;
        summary.rows.push_back(std::move(row));
    }
    return summary;
}

bool folds(const StageRow& row, double below) { return !row.unfinished && row.share < below; }

std::optional<OtherRow> collapse(std::vector<StageRow>& rows, double below)
{
    const auto folded = [below](const StageRow& row) { return folds(row, below); };
    OtherRow   other;
    for (const StageRow& row : rows) {
        if (!folded(row))
            continue;
        ++other.stages;
        if (row.state == StageState::NotRun)
            ++other.not_run;
        other.mean += row.mean;
        other.share += row.share;
    }
    rows.erase(std::remove_if(rows.begin(), rows.end(), folded), rows.end());
    if (other.stages == 0)
        return std::nullopt;
    return other;
}

}} // namespace Slic3r::Bench
