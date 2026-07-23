#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GpuExactGeometry.hpp"

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

// One request represents the exact rational intersection of a polygon edge
// and a vertical infill scanline.  The caller only submits strict interior
// intersections; contour-vertex rules stay on the CPU because they depend on
// neighbouring edges and exact winding rules.
struct VulkanVerticalIntersectionRequest {
    Segment  segment;
    Coord    scan_x { 0 };
    uint64_t stable_id { 0 };
};

struct VulkanVerticalIntersection {
    Coord    numerator { 0 };
    Coord    denominator { 1 };
    uint64_t stable_id { 0 };
    bool     valid { false };
};

struct VulkanVerticalIntersectionBatch {
    bool                                    dispatched { false };
    std::string                             diagnostic;
    std::vector<VulkanVerticalIntersection> intersections;
};

// This capability layer is intentionally independent of the slicing pipeline.
// It lets the UI select a GPU only when exact-integer prerequisites are met;
// the CPU pipeline remains the authoritative fallback for every stage.
class VulkanSlicerBackend {
public:
    static VulkanSlicerCapabilities query_capabilities();

    // Executes the high-cardinality, fixed-point portion of rectilinear and
    // support-infill scan conversion.  Results are returned in input order;
    // callers keep the CPU exact-reference check as the authority before an
    // intersection is committed to a toolpath.
    static VulkanVerticalIntersectionBatch dispatch_vertical_intersections(
        const std::vector<VulkanVerticalIntersectionRequest>& requests);

    static bool supports_deterministic_geometry(const VulkanDeviceInfo& device)
    {
        return device.compute_queue && device.shader_int64;
    }
};

} // namespace Slic3r::Gpu
