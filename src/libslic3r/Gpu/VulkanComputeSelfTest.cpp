#include "Gpu/VulkanSlicer.hpp"
#include "Utils.hpp"

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

    std::cout << "Vulkan exact vertical-intersection self-test passed for "
              << batch.intersections.size() << " requests.\n";
    return 0;
}
