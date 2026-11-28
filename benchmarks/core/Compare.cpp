#include "core/Compare.hpp"

#include "core/Result.hpp"
#include "core/Summary.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

// The change from one run's spread to the other's, or nothing when the first run's minimum is not
// above zero.
std::optional<Change> change_between(const Spread& a, const Spread& b)
{
    if (a.min <= 0)
        return std::nullopt;
    Change change;
    change.a           = a;
    change.b           = b;
    change.change      = b.min / a.min - 1;
    change.mean_change = a.mean > 0 ? b.mean / a.mean - 1 : 0.0;
    change.verdict     = judge(change.change, a.cv, b.cv);
    return change;
}

std::optional<Change> figure_change(const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.empty() || b.empty())
        return std::nullopt;
    return change_between(spread_of(a), spread_of(b));
}

Spread spread_in(const StageRow& row) { return {row.min.count(), row.mean.count(), row.cv}; }

// A run's figures over its iterations, with its peaks only when every iteration has one.
struct Totals
{
    std::vector<double> wall;
    std::vector<double> summed;
    std::vector<double> unaccounted;
    std::vector<double> peak;
};

Totals totals_of(const WorkloadResult& workload)
{
    Totals totals;
    bool   every_peak = true;
    for (const IterationResult& iteration : workload.iterations) {
        Clock::duration summed {};
        for (const StageSpan& span : iteration.timeline)
            summed += span.done_at - span.started_at;
        totals.wall.push_back(Millis(iteration.wall).count());
        totals.summed.push_back(Millis(summed).count());
        totals.unaccounted.push_back(Millis(iteration.wall - covered(iteration.timeline)).count());
        if (iteration.peak_rss_bytes)
            totals.peak.push_back(double(*iteration.peak_rss_bytes));
        else
            every_peak = false;
    }
    if (!every_peak)
        totals.peak.clear();
    return totals;
}

// The wall per unit of work, whose CV is the wall's.
std::optional<Change> per_unit(const std::optional<Change>& wall, double unit_a, double unit_b)
{
    if (!wall || unit_a <= 0 || unit_b <= 0)
        return std::nullopt;
    const auto scaled = [](Spread spread, double unit) {
        spread.min /= unit;
        spread.mean /= unit;
        return spread;
    };
    return change_between(scaled(wall->a, unit_a), scaled(wall->b, unit_b));
}

// Every work stat and per-role metric whose value differs, typed stats first.
std::vector<WorkDifference> work_differences(const std::optional<WorkStats>& a, const std::optional<WorkStats>& b)
{
    using Named = std::vector<std::pair<std::string, double>>;
    const auto named = [](const std::optional<WorkStats>& work) {
        Named values;
        if (!work)
            return values;
        // Names every field, so one added to WorkStats does not compile here until it is compared.
        const auto& [moves, layers, gcode_bytes, travel_moves, extrusion_mm3, metrics] = *work;
        values = {{"moves", double(moves)},
                  {"layers", double(layers)},
                  {"gcode_bytes", double(gcode_bytes)},
                  {"travel_moves", double(travel_moves)},
                  {"extrusion_mm3", extrusion_mm3}};
        values.insert(values.end(), metrics.begin(), metrics.end());
        return values;
    };
    const auto value_in = [](const Named& values, const std::string& name) -> std::optional<double> {
        const auto found = std::find_if(values.begin(), values.end(), [&name](const auto& value) { return value.first == name; });
        return found == values.end() ? std::nullopt : std::optional<double>(found->second);
    };
    const Named              in_a = named(a);
    const Named              in_b = named(b);
    std::vector<std::string> names;
    for (const Named* side : {&in_a, &in_b})
        for (const auto& value : *side)
            if (std::find(names.begin(), names.end(), value.first) == names.end())
                names.push_back(value.first);
    std::vector<WorkDifference> differences;
    for (const std::string& name : names) {
        const std::optional<double> x = value_in(in_a, name);
        const std::optional<double> y = value_in(in_b, name);
        if (x != y)
            differences.push_back({name, x, y});
    }
    return differences;
}

std::vector<StagePair> pair_stages(const std::vector<StageRow>& a, const std::vector<StageRow>& b)
{
    std::map<std::pair<std::string, std::string>, StagePair> pairs;
    for (const StageRow& row : a) {
        StagePair& pair = pairs[{row.stage, row.scope}];
        pair.stage      = row.stage;
        pair.scope      = row.scope;
        pair.a          = row;
    }
    for (const StageRow& row : b) {
        StagePair& pair = pairs[{row.stage, row.scope}];
        pair.stage      = row.stage;
        pair.scope      = row.scope;
        pair.b          = row;
    }
    const auto settled = [](const StageRow& row) {
        return row.state == StageState::Ran && row.mean < resolvable_time ? StageState::Instant : row.state;
    };
    std::vector<StagePair> paired;
    for (auto& entry : pairs) {
        StagePair& pair  = entry.second;
        pair.significant = !pair.a || !pair.b || settled(*pair.a) != settled(*pair.b) || pair.a->unfinished != pair.b->unfinished;
        if (pair.a && pair.b && pair.a->state == StageState::Ran && pair.b->state == StageState::Ran) {
            pair.time = change_between(spread_in(*pair.a), spread_in(*pair.b));
            if (pair.time && Millis(std::abs(pair.time->b.min - pair.time->a.min)) < resolvable_time)
                pair.time->verdict = Verdict::Unchanged;
            if (pair.time && (pair.time->verdict == Verdict::Worse || pair.time->verdict == Verdict::Better))
                pair.significant = true;
        }
        paired.push_back(std::move(pair));
    }
    return paired;
}

const WorkloadResult* find_workload(const Result& result, const std::string& name)
{
    const auto found = std::find_if(result.workloads.begin(), result.workloads.end(),
                                    [&name](const WorkloadResult& workload) { return workload.name == name; });
    return found == result.workloads.end() ? nullptr : &*found;
}

// Why a workload both runs list was not compared, or empty when both ran it.
std::string not_run_reason(const WorkloadResult& a, const WorkloadResult& b)
{
    const auto outcome = [](const WorkloadResult& workload) { return workload.outcome == Outcome::Skipped ? "skipped in " : "failed in "; };
    if (a.outcome != Outcome::Ran && a.outcome == b.outcome && a.reason == b.reason)
        return outcome(a) + ("both: " + a.reason);
    std::string reason;
    const auto  add = [&reason, &outcome](const WorkloadResult& workload, const char* side) {
        if (workload.outcome == Outcome::Ran)
            return;
        if (!reason.empty())
            reason += "; ";
        reason += outcome(workload) + (side + (": " + workload.reason));
    };
    add(a, "a");
    add(b, "b");
    return reason;
}

WorkloadComparison compare_workload(const std::string& name, const Result& a, const Result& b, const CompareOptions& options)
{
    WorkloadComparison compared;
    compared.name              = name;
    const WorkloadResult* in_a = find_workload(a, name);
    const WorkloadResult* in_b = find_workload(b, name);
    if (!in_a || !in_b) {
        compared.not_compared = in_a ? "only in a" : "only in b";
        return compared;
    }
    compared.not_compared = not_run_reason(*in_a, *in_b);
    if (!compared.not_compared.empty())
        return compared;

    // A hash or work stats that only one run has were not collected in the other, so they say nothing about the output.
    compared.hash_a       = in_a->output_hash;
    compared.hash_b       = in_b->output_hash;
    compared.work_in_both = in_a->work && in_b->work;
    if (compared.work_in_both)
        compared.work_differences = work_differences(in_a->work, in_b->work);
    compared.output_changed = (compared.hash_a && compared.hash_b && *compared.hash_a != *compared.hash_b) ||
                              !compared.work_differences.empty();

    const WorkloadSummary summary_a = summarize(*in_a, a, options.verbose);
    const WorkloadSummary summary_b = summarize(*in_b, b, options.verbose);
    compared.stages                 = pair_stages(summary_a.rows, summary_b.rows);
    compared.cpu_a                  = summary_a.cpu;
    compared.cpu_b                  = summary_b.cpu;
    compared.cpu_below_floor_a      = summary_a.cpu_below_floor;
    compared.cpu_below_floor_b      = summary_b.cpu_below_floor;

    const Totals totals_a   = totals_of(*in_a);
    const Totals totals_b   = totals_of(*in_b);
    compared.summed_work    = figure_change(totals_a.summed, totals_b.summed);
    compared.unaccounted    = figure_change(totals_a.unaccounted, totals_b.unaccounted);
    compared.wall           = figure_change(totals_a.wall, totals_b.wall);
    compared.peak_rss_bytes = figure_change(totals_a.peak, totals_b.peak);
    if (in_a->work && in_b->work) {
        const WorkStats& work_a    = *in_a->work;
        const WorkStats& work_b    = *in_b->work;
        compared.per_million_moves = per_unit(compared.wall, double(work_a.moves) / 1e6, double(work_b.moves) / 1e6);
        compared.per_layer         = per_unit(compared.wall, double(work_a.layers), double(work_b.layers));
        compared.per_cm3           = per_unit(compared.wall, work_a.extrusion_mm3 / 1000, work_b.extrusion_mm3 / 1000);
    }
    return compared;
}

} // namespace

Verdict judge(double change, std::optional<double> cv_a, std::optional<double> cv_b)
{
    if (!cv_a || !cv_b)
        return Verdict::Unjudged;
    const double size = std::abs(change);
    if (size < significance_bar)
        return Verdict::Unchanged;
    if (size <= *cv_a || size <= *cv_b)
        return Verdict::Noise;
    return change > 0 ? Verdict::Worse : Verdict::Better;
}

Comparison compare(const Result& a, const Result& b, const CompareOptions& options)
{
    Comparison comparison;
    comparison.measurement = property_differences(a.measurement, b.measurement);
    if (!comparison.measurement.empty() && !options.allow_mismatch) {
        std::string listed;
        for (const PropertyDifference& difference : comparison.measurement)
            listed += "\n  " + difference.key + "  " + difference.a.value_or("-") + " -> " + difference.b.value_or("-");
        throw CompareError("the runs measured differently, so they are not compared:" + listed);
    }
    comparison.a       = Result {a.suite, a.measurement, a.build, a.machine, {}};
    comparison.b       = Result {b.suite, b.measurement, b.build, b.machine, {}};
    comparison.verbose = options.verbose;

    std::vector<std::string> names;
    for (const WorkloadResult& workload : a.workloads)
        names.push_back(workload.name);
    for (const WorkloadResult& workload : b.workloads)
        if (!find_workload(a, workload.name))
            names.push_back(workload.name);
    double log_sum = 0.0;
    for (const std::string& name : names) {
        comparison.workloads.push_back(compare_workload(name, a, b, options));
        const WorkloadComparison& workload = comparison.workloads.back();
        if (workload.output_changed)
            ++comparison.changed_outputs;
        const WorkloadResult* in_a = find_workload(a, name);
        const WorkloadResult* in_b = find_workload(b, name);
        if (in_a && in_b && in_a->outcome != Outcome::Failed && in_b->outcome == Outcome::Failed)
            ++comparison.failed_only_in_b;
        // A workload that timed no stage has a wall of overhead alone, and a wall of zero in b has no
        // logarithm, which would take the mean to -100%.
        if (workload.not_compared.empty() && !workload.output_changed && workload.summed_work && workload.summed_work->b.min > 0 &&
            workload.wall && workload.wall->b.min > 0) {
            log_sum += std::log1p(workload.wall->change);
            ++comparison.geometric_mean_of;
        }
    }
    if (comparison.geometric_mean_of > 0)
        comparison.wall_geometric_mean = std::expm1(log_sum / double(comparison.geometric_mean_of));
    return comparison;
}

std::vector<PropertyDifference> property_differences(const Properties& a, const Properties& b)
{
    std::vector<PropertyDifference> differences;
    auto                            x = a.begin();
    auto                            y = b.begin();
    while (x != a.end() || y != b.end()) {
        if (y == b.end() || (x != a.end() && x->first < y->first)) {
            differences.push_back({x->first, x->second, std::nullopt});
            ++x;
        } else if (x == a.end() || y->first < x->first) {
            differences.push_back({y->first, std::nullopt, y->second});
            ++y;
        } else {
            if (x->second != y->second)
                differences.push_back({x->first, x->second, y->second});
            ++x;
            ++y;
        }
    }
    return differences;
}

std::optional<OtherPair> collapse(std::vector<StagePair>& pairs, double below)
{
    const auto folded = [below](const StagePair& pair) {
        return !pair.significant && pair.a && pair.b && folds(*pair.a, below) && folds(*pair.b, below);
    };
    OtherPair other;
    for (const StagePair& pair : pairs) {
        if (!folded(pair))
            continue;
        ++other.stages;
        if (pair.a->state == StageState::NotRun)
            ++other.not_run;
        other.a += pair.a->min;
        other.b += pair.b->min;
    }
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), folded), pairs.end());
    if (other.stages == 0)
        return std::nullopt;
    return other;
}

}} // namespace Slic3r::Bench
