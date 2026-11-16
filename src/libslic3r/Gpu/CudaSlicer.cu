#include "CudaSlicer.hpp"
#include "GpuBatchSafety.hpp"
#include "CudaDispatchBuffers.hpp"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdint>
#include <mutex>

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

__global__ void vertical_intersection_kernel(const CudaRequest* requests, CudaResult* results, size_t count)
{
    const size_t index = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= count)
        return;
    const CudaRequest request = requests[index];
    CudaResult result {0, 1, request.stable_id, 0};
    const int64_t lower_x = min(request.ax, request.bx);
    const int64_t upper_x = max(request.ax, request.bx);
    if (request.ax != request.bx && request.scan_x > lower_x && request.scan_x < upper_x) {
        const bool forward = request.bx > request.ax;
        const int64_t t = forward ? request.scan_x - request.ax : request.ax - request.scan_x;
        result.denominator = forward ? request.bx - request.ax : request.ax - request.bx;
        // Host preflight proves each raw-coordinate intermediate fits int64.
        result.numerator = t * (request.by - request.ay) + request.ay * result.denominator;
        result.valid = 1;
    }
    results[index] = result;
}

std::mutex g_cuda_mutex;
int g_device = -1;
bool g_qualified = false;
uint32_t g_maximum_grid_x = 0;
constexpr uint32_t block_size = 128;
CudaDispatchBuffers g_dispatch_buffers(maximum_intersection_requests * sizeof(CudaRequest),
                                      maximum_intersection_requests * sizeof(CudaResult));

bool check(cudaError_t error, std::string& diagnostic, const char* operation)
{
    if (error == cudaSuccess)
        return true;
    diagnostic = std::string(operation) + " failed: " + cudaGetErrorString(error);
    return false;
}

struct CudaBufferMemory {
    std::string& diagnostic;

    bool allocate(void*& buffer, size_t bytes)
    {
        return check(cudaMalloc(&buffer, bytes), diagnostic, "cudaMalloc(dispatch buffer)");
    }

    bool release(void* buffer)
    {
        return check(cudaFree(buffer), diagnostic, "cudaFree(dispatch buffer)");
    }
};

bool query_locked(CudaSlicerCapabilities& capabilities)
{
    if (g_dispatch_buffers.cleanup_failed()) {
        capabilities.diagnostic = "CUDA buffer cleanup failed; restart the application before reusing CUDA.";
        return false;
    }
    int count = 0;
    if (!check(cudaGetDeviceCount(&count), capabilities.diagnostic, "cudaGetDeviceCount") || count == 0) {
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

bool dispatch_locked(const std::vector<VerticalIntersectionRequest>& requests,
                     std::vector<VerticalIntersection>& intersections,
                     double& elapsed_ms, std::string& diagnostic)
{
    intersections.clear();
    if (!preflight_vertical_batch(requests)) {
        diagnostic = "CUDA numeric domain or bounded batch contract rejected.";
        return false;
    }
    const size_t grid_size = (requests.size() + block_size - 1) / block_size;
    if (grid_size > g_maximum_grid_x ||
        !check(cudaSetDevice(g_device), diagnostic, "cudaSetDevice"))
        return false;

    std::vector<CudaRequest> host_requests;
    host_requests.reserve(requests.size());
    for (const auto& request : requests)
        host_requests.push_back({request.segment.a.x, request.segment.a.y,
                                 request.segment.b.x, request.segment.b.y,
                                 request.scan_x, request.stable_id});
    std::vector<CudaResult> host_results(requests.size());
    // maximum_intersection_requests bounds these byte and grid calculations.
    const size_t request_bytes = host_requests.size() * sizeof(CudaRequest);
    const size_t result_bytes = host_results.size() * sizeof(CudaResult);
    CudaBufferMemory memory {diagnostic};
    const bool success = g_dispatch_buffers.dispatch(request_bytes, result_bytes, memory,
        [&](void* request_storage, void* result_storage) {
            auto* device_requests = static_cast<CudaRequest*>(request_storage);
            auto* device_results = static_cast<CudaResult*>(result_storage);
            bool copied = check(cudaMemcpy(device_requests, host_requests.data(), request_bytes, cudaMemcpyHostToDevice),
                                diagnostic, "cudaMemcpy(requests)");
            if (copied) {
                const auto start = std::chrono::steady_clock::now();
                vertical_intersection_kernel<<<uint32_t(grid_size), block_size>>>(device_requests, device_results, requests.size());
                copied = check(cudaGetLastError(), diagnostic, "CUDA kernel launch") &&
                         check(cudaDeviceSynchronize(), diagnostic, "CUDA kernel synchronization");
                elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
            return copied && check(cudaMemcpy(host_results.data(), device_results, result_bytes, cudaMemcpyDeviceToHost),
                                   diagnostic, "cudaMemcpy(results)");
        });
    if (!success)
        return false;
    intersections.reserve(host_results.size());
    for (const auto& result : host_results)
        intersections.push_back({result.numerator, result.denominator, result.stable_id, result.valid == 1});
    diagnostic = "CUDA exact vertical-intersection kernel completed.";
    return true;
}

bool prepare_locked(std::string& diagnostic)
{
    if (g_dispatch_buffers.cleanup_failed()) {
        diagnostic = "CUDA buffer cleanup failed; restart the application before reusing CUDA.";
        return false;
    }
    if (g_qualified)
        return check(cudaSetDevice(g_device), diagnostic, "cudaSetDevice");
    CudaSlicerCapabilities capabilities;
    if (!query_locked(capabilities) || !check(cudaSetDevice(0), diagnostic, "cudaSetDevice")) {
        if (diagnostic.empty()) diagnostic = capabilities.diagnostic;
        return false;
    }
    cudaDeviceProp properties {};
    cudaFuncAttributes attributes {};
    if (!check(cudaGetDeviceProperties(&properties, 0), diagnostic, "cudaGetDeviceProperties") ||
        !check(cudaFuncGetAttributes(&attributes, vertical_intersection_kernel), diagnostic, "CUDA kernel image compatibility") ||
        properties.maxThreadsPerBlock < int(block_size) || attributes.maxThreadsPerBlock < int(block_size) ||
        properties.maxGridSize[0] <= 0)
        return false;
    g_device = 0;
    g_maximum_grid_x = uint32_t(properties.maxGridSize[0]);
    // Qualify reversed edges, signs and translated coordinates before any
    // live output is accepted. The same wide oracle validates the entire set.
    std::vector<VerticalIntersectionRequest> requests;
    for (uint64_t index = 0; index < 2048; ++index) {
        const Coord left = -500'000 + Coord(index % 97) * 101;
        const Coord right = left + 1003 + Coord(index % 89);
        const bool reverse = (index & 1) != 0;
        requests.push_back({{{reverse ? right : left, -750'000 + Coord(index) * 37},
                             {reverse ? left : right, 950'000 - Coord(index) * 53}},
                            left + 1 + Coord(index % uint64_t(right - left - 1)), index});
    }
    VerticalIntersectionBatch qualification;
    qualification.dispatched = dispatch_locked(requests, qualification.intersections, qualification.gpu_elapsed_ms, diagnostic);
    if (!validate_vertical_batch(requests, qualification)) {
        diagnostic = "CUDA complete exact-integer qualification failed: " + diagnostic;
        return false;
    }
    g_qualified = true;
    diagnostic = capabilities.diagnostic;
    return true;
}

} // namespace

bool cuda_runtime_query(CudaSlicerCapabilities& capabilities)
{
    std::lock_guard<std::mutex> lock(g_cuda_mutex);
    return query_locked(capabilities);
}

bool cuda_runtime_prepare(std::string& diagnostic)
{
    std::lock_guard<std::mutex> lock(g_cuda_mutex);
    return prepare_locked(diagnostic);
}

bool cuda_runtime_dispatch(const std::vector<VerticalIntersectionRequest>& requests,
                           std::vector<VerticalIntersection>& intersections,
                           double& elapsed_ms, std::string& diagnostic)
{
    // Check before selecting a device or constructing a runtime context.
    if (!preflight_vertical_batch(requests))
        return false;
    std::lock_guard<std::mutex> lock(g_cuda_mutex);
    return prepare_locked(diagnostic) && dispatch_locked(requests, intersections, elapsed_ms, diagnostic);
}

void cuda_runtime_release()
{
    // Release only our buffers on their owning device. Never reset a context
    // that may have independent CUDA users in this process.
    std::lock_guard<std::mutex> lock(g_cuda_mutex);
    if (g_dispatch_buffers.owns_storage()) {
        std::string diagnostic;
        if (g_device >= 0 && check(cudaSetDevice(g_device), diagnostic, "cudaSetDevice(release)")) {
            CudaBufferMemory memory {diagnostic};
            g_dispatch_buffers.release(memory);
        } else {
            g_dispatch_buffers.poison_cleanup();
        }
    }
    g_qualified = false;
    if (!g_dispatch_buffers.owns_storage())
        g_device = -1;
}

} // namespace Slic3r::Gpu
