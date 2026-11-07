#pragma once
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
#include <vulkan/vulkan.h>
namespace Slic3r::Gpu {
inline VkResult end_compute_readback(VkCommandBuffer command,
    PFN_vkCmdPipelineBarrier barrier, PFN_vkEndCommandBuffer end)
{
    // HOST_COHERENT removes invalidation, not the shader-to-host dependency.
    VkMemoryBarrier memory { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    return end(command);
}
}
#endif
