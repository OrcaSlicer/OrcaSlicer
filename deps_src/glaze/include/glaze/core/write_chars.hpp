// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <charconv>
#include <type_traits>

// Detection: is std::to_chars for floating-point available?
// - Non-libc++ implementations (libstdc++, MSVC): always available
// - libc++: only when _LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT is 1
// This is needed because std::to_chars for floats is unavailable on older Apple platforms
// (iOS < 16.3, macOS < 13.3). The macro is always defined in libc++, but set to 0 or 1.
// Users can define GLZ_USE_STD_FORMAT_FLOAT before including glaze to override detection
#ifndef GLZ_USE_STD_FORMAT_FLOAT
#if !defined(_LIBCPP_VERSION) || _LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT
#define GLZ_USE_STD_FORMAT_FLOAT 1
#else
#define GLZ_USE_STD_FORMAT_FLOAT 0
#endif
#endif

#if GLZ_USE_STD_FORMAT_FLOAT
#include <format>
#else
#include <cstdio>
#endif

#include "glaze/concepts/container_concepts.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/util/dtoa.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/itoa.hpp"
#include "glaze/util/itoa_40kb.hpp"
#include "glaze/util/simple_float.hpp"

namespace glz
{
   namespace detail
   {
      // Result type for compile-time format string conversion (std::format -> printf)
      template <size_t N>
      struct printf_fmt_t
      {
         char data[N + 4]{}; // Extra space for '%' and safety margin
         size_t len = 0;
      };

      // Converts std::format float spec to printf format at compile time
      // Only supports JSON-relevant formatting: precision and type specifier
      // Examples: "{:.2f}" -> "%.2f", "{:.6g}" -> "%.6g", "{}" -> "%g"
      template <size_t N>
      consteval auto to_printf_fmt(const char (&fmt)[N]) -> printf_fmt_t<N>
      {
         printf_fmt_t<N> result;
         size_t i = 0;

         // Skip opening brace
         if (i < N && fmt[i] == '{') ++i;

         // Check for ':' indicating format spec
         const bool has_spec = (i < N && fmt[i] == ':');
         if (has_spec) ++i;

         // Start printf format with '%'
         result.data[result.len++] = '%';

         // No spec or empty spec -> default to %g
         if (!has_spec || (i < N && fmt[i] == '}')) {
            result.data[result.len++] = 'g';
            result.data[result.len] = '\0';
            return result;
         }

         // Skip to precision (.) or type specifier
         while (i < N && fmt[i] != '.' && fmt[i] != '}' && fmt[i] != '\0') {
            // Check if this is a type specifier
            if (fmt[i] == 'f' || fmt[i] == 'F' || fmt[i] == 'e' || fmt[i] == 'E' || fmt[i] == 'g' || fmt[i] == 'G') {
               break;
            }
            ++i;
         }

         // Precision: .digits
         if (i < N && fmt[i] == '.') {
            result.data[result.len++] = '.';
            ++i;
            while (i < N && fmt[i] >= '0' && fmt[i] <= '9') {
               result.data[result.len++] = fmt[i++];
            }
         }

         // Type: e, E, f, F, g, G
         if (i < N && fmt[i] != '}' && fmt[i] != '\0') {
            const char type = fmt[i];
            if (type == 'e' || type == 'E' || type == 'f' || type == 'F' || type == 'g' || type == 'G') {
               result.data[result.len++] = type;
            }
            else {
               result.data[result.len++] = 'g'; // default
            }
         }
         else {
            result.data[result.len++] = 'g'; // default type
         }

         result.data[result.len] = '\0';
         return result;
      }

      // Overload for std::string_view (used when float_format is a string_view in opts)
      consteval auto to_printf_fmt(std::string_view fmt) -> printf_fmt_t<32>
      {
         printf_fmt_t<32> result;
         size_t i = 0;
         const size_t N = fmt.size();

         // Skip opening brace
         if (i < N && fmt[i] == '{') ++i;

         // Check for ':' indicating format spec
         const bool has_spec = (i < N && fmt[i] == ':');
         if (has_spec) ++i;

         // Start printf format with '%'
         result.data[result.len++] = '%';

         // No spec or empty spec -> default to %g
         if (!has_spec || (i < N && fmt[i] == '}')) {
            result.data[result.len++] = 'g';
            result.data[result.len] = '\0';
            return result;
         }

         // Skip to precision (.) or type specifier
         while (i < N && fmt[i] != '.' && fmt[i] != '}' && fmt[i] != '\0') {
            // Check if this is a type specifier
            if (fmt[i] == 'f' || fmt[i] == 'F' || fmt[i] == 'e' || fmt[i] == 'E' || fmt[i] == 'g' || fmt[i] == 'G') {
               break;
            }
            ++i;
         }

         // Precision: .digits
         if (i < N && fmt[i] == '.') {
            result.data[result.len++] = '.';
            ++i;
            while (i < N && fmt[i] >= '0' && fmt[i] <= '9') {
               result.data[result.len++] = fmt[i++];
            }
         }

         // Type: e, E, f, F, g, G
         if (i < N && fmt[i] != '}' && fmt[i] != '\0') {
            const char type = fmt[i];
            if (type == 'e' || type == 'E' || type == 'f' || type == 'F' || type == 'g' || type == 'G') {
               result.data[result.len++] = type;
            }
            else {
               result.data[result.len++] = 'g'; // default
            }
         }
         else {
            result.data[result.len++] = 'g'; // default type
         }

         result.data[result.len] = '\0';
         return result;
      }
   }

   template <class T>
   GLZ_ALWAYS_INLINE constexpr auto sized_integer_conversion() noexcept
   {
      if constexpr (std::is_signed_v<T>) {
         if constexpr (sizeof(T) <= sizeof(int32_t)) {
            return int32_t{};
         }
         else if constexpr (sizeof(T) <= sizeof(int64_t)) {
            return int64_t{};
         }
         else {
            static_assert(false_v<T>, "type is not supported");
         }
      }
      else {
         if constexpr (sizeof(T) <= sizeof(uint32_t)) {
            return uint32_t{};
         }
         else if constexpr (sizeof(T) <= sizeof(uint64_t)) {
            return uint64_t{};
         }
         else {
            static_assert(false_v<T>, "type is not supported");
         }
      }
   }
   static_assert(std::is_same_v<decltype(sized_integer_conversion<long long>()), int64_t>);
   static_assert(std::is_same_v<decltype(sized_integer_conversion<unsigned long long>()), uint64_t>);

   struct write_chars
   {
      template <auto Opts, class B>
      inline static void op(num_t auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         /*if constexpr (std::same_as<std::decay_t<B>, std::string>) {
            // more efficient strings in C++23:
          https://en.cppreference.com/w/cpp/string/basic_string/resize_and_overwrite
          }*/

         // https://stackoverflow.com/questions/1701055/what-is-the-maximum-length-in-chars-needed-to-represent-any-double-value
         // maximum length for a double should be 24 chars, we use 64 to be sufficient for float128_t
         if constexpr (resizable<B> && not check_write_unchecked(Opts)) {
            if (const auto k = ix + 64; k > b.size()) {
               b.resize(2 * k);
            }
         }

         using V = std::decay_t<decltype(value)>;

         if constexpr (std::floating_point<V>) {
            using opts_type = std::decay_t<decltype(Opts)>;
            constexpr bool use_float_format = requires { opts_type::float_format; };

            // User-specified float_format takes priority over all other options
            if constexpr (use_float_format && is_any_of<V, float, double>) {
#if GLZ_USE_STD_FORMAT_FLOAT
               // Use std::format with user-provided format string (e.g., "{:.2f}")
               // Format string is validated at compile-time via std::format_string
               constexpr auto fmt = std::format_string<V>{opts_type::float_format};

               if constexpr (check_write_unchecked(Opts)) {
                  // Caller guarantees buffer has enough space
                  const auto start = reinterpret_cast<char*>(&b[ix]);
                  auto result = std::format_to(start, fmt, V(value));
                  ix += size_t(result - start);
               }
               else {
                  // format_to_n writes up to 'available' chars and returns total size needed.
                  // Common case: output fits in pre-allocated buffer (single pass).
                  // Rare case: output exceeds buffer (e.g., "{:.100f}"), requires resize.
                  const auto start = reinterpret_cast<char*>(&b[ix]);
                  const auto available = b.size() - ix;
                  auto [out, size] = std::format_to_n(start, available, fmt, V(value));

                  if (static_cast<size_t>(size) > available) {
                     // Output was truncated - size tells us exactly how much space we need
                     if constexpr (resizable<B>) {
                        b.resize(2 * (ix + size));
                        std::format_to(reinterpret_cast<char*>(&b[ix]), fmt, V(value));
                     }
                     else {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                  }
                  ix += size;
               }
#else
               // snprintf fallback for platforms without std::format float support (e.g., iOS < 16.3)
               // Convert std::format spec to printf format at compile time
               constexpr auto printf_fmt = detail::to_printf_fmt(opts_type::float_format);

               const auto start = reinterpret_cast<char*>(&b[ix]);
               const auto available = check_write_unchecked(Opts) ? size_t(64) : (b.size() - ix);

               // snprintf returns chars that would be written (excluding null), or negative on error
               const int len = std::snprintf(start, available, printf_fmt.data, static_cast<double>(value));

               if (len < 0) {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               if (static_cast<size_t>(len) >= available) {
                  // Output was truncated, need to resize and retry
                  if constexpr (resizable<B> && not check_write_unchecked(Opts)) {
                     b.resize(2 * (ix + static_cast<size_t>(len) + 1));
                     std::snprintf(reinterpret_cast<char*>(&b[ix]), static_cast<size_t>(len) + 1, printf_fmt.data,
                                   static_cast<double>(value));
                  }
                  else {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               ix += static_cast<size_t>(len);
#endif
            }
            else if constexpr (uint8_t(check_float_max_write_precision(Opts)) > 0 &&
                               uint8_t(check_float_max_write_precision(Opts)) < sizeof(V)) {
               // we cast to a lower precision floating point value before writing out
               if constexpr (uint8_t(check_float_max_write_precision(Opts)) == 8) {
                  const auto reduced = static_cast<double>(value);
                  const auto start = reinterpret_cast<char*>(&b[ix]);
                  const auto end = glz::to_chars(start, reduced);
                  ix += size_t(end - start);
               }
               else if constexpr (uint8_t(check_float_max_write_precision(Opts)) == 4) {
                  const auto reduced = static_cast<float>(value);
                  const auto start = reinterpret_cast<char*>(&b[ix]);
                  const auto end = glz::to_chars(start, reduced);
                  ix += size_t(end - start);
               }
               else {
                  static_assert(false_v<V>, "invalid float_max_write_precision");
               }
            }
            // Size optimization: use simple_float::to_chars to avoid dragonbox lookup tables (~20KB)
            // This works on all platforms including bare-metal
            else if constexpr (is_size_optimized(Opts) && is_any_of<V, float, double>) {
               const auto start = reinterpret_cast<char*>(&b[ix]);
               const auto end = simple_float::to_chars(start, V(value));
               ix += size_t(end - start);
            }
            else if constexpr (is_any_of<V, float, double>) {
               const auto start = reinterpret_cast<char*>(&b[ix]);
               const auto end = glz::to_chars(start, value);
               ix += size_t(end - start);
            }
// float128_t requires std::to_chars for floating-point, unavailable on older Apple platforms (iOS < 16.3)
#if !defined(_LIBCPP_VERSION) || _LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT
            else if constexpr (is_float128<V>) {
               const auto start = reinterpret_cast<char*>(&b[ix]);
               const auto [ptr, ec] = std::to_chars(start, &b[0] + b.size(), value, std::chars_format::general);
               if (ec != std::errc()) {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               ix += size_t(ptr - start);
            }
#endif
            else {
               static_assert(false_v<V>, "type is not supported");
            }
         }
         else if constexpr (is_any_of<V, int8_t, uint8_t, int16_t, uint16_t>) {
            // Small integers: always use to_chars (400B tables)
            // The 40KB digit_quads table doesn't help for these small ranges
            const auto start = reinterpret_cast<char*>(&b[ix]);
            const auto end = glz::to_chars(start, value);
            ix += size_t(end - start);
         }
         else if constexpr (is_any_of<V, int32_t, uint32_t, int64_t, uint64_t>) {
            const auto start = reinterpret_cast<char*>(&b[ix]);
            if constexpr (is_size_optimized(Opts)) {
               // Size mode: use glz::to_chars (400B lookup tables)
               const auto end = glz::to_chars(start, value);
               ix += size_t(end - start);
            }
            else {
               // Normal mode: use to_chars_40kb (40KB digit_quads table)
               const auto end = glz::to_chars_40kb(start, value);
               ix += size_t(end - start);
            }
         }
         else if constexpr (std::integral<V>) {
            // Handle other integral types (char, wchar_t, etc.) by casting to sized types
            using X = std::decay_t<decltype(sized_integer_conversion<V>())>;
            const auto start = reinterpret_cast<char*>(&b[ix]);
            if constexpr (is_size_optimized(Opts)) {
               // Size mode: use glz::to_chars (400B lookup tables)
               const auto end = glz::to_chars(start, static_cast<X>(value));
               ix += size_t(end - start);
            }
            else {
               // Normal mode: use to_chars_40kb
               const auto end = glz::to_chars_40kb(start, static_cast<X>(value));
               ix += size_t(end - start);
            }
         }
         else {
            static_assert(false_v<V>, "type is not supported");
         }
      }
   };
}
