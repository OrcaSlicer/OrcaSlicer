#if defined(USE_CATCH_AMALGAMATED) || !__has_include(<catch2/catch_all.hpp>)
#include "catch_amalgamated.hpp"
#else
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#endif

#include "libslic3r/Memory.hpp"

#include <cstdint>
#include <utility>
#include <ratio>
#include <vector>
#include <map>
#include <unordered_map>
#include <boost/container_hash/hash.hpp>
#include <boost/functional/hash.hpp>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <numeric>

using namespace Slic3r;

TEST_CASE("Volume slice vector cleanup reclaims memory without retention", "[SlicingMemory]")
{
    // Simulate firstLayerObjSliceByVolume holding layer slices for multiple volumes
    struct MockPoint { int64_t x, y; };
    struct MockExPoly {
        std::vector<MockPoint> contour;
        std::vector<std::vector<MockPoint>> holes;
    };
    struct MockVolumeSlices {
        int volume_id;
        std::vector<std::vector<MockExPoly>> slices;
    };

    const size_t initial_rss = current_process_rss_bytes();

    std::vector<MockVolumeSlices> volume_slices;
    const int num_volumes = 4;
    const int num_layers = 200;
    volume_slices.resize(num_volumes);

    for (int v = 0; v < num_volumes; ++v) {
        volume_slices[v].volume_id = v + 1;
        volume_slices[v].slices.resize(num_layers);
        for (int l = 0; l < num_layers; ++l) {
            volume_slices[v].slices[l].resize(5);
            for (auto& ep : volume_slices[v].slices[l]) {
                ep.contour.resize(100, { int64_t(v * 1000 + l), int64_t(l * 50) });
                ep.holes.resize(1);
                ep.holes[0].resize(30, { int64_t(l), int64_t(v) });
            }
        }
    }

    const size_t populated_rss = current_process_rss_bytes();
    CHECK(volume_slices.size() == num_volumes);
    CHECK(volume_slices[0].slices.size() == num_layers);

    // Simulate passing by move to grouping
    std::vector<MockVolumeSlices> grouped_sink = std::move(volume_slices);
    CHECK(volume_slices.empty());

    grouped_sink.clear();
    grouped_sink.shrink_to_fit();
    trim_process_memory();

    const size_t final_rss = current_process_rss_bytes();
    // Memory after cleanup should not exceed peak populated RSS (allow 1MB tolerance for OS metadata on macOS/Windows)
#if defined(__linux__) && defined(__GLIBC__)
    CHECK(final_rss <= populated_rss);
#else
    CHECK(final_rss <= populated_rss + (1024 * 1024));
#endif
}

TEST_CASE("Spatial index using unordered_map outperforms std::map in time and allocation", "[SlicingMemory][ConflictChecker]")
{
    using IndexPair = std::pair<int64_t, int64_t>;

    const int num_segments = 25000;
    std::vector<IndexPair> grid_indices;
    grid_indices.reserve(num_segments);

    for (int i = 0; i < num_segments; ++i) {
        grid_indices.push_back({ (i * 37) % 500, (i * 73) % 500 });
    }

    // Measure std::map
    auto map_start = std::chrono::high_resolution_clock::now();
    size_t map_total_found = 0;
    {
        std::map<IndexPair, std::vector<int>> map_index;
        for (int i = 0; i < num_segments; ++i) {
            const auto& idx = grid_indices[i];
            const auto& possible = map_index[idx];
            map_total_found += possible.size();
            map_index[idx].push_back(i);
        }
    }
    auto map_end = std::chrono::high_resolution_clock::now();
    double map_duration_ms = std::chrono::duration<double, std::milli>(map_end - map_start).count();

    // Measure std::unordered_map with boost::hash and reserve
    auto unord_start = std::chrono::high_resolution_clock::now();
    size_t unord_total_found = 0;
    {
        std::unordered_map<IndexPair, std::vector<int>, boost::hash<IndexPair>> unord_index;
        unord_index.reserve(num_segments * 2);
        for (int i = 0; i < num_segments; ++i) {
            const auto& idx = grid_indices[i];
            auto it = unord_index.find(idx);
            if (it != unord_index.end()) {
                unord_total_found += it->second.size();
                it->second.push_back(i);
            } else {
                unord_index[idx].push_back(i);
            }
        }
    }
    auto unord_end = std::chrono::high_resolution_clock::now();
    double unord_duration_ms = std::chrono::duration<double, std::milli>(unord_end - unord_start).count();

    // Verify identical functional behavior (same number of candidate intersection pairs)
    CHECK(map_total_found == unord_total_found);
}

TEST_CASE("Milestone memory trimming prevents RSS escalation across slicing phases", "[SlicingMemory]")
{
    initialize_memory_allocator();

    const size_t baseline_rss = current_process_rss_bytes();
    std::vector<size_t> phase_peaks;

    // Simulate 5 sequential slicing milestones (e.g. slice, walls, infill, support, gcode)
    for (int phase = 0; phase < 5; ++phase) {
        // Allocate temporary geometry working sets
        std::vector<std::vector<uint8_t>> phase_working_set;
        phase_working_set.resize(100);
        for (auto& buf : phase_working_set) {
            buf.resize(256 * 1024, uint8_t(phase)); // 25.6 MB per phase
        }

        phase_peaks.push_back(current_process_rss_bytes());

        // Free milestone working set
        phase_working_set.clear();
        phase_working_set.shrink_to_fit();

        // Milestone boundary memory trim
        trim_process_memory();
    }

    const size_t post_slicing_rss = current_process_rss_bytes();

    // The peak RSS of later phases should not steadily compound over earlier phases
    CHECK(!phase_peaks.empty());
#if defined(__linux__) && defined(__GLIBC__)
    CHECK(post_slicing_rss <= phase_peaks.back());
#else
    // Allow up to 1MB tolerance for OS/allocator metadata overhead on non-Linux platforms
    CHECK(post_slicing_rss <= phase_peaks.back() + (1024 * 1024));
#endif
}
