#include <catch2/catch_all.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Gpu/VulkanSlicer.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/Utils.hpp"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {
void set_backend_environment(const char* value)
{
#ifdef _WIN32
    if (_putenv_s("SLIC3R_GPU_BACKEND", value ? value : "") != 0)
        std::abort();
#else
    if ((value ? setenv("SLIC3R_GPU_BACKEND", value, 1) : unsetenv("SLIC3R_GPU_BACKEND")) != 0)
        std::abort();
#endif
}

struct BackendSettings {
    bool had_environment = std::getenv("SLIC3R_GPU_BACKEND") != nullptr;
    std::string environment = had_environment ? std::getenv("SLIC3R_GPU_BACKEND") : "";
    std::string resources = resources_dir();
    bool compute = Gpu::VulkanSlicerBackend::compute_enabled();
    bool cuda = Gpu::VulkanSlicerBackend::cuda_enabled();

    Gpu::ComputeBackendPreference saved_preference = Gpu::VulkanSlicerBackend::backend_preference();
    ~BackendSettings()
    {
        set_backend_environment(had_environment ? environment.c_str() : nullptr);
        set_resources_dir(resources);
        Gpu::VulkanSlicerBackend::set_backend_preference(saved_preference);
        Gpu::VulkanSlicerBackend::set_compute_enabled(compute);
        Gpu::VulkanSlicerBackend::set_cuda_enabled(cuda);
    }
};

Polylines fill_fixture(const ExPolygon& polygon, float angle)
{
    std::unique_ptr<Fill> fill(Fill::new_from_type("rectilinear"));
    fill->angle = angle;
    fill->layer_id = 0;
    fill->z = 0.2;
    fill->spacing = 0.4;
    fill->bounding_box = get_extents(polygon);
    FillParams params;
    params.density = 1.0f;
    params.dont_adjust = true;
    Surface surface(stTop, polygon);
    return fill->fill_surface(&surface, params);
}

void compare_backend(Gpu::ComputeBackendPreference backend, bool require_hardware)
{
    BackendSettings restore;
    if (const char* resources = std::getenv("SLIC3R_GPU_TEST_RESOURCES"))
        set_resources_dir(resources);
    if (require_hardware && backend == Gpu::ComputeBackendPreference::Vulkan)
        REQUIRE_FALSE(resources_dir().empty());
    const auto p = [](int x, int y) { return Point(scale_(double(x)), scale_(double(y))); };
    // Sloping/reversed edges, aligned vertices, concavity, negative coordinates,
    // and a clockwise hole exercise the product's CPU-only topology decisions.
    const std::vector<ExPolygon> fixtures{
        ExPolygon(Polygon{{p(-80,-60),p(80,-60),p(80,60),p(-80,60)}}),
        ExPolygon(Polygon{{p(-90,0),p(0,-70),p(90,0),p(0,70)}}),
        ExPolygon(Polygon{{p(-90,-70),p(90,-70),p(90,70),p(0,20),p(-90,70)}},
                  Polygon{{p(-30,-30),p(-30,0),p(30,0),p(30,-30)}})
    };
    size_t accepted = 0;
    size_t submissions = 0;
    for (const auto& fixture : fixtures) {
        for (float angle : {0.0f, 0.78539816f, 1.5707963f}) {
            set_backend_environment("cpu");
            Gpu::VulkanSlicerBackend::set_compute_enabled(false);
            Gpu::VulkanSlicerBackend::set_cuda_enabled(false);
            Gpu::VulkanSlicerBackend::set_backend_preference(Gpu::ComputeBackendPreference::Cpu);
            const Polylines cpu = fill_fixture(fixture, angle);
            REQUIRE_FALSE(cpu.empty());
            set_backend_environment(backend == Gpu::ComputeBackendPreference::Cuda ? "cuda" : "vulkan");
            Gpu::VulkanSlicerBackend::set_backend_preference(backend);
            Gpu::VulkanSlicerBackend::set_compute_enabled(backend == Gpu::ComputeBackendPreference::Vulkan);
            Gpu::VulkanSlicerBackend::set_cuda_enabled(backend == Gpu::ComputeBackendPreference::Cuda);
            Gpu::VulkanSlicerBackend::begin_slicing_session();
            REQUIRE(fill_fixture(fixture, angle) == cpu);
            REQUIRE(fill_fixture(fixture, angle) == cpu);
            const auto stats = Gpu::VulkanSlicerBackend::query_runtime_stats();
            accepted += stats.accepted_gpu_intersections;
            submissions += stats.queue_submissions;
            REQUIRE(stats.validation_failures == 0);
            if (stats.accepted_gpu_intersections > 0)
                REQUIRE(stats.active_backend == (backend == Gpu::ComputeBackendPreference::Cuda ? "CUDA" : "Vulkan"));
        }
    }
    if (require_hardware) {
        REQUIRE(submissions > 0);
        REQUIRE(accepted > 0);
    }
}
}

TEST_CASE("Rectilinear backend selection preserves ordered CPU paths or falls back", "[GpuRectilinearEquivalence][Fill][Gpu]")
{
    SECTION("CUDA") { compare_backend(Gpu::ComputeBackendPreference::Cuda, false); }
    SECTION("Vulkan") { compare_backend(Gpu::ComputeBackendPreference::Vulkan, false); }
}

// Hidden tests require the named physical backend and record actual dispatch.
TEST_CASE("Rectilinear CUDA dispatch preserves ordered CPU paths", "[.][GpuRectilinearEquivalence][GpuHardware][Cuda]")
{
    compare_backend(Gpu::ComputeBackendPreference::Cuda, true);
}

TEST_CASE("Rectilinear Vulkan dispatch preserves ordered CPU paths", "[.][GpuRectilinearEquivalence][GpuHardware][Vulkan]")
{
    compare_backend(Gpu::ComputeBackendPreference::Vulkan, true);
}
