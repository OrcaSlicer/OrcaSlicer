#include "core/Sampler.hpp"

#include "core/Host.hpp"
#include "core/Result.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

using Readings = std::vector<Reading>;

// The readings taken from `from` to `to`, out of readings in time order.
std::pair<Readings::const_iterator, Readings::const_iterator> between(const Readings& readings, Clock::time_point from,
                                                                     Clock::time_point to)
{
    const auto first = std::lower_bound(readings.begin(), readings.end(), from,
                                        [](const Reading& reading, Clock::time_point at) { return reading.at < at; });
    const auto last  = std::upper_bound(first, readings.end(), to,
                                        [](Clock::time_point at, const Reading& reading) { return at < reading.at; });
    return {first, last};
}

std::optional<std::uint64_t> highest(Readings::const_iterator first, Readings::const_iterator last)
{
    if (first == last)
        return std::nullopt;
    return std::max_element(first, last, [](const Reading& a, const Reading& b) { return a.rss_bytes < b.rss_bytes; })->rss_bytes;
}

} // namespace

Reading host_reading() { return {Clock::now(), current_rss_bytes(), process_cpu_ns()}; }

Sampler::~Sampler() { join(); }

void Sampler::start()
{
    m_readings.clear();
    m_error    = nullptr;
    m_stopping = false;
    m_thread   = std::thread([this] {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_wake.wait_for(lock, sampling_interval, [this] { return m_stopping; })) {
            lock.unlock();
            Reading reading;
            try {
                reading = m_probe();
            } catch (...) {
                lock.lock();
                m_error = std::current_exception();
                return;
            }
            lock.lock();
            m_readings.push_back(reading);
        }
    });
}

std::vector<Reading> Sampler::stop()
{
    join();
    if (m_error)
        std::rethrow_exception(m_error);
    return std::move(m_readings);
}

void Sampler::join()
{
    if (!m_thread.joinable())
        return;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_one();
    m_thread.join();
}

std::optional<std::uint64_t> peak_rss(const std::vector<Reading>& readings, Clock::time_point from, Clock::time_point to)
{
    const auto [first, last] = between(readings, from, to);
    return highest(first, last);
}

Metrics sampled_metrics(const std::vector<Reading>& readings, Clock::time_point from, Clock::time_point to, bool memory)
{
    Metrics metrics;
    const auto [first, last] = between(readings, from, to);
    if (const std::optional<std::uint64_t> peak = highest(first, last); memory && peak)
        metrics[SampledMetric::peak_rss_bytes] = double(*peak);
    if (last - first >= 2 && (last - 1)->cpu_ns >= first->cpu_ns) {
        const Reading& earliest               = *first;
        const Reading& latest                 = *(last - 1);
        const auto     window                 = std::chrono::duration_cast<std::chrono::nanoseconds>(latest.at - earliest.at);
        metrics[SampledMetric::cpu_ns]        = double(latest.cpu_ns - earliest.cpu_ns);
        metrics[SampledMetric::cpu_window_ns] = double(window.count());
    }
    return metrics;
}

}} // namespace Slic3r::Bench
