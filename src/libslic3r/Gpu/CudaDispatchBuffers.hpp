#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <utility>

namespace Slic3r::Gpu {

// Allocation ownership is independent of CUDA calls so the actual dispatch
// boundary can be exercised with failing allocators on every build platform.
// The caller serializes access and selects the owning device before release.
// No static destructor calls CUDA after runtime teardown.
class CudaDispatchBuffers {
    struct Buffer {
        void* data {nullptr};
        size_t capacity {0};
    };

public:
    CudaDispatchBuffers(size_t input_limit, size_t output_limit)
        : m_input_limit(input_limit), m_output_limit(output_limit) {}

    CudaDispatchBuffers(const CudaDispatchBuffers&) = delete;
    CudaDispatchBuffers& operator=(const CudaDispatchBuffers&) = delete;

    template<class Memory, class Operation>
    bool dispatch(size_t input_bytes, size_t output_bytes, Memory& memory, Operation&& operation)
    {
        if (m_cleanup_failed || input_bytes == 0 || output_bytes == 0 ||
            input_bytes > m_input_limit || output_bytes > m_output_limit)
            return false;
        bool success = false;
        try {
            success = ensure_capacity(m_input, input_bytes, m_input_limit, memory) &&
                      ensure_capacity(m_output, output_bytes, m_output_limit, memory) &&
                      operation(m_input.data, m_output.data);
        } catch (...) {
            const std::exception_ptr operation_error = std::current_exception();
            try {
                release(memory);
            } catch (...) {
                // release still attempts both owners and records sticky poison.
            }
            std::rethrow_exception(operation_error);
        }
        if (!success)
            release(memory);
        return success;
    }

    template<class Memory>
    bool release(Memory& memory)
    {
        // Attempt both result-before-request frees even after an error.
        // Clear ownership first: a failed free may already have taken effect.
        std::exception_ptr first_error;
        const auto release_buffer = [&](Buffer& buffer) {
            try {
                return release_one(buffer, memory);
            } catch (...) {
                if (!first_error)
                    first_error = std::current_exception();
                return false;
            }
        };
        const bool output_released = release_buffer(m_output);
        const bool input_released = release_buffer(m_input);
        m_cleanup_failed = m_cleanup_failed || !output_released || !input_released;
        if (first_error)
            std::rethrow_exception(first_error);
        return output_released && input_released;
    }

    void poison_cleanup() { m_cleanup_failed = true; }
    bool cleanup_failed() const { return m_cleanup_failed; }
    bool owns_storage() const { return m_input.data != nullptr || m_output.data != nullptr; }

private:
    template<class Memory>
    bool ensure_capacity(Buffer& buffer, size_t bytes, size_t limit, Memory& memory)
    {
        if (buffer.data != nullptr && buffer.capacity >= bytes)
            return true;
        size_t capacity = std::min(size_t(256), limit);
        while (capacity < bytes && capacity <= limit / 2)
            capacity *= 2;
        capacity = std::max(capacity, bytes);

        Buffer replacement {nullptr, capacity};
        bool allocated = false;
        try {
            allocated = memory.allocate(replacement.data, capacity);
        } catch (...) {
            const std::exception_ptr allocation_error = std::current_exception();
            try {
                release_one(replacement, memory);
            } catch (...) {
                // Preserve the allocation error; outer cleanup handles owners.
            }
            std::rethrow_exception(allocation_error);
        }
        if (!allocated || replacement.data == nullptr) {
            release_one(replacement, memory);
            return false;
        }
        Buffer previous = std::exchange(buffer, replacement);
        return release_one(previous, memory);
    }

    template<class Memory>
    bool release_one(Buffer& buffer, Memory& memory)
    {
        void* owned = std::exchange(buffer, Buffer{}).data;
        if (owned == nullptr)
            return true;
        try {
            if (memory.release(owned))
                return true;
        } catch (...) {
            m_cleanup_failed = true;
            throw;
        }
        m_cleanup_failed = true;
        return false;
    }

    const size_t m_input_limit;
    const size_t m_output_limit;
    Buffer m_input;
    Buffer m_output;
    bool m_cleanup_failed {false};
};

} // namespace Slic3r::Gpu
