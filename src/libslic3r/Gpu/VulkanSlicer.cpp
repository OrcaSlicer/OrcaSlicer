#include "VulkanSlicer.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
#include <vulkan/vulkan.h>
#endif

namespace Slic3r::Gpu {

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

} // namespace Slic3r::Gpu
