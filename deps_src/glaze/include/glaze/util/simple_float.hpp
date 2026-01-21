// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// Simple computational float parsing and serialization for size-optimized builds.
// Uses minimal lookup tables (~660 bytes) instead of fast_float/Dragonbox (~20KB+).
// Trades some performance for dramatically smaller binary size.
// Suitable for embedded systems and bare-metal environments.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "glaze/util/inline.hpp"

namespace glz::simple_float
{
   // ============================================================================
   // FLOAT PARSING - Computational approach with tiny power-of-5 tables
   // Uses 128-bit arithmetic for correct rounding on all platforms
   // ============================================================================

   namespace detail
   {
      struct decimal_number
      {
         bool negative{};
         uint64_t mantissa{};
         int32_t exp10{};
      };

      static constexpr int32_t max_exp10 = 400;
      static constexpr int32_t min_exp10 = -400;

      // Strict JSON-compliant number parser
      // JSON number format (RFC 8259):
      //   number = [ minus ] int [ frac ] [ exp ]
      //   int = zero / ( digit1-9 *DIGIT )
      //   frac = decimal-point 1*DIGIT
      //   exp = e [ minus / plus ] 1*DIGIT
      //
      // Key rules:
      // - No leading + sign
      // - No leading zeros (except single 0 before decimal or as the number itself)
      // - Decimal point must be followed by at least one digit
      // - Decimal point must be preceded by at least one digit
      // - Exponent must have at least one digit
      template <bool null_terminated>
      GLZ_ALWAYS_INLINE constexpr const char* parse_decimal_strict(const char* buf, const char* end,
                                                                   decimal_number& out) noexcept
      {
         const char* p = buf;

         auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

         auto at_end = [&]() {
            if constexpr (null_terminated) {
               return *p == '\0';
            }
            else {
               return p >= end;
            }
         };

         auto peek = [&]() -> char {
            if (at_end()) return '\0';
            return *p;
         };

         // Handle optional minus sign (JSON doesn't allow leading +)
         bool negative = false;
         if (peek() == '-') {
            negative = true;
            ++p;
         }

         // Must have at least one digit in integer part
         if (at_end() || !is_digit(peek())) {
            return nullptr; // Error: no digits after sign or at start
         }

         constexpr int max_sig_digits = 17;
         uint64_t mantissa = 0;
         int32_t exp10 = 0;
         int sig_digits = 0;

         // Parse integer part
         char first_digit = peek();
         if (first_digit == '0') {
            // Leading zero - next char must NOT be a digit (JSON rule)
            ++p;
            if (!at_end() && is_digit(peek())) {
               return nullptr; // Error: leading zero followed by digit (e.g., "01", "007")
            }
         }
         else {
            // Non-zero digit - parse all integer digits
            while (!at_end() && is_digit(peek())) {
               unsigned digit = static_cast<unsigned>(*p - '0');
               if (sig_digits < max_sig_digits) {
                  mantissa = mantissa * 10u + digit;
                  ++sig_digits;
               }
               else {
                  // Mantissa full - adjust exponent for extra integer digits
                  if (exp10 < max_exp10) ++exp10;
               }
               ++p;
            }
         }

         // Parse optional fractional part
         if (!at_end() && peek() == '.') {
            ++p;

            // JSON requires at least one digit after decimal point
            if (at_end() || !is_digit(peek())) {
               return nullptr; // Error: decimal point without following digit (e.g., "1.", "1.e5")
            }

            // Parse fractional digits
            while (!at_end() && is_digit(peek())) {
               unsigned digit = static_cast<unsigned>(*p - '0');

               // Skip leading zeros in fraction (don't count as significant)
               if (mantissa == 0 && digit == 0) {
                  // Leading fractional zero: just adjust exponent
                  if (exp10 > min_exp10) --exp10;
               }
               else if (sig_digits < max_sig_digits) {
                  mantissa = mantissa * 10u + digit;
                  ++sig_digits;
                  if (exp10 > min_exp10) --exp10;
               }
               // else: mantissa full, digit is truncated (no exponent adjustment for fractions)
               ++p;
            }
         }

         // Parse optional exponent part
         if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++p;

            // Optional sign
            bool exp_negative = false;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
               exp_negative = (peek() == '-');
               ++p;
            }

            // JSON requires at least one digit in exponent
            if (at_end() || !is_digit(peek())) {
               return nullptr; // Error: exponent without digits (e.g., "1e", "1e+", "1e-")
            }

            int32_t exp_part = 0;
            while (!at_end() && is_digit(peek())) {
               unsigned digit = static_cast<unsigned>(*p - '0');
               if (exp_part < max_exp10) {
                  exp_part = exp_part * 10 + static_cast<int32_t>(digit);
                  if (exp_part > max_exp10) exp_part = max_exp10;
               }
               ++p;
            }

            if (exp_negative) {
               exp10 -= exp_part;
               if (exp10 < min_exp10) exp10 = min_exp10;
            }
            else {
               exp10 += exp_part;
               if (exp10 > max_exp10) exp10 = max_exp10;
            }
         }

         out.negative = negative;
         out.mantissa = mantissa;
         out.exp10 = exp10;
         return p; // Return pointer to position after parsed number
      }

      // ============================================================================
      // 128-bit power-of-5 tables for exact decimal-to-binary conversion
      // Uses binary exponentiation: 9 entries cover 5^1 to 5^256
      // Total table size: 9 entries × 2 tables × 20 bytes = 360 bytes
      // ============================================================================

      // Normalized 128-bit representation of 5^(2^k)
      // mantissa has MSB at bit 127, value = mantissa × 2^exp
      struct pow5_128
      {
         uint64_t hi;
         uint64_t lo;
         int32_t exp;
      };

      // Positive powers: 5^(2^k) for k = 0..8
      // Used for decimal exponents >= 0
      inline constexpr pow5_128 pow5_pos_table[] = {
         {0xA000000000000000ULL, 0x0000000000000000ULL, -125}, // 5^1
         {0xC800000000000000ULL, 0x0000000000000000ULL, -123}, // 5^2
         {0x9C40000000000000ULL, 0x0000000000000000ULL, -118}, // 5^4
         {0xBEBC200000000000ULL, 0x0000000000000000ULL, -109}, // 5^8
         {0x8E1BC9BF04000000ULL, 0x0000000000000000ULL, -90}, // 5^16
         {0x9DC5ADA82B70B59DULL, 0xF020000000000000ULL, -53}, // 5^32
         {0xC2781F49FFCFA6D5ULL, 0x3CBF6B71C76B25FBULL, 21}, // 5^64
         {0x93BA47C980E98CDFULL, 0xC66F336C36B10137ULL, 170}, // 5^128
         {0xAA7EEBFB9DF9DE8DULL, 0xDDBB901B98FEEAB8ULL, 467}, // 5^256
      };

      // Negative powers: 5^(-(2^k)) normalized to 128 bits
      // Used for decimal exponents < 0
      inline constexpr pow5_128 pow5_neg_table[] = {
         {0xCCCCCCCCCCCCCCCCULL, 0xCCCCCCCCCCCCCCCDULL, -130}, // 5^-1
         {0xA3D70A3D70A3D70AULL, 0x3D70A3D70A3D70A4ULL, -132}, // 5^-2
         {0xD1B71758E219652BULL, 0xD3C36113404EA4A9ULL, -137}, // 5^-4
         {0xABCC77118461CEFCULL, 0xFDC20D2B36BA7C3DULL, -146}, // 5^-8
         {0xE69594BEC44DE15BULL, 0x4C2EBE687989A9B4ULL, -165}, // 5^-16
         {0xCFB11EAD453994BAULL, 0x67DE18EDA5814AF2ULL, -202}, // 5^-32
         {0xA87FEA27A539E9A5ULL, 0x3F2398D747B36224ULL, -276}, // 5^-64
         {0xDDD0467C64BCE4A0ULL, 0xAC7CB3F6D05DDBDFULL, -425}, // 5^-128
         {0xC0314325637A1939ULL, 0xFA911155FEFB5309ULL, -722}, // 5^-256
      };

#ifdef __SIZEOF_INT128__
      // ============================================================================
      // Compact pow5 lookup table for common exponents (-64 to +64)
      // Provides O(1) lookup for ~85% of typical JSON numbers while using only 2.5KB
      // Falls back to binary exponentiation for extreme exponents
      // ============================================================================

      // Suppress -Wpedantic warnings for __int128 (compiler extension)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

      // Compute a single pow5 table entry at compile time using binary exponentiation
      inline constexpr pow5_128 compute_pow5_entry(int q) noexcept
      {
         using u128 = unsigned __int128;

         if (q == 0) {
            return {0x8000000000000000ULL, 0x0000000000000000ULL, -127};
         }

         const bool is_negative = q < 0;
         uint32_t e = static_cast<uint32_t>(is_negative ? -q : q);

         // Start with 1.0 normalized
         u128 mantissa = u128(1) << 127;
         int32_t exp = -127;

         // Binary exponentiation using the base pow5 tables
         for (int k = 0; k < 9 && e != 0; ++k) {
            if (e & 1) {
               const auto& p = is_negative ? pow5_neg_table[k] : pow5_pos_table[k];
               u128 p_mantissa = (u128(p.hi) << 64) | p.lo;

               // 128x128 multiply, keep high 128 bits
               uint64_t a_lo = static_cast<uint64_t>(mantissa);
               uint64_t a_hi = static_cast<uint64_t>(mantissa >> 64);
               uint64_t b_lo = static_cast<uint64_t>(p_mantissa);
               uint64_t b_hi = static_cast<uint64_t>(p_mantissa >> 64);

               u128 p0 = u128(a_lo) * b_lo;
               u128 p1 = u128(a_lo) * b_hi;
               u128 p2 = u128(a_hi) * b_lo;
               u128 p3 = u128(a_hi) * b_hi;

               u128 mid = (p0 >> 64) + uint64_t(p1) + uint64_t(p2);
               u128 prod_hi = p3 + (p1 >> 64) + (p2 >> 64) + (mid >> 64);

               mantissa = prod_hi;
               exp += p.exp + 128;

               // Normalize
               if (mantissa != 0) {
                  int lz = 0;
                  u128 tmp = mantissa;
                  if (!(tmp >> 64)) {
                     lz = 64;
                     tmp <<= 64;
                  }
                  uint64_t hi = uint64_t(tmp >> 64);
                  if (!(hi >> 32)) {
                     lz += 32;
                     hi <<= 32;
                  }
                  if (!(hi >> 48)) {
                     lz += 16;
                     hi <<= 16;
                  }
                  if (!(hi >> 56)) {
                     lz += 8;
                     hi <<= 8;
                  }
                  if (!(hi >> 60)) {
                     lz += 4;
                     hi <<= 4;
                  }
                  if (!(hi >> 62)) {
                     lz += 2;
                     hi <<= 2;
                  }
                  if (!(hi >> 63)) {
                     lz += 1;
                  }
                  if (lz > 0 && lz < 128) {
                     mantissa <<= lz;
                     exp -= lz;
                  }
               }
            }
            e >>= 1;
         }

         return {static_cast<uint64_t>(mantissa >> 64), static_cast<uint64_t>(mantissa), exp};
      }

      // Compact table bounds: covers -64 to +64 (129 entries, ~2.5KB)
      inline constexpr int pow5_compact_min = -64;
      inline constexpr int pow5_compact_max = 64;
      inline constexpr int pow5_compact_size = pow5_compact_max - pow5_compact_min + 1;

      // Generate compact table at compile time
      inline constexpr auto make_pow5_compact_table() noexcept
      {
         struct table_type
         {
            pow5_128 entries[pow5_compact_size];
         };
         table_type result{};
         for (int i = 0; i < pow5_compact_size; ++i) {
            result.entries[i] = compute_pow5_entry(pow5_compact_min + i);
         }
         return result;
      }

      inline constexpr auto pow5_compact_table = make_pow5_compact_table();

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

      // ============================================================================
      // 128-bit multiplication primitives
      // ============================================================================

#ifdef __SIZEOF_INT128__
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
      using uint128_native = __uint128_t;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
      // Fallback: software 128-bit multiplication
      struct uint128_native
      {
         uint64_t lo, hi;
      };
#endif

      // Multiply two 64-bit numbers, return full 128-bit result
      GLZ_ALWAYS_INLINE constexpr void mul64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) noexcept
      {
#ifdef __SIZEOF_INT128__
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
         uint128_native prod = static_cast<uint128_native>(a) * b;
         hi = static_cast<uint64_t>(prod >> 64);
         lo = static_cast<uint64_t>(prod);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
         // Software implementation
         uint64_t a_lo = a & 0xFFFFFFFF;
         uint64_t a_hi = a >> 32;
         uint64_t b_lo = b & 0xFFFFFFFF;
         uint64_t b_hi = b >> 32;

         uint64_t p0 = a_lo * b_lo;
         uint64_t p1 = a_lo * b_hi;
         uint64_t p2 = a_hi * b_lo;
         uint64_t p3 = a_hi * b_hi;

         uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);
         hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
         lo = (mid << 32) | (p0 & 0xFFFFFFFF);
#endif
      }

      // Multiply 64-bit mantissa by 128-bit pow5 entry
      // Returns high 128 bits of the 192-bit product
      // exp is updated: exp_out = exp_in + p.exp + 64
      GLZ_ALWAYS_INLINE constexpr void mul64_pow5(uint64_t m, const pow5_128& p, uint64_t& rh, uint64_t& rl,
                                                  int32_t& exp, bool& round_bit, bool& sticky_bit) noexcept
      {
         // m × (p.hi : p.lo) = m×p.hi × 2^64 + m×p.lo
         // This is 64 × 128 = 192 bits, we keep high 128

         uint64_t ph_hi, ph_lo, pl_hi, pl_lo;
         mul64(m, p.hi, ph_hi, ph_lo);
         mul64(m, p.lo, pl_hi, pl_lo);

         // Add pl_hi to ph_lo with carry
         uint64_t sum_lo = ph_lo + pl_hi;
         uint64_t carry = (sum_lo < ph_lo) ? 1 : 0;
         uint64_t sum_hi = ph_hi + carry;

         rh = sum_hi;
         rl = sum_lo;
         exp += p.exp + 64; // ADD to existing exponent (not overwrite!)

         // Rounding info from discarded pl_lo
         round_bit = (pl_lo >> 63) != 0;
         sticky_bit = (pl_lo & 0x7FFFFFFFFFFFFFFFULL) != 0;
      }

      // Multiply two 128-bit numbers, return high 128 bits of 256-bit product
      GLZ_ALWAYS_INLINE constexpr void mul128(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl, uint64_t& rh,
                                              uint64_t& rl, bool& round_bit, bool& sticky_bit) noexcept
      {
         // (ah:al) × (bh:bl) = ah×bh × 2^128 + (ah×bl + al×bh) × 2^64 + al×bl
         // We need bits 255..128 (high 128 bits)

         uint64_t hh_hi, hh_lo; // ah × bh (bits 128-255)
         uint64_t hl_hi, hl_lo; // ah × bl (bits 64-191)
         uint64_t lh_hi, lh_lo; // al × bh (bits 64-191)
         uint64_t ll_hi, ll_lo; // al × bl (bits 0-127)

         mul64(ah, bh, hh_hi, hh_lo);
         mul64(ah, bl, hl_hi, hl_lo);
         mul64(al, bh, lh_hi, lh_lo);
         mul64(al, bl, ll_hi, ll_lo);

         // Sum the middle terms with ll_hi to get carry into high 128 bits
         // mid = hl_lo + lh_lo + ll_hi (with carries tracked)
         uint64_t mid = hl_lo;
         uint64_t mid_carry = 0;

         uint64_t tmp = mid + lh_lo;
         mid_carry += (tmp < mid) ? 1 : 0;
         mid = tmp;

         tmp = mid + ll_hi;
         mid_carry += (tmp < mid) ? 1 : 0;
         mid = tmp;

         // Compute bits 128-191: hh_lo + hl_hi + lh_hi + mid_carry
         uint64_t high_lo = hh_lo;
         uint64_t high_carry = 0;

         tmp = high_lo + hl_hi;
         high_carry += (tmp < high_lo) ? 1 : 0;
         high_lo = tmp;

         tmp = high_lo + lh_hi;
         high_carry += (tmp < high_lo) ? 1 : 0;
         high_lo = tmp;

         tmp = high_lo + mid_carry;
         high_carry += (tmp < high_lo) ? 1 : 0;
         high_lo = tmp;

         // Compute bits 192-255: hh_hi + high_carry
         uint64_t high_hi = hh_hi + high_carry;

         rh = high_hi;
         rl = high_lo;

         // Rounding info from discarded bits
         round_bit = (mid >> 63) != 0;
         sticky_bit = ((mid & 0x7FFFFFFFFFFFFFFFULL) | ll_lo) != 0;
      }

      // Count leading zeros
      GLZ_ALWAYS_INLINE constexpr int clz64(uint64_t x) noexcept
      {
         if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
         return __builtin_clzll(x);
#else
         int n = 0;
         if ((x & 0xFFFFFFFF00000000ULL) == 0) {
            n += 32;
            x <<= 32;
         }
         if ((x & 0xFFFF000000000000ULL) == 0) {
            n += 16;
            x <<= 16;
         }
         if ((x & 0xFF00000000000000ULL) == 0) {
            n += 8;
            x <<= 8;
         }
         if ((x & 0xF000000000000000ULL) == 0) {
            n += 4;
            x <<= 4;
         }
         if ((x & 0xC000000000000000ULL) == 0) {
            n += 2;
            x <<= 2;
         }
         if ((x & 0x8000000000000000ULL) == 0) {
            n += 1;
         }
         return n;
#endif
      }

      // Convert 128-bit mantissa + binary exponent to double with correct IEEE 754 rounding
      GLZ_ALWAYS_INLINE constexpr double assemble_double(uint64_t hi, uint64_t lo, int32_t exp2, bool negative,
                                                         bool round_bit, bool sticky_bit) noexcept
      {
         // Normalize: ensure MSB of hi is set
         if (hi == 0) {
            if (lo == 0) return negative ? -0.0 : 0.0;
            hi = lo;
            lo = 0;
            exp2 -= 64;
         }

         int lz = clz64(hi);
         if (lz > 0) {
            hi = (hi << lz) | (lo >> (64 - lz));
            lo = lo << lz;
            exp2 -= lz;
         }

         // Now hi has MSB at bit 63 (of hi), which is bit 127 of the full 128-bit value
         // Compute biased exponent first to determine if we need subnormal handling
         // For normalized double: value = mantissa × 2^(-52) × 2^e
         // hi has implicit bit at position 63, so: value = (hi:lo) × 2^(exp2 - 127)
         // This means biased_exp = exp2 + 127 + 1023 (before extracting mantissa)
         // But we extract mantissa53 = hi >> 11, adding 11 to the implicit bit position
         // So: biased_exp = exp2 + 127 + 1023 (after all adjustments)
         int32_t biased_exp = exp2 + 127 + 1023;

         // Handle overflow
         if (biased_exp >= 2047) {
            uint64_t bits = 0x7FF0000000000000ULL;
            if (negative) bits |= 0x8000000000000000ULL;
            double result;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
         }

         // Handle underflow to zero
         // For subnormals, the minimum mantissa is 1, so we need biased_exp >= -52
         // (shift of 64 + 64 = 128 bits means we'd get 0 mantissa)
         if (biased_exp < -63) {
            // Even with rounding, the mantissa would be 0
            return negative ? -0.0 : 0.0;
         }

         uint64_t mantissa;
         bool final_round, final_sticky;

         if (biased_exp > 0) {
            // Normal number: extract 53 bits (bits 63..11 of hi)
            mantissa = hi >> 11;
            final_round = (hi >> 10) & 1;
            final_sticky = ((hi & 0x3FF) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
         }
         else {
            // Subnormal: the IEEE mantissa is computed as:
            // mantissa = (hi:lo) × 2^(exp2 + 1074)
            // Since biased_exp = exp2 + 1150, we have exp2 = biased_exp - 1150
            // So the shift is: -(exp2 + 1074) = -(biased_exp - 1150 + 1074) = 76 - biased_exp
            //
            // For biased_exp = 0: shift = 76 (extract ~52 bits)
            // For biased_exp = -52: shift = 128 (mantissa ≈ 1)
            int total_shift = 76 - biased_exp;

            if (total_shift < 64) {
               // Shift is within hi
               mantissa = hi >> total_shift;
               uint64_t round_bit_mask = 1ULL << (total_shift - 1);
               uint64_t sticky_bits_mask = round_bit_mask - 1;
               final_round = (hi & round_bit_mask) != 0;
               final_sticky = ((hi & sticky_bits_mask) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
            }
            else if (total_shift < 128) {
               // Shift spans hi and lo
               int lo_shift = total_shift - 64;
               if (lo_shift == 0) {
                  // Special case: total_shift == 64
                  // (hi:lo) >> 64 = hi, round bit from MSB of lo
                  mantissa = hi;
                  final_round = (lo >> 63) & 1;
                  final_sticky = ((lo & 0x7FFFFFFFFFFFFFFFULL) | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
               }
               else {
                  // (hi:lo) >> total_shift = (hi >> lo_shift) | (bits shifted out)
                  mantissa = hi >> lo_shift;
                  // Round bit is at position (lo_shift - 1) of hi
                  uint64_t round_bit_mask = 1ULL << (lo_shift - 1);
                  uint64_t sticky_bits_mask = round_bit_mask - 1;
                  final_round = (hi & round_bit_mask) != 0;
                  // Sticky includes lower bits of hi plus all of lo
                  final_sticky = ((hi & sticky_bits_mask) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
               }
            }
            else if (total_shift == 128) {
               // Special case: shift by exactly 128 bits
               // (hi:lo) >> 128 = 0, but we need to round based on (hi:lo)
               mantissa = 0;
               final_round = (hi >> 63) & 1; // MSB of (hi:lo) is at bit 127 = hi bit 63
               final_sticky = ((hi & 0x7FFFFFFFFFFFFFFFULL) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
            }
            else {
               // total_shift > 128: result is 0 with rounding from hi
               mantissa = 0;
               final_round = false;
               final_sticky = (hi | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
            }

            biased_exp = 0;
         }

         // IEEE 754 round-to-nearest-even
         if (final_round && (final_sticky || (mantissa & 1))) {
            ++mantissa;
            if (biased_exp > 0 && mantissa >= (1ULL << 53)) {
               // Overflow from rounding in normal case
               mantissa >>= 1;
               ++biased_exp;
               if (biased_exp >= 2047) {
                  uint64_t bits = 0x7FF0000000000000ULL;
                  if (negative) bits |= 0x8000000000000000ULL;
                  double result;
                  std::memcpy(&result, &bits, sizeof(result));
                  return result;
               }
            }
            // For subnormals, overflow from rounding just increases the mantissa
            // which might promote to smallest normal, but that's handled correctly
         }

         // Remove implicit bit for normal numbers
         if (biased_exp > 0) {
            mantissa &= ~(1ULL << 52);
         }

         // Assemble IEEE 754 double
         uint64_t bits = (static_cast<uint64_t>(biased_exp) << 52) | mantissa;
         if (negative) bits |= 0x8000000000000000ULL;

         double result;
         std::memcpy(&result, &bits, sizeof(result));
         return result;
      }

      // Convert 128-bit mantissa + binary exponent to float
      GLZ_ALWAYS_INLINE constexpr float assemble_float(uint64_t hi, uint64_t lo, int32_t exp2, bool negative,
                                                       bool round_bit, bool sticky_bit) noexcept
      {
         // Similar to assemble_double but for 24-bit mantissa

         // Normalize
         if (hi == 0) {
            if (lo == 0) return negative ? -0.0f : 0.0f;
            hi = lo;
            lo = 0;
            exp2 -= 64;
         }

         int lz = clz64(hi);
         if (lz > 0) {
            hi = (hi << lz) | (lo >> (64 - lz));
            lo = lo << lz;
            exp2 -= lz;
         }

         // Compute biased exponent first to determine if we need subnormal handling
         int32_t biased_exp = exp2 + 127 + 127;

         // Handle overflow
         if (biased_exp >= 255) {
            uint32_t bits = 0x7F800000U;
            if (negative) bits |= 0x80000000U;
            float result;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
         }

         // Handle underflow to zero
         if (biased_exp < -32) {
            return negative ? -0.0f : 0.0f;
         }

         uint32_t mantissa;
         bool final_round, final_sticky;

         if (biased_exp > 0) {
            // Normal number: extract 24 bits (bits 63..40 of hi)
            mantissa = static_cast<uint32_t>(hi >> 40);
            final_round = (hi >> 39) & 1;
            final_sticky = ((hi & 0x7FFFFFFFFFULL) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
         }
         else {
            // Subnormal: the IEEE mantissa is computed as:
            // mantissa = (hi:lo) × 2^(exp2 + 149)
            // Since biased_exp = exp2 + 254, we have exp2 = biased_exp - 254
            // So the shift is: -(exp2 + 149) = -(biased_exp - 254 + 149) = 105 - biased_exp
            //
            // For biased_exp = 0: shift = 105 (extracting ~23 bits)
            // For biased_exp = -23: shift = 128 (extracting ~1 bit from rounding)
            int total_shift = 105 - biased_exp;

            if (total_shift < 64) {
               // Shift is within hi
               mantissa = static_cast<uint32_t>(hi >> total_shift);
               uint64_t round_bit_mask = 1ULL << (total_shift - 1);
               uint64_t sticky_bits_mask = round_bit_mask - 1;
               final_round = (hi & round_bit_mask) != 0;
               final_sticky = ((hi & sticky_bits_mask) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
            }
            else if (total_shift < 128) {
               // Shift spans hi and lo
               int lo_shift = total_shift - 64;
               if (lo_shift == 0) {
                  // Special case: total_shift == 64
                  // (hi:lo) >> 64 = hi, but we need the HIGH bits of hi for the mantissa
                  // The mantissa bits are in the top of hi, extract them
                  mantissa = static_cast<uint32_t>(hi >> 32);
                  final_round = (hi >> 31) & 1;
                  final_sticky = ((hi & 0x7FFFFFFFULL) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
               }
               else {
                  mantissa = static_cast<uint32_t>(hi >> lo_shift);
                  uint64_t round_bit_mask = 1ULL << (lo_shift - 1);
                  uint64_t sticky_bits_mask = round_bit_mask - 1;
                  final_round = (hi & round_bit_mask) != 0;
                  final_sticky = ((hi & sticky_bits_mask) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
               }
            }
            else if (total_shift == 128) {
               // Special case: shift by exactly 128 bits
               // (hi:lo) >> 128 = 0, but we need to round based on (hi:lo)
               mantissa = 0;
               final_round = (hi >> 63) & 1; // MSB of (hi:lo) is at bit 127 = hi bit 63
               final_sticky = ((hi & 0x7FFFFFFFFFFFFFFFULL) | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
            }
            else {
               // total_shift > 128: result is 0 with rounding from hi
               mantissa = 0;
               final_round = false;
               final_sticky = (hi | lo | (round_bit ? 1 : 0) | (sticky_bit ? 1 : 0)) != 0;
            }

            biased_exp = 0;
         }

         // IEEE 754 round-to-nearest-even
         if (final_round && (final_sticky || (mantissa & 1))) {
            ++mantissa;
            if (biased_exp > 0 && mantissa >= (1U << 24)) {
               // Overflow from rounding in normal case
               mantissa >>= 1;
               ++biased_exp;
               if (biased_exp >= 255) {
                  uint32_t bits = 0x7F800000U;
                  if (negative) bits |= 0x80000000U;
                  float result;
                  std::memcpy(&result, &bits, sizeof(result));
                  return result;
               }
            }
         }

         // Remove implicit bit for normal numbers
         if (biased_exp > 0) {
            mantissa &= ~(1U << 23);
         }

         uint32_t bits = (static_cast<uint32_t>(biased_exp) << 23) | mantissa;
         if (negative) bits |= 0x80000000U;

         float result;
         std::memcpy(&result, &bits, sizeof(result));
         return result;
      }

      // Compute mantissa × 5^|q| using binary exponentiation
      // Returns 128-bit result (rh:rl) and binary exponent exp2
      // Value = (rh × 2^64 + rl) × 2^exp2
      // Note: table parameter allows sharing code between positive/negative exponents
      inline constexpr void apply_pow5_impl(uint64_t mantissa, int32_t q, uint64_t& rh, uint64_t& rl, int32_t& exp2,
                                            bool& round_bit, bool& sticky_bit, const pow5_128* table) noexcept
      {
         // Normalize mantissa to have MSB at bit 63 of rh
         // (rh:rl) = rh × 2^64 + rl, so with rl=0 and rh = mantissa << lz:
         // (rh:0) = (mantissa << lz) × 2^64 = mantissa × 2^(lz + 64)
         // To make value = mantissa, we need exp2 = -(lz + 64)
         int lz = clz64(mantissa);
         rh = mantissa << lz;
         rl = 0;
         exp2 = -lz - 64; // Value = (rh:0) × 2^exp2 = mantissa
         round_bit = false;
         sticky_bit = false;

         uint32_t e = static_cast<uint32_t>(q);

         for (int k = 0; k < 9 && e != 0; ++k) {
            if (e & 1) {
               // Multiply current 128-bit result by table[k]
               uint64_t th, tl;
               bool tr, ts;

               if (rl == 0 && rh < (1ULL << 63)) {
                  // rh is less than 64 bits and rl is 0, use simpler multiply
                  mul64_pow5(rh, table[k], th, tl, exp2, tr, ts);
                  // exp2 already updated by mul64_pow5
               }
               else {
                  // Full 128×128 multiply
                  mul128(rh, rl, table[k].hi, table[k].lo, th, tl, tr, ts);
                  exp2 += table[k].exp + 128;
               }

               rh = th;
               rl = tl;
               sticky_bit = sticky_bit || round_bit || ts;
               round_bit = tr;

               // Normalize after multiply
               if (rh != 0) {
                  int nlz = clz64(rh);
                  if (nlz > 0 && nlz < 64) {
                     rh = (rh << nlz) | (rl >> (64 - nlz));
                     rl = rl << nlz;
                     exp2 -= nlz;
                  }
               }
            }
            e >>= 1;
         }
      }

#ifdef __SIZEOF_INT128__
      // Hybrid pow5 application: uses compact table for common exponents, binary exp for extreme values
      // This gives O(1) performance for ~85% of typical JSON numbers while using only 2.5KB
      template <bool positive_exp>
      GLZ_ALWAYS_INLINE constexpr void apply_pow5_hybrid(uint64_t mantissa, int32_t q, uint64_t& rh, uint64_t& rl,
                                                         int32_t& exp2, bool& round_bit, bool& sticky_bit) noexcept
      {
         const int32_t lookup_q = q;

         // Check if we can use the compact table
         if (lookup_q >= pow5_compact_min && lookup_q <= pow5_compact_max) {
            // O(1) direct lookup from compact table
            const auto& p = pow5_compact_table.entries[lookup_q - pow5_compact_min];

            // Normalize mantissa to have MSB at bit 63
            int lz = clz64(mantissa);
            uint64_t norm_mantissa = mantissa << lz;
            int32_t mantissa_exp = -lz;

            // Multiply: norm_mantissa (64-bit) × p (128-bit) = 192-bit result
            uint64_t ph_hi, ph_lo, pl_hi, pl_lo;
            mul64(norm_mantissa, p.hi, ph_hi, ph_lo);
            mul64(norm_mantissa, p.lo, pl_hi, pl_lo);

            // Add pl_hi to ph_lo with carry
            uint64_t sum_lo = ph_lo + pl_hi;
            uint64_t carry = (sum_lo < ph_lo) ? 1 : 0;
            uint64_t sum_hi = ph_hi + carry;

            rh = sum_hi;
            rl = sum_lo;

            // Compute exponent
            exp2 = mantissa_exp + p.exp + 64 + q;

            // Rounding info from discarded pl_lo
            round_bit = (pl_lo >> 63) != 0;
            sticky_bit = (pl_lo & 0x7FFFFFFFFFFFFFFFULL) != 0;

            // Normalize result
            if (rh != 0) {
               int nlz = clz64(rh);
               if (nlz > 0 && nlz < 64) {
                  rh = (rh << nlz) | (rl >> (64 - nlz));
                  rl = rl << nlz;
                  exp2 -= nlz;
               }
            }
         }
         else {
            // Fall back to binary exponentiation for extreme exponents
            apply_pow5_impl(mantissa, positive_exp ? q : -q, rh, rl, exp2, round_bit, sticky_bit,
                            positive_exp ? pow5_pos_table : pow5_neg_table);
            exp2 += q; // Add 2^q factor for 10^q = 5^q * 2^q
         }
      }
#endif

      // ============================================================================
      // Legacy long double tables for float (still accurate enough)
      // ============================================================================

      inline constexpr long double pow10_pos[] = {1e1L, 1e2L, 1e4L, 1e8L, 1e16L, 1e32L, 1e64L, 1e128L, 1e256L};

      inline constexpr long double pow10_neg[] = {1e-1L, 1e-2L, 1e-4L, 1e-8L, 1e-16L, 1e-32L, 1e-64L, 1e-128L, 1e-256L};

      GLZ_ALWAYS_INLINE constexpr long double scale_by_pow10(long double value, int32_t exp10) noexcept
      {
         if (exp10 == 0 || value == 0.0L) return value;

         bool negative_exp = exp10 < 0;
         uint32_t e = static_cast<uint32_t>(negative_exp ? -exp10 : exp10);

         long double result = value;
         unsigned idx = 0;

         while (e != 0 && idx < 9) {
            if (e & 1u) {
               result *= (negative_exp ? pow10_neg[idx] : pow10_pos[idx]);
            }
            e >>= 1;
            ++idx;
         }

         return result;
      }

      // ============================================================================
      // Fast path for float parsing using 64-bit arithmetic
      // ============================================================================

      // Exact powers of 10 that fit in a double without rounding error
      inline constexpr double exact_pow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

      // Fast float parsing for common cases (small exponents, normal values)
      // Returns true if fast path succeeded, false if 128-bit path needed
      GLZ_ALWAYS_INLINE bool try_fast_float_parse(uint64_t mantissa, int32_t exp10, bool negative,
                                                  float& result) noexcept
      {
         // Fast path uses double precision arithmetic which has 53-bit mantissa.
         // For float (24-bit mantissa), this gives us ~29 bits of headroom for error.
         // We can safely handle exponents in [-22, 22] where powers of 10 are exact in double.

         if (exp10 >= -22 && exp10 <= 22 && mantissa != 0) {
            double d = static_cast<double>(mantissa);
            if (exp10 >= 0) {
               d *= exact_pow10[exp10];
            }
            else {
               d /= exact_pow10[-exp10];
            }

            // Check if result is a normal float (not subnormal, zero, or overflow)
            float f = static_cast<float>(d);
            uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            uint32_t exp_field = (bits >> 23) & 0xFF;

            // Only use fast path for normal floats (exp_field in [1, 254])
            if (exp_field != 0 && exp_field != 0xFF) {
               result = negative ? -f : f;
               return true;
            }
         }

         // Extended range using long double (80-bit or 128-bit depending on platform)
         // Long double has at least 64-bit mantissa, giving us more headroom
         if (exp10 >= -45 && exp10 <= 38 && mantissa != 0) {
            long double ld = static_cast<long double>(mantissa);
            ld = scale_by_pow10(ld, exp10);

            // Check bounds before converting to float
            constexpr long double float_max = 3.402823466e+38L;
            constexpr long double float_min_normal = 1.175494351e-38L;

            if (ld >= float_min_normal && ld <= float_max) {
               float f = static_cast<float>(ld);
               uint32_t bits;
               std::memcpy(&bits, &f, sizeof(bits));
               uint32_t exp_field = (bits >> 23) & 0xFF;

               if (exp_field != 0 && exp_field != 0xFF) {
                  result = negative ? -f : f;
                  return true;
               }
            }
            else if (ld >= -float_max && ld <= -float_min_normal) {
               float f = static_cast<float>(ld);
               uint32_t bits;
               std::memcpy(&bits, &f, sizeof(bits));
               uint32_t exp_field = (bits >> 23) & 0xFF;

               if (exp_field != 0 && exp_field != 0xFF) {
                  result = negative ? -f : f;
                  return true;
               }
            }
         }

         return false; // Need slow path for subnormals and edge cases
      }

      // Write a single decimal digit
      GLZ_ALWAYS_INLINE constexpr void write_digit(char*& p, int d) noexcept { *p++ = static_cast<char>('0' + d); }

      // Write an unsigned integer (for exponent)
      GLZ_ALWAYS_INLINE char* write_uint(char* buf, uint32_t val) noexcept
      {
         if (val == 0) {
            *buf++ = '0';
            return buf;
         }

         char temp[10];
         int len = 0;
         while (val > 0) {
            temp[len++] = static_cast<char>('0' + (val % 10));
            val /= 10;
         }

         // Reverse
         for (int i = len - 1; i >= 0; --i) {
            *buf++ = temp[i];
         }
         return buf;
      }

      // ============================================================================
      // 128-bit double serialization
      // ============================================================================

      // Decompose IEEE 754 double into mantissa and binary exponent
      // Returns: mantissa × 2^exp2 = value (for positive values)
      GLZ_ALWAYS_INLINE void decompose_double(double value, uint64_t& mantissa, int32_t& exp2) noexcept
      {
         uint64_t bits;
         std::memcpy(&bits, &value, sizeof(bits));

         uint64_t raw_mantissa = bits & 0x000FFFFFFFFFFFFFULL;
         int32_t raw_exp = static_cast<int32_t>((bits >> 52) & 0x7FF);

         if (raw_exp == 0) {
            // Subnormal: value = raw_mantissa × 2^(-1022 - 52)
            mantissa = raw_mantissa;
            exp2 = -1022 - 52;
         }
         else {
            // Normal: value = (1 + raw_mantissa/2^52) × 2^(raw_exp - 1023)
            //               = (2^52 + raw_mantissa) × 2^(raw_exp - 1023 - 52)
            mantissa = (1ULL << 52) | raw_mantissa;
            exp2 = raw_exp - 1023 - 52;
         }
      }

      // Compute the number of decimal digits for a double value
      // Returns floor(log10(value)) + 1
      GLZ_ALWAYS_INLINE constexpr int32_t estimate_decimal_exponent(uint64_t mantissa, int32_t exp2) noexcept
      {
         // value = mantissa × 2^exp2
         // log10(value) = log10(mantissa) + exp2 × log10(2)
         // log10(2) ≈ 0.30103

         if (mantissa == 0) return 0;

         int mantissa_bits = 64 - clz64(mantissa);
         // log10(mantissa) ≈ (mantissa_bits - 1) × 0.30103
         // But for mantissa in [2^52, 2^53), log10 ∈ [15.65, 15.95]

         // Use fixed-point arithmetic: multiply by 78913 and divide by 262144 (≈ 0.30103)
         int64_t log2_total = static_cast<int64_t>(mantissa_bits - 1) + exp2;
         int64_t log10_approx = (log2_total * 78913) >> 18; // Divide by 262144

         return static_cast<int32_t>(log10_approx);
      }

      // 128-bit to_chars for double
      // Computes exact decimal representation using 128-bit integer arithmetic
      GLZ_ALWAYS_INLINE char* to_chars_double_128(char* buf, double value) noexcept
      {
         // Handle special cases
         if (value != value) { // NaN
            std::memcpy(buf, "null", 4);
            return buf + 4;
         }

         if (value == std::numeric_limits<double>::infinity()) {
            std::memcpy(buf, "null", 4);
            return buf + 4;
         }

         if (value == -std::numeric_limits<double>::infinity()) {
            std::memcpy(buf, "null", 4);
            return buf + 4;
         }

         if (value == 0.0) {
            *buf++ = '0';
            return buf;
         }

         // Handle sign
         bool negative = std::signbit(value);
         if (negative) {
            *buf++ = '-';
            value = -value;
         }

         // Decompose: value = mantissa × 2^exp2
         uint64_t mantissa;
         int32_t exp2;
         decompose_double(value, mantissa, exp2);

         // Estimate decimal exponent: exp10 ≈ floor(log10(value))
         int32_t exp10 = estimate_decimal_exponent(mantissa, exp2);

         // We want to compute a 17-digit integer representation:
         // digits_int = round(mantissa × 2^exp2 × 10^(16 - exp10))
         //            = round(mantissa × 5^(16-exp10) × 2^(exp2 + 16 - exp10))
         //
         // Define: pow5_exp = 16 - exp10 (can be positive or negative)
         //         pow2_exp = exp2 + 16 - exp10 = exp2 + pow5_exp

         int32_t pow5_exp = 16 - exp10;
         int32_t pow2_exp = exp2 + pow5_exp;

         // Setup 128-bit representation for mantissa
         uint64_t rh = mantissa;
         uint64_t rl = 0;
         int32_t bin_exp = 0; // Current binary shift to apply

         // Multiply by 5^|pow5_exp|
         if (pow5_exp > 0) {
            // Multiply by 5^pow5_exp using positive table
            uint32_t e = static_cast<uint32_t>(pow5_exp);
            for (int k = 0; k < 9 && e != 0; ++k) {
               if (e & 1) {
                  // Normalize before multiply to prevent overflow
                  if (rh != 0) {
                     int nlz = clz64(rh);
                     if (nlz > 0 && nlz < 64) {
                        rh = (rh << nlz) | (rl >> (64 - nlz));
                        rl = rl << nlz;
                        bin_exp -= nlz;
                     }
                  }

                  uint64_t th, tl;
                  bool tr, ts;
                  mul128(rh, rl, pow5_pos_table[k].hi, pow5_pos_table[k].lo, th, tl, tr, ts);
                  bin_exp += pow5_pos_table[k].exp + 128;
                  rh = th;
                  rl = tl;
               }
               e >>= 1;
            }
         }
         else if (pow5_exp < 0) {
            // Multiply by 5^(-|pow5_exp|) = divide by 5^|pow5_exp|
            uint32_t e = static_cast<uint32_t>(-pow5_exp);
            for (int k = 0; k < 9 && e != 0; ++k) {
               if (e & 1) {
                  // Normalize before multiply
                  if (rh != 0) {
                     int nlz = clz64(rh);
                     if (nlz > 0 && nlz < 64) {
                        rh = (rh << nlz) | (rl >> (64 - nlz));
                        rl = rl << nlz;
                        bin_exp -= nlz;
                     }
                  }

                  uint64_t th, tl;
                  bool tr, ts;
                  mul128(rh, rl, pow5_neg_table[k].hi, pow5_neg_table[k].lo, th, tl, tr, ts);
                  bin_exp += pow5_neg_table[k].exp + 128;
                  rh = th;
                  rl = tl;
               }
               e >>= 1;
            }
         }

         // Apply the 2^pow2_exp factor
         // Total shift = bin_exp + pow2_exp
         int32_t total_shift = bin_exp + pow2_exp;

         // Normalize (rh:rl)
         if (rh == 0 && rl != 0) {
            rh = rl;
            rl = 0;
            total_shift -= 64;
         }
         if (rh != 0) {
            int nlz = clz64(rh);
            if (nlz > 0 && nlz < 64) {
               rh = (rh << nlz) | (rl >> (64 - nlz));
               rl = rl << nlz;
               total_shift -= nlz;
            }
         }

         // Now (rh:rl) × 2^total_shift = digits_int (approximately)
         // We need to extract the integer part

         // The result should be around 10^16 to 10^17, which is about 54-57 bits
         // So total_shift should bring us to that range

         // Extract the high bits as our digit integer
         // If total_shift >= 0, shift left; if < 0, shift right
         uint64_t digits_int;
         uint64_t remainder = 0;

         if (total_shift >= 64) {
            // Overflow - result is too large, adjust exp10
            ++exp10;
            total_shift -= 3; // Approximately log2(10)
         }

         if (total_shift >= 0 && total_shift < 64) {
            digits_int = (rh << total_shift) | (rl >> (64 - total_shift));
            remainder = rl << total_shift;
         }
         else if (total_shift >= 64) {
            // Very large shift, use approximation
            digits_int = rh;
         }
         else if (total_shift < 0 && total_shift > -64) {
            int rshift = -total_shift;
            digits_int = rh >> rshift;
            remainder = (rh << (64 - rshift)) | (rl >> rshift);
         }
         else {
            // total_shift <= -64
            int rshift = -total_shift - 64;
            if (rshift < 64) {
               digits_int = rl >> rshift;
               remainder = rl << (64 - rshift);
            }
            else {
               digits_int = 0;
               remainder = 0;
            }
         }

         // Round based on remainder (128-bit arithmetic is sufficient with proper rounding)
         if (remainder >= 0x8000000000000000ULL) {
            ++digits_int;
         }

         // Adjust if digits_int is out of expected range
         // Should be in [10^16, 10^17) for 17-digit representation
         constexpr uint64_t pow10_16 = 10000000000000000ULL;
         constexpr uint64_t pow10_17 = 100000000000000000ULL;

         // When dividing by 10, use proper rounding (round half up)
         while (digits_int >= pow10_17) {
            uint64_t rem = digits_int % 10;
            digits_int /= 10;
            if (rem >= 5) {
               ++digits_int;
               // Handle overflow from rounding (rare but possible)
               if (digits_int >= pow10_17) {
                  digits_int /= 10;
                  ++exp10;
               }
            }
            ++exp10;
         }
         while (digits_int > 0 && digits_int < pow10_16) {
            digits_int *= 10;
            --exp10;
         }

         // Extract individual digits
         constexpr int prec = 17;
         int digits[20];

         uint64_t temp = digits_int;
         for (int i = prec - 1; i >= 0; --i) {
            digits[i] = static_cast<int>(temp % 10);
            temp /= 10;
         }

         // Find last non-zero digit
         int last_nonzero = prec - 1;
         while (last_nonzero > 0 && digits[last_nonzero] == 0) {
            --last_nonzero;
         }
         int num_digits = last_nonzero + 1;

         // Format output
         const bool use_exp = (exp10 < -4 || exp10 >= num_digits);

         char* out = buf;

         if (!use_exp) {
            if (exp10 >= 0) {
               int int_digits_count = exp10 + 1;
               int i = 0;
               for (; i < int_digits_count && i < num_digits; ++i) {
                  write_digit(out, digits[i]);
               }
               for (; i < int_digits_count; ++i) {
                  *out++ = '0';
               }
               if (i < num_digits) {
                  *out++ = '.';
                  for (; i < num_digits; ++i) {
                     write_digit(out, digits[i]);
                  }
               }
            }
            else {
               *out++ = '0';
               *out++ = '.';
               for (int i = 0; i < -exp10 - 1; ++i) {
                  *out++ = '0';
               }
               for (int i = 0; i < num_digits; ++i) {
                  write_digit(out, digits[i]);
               }
            }
         }
         else {
            write_digit(out, digits[0]);
            if (num_digits > 1) {
               *out++ = '.';
               for (int i = 1; i < num_digits; ++i) {
                  write_digit(out, digits[i]);
               }
            }
            *out++ = 'e';
            if (exp10 >= 0) {
               *out++ = '+';
            }
            else {
               *out++ = '-';
               exp10 = -exp10;
            }
            out = write_uint(out, static_cast<uint32_t>(exp10));
         }

         return out;
      }
   } // namespace detail

   // ============================================================================
   // PARSING API
   // ============================================================================

   // Parse a floating-point number from a character buffer
   // Returns: {pointer past last parsed char, error code}
   // Uses strict JSON-compliant parsing (RFC 8259)
   template <bool null_terminated, class T>
   GLZ_ALWAYS_INLINE constexpr std::from_chars_result from_chars(const char* first, const char* last, T& value) noexcept
   {
      static_assert(std::is_floating_point_v<T>, "T must be a floating-point type");

      detail::decimal_number dec{};
      const char* end_ptr = detail::parse_decimal_strict<null_terminated>(first, last, dec);

      if (end_ptr == nullptr) {
         return {first, std::errc::invalid_argument};
      }

      if (dec.mantissa == 0) {
         value = dec.negative ? T(-0.0) : T(0.0);
         return {end_ptr, std::errc{}};
      }

      // Fast path for float: use 64-bit arithmetic for common cases
      if constexpr (std::is_same_v<T, float>) {
         if (detail::try_fast_float_parse(dec.mantissa, dec.exp10, dec.negative, value)) {
            return {end_ptr, std::errc{}};
         }
      }

      // Use 128-bit arithmetic for correct rounding
      uint64_t rh, rl;
      int32_t exp2;
      bool round_bit, sticky_bit;
#ifdef __SIZEOF_INT128__
      // Hybrid approach: O(1) table lookup for common exponents, binary exp for extreme values
      // Uses ~2.5KB compact table covering exponents -64 to +64 (~85% of typical JSON numbers)
      if (dec.exp10 >= 0) {
         detail::apply_pow5_hybrid<true>(dec.mantissa, dec.exp10, rh, rl, exp2, round_bit, sticky_bit);
      }
      else {
         detail::apply_pow5_hybrid<false>(dec.mantissa, dec.exp10, rh, rl, exp2, round_bit, sticky_bit);
      }
#else
      // Fallback: pure binary exponentiation (slower but smaller)
      if (dec.exp10 >= 0) {
         detail::apply_pow5_impl(dec.mantissa, dec.exp10, rh, rl, exp2, round_bit, sticky_bit, detail::pow5_pos_table);
         exp2 += dec.exp10; // Add 2^exp10 factor
      }
      else {
         detail::apply_pow5_impl(dec.mantissa, -dec.exp10, rh, rl, exp2, round_bit, sticky_bit, detail::pow5_neg_table);
         exp2 += dec.exp10; // Subtract (exp10 is negative)
      }
#endif

      if constexpr (std::is_same_v<T, float>) {
         value = detail::assemble_float(rh, rl, exp2, dec.negative, round_bit, sticky_bit);
      }
      else {
         value = detail::assemble_double(rh, rl, exp2, dec.negative, round_bit, sticky_bit);
      }
      return {end_ptr, std::errc{}};
   }

   // ============================================================================
   // FLOAT SERIALIZATION - Simple digit-by-digit conversion
   // ============================================================================

   // Format a floating-point number to a character buffer
   // Returns: pointer past the last written character
   // Buffer must have at least 32 bytes available
   template <class T>
   GLZ_ALWAYS_INLINE char* to_chars(char* buf, T value) noexcept
   {
      static_assert(std::is_floating_point_v<T>, "T must be a floating-point type");

      // Use 128-bit arithmetic for doubles to ensure correct roundtrip
      if constexpr (std::is_same_v<T, double>) {
         return detail::to_chars_double_128(buf, value);
      }

      // Handle special cases
      if (value != value) { // NaN
         std::memcpy(buf, "null", 4);
         return buf + 4;
      }

      if (value == std::numeric_limits<T>::infinity()) {
         std::memcpy(buf, "null", 4);
         return buf + 4;
      }

      if (value == -std::numeric_limits<T>::infinity()) {
         std::memcpy(buf, "null", 4);
         return buf + 4;
      }

      // Handle zero first (including negative zero)
      if (value == T(0)) {
         *buf++ = '0';
         return buf;
      }

      // Handle sign (must check after zero since -0.0 < 0.0 is false)
      bool negative = std::signbit(value);
      if (negative) {
         *buf++ = '-';
         value = -value;
      }

      // Use long double for better precision during conversion (floats only)
      long double v = static_cast<long double>(value);

      // Normalize to [1, 10) and compute decimal exponent
      int32_t exp10 = 0;

      // Scale down large numbers
      if (v >= 10.0L) {
         if (v >= 1e256L) {
            v /= 1e256L;
            exp10 += 256;
         }
         if (v >= 1e128L) {
            v /= 1e128L;
            exp10 += 128;
         }
         if (v >= 1e64L) {
            v /= 1e64L;
            exp10 += 64;
         }
         if (v >= 1e32L) {
            v /= 1e32L;
            exp10 += 32;
         }
         if (v >= 1e16L) {
            v /= 1e16L;
            exp10 += 16;
         }
         if (v >= 1e8L) {
            v /= 1e8L;
            exp10 += 8;
         }
         if (v >= 1e4L) {
            v /= 1e4L;
            exp10 += 4;
         }
         if (v >= 1e2L) {
            v /= 1e2L;
            exp10 += 2;
         }
         if (v >= 10.0L) {
            v /= 10.0L;
            exp10 += 1;
         }
      }
      // Scale up small numbers
      else if (v < 1.0L && v > 0.0L) {
         if (v < 1e-255L) {
            v *= 1e256L;
            exp10 -= 256;
         }
         if (v < 1e-127L) {
            v *= 1e128L;
            exp10 -= 128;
         }
         if (v < 1e-63L) {
            v *= 1e64L;
            exp10 -= 64;
         }
         if (v < 1e-31L) {
            v *= 1e32L;
            exp10 -= 32;
         }
         if (v < 1e-15L) {
            v *= 1e16L;
            exp10 -= 16;
         }
         if (v < 1e-7L) {
            v *= 1e8L;
            exp10 -= 8;
         }
         if (v < 1e-3L) {
            v *= 1e4L;
            exp10 -= 4;
         }
         if (v < 1e-1L) {
            v *= 1e2L;
            exp10 -= 2;
         }
         if (v < 1.0L) {
            v *= 10.0L;
            exp10 -= 1;
         }
      }

      // Determine precision based on type
      // Use full precision to guarantee roundtrip correctness
      constexpr int prec = std::is_same_v<T, float> ? 9 : 17;

      // Generate digits with extra guard digits for better rounding
      int digits[20];
      for (int i = 0; i < prec + 2; ++i) {
         int d = static_cast<int>(v);
         if (d > 9) d = 9;
         if (d < 0) d = 0;
         digits[i] = d;
         v = (v - static_cast<long double>(d)) * 10.0L;
      }

      // Round using guard digits - round half to even (banker's rounding)
      bool do_round = false;
      if (digits[prec] > 5) {
         do_round = true;
      }
      else if (digits[prec] == 5) {
         // Check if there are non-zero digits after
         if (digits[prec + 1] > 0) {
            do_round = true;
         }
         else {
            // Exact 0.5 case - round to even
            do_round = (digits[prec - 1] & 1) != 0;
         }
      }

      if (do_round) {
         int i = prec - 1;
         for (;;) {
            if (digits[i] < 9) {
               ++digits[i];
               break;
            }
            digits[i] = 0;
            if (i == 0) {
               digits[0] = 1;
               ++exp10;
               break;
            }
            --i;
         }
      }

      // Find last non-zero digit for trimming trailing zeros
      int last_nonzero = prec - 1;
      while (last_nonzero > 0 && digits[last_nonzero] == 0) {
         --last_nonzero;
      }
      int num_digits = last_nonzero + 1;

      // Decide between fixed and exponential notation (like %g)
      // Use fixed if -4 <= exp10 < precision, otherwise exponential
      const bool use_exp = (exp10 < -4 || exp10 >= num_digits);
      const int32_t saved_exp10 = exp10; // Save for regeneration

      // Lambda to generate output from digits array
      auto generate_output = [&]() {
         char* out = buf;
         int32_t e = saved_exp10;
         if (!use_exp) {
            if (e >= 0) {
               int int_digits_count = e + 1;
               int i = 0;
               for (; i < int_digits_count && i < num_digits; ++i) {
                  detail::write_digit(out, digits[i]);
               }
               for (; i < int_digits_count; ++i) {
                  *out++ = '0';
               }
               if (i < num_digits) {
                  *out++ = '.';
                  for (; i < num_digits; ++i) {
                     detail::write_digit(out, digits[i]);
                  }
               }
            }
            else {
               *out++ = '0';
               *out++ = '.';
               for (int i = 0; i < -e - 1; ++i) {
                  *out++ = '0';
               }
               for (int i = 0; i < num_digits; ++i) {
                  detail::write_digit(out, digits[i]);
               }
            }
         }
         else {
            detail::write_digit(out, digits[0]);
            if (num_digits > 1) {
               *out++ = '.';
               for (int i = 1; i < num_digits; ++i) {
                  detail::write_digit(out, digits[i]);
               }
            }
            *out++ = 'e';
            if (e >= 0) {
               *out++ = '+';
            }
            else {
               *out++ = '-';
               e = -e;
            }
            out = detail::write_uint(out, static_cast<uint32_t>(e));
         }
         return out;
      };

      return generate_output();
   }

} // namespace glz::simple_float
