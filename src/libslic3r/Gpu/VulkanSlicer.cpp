#include "VulkanSlicer.hpp"

#include "Utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
#include <vulkan/vulkan.h>
#endif

namespace Slic3r::Gpu {

namespace {

constexpr uint32_t kDefaultWorkgroupSize = 128;
constexpr size_t   kNvidiaPascalStagingRequestCapacity = 16 * 1024;
constexpr size_t   kNvidiaRtxStagingRequestCapacity = 64 * 1024;
constexpr size_t   kGenericStagingRequestCapacity = 4 * 1024;
// Host-visible input/output staging stays resident only during a slice and is
// then returned to Vulkan. Keeping the per-dispatch maximum bounded prevents
// one unusually dense region from needing a multi-gigabyte allocation.
constexpr size_t   kMaximumReusableStagingRequestCapacity = 256 * 1024;

// The GUI changes this flag through VulkanSlicerBackend. Keeping it here
// avoids coupling the slicing engine to GUI/AppConfig headers.
std::atomic_bool g_compute_enabled { true };

class RuntimeStatsRegistry {
public:
    void set_backend(std::string device, std::string profile, std::string diagnostic,
                     uint32_t configured_workgroup_size, uint32_t maximum_workgroup_size,
                     size_t reusable_staging_capacity)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.selected_device = std::move(device);
        m_stats.execution_profile = std::move(profile);
        m_stats.last_diagnostic = std::move(diagnostic);
        m_stats.configured_workgroup_size = configured_workgroup_size;
        m_stats.maximum_workgroup_size = maximum_workgroup_size;
        m_stats.reusable_staging_capacity = reusable_staging_capacity;
        m_stats.current_operation = m_stats.selected_device.empty() ?
            "Vulkan unavailable" : "Ready for infill/support scan conversion";
    }

    void set_preferred_intersection_batch(size_t request_count)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.preferred_intersection_batch = request_count;
    }

    void begin_slice()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.dispatch_calls               = 0;
        m_stats.queue_submissions            = 0;
        m_stats.submitted_intersections      = 0;
        m_stats.accepted_gpu_intersections   = 0;
        m_stats.cpu_validation_checks        = 0;
        m_stats.validation_failures          = 0;
        m_stats.skipped_small_workloads      = 0;
        m_stats.skipped_intersections        = 0;
        m_stats.last_gpu_ms                  = 0.0;
        m_stats.last_host_ms                 = 0.0;
        m_stats.total_gpu_ms                 = 0.0;
        m_stats.total_host_ms                = 0.0;
        m_stats.current_operation = m_stats.selected_device.empty() ?
            "Vulkan not initialized for this slice" :
            "No GPU batch has been submitted for this slice";
        m_stats.last_diagnostic = "GPU work counters reset for a new slicing session.";
    }

    void record_dispatch(size_t request_count, const VulkanVerticalIntersectionBatch& batch)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.dispatch_calls;
        m_stats.queue_submissions += batch.queue_submissions;
        m_stats.submitted_intersections += request_count;
        m_stats.last_gpu_ms = batch.gpu_elapsed_ms;
        m_stats.last_host_ms = batch.host_elapsed_ms;
        if (batch.gpu_elapsed_ms >= 0.0)
            m_stats.total_gpu_ms += batch.gpu_elapsed_ms;
        m_stats.total_host_ms += batch.host_elapsed_ms;
        m_stats.current_operation = batch.dispatched ?
            "Exact infill/support edge intersections" : "CPU fallback after Vulkan dispatch failure";
        m_stats.last_diagnostic = batch.diagnostic;
    }

    void record_skipped(size_t request_count)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.skipped_small_workloads;
        m_stats.skipped_intersections += request_count;
        m_stats.current_operation = "CPU fallback for a small intersection batch";
        m_stats.last_diagnostic = "Vulkan skipped a small vertical-intersection workload.";
    }

    void record_usage(size_t accepted_gpu_results, size_t cpu_validation_checks,
                      bool validation_failed, const std::string& diagnostic)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.accepted_gpu_intersections += accepted_gpu_results;
        m_stats.cpu_validation_checks += cpu_validation_checks;
        if (validation_failed)
            ++m_stats.validation_failures;
        m_stats.last_diagnostic = diagnostic;
    }

    void record_tree_contour_broad_phase(size_t request_count, size_t edge_count,
                                         const std::string& diagnostic)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.dispatch_calls;
        ++m_stats.queue_submissions;
        m_stats.current_operation = "Tree support contour broad phase (CPU exact confirmation)";
        m_stats.last_diagnostic = diagnostic + " (" + std::to_string(request_count) +
            " branches, " + std::to_string(edge_count) + " contour edges).";
    }

    VulkanSlicerRuntimeStats snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        VulkanSlicerRuntimeStats snapshot = m_stats;
        snapshot.validation_mode = validation_mode_name();
        return snapshot;
    }

private:
    static std::string validation_mode_name();

    mutable std::mutex          m_mutex;
    VulkanSlicerRuntimeStats    m_stats;
};

VulkanIntersectionValidationMode configured_validation_mode()
{
    static const VulkanIntersectionValidationMode mode = [] {
        const char* value = std::getenv("ORCA_VULKAN_SLICER_VALIDATION");
        if (value != nullptr) {
            std::string normalized(value);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char character) { return char(std::tolower(character)); });
            if (normalized == "strict")
                return VulkanIntersectionValidationMode::Strict;
        }
        // Exact fixed-point arithmetic is qualified at Vulkan initialization;
        // sampled checks avoid recreating that same arithmetic on the CPU for
        // every candidate intersection.
        return VulkanIntersectionValidationMode::Sampled;
    }();
    return mode;
}

std::string RuntimeStatsRegistry::validation_mode_name()
{
    return configured_validation_mode() == VulkanIntersectionValidationMode::Strict ?
        "strict CPU reference for every GPU result" :
        "sampled CPU reference after GPU qualification";
}

RuntimeStatsRegistry& runtime_stats_registry()
{
    static RuntimeStatsRegistry registry;
    return registry;
}

bool is_gtx_1060(const std::string& name, uint32_t vendor_id)
{
    return vendor_id == 0x10de && name.find("GTX 1060") != std::string::npos;
}

bool is_nvidia_rtx(const std::string& name, uint32_t vendor_id)
{
    return vendor_id == 0x10de && name.find("RTX") != std::string::npos;
}

uint32_t choose_workgroup_size(uint32_t maximum_invocations, uint32_t maximum_size_x,
                               bool gtx_1060, bool nvidia_rtx)
{
    const uint32_t hard_limit = std::min(maximum_invocations, maximum_size_x);
    // A workgroup does not become faster merely by using the API maximum.
    // 128 threads is a good Pascal occupancy target; 256 gives the current
    // RTX device enough warps to hide integer-arithmetic latency without
    // consuming the 1024-thread maximum in one group.
    uint32_t preferred = nvidia_rtx ? 256 : (gtx_1060 ? 128 : kDefaultWorkgroupSize);
    while (preferred > hard_limit && preferred > 1)
        preferred /= 2;
    return std::max(1u, preferred);
}

} // namespace

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

struct alignas(8) PackedTreeContourRequest {
    int64_t  ax;
    int64_t  ay;
    int64_t  bx;
    int64_t  by;
    uint64_t stable_id;
};
static_assert(sizeof(PackedTreeContourRequest) == 40);

struct alignas(8) PackedTreeContourEdge {
    int64_t ax;
    int64_t ay;
    int64_t bx;
    int64_t by;
};
static_assert(sizeof(PackedTreeContourEdge) == 32);

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

std::vector<uint32_t> load_intersection_shader(uint32_t workgroup_size)
{
    const std::string filename = "perimeter_infill_candidates_" +
        std::to_string(workgroup_size) + ".spv";
    std::vector<std::string> paths { Slic3r::resources_dir() + "/vulkan/" + filename };
#ifdef SLIC3R_VULKAN_BUILD_SHADER_DIR
    // The Windows/macOS build tree links resources/ to the source checkout.
    // The generated module is deliberately not written there, so use the
    // CMake binary directory only when the packaged resource is unavailable.
    paths.emplace_back(std::string(SLIC3R_VULKAN_BUILD_SHADER_DIR) + "/" + filename);
#endif
    for (const std::string& path : paths) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            continue;
        const std::streamsize byte_count = stream.tellg();
        if (byte_count <= 0 || (byte_count % std::streamsize(sizeof(uint32_t))) != 0)
            continue;
        std::vector<uint32_t> words(size_t(byte_count) / sizeof(uint32_t));
        stream.seekg(0);
        if (stream.read(reinterpret_cast<char*>(words.data()), byte_count))
            return words;
    }
    return {};
}

std::vector<uint32_t> load_tree_contour_shader()
{
    const std::string filename = "tree_support_contour_candidates.spv";
    std::vector<std::string> paths { Slic3r::resources_dir() + "/vulkan/" + filename };
#ifdef SLIC3R_VULKAN_BUILD_SHADER_DIR
    paths.emplace_back(std::string(SLIC3R_VULKAN_BUILD_SHADER_DIR) + "/" + filename);
#endif
    for (const std::string& path : paths) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            continue;
        const std::streamsize byte_count = stream.tellg();
        if (byte_count <= 0 || (byte_count % std::streamsize(sizeof(uint32_t))) != 0)
            continue;
        std::vector<uint32_t> words(size_t(byte_count) / sizeof(uint32_t));
        stream.seekg(0);
        if (stream.read(reinterpret_cast<char*>(words.data()), byte_count))
            return words;
    }
    return {};
}

class VulkanIntersectionContext {
public:
    ~VulkanIntersectionContext()
    {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            destroy_tree_staging_buffers();
            destroy_staging_buffers();
            if (m_tree_descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_device, m_tree_descriptor_pool, nullptr);
            if (m_dispatch_fence != VK_NULL_HANDLE)
                vkDestroyFence(m_device, m_dispatch_fence, nullptr);
            if (m_timestamp_query_pool != VK_NULL_HANDLE)
                vkDestroyQueryPool(m_device, m_timestamp_query_pool, nullptr);
            if (m_descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
            if (m_command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(m_device, m_command_pool, nullptr);
            if (m_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_device, m_pipeline, nullptr);
            if (m_tree_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_device, m_tree_pipeline, nullptr);
            if (m_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
            if (m_tree_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_device, m_tree_pipeline_layout, nullptr);
            if (m_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_device, m_descriptor_set_layout, nullptr);
            if (m_tree_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_device, m_tree_descriptor_set_layout, nullptr);
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

        if (!prepare_for_slicing()) {
            batch.diagnostic = m_diagnostic;
            return batch;
        }

        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        return dispatch_locked(requests);
    }

    VulkanTreeContourBatch dispatch_tree_contours(
        const std::vector<VulkanTreeContourRequest>& requests,
        const std::vector<Segment>& contour_edges)
    {
        VulkanTreeContourBatch batch;
        constexpr size_t minimum_pair_count = 16 * 1024;
        if (requests.empty() || contour_edges.empty()) {
            batch.diagnostic = "Tree contour broad phase has no branch or contour segments.";
            return batch;
        }
        if (requests.size() > std::numeric_limits<size_t>::max() / contour_edges.size() ||
            requests.size() * contour_edges.size() < minimum_pair_count) {
            batch.diagnostic = "Tree contour broad phase retained on CPU for a small workload.";
            return batch;
        }
        if (!prepare_for_slicing()) {
            batch.diagnostic = m_diagnostic;
            return batch;
        }
        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        return dispatch_tree_contours_locked(requests, contour_edges);
    }

    bool prepare_for_slicing()
    {
        std::call_once(m_initialize_once, [this] { this->initialize(); });
        if (!m_ready) {
            if (m_diagnostic.empty())
                m_diagnostic = "Vulkan infill compute did not finish initialization.";
            runtime_stats_registry().set_backend(m_selected_device, m_execution_profile, m_diagnostic,
                                                 m_workgroup_size, m_maximum_workgroup_size,
                                                 m_staging_request_capacity);
        }
        return m_ready;
    }

    bool should_dispatch_vertical_intersections(size_t request_count) const
    {
        return request_count >= (m_ready ? m_preferred_intersection_batch : kDefaultPreferredIntersectionBatch);
    }

    void release_unused_staging_memory()
    {
        // Slicing is synchronous at this boundary, but waiting here also keeps
        // this safe if a future caller releases memory after an asynchronous
        // submission. The pipeline stays resident; only host-visible request
        // and result buffers are returned to the Vulkan allocator.
        if (!m_ready || m_device == VK_NULL_HANDLE)
            return;
        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        if (m_staging_request_capacity == 0 && m_tree_request_capacity == 0)
            return;
        if (vkDeviceWaitIdle(m_device) != VK_SUCCESS)
            return;
        destroy_staging_buffers();
        destroy_tree_staging_buffers();
        runtime_stats_registry().set_backend(
            m_selected_device, m_execution_profile,
            "Released unused Vulkan staging buffers after slicing.",
            m_workgroup_size, m_maximum_workgroup_size, m_staging_request_capacity);
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

        int64_t best_score = -1;
        VkPhysicalDeviceProperties selected_properties {};
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
            VkPhysicalDeviceMemoryProperties memory_properties {};
            vkGetPhysicalDeviceMemoryProperties(candidate, &memory_properties);
            uint64_t device_local_bytes = 0;
            for (uint32_t heap = 0; heap < memory_properties.memoryHeapCount; ++heap) {
                if ((memory_properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                    device_local_bytes += memory_properties.memoryHeaps[heap].size;
            }
            for (uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0 || families[family].queueCount == 0)
                    continue;
                // Prefer the strongest eligible physical device instead of
                // assuming enumeration order. Discrete GPUs dominate, then
                // device-local memory and compute-dispatch limits break ties.
                const int64_t score =
                    (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? int64_t(1) << 60 : 0) +
                    int64_t(std::min<uint64_t>(device_local_bytes / (1024 * 1024), (int64_t(1) << 40) - 1)) * 1024 +
                    properties.limits.maxComputeWorkGroupInvocations;
                if (score > best_score) {
                    best_score = score;
                    m_physical_device = candidate;
                    m_queue_family = family;
                    selected_properties = properties;
                }
            }
        }
        if (m_physical_device == VK_NULL_HANDLE) {
            m_diagnostic = "No Vulkan compute queue with shaderInt64 is available for exact infill intersections.";
            return;
        }

        m_selected_device = selected_properties.deviceName;
        m_is_gtx_1060 = is_gtx_1060(m_selected_device, selected_properties.vendorID);
        m_is_nvidia_rtx = is_nvidia_rtx(m_selected_device, selected_properties.vendorID);
        m_maximum_workgroup_size = std::min(selected_properties.limits.maxComputeWorkGroupInvocations,
                                            selected_properties.limits.maxComputeWorkGroupSize[0]);
        m_workgroup_size = choose_workgroup_size(selected_properties.limits.maxComputeWorkGroupInvocations,
                                                  selected_properties.limits.maxComputeWorkGroupSize[0],
                                                  m_is_gtx_1060, m_is_nvidia_rtx);
        m_initial_staging_request_capacity = m_is_nvidia_rtx ? kNvidiaRtxStagingRequestCapacity :
            (m_is_gtx_1060 ? kNvidiaPascalStagingRequestCapacity : kGenericStagingRequestCapacity);
        m_timestamp_period_ns = double(selected_properties.limits.timestampPeriod);
        m_compute_timestamps_available =
            selected_properties.limits.timestampComputeAndGraphics == VK_TRUE && m_timestamp_period_ns > 0.0;
        const uint64_t maximum_request_size = std::min(
            uint64_t(selected_properties.limits.maxStorageBufferRange) / sizeof(PackedVerticalIntersectionRequest),
            uint64_t(selected_properties.limits.maxStorageBufferRange) / sizeof(PackedVerticalIntersectionResult));
        m_storage_buffer_request_limit = size_t(std::min<uint64_t>(maximum_request_size, std::numeric_limits<uint32_t>::max()));
        m_max_compute_workgroup_count_x = selected_properties.limits.maxComputeWorkGroupCount[0];
        update_submission_request_limit();
        if (m_max_requests_per_submission == 0) {
            m_diagnostic = "The selected Vulkan device has no usable storage-buffer range for infill compute.";
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

        VkDescriptorSetAllocateInfo descriptor_allocation_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        descriptor_allocation_info.descriptorPool = m_descriptor_pool;
        descriptor_allocation_info.descriptorSetCount = 1;
        descriptor_allocation_info.pSetLayouts = &m_descriptor_set_layout;
        if (vkAllocateDescriptorSets(m_device, &descriptor_allocation_info, &m_descriptor_set) != VK_SUCCESS) {
            m_diagnostic = "Vulkan could not allocate the reusable infill descriptor set.";
            return;
        }

        VkCommandBufferAllocateInfo command_allocation_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_allocation_info.commandPool = m_command_pool;
        command_allocation_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocation_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &command_allocation_info, &m_command_buffer) != VK_SUCCESS) {
            m_diagnostic = "Vulkan could not allocate the reusable infill command buffer.";
            return;
        }

        // Both geometry kernels synchronously consume their host-visible
        // result buffer. A fence waits only for this submission, unlike
        // vkQueueWaitIdle(), which drains every operation in the queue and
        // adds avoidable driver latency to dense infill/wall batches.
        VkFenceCreateInfo dispatch_fence_info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(m_device, &dispatch_fence_info, nullptr, &m_dispatch_fence) != VK_SUCCESS) {
            m_diagnostic = "Vulkan could not allocate the reusable compute dispatch fence.";
            return;
        }

        if (m_compute_timestamps_available) {
            VkQueryPoolCreateInfo query_pool_info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_pool_info.queryCount = 2;
            if (vkCreateQueryPool(m_device, &query_pool_info, nullptr, &m_timestamp_query_pool) != VK_SUCCESS)
                m_compute_timestamps_available = false;
        }

        if (!autotune_workgroup_size())
            return;
        update_execution_profile();

        m_ready = true;
        if (!run_exact_qualification()) {
            m_ready = false;
            if (m_diagnostic.empty())
                m_diagnostic = "Vulkan exact vertical-intersection qualification failed.";
            runtime_stats_registry().set_backend(m_selected_device, m_execution_profile, m_diagnostic,
                                                 m_workgroup_size, m_maximum_workgroup_size,
                                                 m_staging_request_capacity);
            return;
        }
        calibrate_intersection_dispatch_policy();
        update_execution_profile();
        runtime_stats_registry().set_preferred_intersection_batch(m_preferred_intersection_batch);
        m_diagnostic = "Vulkan exact vertical-intersection compute is qualified and ready on " + m_selected_device + ".";
        runtime_stats_registry().set_backend(m_selected_device, m_execution_profile, m_diagnostic,
                                             m_workgroup_size, m_maximum_workgroup_size,
                                             m_staging_request_capacity);
    }

    void update_submission_request_limit()
    {
        const uint64_t maximum_workgroup_requests = uint64_t(m_max_compute_workgroup_count_x) * m_workgroup_size;
        m_max_requests_per_submission = size_t(std::min<uint64_t>(m_storage_buffer_request_limit,
                                                                    maximum_workgroup_requests));
        m_max_requests_per_submission = std::min(m_max_requests_per_submission,
                                                 kMaximumReusableStagingRequestCapacity);
    }

    void update_execution_profile()
    {
        std::ostringstream profile;
        profile << (m_is_nvidia_rtx ? "NVIDIA RTX" : (m_is_gtx_1060 ? "NVIDIA GTX 1060 / Pascal" : "Generic Vulkan"))
                << ": " << m_workgroup_size << " threads/group (autotuned; device maximum "
                << m_maximum_workgroup_size << "), " << m_initial_staging_request_capacity
                << " reusable requests, CPU/GPU batch >= "
                << m_preferred_intersection_batch;
        m_execution_profile = profile.str();
    }

    void calibrate_intersection_dispatch_policy()
    {
        const char* policy = std::getenv("ORCA_VULKAN_SLICER_POLICY");
        if (policy != nullptr && std::string(policy) == "cpu") {
            m_preferred_intersection_batch = m_max_requests_per_submission + 1;
            return;
        }
        if (policy != nullptr && std::string(policy) == "gpu") {
            m_preferred_intersection_batch = kDefaultPreferredIntersectionBatch;
            return;
        }

        const size_t small_count = std::min<size_t>(4096, m_max_requests_per_submission);
        const size_t large_count = std::min<size_t>(32768, m_max_requests_per_submission);
        if (small_count < 256 || large_count <= small_count) {
            m_preferred_intersection_batch = kDefaultPreferredIntersectionBatch;
            return;
        }

        std::vector<VulkanVerticalIntersectionRequest> requests;
        requests.reserve(large_count);
        for (size_t index = 0; index < large_count; ++index) {
            const int64_t left = -900000 + int64_t(index % 509) * 73;
            const int64_t right = left + 3001 + int64_t(index % 127);
            requests.push_back({ { { left, -700000 + int64_t(index) * 13 },
                                   { right, 900000 - int64_t(index) * 17 } },
                                 left + 1 + int64_t(index % (right - left - 1)), uint64_t(index) });
        }
        const std::vector<VulkanVerticalIntersectionRequest> small(requests.begin(), requests.begin() + small_count);
        const VulkanVerticalIntersectionBatch gpu_small = dispatch_locked(small);
        const VulkanVerticalIntersectionBatch gpu_large = dispatch_locked(requests);
        if (!gpu_small.dispatched || !gpu_large.dispatched) {
            m_preferred_intersection_batch = kDefaultPreferredIntersectionBatch;
            return;
        }

        constexpr int cpu_repetitions = 8;
        volatile int64_t checksum = 0;
        const auto cpu_start = std::chrono::steady_clock::now();
        for (int repetition = 0; repetition < cpu_repetitions; ++repetition) {
            for (const VulkanVerticalIntersectionRequest& request : requests) {
                const int64_t denominator = request.segment.b.x > request.segment.a.x ?
                    request.segment.b.x - request.segment.a.x : request.segment.a.x - request.segment.b.x;
                const int64_t t_numerator = request.segment.b.x > request.segment.a.x ?
                    request.scan_x - request.segment.a.x : request.segment.a.x - request.scan_x;
                checksum += t_numerator * (request.segment.b.y - request.segment.a.y) +
                    request.segment.a.y * denominator;
            }
        }
        const double cpu_per_request_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpu_start).count() / (cpu_repetitions * large_count);
        (void)checksum;
        const double gpu_per_request_ms = std::max(0.0, gpu_large.host_elapsed_ms - gpu_small.host_elapsed_ms) /
            double(large_count - small_count);
        const double gpu_fixed_overhead_ms = std::max(0.0, gpu_small.host_elapsed_ms - gpu_per_request_ms * small_count);
        // Packing and CPU topology still remain after this GPU result arrives.
        // Only a conservative fraction of the arithmetic benchmark is treated
        // as recoverable CPU work when choosing the crossover point.
        const double recoverable_cpu_per_request_ms = cpu_per_request_ms * 0.35;
        if (gpu_per_request_ms >= recoverable_cpu_per_request_ms) {
            m_preferred_intersection_batch = std::min<size_t>(65536, m_max_requests_per_submission);
            return;
        }
        const size_t crossover = size_t(std::ceil(gpu_fixed_overhead_ms /
            (recoverable_cpu_per_request_ms - gpu_per_request_ms)));
        m_preferred_intersection_batch = std::clamp<size_t>(crossover, kDefaultPreferredIntersectionBatch,
                                                              std::min<size_t>(65536, m_max_requests_per_submission));
    }

    bool create_compute_pipeline(uint32_t workgroup_size, VkPipeline& pipeline)
    {
        const std::vector<uint32_t> shader_words = load_intersection_shader(workgroup_size);
        if (shader_words.empty())
            return false;
        VkShaderModuleCreateInfo shader_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shader_info.codeSize = shader_words.size() * sizeof(uint32_t);
        shader_info.pCode = shader_words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &shader_info, nullptr, &shader) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage_info { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline_info.stage = stage_info;
        pipeline_info.layout = m_pipeline_layout;
        const VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        return result == VK_SUCCESS;
    }

    bool autotune_workgroup_size()
    {
        // Measure the actual hardware instead of assuming that an RTX name or
        // the API's 1024-thread limit is optimal. Integer intersection work is
        // latency-sensitive; several resident 64-256-thread groups usually
        // beat one maximum-sized group.
        std::vector<uint32_t> candidates { m_workgroup_size };
        for (const uint32_t candidate : { 64u, 128u, 256u, 512u, 1024u }) {
            if (candidate <= m_maximum_workgroup_size)
                candidates.emplace_back(candidate);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        std::vector<std::pair<uint32_t, VkPipeline>> pipelines;
        pipelines.reserve(candidates.size());
        for (const uint32_t candidate : candidates) {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (create_compute_pipeline(candidate, pipeline))
                pipelines.emplace_back(candidate, pipeline);
        }
        if (pipelines.empty()) {
            m_diagnostic = "Vulkan could not create any specialized infill compute pipeline.";
            return false;
        }

        const size_t probe_count = std::min(m_max_requests_per_submission,
            std::max<size_t>(256, std::min(m_initial_staging_request_capacity,
                                            m_max_requests_per_submission)));
        std::vector<VulkanVerticalIntersectionRequest> probe_requests;
        probe_requests.reserve(probe_count);
        for (size_t index = 0; index < probe_count; ++index) {
            const int64_t left = -300000 + int64_t(index % 211) * 37;
            const int64_t right = left + 1009 + int64_t(index % 71);
            const bool reverse = (index & 1) != 0;
            probe_requests.push_back({
                { { reverse ? right : left, -500000 + int64_t(index) * 7 },
                  { reverse ? left : right, 600000 - int64_t(index) * 11 } },
                left + 1 + int64_t(index % (right - left - 1)), uint64_t(index)
            });
        }

        VkPipeline selected_pipeline = VK_NULL_HANDLE;
        uint32_t selected_workgroup_size = m_workgroup_size;
        double best_elapsed_ms = std::numeric_limits<double>::infinity();
        for (const auto& candidate : pipelines) {
            m_pipeline = candidate.second;
            m_workgroup_size = candidate.first;
            update_submission_request_limit();
            // One warm-up run lets shader/power state settle before the three
            // timestamped trials that decide the live group size.
            if (!dispatch_locked(probe_requests).dispatched)
                continue;
            double candidate_elapsed_ms = std::numeric_limits<double>::infinity();
            bool succeeded = true;
            for (int trial = 0; trial < 3; ++trial) {
                const VulkanVerticalIntersectionBatch batch = dispatch_locked(probe_requests);
                if (!batch.dispatched) {
                    succeeded = false;
                    break;
                }
                const double elapsed_ms = batch.gpu_elapsed_ms >= 0.0 ?
                    batch.gpu_elapsed_ms : batch.host_elapsed_ms;
                candidate_elapsed_ms = std::min(candidate_elapsed_ms, elapsed_ms);
            }
            if (succeeded && candidate_elapsed_ms < best_elapsed_ms) {
                best_elapsed_ms = candidate_elapsed_ms;
                selected_workgroup_size = candidate.first;
                selected_pipeline = candidate.second;
            }
        }

        if (selected_pipeline == VK_NULL_HANDLE) {
            for (const auto& candidate : pipelines)
                vkDestroyPipeline(m_device, candidate.second, nullptr);
            m_pipeline = VK_NULL_HANDLE;
            m_diagnostic = "Vulkan workgroup autotuning could not complete a compute dispatch.";
            return false;
        }
        for (const auto& candidate : pipelines) {
            if (candidate.second != selected_pipeline)
                vkDestroyPipeline(m_device, candidate.second, nullptr);
        }
        m_pipeline = selected_pipeline;
        m_workgroup_size = selected_workgroup_size;
        update_submission_request_limit();
        return true;
    }

    bool ensure_tree_pipeline()
    {
        if (m_tree_pipeline != VK_NULL_HANDLE)
            return true;
        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };
        VkDescriptorSetLayoutCreateInfo layout_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.bindingCount = uint32_t(std::size(bindings));
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_tree_descriptor_set_layout) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo pipeline_layout_info { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &m_tree_descriptor_set_layout;
        if (vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &m_tree_pipeline_layout) != VK_SUCCESS)
            return false;
        const std::vector<uint32_t> shader_words = load_tree_contour_shader();
        if (shader_words.empty())
            return false;
        VkShaderModuleCreateInfo shader_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shader_info.codeSize = shader_words.size() * sizeof(uint32_t);
        shader_info.pCode = shader_words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &shader_info, nullptr, &shader) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage_info { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline_info.stage = stage_info;
        pipeline_info.layout = m_tree_pipeline_layout;
        const VkResult pipeline_result = vkCreateComputePipelines(
            m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_tree_pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        if (pipeline_result != VK_SUCCESS)
            return false;
        const VkDescriptorPoolSize pool_size { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
        VkDescriptorPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_tree_descriptor_pool) != VK_SUCCESS)
            return false;
        VkDescriptorSetAllocateInfo descriptor_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        descriptor_info.descriptorPool = m_tree_descriptor_pool;
        descriptor_info.descriptorSetCount = 1;
        descriptor_info.pSetLayouts = &m_tree_descriptor_set_layout;
        if (vkAllocateDescriptorSets(m_device, &descriptor_info, &m_tree_descriptor_set) != VK_SUCCESS)
            return false;
        VkCommandBufferAllocateInfo command_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_info.commandPool = m_command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        return vkAllocateCommandBuffers(m_device, &command_info, &m_tree_command_buffer) == VK_SUCCESS;
    }

    void destroy_tree_staging_buffers()
    {
        const VkDeviceMemory memories[] { m_tree_request_memory, m_tree_edge_memory, m_tree_result_memory };
        const VkBuffer buffers[] { m_tree_request_buffer, m_tree_edge_buffer, m_tree_result_buffer };
        void* const mappings[] { m_tree_request_mapping, m_tree_edge_mapping, m_tree_result_mapping };
        for (size_t index = 0; index < std::size(memories); ++index) {
            if (mappings[index] != nullptr && memories[index] != VK_NULL_HANDLE)
                vkUnmapMemory(m_device, memories[index]);
            if (buffers[index] != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, buffers[index], nullptr);
            if (memories[index] != VK_NULL_HANDLE)
                vkFreeMemory(m_device, memories[index], nullptr);
        }
        m_tree_request_buffer = m_tree_edge_buffer = m_tree_result_buffer = VK_NULL_HANDLE;
        m_tree_request_memory = m_tree_edge_memory = m_tree_result_memory = VK_NULL_HANDLE;
        m_tree_request_mapping = m_tree_edge_mapping = m_tree_result_mapping = nullptr;
        m_tree_request_capacity = m_tree_edge_capacity = 0;
    }

    bool ensure_tree_staging_buffers(size_t request_count, size_t edge_count)
    {
        constexpr size_t maximum_capacity = 128 * 1024;
        if (request_count > maximum_capacity || edge_count > maximum_capacity ||
            request_count > m_max_compute_workgroup_count_x)
            return false;
        if (m_tree_request_capacity >= request_count && m_tree_edge_capacity >= edge_count &&
            m_tree_request_mapping != nullptr && m_tree_edge_mapping != nullptr &&
            m_tree_result_mapping != nullptr)
            return true;
        size_t request_capacity = 1024;
        while (request_capacity < request_count)
            request_capacity *= 2;
        size_t edge_capacity = 1024;
        while (edge_capacity < edge_count)
            edge_capacity *= 2;
        destroy_tree_staging_buffers();
        const VkDeviceSize request_size = VkDeviceSize(request_capacity * sizeof(PackedTreeContourRequest));
        const VkDeviceSize edge_size = VkDeviceSize(edge_capacity * sizeof(PackedTreeContourEdge));
        const VkDeviceSize result_size = VkDeviceSize(request_capacity * sizeof(uint32_t));
        if (!create_storage_buffer(m_physical_device, m_device, request_size, m_tree_request_buffer, m_tree_request_memory) ||
            !create_storage_buffer(m_physical_device, m_device, edge_size, m_tree_edge_buffer, m_tree_edge_memory) ||
            !create_storage_buffer(m_physical_device, m_device, result_size, m_tree_result_buffer, m_tree_result_memory)) {
            destroy_tree_staging_buffers();
            return false;
        }
        if (vkMapMemory(m_device, m_tree_request_memory, 0, request_size, 0, &m_tree_request_mapping) != VK_SUCCESS ||
            vkMapMemory(m_device, m_tree_edge_memory, 0, edge_size, 0, &m_tree_edge_mapping) != VK_SUCCESS ||
            vkMapMemory(m_device, m_tree_result_memory, 0, result_size, 0, &m_tree_result_mapping) != VK_SUCCESS) {
            destroy_tree_staging_buffers();
            return false;
        }
        m_tree_request_capacity = request_capacity;
        m_tree_edge_capacity = edge_capacity;
        return true;
    }

    void destroy_staging_buffers()
    {
        if (m_input_mapping != nullptr && m_input_memory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, m_input_memory);
        if (m_output_mapping != nullptr && m_output_memory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, m_output_memory);
        m_input_mapping = nullptr;
        m_output_mapping = nullptr;
        if (m_input_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, m_input_buffer, nullptr);
        if (m_output_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, m_output_buffer, nullptr);
        if (m_input_memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, m_input_memory, nullptr);
        if (m_output_memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, m_output_memory, nullptr);
        m_input_buffer = VK_NULL_HANDLE;
        m_output_buffer = VK_NULL_HANDLE;
        m_input_memory = VK_NULL_HANDLE;
        m_output_memory = VK_NULL_HANDLE;
        m_staging_request_capacity = 0;
    }

    bool ensure_staging_buffers(size_t request_count)
    {
        if (request_count > m_max_requests_per_submission)
            return false;
        if (m_staging_request_capacity >= request_count &&
            m_input_mapping != nullptr && m_output_mapping != nullptr)
            return true;

        size_t capacity = m_initial_staging_request_capacity;
        while (capacity < request_count && capacity <= m_max_requests_per_submission / 2)
            capacity *= 2;
        capacity = std::max(capacity, request_count);
        capacity = std::min(capacity, m_max_requests_per_submission);

        destroy_staging_buffers();
        const VkDeviceSize input_size = VkDeviceSize(capacity * sizeof(PackedVerticalIntersectionRequest));
        const VkDeviceSize output_size = VkDeviceSize(capacity * sizeof(PackedVerticalIntersectionResult));
        if (!create_storage_buffer(m_physical_device, m_device, input_size, m_input_buffer, m_input_memory) ||
            !create_storage_buffer(m_physical_device, m_device, output_size, m_output_buffer, m_output_memory)) {
            destroy_staging_buffers();
            return false;
        }
        if (vkMapMemory(m_device, m_input_memory, 0, input_size, 0, &m_input_mapping) != VK_SUCCESS ||
            vkMapMemory(m_device, m_output_memory, 0, output_size, 0, &m_output_mapping) != VK_SUCCESS) {
            destroy_staging_buffers();
            return false;
        }
        m_staging_request_capacity = capacity;
        return true;
    }

    bool run_exact_qualification()
    {
        // Qualification covers signs, reversed edges, non-zero origins and
        // high-but-safe fixed-point values before live geometry is accepted.
        std::vector<VulkanVerticalIntersectionRequest> requests;
        requests.reserve(2048);
        for (uint64_t index = 0; index < 2048; ++index) {
            const int64_t left = -500000 + int64_t(index % 97) * 101;
            const int64_t right = left + 1003 + int64_t(index % 89);
            const bool reverse = (index & 1) != 0;
            const int64_t ay = -750000 + int64_t(index) * 37;
            const int64_t by = 950000 - int64_t(index) * 53;
            const int64_t scan_x = left + 1 + int64_t(index % (right - left - 1));
            requests.push_back({
                { { reverse ? right : left, ay }, { reverse ? left : right, by } },
                scan_x, index
            });
        }

        const VulkanVerticalIntersectionBatch batch = dispatch_locked(requests);
        if (!batch.dispatched || batch.intersections.size() != requests.size()) {
            m_diagnostic = "Vulkan exact-integer qualification dispatch failed: " + batch.diagnostic;
            return false;
        }
        for (size_t index = 0; index < requests.size(); ++index) {
            const VulkanVerticalIntersectionRequest& request = requests[index];
            const int64_t denominator = request.segment.b.x > request.segment.a.x ?
                request.segment.b.x - request.segment.a.x : request.segment.a.x - request.segment.b.x;
            const int64_t t_numerator = request.segment.b.x > request.segment.a.x ?
                request.scan_x - request.segment.a.x : request.segment.a.x - request.scan_x;
            const int64_t numerator = t_numerator * (request.segment.b.y - request.segment.a.y) +
                request.segment.a.y * denominator;
            const VulkanVerticalIntersection& result = batch.intersections[index];
            if (!result.valid || result.stable_id != request.stable_id ||
                result.numerator != numerator || result.denominator != denominator) {
                m_diagnostic = "Vulkan exact-integer qualification mismatch at vector " + std::to_string(index) + ".";
                return false;
            }
        }
        return true;
    }

    VulkanTreeContourBatch dispatch_tree_contours_locked(
        const std::vector<VulkanTreeContourRequest>& requests,
        const std::vector<Segment>& contour_edges)
    {
        VulkanTreeContourBatch batch;
        auto fail = [&](const std::string& diagnostic) {
            batch.diagnostic = diagnostic;
            return batch;
        };
        if (!ensure_tree_pipeline())
            return fail("Vulkan tree contour pipeline is unavailable.");
        if (!ensure_tree_staging_buffers(requests.size(), contour_edges.size()))
            return fail("Vulkan tree contour workload exceeds its bounded staging capacity.");

        auto* packed_requests = static_cast<PackedTreeContourRequest*>(m_tree_request_mapping);
        for (size_t index = 0; index < requests.size(); ++index) {
            const VulkanTreeContourRequest& request = requests[index];
            packed_requests[index] = { request.segment.a.x, request.segment.a.y,
                                       request.segment.b.x, request.segment.b.y, request.stable_id };
        }
        auto* packed_edges = static_cast<PackedTreeContourEdge*>(m_tree_edge_mapping);
        for (size_t index = 0; index < contour_edges.size(); ++index) {
            const Segment& edge = contour_edges[index];
            packed_edges[index] = { edge.a.x, edge.a.y, edge.b.x, edge.b.y };
        }

        const VkDescriptorBufferInfo request_info {
            m_tree_request_buffer, 0, VkDeviceSize(requests.size() * sizeof(PackedTreeContourRequest)) };
        const VkDescriptorBufferInfo edge_info {
            m_tree_edge_buffer, 0, VkDeviceSize(contour_edges.size() * sizeof(PackedTreeContourEdge)) };
        const VkDescriptorBufferInfo result_info {
            m_tree_result_buffer, 0, VkDeviceSize(requests.size() * sizeof(uint32_t)) };
        const VkWriteDescriptorSet writes[] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tree_descriptor_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &request_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tree_descriptor_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &edge_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tree_descriptor_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &result_info, nullptr }
        };
        vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);
        if (vkResetCommandBuffer(m_tree_command_buffer, 0) != VK_SUCCESS)
            return fail("Vulkan could not reset the tree contour command buffer.");
        VkCommandBufferBeginInfo begin_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_tree_command_buffer, &begin_info) != VK_SUCCESS)
            return fail("Vulkan could not begin the tree contour command buffer.");
        vkCmdBindPipeline(m_tree_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tree_pipeline);
        vkCmdBindDescriptorSets(m_tree_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_tree_pipeline_layout, 0, 1, &m_tree_descriptor_set, 0, nullptr);
        vkCmdDispatch(m_tree_command_buffer, uint32_t(requests.size()), 1, 1);
        if (vkEndCommandBuffer(m_tree_command_buffer) != VK_SUCCESS)
            return fail("Vulkan could not end the tree contour command buffer.");
        const VkSubmitInfo submit_info {
            VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &m_tree_command_buffer, 0, nullptr };
        if (vkResetFences(m_device, 1, &m_dispatch_fence) != VK_SUCCESS ||
            vkQueueSubmit(m_queue, 1, &submit_info, m_dispatch_fence) != VK_SUCCESS ||
            vkWaitForFences(m_device, 1, &m_dispatch_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return fail("Vulkan could not complete the tree contour dispatch.");

        const auto* result = static_cast<const uint32_t*>(m_tree_result_mapping);
        batch.may_intersect.resize(requests.size());
        for (size_t index = 0; index < requests.size(); ++index)
            batch.may_intersect[index] = result[index] != 0 ? 1 : 0;
        batch.dispatched = true;
        batch.diagnostic = "Vulkan tree contour broad phase completed on " + m_selected_device + ".";
        runtime_stats_registry().record_tree_contour_broad_phase(
            requests.size(), contour_edges.size(), batch.diagnostic);
        return batch;
    }

    VulkanVerticalIntersectionBatch dispatch_locked(const std::vector<VulkanVerticalIntersectionRequest>& requests)
    {
        const auto host_start = std::chrono::steady_clock::now();
        VulkanVerticalIntersectionBatch batch;
        auto finish = [&] {
            batch.host_elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - host_start).count();
            return batch;
        };
        auto fail = [&](const std::string& message) {
            batch.diagnostic = message;
            return finish();
        };
        if (requests.size() > m_max_requests_per_submission) {
            std::ostringstream message;
            message << "Vulkan vertical-intersection workload of " << requests.size()
                    << " requests exceeds this device's reusable-buffer limit of "
                    << m_max_requests_per_submission << ".";
            return fail(message.str());
        }
        if (!ensure_staging_buffers(requests.size()))
            return fail("Vulkan could not prepare reusable host-visible infill-intersection buffers.");

        auto* input = static_cast<PackedVerticalIntersectionRequest*>(m_input_mapping);
        for (size_t index = 0; index < requests.size(); ++index) {
            const VulkanVerticalIntersectionRequest& request = requests[index];
            input[index] = { request.segment.a.x, request.segment.a.y, request.segment.b.x, request.segment.b.y,
                             request.scan_x, request.stable_id };
        }

        const VkDeviceSize input_size = VkDeviceSize(requests.size() * sizeof(PackedVerticalIntersectionRequest));
        const VkDeviceSize output_size = VkDeviceSize(requests.size() * sizeof(PackedVerticalIntersectionResult));
        const VkDescriptorBufferInfo input_info { m_input_buffer, 0, input_size };
        const VkDescriptorBufferInfo output_info { m_output_buffer, 0, output_size };
        const VkWriteDescriptorSet writes[] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_descriptor_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &input_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_descriptor_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output_info, nullptr }
        };
        vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);

        if (vkResetCommandBuffer(m_command_buffer, 0) != VK_SUCCESS)
            return fail("Vulkan could not reset the reusable infill command buffer.");
        VkCommandBufferBeginInfo begin_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_command_buffer, &begin_info) != VK_SUCCESS)
            return fail("Vulkan could not begin the reusable infill command buffer.");
        if (m_compute_timestamps_available)
            vkCmdResetQueryPool(m_command_buffer, m_timestamp_query_pool, 0, 2);
        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_descriptor_set, 0, nullptr);
        if (m_compute_timestamps_available)
            vkCmdWriteTimestamp(m_command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, 0);
        vkCmdDispatch(m_command_buffer, uint32_t((requests.size() + m_workgroup_size - 1) / m_workgroup_size), 1, 1);
        if (m_compute_timestamps_available)
            vkCmdWriteTimestamp(m_command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, 1);
        if (vkEndCommandBuffer(m_command_buffer) != VK_SUCCESS)
            return fail("Vulkan could not end the reusable infill command buffer.");
        const VkSubmitInfo submit_info { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &m_command_buffer, 0, nullptr };
        if (vkResetFences(m_device, 1, &m_dispatch_fence) != VK_SUCCESS ||
            vkQueueSubmit(m_queue, 1, &submit_info, m_dispatch_fence) != VK_SUCCESS ||
            vkWaitForFences(m_device, 1, &m_dispatch_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return fail("Vulkan could not complete the infill-intersection dispatch.");

        if (m_compute_timestamps_available) {
            uint64_t timestamps[2] {};
            if (vkGetQueryPoolResults(m_device, m_timestamp_query_pool, 0, 2, sizeof(timestamps), timestamps,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                batch.gpu_elapsed_ms = double(timestamps[1] - timestamps[0]) * m_timestamp_period_ns / 1'000'000.0;
        }

        const auto* output = static_cast<const PackedVerticalIntersectionResult*>(m_output_mapping);
        batch.intersections.reserve(requests.size());
        for (size_t index = 0; index < requests.size(); ++index) {
            const PackedVerticalIntersectionResult& result = output[index];
            batch.intersections.push_back({ result.numerator, result.denominator, result.stable_id, result.valid != 0 });
        }
        batch.dispatched = true;
        batch.queue_submissions = 1;
        batch.diagnostic = "Vulkan exact vertical-intersection dispatch completed on " + m_selected_device + ".";
        return finish();
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
    VkDescriptorSet m_descriptor_set { VK_NULL_HANDLE };
    VkCommandBuffer m_command_buffer { VK_NULL_HANDLE };
    VkDescriptorSetLayout m_tree_descriptor_set_layout { VK_NULL_HANDLE };
    VkPipelineLayout m_tree_pipeline_layout { VK_NULL_HANDLE };
    VkPipeline m_tree_pipeline { VK_NULL_HANDLE };
    VkDescriptorPool m_tree_descriptor_pool { VK_NULL_HANDLE };
    VkDescriptorSet m_tree_descriptor_set { VK_NULL_HANDLE };
    VkCommandBuffer m_tree_command_buffer { VK_NULL_HANDLE };
    VkFence         m_dispatch_fence { VK_NULL_HANDLE };
    VkQueryPool     m_timestamp_query_pool { VK_NULL_HANDLE };
    VkBuffer        m_input_buffer { VK_NULL_HANDLE };
    VkBuffer        m_output_buffer { VK_NULL_HANDLE };
    VkDeviceMemory  m_input_memory { VK_NULL_HANDLE };
    VkDeviceMemory  m_output_memory { VK_NULL_HANDLE };
    void*           m_input_mapping { nullptr };
    void*           m_output_mapping { nullptr };
    VkBuffer        m_tree_request_buffer { VK_NULL_HANDLE };
    VkBuffer        m_tree_edge_buffer { VK_NULL_HANDLE };
    VkBuffer        m_tree_result_buffer { VK_NULL_HANDLE };
    VkDeviceMemory  m_tree_request_memory { VK_NULL_HANDLE };
    VkDeviceMemory  m_tree_edge_memory { VK_NULL_HANDLE };
    VkDeviceMemory  m_tree_result_memory { VK_NULL_HANDLE };
    void*           m_tree_request_mapping { nullptr };
    void*           m_tree_edge_mapping { nullptr };
    void*           m_tree_result_mapping { nullptr };
    size_t          m_staging_request_capacity { 0 };
    size_t          m_tree_request_capacity { 0 };
    size_t          m_tree_edge_capacity { 0 };
    size_t          m_initial_staging_request_capacity { kGenericStagingRequestCapacity };
    size_t          m_preferred_intersection_batch { kDefaultPreferredIntersectionBatch };
    size_t          m_max_requests_per_submission { 0 };
    size_t          m_storage_buffer_request_limit { 0 };
    uint32_t        m_workgroup_size { kDefaultWorkgroupSize };
    uint32_t        m_maximum_workgroup_size { 0 };
    uint32_t        m_max_compute_workgroup_count_x { 0 };
    double          m_timestamp_period_ns { 0.0 };
    std::string     m_selected_device;
    std::string     m_execution_profile;
    bool            m_is_gtx_1060 { false };
    bool            m_is_nvidia_rtx { false };
    bool            m_compute_timestamps_available { false };
    bool           m_ready { false };
    std::string    m_diagnostic;

    static constexpr size_t kDefaultPreferredIntersectionBatch = 4096;
};

VulkanIntersectionContext& vulkan_intersection_context()
{
    // The Vulkan loader may already be torn down when C++ function-static
    // destructors run during wx/driver shutdown. Keep this small context alive
    // until Windows reclaims it with the process, instead of calling Vulkan
    // after the loader has unloaded. Runtime staging buffers are explicitly
    // released after every slice, so this is not a growing allocation.
    static auto* context = new VulkanIntersectionContext;
    return *context;
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
        info.max_workgroup_invocations = properties2.properties.limits.maxComputeWorkGroupInvocations;
        info.max_workgroup_size_x = properties2.properties.limits.maxComputeWorkGroupSize[0];
        info.max_workgroup_count_x = properties2.properties.limits.maxComputeWorkGroupCount[0];
        info.max_storage_buffer_range = properties2.properties.limits.maxStorageBufferRange;

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
        if (lhs.max_workgroup_invocations != rhs.max_workgroup_invocations)
            return lhs.max_workgroup_invocations > rhs.max_workgroup_invocations;
        if (lhs.max_storage_buffer_range != rhs.max_storage_buffer_range)
            return lhs.max_storage_buffer_range > rhs.max_storage_buffer_range;
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
    if (!compute_enabled()) {
        VulkanVerticalIntersectionBatch batch;
        batch.diagnostic = "Vulkan infill compute is disabled in Preferences.";
        return batch;
    }
    VulkanVerticalIntersectionBatch batch = vulkan_intersection_context().dispatch(requests);
    if (!requests.empty())
        runtime_stats_registry().record_dispatch(requests.size(), batch);
    return batch;
#else
    VulkanVerticalIntersectionBatch batch;
    batch.diagnostic = "Vulkan infill compute was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.";
    return batch;
#endif
}

VulkanTreeContourBatch VulkanSlicerBackend::dispatch_tree_contour_candidates(
    const std::vector<VulkanTreeContourRequest>& requests,
    const std::vector<Segment>& contour_edges)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled()) {
        VulkanTreeContourBatch batch;
        batch.diagnostic = "Vulkan tree contour compute is disabled in Preferences.";
        return batch;
    }
    return vulkan_intersection_context().dispatch_tree_contours(requests, contour_edges);
#else
    VulkanTreeContourBatch batch;
    batch.diagnostic = "Vulkan tree contour compute was not compiled.";
    return batch;
#endif
}

bool VulkanSlicerBackend::should_dispatch_vertical_intersections(size_t request_count)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled())
        return false;
    return vulkan_intersection_context().should_dispatch_vertical_intersections(request_count);
#else
    return false;
#endif
}

bool VulkanSlicerBackend::prepare_for_slicing()
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled()) {
        runtime_stats_registry().set_backend({}, "Disabled",
            "Vulkan compute is disabled in Preferences; using the exact CPU geometry path.",
            0, 0, 0);
        return false;
    }
    return vulkan_intersection_context().prepare_for_slicing();
#else
    runtime_stats_registry().set_backend({}, {},
        "Vulkan infill compute was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.",
        0, 0, 0);
    return false;
#endif
}

void VulkanSlicerBackend::set_compute_enabled(bool enabled)
{
    g_compute_enabled.store(enabled, std::memory_order_release);
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!enabled) {
        // Do not tear the context down here: the preference can change while a
        // worker is completing a slice. Post-slice cleanup owns staging
        // reclamation and a later enabled slice can reuse its tuned pipeline.
        runtime_stats_registry().set_backend({}, "Disabled",
            "Vulkan compute is disabled in Preferences; using the exact CPU geometry path.",
            0, 0, 0);
    }
#endif
}

bool VulkanSlicerBackend::compute_enabled()
{
    return g_compute_enabled.load(std::memory_order_acquire);
}

void VulkanSlicerBackend::begin_slicing_session()
{
    runtime_stats_registry().begin_slice();
}

void VulkanSlicerBackend::release_unused_staging_memory()
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    vulkan_intersection_context().release_unused_staging_memory();
#endif
}

VulkanIntersectionValidationMode VulkanSlicerBackend::vertical_intersection_validation_mode()
{
    return configured_validation_mode();
}

bool VulkanSlicerBackend::should_validate_vertical_intersection(size_t index, size_t count)
{
    if (configured_validation_mode() == VulkanIntersectionValidationMode::Strict)
        return true;
    // The in-process qualification has already checked broad signed-integer
    // vectors. Keep a cheap guard at both boundaries and throughout long
    // real-model batches without redoing every GPU multiply/add on the CPU.
    return index == 0 || index + 1 == count || (index % 512) == 0;
}

void VulkanSlicerBackend::note_skipped_vertical_intersection_workload(size_t request_count)
{
    if (request_count != 0)
        runtime_stats_registry().record_skipped(request_count);
}

void VulkanSlicerBackend::note_vertical_intersection_usage(size_t accepted_gpu_results,
                                                            size_t cpu_validation_checks,
                                                            bool validation_failed,
                                                            const std::string& diagnostic)
{
    runtime_stats_registry().record_usage(accepted_gpu_results, cpu_validation_checks,
                                          validation_failed, diagnostic);
}

VulkanSlicerRuntimeStats VulkanSlicerBackend::query_runtime_stats()
{
    return runtime_stats_registry().snapshot();
}

std::string VulkanSlicerBackend::runtime_diagnostic_report()
{
    const VulkanSlicerRuntimeStats stats = query_runtime_stats();
    std::ostringstream report;
    report << "Vulkan slicer runtime diagnostics\n"
           << "device: " << (stats.selected_device.empty() ? "not initialized" : stats.selected_device) << '\n'
           << "profile: " << (stats.execution_profile.empty() ? "not initialized" : stats.execution_profile) << '\n'
           << "validation: " << stats.validation_mode << '\n'
           << "workgroup: " << stats.configured_workgroup_size << " configured / "
           << stats.maximum_workgroup_size << " device maximum\n"
           << "reusable staging: " << stats.reusable_staging_capacity << " requests\n"
           << "dispatch calls: " << stats.dispatch_calls << ", queue submissions: " << stats.queue_submissions << '\n'
           << "submitted intersections: " << stats.submitted_intersections
           << ", GPU accepted: " << stats.accepted_gpu_intersections << '\n'
           << "CPU validation checks: " << stats.cpu_validation_checks
           << ", failures: " << stats.validation_failures << '\n'
           << "small workloads skipped: " << stats.skipped_small_workloads
           << " (" << stats.skipped_intersections << " intersections)\n"
           << "last GPU time: " << stats.last_gpu_ms << " ms, last host time: " << stats.last_host_ms << " ms\n"
           << "last status: " << stats.last_diagnostic;
    return report.str();
}

} // namespace Slic3r::Gpu
