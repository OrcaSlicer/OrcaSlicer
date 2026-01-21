// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cctype>
#include <charconv>
#include <iostream>

#include "glaze/core/chrono.hpp"
#include "glaze/core/common.hpp"
// #include "glaze/core/opts.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/file/file_ops.hpp"
#include "glaze/toml/common.hpp"
#include "glaze/toml/opts.hpp"
#include "glaze/toml/skip.hpp"
#include "glaze/util/glaze_fast_float.hpp"
#include "glaze/util/parse.hpp"
#include "glaze/util/type_traits.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   template <>
   struct parse<TOML>
   {
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = std::remove_cvref_t<T>;
         from<TOML, V>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it), end);
      }
   };

   // Parse TOML key (bare key or quoted key)
   template <class Ctx, class It, class End>
   GLZ_ALWAYS_INLINE bool parse_toml_key(std::string& key, Ctx& ctx, It&& it, End end) noexcept
   {
      key.clear();
      skip_ws_and_comments(it, end);

      if (it == end) {
         ctx.error = error_code::unexpected_end;
         return false;
      }

      if (*it == '"') {
         // Quoted key
         ++it;
         while (it != end && *it != '"') {
            if (*it == '\\') {
               ++it;
               if (it == end) {
                  ctx.error = error_code::unexpected_end;
                  return false;
               }
               switch (*it) {
               case '"':
                  key.push_back('"');
                  break;
               case '\\':
                  key.push_back('\\');
                  break;
               case 'n':
                  key.push_back('\n');
                  break;
               case 't':
                  key.push_back('\t');
                  break;
               case 'r':
                  key.push_back('\r');
                  break;
               default:
                  key.push_back('\\');
                  key.push_back(*it);
                  break;
               }
            }
            else {
               key.push_back(*it);
            }
            ++it;
         }

         if (it == end || *it != '"') {
            ctx.error = error_code::syntax_error;
            return false;
         }
         ++it; // Skip closing quote
      }
      else {
         // Bare key
         while (it != end && (std::isalnum(*it) || *it == '_' || *it == '-')) {
            key.push_back(*it);
            ++it;
         }

         if (key.empty()) {
            ctx.error = error_code::syntax_error;
            return false;
         }
      }

      return true;
   }

   template <class Ctx, class It, class End>
   GLZ_ALWAYS_INLINE bool parse_toml_key(std::vector<std::string>& keys, Ctx& ctx, It& it, End end) noexcept
   {
      keys.clear();
      skip_ws_and_comments(it, end);

      if (it == end) {
         ctx.error = error_code::unexpected_end;
         return false;
      }

      while (true) {
         std::string key;

         if (it == end) {
            ctx.error = error_code::unexpected_end;
            return false;
         }

         if (*it == '"') {
            ++it;
            while (it != end && *it != '"') {
               if (*it == '\\') {
                  ++it;
                  if (it == end) {
                     ctx.error = error_code::unexpected_end;
                     return false;
                  }
                  switch (*it) {
                  case '"':
                     key.push_back('"');
                     break;
                  case '\\':
                     key.push_back('\\');
                     break;
                  case 'n':
                     key.push_back('\n');
                     break;
                  case 't':
                     key.push_back('\t');
                     break;
                  case 'r':
                     key.push_back('\r');
                     break;
                  default:
                     key.push_back('\\');
                     key.push_back(*it);
                     break;
                  }
               }
               else {
                  key.push_back(*it);
               }
               ++it;
            }

            if (it == end || *it != '"') {
               ctx.error = error_code::syntax_error;
               return false;
            }
            ++it; // skip closing quote
         }
         else {
            while (it != end && (std::isalnum(*it) || *it == '_' || *it == '-')) {
               key.push_back(*it);
               ++it;
            }

            if (key.empty()) {
               ctx.error = error_code::syntax_error;
               return false;
            }
         }

         keys.push_back(std::move(key));

         skip_ws_and_comments(it, end);

         if (it == end || *it != '.') {
            break;
         }

         ++it; // skip '.', We know it's '.' because we've checked before
         skip_ws_and_comments(it, end);

         if (it == end) {
            ctx.error = error_code::unexpected_end;
            return false;
         }
      }

      return true;
   }

   template <glaze_value_t T>
   struct from<TOML, T>
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = decltype(get_member(std::declval<T>(), meta_wrapper_v<T>));
         from<TOML, V>::template op<Opts>(get_member(value, meta_wrapper_v<T>), std::forward<Ctx>(ctx),
                                          std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   namespace detail
   {
      constexpr bool is_toml_base_specifier(char c) noexcept { return (c == 'x' || c == 'b' || c == 'o'); }

      constexpr int toml_specified_base(char c) noexcept
      {
         switch (c) {
         case 'x':
            return 16;
         case 'b':
            return 2;
         case 'o':
            return 8;
         }

         // Should never reach here, but seems a good default.
         return 10;
      }

      constexpr bool is_any_toml_digit(char c) noexcept
      {
         if (c >= '0' && c <= '9') {
            return true;
         }

         if (c >= 'a' && c <= 'f') {
            return true;
         }

         if (c >= 'A' && c <= 'F') {
            return true;
         }

         return false;
      }

      constexpr bool is_valid_toml_digit(char c, int base) noexcept
      {
         if (base <= 10) {
            return c >= '0' && c < ('0' + base);
         }

         return is_any_toml_digit(c);
      }

      struct unsigned_accumulator
      {
        private:
         int base = 10;

        public:
         constexpr void inform_base(int base) noexcept { this->base = base; }

         constexpr bool is_valid_digit(char c) const noexcept { return is_valid_toml_digit(c, this->base); }

         template <std::unsigned_integral T>
         constexpr bool try_accumulate(T& v, int digit) const noexcept
         {
            if (digit < 0 || digit >= this->base) [[unlikely]] {
               return false;
            }

            // Check overflow.
            const auto max_without_overflow = ((std::numeric_limits<T>::max)() - digit) / this->base;
            if (v > max_without_overflow) [[unlikely]] {
               return false;
            }

            v = v * this->base + digit;

            return true;
         }
      };

      struct signed_accumulator
      {
        private:
         int base = 10;
         bool negated = false;

        public:
         constexpr void inform_negated() noexcept { this->negated = true; }

         constexpr void inform_base(int base) noexcept { this->base = base; }

         constexpr bool is_valid_digit(char c) const noexcept { return is_valid_toml_digit(c, this->base); }

         template <std::signed_integral T>
         constexpr bool try_accumulate(T& v, int digit) const noexcept
         {
            if (digit < 0 || digit >= this->base) [[unlikely]] {
               return false;
            }

            if (this->negated) {
               // Check underflow.
               const auto min_without_underflow = ((std::numeric_limits<T>::min)() + digit) / this->base;
               if (v < min_without_underflow) [[unlikely]] {
                  return false;
               }

               v = v * this->base - digit;

               return true;
            }

            // Check overflow.
            const auto max_without_overflow = ((std::numeric_limits<T>::max)() - digit) / this->base;
            if (v > max_without_overflow) [[unlikely]] {
               return false;
            }

            v = v * this->base + digit;

            return true;
         }
      };

      template <std::integral T>
      constexpr bool parse_toml_integer(T& v, auto&& it, auto end) noexcept
      {
         // Ensure the caller gave us at least one character.
         if (it == end) [[unlikely]] {
            return false;
         }

         auto accumulator = [] {
            if constexpr (std::unsigned_integral<T>) {
               return unsigned_accumulator{};
            }
            else {
               return signed_accumulator{};
            }
         }();

         bool saw_negative_sign = false;
         bool saw_sign = false; // Tracks whether +/- was supplied so prefixed bases can reject it.

         if (*it == '-') {
            saw_negative_sign = true;
            saw_sign = true;
            ++it;

            if (it == end) [[unlikely]] {
               return false;
            }

            if constexpr (std::signed_integral<T>) {
               accumulator.inform_negated();
            }
         }

         if (*it == '+') {
            // Only a single leading sign is permitted.
            if (saw_sign) [[unlikely]] {
               return false;
            }

            ++it;
            saw_sign = true;

            if (it == end) [[unlikely]] {
               return false;
            }
         }

         if (it == end) [[unlikely]] {
            return false;
         }

         if (*it == '_') [[unlikely]] {
            return false;
         }

         int base = 10;
         bool base_overridden = false;

         if (*it == '0') {
            auto next = it;
            ++next;

            if (next == end) {
               it = next;
               v = 0;
               return true;
            }

            if (*next == '_') [[unlikely]] {
               return false;
            }

            if (is_toml_base_specifier(*next)) {
               // Switching to hexadecimal/octal/binary after 0.
               base = toml_specified_base(*next);
               base_overridden = true;

               it = next;
               ++it;

               if (it == end) [[unlikely]] {
                  return false;
               }

               if (not is_valid_toml_digit(*it, base)) [[unlikely]] {
                  return false;
               }
            }
            else if (is_any_toml_digit(*next)) [[unlikely]] {
               // Decimal literals like 0123 are invalid.
               return false;
            }
            else {
               it = next;
               v = 0;
               return true;
            }
         }

         if (base_overridden) {
            accumulator.inform_base(base);

            // Only non-negative values are allowed for prefixed bases.
            if (saw_sign) [[unlikely]] {
               return false;
            }
         }

         v = 0;
         bool saw_digit = false;

         // Consume digits (with optional underscores) while respecting overflow limits.
         while (true) {
            if (it == end) {
               break;
            }

            if (*it == '_') {
               ++it;

               if (it == end) [[unlikely]] {
                  return false;
               }

               if (not is_any_toml_digit(*it)) [[unlikely]] {
                  return false;
               }
            }
            else if (not is_any_toml_digit(*it)) {
               break;
            }

            const auto digit = char_to_digit(*it);

            if (not accumulator.try_accumulate(v, digit)) {
               return false;
            }

            saw_digit = true;
            ++it;
         }

         if (not saw_digit) [[unlikely]] {
            return false;
         }

         if constexpr (std::unsigned_integral<T>) {
            if (saw_negative_sign && v != 0) [[unlikely]] {
               return false;
            }
         }

         return true;
      }
   }

   template <num_t T>
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         using V = decay_keep_volatile_t<decltype(value)>;
         if constexpr (int_t<V>) {
            if (not detail::parse_toml_integer(value, it, end)) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }
         }
         else {
            auto [ptr, ec] = glz::from_chars<false>(it, end, value); // Always treat as non-null-terminated
            if (ec != std::errc()) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }
            it = ptr;
         }
      }
   };

   template <string_t T>
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         value.clear();
         skip_ws_and_comments(it, end);

         if (it == end) {
            ctx.error = error_code::unexpected_end;
            return;
         }

         if (it + 2 < end && *it == '"' && *(it + 1) == '"' && *(it + 2) == '"') {
            // Basic Multiline String
            it += 3; // Skip """
            if (it != end && *it == '\n') { // Skip initial newline
               ++it;
            }
            else if (it + 1 < end && *it == '\r' && *(it + 1) == '\n') { // Skip initial CRLF
               it += 2;
            }

            while (it + 2 < end && !(*it == '"' && *(it + 1) == '"' && *(it + 2) == '"')) {
               if (*it == '\\') {
                  ++it;
                  if (it == end) {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  switch (*it) {
                  case '"':
                     value.push_back('"');
                     break;
                  case '\\':
                     value.push_back('\\');
                     break;
                  case 'n':
                     value.push_back('\n');
                     break;
                  case 't':
                     value.push_back('\t');
                     break;
                  case 'r':
                     value.push_back('\r');
                     break;
                  case 'b':
                     value.push_back('\b');
                     break;
                  case 'f':
                     value.push_back('\f');
                     break;
                  // TOML: Any other character is an error for escape sequences in basic strings
                  // However, we also need to handle escaped newlines for line trimming
                  case '\n': /* ignore escaped newline */
                     // Trim all whitespace after escaped newline until non-whitespace or actual newline
                     while (it + 1 < end &&
                            (*(it + 1) == ' ' || *(it + 1) == '\t' || *(it + 1) == '\r' || *(it + 1) == '\n')) {
                        ++it;
                        if (*it == '\n') break; // Stop if we hit an actual newline
                     }
                     break;
                  case '\r': // part of CRLF, handle similar to \n
                     if (it + 1 < end && *(it + 1) == '\n') ++it;
                     while (it + 1 < end &&
                            (*(it + 1) == ' ' || *(it + 1) == '\t' || *(it + 1) == '\r' || *(it + 1) == '\n')) {
                        ++it;
                        if (*it == '\n') break;
                     }
                     break;
                  default:
                     // In TOML, an unknown escape sequence is an error.
                     // For simplicity here, we might just append them or flag an error.
                     // Glaze JSON parser often appends, let's be stricter for TOML.
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else {
                  value.push_back(*it);
               }
               ++it;
            }

            if (it + 2 >= end || !(*it == '"' && *(it + 1) == '"' && *(it + 2) == '"')) {
               ctx.error = error_code::syntax_error; // Unterminated multiline string
               return;
            }
            it += 3; // Skip closing """
         }
         else if (*it == '"') {
            // Basic string
            ++it; // Skip opening quote

            while (it != end && *it != '"') {
               if (*it == '\\') {
                  ++it;
                  if (it == end) {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  switch (*it) {
                  case '"':
                     value.push_back('"');
                     break;
                  case '\\':
                     value.push_back('\\');
                     break;
                  case 'n':
                     value.push_back('\n');
                     break;
                  case 't':
                     value.push_back('\t');
                     break;
                  case 'r':
                     value.push_back('\r');
                     break;
                  case 'b':
                     value.push_back('\b');
                     break;
                  case 'f':
                     value.push_back('\f');
                     break;
                  default:
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else if (*it == '\n' || *it == '\r') { // Newlines not allowed in single-line basic strings
                  ctx.error = error_code::syntax_error;
                  return;
               }
               else {
                  value.push_back(*it);
               }
               ++it;
            }

            if (it == end || *it != '"') {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it; // Skip closing quote
         }
         else if (it + 2 < end && *it == '\'' && *(it + 1) == '\'' && *(it + 2) == '\'') {
            // Literal Multiline String
            it += 3; // Skip '''
            if (it != end && *it == '\n') { // Skip initial newline
               ++it;
            }
            else if (it + 1 < end && *it == '\r' && *(it + 1) == '\n') { // Skip initial CRLF
               it += 2;
            }

            while (it + 2 < end && !(*it == '\'' && *(it + 1) == '\'' && *(it + 2) == '\'')) {
               value.push_back(*it);
               ++it;
            }

            if (it + 2 >= end || !(*it == '\'' && *(it + 1) == '\'' && *(it + 2) == '\'')) {
               ctx.error = error_code::syntax_error; // Unterminated multiline literal string
               return;
            }
            it += 3; // Skip closing '''
         }
         else if (*it == '\'') {
            // Literal string
            ++it; // Skip opening quote

            while (it != end && *it != '\'') {
               if (*it == '\n' || *it == '\r') { // Newlines not allowed in single-line literal strings
                  ctx.error = error_code::syntax_error;
                  return;
               }
               value.push_back(*it);
               ++it;
            }

            if (it == end || *it != '\'') {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it; // Skip closing quote
         }
         else {
            // Bare string (not typical in TOML values, but handle it if necessary for some edge cases)
            // TOML spec does not allow bare strings for values. This path should ideally not be hit for valid TOML.
            // If it is, it's likely an error or a deviation.
            // For now, let's assume this is an error according to strict TOML.
            ctx.error = error_code::syntax_error;
            return;
            /*
            while (it != end && *it != '\n' && *it != '\r' && *it != '#') {
               value.push_back(*it);
               ++it;
            }

            // Trim trailing whitespace
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
               value.pop_back();
            }
            */
         }
      }
   };

   template <bool_t T>
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         if (it + 4 <= end &&
             std::string_view(it, 4) == "true") { // TODO: Maybe we can use the bool_true from header.hpp
            value = true;
            it += 4;
         }
         else if (it + 5 <= end &&
                  std::string_view(it, 5) == "false") { // TODO: Maybe we can use the bool_false from header.hpp
            value = false;
            it += 5;
         }
         else {
            ctx.error = error_code::expected_true_or_false;
            return;
         }
      }
   };

   // Enum with glz::meta/glz::enumerate - reads string representation
   template <class T>
      requires(is_named_enum<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // TOML enum values are expected as quoted strings: "EnumValue"
         if (*it != '"') [[unlikely]] {
            ctx.error = error_code::expected_quote;
            return;
         }
         ++it;

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // Find the closing quote to determine string length
         auto start = it;
         while (it != end && *it != '"') {
            ++it;
         }

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         const auto n = static_cast<size_t>(it - start);
         ++it; // skip closing quote

         constexpr auto N = reflect<T>::size;

         if constexpr (N == 1) {
            // Single enum value - just verify it matches
            static constexpr auto key = glz::get<0>(reflect<T>::keys);
            if (n == key.size() && std::string_view(start, n) == key) {
               value = glz::get<0>(reflect<T>::values);
            }
            else {
               ctx.error = error_code::unexpected_enum;
            }
         }
         else {
            static constexpr auto HashInfo = hash_info<T>;

            const auto index = decode_hash_with_size<TOML, T, HashInfo, HashInfo.type>::op(start, start + n, n);

            if (index >= N) [[unlikely]] {
               ctx.error = error_code::unexpected_enum;
               return;
            }

            // Use visit to convert runtime index to compile-time index
            visit<N>(
               [&]<size_t I>() {
                  // Verify the key matches (hash collision check)
                  static constexpr auto key = glz::get<I>(reflect<T>::keys);
                  if (n == key.size() && std::string_view(start, n) == key) [[likely]] {
                     value = glz::get<I>(reflect<T>::values);
                  }
                  else {
                     ctx.error = error_code::unexpected_enum;
                  }
               },
               index);
         }
      }
   };

   // Raw enum (without glz::meta) - reads as underlying numeric type
   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T> && !meta_keys<T> && !custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         std::underlying_type_t<std::decay_t<T>> x{};
         from<TOML, decltype(x)>::template op<Opts>(x, ctx, it, end);
         value = static_cast<std::decay_t<T>>(x);
      }
   };

   // ============================================
   // std::chrono deserialization
   // ============================================

   // Duration: parse from count
   template <is_duration T>
      requires(not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         using Rep = typename std::remove_cvref_t<T>::rep;
         Rep count{};
         from<TOML, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value = std::remove_cvref_t<T>(count);
      }
   };

   // system_clock::time_point: parse from TOML native datetime (RFC 3339)
   // TOML datetimes are NOT quoted - they're native values
   // Format: YYYY-MM-DDTHH:MM:SS[.fraction][Z|+HH:MM|-HH:MM]
   // Also accepts space instead of T, and seconds can be omitted
   template <is_system_time_point T>
      requires(not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It0, class It1>
      static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]]
            return;

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // Find the start of datetime and determine its length
         const auto start = it;

         // Helper to parse N digits starting at given pointer
         auto parse_digits = [](const auto* s, size_t count) -> int {
            int val = 0;
            for (size_t i = 0; i < count; ++i) {
               const char c = s[i];
               if (c < '0' || c > '9') return -1;
               val = val * 10 + (c - '0');
            }
            return val;
         };

         // Consume datetime characters (digits, -, :, T, space, ., Z, +)
         while (it != end) {
            const char c = *it;
            if ((c >= '0' && c <= '9') || c == '-' || c == ':' || c == 'T' || c == 't' || c == ' ' || c == '.' ||
                c == 'Z' || c == 'z' || c == '+') {
               ++it;
            }
            else {
               break;
            }
         }

         const auto n = static_cast<size_t>(it - start);

         // Minimum: YYYY-MM-DDTHH:MM = 16 chars (seconds optional in TOML)
         if (n < 16) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         const char* s = &(*start);

         // YYYY-MM-DDTHH:MM (T can be space)
         const int yr = parse_digits(s, 4);
         const int mo = parse_digits(s + 5, 2);
         const int dy = parse_digits(s + 8, 2);
         const int hr = parse_digits(s + 11, 2);
         const int mi = parse_digits(s + 14, 2);

         // Validate basic structure
         if (yr < 0 || mo < 0 || dy < 0 || hr < 0 || mi < 0 || s[4] != '-' || s[7] != '-' ||
             (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || s[13] != ':') [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         // Validate ranges
         if (mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr > 23 || mi > 59) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         // Parse optional seconds
         size_t pos = 16;
         int sc = 0;
         if (pos < n && s[pos] == ':') {
            ++pos;
            if (pos + 2 > n) [[unlikely]] {
               ctx.error = error_code::parse_error;
               return;
            }
            sc = parse_digits(s + pos, 2);
            if (sc < 0 || sc > 59) [[unlikely]] {
               ctx.error = error_code::parse_error;
               return;
            }
            pos += 2;
         }

         // Parse optional fractional seconds
         int64_t subsec_nanos = 0;
         if (pos < n && s[pos] == '.') {
            ++pos;
            int64_t frac = 0;
            int digits = 0;
            while (pos < n && s[pos] >= '0' && s[pos] <= '9') {
               if (digits < 9) {
                  frac = frac * 10 + (s[pos] - '0');
                  ++digits;
               }
               ++pos;
            }
            // Scale to nanoseconds
            static constexpr int64_t scale[] = {1000000000, 100000000, 10000000, 1000000, 100000,
                                                10000,      1000,      100,      10,      1};
            if (digits > 0 && digits <= 9) {
               subsec_nanos = frac * scale[digits];
            }
         }

         // Parse timezone: Z or +HH:MM or -HH:MM (defaults to UTC if missing - local datetime)
         int tz_offset_seconds = 0;
         if (pos < n) {
            if (s[pos] == 'Z' || s[pos] == 'z') {
               // UTC - no adjustment needed
            }
            else if (s[pos] == '+' || s[pos] == '-') {
               const int utc_adjustment = (s[pos] == '+') ? -1 : 1;
               ++pos;
               if (pos + 2 > n) [[unlikely]] {
                  ctx.error = error_code::parse_error;
                  return;
               }
               const int tz_hour = parse_digits(s + pos, 2);
               if (tz_hour < 0 || tz_hour > 23) [[unlikely]] {
                  ctx.error = error_code::parse_error;
                  return;
               }
               pos += 2;
               int tz_min = 0;
               if (pos < n && s[pos] == ':') ++pos;
               if (pos + 2 <= n) {
                  const int m = parse_digits(s + pos, 2);
                  if (m >= 0 && m <= 59) {
                     tz_min = m;
                  }
               }
               tz_offset_seconds = utc_adjustment * (tz_hour * 3600 + tz_min * 60);
            }
         }

         // Construct time_point using std::chrono calendar types
         using namespace std::chrono;

         const auto ymd = year_month_day{year{yr}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(dy)}};
         if (!ymd.ok()) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         const auto tp = sys_days{ymd} + hours{hr} + minutes{mi} + seconds{sc} + seconds{tz_offset_seconds} +
                         nanoseconds{subsec_nanos};

         using Duration = typename std::remove_cvref_t<T>::duration;
         value = time_point_cast<Duration>(tp);
      }
   };

   // year_month_day: parse from TOML Local Date (YYYY-MM-DD)
   template <is_year_month_day T>
      requires(not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It0, class It1>
      static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]]
            return;

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // Helper to parse N digits
         auto parse_digits = [](const auto* s, size_t count) -> int {
            int val = 0;
            for (size_t i = 0; i < count; ++i) {
               const char c = s[i];
               if (c < '0' || c > '9') return -1;
               val = val * 10 + (c - '0');
            }
            return val;
         };

         const auto start = it;

         // Consume date characters (digits and -)
         while (it != end) {
            const char c = *it;
            if ((c >= '0' && c <= '9') || c == '-') {
               ++it;
            }
            else {
               break;
            }
         }

         const auto n = static_cast<size_t>(it - start);

         // Minimum: YYYY-MM-DD = 10 chars
         if (n < 10) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         const char* s = &(*start);

         const int yr = parse_digits(s, 4);
         const int mo = parse_digits(s + 5, 2);
         const int dy = parse_digits(s + 8, 2);

         if (yr < 0 || mo < 0 || dy < 0 || s[4] != '-' || s[7] != '-') [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         if (mo < 1 || mo > 12 || dy < 1 || dy > 31) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         using namespace std::chrono;
         value = year_month_day{year{yr}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(dy)}};

         if (!value.ok()) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }
      }
   };

   // hh_mm_ss: parse from TOML Local Time (HH:MM:SS[.fraction])
   template <is_hh_mm_ss T>
      requires(not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It0, class It1>
      static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]]
            return;

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // Helper to parse N digits
         auto parse_digits = [](const auto* s, size_t count) -> int {
            int val = 0;
            for (size_t i = 0; i < count; ++i) {
               const char c = s[i];
               if (c < '0' || c > '9') return -1;
               val = val * 10 + (c - '0');
            }
            return val;
         };

         const auto start = it;

         // Consume time characters (digits, :, .)
         while (it != end) {
            const char c = *it;
            if ((c >= '0' && c <= '9') || c == ':' || c == '.') {
               ++it;
            }
            else {
               break;
            }
         }

         const auto n = static_cast<size_t>(it - start);

         // Minimum: HH:MM = 5 chars (seconds optional per TOML spec)
         if (n < 5) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         const char* s = &(*start);

         const int hr = parse_digits(s, 2);
         const int mi = parse_digits(s + 3, 2);

         if (hr < 0 || mi < 0 || s[2] != ':') [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         if (hr > 23 || mi > 59) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         // Parse optional seconds
         size_t pos = 5;
         int sc = 0;
         if (pos < n && s[pos] == ':') {
            ++pos;
            if (pos + 2 > n) [[unlikely]] {
               ctx.error = error_code::parse_error;
               return;
            }
            sc = parse_digits(s + pos, 2);
            if (sc < 0 || sc > 59) [[unlikely]] {
               ctx.error = error_code::parse_error;
               return;
            }
            pos += 2;
         }

         // Parse optional fractional seconds
         int64_t subsec_nanos = 0;
         if (pos < n && s[pos] == '.') {
            ++pos;
            int64_t frac = 0;
            int digits = 0;
            while (pos < n && s[pos] >= '0' && s[pos] <= '9') {
               if (digits < 9) {
                  frac = frac * 10 + (s[pos] - '0');
                  ++digits;
               }
               ++pos;
            }
            static constexpr int64_t scale[] = {1000000000, 100000000, 10000000, 1000000, 100000,
                                                10000,      1000,      100,      10,      1};
            if (digits > 0 && digits <= 9) {
               subsec_nanos = frac * scale[digits];
            }
         }

         using namespace std::chrono;
         using Precision = typename std::remove_cvref_t<T>::precision;

         auto total_duration =
            hours{hr} + minutes{mi} + seconds{sc} + duration_cast<Precision>(nanoseconds{subsec_nanos});
         value = std::remove_cvref_t<T>{total_duration};
      }
   };

   // steady_clock::time_point: parse as count in the time_point's native duration
   template <is_steady_time_point T>
      requires(not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         Rep count{};
         from<TOML, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value = std::remove_cvref_t<T>(Duration(count));
      }
   };

   // high_resolution_clock::time_point when it's a distinct type (rare)
   template <is_high_res_time_point T>
      requires(not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         // Treat like steady_clock - parse as count
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         Rep count{};
         from<TOML, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value = std::remove_cvref_t<T>(Duration(count));
      }
   };

   // for set types (emplaceable but not emplace_backable)
   template <class T>
      requires(readable_array_t<T> && !emplace_backable<T> && !resizable<T> && emplaceable<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         skip_ws_and_comments(it, end);

         if (it == end || *it != '[') {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it; // Skip '['
         skip_ws_and_comments(it, end);

         value.clear();

         // Handle empty array
         if (it != end && *it == ']') {
            ++it;
            return;
         }

         while (it != end) {
            using V = range_value_t<T>;
            V v;
            from<TOML, V>::template op<Opts>(v, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value.emplace(std::move(v));

            skip_ws_and_comments(it, end);

            if (it == end) {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if (*it == ']') {
               ++it;
               return;
            }
            else if (*it == ',') {
               ++it;
               skip_ws_and_comments(it, end);
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }
      }
   };

   // for types like std::vector, std::array, std::deque, etc.
   template <class T>
      requires(readable_array_t<T> && (emplace_backable<T> || !resizable<T>) && !emplaceable<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         skip_ws_and_comments(it, end);

         if (it == end || *it != '[') {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it; // Skip '['
         skip_ws_and_comments(it, end);

         // Handle empty array
         if (it != end && *it == ']') {
            ++it;
            return;
         }

         size_t index = 0;
         while (it != end) {
            using value_type = typename std::remove_cvref_t<T>::value_type;

            if constexpr (emplace_backable<T>) {
               auto& element = value.emplace_back();
               from<TOML, value_type>::template op<Opts>(element, ctx, it, end);
            }
            else {
               // For fixed-size arrays like std::array
               if (index >= value.size()) {
                  ctx.error = error_code::exceeded_static_array_size;
                  return;
               }
               from<TOML, value_type>::template op<Opts>(value[index], ctx, it, end);
               ++index;
            }

            if (bool(ctx.error)) {
               return;
            }

            skip_ws_and_comments(it, end);

            if (it == end) {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if (*it == ']') {
               ++it;
               break;
            }
            else if (*it == ',') {
               ++it;
               skip_ws_and_comments(it, end);
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }
      }
   };

   namespace detail
   {
      template <auto Opts, class T, class It, class End, class Ctx>
      GLZ_ALWAYS_INLINE void parse_toml_object_members(T&& value, It&& it, End end, Ctx&& ctx, bool is_inline_table)
      {
         using U = std::remove_cvref_t<T>;
         static constexpr auto N = reflect<U>::size;
         static constexpr auto HashInfo = hash_info<U>;
         // TODO: Think if we should make inline table a constexpr to reduce runtime checks
         // TODO: Think if it's feasible to write a function to find out the deepest nested structure
         // and use it instead of vector

         while (it != end) {
            skip_ws_and_comments(it, end);

            if (it == end) {
               if (is_inline_table) ctx.error = error_code::unexpected_end; // Inline table must end with '}'
               break;
            }

            if (is_inline_table && *it == '}') {
               ++it; // Consume '}'
               return; // End of inline table
            }

            // Skip empty lines (only if not in an inline table, inline tables don't have newlines)
            if (!is_inline_table && (*it == '\n' || *it == '\r')) {
               skip_to_next_line(ctx, it, end);
               continue;
            }

            std::string key_str;
            // std::vector<std::string> key_str; // TODO: std::string is used temporarily, we may swap it to view later
            // on or as said before completely switch to array

            // Handle section headers [section] (only if not in an inline table)
            if (!is_inline_table && *it == '[') {
               // For now, skip section headers - we'll implement nested object support later
               // Or, this could be where we handle table arrays or nested tables.
               // For the current task, we are focusing on inline tables.
               // This part might need to be more sophisticated for full TOML table support.
               skip_to_next_line(ctx, it, end);
               continue;
            }

            if (!parse_toml_key(key_str, ctx, it, end)) {
               return;
            }

            skip_ws_and_comments(it, end);

            if (it == end || *it != '=') {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it; // Skip '='
            skip_ws_and_comments(it, end);

            if (it == end) {
               ctx.error = error_code::unexpected_end; // Value expected
               return;
            }

            const auto index = decode_hash_with_size<TOML, U, HashInfo, HashInfo.type>::op(
               key_str.data(), key_str.data() + key_str.size(), key_str.size());

            const bool key_matches = index < N && key_str == reflect<U>::keys[index];

            if (key_matches) [[likely]] {
               visit<N>(
                  [&]<size_t I>() {
                     if (I == index) {
                        decltype(auto) member_obj = [&]() -> decltype(auto) {
                           if constexpr (reflectable<U>) {
                              // For reflectable types, to_tie provides access to members
                              return get<I>(to_tie(value));
                           }
                           else {
                              // For glaze_object_t, reflect<T>::values gives member metadata
                              // and get_member accesses the actual member value
                              return get_member(value, get<I>(reflect<U>::values));
                           }
                        }();

                        // member_obj is now a reference to the actual member or its wrapper
                        using member_type = std::decay_t<decltype(member_obj)>;
                        from<TOML, member_type>::template op<Opts>(member_obj, ctx, it, end);
                     }
                     return !bool(ctx.error);
                  },
                  index);

               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
            }
            else {
               if constexpr (Opts.error_on_unknown_keys) {
                  ctx.error = error_code::unknown_key;
                  return;
               }

               skip_value<TOML>::template op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
            }

            skip_ws_and_comments(it, end);
            if (it == end) {
               if (is_inline_table) ctx.error = error_code::unexpected_end; // Inline table must end with '}'
               break;
            }

            if (is_inline_table) {
               if (*it == '}') {
                  // Handled at the start of the loop
                  continue;
               }
               else if (*it == ',') {
                  ++it; // Consume comma
                  skip_ws_and_comments(it, end);
                  if (it != end && *it == '}') { // Trailing comma case like { key = "val", }
                     // This is allowed by TOML v1.0.0 for inline tables
                     // The '}' will be consumed at the start of the next iteration.
                  }
               }
               else {
                  ctx.error = error_code::syntax_error; // Expected comma or '}'
                  return;
               }
            }
            else {
               // For top-level or standard tables, expect newline or EOF
               if (it != end && (*it == '\n' || *it == '\r')) {
                  skip_to_next_line(ctx, it, end);
               }
               else if (it != end && *it != '#') { // If not a comment, it's an error unless it's EOF
                  // Could be an issue if there's no newline before EOF for the last key-value
               }
            }
         }
      }
   }

   template <auto Opts, class T>
   GLZ_ALWAYS_INLINE bool resolve_nested(T& root, std::span<std::string> path, auto& ctx, auto& it, auto& end)
   {
      if constexpr (!(glz::reflectable<T> || glz::glaze_object_t<T>)) {
         return true;
      }
      else {
         using U = std::remove_cvref_t<T>;
         static constexpr auto N = reflect<U>::size;
         static constexpr auto HashInfo = hash_info<U>;
         const auto index = decode_hash_with_size<TOML, U, HashInfo, HashInfo.type>::op(
            path.front().data(), path.front().data() + path.front().size(), path.front().size());

         const bool key_matches = index < N && path.front() == reflect<U>::keys[index];

         if (key_matches) [[likely]] {
            visit<N>(
               [&]<size_t I>() {
                  if (I == index) {
                     decltype(auto) member_obj = [&]() -> decltype(auto) {
                        if constexpr (reflectable<U>) {
                           return get<I>(to_tie(root));
                        }
                        else {
                           return get_member(root, get<I>(reflect<U>::values));
                        }
                     }();

                     if (!(path.size() - 1)) {
                        using member_type = std::decay_t<decltype(member_obj)>;
                        from<TOML, member_type>::template op<toml::is_internal_on<Opts>()>(member_obj, ctx, it, end);
                     }
                     else {
                        return resolve_nested<Opts>(member_obj, path.subspan(1), ctx, it, end);
                     }
                  }
                  return !bool(ctx.error);
               },
               index);
         }
         else {
            if constexpr (Opts.error_on_unknown_keys) {
               ctx.error = error_code::unknown_key;
               return false;
            }

            skip_value<TOML>::template op<Opts>(ctx, it, end);
            return !bool(ctx.error);
         }
         return !bool(ctx.error);
      }
   }

   // Helper to resolve an array-of-tables path and emplace a new element
   // Returns true if successful, false if error
   template <auto Opts, class T>
   GLZ_ALWAYS_INLINE bool resolve_array_of_tables(T& root, std::span<std::string> path, auto& ctx, auto& it, auto& end)
   {
      if constexpr (!(glz::reflectable<T> || glz::glaze_object_t<T>)) {
         ctx.error = error_code::syntax_error;
         return false;
      }
      else {
         using U = std::remove_cvref_t<T>;
         static constexpr auto N = reflect<U>::size;
         static constexpr auto HashInfo = hash_info<U>;
         const auto index = decode_hash_with_size<TOML, U, HashInfo, HashInfo.type>::op(
            path.front().data(), path.front().data() + path.front().size(), path.front().size());

         const bool key_matches = index < N && path.front() == reflect<U>::keys[index];

         if (key_matches) [[likely]] {
            bool success = false;
            visit<N>(
               [&]<size_t I>() {
                  if (I == index) {
                     decltype(auto) member_obj = [&]() -> decltype(auto) {
                        if constexpr (reflectable<U>) {
                           return get<I>(to_tie(root));
                        }
                        else {
                           return get_member(root, get<I>(reflect<U>::values));
                        }
                     }();

                     using member_type = std::decay_t<decltype(member_obj)>;

                     if (path.size() == 1) {
                        // This is the final element in the path - it should be an array
                        if constexpr (emplace_backable<member_type>) {
                           // Emplace a new element and parse into it
                           auto& new_element = member_obj.emplace_back();
                           using element_type = std::decay_t<decltype(new_element)>;
                           from<TOML, element_type>::template op<toml::is_internal_on<Opts>()>(new_element, ctx, it,
                                                                                               end);
                           success = !bool(ctx.error);
                        }
                        else {
                           // Not an array type that supports emplace_back
                           ctx.error = error_code::syntax_error;
                           success = false;
                        }
                     }
                     else {
                        // More path segments remaining - recurse
                        if constexpr (emplace_backable<member_type>) {
                           // If this is an array, navigate to the last element
                           if (member_obj.empty()) {
                              // Need to add an element first
                              member_obj.emplace_back();
                           }
                           success = resolve_array_of_tables<Opts>(member_obj.back(), path.subspan(1), ctx, it, end);
                        }
                        else if constexpr (glz::reflectable<member_type> || glz::glaze_object_t<member_type>) {
                           success = resolve_array_of_tables<Opts>(member_obj, path.subspan(1), ctx, it, end);
                        }
                        else {
                           ctx.error = error_code::syntax_error;
                           success = false;
                        }
                     }
                  }
               },
               index);
            return success;
         }
         else {
            if constexpr (Opts.error_on_unknown_keys) {
               ctx.error = error_code::unknown_key;
               return false;
            }

            // Skip unknown key's content
            skip_value<TOML>::template op<Opts>(ctx, it, end);
            return !bool(ctx.error);
         }
      }
   }

   // ============================================
   // Map support for TOML
   // ============================================

   namespace detail
   {
      // Concept to detect types with a variant 'data' member (like generic_json)
      template <class T>
      concept has_variant_data_member =
         requires { requires is_variant<std::remove_cvref_t<decltype(std::declval<T>().data)>>; };

      // Find the index of the first map alternative in a variant
      template <class Variant, size_t I = 0>
      constexpr size_t find_map_alternative_index()
      {
         if constexpr (I >= std::variant_size_v<Variant>) {
            return std::variant_npos;
         }
         else {
            using Alt = std::variant_alternative_t<I, Variant>;
            if constexpr (writable_map_t<Alt>) {
               return I;
            }
            else {
               return find_map_alternative_index<Variant, I + 1>();
            }
         }
      }

      // Helper to resolve a dotted key path into a nested map, creating entries as needed
      // Returns a reference to the innermost value where data should be stored
      // Note: Not GLZ_ALWAYS_INLINE because this function is recursive
      template <auto Opts, class T>
      bool resolve_nested_map(T& root, std::span<std::string> path, auto& ctx, auto& it, auto& end)
      {
         if (path.empty()) {
            ctx.error = error_code::syntax_error;
            return false;
         }

         using Value = typename T::mapped_type;

         if (path.size() == 1) {
            // Last key in path - parse the value directly
            from<TOML, Value>::template op<toml::is_internal_on<Opts>()>(root[path.front()], ctx, it, end);
            return !bool(ctx.error);
         }
         else {
            // Navigate/create nested map
            auto& nested_value = root[path.front()];

            // Handle different value types for nested navigation
            if constexpr (is_variant<Value>) {
               // Direct variant type - emplace the map alternative
               constexpr auto map_idx = find_map_alternative_index<Value>();
               if constexpr (map_idx != std::variant_npos) {
                  using MapType = std::variant_alternative_t<map_idx, Value>;
                  if (!std::holds_alternative<MapType>(nested_value)) {
                     nested_value.template emplace<map_idx>();
                  }
                  return resolve_nested_map<Opts>(std::get<MapType>(nested_value), path.subspan(1), ctx, it, end);
               }
               else {
                  ctx.error = error_code::no_matching_variant_type;
                  return false;
               }
            }
            else if constexpr (has_variant_data_member<Value>) {
               // Types like generic_json that wrap a variant in a 'data' member
               using DataType = decltype(nested_value.data);
               constexpr auto map_idx = find_map_alternative_index<std::remove_reference_t<DataType>>();
               if constexpr (map_idx != std::variant_npos) {
                  using MapType = std::variant_alternative_t<map_idx, std::remove_reference_t<DataType>>;
                  if (!std::holds_alternative<MapType>(nested_value.data)) {
                     nested_value.data.template emplace<map_idx>();
                  }
                  return resolve_nested_map<Opts>(std::get<MapType>(nested_value.data), path.subspan(1), ctx, it, end);
               }
               else {
                  ctx.error = error_code::no_matching_variant_type;
                  return false;
               }
            }
            else if constexpr (readable_map_t<Value>) {
               return resolve_nested_map<Opts>(nested_value, path.subspan(1), ctx, it, end);
            }
            else {
               // Value type doesn't support nesting
               ctx.error = error_code::syntax_error;
               return false;
            }
         }
      }

      // Helper to ensure a path of nested maps exists (for table section headers)
      // This creates the nested map structure without parsing a value
      // Note: Not GLZ_ALWAYS_INLINE because this function is recursive
      template <auto Opts, class T>
      bool ensure_map_path(T& root, std::span<std::string> path, auto& ctx)
      {
         if (path.empty()) {
            return true;
         }

         using Value = typename T::mapped_type;
         auto& nested_value = root[path.front()];

         if (path.size() == 1) {
            // Last key in path - just ensure the map exists at this location
            if constexpr (is_variant<Value>) {
               constexpr auto map_idx = find_map_alternative_index<Value>();
               if constexpr (map_idx != std::variant_npos) {
                  if (!std::holds_alternative<std::variant_alternative_t<map_idx, Value>>(nested_value)) {
                     nested_value.template emplace<map_idx>();
                  }
                  return true;
               }
               else {
                  ctx.error = error_code::no_matching_variant_type;
                  return false;
               }
            }
            else if constexpr (has_variant_data_member<Value>) {
               using DataType = std::remove_reference_t<decltype(nested_value.data)>;
               constexpr auto map_idx = find_map_alternative_index<DataType>();
               if constexpr (map_idx != std::variant_npos) {
                  if (!std::holds_alternative<std::variant_alternative_t<map_idx, DataType>>(nested_value.data)) {
                     nested_value.data.template emplace<map_idx>();
                  }
                  return true;
               }
               else {
                  ctx.error = error_code::no_matching_variant_type;
                  return false;
               }
            }
            else if constexpr (readable_map_t<Value>) {
               return true; // Already a map type, nothing to do
            }
            else {
               ctx.error = error_code::syntax_error;
               return false;
            }
         }
         else {
            // Navigate deeper
            if constexpr (is_variant<Value>) {
               constexpr auto map_idx = find_map_alternative_index<Value>();
               if constexpr (map_idx != std::variant_npos) {
                  using MapType = std::variant_alternative_t<map_idx, Value>;
                  if (!std::holds_alternative<MapType>(nested_value)) {
                     nested_value.template emplace<map_idx>();
                  }
                  return ensure_map_path<Opts>(std::get<MapType>(nested_value), path.subspan(1), ctx);
               }
               else {
                  ctx.error = error_code::no_matching_variant_type;
                  return false;
               }
            }
            else if constexpr (has_variant_data_member<Value>) {
               using DataType = std::remove_reference_t<decltype(nested_value.data)>;
               constexpr auto map_idx = find_map_alternative_index<DataType>();
               if constexpr (map_idx != std::variant_npos) {
                  using MapType = std::variant_alternative_t<map_idx, DataType>;
                  if (!std::holds_alternative<MapType>(nested_value.data)) {
                     nested_value.data.template emplace<map_idx>();
                  }
                  return ensure_map_path<Opts>(std::get<MapType>(nested_value.data), path.subspan(1), ctx);
               }
               else {
                  ctx.error = error_code::no_matching_variant_type;
                  return false;
               }
            }
            else if constexpr (readable_map_t<Value>) {
               return ensure_map_path<Opts>(nested_value, path.subspan(1), ctx);
            }
            else {
               ctx.error = error_code::syntax_error;
               return false;
            }
         }
      }

      // Parse inline table {...} into a map
      template <auto Opts, class T>
      GLZ_ALWAYS_INLINE void parse_toml_inline_table_map(T& value, auto& it, auto& end, auto& ctx)
      {
         // Already consumed the opening '{'
         skip_ws_and_comments(it, end);

         if (it != end && *it == '}') {
            ++it; // Empty inline table
            return;
         }

         while (it != end) {
            skip_ws_and_comments(it, end);
            if (it == end) {
               ctx.error = error_code::unexpected_end;
               return;
            }

            std::vector<std::string> path;
            if (!parse_toml_key(path, ctx, it, end)) {
               return;
            }

            if (it == end || *it != '=') {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it; // Skip '='
            skip_ws_and_comments(it, end);

            if (it == end) {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if (!resolve_nested_map<Opts>(value, std::span{path}, ctx, it, end)) {
               return;
            }

            skip_ws_and_comments(it, end);
            if (it == end) {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if (*it == '}') {
               ++it;
               return;
            }
            else if (*it == ',') {
               ++it;
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }
      }
   } // namespace detail

   template <class T>
      requires(readable_map_t<T> && not glaze_object_t<T> && not reflectable<T> && not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         skip_ws_and_comments(it, end);

         if (it == end) {
            return; // Empty input is valid (empty map)
         }

         // Check if starting with inline table
         if (*it == '{') {
            ++it;
            detail::parse_toml_inline_table_map<Opts>(value, it, end, ctx);
            return;
         }

         // TOML document-level parsing
         // Track current section path instead of a pointer to avoid UB from casting
         // between map types with different comparators (e.g., std::less<> vs std::less<std::string>)
         std::vector<std::string> current_section_path;

         while (it != end) {
            skip_ws_and_comments(it, end);
            if (it == end) {
               return;
            }
            if (*it == '\n' || *it == '\r') {
               skip_to_next_line(ctx, it, end);
               continue;
            }

            if (*it == '[') {
               if constexpr (toml::check_is_internal(Opts)) {
                  return; // if it's internal we return to root
               }

               ++it;
               if (it != end && *it == '[') {
                  // Array of tables [[name]] - not fully supported for maps yet
                  // For now, skip array of tables in map context
                  ctx.error = error_code::syntax_error;
                  return;
               }
               else {
                  // Table section [name]
                  skip_ws_and_comments(it, end);
                  current_section_path.clear();
                  if (!parse_toml_key(current_section_path, ctx, it, end)) {
                     return;
                  }
                  if (it == end || *it != ']') {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  ++it; // skip ']'

                  // Ensure the nested map structure exists for this section
                  if (!detail::ensure_map_path<Opts>(value, std::span{current_section_path}, ctx)) {
                     return;
                  }
               }
            }
            else {
               // Key-value pair
               std::vector<std::string> key_path;
               if (!parse_toml_key(key_path, ctx, it, end)) {
                  return;
               }

               if (it == end || *it != '=') {
                  ctx.error = error_code::syntax_error;
                  return;
               }
               ++it; // Skip '='
               skip_ws_and_comments(it, end);

               if (it == end) {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               // Combine section path with key path for full navigation from root
               std::vector<std::string> full_path;
               full_path.reserve(current_section_path.size() + key_path.size());
               full_path.insert(full_path.end(), current_section_path.begin(), current_section_path.end());
               full_path.insert(full_path.end(), key_path.begin(), key_path.end());

               if (!detail::resolve_nested_map<Opts>(value, std::span{full_path}, ctx, it, end)) {
                  return;
               }
            }
         }
      }
   };

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_read<T>)
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         while (it != end) {
            // TODO: Introduce OPTS here
            skip_ws_and_comments(it, end);
            if (it == end) { // Empty input for an object is valid (empty object)
               return;
            }
            if (*it == '\n' || *it == '\r') {
               skip_to_next_line(ctx, it, end);
               continue;
            }

            // TODO: We probably should reorder that so that we dont check against less used inline table more often
            if (*it == '{') { // Check if it's an inline table
               ++it; // Consume '{'
               skip_ws_and_comments(it, end);
               if (it != end && *it == '}') { // Empty inline table {}
                  ++it;
                  return;
               }
               // TODO: Rewrite logic here, for now it works just fine so we leave it.
               detail::parse_toml_object_members<Opts>(value, it, end, ctx, true); // true for is_inline_table
               // The parse_toml_object_members should consume the final '}' if successful
            }
            else if (*it == '[') { // Normal table or array of tables
               std::vector<std::string> path;

               if constexpr (toml::check_is_internal(Opts)) {
                  return; // if it's internal we return to root.
               }
               else {
                  ++it;
                  if (it != end && *it == '[') { // Array of tables [[name]]
                     ++it; // skip the second bracket
                     skip_ws_and_comments(it, end);

                     if (!parse_toml_key(path, ctx, it, end)) {
                        return;
                     }

                     // Expect closing ]]
                     skip_ws_and_comments(it, end);
                     if (it == end || *it != ']') {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                     ++it; // skip first ']'
                     if (it == end || *it != ']') {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                     ++it; // skip second ']'

                     // Navigate to the array and emplace a new element
                     if (!resolve_array_of_tables<Opts>(value, std::span{path}, ctx, it, end)) {
                        return;
                     }
                  }
                  else {
                     // Normal table [name]
                     skip_ws_and_comments(it, end);

                     if (!parse_toml_key(path, ctx, it, end)) {
                        return;
                     }
                     if (it == end || *it != ']') {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                     ++it; // skip ']'
                     if (!resolve_nested<Opts>(value, std::span{path}, ctx, it, end)) {
                        return;
                     }
                  }
               }
            }
            else {
               std::vector<std::string> path;
               skip_ws_and_comments(it, end);

               if (!parse_toml_key(path, ctx, it, end)) {
                  return;
               }

               if (it == end || *it != '=') {
                  ctx.error = error_code::syntax_error;
                  return;
               }

               ++it; // Skip '='
               skip_ws_and_comments(it, end);

               if (it == end) {
                  ctx.error = error_code::unexpected_end; // Value expected
                  return;
               }
               if (!resolve_nested<Opts>(value, std::span{path}, ctx, it, end)) {
                  return;
               }
            }
         }
      }
   };

   // ============================================
   // Variant support for TOML
   // ============================================

   namespace detail
   {
      // Type traits for TOML variant type classification
      template <class T>
      concept toml_variant_bool_type = glz::bool_t<T>;

      template <class T>
      concept toml_variant_string_type = glz::str_t<T>;

      template <class T>
      concept toml_variant_int_type = glz::int_t<T> && !glz::bool_t<T>;

      template <class T>
      concept toml_variant_signed_int_type = std::signed_integral<T> && !glz::bool_t<T>;

      template <class T>
      concept toml_variant_unsigned_int_type = std::unsigned_integral<T> && !glz::bool_t<T>;

      template <class T>
      concept toml_variant_float_type = std::floating_point<T>;

      template <class T>
      concept toml_variant_array_type = glz::readable_array_t<T> && !glz::str_t<T>;

      template <class T>
      concept toml_variant_object_type = (glz::glaze_object_t<T> || glz::reflectable<T> || glz::writable_map_t<T>);

      template <class T>
      concept toml_variant_null_type = glz::null_t<T>;

      // Type trait wrappers for template use
      template <class T>
      struct is_toml_variant_bool : std::bool_constant<toml_variant_bool_type<T>>
      {};
      template <class T>
      struct is_toml_variant_string : std::bool_constant<toml_variant_string_type<T>>
      {};
      template <class T>
      struct is_toml_variant_int : std::bool_constant<toml_variant_int_type<T>>
      {};
      template <class T>
      struct is_toml_variant_signed_int : std::bool_constant<toml_variant_signed_int_type<T>>
      {};
      template <class T>
      struct is_toml_variant_unsigned_int : std::bool_constant<toml_variant_unsigned_int_type<T>>
      {};
      template <class T>
      struct is_toml_variant_float : std::bool_constant<toml_variant_float_type<T>>
      {};
      template <class T>
      struct is_toml_variant_array : std::bool_constant<toml_variant_array_type<T>>
      {};
      template <class T>
      struct is_toml_variant_object : std::bool_constant<toml_variant_object_type<T>>
      {};
      template <class T>
      struct is_toml_variant_null : std::bool_constant<toml_variant_null_type<T>>
      {};

      // Count types in variant matching a trait
      template <class Variant, template <class> class Trait>
      struct toml_variant_count_impl;

      template <template <class> class Trait, class... Ts>
      struct toml_variant_count_impl<std::variant<Ts...>, Trait>
      {
         static constexpr size_t value = (size_t(Trait<Ts>::value) + ... + 0);
      };

      template <class Variant, template <class> class Trait>
      constexpr size_t toml_variant_count_v = toml_variant_count_impl<Variant, Trait>::value;

      // Get first index matching trait (or variant_npos if none)
      template <class Variant, template <class> class Trait>
      struct toml_variant_first_index_impl;

      template <template <class> class Trait, class... Ts>
      struct toml_variant_first_index_impl<std::variant<Ts...>, Trait>
      {
         static constexpr size_t find()
         {
            size_t result = std::variant_npos;
            size_t idx = 0;
            ((Trait<Ts>::value && result == std::variant_npos ? (result = idx, ++idx) : ++idx), ...);
            return result;
         }
         static constexpr size_t value = find();
      };

      template <class Variant, template <class> class Trait>
      constexpr size_t toml_variant_first_index_v = toml_variant_first_index_impl<Variant, Trait>::value;

      // Detect if a TOML number is a float by scanning ahead
      // Returns true if the number contains float indicators (., e, E, inf, nan)
      // Returns false for hex (0x), octal (0o), and binary (0b) prefixed numbers
      template <class It, class End>
      inline bool is_toml_float(It it, End end) noexcept
      {
         // Skip leading sign
         if (it != end && (*it == '+' || *it == '-')) {
            ++it;
         }

         // Check for special float values
         if (it != end) {
            // Check for inf/nan (with optional sign already consumed)
            if (*it == 'i' || *it == 'n') {
               return true; // inf or nan
            }
            // Check for hex (0x), octal (0o), binary (0b) prefixes - these are always integers
            if (*it == '0') {
               auto next = it;
               ++next;
               if (next != end) {
                  const char prefix = *next;
                  if (prefix == 'x' || prefix == 'X' || prefix == 'o' || prefix == 'O' || prefix == 'b' ||
                      prefix == 'B') {
                     return false; // Always an integer
                  }
               }
            }
         }

         // Scan the number looking for float indicators
         while (it != end) {
            const char c = *it;
            if (c == '.' || c == 'e' || c == 'E') {
               return true;
            }
            // Stop at value terminators
            if (c == ',' || c == ']' || c == '}' || c == '\n' || c == '\r' || c == '#' || c == ' ' || c == '\t') {
               break;
            }
            ++it;
         }
         return false;
      }

      // Check if we're looking at 'true' or 'false' exactly (not an identifier starting with 't' or 'f')
      template <class It, class End>
      inline bool is_toml_bool(It it, End end) noexcept
      {
         if (it == end) return false;

         const char c = *it;
         if (c == 't') {
            // Check for "true"
            ++it;
            if (it == end || *it != 'r') return false;
            ++it;
            if (it == end || *it != 'u') return false;
            ++it;
            if (it == end || *it != 'e') return false;
            ++it;
            // Must be followed by a terminator
            if (it == end) return true;
            const char next = *it;
            return next == ',' || next == ']' || next == '}' || next == '\n' || next == '\r' || next == '#' ||
                   next == ' ' || next == '\t';
         }
         else if (c == 'f') {
            // Check for "false"
            ++it;
            if (it == end || *it != 'a') return false;
            ++it;
            if (it == end || *it != 'l') return false;
            ++it;
            if (it == end || *it != 's') return false;
            ++it;
            if (it == end || *it != 'e') return false;
            ++it;
            // Must be followed by a terminator
            if (it == end) return true;
            const char next = *it;
            return next == ',' || next == ']' || next == '}' || next == '\n' || next == '\r' || next == '#' ||
                   next == ' ' || next == '\t';
         }
         return false;
      }

      // Check if we're looking at 'inf' or 'nan' specifically (not an identifier starting with 'i' or 'n')
      template <class It, class End>
      inline bool is_inf_or_nan(It it, End end) noexcept
      {
         // Skip optional sign
         if (it != end && (*it == '+' || *it == '-')) {
            ++it;
         }
         if (it == end) return false;

         const char c = *it;
         if (c == 'i') {
            // Check for "inf"
            ++it;
            if (it != end && *it == 'n') {
               ++it;
               if (it != end && *it == 'f') {
                  ++it;
                  // Must be followed by a terminator
                  if (it == end) return true;
                  const char next = *it;
                  return next == ',' || next == ']' || next == '}' || next == '\n' || next == '\r' || next == '#' ||
                         next == ' ' || next == '\t';
               }
            }
            return false;
         }
         else if (c == 'n') {
            // Check for "nan"
            ++it;
            if (it != end && *it == 'a') {
               ++it;
               if (it != end && *it == 'n') {
                  ++it;
                  // Must be followed by a terminator
                  if (it == end) return true;
                  const char next = *it;
                  return next == ',' || next == ']' || next == '}' || next == '\n' || next == '\r' || next == '#' ||
                         next == ' ' || next == '\t';
               }
            }
            return false;
         }
         return false;
      }
   } // namespace detail

   template <class T>
      requires is_variant<std::remove_cvref_t<T>>
   struct from<TOML, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         using Variant = std::remove_cvref_t<T>;

         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         skip_ws_and_comments(it, end);

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         const char c = *it;

         // Detect type based on first character
         if (c == '"' || c == '\'') {
            // String
            constexpr auto str_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_string>;
            if constexpr (str_count == 0) {
               ctx.error = error_code::no_matching_variant_type;
               return;
            }
            else {
               constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_string>;
               using V = std::variant_alternative_t<idx, Variant>;
               if (!std::holds_alternative<V>(value)) {
                  value.template emplace<idx>();
               }
               from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
            }
         }
         else if (c == '[') {
            // Array
            constexpr auto arr_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_array>;
            if constexpr (arr_count == 0) {
               ctx.error = error_code::no_matching_variant_type;
               return;
            }
            else {
               constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_array>;
               using V = std::variant_alternative_t<idx, Variant>;
               if (!std::holds_alternative<V>(value)) {
                  value.template emplace<idx>();
               }
               from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
            }
         }
         else if (c == '{') {
            // Inline table (object)
            constexpr auto obj_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_object>;
            if constexpr (obj_count == 0) {
               ctx.error = error_code::no_matching_variant_type;
               return;
            }
            else {
               constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_object>;
               using V = std::variant_alternative_t<idx, Variant>;
               if (!std::holds_alternative<V>(value)) {
                  value.template emplace<idx>();
               }
               from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
            }
         }
         else if ((c == 't' || c == 'f') && detail::is_toml_bool(it, end)) {
            // Boolean (must be exactly 'true' or 'false', not identifiers starting with 't' or 'f')
            constexpr auto bool_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_bool>;
            if constexpr (bool_count == 0) {
               ctx.error = error_code::no_matching_variant_type;
               return;
            }
            else {
               constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_bool>;
               using V = std::variant_alternative_t<idx, Variant>;
               if (!std::holds_alternative<V>(value)) {
                  value.template emplace<idx>();
               }
               from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
            }
         }
         else if ((c >= '0' && c <= '9') || c == '+' || c == '-' ||
                  ((c == 'i' || c == 'n') && detail::is_inf_or_nan(it, end))) {
            // Number (including inf/nan) - need to determine if integer or float
            constexpr auto int_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_int>;
            constexpr auto float_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_float>;
            constexpr auto signed_int_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_signed_int>;
            constexpr auto unsigned_int_count =
               detail::toml_variant_count_v<Variant, detail::is_toml_variant_unsigned_int>;

            if constexpr (int_count == 0 && float_count == 0) {
               ctx.error = error_code::no_matching_variant_type;
               return;
            }
            else if constexpr (int_count == 0) {
               // Only float types available
               constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_float>;
               using V = std::variant_alternative_t<idx, Variant>;
               if (!std::holds_alternative<V>(value)) {
                  value.template emplace<idx>();
               }
               from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
            }
            else if constexpr (float_count == 0) {
               // Only integer types available - choose signed vs unsigned based on sign
               const bool is_negative = (c == '-');
               if constexpr (signed_int_count > 0 && unsigned_int_count > 0) {
                  // Both signed and unsigned available - pick based on sign
                  if (is_negative) {
                     constexpr auto idx =
                        detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_signed_int>;
                     using V = std::variant_alternative_t<idx, Variant>;
                     if (!std::holds_alternative<V>(value)) {
                        value.template emplace<idx>();
                     }
                     from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
                  }
                  else {
                     constexpr auto idx =
                        detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_unsigned_int>;
                     using V = std::variant_alternative_t<idx, Variant>;
                     if (!std::holds_alternative<V>(value)) {
                        value.template emplace<idx>();
                     }
                     from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
                  }
               }
               else {
                  // Only one integer signedness available
                  constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_int>;
                  using V = std::variant_alternative_t<idx, Variant>;
                  if (!std::holds_alternative<V>(value)) {
                     value.template emplace<idx>();
                  }
                  from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
               }
            }
            else {
               // Both int and float types available - need to detect which
               if (detail::is_toml_float(it, end)) {
                  constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_float>;
                  using V = std::variant_alternative_t<idx, Variant>;
                  if (!std::holds_alternative<V>(value)) {
                     value.template emplace<idx>();
                  }
                  from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
               }
               else {
                  // Integer - choose signed vs unsigned based on sign
                  const bool is_negative = (c == '-');
                  if constexpr (signed_int_count > 0 && unsigned_int_count > 0) {
                     // Both signed and unsigned available - pick based on sign
                     if (is_negative) {
                        constexpr auto idx =
                           detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_signed_int>;
                        using V = std::variant_alternative_t<idx, Variant>;
                        if (!std::holds_alternative<V>(value)) {
                           value.template emplace<idx>();
                        }
                        from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
                     }
                     else {
                        constexpr auto idx =
                           detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_unsigned_int>;
                        using V = std::variant_alternative_t<idx, Variant>;
                        if (!std::holds_alternative<V>(value)) {
                           value.template emplace<idx>();
                        }
                        from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
                     }
                  }
                  else {
                     // Only one integer signedness available
                     constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_int>;
                     using V = std::variant_alternative_t<idx, Variant>;
                     if (!std::holds_alternative<V>(value)) {
                        value.template emplace<idx>();
                     }
                     from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
                  }
               }
            }
         }
         else {
            // Identifiers (a-z, A-Z, _) at document root indicate key-value pairs
            // Check if the variant has a map alternative we can use
            const bool is_identifier = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            constexpr auto map_idx = detail::find_map_alternative_index<Variant>();

            if constexpr (map_idx != std::variant_npos) {
               if (is_identifier) {
                  // Parse as a TOML document with key-value pairs into the map alternative
                  using V = std::variant_alternative_t<map_idx, Variant>;
                  if (!std::holds_alternative<V>(value)) {
                     value.template emplace<map_idx>();
                  }
                  from<TOML, V>::template op<Opts>(std::get<V>(value), ctx, it, end);
                  return;
               }
            }

            // Unknown type - could be null or invalid
            constexpr auto null_count = detail::toml_variant_count_v<Variant, detail::is_toml_variant_null>;
            if constexpr (null_count > 0) {
               // Try to parse as null (for std::nullptr_t or similar)
               constexpr auto idx = detail::toml_variant_first_index_v<Variant, detail::is_toml_variant_null>;
               value.template emplace<idx>();
               // Note: TOML doesn't have a native null, so this may fail unless
               // custom handling is provided
            }
            else {
               ctx.error = error_code::syntax_error;
            }
         }
      }
   };

   template <read_supported<TOML> T, is_buffer Buffer>
   [[nodiscard]] inline error_ctx read_toml(T& value, Buffer&& buffer)
   {
      context ctx{};
      return read<opts{.format = TOML}>(value, std::forward<Buffer>(buffer), ctx);
   }

   template <read_supported<TOML> T, is_buffer Buffer>
   [[nodiscard]] inline expected<T, error_ctx> read_toml(Buffer&& buffer)
   {
      T value{};
      context ctx{};
      const error_ctx ec = read<opts{.format = TOML}>(value, std::forward<Buffer>(buffer), ctx);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return value;
   }

   template <read_supported<TOML> T, is_buffer Buffer>
   [[nodiscard]] inline error_ctx read_file_toml(T& value, const sv file_name, Buffer&& buffer)
   {
      context ctx{};
      ctx.current_file = file_name;

      const auto ec = file_to_buffer(buffer, ctx.current_file);

      if (bool(ec)) {
         return {0, ec};
      }

      return read<opts{.format = TOML}>(value, buffer, ctx);
   }
}
