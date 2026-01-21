// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <span>

#include "glaze/concepts/container_concepts.hpp"
#include "glaze/core/context.hpp"
#include "glaze/util/inline.hpp"

namespace glz
{
   // Primary template for buffer traits
   // Handles resizable buffers (std::string, std::vector<char>, etc.)
   template <class Buffer>
   struct buffer_traits
   {
      static constexpr bool is_resizable = resizable<std::remove_cvref_t<Buffer>>;
      static constexpr bool has_bounded_capacity = !is_resizable && has_size<std::remove_cvref_t<Buffer>>;
      static constexpr bool is_output_streaming = false; // True for output buffers that support incremental flushing
      static constexpr bool is_input_streaming = false; // True for input buffers that support incremental refilling

      GLZ_ALWAYS_INLINE static constexpr size_t capacity(const Buffer& b) noexcept
      {
         if constexpr (is_resizable) {
            return std::numeric_limits<size_t>::max(); // Effectively unlimited
         }
         else if constexpr (has_size<std::remove_cvref_t<Buffer>>) {
            return b.size();
         }
         else {
            return std::numeric_limits<size_t>::max();
         }
      }

      // Attempt to ensure buffer can hold `needed` bytes
      // Returns true if successful, false if buffer cannot accommodate
      GLZ_ALWAYS_INLINE static bool ensure_capacity(Buffer& b, size_t needed) noexcept(not is_resizable)
      {
         if constexpr (is_resizable) {
            if (needed > b.size()) {
               b.resize(2 * needed);
            }
            return true;
         }
         else {
            return capacity(b) >= needed;
         }
      }

      // Finalize buffer to actual written size
      GLZ_ALWAYS_INLINE static void finalize(Buffer& b, size_t written) noexcept(not is_resizable)
      {
         if constexpr (is_resizable) {
            b.resize(written);
         }
         // For fixed buffers: no-op, count is returned in result
      }

      // Flush written data to underlying storage (for streaming buffers)
      // Default: no-op for regular buffers
      GLZ_ALWAYS_INLINE static void flush([[maybe_unused]] Buffer& b, [[maybe_unused]] size_t written) noexcept {}
   };

   // Concept to check if a buffer type supports output streaming (flushing)
   template <class B>
   concept is_output_streaming = buffer_traits<std::remove_cvref_t<B>>::is_output_streaming;

   // Flush helper for streaming output buffers
   template <class B>
   GLZ_ALWAYS_INLINE void flush_buffer(B&& b, size_t written) noexcept
   {
      buffer_traits<std::remove_cvref_t<B>>::flush(b, written);
   }

   // Concept to check if a buffer type supports input streaming (refilling)
   template <class B>
   concept is_input_streaming = buffer_traits<std::remove_cvref_t<B>>::is_input_streaming;

   // Concept to check if a buffer type has bounded capacity (like std::array, std::span)
   // Note: Bounded buffers must be at least 512 bytes for reliable serialization.
   // Smaller buffers will return error_code::buffer_overflow.
   template <class B>
   concept has_bounded_capacity = buffer_traits<std::remove_cvref_t<B>>::has_bounded_capacity;

   // Refill helper for streaming input buffers
   // Returns true if buffer has data available after refill, false if EOF
   template <class B>
   GLZ_ALWAYS_INLINE bool refill_buffer(B&& b) noexcept
   {
      if constexpr (is_input_streaming<B>) {
         return buffer_traits<std::remove_cvref_t<B>>::refill(b);
      }
      else {
         return false; // Non-streaming buffers cannot refill
      }
   }

   // Consume helper for streaming input buffers
   // Marks bytes as consumed after successful parsing
   template <class B>
   GLZ_ALWAYS_INLINE void consume_buffer(B&& b, size_t bytes) noexcept
   {
      if constexpr (is_input_streaming<B>) {
         buffer_traits<std::remove_cvref_t<B>>::consume(b, bytes);
      }
   }

   // Specialization for raw char pointers
   // Raw pointers have unknown capacity and must trust the caller
   template <>
   struct buffer_traits<char*>
   {
      static constexpr bool is_resizable = false;
      static constexpr bool has_bounded_capacity = false;
      static constexpr bool is_output_streaming = false;
      static constexpr bool is_input_streaming = false;

      GLZ_ALWAYS_INLINE static constexpr size_t capacity(char*) noexcept { return std::numeric_limits<size_t>::max(); }

      GLZ_ALWAYS_INLINE static constexpr bool ensure_capacity(char*, size_t) noexcept
      {
         return true; // Trust caller
      }

      GLZ_ALWAYS_INLINE static constexpr void finalize(char*, size_t) noexcept {}
   };

   // Specialization for std::span
   template <class T, size_t Extent>
   struct buffer_traits<std::span<T, Extent>>
   {
      static constexpr bool is_resizable = false;
      static constexpr bool has_bounded_capacity = true;
      static constexpr bool is_output_streaming = false;
      static constexpr bool is_input_streaming = false;

      GLZ_ALWAYS_INLINE static constexpr size_t capacity(const std::span<T, Extent>& b) noexcept { return b.size(); }

      GLZ_ALWAYS_INLINE static constexpr bool ensure_capacity(const std::span<T, Extent>& b, size_t needed) noexcept
      {
         return needed <= b.size();
      }

      GLZ_ALWAYS_INLINE static constexpr void finalize(std::span<T, Extent>&, size_t) noexcept {}
   };

   // Specialization for std::array
   template <class T, size_t N>
   struct buffer_traits<std::array<T, N>>
   {
      static constexpr bool is_resizable = false;
      static constexpr bool has_bounded_capacity = true;
      static constexpr bool is_output_streaming = false;
      static constexpr bool is_input_streaming = false;
      static constexpr size_t static_capacity = N;

      GLZ_ALWAYS_INLINE static constexpr size_t capacity(const std::array<T, N>&) noexcept { return N; }

      GLZ_ALWAYS_INLINE static constexpr bool ensure_capacity(std::array<T, N>&, size_t needed) noexcept
      {
         return needed <= N;
      }

      GLZ_ALWAYS_INLINE static constexpr void finalize(std::array<T, N>&, size_t) noexcept {}
   };

   // Unified buffer space checking for write operations
   // Handles resizable buffers (resize), bounded buffers (error on overflow), and raw pointers (trust caller)
   template <class B>
   GLZ_ALWAYS_INLINE bool ensure_space(is_context auto& ctx, B& b,
                                       size_t required) noexcept(not vector_like<std::remove_cvref_t<B>>)
   {
      using Buffer = std::remove_cvref_t<B>;

      if constexpr (vector_like<Buffer>) {
         if (required > b.size()) [[unlikely]] {
            b.resize(2 * required);
         }
         return true;
      }
      else if constexpr (has_bounded_capacity<Buffer>) {
         if (required > buffer_traits<Buffer>::capacity(b)) [[unlikely]] {
            ctx.error = error_code::buffer_overflow;
            return false;
         }
         return true;
      }
      else {
         return true; // Raw pointer or other - trust caller
      }
   }

}
