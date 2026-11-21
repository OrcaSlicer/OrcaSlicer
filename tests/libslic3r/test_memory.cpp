#if defined(USE_CATCH_AMALGAMATED) || !__has_include(<catch2/catch_all.hpp>)
#include "catch_amalgamated.hpp"
#else
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#endif

#include "libslic3r/Memory.hpp"

#include <cstddef>
#include <vector>
#include <cstring>
#include <cstdlib>

using namespace Slic3r;

TEST_CASE("Process RSS queries return valid memory statistics", "[Memory]")
{
    const size_t current_rss = current_process_rss_bytes();
    const size_t peak_rss    = peak_process_rss_bytes();

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    CHECK(current_rss > 0);
    CHECK(peak_rss > 0);
    CHECK(peak_rss >= current_rss);
#else
    // On unsupported platforms, zero is returned as a fallback.
    SUCCEED("Platform memory statistics not implemented");
#endif
}

TEST_CASE("Memory trimming reclaims resident memory after fragmented allocations", "[Memory]")
{
    const size_t initial_rss = current_process_rss_bytes();

    std::vector<void*> temp_ptrs;
    std::vector<void*> pinned_ptrs;

    constexpr size_t num_allocs = 50000;
    constexpr size_t chunk_size = 1024; // 1 KB chunks

    temp_ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
        void* p = std::malloc(chunk_size);
        if (p) {
            std::memset(p, 0xAA, chunk_size);
            temp_ptrs.push_back(p);
        }
        if (i % 200 == 0) {
            void* pinned = std::malloc(32);
            if (pinned) {
                std::memset(pinned, 0x55, 32);
                pinned_ptrs.push_back(pinned);
            }
        }
    }

    const size_t peak_rss = current_process_rss_bytes();

    // Free the temporary buffers, keeping the small pinned objects to cause heap fragmentation
    for (void* p : temp_ptrs) {
        std::free(p);
    }
    temp_ptrs.clear();

    const size_t pre_trim_rss = current_process_rss_bytes();

    // Perform memory trim
    trim_process_memory();

    const size_t post_trim_rss = current_process_rss_bytes();

    // Clean up pinned objects
    for (void* p : pinned_ptrs) {
        std::free(p);
    }
    pinned_ptrs.clear();

    // Final trim
    trim_process_memory();

#if defined(__linux__) && defined(__GLIBC__)
    // Strict regression checks:
    // 1. Post-trim RSS must not exceed pre-trim RSS.
    CHECK(post_trim_rss <= pre_trim_rss);
#else
    // Allow up to 1MB tolerance for OS/allocator metadata overhead (e.g. Darwin zone traversal)
    CHECK(post_trim_rss <= pre_trim_rss + (1024 * 1024));
#endif
    // 2. Post-trim RSS must be significantly less than peak RSS.
    if (peak_rss > initial_rss + (10 * 1024 * 1024)) {
        CHECK(post_trim_rss < peak_rss);
    }
}

TEST_CASE("Idle memory trim prevents RSS RAM regression across repeated cycles", "[Memory][Regression]")
{
    size_t first_cycle_post_trim_rss = 0;

    for (int cycle = 0; cycle < 5; ++cycle) {
        std::vector<void*> cycle_ptrs;
        constexpr size_t cycle_allocs = 20000;
        constexpr size_t alloc_size   = 2048;

        cycle_ptrs.reserve(cycle_allocs);
        for (size_t i = 0; i < cycle_allocs; ++i) {
            void* p = std::malloc(alloc_size);
            if (p) {
                std::memset(p, (int)(i & 0xFF), alloc_size);
                cycle_ptrs.push_back(p);
            }
        }

        for (void* p : cycle_ptrs) {
            std::free(p);
        }
        cycle_ptrs.clear();

        trim_process_memory();

        const size_t current_trimmed_rss = current_process_rss_bytes();
        if (cycle == 0) {
            first_cycle_post_trim_rss = current_trimmed_rss;
        } else {
            // Memory after trimming across repeated cycles must not exhibit runaway bloat / regression.
            // Allow up to 15MB buffer for allocator structures, but ensure no unbounded leak.
            constexpr size_t max_allowed_growth = 15 * 1024 * 1024;
            CHECK(current_trimmed_rss <= first_cycle_post_trim_rss + max_allowed_growth);
        }
    }
}
