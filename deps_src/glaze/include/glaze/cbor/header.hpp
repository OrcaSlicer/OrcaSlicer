// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "glaze/core/context.hpp"
#include "glaze/util/inline.hpp"

namespace glz::cbor
{
   // Major types (high 3 bits of initial byte)
   // These define the structural encoding of CBOR data items per RFC 8949.
   namespace major
   {
      inline constexpr uint8_t uint = 0; // 0b000 - Unsigned integer
      inline constexpr uint8_t nint = 1; // 0b001 - Negative integer (-1 - n)
      inline constexpr uint8_t bstr = 2; // 0b010 - Byte string
      inline constexpr uint8_t tstr = 3; // 0b011 - Text string (UTF-8)
      inline constexpr uint8_t array = 4; // 0b100 - Array of data items
      inline constexpr uint8_t map = 5; // 0b101 - Map of key-value pairs
      inline constexpr uint8_t tag = 6; // 0b110 - Semantic tag
      inline constexpr uint8_t simple = 7; // 0b111 - Simple value or float
   }

   // Additional information special values (low 5 bits of initial byte)
   namespace info
   {
      inline constexpr uint8_t uint8_follows = 24; // 1-byte argument follows
      inline constexpr uint8_t uint16_follows = 25; // 2-byte argument follows (big-endian)
      inline constexpr uint8_t uint32_follows = 26; // 4-byte argument follows (big-endian)
      inline constexpr uint8_t uint64_follows = 27; // 8-byte argument follows (big-endian)
      // 28-30 reserved
      inline constexpr uint8_t indefinite = 31; // Indefinite length (arrays/maps/strings)
   }

   // Simple values (major type 7, additional info values)
   namespace simple
   {
      inline constexpr uint8_t false_value = 20;
      inline constexpr uint8_t true_value = 21;
      inline constexpr uint8_t null_value = 22;
      inline constexpr uint8_t undefined = 23;
      // 24 = simple value in next byte
      inline constexpr uint8_t float16 = 25; // IEEE 754 half-precision (16-bit)
      inline constexpr uint8_t float32 = 26; // IEEE 754 single-precision (32-bit)
      inline constexpr uint8_t float64 = 27; // IEEE 754 double-precision (64-bit)
      // 28-30 reserved
      inline constexpr uint8_t break_code = 31; // "break" stop code for indefinite
   }

   // Semantic tags (major type 6)
   // These provide semantic meaning to the following data item per RFC 8949.
   namespace semantic_tag
   {
      // Standard tags (RFC 8949)
      inline constexpr uint64_t datetime_string = 0; // RFC 3339 date/time string
      inline constexpr uint64_t datetime_epoch = 1; // Epoch-based date/time
      inline constexpr uint64_t unsigned_bignum = 2; // Positive bignum
      inline constexpr uint64_t negative_bignum = 3; // Negative bignum
      inline constexpr uint64_t decimal_fraction = 4; // Decimal fraction [exponent, mantissa]
      inline constexpr uint64_t bigfloat = 5; // Bigfloat [exponent, mantissa]

      // Encoding hints
      inline constexpr uint64_t base64url = 21; // Expected base64url encoding
      inline constexpr uint64_t base64 = 22; // Expected base64 encoding
      inline constexpr uint64_t base16 = 23; // Expected base16 encoding
      inline constexpr uint64_t encoded_cbor = 24; // Embedded CBOR data item

      // Other standard tags
      inline constexpr uint64_t uri = 32; // URI (RFC 3986)
      inline constexpr uint64_t base64url_str = 33; // base64url-encoded text
      inline constexpr uint64_t base64_str = 34; // base64-encoded text
      inline constexpr uint64_t regex = 35; // Regular expression
      inline constexpr uint64_t mime = 36; // MIME message

      // Self-description
      inline constexpr uint64_t self_described = 55799; // Self-described CBOR (magic)

      // Multi-dimensional arrays (RFC 8746)
      inline constexpr uint64_t multi_dim_array = 40; // Row-major multi-dimensional array
      inline constexpr uint64_t multi_dim_array_col_major = 1040; // Column-major multi-dimensional array

      // Complex numbers (IANA CBOR tags registry)
      // https://www.iana.org/assignments/cbor-tags/cbor-tags.xhtml
      inline constexpr uint64_t complex_number = 43000; // Single complex: [real, imag]
      inline constexpr uint64_t complex_array = 43001; // Interleaved: [r0, i0, r1, i1, ...]

      // Typed arrays (RFC 8746)
      inline constexpr uint64_t ta_uint8 = 64;
      inline constexpr uint64_t ta_uint16_be = 65;
      inline constexpr uint64_t ta_uint32_be = 66;
      inline constexpr uint64_t ta_uint64_be = 67;
      inline constexpr uint64_t ta_uint8_clamped = 68;
      inline constexpr uint64_t ta_uint16_le = 69;
      inline constexpr uint64_t ta_uint32_le = 70;
      inline constexpr uint64_t ta_uint64_le = 71;
      inline constexpr uint64_t ta_sint8 = 72;
      inline constexpr uint64_t ta_sint16_be = 73;
      inline constexpr uint64_t ta_sint32_be = 74;
      inline constexpr uint64_t ta_sint64_be = 75;
      // 76 reserved
      inline constexpr uint64_t ta_sint16_le = 77;
      inline constexpr uint64_t ta_sint32_le = 78;
      inline constexpr uint64_t ta_sint64_le = 79;
      inline constexpr uint64_t ta_float16_be = 80;
      inline constexpr uint64_t ta_float32_be = 81;
      inline constexpr uint64_t ta_float64_be = 82;
      inline constexpr uint64_t ta_float128_be = 83;
      inline constexpr uint64_t ta_float16_le = 84;
      inline constexpr uint64_t ta_float32_le = 85;
      inline constexpr uint64_t ta_float64_le = 86;
      inline constexpr uint64_t ta_float128_le = 87;
   }

   // Construct initial byte from major type and additional info
   [[nodiscard]] GLZ_ALWAYS_INLINE constexpr uint8_t initial_byte(uint8_t major_type, uint8_t additional_info) noexcept
   {
      return static_cast<uint8_t>((major_type << 5) | (additional_info & 0x1f));
   }

   // Extract major type from initial byte
   [[nodiscard]] GLZ_ALWAYS_INLINE constexpr uint8_t get_major_type(uint8_t initial) noexcept { return initial >> 5; }

   // Extract additional info from initial byte
   [[nodiscard]] GLZ_ALWAYS_INLINE constexpr uint8_t get_additional_info(uint8_t initial) noexcept
   {
      return initial & 0x1f;
   }

   // Decode IEEE 754 half-precision float (binary16) to double
   // Per RFC 8949 Appendix D
   [[nodiscard]] inline double decode_half(uint16_t half) noexcept
   {
      const int sign = (half >> 15) & 1;
      const int exp = (half >> 10) & 0x1f;
      const int mant = half & 0x3ff;

      double val;
      if (exp == 0) {
         // Subnormal or zero
         val = std::ldexp(static_cast<double>(mant), -24);
      }
      else if (exp != 31) {
         // Normal number
         val = std::ldexp(static_cast<double>(mant + 1024), exp - 25);
      }
      else {
         // Infinity or NaN
         val = (mant == 0) ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
      }

      return sign ? -val : val;
   }

   // Encode double to IEEE 754 half-precision float (binary16)
   // Returns the encoded half value
   [[nodiscard]] inline uint16_t encode_half(double value) noexcept
   {
      // Handle special cases
      if (std::isnan(value)) {
         return 0x7e00; // NaN
      }

      uint64_t bits;
      std::memcpy(&bits, &value, sizeof(double));

      const uint16_t sign = static_cast<uint16_t>((bits >> 63) & 1);
      const int64_t exp = static_cast<int64_t>((bits >> 52) & 0x7ff) - 1023;
      const uint64_t mant = bits & 0xfffffffffffffull;

      if (std::isinf(value)) {
         return static_cast<uint16_t>((sign << 15) | 0x7c00); // Infinity
      }

      if (value == 0.0) {
         return static_cast<uint16_t>(sign << 15); // Zero (preserves sign)
      }

      // Check if exponent is in range for half precision
      if (exp < -24) {
         // Too small, underflows to zero
         return static_cast<uint16_t>(sign << 15);
      }

      if (exp > 15) {
         // Too large, overflows to infinity
         return static_cast<uint16_t>((sign << 15) | 0x7c00);
      }

      uint16_t half_mant;
      uint16_t half_exp;

      if (exp < -14) {
         // Subnormal in half precision
         half_exp = 0;
         // Calculate the shift for subnormal
         const int shift = static_cast<int>(-14 - exp);
         half_mant = static_cast<uint16_t>((mant | 0x10000000000000ull) >> (43 + shift));
      }
      else {
         // Normal in half precision
         half_exp = static_cast<uint16_t>(exp + 15);
         half_mant = static_cast<uint16_t>(mant >> 42);
      }

      return static_cast<uint16_t>((sign << 15) | (half_exp << 10) | (half_mant & 0x3ff));
   }

   // Check if a double can be exactly represented as a half-precision float
   [[nodiscard]] inline bool can_encode_half(double value) noexcept
   {
      if (std::isnan(value) || std::isinf(value)) {
         return true;
      }

      const uint16_t half = encode_half(value);
      return decode_half(half) == value;
   }

   // Check if a double can be exactly represented as a single-precision float
   [[nodiscard]] inline bool can_encode_float(double value) noexcept
   {
      if (std::isnan(value) || std::isinf(value)) {
         return true;
      }

      const float f = static_cast<float>(value);
      return static_cast<double>(f) == value;
   }

   // RFC 8746 Typed Array helpers
   namespace typed_array
   {
      // Select the appropriate tag for a type based on native endianness
      template <class T>
      [[nodiscard]] consteval uint64_t native_tag() noexcept
      {
         constexpr bool is_le = (std::endian::native == std::endian::little);

         // Unsigned integers
         if constexpr (std::same_as<T, uint8_t>) {
            return semantic_tag::ta_uint8; // 64 - single byte, endianness irrelevant
         }
         else if constexpr (std::same_as<T, uint16_t>) {
            return is_le ? semantic_tag::ta_uint16_le : semantic_tag::ta_uint16_be; // 69 or 65
         }
         else if constexpr (std::same_as<T, uint32_t>) {
            return is_le ? semantic_tag::ta_uint32_le : semantic_tag::ta_uint32_be; // 70 or 66
         }
         else if constexpr (std::same_as<T, uint64_t>) {
            return is_le ? semantic_tag::ta_uint64_le : semantic_tag::ta_uint64_be; // 71 or 67
         }
         // Signed integers
         else if constexpr (std::same_as<T, int8_t>) {
            return semantic_tag::ta_sint8; // 72 - single byte, endianness irrelevant
         }
         else if constexpr (std::same_as<T, int16_t>) {
            return is_le ? semantic_tag::ta_sint16_le : semantic_tag::ta_sint16_be; // 77 or 73
         }
         else if constexpr (std::same_as<T, int32_t>) {
            return is_le ? semantic_tag::ta_sint32_le : semantic_tag::ta_sint32_be; // 78 or 74
         }
         else if constexpr (std::same_as<T, int64_t>) {
            return is_le ? semantic_tag::ta_sint64_le : semantic_tag::ta_sint64_be; // 79 or 75
         }
         // Floating point
         else if constexpr (std::same_as<T, float>) {
            static_assert(sizeof(float) == 4);
            return is_le ? semantic_tag::ta_float32_le : semantic_tag::ta_float32_be; // 85 or 81
         }
         else if constexpr (std::same_as<T, double>) {
            static_assert(sizeof(double) == 8);
            return is_le ? semantic_tag::ta_float64_le : semantic_tag::ta_float64_be; // 86 or 82
         }
         else {
            static_assert(sizeof(T) == 0, "Unsupported type for typed array");
            return 0;
         }
      }

      // Check if a tag is a typed array tag and get element info
      struct typed_array_info
      {
         bool valid{false};
         size_t element_size{0};
         bool is_little_endian{false};
         bool is_signed{false};
         bool is_float{false};
      };

      [[nodiscard]] constexpr typed_array_info get_info(uint64_t tag) noexcept
      {
         typed_array_info info{};

         if (tag < 64 || tag > 87 || tag == 76) {
            return info; // Not a typed array tag
         }

         info.valid = true;

         if (tag >= 64 && tag <= 71) {
            // Unsigned integers
            info.is_signed = false;
            info.is_float = false;
            if (tag == 64 || tag == 68) {
               info.element_size = 1;
               info.is_little_endian = true; // Doesn't matter for 1 byte
            }
            else if (tag == 65 || tag == 69) {
               info.element_size = 2;
               info.is_little_endian = (tag == 69);
            }
            else if (tag == 66 || tag == 70) {
               info.element_size = 4;
               info.is_little_endian = (tag == 70);
            }
            else {
               info.element_size = 8;
               info.is_little_endian = (tag == 71);
            }
         }
         else if (tag >= 72 && tag <= 79) {
            // Signed integers
            info.is_signed = true;
            info.is_float = false;
            if (tag == 72) {
               info.element_size = 1;
               info.is_little_endian = true;
            }
            else if (tag == 73 || tag == 77) {
               info.element_size = 2;
               info.is_little_endian = (tag == 77);
            }
            else if (tag == 74 || tag == 78) {
               info.element_size = 4;
               info.is_little_endian = (tag == 78);
            }
            else {
               info.element_size = 8;
               info.is_little_endian = (tag == 79);
            }
         }
         else {
            // Floating point (80-87)
            info.is_signed = true; // Floats can be negative
            info.is_float = true;
            if (tag == 80 || tag == 84) {
               info.element_size = 2; // float16
               info.is_little_endian = (tag == 84);
            }
            else if (tag == 81 || tag == 85) {
               info.element_size = 4; // float32
               info.is_little_endian = (tag == 85);
            }
            else if (tag == 82 || tag == 86) {
               info.element_size = 8; // float64
               info.is_little_endian = (tag == 86);
            }
            else {
               info.element_size = 16; // float128
               info.is_little_endian = (tag == 87);
            }
         }

         return info;
      }

      // Check if we need to byteswap when reading a typed array
      [[nodiscard]] constexpr bool needs_byteswap(uint64_t tag) noexcept
      {
         const auto info = get_info(tag);
         if (!info.valid || info.element_size == 1) {
            return false;
         }
         constexpr bool native_is_le = (std::endian::native == std::endian::little);
         return native_is_le != info.is_little_endian;
      }
   }
}
