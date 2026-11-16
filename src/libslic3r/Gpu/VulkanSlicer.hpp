#pragma once
#include <cstddef>

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
    uint32_t    max_workgroup_invocations { 0 };
    uint32_t    max_workgroup_size_x { 0 };
    uint32_t    max_workgroup_count_x { 0 };
    uint64_t    max_storage_buffer_range { 0 };
};

struct VulkanSlicerCapabilities {
    bool                     compiled_with_vulkan { false };
    bool                     loader_available { false };
    std::string              diagnostic;
    std::vector<VulkanDeviceInfo> devices;
};

// Compatibility aliases for the existing integration; geometry contracts are
// backend neutral and shared by CUDA, Vulkan and the independent host oracle.
using VulkanVerticalIntersectionRequest = VerticalIntersectionRequest;
using VulkanVerticalIntersection = VerticalIntersection;
using VulkanVerticalIntersectionBatch = VerticalIntersectionBatch;
using VulkanTreeContourRequest = TreeContourRequest;
using VulkanTreeContourBatch = TreeContourBatch;

// The accelerated scan-conversion path must remain observable while it is
// experimental. These counters are process-local and deliberately do not
// affect the generated G-code.
struct VulkanSlicerRuntimeStats {
    uint64_t    dispatch_calls { 0 };
    uint64_t    queue_submissions { 0 };
    uint64_t    submitted_intersections { 0 };
    uint64_t    accepted_gpu_intersections { 0 };
    uint64_t    cpu_validation_checks { 0 };
    uint64_t    validation_failures { 0 };
    uint64_t    skipped_small_workloads { 0 };
    uint64_t    skipped_intersections { 0 };
    double      total_gpu_ms { 0.0 };
    double      total_host_ms { 0.0 };
    double      last_gpu_ms { -1.0 };
    double      last_host_ms { 0.0 };
    uint32_t    configured_workgroup_size { 0 };
    uint32_t    maximum_workgroup_size { 0 };
    size_t      reusable_staging_capacity { 0 };
    size_t      preferred_intersection_batch { 4096 };
    std::string selected_device;
    std::string execution_profile;
    // A concise human-readable label for the last compute phase. The progress
    // notification uses this rather than exposing a driver diagnostic there.
    std::string current_operation;
    std::string validation_mode;
    std::string last_diagnostic;
    std::string active_backend;
    bool        cuda_enabled { false };
    bool        vulkan_enabled { false };
    bool        cuda_available { false };
    bool        vulkan_available { false };
    uint32_t    configured_batch_size { 64 };
};

enum class ComputeBackendPreference {
    Cuda,
    Vulkan,
    Cpu,
    Auto
};

enum class VulkanIntersectionValidationMode {
    // Compare every GPU result to the CPU reference. This is the audit mode
    // for a new driver or a correctness investigation.
    Strict,
    // Legacy enum value retained for source compatibility. Production accepts
    // only full exact validation; selecting sampled mode has no effect.
    Sampled
};

// This capability layer is intentionally independent of the slicing pipeline.
// It lets the UI select a GPU only when exact-integer prerequisites are met;
// the CPU pipeline remains the authoritative fallback for every stage.
class VulkanSlicerBackend {
public:
    static VulkanSlicerCapabilities query_capabilities();
    // Set before first Vulkan initialization. CPU mode does not load shaders.
    // A changed directory after initialization is rejected.
    static bool set_shader_directory(const std::string& directory);

    // Vulkan switch; CUDA has its own independent switch. Both default OFF.
    // Explicit SLIC3R_GPU_BACKEND=cpu|cuda|vulkan|auto overrides preferences.
    // Unknown environment values select CPU. Getters also check compilation
    // and process-lifetime quarantine. CPU mode never initializes a GPU.
    static void set_compute_enabled(bool enabled);
    static bool compute_enabled();

    // CUDA and Vulkan are independent switches. A disabled backend is never
    // initialized, even when the other backend is unavailable.
    static void set_cuda_enabled(bool enabled);
    static bool cuda_enabled();
    static void set_backend_preference(ComputeBackendPreference preference);
    static ComputeBackendPreference backend_preference();
    static void set_batch_size(uint32_t batch_size);
    static uint32_t batch_size();
    static void set_strict_validation(bool enabled);
    static bool strict_validation();

    // Legacy tuning preference retained for compatibility. GPU batch size
    // bounds eligibility; complete exact host validation is always required.
    static void set_gpu_priority_enabled(bool enabled);
    static bool gpu_priority_enabled();

    // Clears per-slice counters while retaining the qualified device and
    // pipeline. The progress UI can then report this slice's actual GPU work
    // instead of carrying a stale job label into a later CPU-only stage.
    static void begin_slicing_session();

    // Optional explicit preparation on the slicing worker. Normal adapters
    // let the first eligible geometry dispatch initialize and qualify lazily.
    // Returns false without initialization in CPU/disabled/uncompiled mode.
    static bool prepare_for_slicing();

    // Releases the reusable host-visible Vulkan staging buffers after a slice.
    // The device and qualified pipeline remain ready for the next job.
    static void release_unused_staging_memory();

    // Executes the high-cardinality, fixed-point portion of rectilinear and
    // support-infill scan conversion. Results are returned in input order;
    // every raw-coordinate intermediate/destination is checked before GPU use.
    // Dispatched means count, order, IDs, validity and ALL exact rational values
    // passed the independent wide oracle before any result was consumed.
    // Failure returns no intersections and quarantines the failed backend.
    static VulkanVerticalIntersectionBatch dispatch_vertical_intersections(
        const std::vector<VulkanVerticalIntersectionRequest>& requests);

    // Conservative tree-support broad phase. Inputs and results preserve
    // request order. Empty output means that the CPU must retain all queries.
    static VulkanTreeContourBatch dispatch_tree_contour_candidates(
        const std::vector<VulkanTreeContourRequest>& requests,
        const std::vector<Segment>& contour_boxes);

    // Cheap eligibility only: checks opt-in, compiled availability/quarantine
    // and bounded request count, without initializing/querying a GPU.
    static bool should_dispatch_vertical_intersections(size_t request_count);

    // Compatibility accessors always select full exact validation. Legacy
    // sampled settings/environment cannot weaken batch acceptance.
    static VulkanIntersectionValidationMode vertical_intersection_validation_mode();
    static bool should_validate_vertical_intersection(size_t index, size_t count);

    static void note_skipped_vertical_intersection_workload(size_t request_count);
    static void note_vertical_intersection_usage(size_t accepted_gpu_results,
                                                size_t cpu_validation_checks,
                                                bool validation_failed,
                                                const std::string& diagnostic);
    static VulkanSlicerRuntimeStats query_runtime_stats();
    static std::string runtime_diagnostic_report();

    static bool supports_deterministic_geometry(const VulkanDeviceInfo& device)
    {
        return device.compute_queue && device.shader_int64;
    }
};

} // namespace Slic3r::Gpu
