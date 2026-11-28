#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace Bench {

using Clock = std::chrono::steady_clock;

// Bump the minor for an added field, which older readers ignore, and the major for a changed or
// removed one, which they refuse.
inline constexpr unsigned schema_major = 1;
inline constexpr unsigned schema_minor = 0;

// Measurements a producer may omit, keyed by constants declared beside the producer, where a
// missing key means not collected.
using Metrics = std::map<std::string, double>;

// Labels, compared for equality and shown but never aggregated, keyed like Metrics.
using Properties = std::map<std::string, std::string>;

// One step of one iteration, where scope is "print" for a print-wide step and "object:N"
// otherwise.
struct StageSpan
{
    std::string       stage;
    std::string       scope;
    Clock::time_point started_at {};
    Clock::time_point done_at    {};
    // Measurements taken during this step, such as its peak resident memory.
    Metrics           metrics;
};

using Timeline = std::vector<StageSpan>;

// A step that started and never finished, such as one whose set_done() is never called.
struct UnfinishedStage
{
    std::string       stage;
    std::string       scope;
    Clock::time_point started_at {};
};

// A step of the timed stages that never started.
struct StageNotRun
{
    std::string stage;
    std::string scope;
};

// What a run asked for, where two results are comparable only when these are equal.
using MeasurementIdentity = Properties;

// Keys for the settings every run records, with a list value such as stages written in a fixed
// order so equal settings compare equal.
namespace MeasurementKey {
inline constexpr const char* policy     = "policy";
inline constexpr const char* warmup     = "warmup";
inline constexpr const char* iterations = "iterations";
inline constexpr const char* threads    = "threads";
inline constexpr const char* stages     = "stages";
inline constexpr const char* metrics    = "metrics";
inline constexpr const char* corpus     = "corpus";
inline constexpr const char* affinity   = "affinity";
inline constexpr const char* sampling   = "sampling";
} // namespace MeasurementKey

// What built the code, which may differ between two comparable results.
struct BuildIdentity
{
    std::string revision;
    bool        dirty = false;
    std::string compiler;
    std::string compiler_version;
    std::string config;
    std::string flags;
    // Anything else about the build worth showing beside a result, such as whether it was
    // built with a PGO profile.
    Properties  properties;
};

// Where it ran, which may differ between two comparable results.
struct MachineIdentity
{
    std::string host;
    std::string os;
    std::string cpu;
    unsigned    logical_cores = 0;
    // Anything else about the machine that affects timing, such as the power plan or the split
    // between performance and efficiency cores.
    Properties  properties;
};

// Keys for the machine properties a run records.
namespace MachineProperty {
// The step in which the machine advances a thread's CPU time, in nanoseconds, where it has one.
inline constexpr const char* cpu_time_step_ns = "cpu_time_step_ns";
} // namespace MachineProperty

// What the slicer produced, the same on every machine and thread count.
struct WorkStats
{
    std::uint64_t moves         = 0;
    std::uint64_t layers        = 0;
    std::uint64_t gcode_bytes   = 0;
    std::uint64_t travel_moves  = 0;
    double        extrusion_mm3 = 0.0;
    // Breakdowns by extrusion role, under dotted keys such as "moves.perimeter".
    Metrics       metrics;
};

struct IterationResult
{
    Clock::duration              wall {};
    // Absent when the run did not collect memory.
    std::optional<std::uint64_t> peak_rss_bytes;
    Clock::duration              cpu {};
    Timeline                     timeline;
    std::vector<UnfinishedStage> unfinished;
    std::vector<StageNotRun>     not_run;
    // Measurements of the whole iteration, which no sum over the spans gives.
    Metrics                      metrics;
};

// The earliest start among the iteration's spans and unfinished steps, or the epoch when it has
// neither.
Clock::time_point origin(const IterationResult& iteration);

// Skipped means a fixture was missing and Failed means the workload threw or the Runner refused
// what it reported, and neither counts as a pass.
enum class Outcome { Ran, Skipped, Failed };

struct WorkloadResult
{
    std::string                  name;
    Outcome                      outcome = Outcome::Ran;
    // Why the workload did not run, and empty when it ran.
    std::string                  reason;
    // Absent when the run did not collect it or the workload wrote no G-code to hash.
    std::optional<std::uint64_t> output_hash;
    // Absent when the run did not collect them or the workload reported none.
    std::optional<WorkStats>     work;
    std::vector<IterationResult> iterations;

    // Each returns a valid combination, which the public fields alone do not guarantee, and
    // skipped() and failed() throw std::invalid_argument for an empty reason.
    static WorkloadResult ran(std::string name, std::optional<std::uint64_t> output_hash, std::optional<WorkStats> work,
                              std::vector<IterationResult> iterations);
    static WorkloadResult skipped(std::string name, std::string reason);
    static WorkloadResult failed(std::string name, std::string reason);
};

struct Suite
{
    // Milliseconds, the precision the document writes, so a timestamp reads back equal.
    std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> started_at {};
    Clock::duration                                                               duration {};
};

struct Result
{
    Suite                       suite;
    MeasurementIdentity         measurement;
    BuildIdentity               build;
    MachineIdentity             machine;
    std::vector<WorkloadResult> workloads;
};

// Computed from the workloads on every call and stored nowhere, so no count can disagree with
// the list.
std::size_t count_of(const Result& result, Outcome outcome);

}} // namespace Slic3r::Bench
