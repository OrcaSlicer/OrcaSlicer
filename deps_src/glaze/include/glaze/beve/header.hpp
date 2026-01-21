// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "glaze/core/context.hpp"
#include "glaze/util/inline.hpp"

namespace glz
{
   GLZ_ALWAYS_INLINE bool invalid_end(is_context auto& ctx, auto&& it, auto end) noexcept
   {
      if (it >= end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return true;
      }
      else [[likely]] {
         return false;
      }
   }

   // Byteswaps a numeric value in-place for little-endian wire format compatibility.
   // Call ONLY inside: if constexpr (std::endian::native == std::endian::big) blocks.
   // On little-endian systems, this function is never instantiated (zero overhead).
   template <class T>
      requires(std::integral<T> || std::floating_point<T> || std::is_enum_v<T>)
   GLZ_ALWAYS_INLINE void byteswap_le(T& value) noexcept
   {
      if constexpr (std::is_enum_v<T>) {
         using U = std::underlying_type_t<T>;
         if constexpr (sizeof(U) > 1) {
            U underlying_val;
            std::memcpy(&underlying_val, &value, sizeof(T));
            underlying_val = std::byteswap(underlying_val);
            std::memcpy(&value, &underlying_val, sizeof(T));
         }
      }
      else if constexpr (std::integral<T>) {
         if constexpr (sizeof(T) > 1) {
            value = std::byteswap(value);
         }
      }
      else if constexpr (std::floating_point<T>) {
         using Int = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
         Int int_val;
         std::memcpy(&int_val, &value, sizeof(T));
         int_val = std::byteswap(int_val);
         std::memcpy(&value, &int_val, sizeof(T));
      }
   }
}

namespace glz::tag
{
   constexpr uint8_t null = 0;
   constexpr uint8_t boolean = 0b00001'000;
   constexpr uint8_t number = 1;
   constexpr uint8_t string = 2;
   constexpr uint8_t object = 3;
   constexpr uint8_t typed_array = 4;
   constexpr uint8_t generic_array = 5;
   constexpr uint8_t extensions = 6;

   // Data delimiter for separating multiple BEVE values in a stream/buffer
   // Used like NDJSON's newline delimiter - when converted to JSON outputs '\n'
   constexpr uint8_t delimiter = 0b00000'110; // extensions type (6) with subtype 0

   constexpr uint8_t bool_false = 0b000'01'000;
   constexpr uint8_t bool_true = 0b000'11'000;

   constexpr uint8_t i8 = 0b000'01'001;
   constexpr uint8_t i16 = 0b001'01'001;
   constexpr uint8_t i32 = 0b010'01'001;
   constexpr uint8_t i64 = 0b011'01'001;
   constexpr uint8_t i128 = 0b100'01'001;

   constexpr uint8_t u8 = 0b000'10'001;
   constexpr uint8_t u16 = 0b001'10'001;
   constexpr uint8_t u32 = 0b010'10'001;
   constexpr uint8_t u64 = 0b011'10'001;
   constexpr uint8_t u128 = 0b100'10'001;

   constexpr uint8_t bf16 = 0b000'00'001; // brain float
   constexpr uint8_t f16 = 0b001'00'001;
   constexpr uint8_t f32 = 0b010'00'001;
   constexpr uint8_t f64 = 0b011'00'001;
   constexpr uint8_t f128 = 0b100'00'001;
}

namespace glz
{
   template <class T>
   constexpr uint8_t byte_count = uint8_t(std::bit_width(sizeof(T)) - 1);

   inline constexpr std::array<uint8_t, 8> byte_count_lookup{1, 2, 4, 8, 16, 32, 64, 128};

   [[nodiscard]] GLZ_ALWAYS_INLINE constexpr size_t int_from_compressed(auto&& ctx, auto&& it, auto end) noexcept
   {
      if (it >= end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return 0;
      }

      uint8_t header;
      std::memcpy(&header, it, 1);
      const uint8_t config = header & 0b000000'11;

      if ((it + byte_count_lookup[config]) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return 0;
      }

      switch (config) {
      case 0:
         ++it;
         return header >> 2;
      case 1: {
         uint16_t h;
         std::memcpy(&h, it, 2);
         if constexpr (std::endian::native == std::endian::big) {
            h = std::byteswap(h);
         }
         it += 2;
         return h >> 2;
      }
      case 2: {
         uint32_t h;
         std::memcpy(&h, it, 4);
         if constexpr (std::endian::native == std::endian::big) {
            h = std::byteswap(h);
         }
         it += 4;
         return h >> 2;
      }
      case 3: {
         // On 32-bit systems it's impossible to address more than 4 GBiB of memory, so we should verify first if we are
         // running in 64-bit mode here
         if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
            uint64_t h;
            std::memcpy(&h, it, 8);
            if constexpr (std::endian::native == std::endian::big) {
               h = std::byteswap(h);
            }
            it += 8;
            h = h >> 2;
            static constexpr uint64_t safety_limit = 1ull << 48; // 2^48
            if (h > safety_limit) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return 0;
            }
            return h;
         }
         else {
            // 8-byte length encoding not supported on 32-bit systems
            ctx.error = error_code::invalid_length;
            return 0;
         }
      }
      default:
         ctx.error = error_code::syntax_error;
         return 0;
      }
   }

   GLZ_ALWAYS_INLINE constexpr void skip_compressed_int(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      if (invalid_end(ctx, it, end)) {
         return;
      }

      uint8_t header;
      std::memcpy(&header, it, 1);
      const uint8_t config = header & 0b000000'11;

      if ((it + byte_count_lookup[config]) > end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return;
      }

      switch (config) {
      case 0:
         ++it;
         return;
      case 1: {
         it += 2;
         return;
      }
      case 2: {
         it += 4;
         return;
      }
      case 3: {
         it += 8;
         return;
      }
      default:
         return;
      }
   }
}
