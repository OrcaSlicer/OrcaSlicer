#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/ParallelForFirstError.hpp"

using namespace Slic3r;

TEST_CASE("parallel_for_first_error throws the error of the lowest item that threw, after every item below it ran", "[ParallelForFirstError]")
{
    // Items 9 and 3 throw. A loop over the items stops at item 3, so its error is the one thrown, and the items before
    // it all run. Each item is written by one task only, and read after the loop.
    const size_t items_per_task = GENERATE(size_t(1), size_t(4));
    INFO(items_per_task << " items per task");
    const size_t      count = 64;
    std::vector<char> ran(count, 0);
    std::string       thrown;
    try {
        parallel_for_first_error(count, items_per_task, [&ran](size_t i) {
            ran[i] = 1;
            if (i == 3 || i == 9)
                throw std::runtime_error("item " + std::to_string(i));
        });
    } catch (const std::runtime_error &error) {
        thrown = error.what();
    }
    CHECK(thrown == "item 3");
    for (size_t i = 0; i <= 3; ++ i)
        CHECK(ran[i] == 1);
}

TEST_CASE("parallel_for_first_error starts no item on a thread that waits in another item's parallel loop", "[ParallelForFirstError]")
{
    // Each item runs a parallel loop of its own. A thread waiting for its item's loop could otherwise take another
    // item meanwhile, which would then run nested in the first one on that thread.
    static thread_local int items_on_thread = 0;
    std::atomic<bool>       nested { false };
    parallel_for_first_error(64, 1, [&nested](size_t) {
        if (items_on_thread > 0)
            nested = true;
        ++ items_on_thread;
        tbb::parallel_for(tbb::blocked_range<size_t>(0, 256), [](const tbb::blocked_range<size_t> &range) {
            double sum = 0.;
            for (size_t i = range.begin(); i < range.end(); ++ i)
                for (int k = 1; k < 2000; ++ k)
                    sum += std::sqrt(double(i * size_t(k)));
            // Keeps the work from being optimized away.
            if (sum < 0.)
                throw std::logic_error("negative sum");
        });
        -- items_on_thread;
    });
    CHECK_FALSE(nested);
}
