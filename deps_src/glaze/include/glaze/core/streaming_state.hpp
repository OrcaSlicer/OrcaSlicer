// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstddef>
#include <type_traits>

#include "glaze/core/context.hpp"

namespace glz
{
   // Type-erased interface for streaming buffer operations.
   // Allows parsers to trigger refill without knowing the concrete buffer type.
   //
   // Design Note:
   //   This uses function pointers for type erasure rather than templates. This prevents
   //   the core parsing logic from being templated on buffer type, avoiding code bloat
   //   and compile time increases. The trade-off is that function pointer calls cannot
   //   be inlined. In practice, this overhead is negligible because:
   //   - These functions are called infrequently relative to parsing work
   //   - I/O latency (disk/network) dominates CPU overhead
   //   - The function bodies are trivial (return pointer/size)
   //
   //   Streaming parsing is inherently slower than buffer-based parsing due to this
   //   overhead and the need to handle data spanning buffer boundaries. Users choosing
   //   streaming trade some performance for bounded memory usage.
   struct streaming_state
   {
      void* buffer_ptr = nullptr;

      // Function pointers for type-erased operations
      const char* (*get_data)(void*) = nullptr;
      size_t (*get_size)(void*) = nullptr;
      void (*consume)(void*, size_t) = nullptr;
      bool (*refill)(void*) = nullptr;
      bool (*eof)(void*) = nullptr;

      // Check if streaming is enabled
      bool enabled() const noexcept { return buffer_ptr != nullptr; }

      // Get current data pointer
      const char* data() const noexcept { return get_data(buffer_ptr); }

      // Get current available size
      size_t size() const noexcept { return get_size(buffer_ptr); }

      // Consume n bytes from buffer
      void consume_bytes(size_t n) const noexcept { consume(buffer_ptr, n); }

      // Refill buffer, returns true if data available
      bool refill_buffer() const noexcept { return refill(buffer_ptr); }

      // Check if at end of stream
      bool at_eof() const noexcept { return eof(buffer_ptr); }

      // Consume up to current position and refill
      // Returns new iterators via out parameters
      // it_offset is how far into current buffer we've parsed
      bool consume_and_refill(size_t consumed_bytes, const char*& new_it, const char*& new_end) const noexcept
      {
         consume_bytes(consumed_bytes);
         bool has_data = refill_buffer();
         new_it = data();
         new_end = data() + size();
         return has_data || size() > 0;
      }
   };

   // Helper to create streaming_state from a concrete buffer type
   template <class Buffer>
   streaming_state make_streaming_state(Buffer& buffer) noexcept
   {
      streaming_state state;
      state.buffer_ptr = &buffer;
      state.get_data = [](void* p) -> const char* { return static_cast<Buffer*>(p)->data(); };
      state.get_size = [](void* p) -> size_t { return static_cast<Buffer*>(p)->size(); };
      state.consume = [](void* p, size_t n) { static_cast<Buffer*>(p)->consume(n); };
      state.refill = [](void* p) -> bool { return static_cast<Buffer*>(p)->refill(); };
      state.eof = [](void* p) -> bool { return static_cast<Buffer*>(p)->eof(); };
      return state;
   }

   // Streaming context extends base context with streaming state
   // This avoids adding streaming overhead to non-streaming use cases
   struct streaming_context : context
   {
      streaming_state stream{};
   };

   // Concept to detect streaming-capable context at compile time
   // Automatically removes cv-ref qualifiers for convenience
   template <class T>
   concept has_streaming_state = is_context<std::remove_cvref_t<T>> && requires(std::remove_cvref_t<T>& ctx) {
      { ctx.stream } -> std::same_as<streaming_state&>;
   };
}
