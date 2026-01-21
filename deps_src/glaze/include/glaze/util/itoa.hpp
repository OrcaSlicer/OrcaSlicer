// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// Default integer to ascii conversion
// Uses 400 bytes of lookup tables (char_table + digit_pairs)
// For maximum speed with 40KB tables, use itoa_40kb.hpp instead

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>

#include "glaze/util/inline.hpp"

namespace glz
{
   namespace itoa_impl
   {
      // 2-digit character pairs table (200 bytes) - cache-line aligned
      alignas(64) inline constexpr char char_table[200] = {
         '0', '0', '0', '1', '0', '2', '0', '3', '0', '4', '0', '5', '0', '6', '0', '7', '0', '8', '0', '9',
         '1', '0', '1', '1', '1', '2', '1', '3', '1', '4', '1', '5', '1', '6', '1', '7', '1', '8', '1', '9',
         '2', '0', '2', '1', '2', '2', '2', '3', '2', '4', '2', '5', '2', '6', '2', '7', '2', '8', '2', '9',
         '3', '0', '3', '1', '3', '2', '3', '3', '3', '4', '3', '5', '3', '6', '3', '7', '3', '8', '3', '9',
         '4', '0', '4', '1', '4', '2', '4', '3', '4', '4', '4', '5', '4', '6', '4', '7', '4', '8', '4', '9',
         '5', '0', '5', '1', '5', '2', '5', '3', '5', '4', '5', '5', '5', '6', '5', '7', '5', '8', '5', '9',
         '6', '0', '6', '1', '6', '2', '6', '3', '6', '4', '6', '5', '6', '6', '6', '7', '6', '8', '6', '9',
         '7', '0', '7', '1', '7', '2', '7', '3', '7', '4', '7', '5', '7', '6', '7', '7', '7', '8', '7', '9',
         '8', '0', '8', '1', '8', '2', '8', '3', '8', '4', '8', '5', '8', '6', '8', '7', '8', '8', '8', '9',
         '9', '0', '9', '1', '9', '2', '9', '3', '9', '4', '9', '5', '9', '6', '9', '7', '9', '8', '9', '9'};

      // 2-digit uint16_t table for direct 2-byte memcpy (100 × 2 = 200 bytes)
      // Generated at compile-time with correct byte order for target endianness
      inline constexpr auto digit_pairs = []() consteval {
         std::array<uint16_t, 100> table{};
         for (uint32_t i = 0; i < 100; ++i) {
            const auto d0 = uint16_t('0' + i / 10);
            const auto d1 = uint16_t('0' + i % 10);
            if constexpr (std::endian::native == std::endian::little) {
               table[i] = d0 | (d1 << 8);
            }
            else {
               table[i] = (d0 << 8) | d1;
            }
         }
         return table;
      }();

      // 128-bit multiplication for efficient division by 100000000
      // Modern compilers optimize regular division the same way, but this ensures it
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
         const uint32_t lz = val < 10;
         std::memcpy(buf, char_table + ((val * 2) | lz), 2);
         return buf + 2 - lz;
      }

      GLZ_ALWAYS_INLINE char* u32_4(char* buf, uint32_t val) noexcept
      {
         const uint32_t aa = (val * 5243) >> 19; // val / 100
         const uint32_t lz = aa < 10;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[val - aa * 100], 2);
         return buf + 4;
      }

      GLZ_ALWAYS_INLINE char* u32_6(char* buf, uint32_t val) noexcept
      {
         const uint32_t aa = uint32_t((uint64_t(val) * 429497) >> 32); // val / 10000
         const uint32_t bbcc = val - aa * 10000;
         const uint32_t bb = (bbcc * 5243) >> 19; // bbcc / 100
         const uint32_t lz = aa < 10;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[bb], 2);
         std::memcpy(buf + 4, &digit_pairs[bbcc - bb * 100], 2);
         return buf + 6;
      }

      GLZ_ALWAYS_INLINE char* u32_8(char* buf, uint32_t val) noexcept
      {
         const uint32_t aabb = uint32_t((uint64_t(val) * 109951163) >> 40); // val / 10000
         const uint32_t ccdd = val - aabb * 10000;
         const uint32_t aa = (aabb * 5243) >> 19; // aabb / 100
         const uint32_t cc = (ccdd * 5243) >> 19; // ccdd / 100
         const uint32_t lz = aa < 10;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[aabb - aa * 100], 2);
         std::memcpy(buf + 4, &digit_pairs[cc], 2);
         std::memcpy(buf + 6, &digit_pairs[ccdd - cc * 100], 2);
         return buf + 8;
      }

      GLZ_ALWAYS_INLINE char* u32_10(char* buf, uint32_t val) noexcept
      {
         const uint32_t aabbcc = uint32_t((uint64_t(val) * 3518437209ul) >> 45); // val / 10000
         const uint32_t aa = uint32_t((uint64_t(aabbcc) * 429497) >> 32); // aabbcc / 10000
         const uint32_t ddee = val - aabbcc * 10000;
         const uint32_t bbcc = aabbcc - aa * 10000;
         const uint32_t bb = (bbcc * 5243) >> 19;
         const uint32_t dd = (ddee * 5243) >> 19;
         const uint32_t lz = aa < 10;
         std::memcpy(buf, char_table + ((aa * 2) | lz), 2);
         buf -= lz;
         std::memcpy(buf + 2, &digit_pairs[bb], 2);
         std::memcpy(buf + 4, &digit_pairs[bbcc - bb * 100], 2);
         std::memcpy(buf + 6, &digit_pairs[dd], 2);
         std::memcpy(buf + 8, &digit_pairs[ddee - dd * 100], 2);
         return buf + 10;
      }

      // Fixed 8-digit output (no leading zero handling)
      GLZ_ALWAYS_INLINE char* u64_len_8(char* buf, uint32_t val) noexcept
      {
         const uint32_t aabb = uint32_t((uint64_t(val) * 109951163) >> 40);
         const uint32_t ccdd = val - aabb * 10000;
         const uint32_t aa = (aabb * 5243) >> 19;
         const uint32_t cc = (ccdd * 5243) >> 19;
         std::memcpy(buf, &digit_pairs[aa], 2);
         std::memcpy(buf + 2, &digit_pairs[aabb - aa * 100], 2);
         std::memcpy(buf + 4, &digit_pairs[cc], 2);
         std::memcpy(buf + 6, &digit_pairs[ccdd - cc * 100], 2);
         return buf + 8;
      }

      // Fixed 4-digit output (no leading zero handling)
      GLZ_ALWAYS_INLINE char* u64_len_4(char* buf, uint32_t val) noexcept
      {
         const uint32_t aa = (val * 5243) >> 19;
         std::memcpy(buf, &digit_pairs[aa], 2);
         std::memcpy(buf + 2, &digit_pairs[val - aa * 100], 2);
         return buf + 4;
      }

      // 1-8 digits with leading zero handling
      GLZ_ALWAYS_INLINE char* u64_len_1_8(char* buf, uint32_t val) noexcept
      {
         if (val < 100) {
            return u32_2(buf, val);
         }
         else if (val < 10000) {
            return u32_4(buf, val);
         }
         else if (val < 1000000) {
            return u32_6(buf, val);
         }
         else {
            return u32_8(buf, val);
         }
      }

      // 5-8 digits with leading zero handling
      GLZ_ALWAYS_INLINE char* u64_len_5_8(char* buf, uint32_t val) noexcept
      {
         if (val < 1000000) {
            return u32_6(buf, val);
         }
         else {
            return u32_8(buf, val);
         }
      }
   } // namespace itoa_impl

   // ==================== Public API ====================
   // Default integer to chars (uses 400 bytes of tables)
   // For maximum speed with 40KB tables, use to_chars_40kb from itoa_40kb.hpp

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, uint32_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      using namespace itoa_impl;
      if (val < 100) {
         return u32_2(buf, val);
      }
      else if (val < 10000) {
         return u32_4(buf, val);
      }
      else if (val < 1000000) {
         return u32_6(buf, val);
      }
      else if (val < 100000000) {
         return u32_8(buf, val);
      }
      else {
         return u32_10(buf, val);
      }
   }

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, int32_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      *buf = '-';
      return to_chars(buf + (val < 0), uint32_t(val ^ (val >> 31)) - (val >> 31));
   }

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, uint64_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      using namespace itoa_impl;
      if (val < 100000000) {
         return u64_len_1_8(buf, uint32_t(val));
      }
      else if (val < 10000000000000000ull) {
         const uint64_t hgh = div_1e8(val);
         const uint32_t low = uint32_t(val - hgh * 100000000);
         buf = u64_len_1_8(buf, uint32_t(hgh));
         return u64_len_8(buf, low);
      }
      else {
         const uint64_t tmp = div_1e8(val);
         const uint32_t low = uint32_t(val - tmp * 100000000);
         const uint32_t hgh = uint32_t(tmp / 10000);
         const uint32_t mid = uint32_t(tmp - uint64_t(hgh) * 10000);
         buf = u64_len_5_8(buf, hgh);
         buf = u64_len_4(buf, mid);
         return u64_len_8(buf, low);
      }
   }

   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, int64_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      *buf = '-';
      return to_chars(buf + (val < 0), uint64_t(val ^ (val >> 63)) - (val >> 63));
   }

   // ==================== Small integer types ====================
   // Optimized for 8-bit and 16-bit integers with compact code paths
   // The 40KB digit_quads table doesn't help for these small ranges

   // uint8_t: 0-255 (1-3 digits)
   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, uint8_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      using namespace itoa_impl;
      if (val < 100) {
         return u32_2(buf, val);
      }
      else {
         // 100-255: 3 digits
         const uint32_t q = val / 100;
         *buf = char('0' + q);
         std::memcpy(buf + 1, &digit_pairs[val - q * 100], 2);
         return buf + 3;
      }
   }

   // int8_t: -128 to 127 (1-4 chars)
   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, int8_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      *buf = '-';
      return to_chars(buf + (val < 0), uint8_t(val ^ (val >> 7)) - (val >> 7));
   }

   // uint16_t: 0-65535 (1-5 digits)
   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, uint16_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      using namespace itoa_impl;
      if (val < 100) {
         return u32_2(buf, val);
      }
      else if (val < 10000) {
         return u32_4(buf, val);
      }
      else {
         // 10000-65535: 5 digits
         const uint32_t q = val / 10000;
         *buf = char('0' + q);
         return u64_len_4(buf + 1, val - q * 10000);
      }
   }

   // int16_t: -32768 to 32767 (1-6 chars)
   template <class T>
      requires std::same_as<std::remove_cvref_t<T>, int16_t>
   inline char* to_chars(char* buf, T val) noexcept
   {
      *buf = '-';
      return to_chars(buf + (val < 0), uint16_t(val ^ (val >> 15)) - (val >> 15));
   }

   // Keep char_table in glz namespace for dtoa.hpp compatibility
   inline constexpr auto& char_table = itoa_impl::char_table;
} // namespace glz
