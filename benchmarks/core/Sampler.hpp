#pragma once

#include "core/Result.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

// One look at the process, with when it was taken, its resident memory and the CPU time it has used.
struct Reading
{
    Clock::time_point at;
    std::uint64_t     rss_bytes = 0;
    std::uint64_t     cpu_ns    = 0;
};

// Takes a reading, and may be called from the sampler's thread and the Runner's at once.
using ProcessProbe = std::function<Reading()>;

// Reads this process through Host.
Reading host_reading();

// Keys the sampler writes into each span's metrics.
namespace SampledMetric {
// The highest resident memory read while the span ran.
inline constexpr const char* peak_rss_bytes = "peak_rss_bytes";
// The CPU time the process used between the first and last readings inside the span, and the
// time between those two readings.
inline constexpr const char* cpu_ns        = "cpu_ns";
inline constexpr const char* cpu_window_ns = "cpu_window_ns";
// Every key above, which Measurement refuses from a workload. A new key goes here too.
inline constexpr const char* all[] = {peak_rss_bytes, cpu_ns, cpu_window_ns};
} // namespace SampledMetric

// How often the sampler reads the process, which Windows rounds up to its clock tick.
inline constexpr std::chrono::milliseconds sampling_interval {5};

// Reads the process every sampling_interval on its own thread from start() until stop().
class Sampler
{
public:
    explicit Sampler(ProcessProbe probe) : m_probe(std::move(probe)) {}
    ~Sampler();

    Sampler(const Sampler&)            = delete;
    Sampler& operator=(const Sampler&) = delete;

    void start();

    // The readings in the order they were taken, rethrowing anything the probe threw.
    std::vector<Reading> stop();

private:
    void join();

    ProcessProbe            m_probe;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_wake;
    bool                    m_stopping = false;
    std::vector<Reading>    m_readings;
    std::exception_ptr      m_error;
};

// The highest memory among the readings taken from `from` to `to`, or nothing when none was, where
// the readings are in time order.
std::optional<std::uint64_t> peak_rss(const std::vector<Reading>& readings, Clock::time_point from, Clock::time_point to);

// A span's sampled metrics from readings in time order, with its highest memory when memory is
// collected and one reading falls inside it, and its CPU when two do and the CPU time did not go
// backwards.
Metrics sampled_metrics(const std::vector<Reading>& readings, Clock::time_point from, Clock::time_point to, bool memory);

}} // namespace Slic3r::Bench
