#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/partitioner.h>
#include <tbb/task_arena.h>

namespace Slic3r {

// Calls process(i) for every i in [0, count) on the TBB threads, items_per_task consecutive items in each task, and
// throws what calling them in order would: the exception of the lowest i that threw. Items after it that have not
// started yet are skipped, as a loop stops at its first error.
//
// Each task runs isolated: while it waits in the parallel loops of its own items, its thread takes no other task of
// this loop. Otherwise it could start another item meanwhile, which holds up the waiting item until that one is
// done, keeps more items in memory at once than there are threads, and breaks code that keeps a lock or thread-local
// scratch across a parallel loop.
template<class Process>
void parallel_for_first_error(size_t count, size_t items_per_task, Process &&process)
{
    std::vector<std::exception_ptr> errors(count);
    std::atomic<size_t>             first_error { count };
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count, items_per_task), [&](const tbb::blocked_range<size_t> &range) {
        tbb::this_task_arena::isolate([&]() {
            for (size_t i = range.begin(); i < range.end() && i < first_error.load(std::memory_order_relaxed); ++ i)
                try {
                    process(i);
                } catch (...) {
                    errors[i] = std::current_exception();
                    size_t lowest = first_error.load(std::memory_order_relaxed);
                    while (i < lowest && ! first_error.compare_exchange_weak(lowest, i, std::memory_order_relaxed))
                        ;
                }
        });
    }, tbb::simple_partitioner());
    for (const std::exception_ptr &error : errors)
        if (error)
            std::rethrow_exception(error);
}

} // namespace Slic3r
