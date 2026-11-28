#pragma once

#include "core/Result.hpp"
#include "core/Summary.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r { namespace Bench {

// Thrown for two results that cannot be compared.
class CompareError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct CompareOptions
{
    // Compares results whose measurement identities differ, listing each difference.
    bool allow_mismatch = false;
    // Pairs a stage per scope, as the console's verbose rows are.
    bool verbose = false;
};

// A key whose value differs between two maps, absent on the side that lacks it.
struct PropertyDifference
{
    std::string                key;
    std::optional<std::string> a;
    std::optional<std::string> b;
};

// Worse and Better clear the significance bar and both runs' CV, Noise clears the bar inside a CV, and
// Unjudged has a run without a CV to judge by.
enum class Verdict { Unchanged, Worse, Better, Noise, Unjudged };

// Judges a change, b's minimum over a's less one, against each run's CV.
Verdict judge(double change, std::optional<double> cv_a, std::optional<double> cv_b);

// One figure in both runs, where the change is of the minimum.
struct Change
{
    Spread  a;
    Spread  b;
    double  change      = 0.0;
    double  mean_change = 0.0;
    Verdict verdict     = Verdict::Unchanged;
};

// One stage in both runs, or one stage in one scope under verbose.
struct StagePair
{
    std::string stage;
    std::string scope;
    // Absent in the run that never mentions the stage.
    std::optional<StageRow> a;
    std::optional<StageRow> b;
    // Present when the stage ran measurably in both.
    std::optional<Change> time;
    // A significant change of time, a change of state, or a stage only one run has, none of which folds.
    bool significant = false;
};

// What the pairs that fold add up to in each run.
struct OtherPair
{
    std::size_t stages  = 0;
    std::size_t not_run = 0;
    Millis      a {};
    Millis      b {};
};

// A work stat or per-role metric that differs, absent in the run that lacks it.
struct WorkDifference
{
    std::string           name;
    std::optional<double> a;
    std::optional<double> b;
};

struct WorkloadComparison
{
    std::string name;
    // Why the workload was not compared, and empty when it was.
    std::string                  not_compared;
    std::optional<std::uint64_t> hash_a;
    std::optional<std::uint64_t> hash_b;
    std::vector<WorkDifference>  work_differences;
    // Both runs reported work stats, so a hash that changed with no work difference changed values alone.
    bool work_in_both = false;
    // The hashes or the work differ, so the times measure different work.
    bool                   output_changed = false;
    std::vector<StagePair> stages;
    std::optional<Change>  summed_work;
    std::optional<Change>  unaccounted;
    std::optional<Change>  wall;
    std::optional<Change>  peak_rss_bytes;
    std::optional<double>  cpu_a;
    std::optional<double>  cpu_b;
    bool                   cpu_below_floor_a = false;
    bool                   cpu_below_floor_b = false;
    // The wall per million moves, per layer and per cm3 of extrusion, where both runs have the stat.
    std::optional<Change> per_million_moves;
    std::optional<Change> per_layer;
    std::optional<Change> per_cm3;
};

struct Comparison
{
    // Each run's identities without its workloads.
    Result a;
    Result b;
    // Empty unless the options allowed a mismatch.
    std::vector<PropertyDifference> measurement;
    // The stages are paired per scope, as CompareOptions::verbose asks.
    bool verbose = false;
    // In a's order, then those only b has.
    std::vector<WorkloadComparison> workloads;
    // How many of the workloads compared changed their output.
    std::size_t changed_outputs = 0;
    // How many workloads failed in b and not in a.
    std::size_t failed_only_in_b = 0;
    // Taken over the workloads compared with unchanged output, a timed stage and a wall above zero in
    // b, which geometric_mean_of counts.
    std::optional<double> wall_geometric_mean;
    std::size_t           geometric_mean_of = 0;
};

// Pairs the workloads by name and their stages by stage, and throws CompareError naming each
// differing key when the measurement identities differ and the options do not allow it.
Comparison compare(const Result& a, const Result& b, const CompareOptions& options);

// Every key whose value differs between the maps.
std::vector<PropertyDifference> property_differences(const Properties& a, const Properties& b);

// Moves each pair whose rows both fold and whose change is not significant into the returned other
// pair, and returns nothing when none does.
std::optional<OtherPair> collapse(std::vector<StagePair>& pairs, double below);

}} // namespace Slic3r::Bench
