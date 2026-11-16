#include "libslic3r/Gpu/GpuExactGeometry.hpp"
#include <cstddef>
#include <utility>
#if __has_include(<catch2/catch_all.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "libslic3r/Gpu/GpuBatchSafety.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Slic3r::Gpu;

namespace {

// Literal rational values, independent of the production reference function.
std::vector<VerticalIntersectionRequest> requests_with_ids(const std::array<uint64_t, 3>& ids)
{
    return {
        {{{0, -100}, {1000, 900}}, 500, ids[0]},
        {{{1000, 900}, {0, -100}}, 250, ids[1]},
        {{{-5, -7}, {7, 11}}, -1, ids[2]}
    };
}

VerticalIntersectionBatch literal_results(const std::array<uint64_t, 3>& ids)
{
    VerticalIntersectionBatch batch;
    batch.dispatched = true;
    batch.intersections = {{400000, 1000, ids[0], true},
                           {150000, 1000, ids[1], true},
                           {-12, 12, ids[2], true}};
    return batch;
}

} // namespace

TEST_CASE("Exact batches accept increasing and arbitrary unique stable IDs", "[GpuBatchPreflight]")
{
    const uint64_t last = std::numeric_limits<uint64_t>::max();
    const auto ids = GENERATE_COPY(std::array<uint64_t, 3>{0, 1, 2},
                                  std::array<uint64_t, 3>{0, 9, last},
                                  std::array<uint64_t, 3>{last, 0, 9},
                                  std::array<uint64_t, 3>{9, last, 0});
    const auto requests = requests_with_ids(ids);
    REQUIRE(preflight_vertical_batch(requests));
    REQUIRE(validate_vertical_batch(requests, literal_results(ids)));
    BackendSafetyState state;
    const auto accepted = dispatch_checked_vertical_batch(requests, state, true,
        [&] { return literal_results(ids); });
    REQUIRE(accepted.dispatched);
    REQUIRE_FALSE(state.quarantined());
    REQUIRE(accepted.intersections.size() == 3);
    REQUIRE(accepted.intersections[0].numerator == 400000);
    REQUIRE(accepted.intersections[1].numerator == 150000);
    REQUIRE(accepted.intersections[2].numerator == -12);
    for (size_t i = 0; i < ids.size(); ++i)
        REQUIRE(accepted.intersections[i].stable_id == ids[i]);
}

TEST_CASE("Repeated stable IDs reject the complete batch before dispatch", "[GpuBatchPreflight]")
{
    const uint64_t last = std::numeric_limits<uint64_t>::max();
    const auto ids = GENERATE_COPY(std::array<uint64_t, 3>{0, 1, 1},
                                  std::array<uint64_t, 3>{0, 9, 0},
                                  std::array<uint64_t, 3>{last, 0, last});
    const auto requests = requests_with_ids(ids);
    REQUIRE_FALSE(preflight_vertical_batch(requests));
    BackendSafetyState state;
    bool dispatched = false;
    const auto rejected = dispatch_checked_vertical_batch(requests, state, true, [&] {
        dispatched = true;
        return literal_results(ids);
    });
    REQUIRE_FALSE(dispatched);
    REQUIRE_FALSE(rejected.dispatched);
    REQUIRE(rejected.intersections.empty());
    REQUIRE_FALSE(state.quarantined());
}

TEST_CASE("Exact batch validation rejects every changed returned field", "[GpuBatchPreflight]")
{
    const auto damage = GENERATE(0, 1, 2, 3, 4, 5);
    const std::array<uint64_t, 3> ids {9, std::numeric_limits<uint64_t>::max(), 0};
    const auto requests = requests_with_ids(ids);
    auto returned = literal_results(ids);
    switch (damage) {
    case 0: returned.intersections[2].numerator += 1; break;
    case 1: returned.intersections[2].denominator += 1; break;
    case 2: returned.intersections[2].stable_id = 1; break;
    case 3: returned.intersections[2].valid = false; break;
    case 4: std::swap(returned.intersections[0], returned.intersections[2]); break;
    case 5: returned.intersections.pop_back(); break;
    }
    REQUIRE_FALSE(validate_vertical_batch(requests, returned));
    BackendSafetyState state;
    const auto rejected = dispatch_checked_vertical_batch(requests, state, true,
        [&] { return returned; });
    REQUIRE_FALSE(rejected.dispatched);
    REQUIRE(rejected.intersections.empty());
    REQUIRE(state.quarantined());
}

TEST_CASE("An unsafe request after a valid prefix prevents the entire dispatch", "[GpuBatchPreflight]")
{
    auto requests = requests_with_ids({0, 1, 2});
    requests.back() = {{{0, 3000000000}, {4000000000, 3000000000}}, 2000000000, 2};
    REQUIRE_FALSE(preflight_vertical_batch(requests));
    BackendSafetyState state;
    bool dispatched = false;
    const auto batch = dispatch_checked_vertical_batch(requests, state, true, [&] {
        dispatched = true;
        return literal_results({0, 1, 2});
    });
    REQUIRE_FALSE(dispatched);
    REQUIRE_FALSE(batch.dispatched);
    REQUIRE(batch.intersections.empty());
    REQUIRE_FALSE(state.quarantined());
}

TEST_CASE("Tree batches preserve arbitrary unique IDs and reject repeated IDs", "[GpuBatchPreflight]")
{
    const auto duplicate = GENERATE(false, true);
    const uint64_t last = std::numeric_limits<uint64_t>::max();
    const std::vector<TreeContourRequest> requests {
        {{{10, 10}, {20, 10}}, last}, {{{10, 20}, {20, 20}}, 0},
        {{{10, 30}, {20, 30}}, duplicate ? last : 9}
    };
    const std::vector<Segment> boxes {{{0, 0}, {100, 100}}};
    BackendSafetyState state;
    bool dispatched = false;
    const auto batch = dispatch_checked_tree_batch(requests, boxes, state, true, [&] {
        dispatched = true;
        TreeContourBatch result;
        result.dispatched = true;
        result.may_intersect = {1, 1, 1};
        return result;
    });
    REQUIRE(dispatched == !duplicate);
    REQUIRE(batch.dispatched == !duplicate);
    REQUIRE_FALSE(state.quarantined());
    REQUIRE(batch.may_intersect.size() == (duplicate ? 0 : 3));
}
