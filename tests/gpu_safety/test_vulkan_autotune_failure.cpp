// Compile the real context against fake Vulkan entry points, never a loader.
// Preinclude dependencies before exposing private context operations to this
// translation unit; production headers/API need no test hooks.
#include "GpuExactGeometry.hpp"
#include "VulkanSlicer.hpp"
#include "CudaSlicer.hpp"
#include "GpuBatchSafety.hpp"
#include "GpuShaderLoader.hpp"
#include "GpuContextLifecycle.hpp"
#include "GpuVulkanSynchronization.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <string>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <locale>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <iostream>
#include <set>
#define private public
#include "../../src/libslic3r/Gpu/VulkanSlicer.cpp"
#undef private

using namespace Slic3r::Gpu;
namespace {
unsigned failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; ++failures; } } while (false)
template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
struct Driver {
    unsigned submits = 0, resets = 0, descriptor_writes = 0, destroys = 0;
    unsigned unsafe = 0, fail_wait = 0, pipeline_creates = 0;
    bool pending = false, idle_success = true, fail_reset = false;
    unsigned fail_create = 0;
    bool fail_submit = false, fail_fence_reset = false;
    VkPipeline bound = VK_NULL_HANDLE;
    std::set<VkPipeline> pipelines;
    std::vector<PackedVerticalIntersectionRequest> input = std::vector<PackedVerticalIntersectionRequest>(4096);
    std::vector<PackedVerticalIntersectionResult> output = std::vector<PackedVerticalIntersectionResult>(4096);
    std::vector<unsigned char> pending_input;
    void mutation() { if (pending) ++unsafe; }
} driver;

void configure(VulkanIntersectionContext& context)
{
    context.m_device = handle<VkDevice>(1);
    context.m_queue = handle<VkQueue>(2);
    context.m_command_buffer = handle<VkCommandBuffer>(3);
    context.m_dispatch_fence = handle<VkFence>(4);
    context.m_workgroup_size = 128;
    context.m_maximum_workgroup_size = 256;
    context.m_max_compute_workgroup_count_x = 64;
    context.m_storage_buffer_request_limit = 4096;
    context.m_initial_staging_request_capacity = 256;
    context.m_staging_request_capacity = driver.input.size();
    context.m_input_mapping = driver.input.data();
    context.m_output_mapping = driver.output.data();
    context.m_compute_timestamps_available = true;
    context.m_timestamp_period_ns = 1;
    context.update_submission_request_limit();
}

void make_shaders(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    const uint32_t words[] = {0x07230203, 0x00010500, 0, 1, 0};
    for (unsigned size : {64u, 128u, 256u}) {
        std::ofstream file(directory / ("perimeter_infill_candidates_" + std::to_string(size) + ".spv"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(words), sizeof(words));
    }
    g_shader_directory = directory.u8string();
}
}

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* shader)
{ *shader = handle<VkShaderModule>(5); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* pipeline)
{
    ++driver.pipeline_creates;
    if (driver.fail_create == driver.pipeline_creates && driver.fail_create == 1)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pipeline = handle<VkPipeline>(100 + driver.pipeline_creates);
    driver.pipelines.insert(*pipeline);
    if (driver.fail_create == driver.pipeline_creates)
        return VK_ERROR_OUT_OF_HOST_MEMORY; // API may return a partially created handle.
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*)
{ driver.mutation(); ++driver.destroys; CHECK(driver.pipelines.erase(pipeline) == 1); }
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*)
{ driver.mutation(); ++driver.descriptor_writes; }
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags)
{ driver.mutation(); ++driver.resets; return driver.fail_reset ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline pipeline) { driver.bound = pipeline; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t, const VkFence*)
{ driver.mutation(); return driver.fail_fence_reset ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)
{
    driver.mutation(); ++driver.submits; driver.pending = true;
    if (driver.fail_submit) {
        const auto* first = reinterpret_cast<const unsigned char*>(driver.input.data());
        driver.pending_input.assign(first, first + driver.input.size() * sizeof(driver.input[0]));
        return VK_ERROR_DEVICE_LOST;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)
{
    if (driver.submits == driver.fail_wait) {
        // Poison mapped input so a repeated identical probe write is detectable.
        std::memset(driver.input.data(), 0xa5, driver.input.size() * sizeof(driver.input[0]));
        const auto* first = reinterpret_cast<const unsigned char*>(driver.input.data());
        driver.pending_input.assign(first, first + driver.input.size() * sizeof(driver.input[0]));
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    driver.pending = false;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice)
{ if (!driver.idle_success) return VK_ERROR_OUT_OF_HOST_MEMORY; driver.pending = false; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void* values, VkDeviceSize, VkQueryResultFlags)
{
    auto* times = static_cast<uint64_t*>(values);
    times[0] = 0;
    times[1] = driver.bound == handle<VkPipeline>(102) ? 10 : 100;
    return VK_SUCCESS;
}

// These ordinary resource operations must not touch a pending submission.
#define DESTROY(name, type) VKAPI_ATTR void VKAPI_CALL name(VkDevice, type, const VkAllocationCallbacks*) { driver.mutation(); }
DESTROY(vkDestroyDescriptorPool, VkDescriptorPool)
DESTROY(vkDestroyFence, VkFence)
DESTROY(vkDestroyQueryPool, VkQueryPool)
DESTROY(vkDestroyCommandPool, VkCommandPool)
DESTROY(vkDestroyPipelineLayout, VkPipelineLayout)
DESTROY(vkDestroyDescriptorSetLayout, VkDescriptorSetLayout)
DESTROY(vkDestroyBuffer, VkBuffer)
DESTROY(vkFreeMemory, VkDeviceMemory)
#undef DESTROY
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) { driver.mutation(); }
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*) { driver.mutation(); }
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*) { driver.mutation(); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* properties)
{ properties->memoryTypeCount = 1; properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**) { return VK_ERROR_OUT_OF_HOST_MEMORY; }

// Unused initialization/capability entry points are defined too: the fixture
// links portably without a Vulkan library or reliance on linker dead stripping.
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
#define UNEXPECTED_RESULT(name, ...) VKAPI_ATTR VkResult VKAPI_CALL name(__VA_ARGS__) { CHECK(false); return VK_ERROR_INITIALIZATION_FAILED; }
#define UNEXPECTED_VOID(name, ...) VKAPI_ATTR void VKAPI_CALL name(__VA_ARGS__) { CHECK(false); }
UNEXPECTED_RESULT(vkEnumeratePhysicalDevices, VkInstance, uint32_t*, VkPhysicalDevice*)
UNEXPECTED_VOID(vkGetPhysicalDeviceFeatures, VkPhysicalDevice, VkPhysicalDeviceFeatures*)
UNEXPECTED_VOID(vkGetPhysicalDeviceProperties, VkPhysicalDevice, VkPhysicalDeviceProperties*)
UNEXPECTED_VOID(vkGetPhysicalDeviceProperties2, VkPhysicalDevice, VkPhysicalDeviceProperties2*)
UNEXPECTED_VOID(vkGetPhysicalDeviceQueueFamilyProperties, VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*)
UNEXPECTED_RESULT(vkCreateDevice, VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*)
UNEXPECTED_VOID(vkGetDeviceQueue, VkDevice, uint32_t, uint32_t, VkQueue*)
UNEXPECTED_RESULT(vkCreateDescriptorSetLayout, VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout*)
UNEXPECTED_RESULT(vkCreatePipelineLayout, VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*)
UNEXPECTED_RESULT(vkCreateCommandPool, VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*)
UNEXPECTED_RESULT(vkCreateDescriptorPool, VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*)
UNEXPECTED_RESULT(vkAllocateDescriptorSets, VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*)
UNEXPECTED_RESULT(vkAllocateCommandBuffers, VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*)
UNEXPECTED_RESULT(vkCreateFence, VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*)
UNEXPECTED_RESULT(vkCreateQueryPool, VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool*)
#undef UNEXPECTED_RESULT
#undef UNEXPECTED_VOID
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::string scenario = argv[1];
    make_shaders(std::filesystem::u8path(argv[2]));
    const std::vector<VerticalIntersectionRequest> valid {{{{0, 0}, {10, 20}}, 5, 1}};
    {
        VulkanIntersectionContext context;
        configure(context);
        if (scenario == "initialize") {
            context.initialize();
            CHECK(!context.m_ready);
            CHECK(vulkan_safety_state().quarantined());
            CHECK(driver.submits == 0);
        } else if (scenario == "preflight") {
            auto invalid = valid;
            invalid[0].scan_x = 10;
            CHECK(!context.dispatch_locked(invalid).dispatched);
            context.m_max_requests_per_submission = 0;
            CHECK(!context.dispatch_locked(valid).dispatched);
            CHECK(!vulkan_safety_state().quarantined());
            CHECK(driver.submits == 0);
            CHECK(driver.descriptor_writes == 0);
        } else if (scenario == "success" || scenario == "capacity-candidate") {
            if (scenario == "capacity-candidate") {
                context.m_max_compute_workgroup_count_x = 1;
                context.update_submission_request_limit();
            }
            CHECK(context.autotune_workgroup_size());
            CHECK(context.m_workgroup_size == 128);
            CHECK(driver.submits == (scenario == "success" ? 12u : 8u));
            CHECK(driver.destroys == 2);
            CHECK(!vulkan_safety_state().quarantined());
        } else {
            driver.fail_wait = scenario == "timed" ? 3 : 1;
            driver.fail_create = scenario == "create" ? 1 : (scenario == "partial-create" ? 2 : 0);
            driver.fail_reset = scenario == "reset";
            driver.fail_submit = scenario == "submit";
            driver.fail_fence_reset = scenario == "fence-reset";
            driver.idle_success = scenario == "completed";
            context.m_ready = true;
            CHECK(!context.autotune_workgroup_size());
            CHECK(!context.m_ready);
            CHECK(vulkan_safety_state().quarantined());
            CHECK(driver.unsafe == 0);
            if (driver.fail_create) {
                CHECK(driver.pipeline_creates == driver.fail_create);
                CHECK(driver.pipelines.empty());
            }
            else if (driver.fail_reset) CHECK(driver.resets == 1);
            else if (driver.fail_fence_reset) CHECK(driver.submits == 0);
            else CHECK(driver.submits == driver.fail_wait);
            const auto submits = driver.submits;
            const auto resets = driver.resets;
            const auto writes = driver.descriptor_writes;
            CHECK(!context.dispatch_locked(valid).dispatched);
            CHECK(!context.dispatch_tree_contours_locked({{{{0, 0}, {10, 20}}, 1}}, {{{0, 0}, {10, 20}}}).dispatched);
            CHECK(driver.submits == submits);
            CHECK(driver.resets == resets);
            CHECK(driver.descriptor_writes == writes);
            bool dispatched = false;
            CHECK(!dispatch_checked_vertical_batch(valid, vulkan_safety_state(), true, [&] {
                dispatched = true; return context.dispatch_locked(valid);
            }).dispatched);
            CHECK(!dispatched);
            if (driver.pending) {
                CHECK(driver.destroys == 0);
                CHECK(std::memcmp(driver.input.data(), driver.pending_input.data(), driver.pending_input.size()) == 0);
            }
        }
    }
    CHECK(driver.unsafe == 0);
    if (driver.idle_success) CHECK(driver.pipelines.empty());
    if (failures) std::cerr << scenario << ": " << failures << " failures\n";
    else std::cout << scenario << ": PASS (fake Vulkan API; no GPU)\n";
    return failures ? 1 : 0;
}
