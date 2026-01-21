// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>
#include <string>

#include "glaze/core/feature_test.hpp"
#include "glaze/util/string_literal.hpp"

// JSON does not require escaped unicode keys to match with unescaped UTF-8
// In order to match with escaped unicode you can register your fields with
// the escaped unicode value.
// glz::escape_unicode<"😀"> will generate a compile time escaped unicode version
// of your key.

// These features require constexpr std::string support (not available with _GLIBCXX_USE_CXX11_ABI=0)
#if GLZ_HAS_CONSTEXPR_STRING

namespace glz::detail
{
   // Helper function to append a Unicode escape sequence to the output string.
   inline constexpr void append_unicode_escape(std::string& output, uint16_t code_unit)
   {
      output += '\\';
      output += 'u';
      for (int shift = 12; shift >= 0; shift -= 4) {
         uint8_t digit = (code_unit >> shift) & 0xF;
         output += (digit < 10) ? ('0' + digit) : ('A' + (digit - 10));
      }
   }

   // Function to calculate the length of the escaped JSON string.
   inline constexpr size_t escaped_length(const std::string_view input)
   {
      size_t length = 0;
      size_t i = 0;
      size_t len = input.size();

      while (i < len) {
         unsigned char c = static_cast<unsigned char>(input[i++]);

         if (c <= 0x7F) {
            // ASCII character
            switch (c) {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
               length += 2; // Escaped as two characters
               break;
            default:
               if (c <= 0x1F) {
                  length += 6; // Control character, escaped as \u00XX
               }
               else {
                  length += 1; // Regular character
               }
               break;
            }
         }
         else {
            // Multibyte UTF-8 character
            uint32_t codepoint = 0;
            int bytes = 0;

            if ((c & 0xE0) == 0xC0) {
               // 2-byte sequence
               codepoint = c & 0x1F;
               bytes = 1;
            }
            else if ((c & 0xF0) == 0xE0) {
               // 3-byte sequence
               codepoint = c & 0x0F;
               bytes = 2;
            }
            else if ((c & 0xF8) == 0xF0) {
               // 4-byte sequence
               codepoint = c & 0x07;
               bytes = 3;
            }
            else {
               // Invalid UTF-8 start byte
               codepoint = 0xFFFD;
               bytes = 0;
            }

            bool invalid_sequence = false;

            for (int j = 0; j < bytes; ++j) {
               if (i == len) {
                  invalid_sequence = true;
                  break;
               }
               unsigned char c2 = static_cast<unsigned char>(input[i]);
               if ((c2 & 0xC0) != 0x80) {
                  invalid_sequence = true;
                  break;
               }
               codepoint = (codepoint << 6) | (c2 & 0x3F);
               ++i;
            }

            if (invalid_sequence) {
               // Invalid UTF-8 sequence, replace with U+FFFD
               codepoint = 0xFFFD;
            }

            if (codepoint <= 0xFFFF) {
               length += 6; // Escaped as \uXXXX
            }
            else {
               length += 12; // Surrogate pair, escaped as \uXXXX\uXXXX
            }
         }
      }

      return length;
   }

   // Main function to escape the JSON string.
   inline constexpr std::string escape_json_string(const std::string_view input, const size_t output_length)
   {
      std::string output;
      output.reserve(output_length);

      size_t i = 0;
      size_t len = input.size();

      while (i < len) {
         unsigned char c = static_cast<unsigned char>(input[i++]);

         if (c <= 0x7F) {
            // ASCII character
            switch (c) {
            case '\"':
               output += "\\\"";
               break;
            case '\\':
               output += "\\\\";
               break;
            case '\b':
               output += "\\b";
               break;
            case '\f':
               output += "\\f";
               break;
            case '\n':
               output += "\\n";
               break;
            case '\r':
               output += "\\r";
               break;
            case '\t':
               output += "\\t";
               break;
            default:
               if (c <= 0x1F) {
                  // Control character, escape using \u00XX
                  output += "\\u00";
                  uint8_t high_nibble = (c >> 4) & 0xF;
                  uint8_t low_nibble = c & 0xF;
                  output += (high_nibble < 10) ? ('0' + high_nibble) : ('A' + high_nibble - 10);
                  output += (low_nibble < 10) ? ('0' + low_nibble) : ('A' + low_nibble - 10);
               }
               else {
                  output += c;
               }
               break;
            }
         }
         else {
            // Multibyte UTF-8 character
            uint32_t codepoint = 0;
            int bytes = 0;

            if ((c & 0xE0) == 0xC0) {
               // 2-byte sequence
               codepoint = c & 0x1F;
               bytes = 1;
            }
            else if ((c & 0xF0) == 0xE0) {
               // 3-byte sequence
               codepoint = c & 0x0F;
               bytes = 2;
            }
            else if ((c & 0xF8) == 0xF0) {
               // 4-byte sequence
               codepoint = c & 0x07;
               bytes = 3;
            }
            else {
               // Invalid UTF-8 start byte, replace with U+FFFD
               codepoint = 0xFFFD;
               bytes = 0;
            }

            bool invalid_sequence = false;

            for (int j = 0; j < bytes; ++j) {
               if (i == len) {
                  invalid_sequence = true;
                  break;
               }
               unsigned char c2 = static_cast<unsigned char>(input[i]);
               if ((c2 & 0xC0) != 0x80) {
                  invalid_sequence = true;
                  break;
               }
               codepoint = (codepoint << 6) | (c2 & 0x3F);
               ++i;
            }

            if (invalid_sequence) {
               // Invalid UTF-8 sequence, replace with U+FFFD
               codepoint = 0xFFFD;
            }

            if (codepoint <= 0xFFFF) {
               // BMP character
               append_unicode_escape(output, static_cast<uint16_t>(codepoint));
            }
            else {
               // Supplementary character (needs surrogate pair)
               codepoint -= 0x10000;
               uint16_t high_surrogate = uint16_t(0xD800 + (codepoint >> 10));
               uint16_t low_surrogate = uint16_t(0xDC00 + (codepoint & 0x3FF));
               append_unicode_escape(output, high_surrogate);
               append_unicode_escape(output, low_surrogate);
            }
         }
      }

      return output;
   }
}

namespace glz
{
   template <string_literal Str>
   inline constexpr auto escape_unicode = []() constexpr -> std::string_view {
      constexpr auto escaped = []() constexpr {
         constexpr auto len = detail::escaped_length(Str.sv());
         std::array<char, len + 1> result; // + 1 for null character
         const auto escaped = detail::escape_json_string(Str.sv(), len);
         for (size_t i = 0; i < len; ++i) {
            result[i] = escaped[i];
         }
         result[len] = '\0';
         return result;
      }();

      // make_static here required for GCC 12, in the future just make escaped static
      auto& arr = detail::make_static<escaped>::value;
      return {arr.data(), arr.size() - 1};
   }();
}

#endif // GLZ_HAS_CONSTEXPR_STRING
