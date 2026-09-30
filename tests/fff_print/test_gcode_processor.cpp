#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using namespace Slic3r;

TEST_CASE("G-code processor tag dialect is instance-local", "[GCodeProcessor][VendorDialect]")
{
    GCodeProcessor bbl_processor(true);
    GCodeProcessor compatible_processor(false);

    CHECK(bbl_processor.reserved_tag(GCodeProcessor::ETags::Role) == " FEATURE: ");
    CHECK(bbl_processor.reserved_tag(GCodeProcessor::ETags::Height) == " LAYER_HEIGHT: ");
    CHECK(bbl_processor.reserved_tag(GCodeProcessor::ETags::Width) == " LINE_WIDTH: ");
    CHECK(compatible_processor.reserved_tag(GCodeProcessor::ETags::Role) == "TYPE:");
    CHECK(compatible_processor.reserved_tag(GCodeProcessor::ETags::Height) == "HEIGHT:");
    CHECK(compatible_processor.reserved_tag(GCodeProcessor::ETags::Width) == "WIDTH:");

    compatible_processor.set_is_bbl_printer(true);
    CHECK(compatible_processor.reserved_tag(GCodeProcessor::ETags::Role) == " FEATURE: ");
    CHECK(bbl_processor.reserved_tag(GCodeProcessor::ETags::Role) == " FEATURE: ");

    compatible_processor.set_is_bbl_printer(false);
    CHECK(compatible_processor.reserved_tag(GCodeProcessor::ETags::Role) == "TYPE:");
    CHECK(bbl_processor.reserved_tag(GCodeProcessor::ETags::Role) == " FEATURE: ");
}

TEST_CASE("G-code processor result ids remain monotonic across concurrent resets", "[GCodeProcessor][Concurrency]")
{
    constexpr size_t thread_count = 8;
    constexpr size_t iterations   = 512;

    std::atomic<size_t> ready{0};
    std::atomic<bool>   start{false};
    std::vector<std::vector<unsigned int>> thread_result_ids(
        thread_count, std::vector<unsigned int>(iterations));
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
        threads.emplace_back([&, thread_id] {
            GCodeProcessor processor;
            ready.fetch_add(1, std::memory_order_release);
            while (! start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t iteration = 0; iteration < iterations; ++iteration) {
                processor.reset();
                thread_result_ids[thread_id][iteration] = processor.get_result().id;
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);

    for (std::thread &thread : threads)
        thread.join();

    std::vector<unsigned int> result_ids;
    result_ids.reserve(thread_count * iterations);
    for (const auto &values : thread_result_ids)
        result_ids.insert(result_ids.end(), values.begin(), values.end());
    std::sort(result_ids.begin(), result_ids.end());

    REQUIRE(result_ids.front() > 0);
    for (size_t i = 1; i < result_ids.size(); ++i)
        REQUIRE(result_ids[i] == result_ids[i - 1] + 1);
}
