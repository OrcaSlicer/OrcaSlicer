#pragma once

#include "VulkanSlicer.hpp"

namespace Slic3r::Gpu {

struct CudaSlicerCapabilities {
    bool        compiled_with_cuda { false };
    bool        runtime_available { false };
    std::string device_name;
    std::string diagnostic;
    uint32_t    multiprocessors { 0 };
    uint32_t    max_threads_per_block { 0 };
};

// CUDA is an optional backend.  The public API is always available so a build
// made without the CUDA toolkit can still select CUDA-first and safely fall
// back to Vulkan or the exact CPU implementation.
class CudaSlicerBackend {
public:
    static CudaSlicerCapabilities query_capabilities();
    static bool prepare_for_slicing();
    static VulkanVerticalIntersectionBatch dispatch_vertical_intersections(
        const std::vector<VulkanVerticalIntersectionRequest>& requests);
    static void release_resources();
};

} // namespace Slic3r::Gpu
