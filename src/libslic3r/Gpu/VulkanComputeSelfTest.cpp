#include "Gpu/VulkanSlicer.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: orca-vulkan-compute-selftest <resources-directory>\n";
        return 64;
    }

    Slic3r::set_resources_dir(argv[1]);
    const Slic3r::Gpu::VulkanSlicerCapabilities capabilities =
        Slic3r::Gpu::VulkanSlicerBackend::query_capabilities();
    std::cout << "Vulkan capability scan: " << capabilities.diagnostic << '\n';
    for (const Slic3r::Gpu::VulkanDeviceInfo& device : capabilities.devices) {
        std::cout << "  device: " << device.name
                  << " (vendor=0x" << std::hex << device.vendor_id << std::dec << ")\n"
                  << "    shaderInt64=" << (device.shader_int64 ? "yes" : "no")
                  << ", compute queue=" << (device.compute_queue ? "yes" : "no") << '\n'
                  << "    workgroup: " << device.max_workgroup_invocations
                  << " invocations, x=" << device.max_workgroup_size_x
                  << ", dispatch-x=" << device.max_workgroup_count_x << '\n'
                  << "    max storage buffer=" << device.max_storage_buffer_range << " bytes\n";
    }
    if (!Slic3r::Gpu::VulkanSlicerBackend::prepare_for_slicing()) {
        std::cerr << "Vulkan initialization unavailable: "
                  << Slic3r::Gpu::VulkanSlicerBackend::query_runtime_stats().last_diagnostic << '\n';
        return 2;
    }
    std::vector<Slic3r::Gpu::VulkanVerticalIntersectionRequest> requests;
    requests.reserve(1024);
    for (uint64_t index = 0; index < 1024; ++index) {
        const int64_t ay = int64_t(index) - 512;
        const int64_t by = 700 - int64_t(index);
        requests.push_back({ { { 0, ay }, { 1000, by } }, 375, index });
    }

    const Slic3r::Gpu::VulkanVerticalIntersectionBatch batch =
        Slic3r::Gpu::VulkanSlicerBackend::dispatch_vertical_intersections(requests);
    if (!batch.dispatched || batch.intersections.size() != requests.size()) {
        std::cerr << "Vulkan dispatch unavailable: " << batch.diagnostic << '\n';
        return 2;
    }

    for (size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        const auto& result = batch.intersections[index];
        const int64_t denominator = request.segment.b.x - request.segment.a.x;
        const int64_t numerator = (request.scan_x - request.segment.a.x) *
            (request.segment.b.y - request.segment.a.y) + request.segment.a.y * denominator;
        if (!result.valid || result.stable_id != request.stable_id ||
            result.denominator != denominator || result.numerator != numerator) {
            std::cerr << "Vulkan result mismatch at request " << index << '\n';
            return 3;
        }
    }

    std::vector<Slic3r::Gpu::VulkanTreeContourRequest> tree_requests;
    std::vector<Slic3r::Gpu::Segment> tree_contours;
    for (uint64_t index = 0; index < 64; ++index)
        tree_requests.push_back({ { { 0, int64_t(index) }, { 1000, int64_t(index) } }, index });
    for (int64_t index = 0; index < 511; ++index)
        tree_contours.push_back({ { 10'000, index }, { 10'100, index } });
    tree_contours.push_back({ { 500, 0 }, { 500, 50 } });
    const Slic3r::Gpu::VulkanTreeContourBatch tree_batch =
        Slic3r::Gpu::VulkanSlicerBackend::dispatch_tree_contour_candidates(tree_requests, tree_contours);
    const bool tree_results_match = tree_batch.may_intersect.size() == tree_requests.size() &&
        std::all_of(tree_batch.may_intersect.begin(), tree_batch.may_intersect.end(),
                    [index = size_t(0)](uint8_t value) mutable { return (index++ <= 50) == (value != 0); });
    if (!tree_batch.dispatched || !tree_results_match) {
        std::cerr << "Vulkan tree contour broad-phase self-test failed: " << tree_batch.diagnostic << '\n';
        return 4;
    }

    std::cout << "Vulkan exact vertical-intersection self-test passed for "
              << batch.intersections.size() << " requests; tree contour broad phase passed for "
              << tree_batch.may_intersect.size() << " branches.\n"
              << Slic3r::Gpu::VulkanSlicerBackend::runtime_diagnostic_report() << '\n';
    return 0;
}
