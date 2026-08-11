#include "CudaSlicer.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace Slic3r::Gpu {
namespace {

struct CudaRequest {
    int64_t ax, ay, bx, by, scan_x;
    uint64_t stable_id;
};

struct CudaResult {
    int64_t numerator, denominator;
    uint64_t stable_id;
    uint32_t valid;
};

__global__ void vertical_intersection_kernel(const CudaRequest* requests,
                                             CudaResult* results,
                                             size_t count)
{
    const size_t index = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= count)
        return;

    const CudaRequest request = requests[index];
    CudaResult result { 0, 1, request.stable_id, 0 };
    const int64_t lower_x = min(request.ax, request.bx);
    const int64_t upper_x = max(request.ax, request.bx);
    if (request.ax != request.bx && request.scan_x > lower_x && request.scan_x < upper_x) {
        const bool forward = request.bx > request.ax;
        const int64_t numerator = forward ? request.scan_x - request.ax : request.ax - request.scan_x;
        const int64_t denominator = forward ? request.bx - request.ax : request.ax - request.bx;
        // The host tiles requests so this fixed-point product is inside int64.
        result.numerator = numerator * (request.by - request.ay) + request.ay * denominator;
        result.denominator = denominator;
        result.valid = 1;
    }
    results[index] = result;
}

int g_device = -1;

bool check(cudaError_t error, std::string& diagnostic, const char* operation)
{
    if (error == cudaSuccess)
        return true;
    diagnostic = std::string(operation) + " failed: " + cudaGetErrorString(error);
    return false;
}

} // namespace

bool cuda_runtime_query(CudaSlicerCapabilities& capabilities)
{
    int device_count = 0;
    if (!check(cudaGetDeviceCount(&device_count), capabilities.diagnostic, "cudaGetDeviceCount") || device_count == 0) {
        if (capabilities.diagnostic.empty())
            capabilities.diagnostic = "No CUDA device was found.";
        return false;
    }

    cudaDeviceProp properties {};
    if (!check(cudaGetDeviceProperties(&properties, 0), capabilities.diagnostic, "cudaGetDeviceProperties"))
        return false;
    capabilities.runtime_available = true;
    capabilities.device_name = properties.name;
    capabilities.multiprocessors = uint32_t(properties.multiProcessorCount);
    capabilities.max_threads_per_block = uint32_t(properties.maxThreadsPerBlock);
    capabilities.diagnostic = "CUDA device " + capabilities.device_name + " is available.";
    return true;
}

bool cuda_runtime_prepare(std::string& diagnostic)
{
    CudaSlicerCapabilities capabilities;
    if (!cuda_runtime_query(capabilities)) {
        diagnostic = capabilities.diagnostic;
        return false;
    }
    if (!check(cudaSetDevice(0), diagnostic, "cudaSetDevice"))
        return false;
    g_device = 0;
    diagnostic = capabilities.diagnostic;
    return true;
}

bool cuda_runtime_dispatch(const std::vector<VulkanVerticalIntersectionRequest>& requests,
                           std::vector<VulkanVerticalIntersection>& intersections,
                           double& gpu_elapsed_ms,
                           std::string& diagnostic)
{
    if (requests.empty()) {
        intersections.clear();
        gpu_elapsed_ms = 0.0;
        diagnostic = "CUDA received an empty batch.";
        return true;
    }
    if (g_device < 0 && !cuda_runtime_prepare(diagnostic))
        return false;

    std::vector<CudaRequest> host_requests;
    host_requests.reserve(requests.size());
    for (const auto& request : requests)
        host_requests.push_back({ request.segment.a.x, request.segment.a.y,
                                  request.segment.b.x, request.segment.b.y,
                                  request.scan_x, request.stable_id });

    CudaRequest* device_requests = nullptr;
    CudaResult* device_results = nullptr;
    std::vector<CudaResult> host_results(requests.size());
    const size_t request_bytes = host_requests.size() * sizeof(CudaRequest);
    const size_t result_bytes = host_results.size() * sizeof(CudaResult);
    if (!check(cudaMalloc(&device_requests, request_bytes), diagnostic, "cudaMalloc(requests)"))
        return false;
    if (!check(cudaMalloc(&device_results, result_bytes), diagnostic, "cudaMalloc(results)")) {
        cudaFree(device_requests);
        return false;
    }

    bool success = check(cudaMemcpy(device_requests, host_requests.data(), request_bytes, cudaMemcpyHostToDevice),
                         diagnostic, "cudaMemcpy(requests)");
    const uint32_t block_size = 128;
    if (success) {
        const uint32_t grid_size = uint32_t((requests.size() + block_size - 1) / block_size);
        const auto gpu_start = std::chrono::steady_clock::now();
        vertical_intersection_kernel<<<grid_size, block_size>>>(device_requests, device_results, requests.size());
        success = check(cudaGetLastError(), diagnostic, "CUDA kernel launch") &&
                  check(cudaDeviceSynchronize(), diagnostic, "CUDA kernel synchronization");
        gpu_elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - gpu_start).count();
    }
    if (success)
        success = check(cudaMemcpy(host_results.data(), device_results, result_bytes, cudaMemcpyDeviceToHost),
                        diagnostic, "cudaMemcpy(results)");
    cudaFree(device_results);
    cudaFree(device_requests);
    if (!success)
        return false;

    intersections.resize(host_results.size());
    for (size_t index = 0; index < host_results.size(); ++index) {
        const CudaResult& result = host_results[index];
        intersections[index] = { result.numerator, result.denominator, result.stable_id, result.valid != 0 };
    }
    diagnostic = "CUDA exact vertical-intersection kernel completed.";
    return true;
}

void cuda_runtime_release()
{
    g_device = -1;
    cudaDeviceReset();
}

} // namespace Slic3r::Gpu
