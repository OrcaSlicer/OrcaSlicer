#include "GpuVulkanSynchronization.hpp"
#include <cassert>
#include <cstdint>
#include <initializer_list>
static unsigned phase;
static VkCommandBuffer expected;
static VKAPI_ATTR void VKAPI_CALL record_barrier(VkCommandBuffer cmd,
    VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags flags,
    uint32_t count, const VkMemoryBarrier* memory, uint32_t buffers,
    const VkBufferMemoryBarrier*, uint32_t images, const VkImageMemoryBarrier*)
{
    assert(phase == 0 && cmd == expected);
    assert(source == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    assert(destination == VK_PIPELINE_STAGE_HOST_BIT);
    assert(flags == 0 && count == 1 && buffers == 0 && images == 0);
    assert(memory && memory->sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER && !memory->pNext);
    assert(memory->srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT);
    assert(memory->dstAccessMask == VK_ACCESS_HOST_READ_BIT);
    phase = 1;
}
static VKAPI_ATTR VkResult VKAPI_CALL end_command(VkCommandBuffer cmd) {
    assert(cmd == expected && phase == 1); phase = 2; return VK_ERROR_DEVICE_LOST;
}
int main() {
    for (uintptr_t handle : {uintptr_t(1), uintptr_t(2)}) {
        expected = reinterpret_cast<VkCommandBuffer>(handle); phase = 0;
        assert(Slic3r::Gpu::end_compute_readback(expected, record_barrier, end_command) == VK_ERROR_DEVICE_LOST);
        assert(phase == 2);
    }
}
