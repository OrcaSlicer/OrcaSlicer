#include "VulkanSlicer.hpp"

#include "Utils.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
#include <vulkan/vulkan.h>
#endif

namespace Slic3r::Gpu {

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
namespace {

struct alignas(8) PackedVerticalIntersectionRequest {
    int64_t  ax;
    int64_t  ay;
    int64_t  bx;
    int64_t  by;
    int64_t  scan_x;
    uint64_t stable_id;
};
static_assert(sizeof(PackedVerticalIntersectionRequest) == 48);

struct alignas(8) PackedVerticalIntersectionResult {
    int64_t  numerator;
    int64_t  denominator;
    uint64_t stable_id;
    uint32_t valid;
    uint32_t reserved;
};
static_assert(sizeof(PackedVerticalIntersectionResult) == 32);

uint32_t host_visible_coherent_memory_type(VkPhysicalDevice physical_device, uint32_t type_mask)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        const bool allowed = (type_mask & (uint32_t(1) << index)) != 0;
        const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[index].propertyFlags;
        if (allowed && (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            return index;
    }
    return UINT32_MAX;
}

bool create_storage_buffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size,
                           VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo buffer_info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const uint32_t memory_type = host_visible_coherent_memory_type(physical_device, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX)
        return false;

    VkMemoryAllocateInfo allocation_info { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation_info.allocationSize = requirements.size;
    allocation_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation_info, nullptr, &memory) != VK_SUCCESS)
        return false;
    return vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
}

std::vector<uint32_t> load_intersection_shader()
{
    const std::string path = Slic3r::resources_dir() + "/vulkan/perimeter_infill_candidates.spv";
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return {};
    const std::streamsize byte_count = stream.tellg();
    if (byte_count <= 0 || (byte_count % std::streamsize(sizeof(uint32_t))) != 0)
        return {};
    std::vector<uint32_t> words(size_t(byte_count) / sizeof(uint32_t));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(words.data()), byte_count))
        return {};
    return words;
}

class VulkanIntersectionContext {
public:
    ~VulkanIntersectionContext()
    {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            if (m_descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
            if (m_command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(m_device, m_command_pool, nullptr);
            if (m_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_device, m_pipeline, nullptr);
            if (m_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
            if (m_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_device, m_descriptor_set_layout, nullptr);
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE)
            vkDestroyInstance(m_instance, nullptr);
    }

    VulkanVerticalIntersectionBatch dispatch(const std::vector<VulkanVerticalIntersectionRequest>& requests)
    {
        VulkanVerticalIntersectionBatch batch;
        if (requests.empty()) {
            batch.diagnostic = "No vertical intersections were submitted to Vulkan.";
            return batch;
        }

        std::call_once(m_initialize_once, [this] { this->initialize(); });
        if (!m_ready) {
            batch.diagnostic = m_diagnostic;
            return batch;
        }

        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        return dispatch_locked(requests);
    }

private:
    void initialize()
    {
        VkApplicationInfo application_info { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        application_info.pApplicationName = "OrcaVulkanSlicer";
        application_info.applicationVersion = 1;
        application_info.pEngineName = "OrcaSlicer";
        application_info.engineVersion = 1;
        application_info.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instance_info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instance_info.pApplicationInfo = &application_info;
        if (vkCreateInstance(&instance_info, nullptr, &m_instance) != VK_SUCCESS) {
            m_diagnostic = "Vulkan instance creation failed for infill compute.";
            return;
        }

        uint32_t device_count = 0;
        if (vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
            m_diagnostic = "No Vulkan physical device is available for infill compute.";
            return;
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        if (vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data()) != VK_SUCCESS) {
            m_diagnostic = "Vulkan physical-device enumeration failed for infill compute.";
            return;
        }

        int best_score = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceFeatures features {};
            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (features.shaderInt64 != VK_TRUE)
                continue;

            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0 || families[family].queueCount == 0)
                    continue;
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best_score) {
                    best_score = score;
                    m_physical_device = candidate;
                    m_queue_family = family;
                }
            }
        }
        if (m_physical_device == VK_NULL_HANDLE) {
            m_diagnostic = "No Vulkan compute queue with shaderInt64 is available for exact infill intersections.";
            return;
        }

        const float queue_priority = 1.f;
        VkDeviceQueueCreateInfo queue_info { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue_info.queueFamilyIndex = m_queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        VkPhysicalDeviceFeatures enabled_features {};
        enabled_features.shaderInt64 = VK_TRUE;
        VkDeviceCreateInfo device_info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.pEnabledFeatures = &enabled_features;
        if (vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device) != VK_SUCCESS) {
            m_diagnostic = "Vulkan logical-device creation failed for infill compute.";
            return;
        }
        vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };
        VkDescriptorSetLayoutCreateInfo layout_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.bindingCount = uint32_t(std::size(bindings));
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_descriptor_set_layout) != VK_SUCCESS) {
            m_diagnostic = "Vulkan descriptor-layout creation failed for infill compute.";
            return;
        }

        VkPipelineLayoutCreateInfo pipeline_layout_info { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &m_descriptor_set_layout;
        if (vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
            m_diagnostic = "Vulkan pipeline-layout creation failed for infill compute.";
            return;
        }

        const std::vector<uint32_t> shader_words = load_intersection_shader();
        if (shader_words.empty()) {
            m_diagnostic = "The Vulkan infill shader is missing from the installed resources.";
            return;
        }
        VkShaderModuleCreateInfo shader_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shader_info.codeSize = shader_words.size() * sizeof(uint32_t);
        shader_info.pCode = shader_words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &shader_info, nullptr, &shader) != VK_SUCCESS) {
            m_diagnostic = "Vulkan shader-module creation failed for infill compute.";
            return;
        }
        VkPipelineShaderStageCreateInfo stage_info { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline_info.stage = stage_info;
        pipeline_info.layout = m_pipeline_layout;
        const VkResult pipeline_result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        if (pipeline_result != VK_SUCCESS) {
            m_diagnostic = "Vulkan compute-pipeline creation failed for infill intersections.";
            return;
        }

        VkCommandPoolCreateInfo command_pool_info { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        command_pool_info.queueFamilyIndex = m_queue_family;
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(m_device, &command_pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
            m_diagnostic = "Vulkan command-pool creation failed for infill compute.";
            return;
        }
        const VkDescriptorPoolSize pool_size { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
        VkDescriptorPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS) {
            m_diagnostic = "Vulkan descriptor-pool creation failed for infill compute.";
            return;
        }

        m_ready = true;
        m_diagnostic = "Vulkan exact vertical-intersection compute is ready.";
    }

    VulkanVerticalIntersectionBatch dispatch_locked(const std::vector<VulkanVerticalIntersectionRequest>& requests)
    {
        VulkanVerticalIntersectionBatch batch;
        std::vector<PackedVerticalIntersectionRequest> input;
        input.reserve(requests.size());
        for (const VulkanVerticalIntersectionRequest& request : requests)
            input.push_back({ request.segment.a.x, request.segment.a.y, request.segment.b.x, request.segment.b.y,
                              request.scan_x, request.stable_id });
        std::vector<PackedVerticalIntersectionResult> output(input.size());

        const VkDeviceSize input_size = VkDeviceSize(input.size() * sizeof(input.front()));
        const VkDeviceSize output_size = VkDeviceSize(output.size() * sizeof(output.front()));
        VkBuffer input_buffer = VK_NULL_HANDLE;
        VkBuffer output_buffer = VK_NULL_HANDLE;
        VkDeviceMemory input_memory = VK_NULL_HANDLE;
        VkDeviceMemory output_memory = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        auto cleanup = [&] {
            if (command_buffer != VK_NULL_HANDLE)
                vkFreeCommandBuffers(m_device, m_command_pool, 1, &command_buffer);
            if (input_memory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, input_memory, nullptr);
            if (output_memory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, output_memory, nullptr);
            if (input_buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, input_buffer, nullptr);
            if (output_buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, output_buffer, nullptr);
        };
        auto fail = [&](const char* message) {
            cleanup();
            batch.diagnostic = message;
            return batch;
        };

        if (!create_storage_buffer(m_physical_device, m_device, input_size, input_buffer, input_memory) ||
            !create_storage_buffer(m_physical_device, m_device, output_size, output_buffer, output_memory))
            return fail("Vulkan could not allocate host-visible infill-intersection buffers.");
        void* mapped = nullptr;
        if (vkMapMemory(m_device, input_memory, 0, input_size, 0, &mapped) != VK_SUCCESS)
            return fail("Vulkan could not map the infill-intersection input buffer.");
        std::memcpy(mapped, input.data(), size_t(input_size));
        vkUnmapMemory(m_device, input_memory);

        vkResetDescriptorPool(m_device, m_descriptor_pool, 0);
        VkDescriptorSetAllocateInfo allocation_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocation_info.descriptorPool = m_descriptor_pool;
        allocation_info.descriptorSetCount = 1;
        allocation_info.pSetLayouts = &m_descriptor_set_layout;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &allocation_info, &descriptor_set) != VK_SUCCESS)
            return fail("Vulkan could not allocate the infill-intersection descriptor set.");
        const VkDescriptorBufferInfo input_info { input_buffer, 0, input_size };
        const VkDescriptorBufferInfo output_info { output_buffer, 0, output_size };
        const VkWriteDescriptorSet writes[] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &input_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output_info, nullptr }
        };
        vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);

        VkCommandBufferAllocateInfo command_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_info.commandPool = m_command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &command_info, &command_buffer) != VK_SUCCESS)
            return fail("Vulkan could not allocate the infill-intersection command buffer.");
        VkCommandBufferBeginInfo begin_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS)
            return fail("Vulkan could not begin the infill-intersection command buffer.");
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        vkCmdDispatch(command_buffer, uint32_t((input.size() + 63) / 64), 1, 1);
        if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS)
            return fail("Vulkan could not end the infill-intersection command buffer.");
        const VkSubmitInfo submit_info { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command_buffer, 0, nullptr };
        if (vkQueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(m_queue) != VK_SUCCESS)
            return fail("Vulkan could not complete the infill-intersection dispatch.");

        if (vkMapMemory(m_device, output_memory, 0, output_size, 0, &mapped) != VK_SUCCESS)
            return fail("Vulkan could not map the infill-intersection output buffer.");
        std::memcpy(output.data(), mapped, size_t(output_size));
        vkUnmapMemory(m_device, output_memory);
        cleanup();

        batch.dispatched = true;
        batch.diagnostic = "Vulkan exact vertical-intersection dispatch completed.";
        batch.intersections.reserve(output.size());
        for (const PackedVerticalIntersectionResult& result : output)
            batch.intersections.push_back({ result.numerator, result.denominator, result.stable_id, result.valid != 0 });
        return batch;
    }

    std::once_flag m_initialize_once;
    std::mutex     m_dispatch_mutex;
    VkInstance     m_instance { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice       m_device { VK_NULL_HANDLE };
    VkQueue        m_queue { VK_NULL_HANDLE };
    uint32_t       m_queue_family { UINT32_MAX };
    VkDescriptorSetLayout m_descriptor_set_layout { VK_NULL_HANDLE };
    VkPipelineLayout m_pipeline_layout { VK_NULL_HANDLE };
    VkPipeline     m_pipeline { VK_NULL_HANDLE };
    VkCommandPool  m_command_pool { VK_NULL_HANDLE };
    VkDescriptorPool m_descriptor_pool { VK_NULL_HANDLE };
    bool           m_ready { false };
    std::string    m_diagnostic;
};

VulkanIntersectionContext& vulkan_intersection_context()
{
    static VulkanIntersectionContext context;
    return context;
}

} // namespace
#endif

VulkanSlicerCapabilities VulkanSlicerBackend::query_capabilities()
{
    VulkanSlicerCapabilities capabilities;

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    capabilities.compiled_with_vulkan = true;

    VkApplicationInfo application_info { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application_info.pApplicationName = "OrcaVulkanSlicer";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "OrcaSlicer";
    application_info.engineVersion = 1;
    application_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance_info.pApplicationInfo = &application_info;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult create_result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (create_result != VK_SUCCESS) {
        capabilities.diagnostic = "Vulkan instance creation failed (" + std::to_string(create_result) + ")";
        return capabilities;
    }

    capabilities.loader_available = true;
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
        vkDestroyInstance(instance, nullptr);
        capabilities.diagnostic = "No Vulkan compute device is available";
        return capabilities;
    }

    std::vector<VkPhysicalDevice> physical_devices(device_count);
    if (vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data()) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        capabilities.diagnostic = "Vulkan device enumeration failed";
        return capabilities;
    }

    capabilities.devices.reserve(physical_devices.size());
    for (VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceSubgroupProperties subgroup_properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 properties2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &subgroup_properties;
        VkPhysicalDeviceFeatures features {};
        vkGetPhysicalDeviceProperties2(physical_device, &properties2);
        vkGetPhysicalDeviceFeatures(physical_device, &features);

        VulkanDeviceInfo info;
        info.name = properties2.properties.deviceName;
        info.vendor_id = properties2.properties.vendorID;
        info.device_id = properties2.properties.deviceID;
        info.discrete = properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        info.shader_int64 = features.shaderInt64 == VK_TRUE;
        info.subgroup_operations = subgroup_properties.supportedStages != 0;

        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        if (queue_family_count != 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());
            info.compute_queue = std::any_of(queue_families.begin(), queue_families.end(), [](const VkQueueFamilyProperties& family) {
                return (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && family.queueCount != 0;
            });
        }
        capabilities.devices.emplace_back(std::move(info));
    }
    vkDestroyInstance(instance, nullptr);

    std::stable_sort(capabilities.devices.begin(), capabilities.devices.end(), [](const VulkanDeviceInfo& lhs, const VulkanDeviceInfo& rhs) {
        if (lhs.discrete != rhs.discrete)
            return lhs.discrete > rhs.discrete;
        if (lhs.shader_int64 != rhs.shader_int64)
            return lhs.shader_int64 > rhs.shader_int64;
        if (lhs.vendor_id != rhs.vendor_id)
            return lhs.vendor_id < rhs.vendor_id;
        return lhs.device_id < rhs.device_id;
    });

    std::ostringstream diagnostic;
    diagnostic << "Detected " << capabilities.devices.size() << " Vulkan device(s); "
               << "only compute-queue and shaderInt64 devices are eligible for exact tiled geometry.";
    capabilities.diagnostic = diagnostic.str();
#else
    capabilities.diagnostic = "Vulkan slicer was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.";
#endif

    return capabilities;
}

VulkanVerticalIntersectionBatch VulkanSlicerBackend::dispatch_vertical_intersections(
    const std::vector<VulkanVerticalIntersectionRequest>& requests)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    return vulkan_intersection_context().dispatch(requests);
#else
    VulkanVerticalIntersectionBatch batch;
    batch.diagnostic = "Vulkan infill compute was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.";
    return batch;
#endif
}

} // namespace Slic3r::Gpu
