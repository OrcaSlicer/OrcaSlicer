#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::Gpu {

struct VulkanDeviceInfo {
    std::string name;
    uint32_t    vendor_id { 0 };
    uint32_t    device_id { 0 };
    bool        discrete { false };
    bool        compute_queue { false };
    bool        shader_int64 { false };
    bool        subgroup_operations { false };
};

struct VulkanSlicerCapabilities {
    bool                     compiled_with_vulkan { false };
    bool                     loader_available { false };
    std::string              diagnostic;
    std::vector<VulkanDeviceInfo> devices;
};

// This capability layer is intentionally independent of the slicing pipeline.
// It lets the UI select a GPU only when exact-integer prerequisites are met;
// the CPU pipeline remains the authoritative fallback for every stage.
class VulkanSlicerBackend {
public:
    static VulkanSlicerCapabilities query_capabilities();

    static bool supports_deterministic_geometry(const VulkanDeviceInfo& device)
    {
        return device.compute_queue && device.shader_int64;
    }
};

} // namespace Slic3r::Gpu
