// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "glaze/concepts/container_concepts.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/util/convert.hpp"

namespace glz
{
   template <class T, class V = std::remove_cvref_t<T>>
   concept byte_sized = sizeof(T) == 1 && (std::same_as<V, char> || std::same_as<V, std::byte>);

   template <uint32_t N, class B>
   GLZ_ALWAYS_INLINE void maybe_pad(B& b, size_t ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         if (const auto k = ix + N; k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }
   }

   template <class B>
   GLZ_ALWAYS_INLINE void maybe_pad(const size_t n, B& b, size_t ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         if (const auto k = ix + n; k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }
   }

   template <auto c>
   GLZ_ALWAYS_INLINE void assign_maybe_cast(auto& b, size_t& ix) noexcept
   {
      using V = std::decay_t<decltype(b[0])>;
      using C = std::decay_t<decltype(c)>;
      if constexpr (std::same_as<V, C>) {
         b[ix] = c;
      }
      else {
         b[ix] = static_cast<V>(c);
      }
   }

   GLZ_ALWAYS_INLINE void assign_maybe_cast(const byte_sized auto c, auto& b, size_t& ix) noexcept
   {
      using V = std::decay_t<decltype(b[0])>;
      using C = std::decay_t<decltype(c)>;
      if constexpr (std::same_as<V, C>) {
         b[ix] = c;
      }
      else {
         b[ix] = static_cast<V>(c);
      }
   }

   // Low-level buffer write primitives (dump functions)
   // ================================================
   // These functions write directly to the buffer WITHOUT bounds checking for bounded buffers.
   //
   // Contract:
   // - For resizable buffers (std::string, std::vector): Auto-resizes when Checked=true (default)
   // - For bounded buffers (std::array, std::span): Caller MUST call ensure_space() first
   // - For raw pointers: Caller assumes responsibility for sufficient space
   //
   // The Checked template parameter ONLY enables auto-resize for resizable buffers.
   // It does NOT enable bounds checking for bounded buffers - that would require ctx for error reporting
   // and would duplicate the ensure_space() logic. The padding model (ensure_space checks for
   // ix + n + write_padding_bytes) guarantees sufficient space for subsequent dump calls.

   template <bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump(const byte_sized auto c, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (Checked && vector_like<B>) {
         if (ix == b.size()) [[unlikely]] {
            b.resize(b.size() == 0 ? 128 : b.size() * 2);
         }
      }
      assign_maybe_cast(c, b, ix);
      ++ix;
   }

   template <auto c, bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump(B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (Checked && vector_like<B>) {
         if (ix == b.size()) [[unlikely]] {
            b.resize(b.size() == 0 ? 128 : b.size() * 2);
         }
      }
      assign_maybe_cast<c>(b, ix);
      ++ix;
   }

   template <string_literal str, bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump(B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      static constexpr auto s = str.sv();
      static constexpr auto n = s.size();

      if constexpr (vector_like<B>) {
         if constexpr (Checked) {
            const auto k = ix + n;
            if (k > b.size()) [[unlikely]] {
               b.resize(2 * k);
            }
         }
      }
      std::memcpy(&b[ix], s.data(), n);
      ix += n;
   }

   template <bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump(const sv str, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      const auto n = str.size();
      if constexpr (vector_like<B>) {
         if constexpr (Checked) {
            const auto k = ix + n;
            if (ix + n > b.size()) [[unlikely]] {
               b.resize(2 * k);
            }
         }
      }
      std::memcpy(&b[ix], str.data(), n);
      ix += n;
   }

   template <auto c, class B>
   [[deprecated("use dumpn(c, n, b, ix) instead of dumpn<c>(n, b, ix) to reduce template instantiations")]]
   GLZ_ALWAYS_INLINE void dumpn(size_t n, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         const auto k = ix + n;
         if (k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }
      std::memset(&b[ix], c, n);
      ix += n;
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dumpn(const byte_sized auto c, size_t n, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         const auto k = ix + n;
         if (k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }
      std::memset(&b[ix], c, n);
      ix += n;
   }

   template <auto c, class B>
   [[deprecated(
      "use dumpn_unchecked(c, n, b, ix) instead of dumpn_unchecked<c>(n, b, ix) to reduce template instantiations")]]
   GLZ_ALWAYS_INLINE void dumpn_unchecked(size_t n, B& b, size_t& ix) noexcept
   {
      std::memset(&b[ix], c, n);
      ix += n;
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dumpn_unchecked(const byte_sized auto c, size_t n, B& b, size_t& ix) noexcept
   {
      std::memset(&b[ix], c, n);
      ix += n;
   }

   template <char IndentChar, class B>
   [[deprecated(
      "use dump_newline_indent(c, n, b, ix) instead of dump_newline_indent<c>(n, b, ix) to reduce template "
      "instantiations")]]
   GLZ_ALWAYS_INLINE void dump_newline_indent(size_t n, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         if (const auto k = ix + n + write_padding_bytes; k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }

      assign_maybe_cast<'\n'>(b, ix);
      ++ix;
      std::memset(&b[ix], IndentChar, n);
      ix += n;
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_newline_indent(const byte_sized auto c, size_t n, B& b,
                                              size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         if (const auto k = ix + n + write_padding_bytes; k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }

      assign_maybe_cast('\n', b, ix);
      ++ix;
      std::memset(&b[ix], c, n);
      ix += n;
   }

   template <const sv& str, bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump(B& b, size_t& ix) noexcept(not vector_like<B> && not Checked)
   {
      static constexpr auto s = str;
      static constexpr auto n = s.size();

      if constexpr (vector_like<B>) {
         if constexpr (Checked) {
            const auto k = ix + n;
            if (k > b.size()) [[unlikely]] {
               b.resize(2 * k);
            }
         }
      }
      std::memcpy(&b[ix], s.data(), n);
      ix += n;
   }

   template <bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump_not_empty(const sv str, B& b, size_t& ix) noexcept(not vector_like<B> && not Checked)
   {
      const auto n = str.size();
      if constexpr (vector_like<B>) {
         if constexpr (Checked) {
            const auto k = ix + n;
            if (k > b.size()) [[unlikely]] {
               b.resize(2 * k);
            }
         }
      }
      std::memcpy(&b[ix], str.data(), n);
      ix += n;
   }

   template <bool Checked = true, class B>
   GLZ_ALWAYS_INLINE void dump_maybe_empty(const sv str, B& b, size_t& ix) noexcept(not vector_like<B> && not Checked)
   {
      const auto n = str.size();
      if (n) {
         if constexpr (vector_like<B>) {
            if constexpr (Checked) {
               const auto k = ix + n;
               if (k > b.size()) [[unlikely]] {
                  b.resize(2 * k);
               }
            }
         }
         std::memcpy(&b[ix], str.data(), n);
         ix += n;
      }
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump(const vector_like auto& bytes, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      const auto n = bytes.size();
      if constexpr (vector_like<B>) {
         const auto k = ix + n;
         if (k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }
      std::memcpy(&b[ix], bytes.data(), n);
      ix += n;
   }

   template <size_t N, class B>
   GLZ_ALWAYS_INLINE void dump(const std::array<uint8_t, N>& bytes, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      if constexpr (vector_like<B>) {
         const auto k = ix + N;
         if (k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }
      }
      std::memcpy(&b[ix], bytes.data(), N);
      ix += N;
   }
}
