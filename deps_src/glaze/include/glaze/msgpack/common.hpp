// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "glaze/core/context.hpp"
#include "glaze/core/error_category.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/inline.hpp"

namespace glz::msgpack
{
   // Core MessagePack marker bytes
   inline constexpr uint8_t nil = 0xC0;
   inline constexpr uint8_t bool_false = 0xC2;
   inline constexpr uint8_t bool_true = 0xC3;

   inline constexpr uint8_t bin8 = 0xC4;
   inline constexpr uint8_t bin16 = 0xC5;
   inline constexpr uint8_t bin32 = 0xC6;

   inline constexpr uint8_t ext8 = 0xC7;
   inline constexpr uint8_t ext16 = 0xC8;
   inline constexpr uint8_t ext32 = 0xC9;

   inline constexpr uint8_t float32 = 0xCA;
   inline constexpr uint8_t float64 = 0xCB;

   inline constexpr uint8_t uint8 = 0xCC;
   inline constexpr uint8_t uint16 = 0xCD;
   inline constexpr uint8_t uint32 = 0xCE;
   inline constexpr uint8_t uint64 = 0xCF;

   inline constexpr uint8_t int8 = 0xD0;
   inline constexpr uint8_t int16 = 0xD1;
   inline constexpr uint8_t int32 = 0xD2;
   inline constexpr uint8_t int64 = 0xD3;

   inline constexpr uint8_t fixext1 = 0xD4;
   inline constexpr uint8_t fixext2 = 0xD5;
   inline constexpr uint8_t fixext4 = 0xD6;
   inline constexpr uint8_t fixext8 = 0xD7;
   inline constexpr uint8_t fixext16 = 0xD8;

   inline constexpr uint8_t str8 = 0xD9;
   inline constexpr uint8_t str16 = 0xDA;
   inline constexpr uint8_t str32 = 0xDB;

   inline constexpr uint8_t array16 = 0xDC;
   inline constexpr uint8_t array32 = 0xDD;

   inline constexpr uint8_t map16 = 0xDE;
   inline constexpr uint8_t map32 = 0xDF;

   inline constexpr uint8_t positive_fixint_mask = 0x80;
   inline constexpr uint8_t negative_fixint_mask = 0xE0;

   inline constexpr uint8_t fixmap_mask = 0xF0;
   inline constexpr uint8_t fixmap_bits = 0x80;

   inline constexpr uint8_t fixarray_mask = 0xF0;
   inline constexpr uint8_t fixarray_bits = 0x90;

   inline constexpr uint8_t fixstr_mask = 0xE0;
   inline constexpr uint8_t fixstr_bits = 0xA0;

   GLZ_ALWAYS_INLINE constexpr bool is_positive_fixint(uint8_t tag) noexcept
   {
      return (tag & positive_fixint_mask) == 0;
   }

   GLZ_ALWAYS_INLINE constexpr bool is_negative_fixint(uint8_t tag) noexcept
   {
      return (tag & negative_fixint_mask) == negative_fixint_mask;
   }

   GLZ_ALWAYS_INLINE constexpr bool is_fixmap(uint8_t tag) noexcept { return (tag & fixmap_mask) == fixmap_bits; }

   GLZ_ALWAYS_INLINE constexpr bool is_fixarray(uint8_t tag) noexcept { return (tag & fixarray_mask) == fixarray_bits; }

   GLZ_ALWAYS_INLINE constexpr bool is_fixstr(uint8_t tag) noexcept { return (tag & fixstr_mask) == fixstr_bits; }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_uint8(uint8_t value, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      dump(static_cast<std::byte>(value), b, ix);
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_uint16(uint16_t value, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      dump(static_cast<std::byte>(value >> 8), b, ix);
      dump(static_cast<std::byte>(value & 0xFF), b, ix);
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_uint32(uint32_t value, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      dump(static_cast<std::byte>(value >> 24), b, ix);
      dump(static_cast<std::byte>((value >> 16) & 0xFF), b, ix);
      dump(static_cast<std::byte>((value >> 8) & 0xFF), b, ix);
      dump(static_cast<std::byte>(value & 0xFF), b, ix);
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_uint64(uint64_t value, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      dump(static_cast<std::byte>(value >> 56), b, ix);
      dump(static_cast<std::byte>((value >> 48) & 0xFF), b, ix);
      dump(static_cast<std::byte>((value >> 40) & 0xFF), b, ix);
      dump(static_cast<std::byte>((value >> 32) & 0xFF), b, ix);
      dump(static_cast<std::byte>((value >> 24) & 0xFF), b, ix);
      dump(static_cast<std::byte>((value >> 16) & 0xFF), b, ix);
      dump(static_cast<std::byte>((value >> 8) & 0xFF), b, ix);
      dump(static_cast<std::byte>(value & 0xFF), b, ix);
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_float32(float value, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      const uint32_t bits = std::bit_cast<uint32_t>(value);
      dump_uint32(bits, b, ix);
   }

   template <class B>
   GLZ_ALWAYS_INLINE void dump_float64(double value, B& b, size_t& ix) noexcept(not vector_like<B>)
   {
      const uint64_t bits = std::bit_cast<uint64_t>(value);
      dump_uint64(bits, b, ix);
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_uint8(is_context auto& ctx, It& it, const It& end, uint8_t& out) noexcept
   {
      if (it >= end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return false;
      }
      out = static_cast<uint8_t>(*it);
      ++it;
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_uint16(is_context auto& ctx, It& it, const It& end, uint16_t& out) noexcept
   {
      if ((it + 2) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return false;
      }
      const auto b0 = static_cast<uint8_t>(it[0]);
      const auto b1 = static_cast<uint8_t>(it[1]);
      it += 2;
      out = (uint16_t(b0) << 8) | uint16_t(b1);
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_uint32(is_context auto& ctx, It& it, const It& end, uint32_t& out) noexcept
   {
      if ((it + 4) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return false;
      }
      uint32_t value = 0;
      value |= uint32_t(static_cast<uint8_t>(it[0])) << 24;
      value |= uint32_t(static_cast<uint8_t>(it[1])) << 16;
      value |= uint32_t(static_cast<uint8_t>(it[2])) << 8;
      value |= uint32_t(static_cast<uint8_t>(it[3]));
      it += 4;
      out = value;
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_uint64(is_context auto& ctx, It& it, const It& end, uint64_t& out) noexcept
   {
      if ((it + 8) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return false;
      }
      uint64_t value = 0;
      value |= uint64_t(static_cast<uint8_t>(it[0])) << 56;
      value |= uint64_t(static_cast<uint8_t>(it[1])) << 48;
      value |= uint64_t(static_cast<uint8_t>(it[2])) << 40;
      value |= uint64_t(static_cast<uint8_t>(it[3])) << 32;
      value |= uint64_t(static_cast<uint8_t>(it[4])) << 24;
      value |= uint64_t(static_cast<uint8_t>(it[5])) << 16;
      value |= uint64_t(static_cast<uint8_t>(it[6])) << 8;
      value |= uint64_t(static_cast<uint8_t>(it[7]));
      it += 8;
      out = value;
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_ext_header(is_context auto& ctx, uint8_t tag, It& it, const It& end, size_t& length,
                                          int8_t& type) noexcept
   {
      switch (tag) {
      case fixext1:
         length = 1;
         break;
      case fixext2:
         length = 2;
         break;
      case fixext4:
         length = 4;
         break;
      case fixext8:
         length = 8;
         break;
      case fixext16:
         length = 16;
         break;
      case ext8: {
         uint8_t len8{};
         if (!read_uint8(ctx, it, end, len8)) {
            return false;
         }
         length = len8;
         break;
      }
      case ext16: {
         uint16_t len16{};
         if (!read_uint16(ctx, it, end, len16)) {
            return false;
         }
         length = len16;
         break;
      }
      case ext32: {
         uint32_t len32{};
         if (!read_uint32(ctx, it, end, len32)) {
            return false;
         }
         length = len32;
         break;
      }
      default:
         ctx.error = error_code::syntax_error;
         return false;
      }

      uint8_t type_byte{};
      if (!read_uint8(ctx, it, end, type_byte)) {
         return false;
      }
      type = static_cast<int8_t>(type_byte);

      if ((it + length) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return false;
      }

      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_float32(is_context auto& ctx, It& it, const It& end, float& out) noexcept
   {
      uint32_t bits{};
      if (!read_uint32(ctx, it, end, bits)) {
         return false;
      }
      out = std::bit_cast<float>(bits);
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_float64(is_context auto& ctx, It& it, const It& end, double& out) noexcept
   {
      uint64_t bits{};
      if (!read_uint64(ctx, it, end, bits)) {
         return false;
      }
      out = std::bit_cast<double>(bits);
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool skip_bytes(is_context auto& ctx, It& it, const It& end, size_t n) noexcept
   {
      if ((it + n) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return false;
      }
      it += n;
      return true;
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_str_length(is_context auto& ctx, uint8_t tag, It& it, const It& end,
                                          size_t& out) noexcept
   {
      if (is_fixstr(tag)) {
         out = tag & 0x1F;
         return true;
      }
      switch (tag) {
      case str8: {
         uint8_t len{};
         if (!read_uint8(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      case str16: {
         uint16_t len{};
         if (!read_uint16(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      case str32: {
         uint32_t len{};
         if (!read_uint32(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      default:
         ctx.error = error_code::syntax_error;
         return false;
      }
   }
   template <class Range>
   inline constexpr bool binary_range_v = contiguous_byte_range<std::remove_cvref_t<Range>>;

   struct ext
   {
      int8_t type{};
      std::vector<std::byte> data{};

      ext() = default;

      ext(int8_t t, std::vector<std::byte> payload) : type(t), data(std::move(payload)) {}

      ext(int8_t t, std::initializer_list<std::byte> payload) : type(t), data(payload) {}

      [[nodiscard]] bool empty() const noexcept { return data.empty(); }

      bool operator==(const ext&) const = default;
   };

   // MessagePack timestamp extension (type -1)
   // Supports the three timestamp formats defined in the MessagePack spec:
   // - Timestamp 32: fixext 4, seconds only (uint32)
   // - Timestamp 64: fixext 8, nanoseconds (30-bit) + seconds (34-bit)
   // - Timestamp 96: ext 8 with 12 bytes, nanoseconds (uint32) + seconds (int64)
   inline constexpr int8_t timestamp_type = -1;

   struct timestamp
   {
      int64_t seconds{};
      uint32_t nanoseconds{};

      timestamp() = default;

      timestamp(int64_t sec, uint32_t nsec = 0) : seconds(sec), nanoseconds(nsec) {}

      bool operator==(const timestamp&) const = default;
      auto operator<=>(const timestamp&) const = default;
   };

   template <class It>
   GLZ_ALWAYS_INLINE bool read_bin_length(is_context auto& ctx, uint8_t tag, It& it, const It& end,
                                          size_t& out) noexcept
   {
      switch (tag) {
      case bin8: {
         uint8_t len{};
         if (!read_uint8(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      case bin16: {
         uint16_t len{};
         if (!read_uint16(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      case bin32: {
         uint32_t len{};
         if (!read_uint32(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      default:
         ctx.error = error_code::syntax_error;
         return false;
      }
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_array_length(is_context auto& ctx, uint8_t tag, It& it, const It& end,
                                            size_t& out) noexcept
   {
      if (is_fixarray(tag)) {
         out = tag & 0x0F;
         return true;
      }
      switch (tag) {
      case array16: {
         uint16_t len{};
         if (!read_uint16(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      case array32: {
         uint32_t len{};
         if (!read_uint32(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      default:
         ctx.error = error_code::syntax_error;
         return false;
      }
   }

   template <class It>
   GLZ_ALWAYS_INLINE bool read_map_length(is_context auto& ctx, uint8_t tag, It& it, const It& end,
                                          size_t& out) noexcept
   {
      if (is_fixmap(tag)) {
         out = tag & 0x0F;
         return true;
      }
      switch (tag) {
      case map16: {
         uint16_t len{};
         if (!read_uint16(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      case map32: {
         uint32_t len{};
         if (!read_uint32(ctx, it, end, len)) {
            return false;
         }
         out = len;
         return true;
      }
      default:
         ctx.error = error_code::syntax_error;
         return false;
      }
   }
}
