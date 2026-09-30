#include <catch2/catch_all.hpp>

#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>

#include "libslic3r/ParallelResolve.hpp"

using namespace Slic3r;

namespace {

thread_local int t_live_setups = 0;

// Counts how many are alive on the constructing thread.
struct CountingSetup
{
    CountingSetup() { ++ t_live_setups; }
    ~CountingSetup() { -- t_live_setups; }
};

std::vector<size_t> first_indices(size_t count)
{
    std::vector<size_t> indices(count);
    std::iota(indices.begin(), indices.end(), size_t(0));
    return indices;
}

} // namespace

TEST_CASE("every item resolves once and commits in index order on the calling thread", "[ParallelResolve]")
{
    const size_t        count = 200;
    std::vector<int>    resolves(count, 0);
    std::vector<size_t> committed, values;
    std::vector<bool>   on_caller;
    const std::thread::id caller = std::this_thread::get_id();
    resolve_then_commit(count,
        [&](size_t i) { ++ resolves[i]; return i * 3; },
        [&](size_t i, size_t resolved) {
            committed.push_back(i);
            values.push_back(resolved);
            on_caller.push_back(std::this_thread::get_id() == caller);
        });

    CHECK(committed == first_indices(count));
    for (size_t i = 0; i < count; ++ i) {
        CHECK(resolves[i] == 1);
        CHECK(values[i] == i * 3);
        CHECK(on_caller[i]);
    }
}

TEST_CASE("every item resolves inside one chunk setup", "[ParallelResolve]")
{
    const size_t     count = 200;
    std::vector<int> live(count, 0);
    resolve_then_commit<CountingSetup>(count,
        [&](size_t i) { live[i] = t_live_setups; return 0; },
        [](size_t, int) {});

    for (size_t i = 0; i < count; ++ i)
        CHECK(live[i] == 1);
}

TEST_CASE("an exception from resolve leaves the batches before it committed", "[ParallelResolve]")
{
    const size_t        count = 200, fails_at = 150;
    REQUIRE(fails_at >= resolve_batch_size);
    std::vector<size_t> committed;
    CHECK_THROWS_AS(resolve_then_commit(count,
                        [&](size_t i) {
                            if (i == fails_at)
                                throw std::runtime_error("resolve failed");
                            return i;
                        },
                        [&](size_t i, size_t) { committed.push_back(i); }),
                    std::runtime_error);

    CHECK(committed == first_indices(fails_at / resolve_batch_size * resolve_batch_size));
}

TEST_CASE("an exception from commit stops at the item that threw", "[ParallelResolve]")
{
    const size_t        count = 200, fails_at = 90;
    std::vector<size_t> committed;
    CHECK_THROWS_AS(resolve_then_commit(count,
                        [](size_t i) { return i; },
                        [&](size_t i, size_t) {
                            if (i == fails_at)
                                throw std::runtime_error("commit failed");
                            committed.push_back(i);
                        }),
                    std::runtime_error);

    CHECK(committed == first_indices(fails_at));
}

TEST_CASE("a canceled task group stops before committing an item it did not resolve", "[ParallelResolve]")
{
    std::vector<size_t>     committed;
    bool                    threw = false;
    tbb::task_group_context context;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, 1), [&](const tbb::blocked_range<size_t>&) {
        context.cancel_group_execution();
        try {
            resolve_then_commit(200,
                [](size_t i) { return i + 1; },
                [&](size_t, size_t resolved) { committed.push_back(resolved); });
        } catch (const std::runtime_error&) {
            threw = true;
        }
    }, context);

    CHECK(threw);
    CHECK(committed.empty());
}
