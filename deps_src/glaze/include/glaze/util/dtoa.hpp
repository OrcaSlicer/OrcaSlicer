#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>

#include "glaze/util/dragonbox.hpp"
#include "glaze/util/inline.hpp"
#include "glaze/util/itoa.hpp"

namespace glz
{
   // std::countl_zero uses another branch check whether the input is zero,
   // we use this function when we know that x > 0
   GLZ_ALWAYS_INLINE constexpr auto countl_zero(const uint32_t x) noexcept
   {
#ifdef _MSC_VER
      return std::countl_zero(x);
#else
#if __has_builtin(__builtin_ctzll)
      return __builtin_clz(x);
#else
      return std::countl_zero(x);
#endif
#endif
   }

   constexpr int int_log2(uint32_t x) noexcept { return 31 - glz::countl_zero(x | 1); }

   inline constexpr uint64_t digit_count_table[] = {
      4294967296,  8589934582,  8589934582,  8589934582,  12884901788, 12884901788, 12884901788, 17179868184,
      17179868184, 17179868184, 21474826480, 21474826480, 21474826480, 21474826480, 25769703776, 25769703776,
      25769703776, 30063771072, 30063771072, 30063771072, 34349738368, 34349738368, 34349738368, 34349738368,
      38554705664, 38554705664, 38554705664, 41949672960, 41949672960, 41949672960, 42949672960, 42949672960};

   // https://lemire.me/blog/2021/06/03/computing-the-number-of-digits-of-an-integer-even-faster/
   constexpr int fast_digit_count(const uint32_t x) noexcept { return (x + digit_count_table[int_log2(x)]) >> 32; }

   /** Trailing zero count table for number 0 to 99.
    (generate with misc/make_tables.c) */
   inline constexpr uint8_t dec_trailing_zero_table[] = {
      2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};

   /**
    Write an unsigned integer with a length of 15 to 17 with trailing zero trimmed.
    These digits are named as "aabbccddeeffgghhii" here.
    For example, input 1234567890123000, output "1234567890123".
    */
   inline auto* write_u64_len_15_to_17_trim(auto* buf, uint64_t sig) noexcept
   {
      uint32_t tz1, tz2, tz; /* trailing zero */

      uint32_t abbccddee = uint32_t(sig / 100000000);
      uint32_t ffgghhii = uint32_t(sig - uint64_t(abbccddee) * 100000000);
      uint32_t abbcc = abbccddee / 10000; /* (abbccddee / 10000) */
      uint32_t ddee = abbccddee - abbcc * 10000; /* (abbccddee % 10000) */
      uint32_t abb = uint32_t((uint64_t(abbcc) * 167773) >> 24); /* (abbcc / 100) */
      uint32_t a = (abb * 41) >> 12; /* (abb / 100) */
      uint32_t bb = abb - a * 100; /* (abb % 100) */
      uint32_t cc = abbcc - abb * 100; /* (abbcc % 100) */

      /* write abbcc */
      buf[0] = uint8_t(a + '0');
      buf += a > 0;
      bool lz = bb < 10 && a == 0; /* leading zero */
      std::memcpy(buf, char_table + (bb * 2 + lz), 2);
      buf -= lz;
      std::memcpy(buf + 2, char_table + 2 * cc, 2);

      if (ffgghhii) {
         uint32_t dd = (ddee * 5243) >> 19; /* (ddee / 100) */
         uint32_t ee = ddee - dd * 100; /* (ddee % 100) */
         uint32_t ffgg = uint32_t((uint64_t(ffgghhii) * 109951163) >> 40); /* (val / 10000) */
         uint32_t hhii = ffgghhii - ffgg * 10000; /* (val % 10000) */
         uint32_t ff = (ffgg * 5243) >> 19; /* (aabb / 100) */
         uint32_t gg = ffgg - ff * 100; /* (aabb % 100) */
         std::memcpy(buf + 4, char_table + 2 * dd, 2);
         std::memcpy(buf + 6, char_table + 2 * ee, 2);
         std::memcpy(buf + 8, char_table + 2 * ff, 2);
         std::memcpy(buf + 10, char_table + 2 * gg, 2);
         if (hhii) {
            uint32_t hh = (hhii * 5243) >> 19; /* (ccdd / 100) */
            uint32_t ii = hhii - hh * 100; /* (ccdd % 100) */
            std::memcpy(buf + 12, char_table + 2 * hh, 2);
            std::memcpy(buf + 14, char_table + 2 * ii, 2);
            tz1 = dec_trailing_zero_table[hh];
            tz2 = dec_trailing_zero_table[ii];
            tz = ii ? tz2 : (tz1 + 2);
            buf += 16 - tz;
            return buf;
         }
         else {
            tz1 = dec_trailing_zero_table[ff];
            tz2 = dec_trailing_zero_table[gg];
            tz = gg ? tz2 : (tz1 + 2);
            buf += 12 - tz;
            return buf;
         }
      }
      else {
         if (ddee) {
            uint32_t dd = (ddee * 5243) >> 19; /* (ddee / 100) */
            uint32_t ee = ddee - dd * 100; /* (ddee % 100) */
            std::memcpy(buf + 4, char_table + 2 * dd, 2);
            std::memcpy(buf + 6, char_table + 2 * ee, 2);
            tz1 = dec_trailing_zero_table[dd];
            tz2 = dec_trailing_zero_table[ee];
            tz = ee ? tz2 : (tz1 + 2);
            buf += 8 - tz;
            return buf;
         }
         else {
            tz1 = dec_trailing_zero_table[bb];
            tz2 = dec_trailing_zero_table[cc];
            tz = cc ? tz2 : (tz1 + tz2);
            buf += 4 - tz;
            return buf;
         }
      }
   }

   inline auto* write_u32_len_1_to_9(auto* buf, uint32_t val) noexcept
   {
      if (val < 10) {
         *buf = uint8_t(val + '0');
         return buf + 1;
      }

      if (val < 100) {
         std::memcpy(buf, char_table + (val * 2), 2);
         return buf + 2;
      }

      const uint32_t digits = fast_digit_count(val);

      // Write digits in reverse order
      auto* end = buf + digits;
      auto* p = end;
      while (val >= 100) {
         const uint32_t q = val / 100;
         const uint32_t r = val % 100;
         val = q;
         std::memcpy(p - 2, char_table + (r * 2), 2);
         p -= 2;
      }

      if (val < 10) {
         *--p = uint8_t(val + '0');
      }
      else {
         std::memcpy(p - 2, char_table + (val * 2), 2);
      }

      return end;
   }

   consteval uint32_t numbits(uint32_t x) noexcept { return x < 2 ? x : 1 + numbits(x >> 1); }

   template <std::floating_point T>
   inline auto* to_chars(auto* buf, T val) noexcept
   {
      static_assert(std::numeric_limits<T>::is_iec559);
      static_assert(std::numeric_limits<T>::radix == 2);
      static_assert(std::is_same_v<float, T> || std::is_same_v<double, T>);
      static_assert(sizeof(float) == 4 && sizeof(double) == 8);
      constexpr bool is_float = std::is_same_v<float, T>;
      using Raw = std::conditional_t<std::is_same_v<float, T>, uint32_t, uint64_t>;

      if (val == 0.0) {
         *buf = '-';
         buf += (std::bit_cast<Raw>(val) >> (sizeof(T) * 8 - 1)); // if negative
         *buf = '0';
         return buf + 1;
      }

      using Conversion = glz::jkj::dragonbox::default_float_bit_carrier_conversion_traits<T>;
      using FormatTraits =
         glz::jkj::dragonbox::ieee754_binary_traits<typename Conversion::format, typename Conversion::carrier_uint>;
      static constexpr uint32_t exp_bits_count =
         numbits(std::numeric_limits<T>::max_exponent - std::numeric_limits<T>::min_exponent + 1);
      const auto float_bits = glz::jkj::dragonbox::make_float_bits<T, Conversion, FormatTraits>(val);
      const auto exp_bits = float_bits.extract_exponent_bits();
      const auto s = float_bits.remove_exponent_bits();

      // NaN or Infinity
      if (exp_bits == (uint32_t(1) << exp_bits_count) - 1) [[unlikely]] {
         std::memcpy(buf, "null", 4);
         return buf + 4;
      }

      *buf = '-';
      constexpr auto zero = T(0.0);
      buf += (val < zero);

      if constexpr (is_float) {
         const auto v = glz::jkj::dragonbox::to_decimal_ex(s, exp_bits, glz::jkj::dragonbox::policy::sign::ignore,
                                                           glz::jkj::dragonbox::policy::trailing_zero::remove);

         uint32_t sig_dec = uint32_t(v.significand);
         int32_t exp_dec = v.exponent;
         const int32_t num_digits = int32_t(fast_digit_count(sig_dec));
         int32_t dot_pos = num_digits + exp_dec; // the decimal point position relative to the first digit

         if (-6 < dot_pos && dot_pos <= 9) {
            // no need to write exponent part
            if (dot_pos <= 0) {
               *buf++ = '0';
               *buf++ = '.';
               while (dot_pos < 0) {
                  *buf++ = '0';
                  ++dot_pos;
               }
               return write_u32_len_1_to_9(buf, sig_dec);
            }
            else {
               auto num_end = write_u32_len_1_to_9(buf, sig_dec);
               int32_t digits_written = int32_t(num_end - buf);
               if (dot_pos < digits_written) {
                  std::memmove(buf + dot_pos + 1, buf + dot_pos, digits_written - dot_pos);
                  buf[dot_pos] = '.';
                  return num_end + 1;
               }
               else if (dot_pos > digits_written) {
                  std::memset(num_end, '0', dot_pos - digits_written);
                  return buf + dot_pos;
               }
               else {
                  return num_end; // Whole number, no decimal point needed
               }
            }
         }
         else {
            // write with scientific notation
            auto end = write_u32_len_1_to_9(buf + 1, sig_dec);
            exp_dec += int32_t(end - (buf + 1)) - 1; // Adjust exponent based on actual digits written
            buf[0] = buf[1];
            buf[1] = '.';
            if (end == buf + 2) { // Only one digit was written
               buf[2] = '0'; // Add trailing zero
               ++end;
            }
            *end = 'E';
            buf = end + 1;
            if (exp_dec < 0) {
               *buf = '-';
               ++buf;
               exp_dec = -exp_dec;
            }
            exp_dec = std::abs(exp_dec);
            uint32_t lz = exp_dec < 10;
            std::memcpy(buf, char_table + (exp_dec * 2 + lz), 2);
            return buf + 2 - lz;
         }
      }
      else {
         const auto v = glz::jkj::dragonbox::to_decimal_ex(s, exp_bits, glz::jkj::dragonbox::policy::sign::ignore,
                                                           glz::jkj::dragonbox::policy::trailing_zero::ignore);

         uint64_t sig_dec = v.significand;
         int32_t exp_dec = v.exponent;

         int32_t sig_len = 17;
         sig_len -= (sig_dec < 100000000ull * 100000000ull);
         sig_len -= (sig_dec < 100000000ull * 10000000ull);
         int32_t dot_pos = sig_len + exp_dec;

         if (-6 < dot_pos && dot_pos <= 21) {
            // no need to write exponent part
            if (dot_pos <= 0) {
               auto num_hdr = buf + (2 - dot_pos);
               auto num_end = write_u64_len_15_to_17_trim(num_hdr, sig_dec);
               buf[0] = '0';
               buf[1] = '.';
               buf += 2;
               std::memset(buf, '0', size_t(num_hdr - buf));
               return num_end;
            }
            else {
               // dot after first digit
               // such as 1.234, 1234.0, 123400000000000000000.0
               std::memset(buf, '0', 24);
               auto num_hdr = buf + 1;
               auto num_end = write_u64_len_15_to_17_trim(num_hdr, sig_dec);
               std::memmove(buf, buf + 1, size_t(dot_pos));
               buf[dot_pos] = '.';
               return ((num_end - num_hdr) <= dot_pos) ? buf + dot_pos : num_end;
            }
         }
         else {
            // write with scientific notation
            auto end = write_u64_len_15_to_17_trim(buf + 1, sig_dec);
            end -= (end == buf + 2); // remove '.0', e.g. 2.0e34 -> 2e34
            exp_dec += sig_len - 1;
            buf[0] = buf[1];
            buf[1] = '.';
            end[0] = 'E';
            buf = end + 1;
            buf[0] = '-';
            buf += exp_dec < 0;
            exp_dec = std::abs(exp_dec);
            if (exp_dec < 100) {
               uint32_t lz = exp_dec < 10;
               std::memcpy(buf, char_table + (exp_dec * 2 + lz), 2);
               return buf + 2 - lz;
            }
            else {
               const uint32_t hi = (uint32_t(exp_dec) * 656) >> 16; // exp / 100
               const uint32_t lo = uint32_t(exp_dec) - hi * 100; // exp % 100
               buf[0] = uint8_t(hi) + '0';
               std::memcpy(&buf[1], char_table + (lo * 2), 2);
               return buf + 3;
            }
         }
      }
   }
}
