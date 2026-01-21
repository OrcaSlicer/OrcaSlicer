// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/buffer_traits.hpp"
#include "glaze/core/chrono.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/to.hpp"
#include "glaze/core/wrappers.hpp"
#include "glaze/core/write.hpp"
#include "glaze/core/write_chars.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/itoa.hpp"
#include "glaze/util/parse.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   // TOML-specific options - inherits from opts to allow TOML-specific configuration
   // without modifying the base opts struct
   struct toml_opts : opts
   {
      bool inline_arrays = false; // Use inline [{...}, {...}] instead of [[array]] syntax

      constexpr toml_opts() : opts{.format = TOML} {}
      constexpr explicit toml_opts(bool inline_arr) : opts{.format = TOML}, inline_arrays(inline_arr) {}
   };

   template <>
   struct serialize<TOML>
   {
      template <auto Opts, class T, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<TOML, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                             std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   template <class T>
      requires(glaze_value_t<T> && !custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class Value, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         to<TOML, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                        std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   template <nullable_like T>
   struct to<TOML, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (value) {
            serialize<TOML>::op<Opts>(*value, ctx, b, ix);
         }
      }
   };

   // std::nullptr_t support for variants containing null types
   // TOML doesn't have a native null type, so we write an empty string
   // This is primarily used when serializing variants like glz::generic_json
   template <>
   struct to<TOML, std::nullptr_t>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(std::nullptr_t, is_context auto&& ctx, B&& b, auto& ix)
      {
         // Write empty string as placeholder for null
         // Note: TOML doesn't support null natively
         if (!ensure_space(ctx, b, ix + 2 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump("\"\"", b, ix);
      }
   };

   template <boolean_like T>
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(const bool value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto checked = not check_write_unchecked(Opts);
         if constexpr (checked) {
            if (!ensure_space(ctx, b, ix + 8)) [[unlikely]] {
               return;
            }
         }

         if constexpr (check_bools_as_numbers(Opts)) {
            if (value) {
               std::memcpy(&b[ix], "1", 1);
            }
            else {
               std::memcpy(&b[ix], "0", 1);
            }
            ++ix;
         }
         else {
            if (value) {
               std::memcpy(&b[ix], "true", 4);
               ix += 4;
            }
            else {
               std::memcpy(&b[ix], "false", 5);
               ix += 5;
            }
         }
      }
   };

   template <num_t T>
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         // Numbers can be up to ~25 chars for doubles
         if (!ensure_space(ctx, b, ix + 32 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         if constexpr (check_quoted_num(Opts)) {
            std::memcpy(&b[ix], "\"", 1);
            ++ix;
            write_chars::op<Opts>(value, ctx, b, ix);
            std::memcpy(&b[ix], "\"", 1);
            ++ix;
         }
         else {
            write_chars::op<Opts>(value, ctx, b, ix);
         }
      }
   };

   // Enum with glz::meta/glz::enumerate - writes string representation
   template <class T>
      requires((glaze_enum_t<T> || (meta_keys<T> && std::is_enum_v<std::decay_t<T>>)) && not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         const sv str = get_enum_name(value);
         if (!str.empty()) {
            // Write as quoted string for TOML
            if (!ensure_space(ctx, b, ix + str.size() + 3 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            if constexpr (not check_unquoted(Opts)) {
               dump('"', b, ix);
            }
            dump_maybe_empty(str, b, ix);
            if constexpr (not check_unquoted(Opts)) {
               dump('"', b, ix);
            }
         }
         else [[unlikely]] {
            // Value doesn't have a mapped string, serialize as underlying number
            serialize<TOML>::op<Opts>(static_cast<std::underlying_type_t<T>>(value), ctx, b, ix);
         }
      }
   };

   // Raw enum (without glz::meta) - writes as underlying numeric type
   template <class T>
      requires(!meta_keys<T> && std::is_enum_v<std::decay_t<T>> && !glaze_enum_t<T> && !custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         // serialize as underlying number
         serialize<TOML>::op<Opts>(static_cast<std::underlying_type_t<std::decay_t<T>>>(value), ctx,
                                   std::forward<Args>(args)...);
      }
   };

   // ============================================
   // std::chrono serialization
   // ============================================

   // Duration: serialize as count
   template <is_duration T>
      requires(not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using Rep = typename std::remove_cvref_t<T>::rep;
         to<TOML, Rep>::template op<Opts>(value.count(), ctx, b, ix);
      }
   };

   // system_clock::time_point: serialize as TOML native datetime (RFC 3339)
   // TOML datetimes are NOT quoted - they're native values
   // Output format: YYYY-MM-DDTHH:MM:SS[.fraction]Z
   template <is_system_time_point T>
      requires(not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using namespace std::chrono;
         using TP = std::remove_cvref_t<T>;
         using Duration = typename TP::duration;

         // Split into date and time-of-day
         const auto dp = floor<days>(value);
         const year_month_day ymd{dp};
         const hh_mm_ss tod{floor<Duration>(value - dp)};

         // Extract components
         const int yr = static_cast<int>(ymd.year());
         const unsigned mo = static_cast<unsigned>(ymd.month());
         const unsigned dy = static_cast<unsigned>(ymd.day());
         const auto hr = static_cast<unsigned>(tod.hours().count());
         const auto mi = static_cast<unsigned>(tod.minutes().count());
         const auto sc = static_cast<unsigned>(tod.seconds().count());

         // Calculate fractional digits based on duration precision
         constexpr size_t frac_digits = []() constexpr {
            using Period = typename Duration::period;
            if constexpr (std::ratio_greater_equal_v<Period, std::ratio<1>>) {
               return 0; // seconds or coarser
            }
            else if constexpr (std::ratio_greater_equal_v<Period, std::milli>) {
               return 3; // milliseconds
            }
            else if constexpr (std::ratio_greater_equal_v<Period, std::micro>) {
               return 6; // microseconds
            }
            else {
               return 9; // nanoseconds or finer
            }
         }();

         // Max size: YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ = 30 (no quotes for TOML)
         constexpr size_t max_size = 21 + (frac_digits > 0 ? 1 + frac_digits : 0);
         if (!ensure_space(ctx, b, ix + max_size + write_padding_bytes)) [[unlikely]] {
            return;
         }

         // Helper to write N-digit zero-padded number
         auto write_digits = [&]<size_t N>(uint64_t val) {
            for (size_t i = N; i > 0; --i) {
               b[ix + i - 1] = static_cast<char>('0' + val % 10);
               val /= 10;
            }
            ix += N;
         };

         // Write datetime without quotes (TOML native format)
         write_digits.template operator()<4>(static_cast<uint64_t>(yr));
         b[ix++] = '-';
         write_digits.template operator()<2>(mo);
         b[ix++] = '-';
         write_digits.template operator()<2>(dy);
         b[ix++] = 'T';
         write_digits.template operator()<2>(hr);
         b[ix++] = ':';
         write_digits.template operator()<2>(mi);
         b[ix++] = ':';
         write_digits.template operator()<2>(sc);

         // Write fractional seconds if duration is finer than seconds
         if constexpr (frac_digits > 0) {
            b[ix++] = '.';
            const auto subsec = tod.subseconds();
            if constexpr (frac_digits == 3) {
               write_digits.template operator()<3>(static_cast<uint64_t>(duration_cast<milliseconds>(subsec).count()));
            }
            else if constexpr (frac_digits == 6) {
               write_digits.template operator()<6>(static_cast<uint64_t>(duration_cast<microseconds>(subsec).count()));
            }
            else {
               write_digits.template operator()<9>(static_cast<uint64_t>(duration_cast<nanoseconds>(subsec).count()));
            }
         }

         b[ix++] = 'Z'; // UTC timezone marker
      }
   };

   // year_month_day: serialize as TOML Local Date (YYYY-MM-DD)
   template <is_year_month_day T>
      requires(not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using namespace std::chrono;

         const int yr = static_cast<int>(value.year());
         const unsigned mo = static_cast<unsigned>(value.month());
         const unsigned dy = static_cast<unsigned>(value.day());

         // YYYY-MM-DD = 10 chars
         constexpr size_t max_size = 10;
         if (!ensure_space(ctx, b, ix + max_size + write_padding_bytes)) [[unlikely]] {
            return;
         }

         // Helper to write N-digit zero-padded number
         auto write_digits = [&]<size_t N>(uint64_t val) {
            for (size_t i = N; i > 0; --i) {
               b[ix + i - 1] = static_cast<char>('0' + val % 10);
               val /= 10;
            }
            ix += N;
         };

         write_digits.template operator()<4>(static_cast<uint64_t>(yr));
         b[ix++] = '-';
         write_digits.template operator()<2>(mo);
         b[ix++] = '-';
         write_digits.template operator()<2>(dy);
      }
   };

   // hh_mm_ss: serialize as TOML Local Time (HH:MM:SS[.fraction])
   template <is_hh_mm_ss T>
      requires(not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using namespace std::chrono;
         using Precision = typename std::remove_cvref_t<T>::precision;

         const auto hr = static_cast<unsigned>(value.hours().count());
         const auto mi = static_cast<unsigned>(value.minutes().count());
         const auto sc = static_cast<unsigned>(value.seconds().count());

         // Calculate fractional digits based on precision
         constexpr size_t frac_digits = []() constexpr {
            using Period = typename Precision::period;
            if constexpr (std::ratio_greater_equal_v<Period, std::ratio<1>>) {
               return 0; // seconds or coarser
            }
            else if constexpr (std::ratio_greater_equal_v<Period, std::milli>) {
               return 3; // milliseconds
            }
            else if constexpr (std::ratio_greater_equal_v<Period, std::micro>) {
               return 6; // microseconds
            }
            else {
               return 9; // nanoseconds or finer
            }
         }();

         // HH:MM:SS.nnnnnnnnn = max 18 chars
         constexpr size_t max_size = 8 + (frac_digits > 0 ? 1 + frac_digits : 0);
         if (!ensure_space(ctx, b, ix + max_size + write_padding_bytes)) [[unlikely]] {
            return;
         }

         // Helper to write N-digit zero-padded number
         auto write_digits = [&]<size_t N>(uint64_t val) {
            for (size_t i = N; i > 0; --i) {
               b[ix + i - 1] = static_cast<char>('0' + val % 10);
               val /= 10;
            }
            ix += N;
         };

         write_digits.template operator()<2>(hr);
         b[ix++] = ':';
         write_digits.template operator()<2>(mi);
         b[ix++] = ':';
         write_digits.template operator()<2>(sc);

         // Write fractional seconds if precision is finer than seconds
         if constexpr (frac_digits > 0) {
            b[ix++] = '.';
            const auto subsec = value.subseconds();
            if constexpr (frac_digits == 3) {
               write_digits.template operator()<3>(static_cast<uint64_t>(duration_cast<milliseconds>(subsec).count()));
            }
            else if constexpr (frac_digits == 6) {
               write_digits.template operator()<6>(static_cast<uint64_t>(duration_cast<microseconds>(subsec).count()));
            }
            else {
               write_digits.template operator()<9>(static_cast<uint64_t>(duration_cast<nanoseconds>(subsec).count()));
            }
         }
      }
   };

   // steady_clock::time_point: serialize as count in the time_point's native duration
   template <is_steady_time_point T>
      requires(not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         const auto count = value.time_since_epoch().count();
         to<TOML, Rep>::template op<Opts>(count, ctx, b, ix);
      }
   };

   // high_resolution_clock::time_point when it's a distinct type (rare)
   template <is_high_res_time_point T>
      requires(not custom_write<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         // Treat like steady_clock - serialize as count since epoch is implementation-defined
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         const auto count = value.time_since_epoch().count();
         to<TOML, Rep>::template op<Opts>(count, ctx, b, ix);
      }
   };

   template <class T>
      requires str_t<T> || char_t<T>
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (check_string_as_number(Opts)) {
            const sv str = [&]() -> const sv {
               if constexpr (char_t<T>) {
                  return sv{&value, 1};
               }
               else if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
                  return value ? value : "";
               }
               else {
                  return sv{value};
               }
            }();
            if (!ensure_space(ctx, b, ix + str.size() + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump_maybe_empty(value, b, ix);
         }
         else if constexpr (char_t<T>) {
            if constexpr (check_unquoted(Opts)) {
               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump(value, b, ix);
            }
            else {
               if (!ensure_space(ctx, b, ix + 8 + write_padding_bytes)) [[unlikely]] {
                  return;
               }

               std::memcpy(&b[ix], "\"", 1);
               ++ix;
               if (const auto escaped = char_escape_table[uint8_t(value)]; escaped) {
                  std::memcpy(&b[ix], &escaped, 2);
                  ix += 2;
               }
               else if (value == '\0') {
                  // null character treated as empty string
               }
               else {
                  std::memcpy(&b[ix], &value, 1);
                  ++ix;
               }
               std::memcpy(&b[ix], "\"", 1);
               ++ix;
            }
         }
         else {
            if constexpr (check_raw_string(Opts)) {
               const sv str = [&]() -> const sv {
                  if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
                     return value ? value : "";
                  }
                  else {
                     return sv{value};
                  }
               }();

               if (!ensure_space(ctx, b, ix + 8 + str.size() + write_padding_bytes)) [[unlikely]] {
                  return;
               }

               std::memcpy(&b[ix], "\"", 1);
               ++ix;
               if (str.size()) [[likely]] {
                  const auto n = str.size();
                  std::memcpy(&b[ix], str.data(), n);
                  ix += n;
               }
               std::memcpy(&b[ix], "\"", 1);
               ++ix;
            }
            else {
               const sv str = [&]() -> const sv {
                  if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
                     return value ? value : "";
                  }
                  else if constexpr (array_char_t<T>) {
                     return sv{value.data(), value.size()};
                  }
                  else {
                     return sv{value};
                  }
               }();
               const auto n = str.size();
               // Worst case: each char becomes 2 chars when escaped, plus quotes
               if (!ensure_space(ctx, b, ix + 10 + 2 * n + write_padding_bytes)) [[unlikely]] {
                  return;
               }

               if constexpr (check_unquoted(Opts)) {
                  const auto n = str.size();
                  if (n) {
                     std::memcpy(&b[ix], str.data(), n);
                     ix += n;
                  }
               }
               else {
                  std::memcpy(&b[ix], "\"", 1);
                  ++ix;
                  const auto* c = str.data();
                  const auto* const e = c + n;
                  const auto start = &b[ix];
                  auto data = start;

                  if (n > 7) {
                     for (const auto end_m7 = e - 7; c < end_m7;) {
                        std::memcpy(data, c, 8);
                        uint64_t swar;
                        std::memcpy(&swar, c, 8);
                        if constexpr (std::endian::native == std::endian::big) {
                           swar = std::byteswap(swar);
                        }

                        constexpr uint64_t lo7_mask = repeat_byte8(0b01111111);
                        const uint64_t lo7 = swar & lo7_mask;
                        const uint64_t quote = (lo7 ^ repeat_byte8('"')) + lo7_mask;
                        const uint64_t backslash = (lo7 ^ repeat_byte8('\\')) + lo7_mask;
                        const uint64_t less_32 = (swar & repeat_byte8(0b01100000)) + lo7_mask;
                        uint64_t next = ~((quote & backslash & less_32) | swar);

                        next &= repeat_byte8(0b10000000);
                        if (next == 0) {
                           data += 8;
                           c += 8;
                           continue;
                        }

                        const auto length = (countr_zero(next) >> 3);
                        c += length;
                        data += length;

                        std::memcpy(data, &char_escape_table[uint8_t(*c)], 2);
                        data += 2;
                        ++c;
                     }
                  }

                  for (; c < e; ++c) {
                     if (const auto escaped = char_escape_table[uint8_t(*c)]; escaped) {
                        std::memcpy(data, &escaped, 2);
                        data += 2;
                     }
                     else {
                        std::memcpy(data, c, 1);
                        ++data;
                     }
                  }

                  ix += size_t(data - start);

                  std::memcpy(&b[ix], "\"", 1);
                  ++ix;
               }
            }
         }
      }
   };

   template <auto Opts, bool minified_check = true, class B>
      requires(Opts.format == TOML)
   GLZ_ALWAYS_INLINE void write_array_entry_separator(is_context auto&& ctx, B&& b, auto& ix)
   {
      if constexpr (minified_check) {
         if (!ensure_space(ctx, b, ix + 2)) [[unlikely]] {
            return;
         }
      }
      std::memcpy(&b[ix], ", ", 2);
      ix += 2;
      if constexpr (is_output_streaming<B>) {
         flush_buffer(b, ix);
      }
   }

   template <auto Opts, bool minified_check = true, class B>
      requires(Opts.format == TOML)
   GLZ_ALWAYS_INLINE void write_object_entry_separator(is_context auto&& ctx, B&& b, auto& ix)
   {
      if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
         return;
      }
      std::memcpy(&b[ix], "\n", 1);
      ++ix;
      if constexpr (is_output_streaming<B>) {
         flush_buffer(b, ix);
      }
   }

   // ============================================
   // inline_table wrapper for TOML
   // Forces array-of-objects to serialize using inline table syntax {key = value}
   // instead of array-of-tables [[name]] syntax
   // ============================================

   template <class T>
   struct inline_table_t
   {
      static constexpr bool glaze_wrapper = true;
      static constexpr auto glaze_reflect = false;
      using value_type = T;
      T& val;
   };

   template <class T>
   inline_table_t(T&) -> inline_table_t<T>;

   // Helper to create wrapper from member pointer
   template <auto MemPtr>
   inline constexpr decltype(auto) inline_table_impl() noexcept
   {
      return [](auto&& val) { return inline_table_t<std::remove_reference_t<decltype(val.*MemPtr)>>{val.*MemPtr}; };
   }

   // Usage: glz::inline_table<&T::member>
   template <auto MemPtr>
   constexpr auto inline_table = inline_table_impl<MemPtr>();

   // Detect inline_table wrapper
   template <class T>
   struct is_inline_table : std::false_type
   {};

   template <class T>
   struct is_inline_table<inline_table_t<T>> : std::true_type
   {};

   template <class T>
   inline constexpr bool is_inline_table_v = is_inline_table<std::remove_cvref_t<T>>::value;

   // Forward declaration for inline object writer
   template <auto Opts, class T, class V, class B>
   void write_inline_object(V&& value, is_context auto&& ctx, B&& b, auto& ix);

   // Specialization for inline_table_t wrapper - writes array of objects as inline tables
   // Example output: [{name = "foo", id = 1}, {name = "bar", id = 2}]
   template <class T>
   struct to<TOML, inline_table_t<T>>
   {
      template <auto Opts, class B>
      static void op(auto&& wrapper, is_context auto&& ctx, B&& b, auto& ix)
      {
         auto& value = wrapper.val;

         if (empty_range(value)) {
            if (!ensure_space(ctx, b, ix + 2 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump("[]", b, ix);
            return;
         }

         if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump('[', b, ix);

         bool first = true;
         for (auto&& element : value) {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if (!first) {
               if (!ensure_space(ctx, b, ix + 2)) [[unlikely]] {
                  return;
               }
               dump(", ", b, ix);
            }
            else {
               first = false;
            }

            // Write inline object {key = value, ...}
            using element_t = std::remove_cvref_t<decltype(element)>;
            write_inline_object<Opts, element_t>(element, ctx, b, ix);
         }

         if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
            return;
         }
         dump(']', b, ix);
      }
   };

   // Write an object in inline table format: {key1 = value1, key2 = value2}
   template <auto Options, class T, class V, class B>
   void write_inline_object(V&& value, is_context auto&& ctx, B&& b, auto& ix)
   {
      static constexpr auto N = reflect<T>::size;
      using Type = T;

      decltype(auto) t = [&]() -> decltype(auto) {
         if constexpr (reflectable<Type>) {
            return to_tie(value);
         }
         else {
            return nullptr;
         }
      }();

      if (!ensure_space(ctx, b, ix + 2 + write_padding_bytes)) [[unlikely]] {
         return;
      }
      dump('{', b, ix);

      bool first = true;
      for_each<N>([&]<size_t I>() {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         using val_t = field_t<Type, I>;

         // Skip member function pointers unless explicitly enabled
         constexpr bool write_function_pointers = check_write_function_pointers(Options);
         if constexpr (always_skipped<val_t> || (!write_function_pointers && is_member_function_pointer<val_t>)) {
            return;
         }

         // Check if nullable and null
         if constexpr (null_t<val_t>) {
            if constexpr (always_null_t<val_t>) {
               return;
            }
            else {
               decltype(auto) element = [&]() -> decltype(auto) {
                  if constexpr (reflectable<Type>) {
                     return get<I>(t);
                  }
                  else {
                     return get<I>(reflect<Type>::values);
                  }
               };

               bool is_null = false;
               if constexpr (nullable_wrapper<val_t>)
                  is_null = !bool(element()(value).val);
               else if constexpr (nullable_value_t<val_t>)
                  is_null = !get_member(value, element()).has_value();
               else
                  is_null = !bool(get_member(value, element()));

               if (is_null) {
                  return;
               }
            }
         }

         static constexpr auto key = glz::get<I>(reflect<Type>::keys);
         static constexpr auto padding = key.size() + 16;
         if (!ensure_space(ctx, b, ix + padding)) [[unlikely]] {
            return;
         }

         if (!first) {
            dump(", ", b, ix);
         }
         else {
            first = false;
         }

         std::memcpy(&b[ix], key.data(), key.size());
         ix += key.size();
         dump(" = ", b, ix);

         // Write the value
         if constexpr (reflectable<Type>) {
            to<TOML, val_t>::template op<Options>(get_member(value, get<I>(t)), ctx, b, ix);
         }
         else {
            to<TOML, val_t>::template op<Options>(get_member(value, get<I>(reflect<Type>::values)), ctx, b, ix);
         }
      });

      if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
         return;
      }
      dump('}', b, ix);
   }

   // Type trait to detect if a type is an array of objects (for array-of-tables)
   template <class T>
   constexpr bool is_array_of_objects_v = []() constexpr {
      if constexpr (writable_array_t<T> && !str_t<T>) {
         using element_t = range_value_t<T>;
         return glaze_object_t<element_t> || reflectable<element_t>;
      }
      return false;
   }();

   // Helper to check if inline_arrays option is set (works with both opts and toml_opts)
   template <auto Opts>
   constexpr bool use_inline_arrays()
   {
      if constexpr (requires { Opts.inline_arrays; }) {
         return Opts.inline_arrays;
      }
      return false;
   }

   // Forward declaration for recursive calls with path
   template <auto Options, class T, class V, class B>
   void write_toml_object_with_path(V&& value, is_context auto&& ctx, B&& b, auto& ix, std::string_view path_prefix);

   template <class T>
      requires(glaze_object_t<T> || reflectable<T>)
   struct to<TOML, T>
   {
      template <auto Options, class V, class B>
         requires(not std::is_pointer_v<std::remove_cvref_t<V>>)
      static void op(V&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         // Call the path-aware version with empty prefix for top-level
         write_toml_object_with_path<Options, T>(std::forward<V>(value), ctx, b, ix, "");
      }
   };

   // Implementation of path-aware TOML object writer
   template <auto Options, class T, class V, class B>
   void write_toml_object_with_path(V&& value, is_context auto&& ctx, B&& b, auto& ix, std::string_view path_prefix)
   {
      static constexpr auto N = reflect<T>::size;
      decltype(auto) t = [&]() -> decltype(auto) {
         if constexpr (reflectable<T>) {
            return to_tie(value);
         }
         else {
            return nullptr;
         }
      }();

      static constexpr auto padding = round_up_to_nearest_16(maximum_key_size<T> + write_padding_bytes);
      bool first = true;

      // Helper lambda to check if a field should be skipped
      auto should_skip_field = [&]<size_t I>() constexpr {
         using val_t = field_t<T, I>;
         constexpr bool write_function_pointers = check_write_function_pointers(Options);
         return always_skipped<val_t> || (!write_function_pointers && is_member_function_pointer<val_t>);
      };

      // Helper lambda to check if nullable field is null
      auto is_null_field = [&]<size_t I>() -> bool {
         using val_t = field_t<T, I>;
         if constexpr (null_t<val_t>) {
            if constexpr (always_null_t<val_t>) {
               return true;
            }
            else {
               decltype(auto) element = [&]() -> decltype(auto) {
                  if constexpr (reflectable<T>) {
                     return get<I>(t);
                  }
                  else {
                     return get<I>(reflect<T>::values);
                  }
               };

               if constexpr (nullable_wrapper<val_t>)
                  return !bool(element()(value).val);
               else if constexpr (nullable_value_t<val_t>)
                  return !get_member(value, element()).has_value();
               else
                  return !bool(get_member(value, element()));
            }
         }
         return false;
      };

      // Helper lambda to write a scalar key-value pair
      auto write_scalar_field = [&]<size_t I>() {
         using val_t = field_t<T, I>;

         if (!ensure_space(ctx, b, ix + padding)) [[unlikely]] {
            return;
         }

         if (!first) {
            std::memcpy(&b[ix], "\n", 1);
            ++ix;
         }
         else {
            first = false;
         }
         static constexpr auto key = glz::get<I>(reflect<T>::keys);
         std::memcpy(&b[ix], key.data(), key.size());
         ix += key.size();

         std::memcpy(&b[ix], " = ", 3);
         ix += 3;

         if constexpr (reflectable<T>) {
            to<TOML, val_t>::template op<Options>(get_member(value, get<I>(t)), ctx, b, ix);
         }
         else {
            to<TOML, val_t>::template op<Options>(get_member(value, get<I>(reflect<T>::values)), ctx, b, ix);
         }
      };

      // Helper lambda to write a nested table [table] - not used when inside array-of-tables context
      auto write_table_field = [&]<size_t I>() {
         using val_t = field_t<T, I>;

         if (!ensure_space(ctx, b, ix + padding)) [[unlikely]] {
            return;
         }

         if (!first) {
            std::memcpy(&b[ix], "\n", 1);
            ++ix;
         }
         else {
            first = false;
         }
         static constexpr auto key = glz::get<I>(reflect<T>::keys);

         // Write [path_prefix.key] or [key] if no prefix
         std::memcpy(&b[ix], "[", 1);
         ++ix;
         if (!path_prefix.empty()) {
            if (!ensure_space(ctx, b, ix + path_prefix.size() + 1)) [[unlikely]] {
               return;
            }
            std::memcpy(&b[ix], path_prefix.data(), path_prefix.size());
            ix += path_prefix.size();
            std::memcpy(&b[ix], ".", 1);
            ++ix;
         }
         std::memcpy(&b[ix], key.data(), key.size());
         ix += key.size();
         std::memcpy(&b[ix], "]\n", 2);
         ix += 2;

         // Build new path prefix for nested content
         std::string new_prefix;
         if (path_prefix.empty()) {
            new_prefix = std::string(key);
         }
         else {
            new_prefix.reserve(path_prefix.size() + 1 + key.size());
            new_prefix = path_prefix;
            new_prefix += '.';
            new_prefix += key;
         }

         // Serialize the nested object with the new path
         if constexpr (reflectable<T>) {
            write_toml_object_with_path<Options, val_t>(get_member(value, get<I>(t)), ctx, b, ix, new_prefix);
         }
         else {
            write_toml_object_with_path<Options, val_t>(get_member(value, get<I>(reflect<T>::values)), ctx, b, ix,
                                                        new_prefix);
         }
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         // Add an extra newline to separate this table section from following keys.
         if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
            return;
         }
         std::memcpy(&b[ix], "\n", 1);
         ++ix;
      };

      // Helper lambda to write an array of tables [[array]]
      auto write_array_of_tables_field = [&]<size_t I>() {
         using val_t = field_t<T, I>;
         using element_t = range_value_t<val_t>;

         decltype(auto) arr = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return get_member(value, get<I>(t));
            }
            else {
               return get_member(value, get<I>(reflect<T>::values));
            }
         }();

         static constexpr auto key = glz::get<I>(reflect<T>::keys);

         if (empty_range(arr)) {
            // Empty array - write as inline empty array
            if (!ensure_space(ctx, b, ix + padding + 5)) [[unlikely]] {
               return;
            }
            if (!first) {
               std::memcpy(&b[ix], "\n", 1);
               ++ix;
            }
            else {
               first = false;
            }
            std::memcpy(&b[ix], key.data(), key.size());
            ix += key.size();
            std::memcpy(&b[ix], " = []", 5);
            ix += 5;
            return;
         }

         // Build the full path for array-of-tables header
         std::string full_path;
         if (path_prefix.empty()) {
            full_path = std::string(key);
         }
         else {
            full_path.reserve(path_prefix.size() + 1 + key.size());
            full_path = path_prefix;
            full_path += '.';
            full_path += key;
         }

         for (auto&& element : arr) {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if (!ensure_space(ctx, b, ix + padding + full_path.size() + 6)) [[unlikely]] {
               return;
            }

            if (!first) {
               std::memcpy(&b[ix], "\n", 1);
               ++ix;
            }
            else {
               first = false;
            }

            // Write [[full_path]]
            std::memcpy(&b[ix], "[[", 2);
            ix += 2;
            std::memcpy(&b[ix], full_path.data(), full_path.size());
            ix += full_path.size();
            std::memcpy(&b[ix], "]]\n", 3);
            ix += 3;

            // Serialize the element with the full_path as prefix for nested arrays
            write_toml_object_with_path<Options, element_t>(element, ctx, b, ix, full_path);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            // Add newline after element
            if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
               return;
            }
            std::memcpy(&b[ix], "\n", 1);
            ++ix;
         }
      };

      // Check if inline_arrays mode is enabled
      constexpr bool inline_mode = use_inline_arrays<Options>();

      // PASS 1: Write all scalar fields first (TOML spec conformance)
      // In inline_mode, arrays of objects are treated as scalars (written inline)
      for_each<N>([&]<size_t I>() {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         using val_t = field_t<T, I>;

         if constexpr (should_skip_field.template operator()<I>()) {
            return;
         }

         // Skip null fields
         if (is_null_field.template operator()<I>()) {
            return;
         }

         // Only process scalar fields in this pass (not objects or arrays of objects)
         // Exception: in inline_mode, arrays of objects are written as inline arrays
         constexpr bool is_scalar =
            !(glaze_object_t<val_t> || reflectable<val_t>) && (!is_array_of_objects_v<val_t> || inline_mode);
         if constexpr (is_scalar) {
            write_scalar_field.template operator()<I>();
         }
      });

      // PASS 2: Write nested tables [table] and arrays of tables [[array]]
      // In inline_mode, arrays of objects are skipped here (already written in pass 1)
      for_each<N>([&]<size_t I>() {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         using val_t = field_t<T, I>;

         if constexpr (should_skip_field.template operator()<I>()) {
            return;
         }

         // Skip null fields
         if (is_null_field.template operator()<I>()) {
            return;
         }

         // Process nested objects and arrays of objects
         if constexpr (glaze_object_t<val_t> || reflectable<val_t>) {
            write_table_field.template operator()<I>();
         }
         else if constexpr (is_array_of_objects_v<val_t> && !inline_mode) {
            write_array_of_tables_field.template operator()<I>();
         }
      });
   }

   template <class T>
      requires(writable_array_t<T> || writable_map_t<T>)
   struct to<TOML, T>
   {
      static constexpr bool map_like_array = writable_array_t<T> && pair_t<range_value_t<T>>;

      // Helper to write an array element - uses inline table format for objects when in inline mode
      template <auto Opts, class V, class B>
      GLZ_ALWAYS_INLINE static void write_element(V&& element, is_context auto&& ctx, B&& b, auto& ix)
      {
         using val_t = std::remove_cvref_t<V>;
         constexpr bool is_object_type = glaze_object_t<val_t> || reflectable<val_t>;
         constexpr bool inline_mode = use_inline_arrays<Opts>();

         if constexpr (is_object_type && inline_mode) {
            // Write object as inline table {key = value, ...}
            write_inline_object<Opts, val_t>(std::forward<V>(element), ctx, b, ix);
         }
         else {
            to<TOML, val_t>::template op<Opts>(std::forward<V>(element), ctx, b, ix);
         }
      }

      // --- Array-like container writer ---
      template <auto Opts, class B>
         requires(writable_array_t<T> && (map_like_array ? check_concatenate(Opts) == false : true))
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (empty_range(value)) {
            if (!ensure_space(ctx, b, ix + 2 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump("[]", b, ix);
         }
         else {
            if constexpr (has_size<T>) {
               const auto n = value.size();
               // Use 2 bytes per separator (", ")
               static constexpr auto comma_padding = 2;
               if (!ensure_space(ctx, b, ix + n * comma_padding + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               std::memcpy(&b[ix], "[", 1);
               ++ix;
               auto it = std::begin(value);
               write_element<Opts>(*it, ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               ++it;
               for (const auto fin = std::end(value); it != fin; ++it) {
                  write_array_entry_separator<Opts>(ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  write_element<Opts>(*it, ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }
               if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
                  return;
               }
               std::memcpy(&b[ix], "]", 1);
               ++ix;
            }
            else {
               if (!ensure_space(ctx, b, ix + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               std::memcpy(&b[ix], "[", 1);
               ++ix;
               auto it = std::begin(value);
               write_element<Opts>(*it, ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               ++it;
               for (const auto fin = std::end(value); it != fin; ++it) {
                  write_array_entry_separator<Opts>(ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  write_element<Opts>(*it, ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }
               if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
                  return;
               }
               dump(']', b, ix);
            }
         }
      }

      // --- Map-like container writer ---
      template <auto Opts, class B>
         requires(writable_map_t<T> || (map_like_array && check_concatenate(Opts) == true))
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         bool first = true;
         for (auto&& [key, val] : value) {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (!first) {
               write_object_entry_separator<Opts>(ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
            }
            else {
               first = false;
            }
            // Write the key as a bare key
            if (!ensure_space(ctx, b, ix + key.size() + 4 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            std::memcpy(&b[ix], key.data(), key.size());
            ix += key.size();
            std::memcpy(&b[ix], " = ", 3);
            ix += 3;
            to<TOML, decltype(val)>::template op<Opts>(val, ctx, b, ix);
         }
      }
   };

   // (The remainder of the code – for C arrays, tuples, includers, etc. – is unchanged.)
   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct to<TOML, T>
   {
      template <auto Opts, class V, size_t N, class... Args>
      GLZ_ALWAYS_INLINE static void op(const V (&value)[N], is_context auto&& ctx, Args&&... args)
      {
         serialize<TOML>::op<Opts>(std::span{value, N}, ctx, std::forward<Args>(args)...);
      }
   };

   template <class T>
      requires glaze_array_t<T> || tuple_t<std::decay_t<T>> || is_std_tuple<T>
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto N = []() constexpr {
            if constexpr (glaze_array_t<std::decay_t<T>>) {
               return glz::tuple_size_v<meta_t<std::decay_t<T>>>;
            }
            else {
               return glz::tuple_size_v<std::decay_t<T>>;
            }
         }();

         if (!ensure_space(ctx, b, ix + 2 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump('[', b, ix);
         using V = std::decay_t<T>;
         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if constexpr (glaze_array_t<V>) {
               serialize<TOML>::op<Opts>(get_member(value, glz::get<I>(meta_v<T>)), ctx, b, ix);
            }
            else if constexpr (is_std_tuple<T>) {
               using Value = core_t<decltype(std::get<I>(value))>;
               to<TOML, Value>::template op<Opts>(std::get<I>(value), ctx, b, ix);
            }
            else {
               using Value = core_t<decltype(glz::get<I>(value))>;
               to<TOML, Value>::template op<Opts>(glz::get<I>(value), ctx, b, ix);
            }
            constexpr bool needs_comma = I < N - 1;
            if constexpr (needs_comma) {
               write_array_entry_separator<Opts>(ctx, b, ix);
            }
         });
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
            return;
         }
         dump(']', b, ix);
      }
   };

   template <is_includer T>
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (!ensure_space(ctx, b, ix + 2 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump<R"("")">(b, ix); // dump an empty string
      }
   };

   // Variant support for TOML
   // Serializes the active variant alternative using the appropriate TOML type
   template <is_variant T>
   struct to<TOML, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         std::visit(
            [&](auto&& val) {
               using V = std::decay_t<decltype(val)>;
               to<TOML, V>::template op<Opts>(val, ctx, b, ix);
            },
            value);
      }
   };

   template <write_supported<TOML> T, output_buffer Buffer>
   [[nodiscard]] error_ctx write_toml(T&& value, Buffer&& buffer)
   {
      return write<opts{.format = TOML}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <write_supported<TOML> T, raw_buffer Buffer>
   [[nodiscard]] glz::expected<size_t, error_ctx> write_toml(T&& value, Buffer&& buffer)
   {
      return write<opts{.format = TOML}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <write_supported<TOML> T>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_toml(T&& value)
   {
      return write<opts{.format = TOML}>(std::forward<T>(value));
   }

   template <auto Opts = opts{.format = TOML}, write_supported<TOML> T>
   [[nodiscard]] error_ctx write_file_toml(T&& value, const sv file_name, auto&& buffer)
   {
      const auto ec = write<set_toml<Opts>()>(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         return ec;
      }
      const auto file_ec = buffer_to_file(buffer, file_name);
      if (bool(file_ec)) [[unlikely]] {
         return {0, file_ec};
      }
      return {buffer.size(), error_code::none};
   }
}
