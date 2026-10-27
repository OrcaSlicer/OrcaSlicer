#pragma once

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>

#include "Exception.hpp"

namespace Slic3r {

// A batch of resolved presets, each a whole config, is what resolve_then_commit adds
// to peak memory, so it stays far below a vendor's preset count and above any core count.
inline constexpr size_t resolve_batch_size = 64;

// Resolve `count` items that do not depend on each other and install them one at
// a time.
//
// `resolve(i)` runs on any thread and must read only, since the items are
// resolved side by side. `commit(i, resolved)` is called for every item in index
// order, on the calling thread, and is where shared state is written.
//
// The items are worked through in batches, so what is resolved and held at once
// does not grow with `count`. An exception from either callable propagates after
// the batches before it have been committed. A cancellation of the caller's task
// group, which stops a batch partway without an exception, throws RuntimeError
// before that batch commits.
//
// `ChunkSetup`, when given, is constructed once for each piece of a batch TBB hands
// out, for per-thread state a resolve would otherwise set up per item, such as the
// C numeric locale, whose setting takes a lock the whole process shares.
template<class ChunkSetup = std::monostate, class Resolve, class Commit>
void resolve_then_commit(size_t count, Resolve resolve, Commit commit)
{
    using Resolved = std::invoke_result_t<Resolve&, size_t>;

    std::vector<Resolved> resolved(std::min(count, resolve_batch_size));
    for (size_t first = 0; first < count; first += resolve_batch_size) {
        const size_t last = std::min(first + resolve_batch_size, count);
        // Isolated, so a thread waiting on the batch runs none of the caller's other
        // tasks before finishing it.
        tbb::this_task_arena::isolate([&] {
            tbb::parallel_for(tbb::blocked_range<size_t>(first, last),
                [&](const tbb::blocked_range<size_t>& range) {
                    ChunkSetup setup;
                    (void) setup;
                    for (size_t i = range.begin(); i < range.end(); ++ i)
                        resolved[i - first] = resolve(i);
                });
        });
        if (tbb::is_current_task_group_canceling())
            throw RuntimeError("resolve_then_commit: canceled before the batch was resolved");
        for (size_t i = first; i < last; ++ i)
            commit(i, std::move(resolved[i - first]));
    }
}

} // namespace Slic3r
