#include <string>
#include <system_error>
#include <vector>
#include <cstdint>
#include <ios>
#include <utility>
#if __has_include(<catch2/catch_all.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "libslic3r/Gpu/GpuExactGeometry.hpp"
#include "libslic3r/Gpu/VulkanSlicer.hpp"
#include "libslic3r/Gpu/GpuBatchSafety.hpp"
#include "libslic3r/Gpu/CudaSlicer.hpp"
#include "libslic3r/Gpu/GpuShaderLoader.hpp"

#include "libslic3r/Gpu/GpuContextLifecycle.hpp"
#include <future>
#include <thread>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#ifdef _WIN32
#include <stdlib.h>
#endif
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace Slic3r::Gpu;

TEST_CASE("GPU shader files open from Unicode paths and reject malformed modules", "[Gpu][Geometry]")
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        std::filesystem::u8path(std::string(u8"gpu_\uD55C\uAE00_") + std::to_string(suffix) + ".spv");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup {path};
    const std::vector<uint32_t> words {0x07230203, 0x00010000, 0, 1, 0};
    {
        std::ofstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        stream.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * sizeof(uint32_t)));
    }
    REQUIRE(read_shader_words(path.u8string()) == words);
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "bad";
    }
    REQUIRE(read_shader_words(path.u8string()).empty());
    REQUIRE(read_shader_words(path.u8string() + ".missing").empty());
}

class ScopedBackendEnvironment {
public:
    explicit ScopedBackendEnvironment(const char* value)
    {
        if (const char* previous = std::getenv("SLIC3R_GPU_BACKEND")) {
            m_previous = previous;
            m_had_previous = true;
        }
        set(value);
    }
    ~ScopedBackendEnvironment() { set(m_had_previous ? m_previous.c_str() : nullptr); }
private:
    static void set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("SLIC3R_GPU_BACKEND", value ? value : "");
#else
        if (value) setenv("SLIC3R_GPU_BACKEND", value, 1);
        else unsetenv("SLIC3R_GPU_BACKEND");
#endif
    }
    std::string m_previous;
    bool m_had_previous { false };
};

TEST_CASE("Explicit CPU environment overrides enabled GPU preferences", "[Gpu][Geometry]")
{
    ScopedBackendEnvironment environment("cpu");
    VulkanSlicerBackend::set_compute_enabled(true);
    VulkanSlicerBackend::set_cuda_enabled(true);
    VulkanSlicerBackend::set_backend_preference(ComputeBackendPreference::Cuda);
    const auto selected = VulkanSlicerBackend::backend_preference();
    const bool cuda = VulkanSlicerBackend::cuda_enabled();
    const bool vulkan = VulkanSlicerBackend::compute_enabled();
    // Restore process configuration before assertions, also under random order.
    VulkanSlicerBackend::set_compute_enabled(false);
    VulkanSlicerBackend::set_cuda_enabled(false);
    VulkanSlicerBackend::set_backend_preference(ComputeBackendPreference::Cpu);
    REQUIRE(selected == ComputeBackendPreference::Cpu);
    REQUIRE_FALSE(cuda);
    REQUIRE_FALSE(vulkan);
    REQUIRE_FALSE(VulkanSlicerBackend::prepare_for_slicing());
}

TEST_CASE("Backend environment selects explicit CUDA Vulkan auto and rejects unknown values", "[Gpu][Geometry]")
{
    const std::vector<std::pair<const char*, ComputeBackendPreference>> choices {
        {"cuda", ComputeBackendPreference::Cuda}, {"vulkan", ComputeBackendPreference::Vulkan},
        {"auto", ComputeBackendPreference::Auto}, {"unknown", ComputeBackendPreference::Cpu}
    };
    for (const auto& choice : choices) {
        DYNAMIC_SECTION("backend " << choice.first) {
            ScopedBackendEnvironment environment(choice.first);
            REQUIRE(VulkanSlicerBackend::backend_preference() == choice.second);
        }
    }
}

static VulkanVerticalIntersectionRequest safe_request(uint64_t id = 0)
{
    return {{{0, -100}, {1000, 900}}, 500, id};
}

static VulkanVerticalIntersectionBatch reference_batch(
    const std::vector<VulkanVerticalIntersectionRequest>& requests)
{
    VulkanVerticalIntersectionBatch batch;
    batch.dispatched = true;
    for (const auto& request : requests) {
        VulkanVerticalIntersection result;
        REQUIRE(checked_vertical_reference(request, result));
        batch.intersections.push_back(result);
    }
    return batch;
}

TEST_CASE("GPU numeric preflight rejects every unsafe intermediate and narrow denominator", "[Gpu][Geometry]")
{
    const Coord lo = std::numeric_limits<Coord>::min();
    const Coord hi = std::numeric_limits<Coord>::max();
    const std::vector<VulkanVerticalIntersectionRequest> unsafe {
        {{{0, 3'000'000'000}, {4'000'000'000, 3'000'000'000}}, 2'000'000'000, 0},
        {{{lo, lo}, {hi, hi}}, 0, 0},
        {{{lo, hi}, {hi, lo}}, 0, 0},
        {{{0, lo}, {10, hi}}, 5, 0},
        {{{0, hi}, {2, hi}}, 1, 0},
        {{{0, 0}, {Coord(UINT32_MAX) + 1, 0}}, 1, 0},
        // Both products overflow even though the final rational numerator cancels.
        {{{0, hi}, {4, -hi}}, 2, 0},
        {{{0, 0}, {10, 0}}, 0, 0},
        {{{0, 0}, {10, 0}}, 10, 0},
        {{{0, 0}, {0, 10}}, 0, 0}
    };
    for (size_t i = 0; i < unsafe.size(); ++i) {
        DYNAMIC_SECTION("unsafe request " << i) {
            VulkanVerticalIntersection result;
            REQUIRE_FALSE(checked_vertical_reference(unsafe[i], result));
            BackendSafetyState state;
            bool invoked = false;
            const auto batch = dispatch_checked_vertical_batch({unsafe[i]}, state, true, [&] {
                invoked = true;
                return VulkanVerticalIntersectionBatch{};
            });
            REQUIRE_FALSE(invoked);
            REQUIRE_FALSE(batch.dispatched);
            REQUIRE(batch.intersections.empty());
            REQUIRE_FALSE(state.quarantined());
        }
    }
}

TEST_CASE("GPU numeric preflight preserves safe signed extremes and reversed edges", "[Gpu][Geometry]")
{
    const Coord lo = std::numeric_limits<Coord>::min();
    const Coord hi = std::numeric_limits<Coord>::max();
    const std::vector<VulkanVerticalIntersectionRequest> requests {
        safe_request(0), {{{1000, 900}, {0, -100}}, 500, 1},
        {{{lo, -10}, {lo + 10, 10}}, lo + 5, 2},
        {{{hi, 10}, {hi - 10, -10}}, hi - 5, 3},
        {{{0, 0}, {UINT32_MAX, 0}}, 1, 4}
    };
    const auto expected = reference_batch(requests);
    BackendSafetyState state;
    auto result = dispatch_checked_vertical_batch(requests, state, true, [&] { return expected; });
    REQUIRE(result.dispatched);
    REQUIRE(result.intersections.size() == requests.size());
    REQUIRE(result.intersections[0].numerator == 400'000);
    REQUIRE(result.intersections[1].numerator == 400'000);
    REQUIRE_FALSE(state.quarantined());
}

TEST_CASE("GPU batch acceptance discards early and periodic corruptions together", "[Gpu][Geometry]")
{
    std::vector<VulkanVerticalIntersectionRequest> requests;
    for (uint64_t i = 0; i < 1024; ++i)
        requests.push_back(safe_request(i));
    auto damaged = reference_batch(requests);
    damaged.intersections[1].numerator += 1;
    damaged.intersections[512].numerator += 1;
    BackendSafetyState state;
    const auto batch = dispatch_checked_vertical_batch(requests, state, true, [&] { return damaged; });
    REQUIRE_FALSE(batch.dispatched);
    REQUIRE(batch.intersections.empty());
    REQUIRE(state.quarantined());
    bool invoked = false;
    const auto retry = dispatch_checked_vertical_batch(requests, state, true, [&] {
        invoked = true;
        return reference_batch(requests);
    });
    REQUIRE_FALSE(invoked);
    REQUIRE_FALSE(retry.dispatched);
}

TEST_CASE("GPU batch acceptance rejects malformed records and count or order changes", "[Gpu][Geometry]")
{
    const std::vector<VulkanVerticalIntersectionRequest> requests {safe_request(10), safe_request(20)};
    for (int damage = 0; damage < 7; ++damage) {
        DYNAMIC_SECTION("malformation " << damage) {
            auto malformed = reference_batch(requests);
            switch (damage) {
            case 0: malformed.intersections.pop_back(); break;
            case 1: malformed.intersections.push_back(malformed.intersections[0]); break;
            case 2: std::swap(malformed.intersections[0], malformed.intersections[1]); break;
            case 3: malformed.intersections[1].stable_id = 10; break;
            case 4: malformed.intersections[1].valid = false; break;
            case 5: malformed.intersections[1].denominator = 0; break;
            case 6: malformed.dispatched = false; break;
            }
            BackendSafetyState state;
            auto batch = dispatch_checked_vertical_batch(requests, state, true, [&] { return malformed; });
            REQUIRE_FALSE(batch.dispatched);
            REQUIRE(batch.intersections.empty());
            REQUIRE(state.quarantined());
        }
    }
}

TEST_CASE("GPU gate avoids disabled oversized duplicate and failed dispatches", "[Gpu][Geometry]")
{
    BackendSafetyState disabled;
    bool invoked = false;
    auto invoke = [&] { invoked = true; return VulkanVerticalIntersectionBatch{}; };
    REQUIRE_FALSE(dispatch_checked_vertical_batch({safe_request()}, disabled, false, invoke).dispatched);
    REQUIRE_FALSE(invoked);
    REQUIRE_FALSE(disabled.quarantined());
    REQUIRE_FALSE(dispatch_checked_vertical_batch({}, disabled, true, invoke).dispatched);
    REQUIRE_FALSE(dispatch_checked_vertical_batch({safe_request(), safe_request()}, disabled, true, invoke).dispatched);
    std::vector<VulkanVerticalIntersectionRequest> oversized(maximum_intersection_requests + 1, safe_request());
    REQUIRE_FALSE(dispatch_checked_vertical_batch(oversized, disabled, true, invoke).dispatched);
    REQUIRE_FALSE(invoked);
    BackendSafetyState failed;
    REQUIRE_FALSE(dispatch_checked_vertical_batch({safe_request()}, failed, true, []() -> VulkanVerticalIntersectionBatch {
        throw std::runtime_error("simulated allocation/API failure");
    }).dispatched);
    REQUIRE(failed.quarantined());
}

TEST_CASE("Uncompiled GPU backends retain CPU fallback", "[Gpu][Geometry]")
{
    if (!CudaSlicerBackend::query_capabilities().compiled_with_cuda &&
        !VulkanSlicerBackend::query_capabilities().compiled_with_vulkan) {
    REQUIRE_FALSE(CudaSlicerBackend::dispatch_vertical_intersections({safe_request()}).dispatched);
    REQUIRE_FALSE(VulkanSlicerBackend::dispatch_vertical_intersections({safe_request()}).dispatched);
    }
}

TEST_CASE("Whole contour boxes retain containment holes nesting touching and degenerate candidates", "[Gpu][Geometry]")
{
    const std::vector<Segment> boxes {{{0, 0}, {100, 100}}, {{30, 30}, {70, 70}}, {{40, 40}, {60, 60}}};
    // Includes a branch inside the outer contour, one in a hole and one nested.
    // False positives remain on CPU clipping; only a disjoint box proves absence.
    for (const Segment& branch : std::vector<Segment>{
            {{10, 10}, {20, 10}}, {{35, 35}, {36, 35}}, {{45, 45}, {46, 45}},
            {{-10, 0}, {0, 0}}, {{50, 50}, {50, 50}}})
        REQUIRE(may_intersect_filled_boxes(branch, boxes));
    REQUIRE_FALSE(may_intersect_filled_boxes({{-20, -20}, {-10, -10}}, boxes));
    REQUIRE(may_intersect_filled_boxes({{-15, -15}, {-12, -12}}, {{{-20, -20}, {-10, -10}}}));
    REQUIRE_FALSE(may_intersect_filled_boxes({{0, 0}, {0, 0}}, {}));
}

TEST_CASE("Tree batch acceptance never consumes an incorrect negative", "[Gpu][Geometry]")
{
    const std::vector<VulkanTreeContourRequest> requests {{{{10, 10}, {20, 10}}, 0}};
    const std::vector<Segment> boxes {{{0, 0}, {100, 100}}};
    BackendSafetyState state;
    auto batch = dispatch_checked_tree_batch(requests, boxes, state, true, [] {
        VulkanTreeContourBatch result;
        result.dispatched = true;
        result.may_intersect = {0};
        return result;
    });
    REQUIRE_FALSE(batch.dispatched);
    REQUIRE(batch.may_intersect.empty());
    REQUIRE(state.quarantined());
}

TEST_CASE("Tree gate rejects malformed output and bounds pair work before dispatch", "[Gpu][Geometry]")
{
    const std::vector<VulkanTreeContourRequest> requests {{{{10, 10}, {20, 10}}, 0}};
    const std::vector<Segment> boxes {{{0, 0}, {100, 100}}};
    for (int damage = 0; damage < 3; ++damage) {
        DYNAMIC_SECTION("tree malformed " << damage) {
            BackendSafetyState state;
            const auto result = dispatch_checked_tree_batch(requests, boxes, state, true, [&] {
                VulkanTreeContourBatch batch;
                batch.dispatched = damage != 0;
                if (damage != 1) batch.may_intersect = {2};
                return batch;
            });
            REQUIRE_FALSE(result.dispatched);
            REQUIRE(result.may_intersect.empty());
            REQUIRE(state.quarantined());
        }
    }
    BackendSafetyState state;
    bool invoked = false;
    std::vector<VulkanTreeContourRequest> many(4097, requests.front());
    std::vector<Segment> many_boxes(4096, boxes.front());
    const auto result = dispatch_checked_tree_batch(many, many_boxes, state, true, [&] {
        invoked = true;
        return VulkanTreeContourBatch{};
    });
    REQUIRE_FALSE(result.dispatched);
    REQUIRE_FALSE(invoked);
    REQUIRE_FALSE(state.quarantined());
    auto candidate = dispatch_checked_tree_batch(requests, boxes, state, true, [] {
        VulkanTreeContourBatch batch;
        batch.dispatched = true;
        batch.may_intersect = {1};
        return batch;
    });
    REQUIRE(candidate.dispatched);
}

TEST_CASE("GPU defaults preserve CPU geometry without initialization", "[Gpu][Geometry]")
{
    REQUIRE_FALSE(VulkanSlicerBackend::compute_enabled());
    REQUIRE_FALSE(VulkanSlicerBackend::cuda_enabled());
    REQUIRE(VulkanSlicerBackend::backend_preference() == ComputeBackendPreference::Cpu);
    REQUIRE_FALSE(VulkanSlicerBackend::prepare_for_slicing());
}

TEST_CASE("GPU acceptance checks the early and periodic records", "[Gpu][Geometry]")
{
    REQUIRE(VulkanSlicerBackend::should_validate_vertical_intersection(1, 1024));
    REQUIRE(VulkanSlicerBackend::should_validate_vertical_intersection(512, 1024));
}

TEST_CASE("GPU orientation reference covers the entire signed coordinate domain", "[Gpu][Geometry]")
{
    const Coord lo = std::numeric_limits<Coord>::min();
    const Coord hi = std::numeric_limits<Coord>::max();
    const boost::multiprecision::int256_t width = boost::multiprecision::int256_t(hi) - lo;
    REQUIRE(orient2d({lo, lo}, {hi, lo}, {lo, hi}) == width * width);
}

TEST_CASE("GPU exact geometry uses a wider host orientation reference", "[Gpu][Geometry]")
{
    // This product cannot fit in a signed 64-bit intermediate. It proves the
    // CPU acceptance gate cannot accidentally mirror an overflowing shader
    // calculation when it validates a tile candidate.
    constexpr Coord extent = 1'000'000'000'000;
    const Point a { 0, 0 };
    const Point b { extent, 0 };
    const Point c { 0, extent };

    const WideCoord orientation = orient2d(a, b, c);
    REQUIRE(orientation > std::numeric_limits<Coord>::max());
}

TEST_CASE("GPU exact geometry enforces the tile dispatch range", "[Gpu][Geometry]")
{
    const Point origin { 30 * coordinate_scale, 40 * coordinate_scale };
    const Segment eligible {
        { origin.x - max_tile_radius, origin.y + max_tile_radius },
        { origin.x + max_tile_radius, origin.y - max_tile_radius }
    };
    const Segment out_of_range {
        { origin.x - max_tile_radius - 1, origin.y },
        { origin.x, origin.y }
    };

    REQUIRE(fits_in_tile(eligible, origin));
    REQUIRE_FALSE(fits_in_tile(out_of_range, origin));
}

TEST_CASE("GPU deterministic geometry requires a compute queue and int64 shaders", "[Gpu][Vulkan]")
{
    VulkanDeviceInfo device;

    REQUIRE_FALSE(VulkanSlicerBackend::supports_deterministic_geometry(device));
    device.compute_queue = true;
    REQUIRE_FALSE(VulkanSlicerBackend::supports_deterministic_geometry(device));
    device.shader_int64 = true;
    REQUIRE(VulkanSlicerBackend::supports_deterministic_geometry(device));
}

TEST_CASE("Staging cleanup waits for complete GPU qualification", "[Gpu][Concurrency]")
{
    ContextLifecycle lifecycle;
    std::promise<void> entered, resume, cleanup_started;
    auto release = resume.get_future().share();
    std::atomic<bool> qualified {false};
    std::thread init([&] { lifecycle.initialize_once([&] {
        entered.set_value(); release.wait(); qualified.store(true);
    }); });
    entered.get_future().wait();
    auto cleanup = std::async(std::launch::async, [&] {
        cleanup_started.set_value();
        auto lock = lifecycle.acquire();
        return qualified.load();
    });
    cleanup_started.get_future().wait();
    const auto blocked = cleanup.wait_for(std::chrono::milliseconds(100));
    resume.set_value();
    init.join();
    const bool observed_qualification = cleanup.get();
    REQUIRE(blocked == std::future_status::timeout);
    REQUIRE(observed_qualification);
    int calls = 0;
    lifecycle.initialize_once([&] { ++calls; });
    REQUIRE(calls == 0);
}
