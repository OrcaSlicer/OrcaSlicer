#pragma once

#include "core/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ratio>
#include <string>
#include <vector>

namespace Slic3r { namespace Bench {

// A row whose share of summed work is under this folds into the other row.
inline constexpr double collapse_share = 0.01;

// The change compare calls significant, which a stage whose CV exceeds it cannot resolve.
inline constexpr double significance_bar = 0.03;

// The most a CPU figure's worst-case rounding may be of the CPU time it shows.
inline constexpr double cpu_error_limit = 0.1;

using Millis = std::chrono::duration<double, std::milli>;

// The shortest stage time, and the smallest change of one, that compare judges, since a span shorter than
// this is two clock readings in a row, which differ only by where the clock ticked.
inline constexpr Millis resolvable_time {0.001};

// A figure over a run's iterations: its minimum, its mean, and the sample standard deviation over
// the mean, absent with fewer than two values or a zero mean.
struct Spread
{
    double                min  = 0.0;
    double                mean = 0.0;
    std::optional<double> cv;
};

Spread spread_of(const std::vector<double>& values);

// The time any span of the timeline covers, counting time that spans share once.
Clock::duration covered(const Timeline& timeline);

// Ran, ran below the clock's resolution, never ran, and started but never finished.
enum class StageState { Ran, Instant, NotRun, Unfinished };

// One stage over a workload's iterations, or one stage in one scope when verbose.
struct StageRow
{
    std::string stage;
    // Empty when the row adds up every scope.
    std::string scope;
    StageState  state = StageState::Ran;
    // The stage started without finishing at least once, which puts that time in unaccounted.
    bool unfinished = false;
    // Over the iterations with a span of the row, of its span time summed within each one.
    Millis      mean {};
    Millis      min {};
    // The sample standard deviation over the mean, absent with one such iteration or a zero mean.
    std::optional<double> cv;
    // Its span time over summed work, each summed over every iteration, so the rows' shares add up to one.
    double                share = 0.0;
    // CPU time over the window it was measured in, absent where no span had one or below the floor.
    std::optional<double>        cpu;
    bool                         cpu_below_floor = false;
    std::optional<std::uint64_t> peak_rss_bytes;
    // A span of the row overlapped one outside it, whose work its readings include.
    bool shared = false;
    // The earliest start from its iteration's origin, for pipeline order, and the latest time for a
    // stage that never started.
    Clock::duration first_start = Clock::duration::max();
};

// What the console shows of a workload that ran.
struct WorkloadSummary
{
    std::vector<StageRow> rows;
    Millis                summed_work {};
    Millis                wall {};
    // The time no span covers, which spans running side by side cannot drive negative.
    Millis                       unaccounted {};
    std::optional<std::uint64_t> peak_rss_bytes;
    std::optional<double>        cpu;
    bool                         cpu_below_floor = false;
};

// The CPU time step the machine recorded, and zero where it recorded none or one that is not a count
// of nanoseconds.
std::chrono::nanoseconds recorded_cpu_time_step(const MachineIdentity& machine);

// The rows and totals of a workload's iterations, where the header's machine decides whether CPU is
// floored and its measurement gives the thread count the floor assumes.
WorkloadSummary summarize(const WorkloadResult& workload, const Result& header, bool verbose);

// What the rows under the threshold add up to.
struct OtherRow
{
    std::size_t stages  = 0;
    std::size_t not_run = 0;
    Millis      mean {};
    double      share = 0.0;
};

// Whether a row under the threshold folds into the other row, which one marked unfinished never does.
bool folds(const StageRow& row, double below);

// Moves each row that folds into the returned other row, and returns nothing when none does.
std::optional<OtherRow> collapse(std::vector<StageRow>& rows, double below);

}} // namespace Slic3r::Bench
