#include "GpuExactGeometry.hpp"
#include <system_error>
// Exercise the real facade and CUDA acceptance gate with a simulated device
// boundary that returns one incorrect result. This is not hardware evidence.
#include "VulkanSlicer.hpp"
#include "CudaSlicer.hpp"
#include "GpuBatchSafety.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static fs::path sink;
static bool rejected = false;
static bool simulated_submission = false;
static void verify_exit_summary()
{
    std::ifstream input(sink);
    std::map<std::string, std::string> values;
    for (std::string line; std::getline(input, line); ) {
        const auto separator = line.find('=');
        if (separator != std::string::npos)
            values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    const bool valid = rejected && values["vertical_submitted"] == "3" &&
        values["queue_submissions"] == "1" && values["vertical_accepted"] == "0" &&
        values["vertical_failures"] == "1" && values["active_backend"] == "CPU";
    std::error_code ignored;
    fs::remove(sink, ignored);
    if (!valid) {
        std::fprintf(stderr, "Rejected simulated CUDA submission was omitted from exit diagnostics.\n");
        std::_Exit(1);
    }
    std::fprintf(stdout, "Rejected simulated CUDA submission accounted; no geometry accepted.\n");
}

namespace Slic3r::Gpu {
bool cuda_runtime_query(CudaSlicerCapabilities& capabilities) {
    capabilities.runtime_available = true;
    capabilities.device_name = "Simulated CUDA boundary (not hardware)";
    return true;
}
bool cuda_runtime_prepare(std::string&) { return true; }
bool cuda_runtime_dispatch(const std::vector<VerticalIntersectionRequest>&,
    std::vector<VerticalIntersection>& output, double& elapsed, std::string& diagnostic)
{
    simulated_submission = true;
    output = {{500001, 1000, 0, true}, {510000, 1000, 1, true}, {510000, 1000, 2, true}};
    elapsed = 0;
    diagnostic = "Simulated kernel completed, deliberately corrupting record zero.";
    return true;
}
void cuda_runtime_release() {}
}

int main()
{
    sink = fs::temp_directory_path() / ("gpu-rejected-cuda-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
#ifdef _WIN32
    _wputenv_s(L"SLIC3R_GPU_DIAGNOSTICS", sink.native().c_str());
    _wputenv_s(L"SLIC3R_GPU_BACKEND", L"cuda");
#else
    setenv("SLIC3R_GPU_DIAGNOSTICS", sink.c_str(), 1);
    setenv("SLIC3R_GPU_BACKEND", "cuda", 1);
#endif
    // Register before core initialization: its exit writer runs first. This
    // verifier reads the file only, never a potentially destroyed core object.
    std::atexit(verify_exit_summary);
    using namespace Slic3r::Gpu;
    const std::vector<VerticalIntersectionRequest> requests {
        {{{0, 0}, {1000, 1000}}, 500, 0},
        {{{0, 10}, {1000, 1010}}, 500, 1},
        {{{1000, 1010}, {0, 10}}, 500, 2}};
    const auto batch = VulkanSlicerBackend::dispatch_vertical_intersections(requests);
    rejected = !batch.dispatched && batch.intersections.empty() &&
        simulated_submission && cuda_safety_state().quarantined();
    return 0;
}
