#include <catch2/catch_all.hpp>

#include "libslic3r/PrintBase.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace Slic3r;

namespace {

enum TestStep {
    test_step,
    test_step_count
};

} // namespace

TEST_CASE("Print state timestamps remain monotonic across concurrent states", "[PrintState][Concurrency]")
{
    constexpr size_t thread_count = 8;
    constexpr size_t iterations   = 512;

    std::atomic<size_t> ready{0};
    std::atomic<bool>   start{false};
    std::vector<std::vector<PrintStateBase::TimeStamp>> thread_timestamps(
        thread_count, std::vector<PrintStateBase::TimeStamp>(iterations));
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
        threads.emplace_back([&, thread_id] {
            ready.fetch_add(1, std::memory_order_release);
            while (! start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t iteration = 0; iteration < iterations; ++iteration) {
                PrintState<TestStep, test_step_count> state;
                std::mutex mutex;
                state.set_started(test_step, mutex, [] {});
                thread_timestamps[thread_id][iteration] = state.state_with_timestamp(test_step, mutex).timestamp;
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);

    for (std::thread &thread : threads)
        thread.join();

    std::vector<PrintStateBase::TimeStamp> timestamps;
    timestamps.reserve(thread_count * iterations);
    for (const auto &values : thread_timestamps)
        timestamps.insert(timestamps.end(), values.begin(), values.end());
    std::sort(timestamps.begin(), timestamps.end());

    REQUIRE(timestamps.front() > 0);
    for (size_t i = 1; i < timestamps.size(); ++i)
        REQUIRE(timestamps[i] == timestamps[i - 1] + 1);
}
