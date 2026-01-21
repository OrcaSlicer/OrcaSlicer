// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/beve/header.hpp"
#include "glaze/beve/key_traits.hpp"
#include "glaze/beve/skip.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/file/file_ops.hpp"
#include "glaze/util/dump.hpp"

// To handle invalid inputs we must check if (it >= end) at the beginning of each function
// This way we can always call a function after incrementing the iterator without needed to do a tail check
// If we know the first function called has an end check, we don't need a guard at the top of the function
// Also, after almost every function call we need to check if an error was produced

namespace glz
{
   template <>
   struct parse<BEVE>
   {
      template <auto Opts, class T, class Tag, is_context Ctx, class It0, class It1>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(T&& value, Tag&& tag, Ctx&& ctx, It0&& it, It1 end)
      {
         if constexpr (const_value_v<T>) {
            if constexpr (check_error_on_const_read(Opts)) {
               ctx.error = error_code::attempt_const_read;
            }
            else {
               // do not read anything into the const value
               skip_value<BEVE>::op<Opts>(std::forward<Ctx>(ctx), std::forward<It0>(it), end);
            }
         }
         else {
            using V = std::remove_cvref_t<T>;
            from<BEVE, V>::template op<Opts>(std::forward<T>(value), std::forward<Tag>(tag), std::forward<Ctx>(ctx),
                                             std::forward<It0>(it), end);
         }
      }

      template <auto Opts, class T, is_context Ctx, class It0, class It1>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         if constexpr (const_value_v<T>) {
            if constexpr (check_error_on_const_read(Opts)) {
               ctx.error = error_code::attempt_const_read;
            }
            else {
               // do not read anything into the const value
               skip_value<BEVE>::op<Opts>(std::forward<Ctx>(ctx), std::forward<It0>(it), end);
            }
         }
         else {
            using V = std::remove_cvref_t<T>;
            from<BEVE, V>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it),
                                             end);
         }
      }
   };

   template <class T>
      requires(glaze_value_t<T> && !custom_read<T>)
   struct from<BEVE, T>
   {
      template <auto Opts, class Value, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = std::decay_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         from<BEVE, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                          std::forward<Ctx>(ctx), std::forward<It0>(it), end);
      }

      template <auto Opts, class Value, class Tag, is_context Ctx, class It0, class It1>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(Value&& value, Tag&& tag, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = std::decay_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         from<BEVE, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                          std::forward<Tag>(tag), std::forward<Ctx>(ctx), std::forward<It0>(it), end);
      }
   };

   template <always_null_t T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         if (uint8_t(*it)) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;
      }
   };

   template <>
   struct from<BEVE, hidden>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&...) noexcept
      {
         ctx.error = error_code::attempt_read_hidden;
      }
   };

   template <is_bitset T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);

         constexpr uint8_t type = uint8_t(3) << 3;
         constexpr uint8_t header = tag::typed_array | type;

         if (tag != header) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it;
         const auto n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         const auto num_bytes = (value.size() + 7) / 8;
         for (size_t byte_i{}, i{}; byte_i < num_bytes; ++byte_i, ++it) {
            if (invalid_end(ctx, it, end)) {
               return;
            }
            uint8_t byte;
            std::memcpy(&byte, it, 1);
            for (size_t bit_i = 0; bit_i < 8 && i < n; ++bit_i, ++i) {
               value[i] = byte >> bit_i & uint8_t(1);
            }
         }
      }
   };

   template <>
   struct from<BEVE, skip>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&... args) noexcept
      {
         skip_value<BEVE>::op<Opts>(ctx, args...);
      }
   };

   template <glaze_flags_t T>
   struct from<BEVE, T>
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         constexpr auto N = reflect<T>::size;

         constexpr auto Length = byte_length<T>();
         uint8_t data[Length];

         if ((it + Length) > end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         std::memcpy(data, it, Length);
         it += Length;

         for_each<N>([&]<size_t I>() {
            get_member(value, get<I>(reflect<T>::values)) = data[I / 8] & (uint8_t{1} << (7 - (I % 8)));
         });
      }
   };

   template <class T>
      requires(num_t<T> || char_t<T> || glaze_enum_t<T>)
   struct from<BEVE, T>
   {
      static constexpr uint8_t type = std::floating_point<T> ? 0 : (std::is_signed_v<T> ? 0b000'01'000 : 0b000'10'000);
      static constexpr uint8_t header = tag::number | type | (byte_count<T> << 5);

      template <auto Opts>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, const uint8_t tag, is_context auto&& ctx, auto&& it,
                                       auto end) noexcept
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }

         using V = std::decay_t<decltype(value)>;

         constexpr auto is_volatile = std::is_volatile_v<std::remove_reference_t<decltype(value)>>;

         if (tag != header) {
            if constexpr (check_allow_conversions(Opts)) {
               if constexpr (num_t<T>) {
                  if ((tag & 0b00000111) != tag::number) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  auto decode = [&](auto&& i) {
                     if ((it + sizeof(i)) > end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                     using I = std::decay_t<decltype(i)>;
                     std::memcpy(&i, it, sizeof(I));
                     if constexpr (std::endian::native == std::endian::big && sizeof(I) > 1) {
                        byteswap_le(i);
                     }
                     value = static_cast<V>(i);
                     it += sizeof(i);
                  };

                  switch (tag) {
                  case tag::f32: {
                     if constexpr (std::integral<V>) {
                        // We do not allow cross conversions from floats to integral types
                        ctx.error = error_code::syntax_error;
                     }
                     else {
                        static_assert(sizeof(float) == 4);
                        // TODO: use float32_t in C++23
                        decode(float{});
                     }
                     return;
                  }
                  case tag::f64: {
                     if constexpr (std::integral<V>) {
                        // We do not allow cross conversions from floats to integral types
                        ctx.error = error_code::syntax_error;
                     }
                     else {
                        static_assert(sizeof(double) == 8);
                        // TODO: use float64_t in C++23
                        decode(double{});
                     }
                     return;
                  }
                  case tag::i8: {
                     decode(int8_t{});
                     return;
                  }
                  case tag::i16: {
                     decode(int16_t{});
                     return;
                  }
                  case tag::i32: {
                     decode(int32_t{});
                     return;
                  }
                  case tag::i64: {
                     decode(int64_t{});
                     return;
                  }
                  case tag::u8: {
                     decode(uint8_t{});
                     return;
                  }
                  case tag::u16: {
                     decode(uint16_t{});
                     return;
                  }
                  case tag::u32: {
                     decode(uint32_t{});
                     return;
                  }
                  case tag::u64: {
                     decode(uint64_t{});
                     return;
                  }
                  default: {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  }
               }
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }

         if ((it + sizeof(V)) > end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         if constexpr (is_volatile) {
            V temp;
            std::memcpy(&temp, it, sizeof(V));
            if constexpr (std::endian::native == std::endian::big && sizeof(V) > 1) {
               byteswap_le(temp);
            }
            value = temp;
         }
         else {
            std::memcpy(&value, it, sizeof(V));
            if constexpr (std::endian::native == std::endian::big && sizeof(V) > 1) {
               byteswap_le(value);
            }
         }
         it += sizeof(V);
      }

      template <auto Opts>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         ++it;
         op<no_header_on<Opts>()>(value, tag, ctx, it, end);
      }
   };

   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T>)
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         using V = std::underlying_type_t<std::decay_t<T>>;

         if constexpr (check_no_header(Opts)) {
            if ((it + sizeof(V)) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if constexpr (std::endian::native == std::endian::big && sizeof(V) > 1) {
               V temp;
               std::memcpy(&temp, it, sizeof(V));
               byteswap_le(temp);
               value = static_cast<std::decay_t<T>>(temp);
            }
            else {
               std::memcpy(&value, it, sizeof(V));
            }
            it += sizeof(V);
         }
         else {
            constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t header = tag::number | type | (byte_count<V> << 5);

            if (invalid_end(ctx, it, end)) {
               return;
            }
            const auto tag = uint8_t(*it);
            if (tag != header) {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            if ((it + sizeof(V)) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if constexpr (std::endian::native == std::endian::big && sizeof(V) > 1) {
               V temp;
               std::memcpy(&temp, it, sizeof(V));
               byteswap_le(temp);
               value = static_cast<std::decay_t<T>>(temp);
            }
            else {
               std::memcpy(&value, it, sizeof(V));
            }
            it += sizeof(V);
         }
      }
   };

   template <class T>
      requires complex_t<T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (check_no_header(Opts)) {
            using V = std::decay_t<T>;
            if ((it + sizeof(V)) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if constexpr (std::endian::native == std::endian::big) {
               using X = typename V::value_type;
               X real_part, imag_part;
               std::memcpy(&real_part, it, sizeof(X));
               it += sizeof(X);
               std::memcpy(&imag_part, it, sizeof(X));
               it += sizeof(X);
               byteswap_le(real_part);
               byteswap_le(imag_part);
               value = V{real_part, imag_part};
            }
            else {
               std::memcpy(&value, it, sizeof(V));
               it += sizeof(V);
            }
         }
         else {
            constexpr uint8_t header = tag::extensions | 0b00011'000;

            if (invalid_end(ctx, it, end)) {
               return;
            }
            const auto tag = uint8_t(*it);
            if (tag != header) {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it;

            using V = typename T::value_type;
            constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t complex_number = 0;
            constexpr uint8_t complex_header = complex_number | type | (byte_count<V> << 5);

            if (invalid_end(ctx, it, end)) {
               return;
            }
            const auto complex_tag = uint8_t(*it);
            if (complex_tag != complex_header) {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it;

            if ((it + 2 * sizeof(V)) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if constexpr (std::endian::native == std::endian::big) {
               V real_part, imag_part;
               std::memcpy(&real_part, it, sizeof(V));
               it += sizeof(V);
               std::memcpy(&imag_part, it, sizeof(V));
               it += sizeof(V);
               byteswap_le(real_part);
               byteswap_le(imag_part);
               value = std::decay_t<T>{real_part, imag_part};
            }
            else {
               std::memcpy(&value, it, 2 * sizeof(V));
               it += 2 * sizeof(V);
            }
         }
      }
   };

   template <boolean_like T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if ((tag & 0b0000'1111) != tag::boolean) {
            ctx.error = error_code::syntax_error;
            return;
         }

         value = tag >> 4;
         ++it;
      }
   };

   template <is_member_function_pointer T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& /*value*/, is_context auto&& /*ctx*/, auto&& /*it*/,
                                       auto&& /*end*/) noexcept
      {}
   };

   template <func_t T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& /*value*/, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         skip_string_beve(ctx, it, end);
      }
   };

   template <class T>
   struct from<BEVE, basic_raw_json<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         parse<BEVE>::op<Opts>(value.str, ctx, it, end);
      }
   };

   template <class T>
   struct from<BEVE, basic_text<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         parse<BEVE>::op<Opts>(value.str, ctx, it, end);
      }
   };

   template <is_variant T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         constexpr uint8_t header = tag::extensions | 0b00001'000;
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != header) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it;
         const auto type_index = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (value.index() != type_index) {
            emplace_runtime_variant(value, type_index);
         }
         std::visit([&](auto&& v) { parse<BEVE>::op<Opts>(v, ctx, it, end); }, value);
      }
   };

   template <str_t T>
   struct from<BEVE, T> final
   {
      using V = typename std::decay_t<T>::value_type;
      static_assert(sizeof(V) == 1);

      template <auto Opts>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, const uint8_t, is_context auto&& ctx, auto&& it, auto end)
      {
         const auto n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         if (uint64_t(end - it) < n) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         if constexpr (check_max_string_length(Opts) > 0) {
            if (n > check_max_string_length(Opts)) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
         }
         if constexpr (has_runtime_max_string_length<std::decay_t<decltype(ctx)>>) {
            if (ctx.max_string_length > 0 && n > ctx.max_string_length) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
         }
         value.resize(n);
         std::memcpy(value.data(), it, n);
         it += n;
      }

      template <auto Opts>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr uint8_t header = tag::string;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != header) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it;
         const auto n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         if (uint64_t(end - it) < n) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         if constexpr (check_max_string_length(Opts) > 0) {
            if (n > check_max_string_length(Opts)) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
         }
         if constexpr (has_runtime_max_string_length<std::decay_t<decltype(ctx)>>) {
            if (ctx.max_string_length > 0 && n > ctx.max_string_length) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
         }

         if constexpr (string_view_t<T>) {
            value = {it, n};
         }
         else {
            value.resize(n);
            std::memcpy(value.data(), it, n);
         }
         it += n;
      }
   };

   // for set types
   template <class T>
      requires(readable_array_t<T> && !emplace_backable<T> && !resizable<T> && emplaceable<T>)
   struct from<BEVE, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using V = range_value_t<std::decay_t<T>>;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);

         if constexpr (boolean_like<V>) {
            constexpr uint8_t type = uint8_t(3) << 3;
            constexpr uint8_t header = tag::typed_array | type;

            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            value.clear();

            const auto num_bytes = (value.size() + 7) / 8;
            for (size_t byte_i{}, i{}; byte_i < num_bytes; ++byte_i, ++it) {
               if (invalid_end(ctx, it, end)) {
                  return;
               }
               uint8_t byte;
               std::memcpy(&byte, it, 1);
               for (size_t bit_i = 7; bit_i < 8 && i < n; --bit_i, ++i) {
                  bool x = byte >> bit_i & uint8_t(1);
                  value.emplace(x);
               }
            }
         }
         else if constexpr (num_t<V>) {
            constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t header = tag::typed_array | type | (byte_count<V> << 5);

            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            value.clear();

            for (size_t i = 0; i < n; ++i) {
               if ((it + sizeof(V)) > end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               V x;
               std::memcpy(&x, it, sizeof(V));
               if constexpr (std::endian::native == std::endian::big) {
                  byteswap_le(x);
               }
               it += sizeof(V);
               value.emplace(x);
            }
         }
         else if constexpr (str_t<V>) {
            constexpr uint8_t type = uint8_t(3) << 3;
            constexpr uint8_t string_indicator = uint8_t(1) << 5;
            constexpr uint8_t header = tag::typed_array | type | string_indicator;

            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (uint64_t(end - it) < n) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            value.clear();

            for (size_t i = 0; i < n; ++i) {
               const auto length = int_from_compressed(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if (uint64_t(end - it) < length) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               V str;
               str.resize(length);
               std::memcpy(str.data(), it, length);
               it += length;
               value.emplace(std::move(str));
            }
         }
         else if constexpr (complex_t<V>) {
            static_assert(false_v<T>, "TODO");
         }
         else {
            // generic array
            if ((tag & 0b00000'111) != tag::generic_array) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            value.clear();

            for (size_t i = 0; i < n; ++i) {
               V v;
               parse<BEVE>::op<Opts>(v, ctx, it, end);
               value.emplace(std::move(v));
            }
         }
      }
   };

   template <readable_array_t T>
   struct from<BEVE, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using V = range_value_t<std::decay_t<T>>;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);

         if constexpr (boolean_like<V>) {
            constexpr uint8_t type = uint8_t(3) << 3;
            constexpr uint8_t header = tag::typed_array | type;

            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            const auto num_bytes = (n + 7) / 8;
            if (uint64_t(end - it) < num_bytes) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
            if constexpr (check_max_array_size(Opts) > 0) {
               if (n > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && n > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            if constexpr (resizable<T>) {
               value.resize(n);

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }

            for (size_t byte_i{}, i{}; byte_i < num_bytes; ++byte_i, ++it) {
               if (invalid_end(ctx, it, end)) {
                  return;
               }
               uint8_t byte;
               std::memcpy(&byte, it, 1);
               for (size_t bit_i = 7; bit_i < 8 && i < n; --bit_i, ++i) {
                  value[i] = byte >> bit_i & uint8_t(1);
               }
            }
         }
         else if constexpr (num_t<V>) {
            constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t header = tag::typed_array | type | (byte_count<V> << 5);

            auto prepare = [&](const size_t element_size) -> size_t {
               ++it;
               if (invalid_end(ctx, it, end)) {
                  return 0;
               }

               std::conditional_t<Opts.partial_read, size_t, const size_t> n = int_from_compressed(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return 0;
               }

               if constexpr (Opts.partial_read) {
                  n = value.size();
               }

               if ((it + n * element_size) > end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return 0;
               }
               if constexpr (check_max_array_size(Opts) > 0) {
                  if (n > check_max_array_size(Opts)) [[unlikely]] {
                     ctx.error = error_code::invalid_length;
                     return 0;
                  }
               }
               if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
                  if (ctx.max_array_size > 0 && n > ctx.max_array_size) [[unlikely]] {
                     ctx.error = error_code::invalid_length;
                     return 0;
                  }
               }

               if constexpr (resizable<T>) {
                  value.resize(n);

                  if constexpr (check_shrink_to_fit(Opts)) {
                     value.shrink_to_fit();
                  }
               }
               else {
                  if (n > value.size()) {
                     ctx.error = error_code::syntax_error;
                     return 0;
                  }
               }

               return n;
            };

            if (tag != header) {
               if constexpr (check_allow_conversions(Opts)) {
                  if (tag != header) [[unlikely]] {
                     if constexpr (check_allow_conversions(Opts)) {
                        if ((tag & 0b00000111) != tag::typed_array) {
                           ctx.error = error_code::syntax_error;
                           return;
                        }

                        const uint8_t byte_count = byte_count_lookup[tag >> 5];
                        prepare(byte_count);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }

                        for (auto&& x : value) {
                           const uint8_t number_tag = tag::number | (tag & 0b11111000);
                           parse<BEVE>::op<no_header_on<Opts>()>(x, number_tag, ctx, it, end);
                        }
                        return;
                     }
                     else {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                  }
               }
               else {
                  ctx.error = error_code::syntax_error;
                  return;
               }
            }

            const auto n = prepare(sizeof(V));
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (contiguous<T>) {
               constexpr auto is_volatile =
                  std::is_volatile_v<std::remove_reference_t<std::remove_pointer_t<decltype(value.data())>>>;

               if constexpr (is_volatile) {
                  for (size_t i = 0; i < n; ++i) {
                     if ((it + sizeof(V)) > end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }

                     V temp;
                     std::memcpy(&temp, it, sizeof(V));
                     if constexpr (std::endian::native == std::endian::big) {
                        byteswap_le(temp);
                     }
                     value[i] = temp;
                     it += sizeof(V);
                  }
               }
               else if constexpr (std::endian::native == std::endian::big && sizeof(V) > 1) {
                  // On big endian, read and swap each element
                  if ((it + n * sizeof(V)) > end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  for (size_t i = 0; i < n; ++i) {
                     std::memcpy(&value[i], it, sizeof(V));
                     byteswap_le(value[i]);
                     it += sizeof(V);
                  }
               }
               else {
                  // Little endian or single-byte: bulk memcpy
                  if ((it + n * sizeof(V)) > end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  std::memcpy(value.data(), it, n * sizeof(V));
                  it += n * sizeof(V);
               }
            }
            else {
               for (auto&& x : value) {
                  if ((it + sizeof(V)) > end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  std::memcpy(&x, it, sizeof(V));
                  if constexpr (std::endian::native == std::endian::big) {
                     byteswap_le(x);
                  }
                  it += sizeof(V);
               }
            }
         }
         else if constexpr (str_t<V>) {
            constexpr uint8_t type = uint8_t(3) << 3;
            constexpr uint8_t string_indicator = uint8_t(1) << 5;
            constexpr uint8_t header = tag::typed_array | type | string_indicator;

            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            std::conditional_t<Opts.partial_read, size_t, const size_t> n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (Opts.partial_read) {
               n = value.size();
            }

            // Each string needs at least 1 byte for length header
            if (uint64_t(end - it) < n) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
            if constexpr (check_max_array_size(Opts) > 0) {
               if (n > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && n > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            if constexpr (resizable<T>) {
               value.resize(n);

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }

            for (auto&& x : value) {
               const auto length = int_from_compressed(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if (uint64_t(end - it) < length) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               if constexpr (check_max_string_length(Opts) > 0) {
                  if (length > check_max_string_length(Opts)) [[unlikely]] {
                     ctx.error = error_code::invalid_length;
                     return;
                  }
               }
               if constexpr (has_runtime_max_string_length<std::decay_t<decltype(ctx)>>) {
                  if (ctx.max_string_length > 0 && length > ctx.max_string_length) [[unlikely]] {
                     ctx.error = error_code::invalid_length;
                     return;
                  }
               }

               x.resize(length);

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }

               std::memcpy(x.data(), it, length);
               it += length;
            }
         }
         else if constexpr (complex_t<V>) {
            constexpr uint8_t header = tag::extensions | 0b00011'000;
            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it;
            if (invalid_end(ctx, it, end)) {
               return;
            }

            using X = typename V::value_type;
            constexpr uint8_t complex_array = 1;
            constexpr uint8_t type = std::floating_point<X> ? 0 : (std::is_signed_v<X> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t complex_header = complex_array | type | (byte_count<X> << 5);
            const auto complex_tag = uint8_t(*it);
            if (complex_tag != complex_header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it;
            std::conditional_t<Opts.partial_read, size_t, const size_t> n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (Opts.partial_read) {
               n = value.size();
            }

            if (uint64_t(end - it) < n * sizeof(V)) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
            if constexpr (check_max_array_size(Opts) > 0) {
               if (n > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && n > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            if constexpr (resizable<T>) {
               value.resize(n);

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }

            if constexpr (contiguous<T>) {
               if constexpr (std::endian::native == std::endian::big && sizeof(X) > 1) {
                  // On big endian, read and swap each complex element's components
                  for (size_t i = 0; i < n; ++i) {
                     std::memcpy(&value[i], it, sizeof(V));
                     X real_part = value[i].real();
                     X imag_part = value[i].imag();
                     byteswap_le(real_part);
                     byteswap_le(imag_part);
                     value[i] = V(real_part, imag_part);
                     it += sizeof(V);
                  }
               }
               else {
                  // Little endian or single-byte: bulk memcpy
                  std::memcpy(value.data(), it, n * sizeof(V));
                  it += n * sizeof(V);
               }
            }
            else {
               for (auto&& x : value) {
                  std::memcpy(&x, it, sizeof(V));
                  if constexpr (std::endian::native == std::endian::big && sizeof(X) > 1) {
                     X real_part = x.real();
                     X imag_part = x.imag();
                     byteswap_le(real_part);
                     byteswap_le(imag_part);
                     x = V(real_part, imag_part);
                  }
                  it += sizeof(V);
               }
            }
         }
         else {
            if ((tag & 0b00000'111) != tag::generic_array) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it;
            std::conditional_t<check_partial_read(Opts), size_t, const size_t> n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (Opts.partial_read) {
               n = value.size();
            }

            // Each element needs at least 1 byte for its tag
            if (uint64_t(end - it) < n) [[unlikely]] {
               ctx.error = error_code::invalid_length;
               return;
            }
            if constexpr (check_max_array_size(Opts) > 0) {
               if (n > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && n > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            if constexpr (resizable<T>) {
               value.resize(n);

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }

            for (auto&& item : value) {
               parse<BEVE>::op<Opts>(item, ctx, it, end);
            }
         }
      }

      // for types like std::vector<std::pair...> that can't look up with operator[]
      // Instead of hashing or linear searching, we just clear the input and overwrite the entire contents
      template <auto Opts>
         requires(pair_t<range_value_t<T>> && check_concatenate(Opts) == true)
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using Element = typename T::value_type;
         using Key = typename Element::first_type;

         constexpr uint8_t header = beve_key_traits<Key>::header;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != header) [[unlikely]] {
            if constexpr (check_allow_conversions(Opts)) {
               const auto key_type = tag & 0b000'11'000;
               if constexpr (beve_key_traits<Key>::as_string) {
                  if (key_type != 0) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else {
                  if (key_type == 0) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }

         ++it;
         const size_t n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         value.clear();

         constexpr uint8_t key_tag = beve_key_traits<Key>::key_tag;
         for (size_t i = 0; i < n; ++i) {
            auto& item = value.emplace_back();
            parse<BEVE>::op<no_header_on<Opts>()>(item.first, key_tag, ctx, it, end);
            parse<BEVE>::op<Opts>(item.second, ctx, it, end);
         }
      }
   };

   template <pair_t T>
   struct from<BEVE, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(T& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using Key = typename T::first_type;

         constexpr uint8_t header = beve_key_traits<Key>::header;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != header) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it;
         const auto n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (n != 1) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         constexpr uint8_t key_tag = beve_key_traits<Key>::key_tag;
         parse<BEVE>::op<no_header_on<Opts>()>(value.first, key_tag, ctx, it, end);
         parse<BEVE>::op<Opts>(value.second, ctx, it, end);
      }
   };

   template <readable_map_t T>
   struct from<BEVE, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using Key = typename T::key_type;

         constexpr uint8_t header = beve_key_traits<Key>::header;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != header) [[unlikely]] {
            if constexpr (check_allow_conversions(Opts)) {
               const auto key_type = tag & 0b000'11'000;
               if constexpr (beve_key_traits<Key>::as_string) {
                  if (key_type != 0) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else {
                  if (key_type == 0) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }

         ++it;
         std::conditional_t<Opts.partial_read, size_t, const size_t> n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if constexpr (Opts.partial_read) {
            n = value.size();
         }
         else {
            // Validate count against remaining buffer size (minimum 1 byte per key-value pair)
            if (n > size_t(end - it)) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check user-configured map size limit
            if constexpr (check_max_map_size(Opts) > 0) {
               if (n > check_max_map_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_map_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_map_size > 0 && n > ctx.max_map_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
         }

         constexpr uint8_t key_tag = beve_key_traits<Key>::key_tag;

         if constexpr (beve_key_traits<Key>::as_number) {
            Key key{}; // Value-initialize to silence false positive -Wmaybe-uninitialized
            for (size_t i = 0; i < n; ++i) {
               if constexpr (Opts.partial_read) {
                  parse<BEVE>::op<no_header_on<Opts>()>(key, key_tag, ctx, it, end);
                  if (auto element = value.find(key); element != value.end()) {
                     parse<BEVE>::op<Opts>(element->second, ctx, it, end);
                  }
               }
               else {
                  // convert the object tag to the key type tag
                  parse<BEVE>::op<no_header_on<Opts>()>(key, key_tag, ctx, it, end);
                  parse<BEVE>::op<Opts>(value[key], ctx, it, end);
               }
            }
         }
         else {
            static thread_local Key key;
            for (size_t i = 0; i < n; ++i) {
               if constexpr (Opts.partial_read) {
                  parse<BEVE>::op<no_header_on<Opts>()>(key, key_tag, ctx, it, end);
                  if (auto element = value.find(key); element != value.end()) {
                     parse<BEVE>::op<Opts>(element->second, ctx, it, end);
                  }
               }
               else {
                  parse<BEVE>::op<no_header_on<Opts>()>(key, key_tag, ctx, it, end);
                  parse<BEVE>::op<Opts>(value[key], ctx, it, end);
               }
            }
         }
      }
   };

   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct from<BEVE, T> final
   {
      template <auto Opts, class V, size_t N>
      GLZ_ALWAYS_INLINE static void op(V (&value)[N], is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         parse<BEVE>::op<Opts>(std::span{value, N}, ctx, it, end);
      }
   };

   template <nullable_t T>
      requires(!std::is_array_v<T>)
   struct from<BEVE, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);

         if (tag == tag::null) {
            ++it;
            if constexpr (is_specialization_v<T, std::optional>)
               value = std::nullopt;
            else if constexpr (is_specialization_v<T, std::unique_ptr>)
               value = nullptr;
            else if constexpr (is_specialization_v<T, std::shared_ptr>)
               value = nullptr;
         }
         else {
            if (!value) {
               if constexpr (is_specialization_v<T, std::optional>)
                  value = std::make_optional<typename T::value_type>();
               else if constexpr (is_specialization_v<T, std::unique_ptr>)
                  value = std::make_unique<typename T::element_type>();
               else if constexpr (is_specialization_v<T, std::shared_ptr>)
                  value = std::make_shared<typename T::element_type>();
               else if constexpr (constructible<T>) {
                  value = meta_construct_v<T>();
               }
               else if constexpr (std::is_pointer_v<T> && can_allocate_raw_pointer<Opts, std::decay_t<decltype(ctx)>>) {
                  if (!try_allocate_raw_pointer<Opts>(value, ctx)) {
                     return;
                  }
               }
               else {
                  ctx.error = error_code::invalid_nullable_read;
                  return;
                  // Cannot read into unset nullable that is not std::optional, std::unique_ptr, or std::shared_ptr
               }
            }
            parse<BEVE>::op<Opts>(*value, ctx, it, end);
         }
      }
   };

   template <class T>
      requires(nullable_value_t<T> && not nullable_like<T> && not is_expected<T> && not custom_read<T>)
   struct from<BEVE, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);

         if (tag == tag::null) {
            ++it;
            if constexpr (requires { value.reset(); }) {
               value.reset();
            }
         }
         else {
            if (not value.has_value()) {
               if constexpr (constructible<T>) {
                  value = meta_construct_v<T>();
               }
               else if constexpr (requires { value.emplace(); }) {
                  value.emplace();
               }
               else {
                  ctx.error = error_code::invalid_nullable_read;
                  return;
               }
            }
            parse<BEVE>::op<Opts>(value.value(), ctx, it, end);
         }
      }
   };

   template <is_includer T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (check_no_header(Opts)) {
            skip_compressed_int(ctx, it, end);
         }
         else {
            constexpr uint8_t header = tag::string;

            if (invalid_end(ctx, it, end)) {
               return;
            }
            const auto tag = uint8_t(*it);
            if (tag != header) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            ++it;
            skip_compressed_int(ctx, it, end);
         }
      }
   };

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_read<T>)
   struct from<BEVE, T> final
   {
      template <auto Opts>
         requires(check_structs_as_arrays(Opts) == true)
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if constexpr (reflectable<T>) {
            constexpr auto N = detail::count_members<T>;
            if constexpr (N == 0) {
               // Handle empty structs by just reading and validating the generic_array header
               const auto tag = uint8_t(*it);
               if (tag != tag::generic_array) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }
               ++it;
               const auto n = int_from_compressed(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if (n != 0) {
                  ctx.error = error_code::syntax_error;
                  return;
               }
            }
            else {
               auto t = to_tie(value);
               parse<BEVE>::op<Opts>(t, ctx, it, end);
            }
         }
         else {
            const auto tag = uint8_t(*it);
            if (tag != tag::generic_array) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }
            ++it;
            using V = std::decay_t<T>;
            constexpr auto N = reflect<V>::size;
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (n != N) {
               ctx.error = error_code::syntax_error;
               return;
            }

            for_each<N>(
               [&]<size_t I>() { parse<BEVE>::op<Opts>(get_member(value, get<I>(reflect<V>::values)), ctx, it, end); });
         }
      }

      template <auto Opts>
         requires(check_structs_as_arrays(Opts) == false)
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr uint8_t type = 0; // string key
         constexpr uint8_t header = tag::object | type;

         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != header) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         ++it;

         static constexpr auto N = reflect<T>::size;

         static constexpr bit_array<N> all_fields = [] {
            bit_array<N> arr{};
            for (size_t i = 0; i < N; ++i) {
               arr[i] = true;
            }
            return arr;
         }();

         decltype(auto) fields = [&]() -> decltype(auto) {
            if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
               return bit_array<N>{};
            }
            else {
               return nullptr;
            }
         }();

         const auto n_keys = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         for (size_t i = 0; i < n_keys; ++i) {
            if constexpr (Opts.partial_read) {
               if ((all_fields & fields) == all_fields) {
                  return;
               }
            }

            if constexpr (N > 0) {
               static constexpr auto HashInfo = hash_info<T>;

               const auto n = int_from_compressed(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if (uint64_t(end - it) < n || it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               const auto index = decode_hash_with_size<BEVE, T, HashInfo, HashInfo.type>::op(it, end, n);

               if (index < N) [[likely]] {
                  if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
                     fields[index] = true;
                  }

                  const sv key{it, n};
                  it += n;

                  visit<N>(
                     [&]<size_t I>() {
                        static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                        static constexpr auto Length = TargetKey.size();
                        if ((Length == n) && compare<Length>(TargetKey.data(), key.data())) [[likely]] {
                           // Check for null value skipping on read
                           if constexpr (check_skip_null_members_on_read(Opts)) {
                              if (invalid_end(ctx, it, end)) {
                                 return;
                              }
                              if (uint8_t(*it) == tag::null) {
                                 ++it; // Skip the null tag
                                 return;
                              }
                           }

                           if constexpr (reflectable<T>) {
                              parse<BEVE>::op<Opts>(get_member(value, get<I>(to_tie(value))), ctx, it, end);
                           }
                           else {
                              parse<BEVE>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, it, end);
                           }
                        }
                        else {
                           if constexpr (Opts.error_on_unknown_keys) {
                              ctx.error = error_code::unknown_key;
                              return;
                           }
                           else {
                              skip_value<BEVE>::op<Opts>(ctx, it, end);
                              if (bool(ctx.error)) [[unlikely]]
                                 return;
                           }
                        }
                     },
                     index);

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }
               else [[unlikely]] {
                  if constexpr (Opts.error_on_unknown_keys) {
                     ctx.error = error_code::unknown_key;
                     return;
                  }
                  else {
                     it += n;
                     skip_value<BEVE>::op<Opts>(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
               }
            }
            else if constexpr (Opts.error_on_unknown_keys) {
               ctx.error = error_code::unknown_key;
               return;
            }
            else {
               skip_value<BEVE>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
         }

         if constexpr (Opts.error_on_missing_keys) {
            constexpr auto req_fields = required_fields<T, Opts>();
            if ((req_fields & fields) != req_fields) {
               for (size_t i = 0; i < N; ++i) {
                  if (not fields[i] && req_fields[i]) {
                     ctx.custom_error_message = reflect<T>::keys[i];
                     break;
                  }
               }
               ctx.error = error_code::missing_key;
               return;
            }
         }
      }
   };

   template <class T>
      requires glaze_array_t<T>
   struct from<BEVE, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != tag::generic_array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;

         constexpr auto N = reflect<T>::size;
         const auto n = int_from_compressed(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         if (n != N) {
            ctx.error = error_code::syntax_error;
            return;
         }

         for_each<N>(
            [&]<size_t I>() { parse<BEVE>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, it, end); });
      }
   };

   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct from<BEVE, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (invalid_end(ctx, it, end)) {
            return;
         }
         const auto tag = uint8_t(*it);
         if (tag != tag::generic_array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;

         using V = std::decay_t<T>;
         constexpr auto N = glz::tuple_size_v<V>;
         if constexpr (Opts.partial_read) {
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (is_std_tuple<T>) {
               for_each_short_circuit<N>([&]<auto I>() {
                  if (I < n) {
                     parse<BEVE>::op<Opts>(std::get<I>(value), ctx, it, end);
                     return false; // continue
                  }
                  return true; // short circuit
               });
            }
            else {
               for_each_short_circuit<N>([&]<auto I>() {
                  if (I < n) {
                     parse<BEVE>::op<Opts>(glz::get<I>(value), ctx, it, end);
                     return false; // continue
                  }
                  return true; // short circuit
               });
            }
         }
         else {
            const auto n = int_from_compressed(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (n != N) {
               ctx.error = error_code::syntax_error;
               return;
            }

            if constexpr (is_std_tuple<T>) {
               for_each<N>([&]<size_t I>() { parse<BEVE>::op<Opts>(std::get<I>(value), ctx, it, end); });
            }
            else {
               for_each<N>([&]<size_t I>() { parse<BEVE>::op<Opts>(glz::get<I>(value), ctx, it, end); });
            }
         }
      }
   };

   template <filesystem_path T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         static thread_local std::string buffer{};
         parse<BEVE>::op<Opts>(buffer, ctx, args...);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         value = buffer;
      }
   };

   template <read_supported<BEVE> T, class Buffer>
   [[nodiscard]] inline error_ctx read_beve(T&& value, Buffer&& buffer)
   {
      return read<opts{.format = BEVE}>(value, std::forward<Buffer>(buffer));
   }

   template <read_supported<BEVE> T, class Buffer>
   [[nodiscard]] inline expected<T, error_ctx> read_beve(Buffer&& buffer)
   {
      T value{};
      const auto pe = read<opts{.format = BEVE}>(value, std::forward<Buffer>(buffer));
      if (pe) [[unlikely]] {
         return unexpected(pe);
      }
      return value;
   }

   template <auto Opts = opts{}, read_supported<BEVE> T>
   [[nodiscard]] inline error_ctx read_file_beve(T& value, const sv file_name, auto&& buffer)
   {
      context ctx{};
      ctx.current_file = file_name;

      const auto file_error = file_to_buffer(buffer, ctx.current_file);

      if (bool(file_error)) [[unlikely]] {
         return error_ctx{0, file_error};
      }

      return read<set_beve<Opts>()>(value, buffer, ctx);
   }

   template <read_supported<BEVE> T, class Buffer>
   [[deprecated("Use read_beve_untagged instead")]] [[nodiscard]] inline error_ctx read_binary_untagged(T&& value,
                                                                                                        Buffer&& buffer)
   {
      return read<opt_true<opts{.format = BEVE}, structs_as_arrays_opt_tag{}>>(std::forward<T>(value),
                                                                               std::forward<Buffer>(buffer));
   }

   template <read_supported<BEVE> T, class Buffer>
   [[deprecated("Use read_beve_untagged instead")]] [[nodiscard]] inline expected<T, error_ctx> read_binary_untagged(
      Buffer&& buffer)
   {
      T value{};
      const auto pe =
         read<opt_true<opts{.format = BEVE}, structs_as_arrays_opt_tag{}>>(value, std::forward<Buffer>(buffer));
      if (pe) [[unlikely]] {
         return unexpected(pe);
      }
      return value;
   }

   // read_beve_untagged aliases for naming consistency with write_beve_untagged
   template <read_supported<BEVE> T, class Buffer>
   [[nodiscard]] inline error_ctx read_beve_untagged(T&& value, Buffer&& buffer)
   {
      return read<opt_true<opts{.format = BEVE}, structs_as_arrays_opt_tag{}>>(std::forward<T>(value),
                                                                               std::forward<Buffer>(buffer));
   }

   template <read_supported<BEVE> T, class Buffer>
   [[nodiscard]] inline expected<T, error_ctx> read_beve_untagged(Buffer&& buffer)
   {
      T value{};
      const auto pe =
         read<opt_true<opts{.format = BEVE}, structs_as_arrays_opt_tag{}>>(value, std::forward<Buffer>(buffer));
      if (pe) [[unlikely]] {
         return unexpected(pe);
      }
      return value;
   }

   template <auto Opts = opts{}, read_supported<BEVE> T>
   [[nodiscard]] inline error_ctx read_file_beve_untagged(T& value, const std::string& file_name, auto&& buffer)
   {
      return read_file_beve<opt_true<Opts, structs_as_arrays_opt_tag{}>>(value, file_name, buffer);
   }

   // ===== Delimited BEVE support for multiple objects in one buffer =====

   // Skip a delimiter byte if present at the current position
   // Returns true if a delimiter was skipped, false otherwise
   GLZ_ALWAYS_INLINE bool skip_beve_delimiter(auto&& it, auto end) noexcept
   {
      if (it < end && uint8_t(*it) == tag::delimiter) {
         ++it;
         return true;
      }
      return false;
   }

   // Read multiple delimiter-separated BEVE values from a buffer into a container
   template <auto Opts = opts{}, class Container, class Buffer>
      requires readable_array_t<Container> && (emplace_backable<Container> || !resizable<Container>)
   [[nodiscard]] error_ctx read_beve_delimited(Container& values, Buffer&& buffer)
   {
      static_assert(sizeof(decltype(*buffer.data())) == 1);

      if (buffer.empty()) {
         if constexpr (resizable<Container>) {
            values.clear();
         }
         return {};
      }

      context ctx{};
      auto it = reinterpret_cast<const char*>(buffer.data());
      auto end = it + buffer.size();
      auto start = it;

      if constexpr (resizable<Container>) {
         values.clear();
      }

      size_t index = 0;
      const size_t container_size = values.size();

      while (it < end) {
         // Skip delimiter if present (except before first value)
         if (index > 0) {
            skip_beve_delimiter(it, end);
         }

         if (it >= end) {
            break;
         }

         if constexpr (emplace_backable<Container>) {
            auto& value = values.emplace_back();
            parse<BEVE>::template op<set_beve<Opts>()>(value, ctx, it, end);
         }
         else {
            if (index >= container_size) {
               ctx.error = error_code::exceeded_static_array_size;
               break;
            }
            parse<BEVE>::template op<set_beve<Opts>()>(values[index], ctx, it, end);
         }

         if (bool(ctx.error)) [[unlikely]] {
            return {size_t(it - start), ctx.error, ctx.custom_error_message};
         }

         ++index;
      }

      return {size_t(it - start), ctx.error, ctx.custom_error_message};
   }

   // Read multiple delimiter-separated BEVE values, returning the container
   template <class Container, auto Opts = opts{}, class Buffer>
      requires readable_array_t<Container> && (emplace_backable<Container> || !resizable<Container>)
   [[nodiscard]] expected<Container, error_ctx> read_beve_delimited(Buffer&& buffer)
   {
      Container values{};
      const auto ec = read_beve_delimited<Opts>(values, std::forward<Buffer>(buffer));
      if (bool(ec)) [[unlikely]] {
         return unexpected(ec);
      }
      return values;
   }

   // Read a single BEVE value from a buffer at a given offset.
   // If a delimiter byte (0x06) is present at the offset, it is automatically skipped.
   // Returns the total bytes consumed from offset (including any skipped delimiter),
   // so the next read offset is simply: offset + *result
   //
   // Example:
   //   size_t offset = 0;
   //   while (offset < buffer.size()) {
   //      auto result = glz::read_beve_at(value, buffer, offset);
   //      if (!result) break;
   //      offset += *result;  // correctly advances past delimiter + value
   //   }
   //
   // Overload with context for runtime options (e.g., max_string_length, max_array_size):
   //   my_context ctx{};
   //   ctx.max_string_length = 1024;
   //   auto result = glz::read_beve_at<glz::opts{}>(value, buffer, offset, ctx);
   template <auto Opts = opts{}, read_supported<BEVE> T, class Buffer>
   [[nodiscard]] glz::expected<size_t, error_ctx> read_beve_at(T& value, Buffer&& buffer, size_t offset,
                                                               is_context auto&& ctx)
   {
      static_assert(sizeof(decltype(*buffer.data())) == 1);

      if (offset >= buffer.size()) {
         return glz::unexpected(error_ctx{0, error_code::unexpected_end});
      }

      auto it = reinterpret_cast<const char*>(buffer.data()) + offset;
      auto end = reinterpret_cast<const char*>(buffer.data()) + buffer.size();
      auto start = it;

      // Skip leading delimiter if present (included in returned byte count)
      skip_beve_delimiter(it, end);

      if (it >= end) {
         return glz::unexpected(error_ctx{0, error_code::unexpected_end});
      }

      parse<BEVE>::template op<set_beve<Opts>()>(value, ctx, it, end);

      if (bool(ctx.error)) [[unlikely]] {
         return glz::unexpected(error_ctx{size_t(it - start), ctx.error, ctx.custom_error_message});
      }

      return size_t(it - start); // total bytes consumed from offset (delimiter + value)
   }

   template <auto Opts = opts{}, read_supported<BEVE> T, class Buffer>
   [[nodiscard]] glz::expected<size_t, error_ctx> read_beve_at(T& value, Buffer&& buffer, size_t offset = 0)
   {
      context ctx{};
      return read_beve_at<Opts>(value, std::forward<Buffer>(buffer), offset, ctx);
   }
}
