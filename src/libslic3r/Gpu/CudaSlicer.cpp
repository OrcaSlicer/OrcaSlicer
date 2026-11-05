#include "GpuExactGeometry.hpp"
#include <ratio>
#include <string>
#include <vector>
#include "CudaSlicer.hpp"
#include "VulkanSlicer.hpp"
#include "GpuBatchSafety.hpp"

#include <chrono>

namespace Slic3r::Gpu {

#ifdef SLIC3R_ENABLE_CUDA_SLICER
extern bool cuda_runtime_query(CudaSlicerCapabilities& capabilities);
extern bool cuda_runtime_prepare(std::string& diagnostic);
extern bool cuda_runtime_dispatch(
    const std::vector<VerticalIntersectionRequest>& requests,
    std::vector<VerticalIntersection>& intersections,
    double& gpu_elapsed_ms,
    std::string& diagnostic);
extern void cuda_runtime_release();
#endif

CudaSlicerCapabilities CudaSlicerBackend::query_capabilities()
{
    CudaSlicerCapabilities capabilities;
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    capabilities.compiled_with_cuda = true;
    if (cuda_safety_state().quarantined()) {
        capabilities.diagnostic = "CUDA backend quarantined after a dispatch or validation failure.";
        return capabilities;
    }
    if (!cuda_runtime_query(capabilities)) {
        cuda_safety_state().quarantine();
        if (capabilities.diagnostic.empty())
            capabilities.diagnostic = "CUDA runtime is unavailable or no CUDA device was found.";
    }
#else
    capabilities.diagnostic = "CUDA slicer was not compiled; configure with -DSLIC3R_ENABLE_CUDA_SLICER=ON and a CUDA toolkit.";
#endif
    return capabilities;
}

bool CudaSlicerBackend::prepare_for_slicing()
{
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    if (!VulkanSlicerBackend::cuda_enabled() ||
        VulkanSlicerBackend::backend_preference() == ComputeBackendPreference::Cpu)
        return false;
    std::string diagnostic;
    if (cuda_runtime_prepare(diagnostic))
        return true;
    cuda_safety_state().quarantine();
    return false;
#else
    return false;
#endif
}

VerticalIntersectionBatch CudaSlicerBackend::dispatch_vertical_intersections(
    const std::vector<VerticalIntersectionRequest>& requests)
{
    VerticalIntersectionBatch batch;
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    batch = dispatch_checked_vertical_batch(requests, cuda_safety_state(),
        VulkanSlicerBackend::cuda_enabled() &&
        VulkanSlicerBackend::backend_preference() != ComputeBackendPreference::Cpu, [&] {
        VerticalIntersectionBatch result;
        const auto host_start = std::chrono::steady_clock::now();
        result.dispatched = cuda_runtime_dispatch(requests, result.intersections,
                                                   result.gpu_elapsed_ms, result.diagnostic);
        result.queue_submissions = result.dispatched ? 1u : 0u;
        result.host_elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - host_start).count();
        return result;
    });
#else
    (void) requests;
    batch.diagnostic = "CUDA slicer is not compiled in this build.";
#endif
    return batch;
}

void CudaSlicerBackend::release_resources()
{
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    cuda_runtime_release();
#endif
}

} // namespace Slic3r::Gpu
