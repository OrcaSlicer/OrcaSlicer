// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <iterator>
#include <span>

#include "glaze/core/context.hpp"
#include "glaze/core/meta.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/util/atoi.hpp"
#include "glaze/util/compare.hpp"
#include "glaze/util/convert.hpp"
#include "glaze/util/expected.hpp"
#include "glaze/util/inline.hpp"
#include "glaze/util/string_literal.hpp"

namespace glz
{
   inline constexpr std::array<bool, 256> numeric_table = [] {
      std::array<bool, 256> t{};
      t['0'] = true;
      t['1'] = true;
      t['2'] = true;
      t['3'] = true;
      t['4'] = true;
      t['5'] = true;
      t['6'] = true;
      t['7'] = true;
      t['8'] = true;
      t['9'] = true;
      t['.'] = true;
      t['+'] = true;
      t['-'] = true;
      t['e'] = true;
      t['E'] = true;
      return t;
   }();

   inline constexpr std::array<char, 256> char_unescape_table = [] {
      std::array<char, 256> t{};
      t['"'] = '"';
      t['/'] = '/';
      t['\\'] = '\\';
      t['b'] = '\b';
      t['f'] = '\f';
      t['n'] = '\n';
      t['r'] = '\r';
      t['t'] = '\t';
      return t;
   }();

   inline constexpr std::array<bool, 256> valid_escape_table = [] {
      std::array<bool, 256> t{};
      t['"'] = true;
      t['/'] = true;
      t['\\'] = true;
      t['b'] = true;
      t['f'] = true;
      t['n'] = true;
      t['r'] = true;
      t['t'] = true;
      t['u'] = true;
      return t;
   }();

   inline constexpr std::array<bool, 256> whitespace_table = [] {
      std::array<bool, 256> t{};
      t['\n'] = true;
      t['\t'] = true;
      t['\r'] = true;
      t[' '] = true;
      return t;
   }();

   // Whitespace plus JSON separators (comma, colon)
   inline constexpr std::array<bool, 256> whitespace_separator_table = [] {
      std::array<bool, 256> t{};
      t['\n'] = true;
      t['\t'] = true;
      t['\r'] = true;
      t[' '] = true;
      t[','] = true;
      t[':'] = true;
      return t;
   }();

   // Character classification for lazy JSON value skipping
   enum class lazy_char_type : uint8_t {
      other = 0, // whitespace, separators, literals - just advance
      quote = 1, // " - skip string
      open = 2, // { or [ - increase depth
      close = 3, // } or ] - decrease depth
      number = 4 // - or 0-9 - skip number
   };

   inline constexpr std::array<lazy_char_type, 256> lazy_char_class = [] {
      using enum lazy_char_type;
      std::array<lazy_char_type, 256> t{};
      t['"'] = quote;
      t['{'] = open;
      t['['] = open;
      t['}'] = close;
      t[']'] = close;
      t['-'] = number;
      t['0'] = number;
      t['1'] = number;
      t['2'] = number;
      t['3'] = number;
      t['4'] = number;
      t['5'] = number;
      t['6'] = number;
      t['7'] = number;
      t['8'] = number;
      t['9'] = number;
      return t;
   }();

   inline constexpr std::array<bool, 256> whitespace_comment_table = [] {
      std::array<bool, 256> t{};
      t['\n'] = true;
      t['\t'] = true;
      t['\r'] = true;
      t[' '] = true;
      t['/'] = true;
      return t;
   }();

   inline constexpr std::array<uint8_t, 256> digit_hex_table = [] {
      std::array<uint8_t, 256> t;
      std::fill(t.begin(), t.end(), uint8_t(255));
      t['0'] = 0;
      t['1'] = 1;
      t['2'] = 2;
      t['3'] = 3;
      t['4'] = 4;
      t['5'] = 5;
      t['6'] = 6;
      t['7'] = 7;
      t['8'] = 8;
      t['9'] = 9;
      t['a'] = 0xA;
      t['b'] = 0xB;
      t['c'] = 0xC;
      t['d'] = 0xD;
      t['e'] = 0xE;
      t['f'] = 0xF;
      t['A'] = 0xA;
      t['B'] = 0xB;
      t['C'] = 0xC;
      t['D'] = 0xD;
      t['E'] = 0xE;
      t['F'] = 0xF;
      return t;
   }();

   inline constexpr std::array<uint16_t, 256> char_escape_table = [] {
      // Build uint16_t so that memcpy produces chars in correct order.
      // On LE: chars[0] in low byte, chars[1] in high byte -> memcpy writes [chars[0]][chars[1]]
      // On BE: chars[0] in high byte, chars[1] in low byte -> memcpy writes [chars[0]][chars[1]]
      auto combine = [](const char chars[2]) -> uint16_t {
         if constexpr (std::endian::native == std::endian::big) {
            return (uint16_t(uint8_t(chars[0])) << 8) | uint16_t(uint8_t(chars[1]));
         }
         else {
            return uint16_t(uint8_t(chars[0])) | (uint16_t(uint8_t(chars[1])) << 8);
         }
      };

      std::array<uint16_t, 256> t{};
      t['\b'] = combine(R"(\b)");
      t['\t'] = combine(R"(\t)");
      t['\n'] = combine(R"(\n)");
      t['\f'] = combine(R"(\f)");
      t['\r'] = combine(R"(\r)");
      t['\"'] = combine(R"(\")");
      t['\\'] = combine(R"(\\)");
      return t;
   }();

   consteval uint32_t repeat_byte4(const auto repeat) { return uint32_t(0x01010101u) * uint8_t(repeat); }

   consteval uint64_t repeat_byte8(const uint8_t repeat) { return 0x0101010101010101ull * repeat; }

#if defined(__SIZEOF_INT128__)
   consteval __uint128_t repeat_byte16(const uint8_t repeat)
   {
      __uint128_t multiplier = (__uint128_t(0x0101010101010101ull) << 64) | 0x0101010101010101ull;
      return multiplier * repeat;
   }
#endif

   consteval uint64_t not_repeat_byte8(const uint8_t repeat) { return ~(0x0101010101010101ull * repeat); }

   [[nodiscard]] GLZ_ALWAYS_INLINE uint32_t hex_to_u32(const char* c) noexcept
   {
      constexpr auto& t = digit_hex_table;
      const uint8_t arr[4]{t[uint8_t(c[3])], t[uint8_t(c[2])], t[uint8_t(c[1])], t[uint8_t(c[0])]};
      auto chunk = std::bit_cast<uint32_t>(arr);
      // On big-endian, bit_cast produces bytes in opposite order than expected
      // byteswap to get consistent little-endian representation
      if constexpr (std::endian::native == std::endian::big) {
         chunk = std::byteswap(chunk);
      }
      // check that all hex characters are valid
      if (chunk & repeat_byte4(0b11110000u)) [[unlikely]] {
         return 0xFFFFFFFFu;
      }

      // now pack into first four bytes of uint32_t
      uint32_t packed{};
      packed |= (chunk & 0x0000000F);
      packed |= (chunk & 0x00000F00) >> 4;
      packed |= (chunk & 0x000F0000) >> 8;
      packed |= (chunk & 0x0F000000) >> 12;
      return packed;
   }

   template <class Char>
   [[nodiscard]] GLZ_ALWAYS_INLINE uint32_t code_point_to_utf8(const uint32_t code_point, Char* c) noexcept
   {
      if (code_point <= 0x7F) {
         c[0] = Char(code_point);
         return 1;
      }
      if (code_point <= 0x7FF) {
         c[0] = Char(0xC0 | ((code_point >> 6) & 0x1F));
         c[1] = Char(0x80 | (code_point & 0x3F));
         return 2;
      }
      if (code_point <= 0xFFFF) {
         c[0] = Char(0xE0 | ((code_point >> 12) & 0x0F));
         c[1] = Char(0x80 | ((code_point >> 6) & 0x3F));
         c[2] = Char(0x80 | (code_point & 0x3F));
         return 3;
      }
      if (code_point <= 0x10FFFF) {
         c[0] = Char(0xF0 | ((code_point >> 18) & 0x07));
         c[1] = Char(0x80 | ((code_point >> 12) & 0x3F));
         c[2] = Char(0x80 | ((code_point >> 6) & 0x3F));
         c[3] = Char(0x80 | (code_point & 0x3F));
         return 4;
      }
      return 0;
   }

   [[nodiscard]] GLZ_ALWAYS_INLINE uint32_t skip_code_point(const uint32_t code_point) noexcept
   {
      if (code_point <= 0x7F) {
         return 1;
      }
      if (code_point <= 0x7FF) {
         return 2;
      }
      if (code_point <= 0xFFFF) {
         return 3;
      }
      if (code_point <= 0x10FFFF) {
         return 4;
      }
      return 0;
   }

   namespace unicode
   {
      constexpr uint32_t generic_surrogate_mask = 0xF800;
      constexpr uint32_t generic_surrogate_value = 0xD800;

      constexpr uint32_t surrogate_mask = 0xFC00;
      constexpr uint32_t high_surrogate_value = 0xD800;
      constexpr uint32_t low_surrogate_value = 0xDC00;

      constexpr uint32_t surrogate_codepoint_offset = 0x10000;
      constexpr uint32_t surrogate_codepoint_mask = 0x03FF;
      constexpr uint32_t surrogate_codepoint_bits = 10;
   }

   template <class SrcChar, class DstChar = SrcChar>
   [[nodiscard]] GLZ_ALWAYS_INLINE uint32_t handle_unicode_code_point(const SrcChar*& it, DstChar*& dst,
                                                                      const SrcChar* end) noexcept
   {
      using namespace unicode;

      if (it + 4 >= end) [[unlikely]] {
         return false;
      }
      const uint32_t high = hex_to_u32(it);
      if (high == 0xFFFFFFFFu) [[unlikely]] {
         return false;
      }
      it += 4; // skip the code point characters

      uint32_t code_point;

      if ((high & generic_surrogate_mask) == generic_surrogate_value) {
         // surrogate pair code points
         if ((high & surrogate_mask) != high_surrogate_value) {
            return false;
         }

         if (it + 6 >= end) [[unlikely]] {
            return false;
         }
         // The next two characters must be `\u`
         uint16_t u;
         std::memcpy(&u, it, 2);
         if (u != to_uint16_t(R"(\u)")) [[unlikely]] {
            return false;
         }
         it += 2;
         // verify that second unicode escape sequence is present
         const uint32_t low = hex_to_u32(it);
         if (low == 0xFFFFFFFFu) [[unlikely]] {
            return false;
         }
         it += 4;

         if ((low & surrogate_mask) != low_surrogate_value) [[unlikely]] {
            return false;
         }

         code_point = (high & surrogate_codepoint_mask) << surrogate_codepoint_bits;
         code_point |= (low & surrogate_codepoint_mask);
         code_point += surrogate_codepoint_offset;
      }
      else {
         code_point = high;
      }
      const uint32_t offset = code_point_to_utf8(code_point, dst);
      dst += offset;
      return offset;
   }

   template <class Char>
   [[nodiscard]] GLZ_ALWAYS_INLINE bool skip_unicode_code_point(const Char*& it, const Char* end) noexcept
   {
      using namespace unicode;
      if (it + 4 >= end) [[unlikely]] {
         return false;
      }

      const uint32_t high = hex_to_u32(it);
      if (high == 0xFFFFFFFFu) [[unlikely]] {
         return false;
      }
      it += 4; // skip the code point characters

      uint32_t code_point;

      if ((high & generic_surrogate_mask) == generic_surrogate_value) {
         // surrogate pair code points
         if ((high & surrogate_mask) != high_surrogate_value) [[unlikely]] {
            return false;
         }

         if (it + 6 >= end) [[unlikely]] {
            return false;
         }
         // The next two characters must be `\u`
         uint16_t u;
         std::memcpy(&u, it, 2);
         if (u != to_uint16_t(R"(\u)")) [[unlikely]] {
            return false;
         }
         it += 2;
         // verify that second unicode escape sequence is present
         const uint32_t low = hex_to_u32(it);
         if (low == 0xFFFFFFFFu) [[unlikely]] {
            return false;
         }
         it += 4;

         if ((low & surrogate_mask) != low_surrogate_value) [[unlikely]] {
            return false;
         }

         code_point = (high & surrogate_codepoint_mask) << surrogate_codepoint_bits;
         code_point |= (low & surrogate_codepoint_mask);
         code_point += surrogate_codepoint_offset;
      }
      else {
         code_point = high;
      }
      return skip_code_point(code_point) > 0;
   }

   // Options struct for match_invalid_end - reduces template instantiations
   struct match_invalid_end_opts
   {
      bool null_terminated;

      // Convert from any opts-like type
      template <typename T>
      consteval match_invalid_end_opts(const T& opts) noexcept : null_terminated{opts.null_terminated}
      {}

      // Direct construction
      explicit consteval match_invalid_end_opts(bool null_terminated_) noexcept : null_terminated{null_terminated_} {}
   };

   // Checks for a character and validates that we are not at the end (considered an error)
   template <char C, match_invalid_end_opts Opts>
   GLZ_ALWAYS_INLINE bool match_invalid_end(is_context auto& ctx, auto&& it, auto end) noexcept
   {
      if (*it != C) [[unlikely]] {
         if constexpr (C == '"') {
            ctx.error = error_code::expected_quote;
         }
         else if constexpr (C == ',') {
            ctx.error = error_code::expected_comma;
         }
         else if constexpr (C == ':') {
            ctx.error = error_code::expected_colon;
         }
         else if constexpr (C == '[' || C == ']') {
            ctx.error = error_code::expected_bracket;
         }
         else if constexpr (C == '{' || C == '}') {
            ctx.error = error_code::expected_brace;
         }
         else {
            ctx.error = error_code::syntax_error;
         }
         return true;
      }
      else [[likely]] {
         ++it;
      }
      if constexpr (not Opts.null_terminated) {
         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return true;
         }
      }
      return false;
   }

   template <char C>
   GLZ_ALWAYS_INLINE bool match(is_context auto& ctx, auto&& it) noexcept
   {
      if (*it != C) [[unlikely]] {
         if constexpr (C == '"') {
            ctx.error = error_code::expected_quote;
         }
         else if constexpr (C == ',') {
            ctx.error = error_code::expected_comma;
         }
         else if constexpr (C == ':') {
            ctx.error = error_code::expected_colon;
         }
         else if constexpr (C == '[' || C == ']') {
            ctx.error = error_code::expected_bracket;
         }
         else if constexpr (C == '{' || C == '}') {
            ctx.error = error_code::expected_brace;
         }
         else {
            ctx.error = error_code::syntax_error;
         }
         return true;
      }
      else [[likely]] {
         ++it;
         return false;
      }
   }

   template <string_literal str, auto Opts>
      requires(check_is_padded(Opts) && str.size() <= padding_bytes)
   GLZ_ALWAYS_INLINE void match(is_context auto&& ctx, auto&& it, auto) noexcept
   {
      static constexpr auto S = str.sv();
      if (not comparitor<S>(it)) [[unlikely]] {
         ctx.error = error_code::syntax_error;
      }
      else [[likely]] {
         it += str.size();
      }
   }

   template <string_literal str, auto Opts>
      requires(!check_is_padded(Opts))
   GLZ_ALWAYS_INLINE void match(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      const auto n = size_t(end - it);
      static constexpr auto S = str.sv();
      if ((n < str.size()) || not comparitor<S>(it)) [[unlikely]] {
         ctx.error = error_code::syntax_error;
      }
      else [[likely]] {
         it += str.size();
      }
   }

   GLZ_ALWAYS_INLINE void skip_comment(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      ++it;
      if (it == end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
      }
      else if (*it == '/') {
         while (++it != end && *it != '\n');
      }
      else if (*it == '*') {
         while (++it != end) {
            if (*it == '*') [[unlikely]] {
               if (++it == end) [[unlikely]]
                  break;
               else if (*it == '/') [[likely]] {
                  ++it;
                  break;
               }
            }
         }
      }
      else [[unlikely]] {
         ctx.error = error_code::expected_end_comment;
      }
   }

   GLZ_ALWAYS_INLINE constexpr auto has_zero(const uint64_t chunk) noexcept
   {
      return (((chunk - 0x0101010101010101u) & ~chunk) & 0x8080808080808080u);
   }

   GLZ_ALWAYS_INLINE constexpr auto has_quote(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte8('"'));
   }

   GLZ_ALWAYS_INLINE constexpr auto has_escape(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte8('\\'));
   }

   GLZ_ALWAYS_INLINE constexpr auto has_space(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte8(' '));
   }

   template <char Char>
   GLZ_ALWAYS_INLINE constexpr auto has_char(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte8(Char));
   }

   GLZ_ALWAYS_INLINE constexpr uint64_t is_less_32(const uint64_t chunk) noexcept
   {
      return has_zero(chunk & repeat_byte8(0b11100000u));
   }

   GLZ_ALWAYS_INLINE constexpr uint64_t is_greater_15(const uint64_t chunk) noexcept
   {
      return (chunk & repeat_byte8(0b11110000u));
   }
}

namespace glz
{
   // Options struct for skip_ws - reduces template instantiations
   struct ws_opts
   {
      bool minified;
      bool null_terminated;
      bool comments;

      // Convert from any opts-like type
      template <typename T>
      consteval ws_opts(const T& opts) noexcept
         : minified{opts.minified}, null_terminated{opts.null_terminated}, comments{opts.comments}
      {}

      // Direct construction - all values required
      consteval ws_opts(bool minified_, bool null_terminated_, bool comments_) noexcept
         : minified{minified_}, null_terminated{null_terminated_}, comments{comments_}
      {}
   };

   // skip whitespace
   template <ws_opts Opts>
   GLZ_ALWAYS_INLINE bool skip_ws(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      using namespace glz::detail;

      if constexpr (not Opts.minified) {
         if constexpr (Opts.null_terminated) {
            if constexpr (Opts.comments) {
               while (whitespace_comment_table[uint8_t(*it)]) {
                  if (*it == '/') [[unlikely]] {
                     skip_comment(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]] {
                        return true;
                     }
                  }
                  else [[likely]] {
                     ++it;
                  }
               }
            }
            else {
               while (whitespace_table[uint8_t(*it)]) {
                  ++it;
               }
            }
         }
         else {
            if constexpr (Opts.comments) {
               while (it < end && whitespace_comment_table[uint8_t(*it)]) {
                  if (*it == '/') [[unlikely]] {
                     skip_comment(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]] {
                        return true;
                     }
                  }
                  else [[likely]] {
                     ++it;
                  }
               }
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::end_reached;
                  return true;
               }
            }
            else {
               while (it < end && whitespace_table[uint8_t(*it)]) {
                  ++it;
               }
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::end_reached;
                  return true;
               }
            }
         }
      }

      return false;
   }

   GLZ_ALWAYS_INLINE void skip_matching_ws(const auto* ws, auto&& it, uint64_t length) noexcept
   {
      if (length > 7) {
         uint64_t v[2];
         while (length > 8) {
            std::memcpy(v, ws, 8);
            std::memcpy(v + 1, it, 8);
            if (v[0] != v[1]) {
               return;
            }
            length -= 8;
            ws += 8;
            it += 8;
         }

         const auto shift = 8 - length;
         ws -= shift;
         it -= shift;

         std::memcpy(v, ws, 8);
         std::memcpy(v + 1, it, 8);
         return;
      }
      {
         constexpr uint64_t n{sizeof(uint32_t)};
         if (length >= n) {
            uint32_t v[2];
            std::memcpy(v, ws, n);
            std::memcpy(v + 1, it, n);
            if (v[0] != v[1]) {
               return;
            }
            length -= n;
            ws += n;
            it += n;
         }
      }
      {
         constexpr uint64_t n{sizeof(uint16_t)};
         if (length >= n) {
            uint16_t v[2];
            std::memcpy(v, ws, n);
            std::memcpy(v + 1, it, n);
            if (v[0] != v[1]) {
               return;
            }
            // length -= n;
            // ws += n;
            it += n;
         }
      }
      // We have to call a whitespace check after this function
      // in case the whitespace is mismatching.
      // So, we forgo this check as to not duplicate.
      /*if (length && *ws == *it) {
         ++it;
      }*/
   }

   // std::countr_zero uses another branch check whether the input is zero,
   // we use this function when we know that x > 0
   GLZ_ALWAYS_INLINE auto countr_zero(const uint32_t x) noexcept
   {
#ifdef _MSC_VER
      return std::countr_zero(x);
#else
#if __has_builtin(__builtin_ctzll)
      return __builtin_ctzl(x);
#else
      return std::countr_zero(x);
#endif
#endif
   }

   GLZ_ALWAYS_INLINE auto countr_zero(const uint64_t x) noexcept
   {
#ifdef _MSC_VER
      return std::countr_zero(x);
#else
#if __has_builtin(__builtin_ctzll)
      return __builtin_ctzll(x);
#else
      return std::countr_zero(x);
#endif
#endif
   }

#if defined(__SIZEOF_INT128__)
   GLZ_ALWAYS_INLINE auto countr_zero(__uint128_t x) noexcept
   {
      uint64_t low = uint64_t(x);
      if (low != 0) {
         return countr_zero(low);
      }
      else {
         uint64_t high = uint64_t(x >> 64);
         return countr_zero(high) + 64;
      }
   }
#endif

   GLZ_ALWAYS_INLINE void skip_till_quote(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      const auto* pc = std::memchr(it, '"', size_t(end - it));
      if (pc) [[likely]] {
         it = reinterpret_cast<std::decay_t<decltype(it)>>(pc);
         return;
      }

      ctx.error = error_code::expected_quote;
   }

   GLZ_ALWAYS_INLINE void skip_string_view(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      while (it < end) [[likely]] {
         const auto* pc = std::memchr(it, '"', size_t(end - it));
         if (pc) [[likely]] {
            it = reinterpret_cast<std::decay_t<decltype(it)>>(pc);
            auto* prev = it - 1;
            while (*prev == '\\') {
               --prev;
            }
            if (size_t(it - prev) % 2) {
               return;
            }
            ++it; // skip the escaped quote
         }
         else [[unlikely]] {
            break;
         }
      }

      ctx.error = error_code::expected_quote;
   }

   // Options struct for skip_string - reduces template instantiations
   struct skip_string_opts
   {
      bool padded;
      bool opening_handled;
      bool validate_skipped;

      // Convert from any opts-like type (consteval because check_* functions are consteval)
      template <typename T>
      consteval skip_string_opts(const T& opts) noexcept
         : padded{check_is_padded(opts)},
           opening_handled{check_opening_handled(opts)},
           validate_skipped{check_validate_skipped(opts)}
      {}

      // Direct construction - all values required
      consteval skip_string_opts(bool padded_, bool opening_handled_, bool validate_skipped_) noexcept
         : padded{padded_}, opening_handled{opening_handled_}, validate_skipped{validate_skipped_}
      {}
   };

   template <skip_string_opts Opts>
      requires(Opts.padded)
   GLZ_ALWAYS_INLINE void skip_string(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      if constexpr (not Opts.opening_handled) {
         ++it;
      }

      if constexpr (Opts.validate_skipped) {
         while (true) {
            uint64_t swar;
            std::memcpy(&swar, it, 8);
            if constexpr (std::endian::native == std::endian::big) {
               swar = std::byteswap(swar);
            }

            constexpr uint64_t lo7_mask = repeat_byte8(0b01111111);
            const uint64_t lo7 = swar & lo7_mask;
            const uint64_t backslash = (lo7 ^ repeat_byte8('\\')) + lo7_mask;
            const uint64_t quote = (lo7 ^ repeat_byte8('"')) + lo7_mask;
            const uint64_t less_32 = (swar & repeat_byte8(0b01100000)) + lo7_mask;
            uint64_t next = ~((backslash & quote & less_32) | swar);
            next &= repeat_byte8(0b10000000);

            if (next == 0) {
               // No special characters in this chunk
               it += 8;
               continue;
            }

            // Find the first occurrence
            size_t offset = (countr_zero(next) >> 3);
            it += offset;

            const auto c = *it;
            if ((c & 0b11100000) == 0) [[unlikely]] {
               // Invalid control character (<0x20)
               ctx.error = error_code::syntax_error;
               return;
            }
            else if (c == '"') {
               // Check if this quote is escaped
               const auto* p = it - 1;
               int backslash_count{};
               // We don't have to worry about rewinding too far because we started with a quote
               while (*p == '\\') {
                  ++backslash_count;
                  --p;
               }
               if ((backslash_count & 1) == 0) {
                  // Even number of backslashes => not escaped => closing quote found
                  ++it;
                  return;
               }
               else {
                  // Odd number of backslashes => escaped quote
                  ++it;
                  continue;
               }
            }
            else if (c == '\\') {
               // Handle escape sequence
               ++it;

               if (*it == 'u') {
                  ++it;
                  if (not skip_unicode_code_point(it, end)) [[unlikely]] {
                     ctx.error = error_code::unicode_escape_conversion_failure;
                     return;
                  }
               }
               else {
                  if (not char_unescape_table[uint8_t(*it)]) [[unlikely]] {
                     ctx.error = error_code::invalid_escape;
                     return;
                  }
                  ++it;
               }
            }
         }

         // If we exit here, we never found a closing quote
         ctx.error = error_code::unexpected_end;
      }
      else {
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         ++it; // skip the quote
      }
   }

   template <skip_string_opts Opts>
      requires(not Opts.padded)
   GLZ_ALWAYS_INLINE void skip_string(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      if constexpr (not Opts.opening_handled) {
         ++it;
      }

      if constexpr (Opts.validate_skipped) {
         while (true) {
            if ((*it & 0b11100000) == 0) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            switch (*it) {
            case '"': {
               ++it;
               return;
            }
            case '\\': {
               ++it;
               if (char_unescape_table[uint8_t(*it)]) {
                  ++it;
                  continue;
               }
               else if (*it == 'u') {
                  ++it;
                  if (skip_unicode_code_point(it, end)) [[likely]] {
                     continue;
                  }
                  else [[unlikely]] {
                     ctx.error = error_code::unicode_escape_conversion_failure;
                     return;
                  }
               }
               ctx.error = error_code::syntax_error;
               return;
            }
            }
            ++it;
         }
      }
      else {
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         ++it; // skip the quote
      }
   }

   // Options struct for skip_until_closed - reduces template instantiations
   struct skip_until_closed_opts
   {
      bool padded;
      bool comments;

      // Convert from any opts-like type (consteval because check_is_padded is consteval)
      template <typename T>
      consteval skip_until_closed_opts(const T& opts) noexcept : padded{check_is_padded(opts)}, comments{opts.comments}
      {}

      // Direct construction - all values required
      consteval skip_until_closed_opts(bool padded_, bool comments_) noexcept : padded{padded_}, comments{comments_} {}
   };

   template <skip_until_closed_opts Opts, char open, char close, size_t Depth = 1>
      requires(Opts.padded && not Opts.comments)
   GLZ_ALWAYS_INLINE void skip_until_closed(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      static constexpr bool opening_not_handled = false;
      static constexpr bool skip_validation = false;

      size_t depth = Depth;

      while (it < end) [[likely]] {
         uint64_t chunk;
         std::memcpy(&chunk, it, 8);
         if constexpr (std::endian::native == std::endian::big) {
            chunk = std::byteswap(chunk);
         }
         const uint64_t test = has_quote(chunk) | has_char<open>(chunk) | has_char<close>(chunk);
         if (test) {
            it += (countr_zero(test) >> 3);

            switch (*it) {
            case '"': {
               skip_string<skip_string_opts{Opts.padded, opening_not_handled, skip_validation}>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
            case open: {
               ++it;
               ++depth;
               break;
            }
            case close: {
               ++it;
               --depth;
               if (depth == 0) {
                  return;
               }
               break;
            }
            default: {
               ctx.error = error_code::unexpected_end;
               return;
            }
            }
         }
         else {
            it += 8;
         }
      }

      ctx.error = error_code::unexpected_end;
   }

   template <skip_until_closed_opts Opts, char open, char close, size_t Depth = 1>
      requires(Opts.padded && Opts.comments)
   GLZ_ALWAYS_INLINE void skip_until_closed(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      static constexpr bool opening_not_handled = false;
      static constexpr bool skip_validation = false;

      size_t depth = Depth;

      while (it < end) [[likely]] {
         uint64_t chunk;
         std::memcpy(&chunk, it, 8);
         if constexpr (std::endian::native == std::endian::big) {
            chunk = std::byteswap(chunk);
         }
         const uint64_t test = has_quote(chunk) | has_char<'/'>(chunk) | has_char<open>(chunk) | has_char<close>(chunk);
         if (test) {
            it += (countr_zero(test) >> 3);

            switch (*it) {
            case '"': {
               skip_string<skip_string_opts{Opts.padded, opening_not_handled, skip_validation}>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
            case '/': {
               skip_comment(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
            case open: {
               ++it;
               ++depth;
               break;
            }
            case close: {
               ++it;
               --depth;
               if (depth == 0) {
                  return;
               }
               break;
            }
            default: {
               ctx.error = error_code::unexpected_end;
               return;
            }
            }
         }
         else {
            it += 8;
         }
      }

      ctx.error = error_code::unexpected_end;
   }

   template <skip_until_closed_opts Opts, char open, char close, size_t Depth = 1>
      requires(not Opts.padded && not Opts.comments)
   GLZ_ALWAYS_INLINE void skip_until_closed(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      static constexpr bool opening_not_handled = false;
      static constexpr bool skip_validation = false;

      size_t depth = Depth;

      for (const auto fin = end - 7; it < fin;) {
         uint64_t chunk;
         std::memcpy(&chunk, it, 8);
         if constexpr (std::endian::native == std::endian::big) {
            chunk = std::byteswap(chunk);
         }
         const uint64_t test = has_quote(chunk) | has_char<open>(chunk) | has_char<close>(chunk);
         if (test) {
            it += (countr_zero(test) >> 3);

            switch (*it) {
            case '"': {
               skip_string<skip_string_opts{Opts.padded, opening_not_handled, skip_validation}>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
            case open: {
               ++it;
               ++depth;
               break;
            }
            case close: {
               ++it;
               --depth;
               if (depth == 0) {
                  return;
               }
               break;
            }
            default: {
               ctx.error = error_code::unexpected_end;
               return;
            }
            }
         }
         else {
            it += 8;
         }
      }

      // Tail end of buffer. Should be rare we even get here
      while (it < end) {
         switch (*it) {
         case '"': {
            skip_string<skip_string_opts{Opts.padded, opening_not_handled, skip_validation}>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            break;
         }
         case '/': {
            skip_comment(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            break;
         }
         case open: {
            ++it;
            ++depth;
            break;
         }
         case close: {
            ++it;
            --depth;
            if (depth == 0) {
               return;
            }
            break;
         }
         default: {
            ++it;
         }
         }
      }

      ctx.error = error_code::unexpected_end;
   }

   template <skip_until_closed_opts Opts, char open, char close, size_t Depth = 1>
      requires(not Opts.padded && Opts.comments)
   GLZ_ALWAYS_INLINE void skip_until_closed(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      static constexpr bool opening_not_handled = false;
      static constexpr bool skip_validation = false;

      size_t depth = Depth;

      for (const auto fin = end - 7; it < fin;) {
         uint64_t chunk;
         std::memcpy(&chunk, it, 8);
         if constexpr (std::endian::native == std::endian::big) {
            chunk = std::byteswap(chunk);
         }
         const uint64_t test = has_quote(chunk) | has_char<'/'>(chunk) | has_char<open>(chunk) | has_char<close>(chunk);
         if (test) {
            it += (countr_zero(test) >> 3);

            switch (*it) {
            case '"': {
               skip_string<skip_string_opts{Opts.padded, opening_not_handled, skip_validation}>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
            case '/': {
               skip_comment(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
            case open: {
               ++it;
               ++depth;
               break;
            }
            case close: {
               ++it;
               --depth;
               if (depth == 0) {
                  return;
               }
               break;
            }
            default: {
               ctx.error = error_code::unexpected_end;
               return;
            }
            }
         }
         else {
            it += 8;
         }
      }

      // Tail end of buffer. Should be rare we even get here
      while (it < end) {
         switch (*it) {
         case '"': {
            skip_string<skip_string_opts{Opts.padded, opening_not_handled, skip_validation}>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            break;
         }
         case '/': {
            skip_comment(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            break;
         }
         case open: {
            ++it;
            ++depth;
            break;
         }
         case close: {
            ++it;
            --depth;
            if (depth == 0) {
               return;
            }
            break;
         }
         default: {
            ++it;
         }
         }
      }

      ctx.error = error_code::unexpected_end;
   }

   inline constexpr std::optional<uint64_t> stoui(const std::string_view s) noexcept
   {
      if (s.empty()) {
         return {};
      }

      uint64_t ret;
      auto* c = s.data();
      bool valid = detail::stoui64(ret, c);
      if (valid) {
         return ret;
      }
      return {};
   }

   GLZ_ALWAYS_INLINE void skip_number_with_validation(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      it += *it == '-';
      const auto sig_start_it = it;
      auto frac_start_it = end;
      if (*it == '0') {
         ++it;
         if (*it != '.') {
            return;
         }
         ++it;
         goto frac_start;
      }
      it = std::find_if_not(it, end, is_digit);
      if (it == sig_start_it) {
         ctx.error = error_code::syntax_error;
         return;
      }
      if ((*it | ('E' ^ 'e')) == 'e') {
         ++it;
         goto exp_start;
      }
      if (*it != '.') return;
      ++it;
   frac_start:
      frac_start_it = it;
      it = std::find_if_not(it, end, is_digit);
      if (it == frac_start_it) {
         ctx.error = error_code::syntax_error;
         return;
      }
      if ((*it | ('E' ^ 'e')) != 'e') return;
      ++it;
   exp_start:
      it += *it == '+' || *it == '-';
      const auto exp_start_it = it;
      it = std::find_if_not(it, end, is_digit);
      if (it == exp_start_it) {
         ctx.error = error_code::syntax_error;
         return;
      }
   }

   // Options struct for skip_number - reduces template instantiations
   struct skip_number_opts
   {
      bool validate;

      // Convert from any opts-like type (consteval because check_validate_skipped is consteval)
      template <typename T>
      consteval skip_number_opts(const T& opts) noexcept : validate{check_validate_skipped(opts)}
      {}

      // Direct construction
      explicit consteval skip_number_opts(bool validate_) noexcept : validate{validate_} {}
   };

   template <skip_number_opts Opts>
   GLZ_ALWAYS_INLINE void skip_number(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      if constexpr (not Opts.validate) {
         while (numeric_table[uint8_t(*it)]) {
            ++it;
         }
      }
      else {
         skip_number_with_validation(ctx, it, end);
      }
   }

   // expects opening whitespace to be handled
   GLZ_ALWAYS_INLINE sv parse_key(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      // TODO this assumes no escapes.
      if (bool(ctx.error)) [[unlikely]]
         return {};

      if (match<'"'>(ctx, it)) {
         return {};
      }
      auto start = it;
      skip_till_quote(ctx, it, end);
      if (bool(ctx.error)) [[unlikely]]
         return {};
      return sv{start, static_cast<size_t>(it++ - start)};
   }

   template <size_t multiple>
   GLZ_ALWAYS_INLINE constexpr auto round_up_to_multiple(const std::integral auto val) noexcept
   {
      return val + (multiple - (val % multiple)) % multiple;
   }

   inline bool validate_utf8(const auto* str, const size_t size) noexcept
   {
      const uint8_t* it = reinterpret_cast<const uint8_t*>(str);
      const uint8_t* end = it + size;

      while (it < end) {
         // Optimistic SWAR check for ASCII
         if (it + 8 <= end) {
            uint64_t chunk;
            std::memcpy(&chunk, it, 8);
            if ((chunk & glz::repeat_byte8(0x80)) == 0) {
               it += 8;
               continue;
            }
         }

         // Byte-by-byte validation (standard conformant)
         uint8_t byte = *it;

         if (byte < 0x80) {
            it++;
         }
         else if ((byte & 0xE0) == 0xC0) {
            // 2-byte sequence
            if (it + 2 > end || (it[1] & 0xC0) != 0x80) return false;
            if (byte < 0xC2) return false; // Overlong
            it += 2;
         }
         else if ((byte & 0xF0) == 0xE0) {
            // 3-byte sequence
            if (it + 3 > end || (it[1] & 0xC0) != 0x80 || (it[2] & 0xC0) != 0x80) return false;
            if (byte == 0xE0 && it[1] < 0xA0) return false; // Overlong
            if (byte == 0xED && it[1] >= 0xA0) return false; // Surrogate
            it += 3;
         }
         else if ((byte & 0xF8) == 0xF0) {
            // 4-byte sequence
            if (it + 4 > end || (it[1] & 0xC0) != 0x80 || (it[2] & 0xC0) != 0x80 || (it[3] & 0xC0) != 0x80)
               return false;
            if (byte == 0xF0 && it[1] < 0x90) return false; // Overlong
            if (byte == 0xF4 && it[1] >= 0x90) return false; // > U+10FFFF
            if (byte > 0xF4) return false; // > U+10FFFF
            it += 4;
         }
         else {
            return false;
         }
      }

      return true;
   }
}

namespace glz
{
   // TODO: GCC 12 lacks constexpr std::from_chars
   // Remove this code when dropping GCC 12 support
   namespace detail
   {
      struct from_chars_result
      {
         const char* ptr;
         std::errc ec;
      };

      inline constexpr int char_to_digit(char c) noexcept
      {
         if (c >= '0' && c <= '9') return c - '0';
         if (c >= 'a' && c <= 'z') return c - 'a' + 10;
         if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
         return -1;
      }

      template <class I>
      constexpr from_chars_result from_chars(const char* first, const char* last, I& value, int base = 10)
      {
         from_chars_result result{first, std::errc{}};

         // Basic validation of base
         if (base < 2 || base > 36) {
            // Not standard behavior to check base validity here, but let's return invalid_argument
            result.ec = std::errc::invalid_argument;
            return result;
         }

         using U = std::make_unsigned_t<I>;
         constexpr bool is_signed = std::is_signed<I>::value;
         constexpr U umax = (std::numeric_limits<U>::max)();

         if (first == last) {
            // Empty range
            result.ec = std::errc::invalid_argument;
            return result;
         }

         bool negative = false;
         // Check for sign only if signed type
         if constexpr (is_signed) {
            if (*first == '-') {
               negative = true;
               ++first;
            }
            else if (*first == '+') {
               ++first;
            }
         }

         if (first == last) {
            // After sign there's nothing
            result.ec = std::errc::invalid_argument;
            return result;
         }

         U acc = 0;
         bool any = false;

         // We'll do overflow checking as we parse
         // For accumulation: acc * base + digit
         // Overflow if acc > (umax - digit)/base

         while (first != last) {
            int d = char_to_digit(*first);
            if (d < 0 || d >= base) break;

            // Check overflow before multiplying/adding
            if (acc > (umax - static_cast<U>(d)) / static_cast<U>(base)) {
               // Overflow
               result.ec = std::errc::result_out_of_range;
               // We still move ptr to the last valid digit parsed
               result.ptr = first;
               // No need to parse further; we know it's out of range.
               return result;
            }

            acc = acc * base + static_cast<U>(d);
            any = true;
            ++first;
         }

         if (!any) {
            // No digits parsed
            result.ec = std::errc::invalid_argument;
            return result;
         }

         // If signed and negative, check if result fits
         if constexpr (is_signed) {
            // The largest magnitude we can represent in a negative value is (max + 1)
            // since -(min()) = max() + 1.
            constexpr U limit = static_cast<U>((std::numeric_limits<I>::max)()) + 1U;
            if (negative) {
               if (acc > limit) {
                  result.ec = std::errc::result_out_of_range;
                  result.ptr = first;
                  return result;
               }
               // Negate in unsigned arithmetic to avoid signed overflow when acc == limit
               // (e.g., when parsing -2147483648, acc = 2147483648u for int32)
#if defined(_MSC_VER) && !defined(__clang__)
               // Use subtraction from zero instead of unary minus to avoid MSVC C4146 error
               value = static_cast<I>(U{0} - acc);
#else
               value = static_cast<I>(-acc);
#endif
            }
            else {
               if (acc > static_cast<U>((std::numeric_limits<I>::max)())) {
                  result.ec = std::errc::result_out_of_range;
                  result.ptr = first;
                  return result;
               }
               value = static_cast<I>(acc);
            }
         }
         else {
            // Unsigned type
            value = acc;
         }

         result.ptr = first;
         return result;
      }
   }
}
