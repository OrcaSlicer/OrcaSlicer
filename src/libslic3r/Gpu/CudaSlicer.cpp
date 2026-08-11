#include "CudaSlicer.hpp"

#include <chrono>

namespace Slic3r::Gpu {

#ifdef SLIC3R_ENABLE_CUDA_SLICER
extern bool cuda_runtime_query(CudaSlicerCapabilities& capabilities);
extern bool cuda_runtime_prepare(std::string& diagnostic);
extern bool cuda_runtime_dispatch(
    const std::vector<VulkanVerticalIntersectionRequest>& requests,
    std::vector<VulkanVerticalIntersection>& intersections,
    double& gpu_elapsed_ms,
    std::string& diagnostic);
extern void cuda_runtime_release();
#endif

CudaSlicerCapabilities CudaSlicerBackend::query_capabilities()
{
    CudaSlicerCapabilities capabilities;
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    capabilities.compiled_with_cuda = true;
    if (!cuda_runtime_query(capabilities) && capabilities.diagnostic.empty())
        capabilities.diagnostic = "CUDA runtime is unavailable or no CUDA device was found.";
#else
    capabilities.diagnostic = "CUDA slicer was not compiled; configure with -DSLIC3R_ENABLE_CUDA_SLICER=ON and a CUDA toolkit.";
#endif
    return capabilities;
}

bool CudaSlicerBackend::prepare_for_slicing()
{
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    std::string diagnostic;
    return cuda_runtime_prepare(diagnostic);
#else
    return false;
#endif
}

VulkanVerticalIntersectionBatch CudaSlicerBackend::dispatch_vertical_intersections(
    const std::vector<VulkanVerticalIntersectionRequest>& requests)
{
    VulkanVerticalIntersectionBatch batch;
#ifdef SLIC3R_ENABLE_CUDA_SLICER
    const auto host_start = std::chrono::steady_clock::now();
    batch.dispatched = cuda_runtime_dispatch(requests, batch.intersections,
                                              batch.gpu_elapsed_ms, batch.diagnostic);
    batch.queue_submissions = batch.dispatched ? 1u : 0u;
    batch.host_elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - host_start).count();
    if (batch.dispatched && batch.diagnostic.empty())
        batch.diagnostic = "CUDA exact vertical-intersection batch completed.";
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
