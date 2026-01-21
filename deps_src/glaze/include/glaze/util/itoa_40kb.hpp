// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// Maximum speed integer to ascii conversion using 40KB lookup table
// Use to_chars from itoa.hpp for a smaller binary with 400B tables
// Based on techniques from:
// - https://github.com/ibireme/c_numconv_benchmark/blob/master/src/itoa/itoa_yy.c
// - https://github.com/RealTimeChris/Jsonifier (IToStr.hpp)

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include "glaze/util/inline.hpp"
#include "glaze/util/itoa.hpp" // For shared char_table and digit_pairs

namespace glz
{
   namespace itoa_40kb_impl
   {
      // Reuse tables from itoa.hpp to avoid duplication
      using itoa_impl::char_table;
      using itoa_impl::digit_pairs;

      // 4-digit uint32_t table for direct 4-byte memcpy (10000 × 4 = 40KB)
      // Generated at compile-time with correct byte order for target endianness
      inline constexpr auto digit_quads = []() consteval {
         std::array<uint32_t, 10000> table{};
         for (uint32_t i = 0; i < 10000; ++i) {
            const auto d0 = uint32_t('0' + i / 1000);
            const auto d1 = uint32_t('0' + (i / 100) % 10);
            const auto d2 = uint32_t('0' + (i / 10) % 10);
            const auto d3 = uint32_t('0' + i % 10);
            if constexpr (std::endian::native == std::endian::little) {
               table[i] = d0 | (d1 << 8) | (d2 << 16) | (d3 << 24);
            }
            else {
               table[i] = (d0 << 24) | (d1 << 16) | (d2 << 8) | d3;
            }
         }
         return table;
      }();

      // 128-bit multiplication for efficient division by 100000000
#if defined(__SIZEOF_INT128__)
      GLZ_ALWAYS_INLINE uint64_t div_1e8(uint64_t value) noexcept
      {
         constexpr __uint128_t multiplier = 0xabcc77118461cefdULL;
         constexpr uint64_t shift = 90;
         return static_cast<uint64_t>((static_cast<__uint128_t>(value) * multiplier) >> shift);
      }
#else
      GLZ_ALWAYS_INLINE uint64_t div_1e8(uint64_t value) noexcept { return value / 100000000ULL; }
#endif

      // ==================== uint32_t implementations ====================

      GLZ_ALWAYS_INLINE char* u32_2(char* buf, uint32_t val) noexcept
      {
         const uint64_t lz = val < 10;
         // Use OR since val*2 is always even (bit 0 is 0)
         std::memcpy(buf, char_table + ((val * 2) | lz), 2);
         buf -= lz;
         return buf + 2;
      }

      GLZ_ALWAYS_INLINE char* u32_4(char* buf, uint32_t val) noexcept
      {
         const uint32_t aa = (val * 5243) >> 19;
         const uint64_t lz = val < 1000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[val - aa * 100], 2);
         return buf + 4;
      }

      GLZ_ALWAYS_INLINE char* u32_6(char* buf, uint32_t val) noexcept
      {
         const uint32_t aa = static_cast<uint32_t>((static_cast<uint64_t>(val) * 429497ULL) >> 32);
         const uint64_t lz = val < 100000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_quads[val - aa * 10000], 4);
         return buf + 6;
      }

      GLZ_ALWAYS_INLINE char* u32_8(char* buf, uint32_t val) noexcept
      {
         const uint64_t aabb = (static_cast<uint64_t>(val) * 109951163ULL) >> 40;
         const uint64_t aa = (aabb * 5243ULL) >> 19;
         const uint64_t lz = val < 10000000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[aabb - aa * 100], 2);
         std::memcpy(buf + 4, &digit_quads[val - aabb * 10000], 4);
         return buf + 8;
      }

      GLZ_ALWAYS_INLINE char* u32_10(char* buf, uint32_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         const uint64_t lz = high < 10;
         std::memcpy(buf, char_table + ((high * 2) | lz), 2);
         buf -= lz;
         const uint64_t aabb = (low * 109951163ULL) >> 40;
         const uint64_t ccdd = low - aabb * 10000;
         std::memcpy(buf + 2, &digit_quads[aabb], 4);
         std::memcpy(buf + 6, &digit_quads[ccdd], 4);
         return buf + 10;
      }

      // ==================== uint64_t implementations ====================

      GLZ_ALWAYS_INLINE char* u64_2(char* buf, uint64_t val) noexcept
      {
         const uint64_t lz = val < 10;
         std::memcpy(buf, char_table + ((val * 2) | lz), 2);
         buf -= lz;
         return buf + 2;
      }

      GLZ_ALWAYS_INLINE char* u64_4(char* buf, uint64_t val) noexcept
      {
         const uint64_t aa = (val * 5243ULL) >> 19;
         const uint64_t lz = val < 1000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[val - aa * 100], 2);
         return buf + 4;
      }

      GLZ_ALWAYS_INLINE char* u64_6(char* buf, uint64_t val) noexcept
      {
         const uint64_t aa = (val * 429497ULL) >> 32;
         const uint64_t lz = val < 100000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_quads[val - aa * 10000], 4);
         return buf + 6;
      }

      GLZ_ALWAYS_INLINE char* u64_8(char* buf, uint64_t val) noexcept
      {
         const uint64_t aabb = (val * 109951163ULL) >> 40;
         const uint64_t aa = (aabb * 5243ULL) >> 19;
         const uint64_t lz = val < 10000000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[aabb - aa * 100], 2);
         std::memcpy(buf + 4, &digit_quads[val - aabb * 10000], 4);
         return buf + 8;
      }

      GLZ_ALWAYS_INLINE char* u64_10(char* buf, uint64_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         const uint64_t lz = high < 10;
         std::memcpy(buf, char_table + ((high * 2) | lz), 2);
         buf -= lz;
         const uint64_t aabb = (low * 109951163ULL) >> 40;
         const uint64_t ccdd = low - aabb * 10000;
         std::memcpy(buf + 2, &digit_quads[aabb], 4);
         std::memcpy(buf + 6, &digit_quads[ccdd], 4);
         return buf + 10;
      }

      GLZ_ALWAYS_INLINE char* u64_12(char* buf, uint64_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         const uint64_t aa = (high * 5243ULL) >> 19;
         const uint64_t lz = aa < 10;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[high - aa * 100], 2);
         const uint64_t aabb = (low * 109951163ULL) >> 40;
         const uint64_t ccdd = low - aabb * 10000;
         std::memcpy(buf + 4, &digit_quads[aabb], 4);
         std::memcpy(buf + 8, &digit_quads[ccdd], 4);
         return buf + 12;
      }

      GLZ_ALWAYS_INLINE char* u64_14(char* buf, uint64_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         const uint64_t aa = (high * 429497ULL) >> 32;
         const uint64_t lz = aa < 10;
         const uint64_t bbcc = high - aa * 10000;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_quads[bbcc], 4);
         const uint64_t aabb = (low * 109951163ULL) >> 40;
         const uint64_t ccdd = low - aabb * 10000;
         std::memcpy(buf + 6, &digit_quads[aabb], 4);
         std::memcpy(buf + 10, &digit_quads[ccdd], 4);
         return buf + 14;
      }

      GLZ_ALWAYS_INLINE char* u64_16(char* buf, uint64_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         uint64_t aabb = (high * 109951163ULL) >> 40;
         uint64_t ccdd = high - aabb * 10000;
         const uint64_t aa = (aabb * 5243ULL) >> 19;
         const uint64_t lz = aa < 10;
         const uint64_t bb = aabb - aa * 100;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[bb], 2);
         std::memcpy(buf + 4, &digit_quads[ccdd], 4);
         aabb = (low * 109951163ULL) >> 40;
         ccdd = low - aabb * 10000;
         std::memcpy(buf + 8, &digit_quads[aabb], 4);
         std::memcpy(buf + 12, &digit_quads[ccdd], 4);
         return buf + 16;
      }

      GLZ_ALWAYS_INLINE char* u64_18(char* buf, uint64_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         const uint64_t high10 = div_1e8(high);
         const uint64_t low10 = high - high10 * 100000000;
         const uint64_t lz = high10 < 10;
         std::memcpy(buf, char_table + ((high10 * 2) | lz), 2);
         buf -= lz;
         uint64_t aabb = (low10 * 109951163ULL) >> 40;
         uint64_t ccdd = low10 - aabb * 10000;
         std::memcpy(buf + 2, &digit_quads[aabb], 4);
         std::memcpy(buf + 6, &digit_quads[ccdd], 4);
         aabb = (low * 109951163ULL) >> 40;
         ccdd = low - aabb * 10000;
         std::memcpy(buf + 10, &digit_quads[aabb], 4);
         std::memcpy(buf + 14, &digit_quads[ccdd], 4);
         return buf + 18;
      }

      GLZ_ALWAYS_INLINE char* u64_20(char* buf, uint64_t val) noexcept
      {
         const uint64_t high = div_1e8(val);
         const uint64_t low = val - high * 100000000;
         const uint64_t high12 = div_1e8(high);
         const uint64_t low12 = high - high12 * 100000000;
         const uint64_t aa = (high12 * 5243ULL) >> 19;
         const uint64_t lz = aa < 10;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[high12 - aa * 100], 2);
         uint64_t aabb = (low12 * 109951163ULL) >> 40;
         uint64_t ccdd = low12 - aabb * 10000;
         std::memcpy(buf + 4, &digit_quads[aabb], 4);
         std::memcpy(buf + 8, &digit_quads[ccdd], 4);
         aabb = (low * 109951163ULL) >> 40;
         ccdd = low - aabb * 10000;
         std::memcpy(buf + 12, &digit_quads[aabb], 4);
         std::memcpy(buf + 16, &digit_quads[ccdd], 4);
         return buf + 20;
      }
   } // namespace itoa_40kb_impl

   // ==================== Public API ====================
   // Maximum speed integer to chars (uses 40KB digit_quads table)
   // Use to_chars from itoa.hpp for smaller binary size

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, uint32_t>
   GLZ_ALWAYS_INLINE char* to_chars_40kb(char* buf, T val) noexcept
   {
      using namespace itoa_40kb_impl;
      if (val < 10000) {
         if (val < 100)
            return u32_2(buf, val);
         else
            return u32_4(buf, val);
      }
      else if (val < 100000000) {
         if (val < 1000000)
            return u32_6(buf, val);
         else
            return u32_8(buf, val);
      }
      else {
         return u32_10(buf, val);
      }
   }

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, int32_t>
   GLZ_ALWAYS_INLINE char* to_chars_40kb(char* buf, T val) noexcept
   {
      *buf = '-';
      return to_chars_40kb(buf + (val < 0),
                           static_cast<uint32_t>((static_cast<uint32_t>(val) ^ (val >> 31)) - (val >> 31)));
   }

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, uint64_t>
   GLZ_ALWAYS_INLINE char* to_chars_40kb(char* buf, T val) noexcept
   {
      using namespace itoa_40kb_impl;
      if (val < 10000) {
         if (val < 100)
            return u64_2(buf, val);
         else
            return u64_4(buf, val);
      }
      else if (val < 100000000) {
         if (val < 1000000)
            return u64_6(buf, val);
         else
            return u64_8(buf, val);
      }
      else if (val < 1000000000000ULL) {
         if (val < 10000000000ULL)
            return u64_10(buf, val);
         else
            return u64_12(buf, val);
      }
      else if (val < 10000000000000000ULL) {
         if (val < 100000000000000ULL)
            return u64_14(buf, val);
         else
            return u64_16(buf, val);
      }
      else if (val < 1000000000000000000ULL) {
         return u64_18(buf, val);
      }
      else {
         return u64_20(buf, val);
      }
   }

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, int64_t>
   GLZ_ALWAYS_INLINE char* to_chars_40kb(char* buf, T val) noexcept
   {
      *buf = '-';
      return to_chars_40kb(buf + (val < 0),
                           static_cast<uint64_t>((static_cast<uint64_t>(val) ^ (val >> 63)) - (val >> 63)));
   }

} // namespace glz
