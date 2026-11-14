#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Gpu/CudaDispatchBuffers.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using Slic3r::Gpu::CudaDispatchBuffers;

namespace {
struct MemoryBoundary {
    size_t allocations {0};
    size_t releases {0};
    size_t fail_allocation {0};
    size_t fail_release {0};
    size_t duplicate_releases {0};
    size_t throw_release {0};
    bool allocation_error_keeps_pointer {false};
    bool allocation_error_throws {false};
    std::array<size_t, 64> tokens {};
    std::map<void*, size_t> live;

    bool allocate(void*& pointer, size_t bytes)
    {
        ++allocations;
        pointer = nullptr;
        if (allocations == fail_allocation && !allocation_error_keeps_pointer)
            return false;
        pointer = &tokens.at(allocations);
        live.emplace(pointer, bytes);
        if (allocations == fail_allocation) {
            if (allocation_error_throws)
                throw std::runtime_error("allocation reported an exception");
            return false;
        }
        return true;
    }

    bool release(void* pointer)
    {
        ++releases;
        if (live.find(pointer) == live.end())
            ++duplicate_releases;
        if (releases == throw_release)
            throw std::runtime_error("free reported an exception");
        if (releases == fail_release)
            return false;
        live.erase(pointer);
        return true;
    }
};
}

TEST_CASE("CUDA dispatch owns both buffers while the operation runs", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    bool called = false;
    REQUIRE(buffers.dispatch(128, 64, memory, [&](void* input, void* output) {
        called = true;
        CHECK(input != nullptr);
        CHECK(output != nullptr);
        CHECK(input != output);
        CHECK(buffers.owns_storage());
        CHECK(memory.live.at(input) >= 128);
        CHECK(memory.live.at(output) >= 64);
        return true;
    }));
    CHECK(called);
    REQUIRE(buffers.release(memory));
    CHECK_FALSE(buffers.owns_storage());
    CHECK(memory.live.empty());
    CHECK(memory.allocations == 2);
    CHECK(memory.releases == 2);
}

TEST_CASE("CUDA dispatch never runs after either allocation fails", "[CudaDispatchBuffers]")
{
    for (size_t failed : {size_t(1), size_t(2)}) {
        DYNAMIC_SECTION("allocation " << failed) {
            MemoryBoundary memory;
            memory.fail_allocation = failed;
            CudaDispatchBuffers buffers(1024, 512);
            bool called = false;
            CHECK_FALSE(buffers.dispatch(128, 64, memory, [&](void*, void*) {
                called = true;
                return true;
            }));
            CHECK_FALSE(called);
            CHECK(memory.allocations == failed);
            CHECK(memory.releases == failed - 1);
            CHECK(memory.live.empty());
            CHECK(memory.duplicate_releases == 0);
        }
    }
}

TEST_CASE("CUDA dispatch cleans both buffers after an operation failure", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    CHECK_FALSE(buffers.dispatch(128, 64, memory, [](void*, void*) { return false; }));
    CHECK(memory.live.empty());
    CHECK(memory.releases == 2);
    REQUIRE(buffers.release(memory));
    CHECK(memory.releases == 2);
}

TEST_CASE("CUDA dispatch preserves exception cleanup ownership", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    CHECK_THROWS_AS(buffers.dispatch(128, 64, memory, [](void*, void*) -> bool {
        throw std::runtime_error("operation failed");
    }), std::runtime_error);
    CHECK(memory.live.empty());
    CHECK(memory.releases == 2);
    CHECK(memory.duplicate_releases == 0);
}

TEST_CASE("CUDA cleanup failure attempts all buffers and prevents reuse", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    memory.fail_release = 1;
    CudaDispatchBuffers buffers(1024, 512);
    CHECK_FALSE(buffers.dispatch(128, 64, memory, [](void*, void*) { return false; }));
    CHECK(memory.releases == 2);
    CHECK(memory.live.size() == 1);
    CHECK(buffers.cleanup_failed());
    CHECK_FALSE(buffers.dispatch(128, 64, memory, [](void*, void*) { return true; }));
    CHECK(memory.allocations == 2);
    REQUIRE(buffers.release(memory));
    CHECK(memory.releases == 2);
    CHECK(memory.duplicate_releases == 0);
    CHECK(buffers.cleanup_failed());
}

TEST_CASE("CUDA dispatch rejects empty and out of bound storage before allocation", "[CudaDispatchBuffers]")
{
    for (const auto& bytes : {std::pair<size_t, size_t>{0, 64}, {128, 0}, {1025, 64}, {128, 513}}) {
        DYNAMIC_SECTION("bytes " << bytes.first << ',' << bytes.second) {
            MemoryBoundary memory;
            CudaDispatchBuffers buffers(1024, 512);
            bool called = false;
            CHECK_FALSE(buffers.dispatch(bytes.first, bytes.second, memory, [&](void*, void*) {
                called = true;
                return true;
            }));
            CHECK_FALSE(called);
            CHECK(memory.allocations == 0);
            CHECK(memory.releases == 0);
        }
    }
}

TEST_CASE("CUDA completed batches retain their bounded allocations", "[CudaDispatchBuffers][Reuse]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    void* first_input = nullptr;
    void* first_output = nullptr;
    REQUIRE(buffers.dispatch(128, 64, memory, [&](void* input, void* output) {
        first_input = input;
        first_output = output;
        return true;
    }));
    CHECK(buffers.owns_storage());
    REQUIRE(buffers.dispatch(128, 64, memory, [&](void* input, void* output) {
        CHECK(input == first_input);
        CHECK(output == first_output);
        return true;
    }));
    CHECK(memory.allocations == 2);
    CHECK(memory.releases == 0);
    REQUIRE(buffers.release(memory));
    CHECK(memory.live.empty());
    CHECK(memory.releases == 2);
}

TEST_CASE("CUDA smaller batches reuse storage without shrinking it", "[CudaDispatchBuffers][Reuse]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    REQUIRE(buffers.dispatch(200, 100, memory, [](void*, void*) { return true; }));
    REQUIRE(buffers.dispatch(1, 1, memory, [](void*, void*) { return true; }));
    CHECK(memory.allocations == 2);
    CHECK(memory.releases == 0);
    REQUIRE(buffers.release(memory));
    CHECK(memory.live.empty());
}

TEST_CASE("CUDA grows only the buffer that no longer covers the batch", "[CudaDispatchBuffers][Reuse]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    void* first_output = nullptr;
    REQUIRE(buffers.dispatch(128, 64, memory, [&](void*, void* output) {
        first_output = output;
        return true;
    }));
    REQUIRE(buffers.dispatch(512, 64, memory, [&](void* input, void* output) {
        CHECK(memory.live.at(input) >= 512);
        CHECK(memory.live.at(input) <= 1024);
        CHECK(output == first_output);
        return true;
    }));
    CHECK(memory.allocations == 3);
    CHECK(memory.releases == 1);
    REQUIRE(buffers.release(memory));
    CHECK(memory.live.empty());
    CHECK(memory.duplicate_releases == 0);
}

TEST_CASE("CUDA failed growth discards the batch and cleans retained ownership", "[CudaDispatchBuffers][Reuse]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    REQUIRE(buffers.dispatch(128, 64, memory, [](void*, void*) { return true; }));
    memory.fail_allocation = 3;
    bool called = false;
    CHECK_FALSE(buffers.dispatch(512, 64, memory, [&](void*, void*) {
        called = true;
        return true;
    }));
    CHECK_FALSE(called);
    CHECK_FALSE(buffers.owns_storage());
    CHECK(memory.live.empty());
    CHECK(memory.duplicate_releases == 0);
}

TEST_CASE("CUDA failed allocation does not lose a returned owned pointer", "[CudaDispatchBuffers]")
{
    for (bool throws : {false, true}) {
        DYNAMIC_SECTION("throws " << throws) {
            MemoryBoundary memory;
            memory.fail_allocation = 2;
            memory.allocation_error_keeps_pointer = true;
            memory.allocation_error_throws = throws;
            CudaDispatchBuffers buffers(1024, 512);
            bool called = false;
            auto dispatch = [&] {
                return buffers.dispatch(128, 64, memory, [&](void*, void*) {
                    called = true;
                    return true;
                });
            };
            if (throws)
                CHECK_THROWS_AS(dispatch(), std::runtime_error);
            else
                CHECK_FALSE(dispatch());
            CHECK_FALSE(called);
            CHECK(memory.live.empty());
            CHECK(memory.releases == 2);
            CHECK(memory.duplicate_releases == 0);
        }
    }
}

TEST_CASE("CUDA growth cleanup failure rejects the new batch", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    REQUIRE(buffers.dispatch(128, 64, memory, [](void*, void*) { return true; }));
    memory.fail_release = 1;
    bool called = false;
    CHECK_FALSE(buffers.dispatch(512, 64, memory, [&](void*, void*) {
        called = true;
        return true;
    }));
    CHECK_FALSE(called);
    CHECK(buffers.cleanup_failed());
    CHECK_FALSE(buffers.owns_storage());
    CHECK(memory.allocations == 3);
    CHECK(memory.releases == 3);
    CHECK(memory.live.size() == 1);
    CHECK(memory.duplicate_releases == 0);
}

TEST_CASE("CUDA release attempts both owners after a callback throws", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    REQUIRE(buffers.dispatch(128, 64, memory, [](void*, void*) { return true; }));
    memory.throw_release = 1;
    CHECK_THROWS_AS(buffers.release(memory), std::runtime_error);
    CHECK(memory.releases == 2);
    CHECK_FALSE(buffers.owns_storage());
    CHECK(buffers.cleanup_failed());
    CHECK(memory.live.size() == 1);
    CHECK(memory.duplicate_releases == 0);
    REQUIRE(buffers.release(memory));
    CHECK(memory.releases == 2);
}

TEST_CASE("CUDA operation exceptions survive a secondary cleanup exception", "[CudaDispatchBuffers][ExceptionCleanup]")
{
    MemoryBoundary memory;
    memory.throw_release = 1;
    CudaDispatchBuffers buffers(1024, 512);
    try {
        buffers.dispatch(128, 64, memory, [](void*, void*) -> bool {
            throw std::runtime_error("primary operation exception");
        });
        FAIL("operation must throw");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "primary operation exception");
    }
    CHECK(memory.releases == 2);
    CHECK_FALSE(buffers.owns_storage());
    CHECK(buffers.cleanup_failed());
    CHECK(memory.duplicate_releases == 0);
}

TEST_CASE("CUDA poisoned storage retains ownership until the caller can release it", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1024, 512);
    REQUIRE(buffers.dispatch(128, 64, memory, [](void*, void*) { return true; }));
    buffers.poison_cleanup();
    CHECK(buffers.owns_storage());
    CHECK_FALSE(buffers.dispatch(128, 64, memory, [](void*, void*) { return true; }));
    CHECK(memory.allocations == 2);
    REQUIRE(buffers.release(memory));
    CHECK_FALSE(buffers.owns_storage());
    CHECK(memory.live.empty());
    CHECK(buffers.cleanup_failed());
}

TEST_CASE("CUDA storage growth stays within non power of two byte limits", "[CudaDispatchBuffers]")
{
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(1000, 600);
    REQUIRE(buffers.dispatch(999, 599, memory, [&](void* input, void* output) {
        CHECK(memory.live.at(input) >= 999);
        CHECK(memory.live.at(input) <= 1000);
        CHECK(memory.live.at(output) >= 599);
        CHECK(memory.live.at(output) <= 600);
        return true;
    }));
    REQUIRE(buffers.release(memory));
    CHECK(memory.live.empty());
}

TEST_CASE("CUDA storage capacity planning cannot wrap size_t", "[CudaDispatchBuffers]")
{
    const size_t maximum = std::numeric_limits<size_t>::max();
    MemoryBoundary memory;
    CudaDispatchBuffers buffers(maximum, maximum);
    REQUIRE(buffers.dispatch(maximum - 1, maximum, memory, [&](void* input, void* output) {
        CHECK(memory.live.at(input) >= maximum - 1);
        CHECK(memory.live.at(output) == maximum);
        return true;
    }));
    REQUIRE(buffers.release(memory));
    CHECK(memory.live.empty());
}
