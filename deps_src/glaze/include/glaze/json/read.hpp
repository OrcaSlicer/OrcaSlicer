// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <charconv>
#include <climits>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <type_traits>

#include "glaze/core/chrono.hpp"
#include "glaze/core/common.hpp"
#include "glaze/core/custom_meta.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/file/file_ops.hpp"
#include "glaze/json/json_concepts.hpp"
#include "glaze/json/skip.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/glaze_fast_float.hpp"
#include "glaze/util/simple_float.hpp"
#include "glaze/util/type_traits.hpp"
#include "glaze/util/variant.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
// Turn off MSVC warning for unreachable code due to constexpr branching
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

namespace glz
{
   // forward declare from json/wrappers.hpp to avoid circular include
   template <class T>
   struct quoted_t;

   // Note: custom_num_t, custom_str_t, custom_bool_t concepts are defined in core/custom_meta.hpp

   template <>
   struct parse<JSON>
   {
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         if constexpr (const_value_v<T>) {
            if constexpr (check_error_on_const_read(Opts)) {
               ctx.error = error_code::attempt_const_read;
            }
            else {
               // do not read anything into the const value
               skip_value<JSON>::op<Opts>(std::forward<Ctx>(ctx), std::forward<It0>(it), end);
            }
         }
         else {
            using V = std::remove_cvref_t<T>;
            from<JSON, V>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it),
                                             end);
         }
      }

      // This unknown key handler should not be given unescaped keys, that is for the user to handle.
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
      static void handle_unknown(const sv& key, T&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using ValueType = std::decay_t<decltype(value)>;
         if constexpr (has_unknown_reader<ValueType>) {
            constexpr auto& reader = meta_unknown_read_v<ValueType>;
            using ReaderType = meta_unknown_read_t<ValueType>;
            if constexpr (std::is_member_object_pointer_v<ReaderType>) {
               using MemberType = typename member_value<ReaderType>::type;
               if constexpr (map_subscriptable<MemberType>) {
                  // Convert the `sv` key to the map's key_type when needed
                  using Key = typename MemberType::key_type;
                  if constexpr (std::is_same_v<Key, sv>) {
                     parse<JSON>::op<Opts>((value.*reader)[key], ctx, it, end);
                  }
                  else if constexpr (std::is_constructible_v<Key, sv>) {
                     parse<JSON>::op<Opts>((value.*reader)[Key{key}], ctx, it, end);
                  }
                  else if constexpr (std::is_constructible_v<Key, const char*, size_t>) {
                     parse<JSON>::op<Opts>((value.*reader)[Key{key.data(), key.size()}], ctx, it, end);
                  }
                  else if constexpr (std::is_constructible_v<Key, std::string_view>) {
                     parse<JSON>::op<Opts>((value.*reader)[Key{std::string_view{key}}], ctx, it, end);
                  }
                  else {
                     static_assert(false_v<T>, "unknown_read key type not handled");
                  }
               }
               else {
                  static_assert(false_v<T>, "target must have subscript operator");
               }
            }
            else if constexpr (std::is_member_function_pointer_v<ReaderType>) {
               using ReturnType = typename return_type<ReaderType>::type;
               if constexpr (std::is_void_v<ReturnType>) {
                  using TupleType = typename inputs_as_tuple<ReaderType>::type;
                  if constexpr (glz::tuple_size_v<TupleType> == 2) {
                     std::decay_t<glz::tuple_element_t<1, TupleType>> input{};
                     parse<JSON>::op<Opts>(input, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     // Convert `sv` key to the method's first parameter type when needed
                     using KeyParam = std::decay_t<glz::tuple_element_t<0, TupleType>>;
                     if constexpr (std::is_same_v<KeyParam, sv>) {
                        (value.*reader)(key, input);
                     }
                     else if constexpr (std::is_constructible_v<KeyParam, sv>) {
                        (value.*reader)(KeyParam{key}, input);
                     }
                     else if constexpr (std::is_constructible_v<KeyParam, const char*, size_t>) {
                        (value.*reader)(KeyParam{key.data(), key.size()}, input);
                     }
                     else if constexpr (std::is_constructible_v<KeyParam, std::string_view>) {
                        (value.*reader)(KeyParam{std::string_view{key}}, input);
                     }
                     else {
                        static_assert(false_v<T>, "unknown_read key parameter type not handled");
                     }
                  }
                  else {
                     static_assert(false_v<T>, "method must have 2 args");
                  }
               }
               else {
                  static_assert(false_v<T>, "method must have void return");
               }
            }
            else {
               static_assert(false_v<T>, "unknown_read type not handled");
            }
         }
         else {
            skip_value<JSON>::op<Opts>(ctx, it, end);
         }
      }
   };

   // Unless we can mutate the input buffer we need somewhere to store escaped strings for key lookup, etc.
   // We don't put this in the context because we don't want to continually reallocate.
   // IMPORTANT: Do not call use this when nested calls may be made that need additional buffers on the same thread.
   inline std::string& string_buffer() noexcept
   {
      static thread_local std::string buffer(256, '\0');
      return buffer;
   }

   // We use an error buffer to avoid multiple allocations in the case that errors occur multiple times.
   inline std::string& error_buffer() noexcept
   {
      static thread_local std::string buffer(256, '\0');
      return buffer;
   }

   template <class T>
      requires(glaze_value_t<T> && !custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class Value, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = std::decay_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         from<JSON, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                          std::forward<Ctx>(ctx), std::forward<It0>(it), end);
      }
   };

   template <auto Opts>
   GLZ_ALWAYS_INLINE bool parse_ws_colon(is_context auto& ctx, auto&& it, auto end) noexcept
   {
      if (skip_ws<Opts>(ctx, it, end)) {
         return true;
      }
      if (match_invalid_end<':', Opts>(ctx, it, end)) {
         return true;
      }
      if (skip_ws<Opts>(ctx, it, end)) {
         return true;
      }
      return false;
   }

   // Extracted from decode_index to reduce code duplication across instantiations.
   // This path handles unknown keys and doesn't depend on the field index I.
   template <auto Opts, class T, class Value>
      requires(glaze_object_t<T> || reflectable<T>)
   void decode_index_unknown_key(Value&& value, is_context auto&& ctx, auto&& it, auto end)
   {
      if constexpr (Opts.error_on_unknown_keys) {
         ctx.error = error_code::unknown_key;
      }
      else {
         auto* start = it;
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         const sv key = {start, size_t(it - start)};
         ++it;
         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }

         if (parse_ws_colon<Opts>(ctx, it, end)) {
            return;
         }

         parse<JSON>::handle_unknown<Opts>(key, value, ctx, it, end);
      }
   }

   // Linear search for key matching - eliminates hash tables for smaller binaries
   // Returns the index of the matching key, or N if not found
   // Advances it past the key and closing quote
   template <class T>
   GLZ_ALWAYS_INLINE size_t decode_linear(is_context auto&& ctx, auto&& it, auto&& end)
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto& keys = reflect<T>::keys;

      auto key_start = it;
      skip_string_view(ctx, it, end);
      if (bool(ctx.error)) [[unlikely]]
         return N;
      const sv key{key_start, size_t(it - key_start)};
      ++it; // skip closing quote

      // Linear search through known keys
      for (size_t i = 0; i < N; ++i) {
         if (keys[i] == key) {
            return i;
         }
      }

      return N; // not found
   }

   template <auto Opts, class T, size_t I, class Value, class... SelectedIndex>
      requires(glaze_object_t<T> || reflectable<T>)
   void decode_index(Value&& value, is_context auto&& ctx, auto&& it, auto&& end, SelectedIndex&&... selected_index)
   {
      static constexpr auto Key = get<I>(reflect<T>::keys);
      static constexpr auto KeyWithEndQuote = join_v<Key, chars<"\"">>;
      static constexpr auto Length = KeyWithEndQuote.size();

      if (((it + Length) < end) && comparitor<KeyWithEndQuote>(it)) [[likely]] {
         it += Length;
         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
         if (match_invalid_end<':', Opts>(ctx, it, end)) {
            return;
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         // Check for null value skipping on read
         if constexpr (check_skip_null_members_on_read(Opts)) {
            if (*it == 'n') {
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               match<"ull", Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               // Successfully matched "null", skip it
               if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
                  ((selected_index = I), ...); // Mark as handled even if skipped
               }
               return;
            }
         }

         // Check for operation-specific skipping
         if constexpr (meta_has_skip<std::remove_cvref_t<T>>) {
            if constexpr (meta<std::remove_cvref_t<T>>::skip(Key, {glz::operation::parse})) {
               skip_value<JSON>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return; // Propagate error from skip_value
               if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
                  ((selected_index = I), ...); // Mark as handled even if skipped
               }
               return;
            }
         }

         using V = refl_t<T, I>;

         if constexpr (const_value_v<V>) {
            if constexpr (check_error_on_const_read(Opts)) {
               ctx.error = error_code::attempt_const_read;
            }
            else {
               // do not read anything into the const value
               skip_value<JSON>::op<Opts>(ctx, it, end);
            }
         }
         else {
            if constexpr (glaze_object_t<T>) {
               from<JSON, std::remove_cvref_t<V>>::template op<ws_handled<Opts>()>(
                  get_member(value, get<I>(reflect<T>::values)), ctx, it, end);
            }
            else {
               from<JSON, std::remove_cvref_t<V>>::template op<ws_handled<Opts>()>(
                  get_member(value, get<I>(to_tie(value))), ctx, it, end);
            }
         }

         if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
            ((selected_index = I), ...);
         }
      }
      else [[unlikely]] {
         decode_index_unknown_key<Opts, T>(value, ctx, it, end);
      }
   }

   template <auto Opts, class T, size_t I, class Value>
      requires(glaze_enum_t<T> || (meta_keys<T> && std::is_enum_v<T>))
   void decode_index(Value&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      static constexpr auto TargetKey = glz::get<I>(reflect<T>::keys);
      static constexpr auto Length = TargetKey.size();

      if (((it + Length) < end) && comparitor<TargetKey>(it)) [[likely]] {
         it += Length;
         if (*it != '"') [[unlikely]] {
            ctx.error = error_code::unexpected_enum;
            return;
         }
         value = get<I>(reflect<T>::values);

         ++it;
         if constexpr (not Opts.null_terminated) {
            if (it == end) {
               ctx.error = error_code::end_reached;
               return;
            }
         }
      }
      else [[unlikely]] {
         ctx.error = error_code::unexpected_enum;
         return;
      }
   }

   template <auto Opts, class T, class Value, class... SelectedIndex>
      requires(glaze_object_t<T> || reflectable<T>)
   GLZ_ALWAYS_INLINE constexpr void parse_and_invoke(Value&& value, is_context auto&& ctx, auto&& it, auto&& end,
                                                     SelectedIndex&&... selected_index)
   {
      constexpr auto N = reflect<T>::size;

      if constexpr (N == 1) {
         decode_index<Opts, T, 0>(value, ctx, it, end, selected_index...);
      }
      else if constexpr (check_linear_search(Opts)) {
         // Linear search path
         auto key_start = it;
         const auto index = decode_linear<T>(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (index >= N) [[unlikely]] {
            if constexpr (Opts.error_on_unknown_keys) {
               ctx.error = error_code::unknown_key;
               return;
            }
            else {
               const sv key{key_start, size_t(it - key_start - 1)};
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               if (parse_ws_colon<Opts>(ctx, it, end)) {
                  return;
               }
               parse<JSON>::handle_unknown<Opts>(key, value, ctx, it, end);
               return;
            }
         }

         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
         if (match_invalid_end<':', Opts>(ctx, it, end)) {
            return;
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         // Fold expression dispatch - avoids jump table overhead for smaller binary size
         auto parse_field = [&]<size_t I>() {
            if constexpr (check_skip_null_members_on_read(Opts)) {
               if (*it == 'n') {
                  ++it;
                  if constexpr (not Opts.null_terminated) {
                     if (it == end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                  }
                  match<"ull", Opts>(ctx, it, end);
                  if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
                     ((selected_index = I), ...);
                  }
                  return;
               }
            }

            // Check for operation-specific skipping
            if constexpr (meta_has_skip<std::remove_cvref_t<T>>) {
               constexpr auto Key = get<I>(reflect<T>::keys);
               if constexpr (meta<std::remove_cvref_t<T>>::skip(Key, {glz::operation::parse})) {
                  skip_value<JSON>::op<Opts>(ctx, it, end);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
                  if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
                     ((selected_index = I), ...);
                  }
                  return;
               }
            }

            using V = refl_t<T, I>;
            if constexpr (const_value_v<V>) {
               if constexpr (check_error_on_const_read(Opts)) {
                  ctx.error = error_code::attempt_const_read;
               }
               else {
                  skip_value<JSON>::op<Opts>(ctx, it, end);
               }
            }
            else {
               // For linear_search, use Opts directly to avoid duplicate template instantiations
               // We've already skipped whitespace before reaching here
               if constexpr (glaze_object_t<T>) {
                  from<JSON, std::remove_cvref_t<V>>::template op<Opts>(get_member(value, get<I>(reflect<T>::values)),
                                                                        ctx, it, end);
               }
               else {
                  from<JSON, std::remove_cvref_t<V>>::template op<Opts>(get_member(value, get<I>(to_tie(value))), ctx,
                                                                        it, end);
               }
            }
            if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
               ((selected_index = I), ...);
            }
         };
         [&]<size_t... Is>(std::index_sequence<Is...>) {
            (void)(((index == Is ? (parse_field.template operator()<Is>(), true) : false) || ...));
         }(std::make_index_sequence<N>{});
      }
      else {
         // Hash-based lookup
         constexpr auto& HashInfo = hash_info<T>;
         constexpr auto type = HashInfo.type;
         static_assert(bool(type), "invalid hash algorithm");

         const auto index = decode_hash<JSON, T, HashInfo, HashInfo.type>::op(it, end);

         if (index >= N) [[unlikely]] {
            if constexpr (Opts.error_on_unknown_keys) {
               ctx.error = error_code::unknown_key;
               return;
            }
            else {
               auto start = it;
               skip_string_view(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               const sv key = {start, size_t(it - start)};
               ++it; // skip the quote
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               if (parse_ws_colon<Opts>(ctx, it, end)) {
                  return;
               }
               parse<JSON>::handle_unknown<Opts>(key, value, ctx, it, end);
               return;
            }
         }

         if constexpr (N == 2) {
            if (index == 0) {
               decode_index<Opts, T, 0>(value, ctx, it, end, selected_index...);
            }
            else {
               decode_index<Opts, T, 1>(value, ctx, it, end, selected_index...);
            }
         }
         else {
            visit<N>([&]<size_t I>() { decode_index<Opts, T, I>(value, ctx, it, end, selected_index...); }, index);
         }
      }
   }

   template <is_member_function_pointer T>
   struct from<JSON, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&...) noexcept
      {
         ctx.error = error_code::attempt_member_func_read;
      }
   };

   template <is_includer T>
   struct from<JSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&&, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         match<R"("")", Opts>(ctx, it, end);
      }
   };

   template <is_bitset T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (match_invalid_end<'"', Opts>(ctx, it, end)) {
            return;
         }

         const auto n = value.size();
         for (size_t i = 1; it < end; ++i, ++it) {
            if (*it == '"') {
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) {
                     ctx.error = error_code::end_reached;
                     return;
                  }
               }
               return;
            }

            if (i > n) {
               ctx.error = error_code::exceeded_static_array_size;
               return;
            }

            if (*it == '0') {
               value[n - i] = 0;
            }
            else if (*it == '1') {
               value[n - i] = 1;
            }
            else [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }
         }

         ctx.error = error_code::expected_quote;
      }
   };

   template <>
   struct from<JSON, skip>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&... args) noexcept
      {
         skip_value<JSON>::op<Opts>(ctx, args...);
      }
   };

   template <is_reference_wrapper T>
   struct from<JSON, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args)
      {
         using V = std::decay_t<decltype(value.get())>;
         from<JSON, V>::template op<Opts>(value.get(), std::forward<Args>(args)...);
      }
   };

   template <>
   struct from<JSON, hidden>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&...) noexcept
      {
         ctx.error = error_code::attempt_read_hidden;
      }
   };

   template <complex_t T>
   struct from<JSON, T>
   {
      template <auto Options>
      static void op(auto&& v, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }
         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }

         auto* ptr = reinterpret_cast<typename T::value_type*>(&v);
         static_assert(sizeof(T) == sizeof(typename T::value_type) * 2);
         parse<JSON>::op<Opts>(ptr[0], ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         if (match_invalid_end<',', Opts>(ctx, it, end)) {
            return;
         }

         parse<JSON>::op<Opts>(ptr[1], ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
         match<']'>(ctx, it);
         if constexpr (not Opts.null_terminated) {
            --ctx.depth;
         }
      }
   };

   template <always_null_t T>
   struct from<JSON, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }
         static constexpr sv null_string = "null";
         if constexpr (not check_is_padded(Opts)) {
            const auto n = size_t(end - it);
            if ((n < 4) || not comparitor<null_string>(it)) [[unlikely]] {
               ctx.error = error_code::syntax_error;
            }
         }
         else {
            if (not comparitor<null_string>(it)) [[unlikely]] {
               ctx.error = error_code::syntax_error;
            }
         }
         it += 4; // always advance for performance
      }
   };

   template <bool_t T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(bool_t auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (check_quoted_num(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (match_invalid_end<'"', Opts>(ctx, it, end)) {
               return;
            }
         }

         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if constexpr (check_bools_as_numbers(Opts)) {
            if (*it == '1') {
               value = true;
               ++it;
            }
            else if (*it == '0') {
               value = false;
               ++it;
            }
            else {
               ctx.error = error_code::syntax_error;
               return;
            }
         }
         else {
            if constexpr (not check_is_padded(Opts)) {
               if (size_t(end - it) < 4) [[unlikely]] {
                  ctx.error = error_code::expected_true_or_false;
                  return;
               }
            }

            uint32_t c;
            static constexpr uint32_t u_true = 0b01100101'01110101'01110010'01110100;
            static constexpr uint32_t u_fals = 0b01110011'01101100'01100001'01100110;
            std::memcpy(&c, it, 4);
            if constexpr (std::endian::native == std::endian::big) {
               c = std::byteswap(c);
            }
            it += 4;
            if (c == u_true) {
               value = true;
            }
            else if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
            else {
               if (c == u_fals && (*it == 'e')) [[likely]] {
                  value = false;
                  ++it;
               }
               else [[unlikely]] {
                  ctx.error = error_code::expected_true_or_false;
                  return;
               }
            }
         }

         if constexpr (check_quoted_num(Opts)) {
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            if (match<'"'>(ctx, it)) {
               return;
            }
            if constexpr (not Opts.null_terminated) {
               if (it == end) {
                  ctx.error = error_code::end_reached;
                  return;
               }
            }
         }
         else {
            if constexpr (not Opts.null_terminated) {
               if (it == end) {
                  ctx.error = error_code::end_reached;
                  return;
               }
            }
         }
      }
   };

   template <num_t T>
   struct from<JSON, T>
   {
      template <auto Opts, class It>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         if constexpr (check_quoted_num(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (match_invalid_end<'"', Opts>(ctx, it, end)) {
               return;
            }
         }

         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         using V = std::decay_t<decltype(value)>;
         if constexpr (int_t<V>) {
            static_assert(sizeof(*it) == sizeof(char));

            if constexpr (Opts.null_terminated) {
               if (not glz::atoi(value, it)) [[unlikely]] {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }
            }
            else {
               if (not glz::atoi(value, it, end)) [[unlikely]] {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }
            }
         }
         else {
// float128_t requires std::from_chars for floating-point, unavailable on older Apple platforms (iOS < 16.3)
#if !defined(_LIBCPP_VERSION) || defined(_LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT)
            if constexpr (is_float128<V>) {
               auto [ptr, ec] = std::from_chars(it, end, value);
               if (ec != std::errc()) {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }
               it = ptr;
            }
            else
#endif
            {
               if constexpr (std::is_volatile_v<std::remove_reference_t<decltype(value)>>) {
                  // Hardware may interact with value changes, so we parse into a temporary and assign in one
                  // place
                  V temp;
                  if constexpr (is_size_optimized(Opts)) {
                     auto [ptr, ec] = simple_float::from_chars<Opts.null_terminated>(it, end, temp);
                     if (ec != std::errc{}) [[unlikely]] {
                        ctx.error = error_code::parse_number_failure;
                        return;
                     }
                     it = ptr;
                  }
                  else {
                     auto [ptr, ec] = glz::from_chars<Opts.null_terminated>(it, end, temp);
                     if (ec != std::errc()) [[unlikely]] {
                        ctx.error = error_code::parse_number_failure;
                        return;
                     }
                     it = ptr;
                  }
                  value = temp;
               }
               else {
                  if constexpr (is_size_optimized(Opts)) {
                     auto [ptr, ec] = simple_float::from_chars<Opts.null_terminated>(it, end, value);
                     if (ec != std::errc{}) [[unlikely]] {
                        ctx.error = error_code::parse_number_failure;
                        return;
                     }
                     it = ptr;
                  }
                  else {
                     auto [ptr, ec] = glz::from_chars<Opts.null_terminated>(it, end, value);
                     if (ec != std::errc()) [[unlikely]] {
                        ctx.error = error_code::parse_number_failure;
                        return;
                     }
                     it = ptr;
                  }
               }
            }
         }

         if constexpr (check_quoted_num(Opts)) {
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            if (match<'"'>(ctx, it)) {
               return;
            }
            if constexpr (not Opts.null_terminated) {
               if (it == end) {
                  ctx.error = error_code::end_reached;
                  return;
               }
            }
         }
         else {
            if constexpr (not Opts.null_terminated) {
               if (it == end) {
                  ctx.error = error_code::end_reached;
                  return;
               }
            }
         }
      }
   };

   template <class T>
      requires(string_t<T> && !u8str_t<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It, class End>
         requires(check_is_padded(Opts))
      static void op(auto& value, is_context auto&& ctx, It&& it, End end)
      {
         if constexpr (check_string_as_number(Opts)) {
            auto start = it;
            skip_number<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            value.append(start, size_t(it - start));
         }
         else {
            if constexpr (!check_opening_handled(Opts)) {
               if constexpr (!check_ws_handled(Opts)) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }

               if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                  return;
               }
            }

            if constexpr (not check_raw_string(Opts)) {
               static constexpr auto string_padding_bytes = 8;

               auto start = it;
               while (true) {
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  uint64_t chunk;
                  std::memcpy(&chunk, it, 8);
                  if constexpr (std::endian::native == std::endian::big) {
                     chunk = std::byteswap(chunk);
                  }
                  const uint64_t test_chars = has_quote(chunk);
                  if (test_chars) {
                     it += (countr_zero(test_chars) >> 3);

                     auto* prev = it - 1;
                     while (*prev == '\\') {
                        --prev;
                     }
                     if (size_t(it - prev) % 2) {
                        break;
                     }
                     ++it; // skip the escaped quote
                  }
                  else {
                     it += 8;
                  }
               }

               auto n = size_t(it - start);
               value.resize(n + string_padding_bytes);

               auto* p = value.data();

               while (true) {
                  if (start >= it) {
                     break;
                  }

                  std::memcpy(p, start, 8);
                  uint64_t swar;
                  std::memcpy(&swar, p, 8);
                  if constexpr (std::endian::native == std::endian::big) {
                     swar = std::byteswap(swar);
                  }

                  constexpr uint64_t lo7_mask = repeat_byte8(0b01111111);
                  const uint64_t lo7 = swar & lo7_mask;
                  const uint64_t backslash = (lo7 ^ repeat_byte8('\\')) + lo7_mask;
                  const uint64_t less_32 = (swar & repeat_byte8(0b01100000)) + lo7_mask;
                  uint64_t next = ~((backslash & less_32) | swar);

                  next &= repeat_byte8(0b10000000);
                  if (next == 0) {
                     start += 8;
                     p += 8;
                     continue;
                  }

                  next = countr_zero(next) >> 3;
                  start += next;
                  if (start >= it) {
                     break;
                  }

                  if ((*start & 0b11100000) == 0) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  ++start; // skip the escape
                  if (*start == 'u') {
                     ++start;
                     p += next;
                     const auto mark = start;
                     const auto offset = handle_unicode_code_point(start, p, end);
                     if (offset == 0) [[unlikely]] {
                        ctx.error = error_code::unicode_escape_conversion_failure;
                        return;
                     }
                     n += offset;
                     // escape + u + unicode code points
                     n -= 2 + uint32_t(start - mark);
                  }
                  else {
                     p += next;
                     *p = char_unescape_table[uint8_t(*start)];
                     if (*p == 0) [[unlikely]] {
                        ctx.error = error_code::invalid_escape;
                        return;
                     }
                     ++p;
                     ++start;
                     --n;
                  }
               }

               value.resize(n);
               ++it;
            }
            else {
               // raw_string
               auto start = it;
               skip_string_view(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               value.assign(start, size_t(it - start));
               ++it;
            }
         }
      }

      template <auto Opts, class It, class End>
         requires(not check_is_padded(Opts))
      static void op(auto& value, is_context auto&& ctx, It&& it, End end)
      {
         if constexpr (check_string_as_number(Opts)) {
            auto start = it;
            skip_number<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (start == it) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }
            value.append(start, size_t(it - start));
         }
         else {
            if constexpr (!check_opening_handled(Opts)) {
               if constexpr (!check_ws_handled(Opts)) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }

               if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                  return;
               }
            }

            if constexpr (not check_raw_string(Opts)) {
               static constexpr auto string_padding_bytes = 8;

               if (size_t(end - it) >= 8) {
                  auto start = it;
                  const auto end8 = end - 8;
                  while (true) {
                     if (it >= end8) [[unlikely]] {
                        break;
                     }

                     uint64_t chunk;
                     std::memcpy(&chunk, it, 8);
                     if constexpr (std::endian::native == std::endian::big) {
                        chunk = std::byteswap(chunk);
                     }
                     const uint64_t test_chars = has_quote(chunk);
                     if (test_chars) {
                        it += (countr_zero(test_chars) >> 3);

                        auto* prev = it - 1;
                        while (*prev == '\\') {
                           --prev;
                        }
                        if (size_t(it - prev) % 2) {
                           goto continue_decode;
                        }
                        ++it; // skip the escaped quote
                     }
                     else {
                        it += 8;
                     }
                  }

                  while (it[-1] == '\\') [[unlikely]] {
                     // if we ended on an escape character then we need to rewind
                     // because we lost our context
                     --it;
                  }

                  for (; it < end; ++it) {
                     if (*it == '"') {
                        auto* prev = it - 1;
                        while (*prev == '\\') {
                           --prev;
                        }
                        if (size_t(it - prev) % 2) {
                           goto continue_decode;
                        }
                     }
                  }

                  ctx.error = error_code::unexpected_end;
                  return;

               continue_decode:

                  const auto available_padding = size_t(end - it);
                  auto n = size_t(it - start);
                  if (available_padding >= 8) [[likely]] {
                     value.resize(n + string_padding_bytes);

                     auto* p = value.data();

                     while (true) {
                        if (start >= it) {
                           break;
                        }

                        std::memcpy(p, start, 8);
                        uint64_t swar;
                        std::memcpy(&swar, p, 8);
                        if constexpr (std::endian::native == std::endian::big) {
                           swar = std::byteswap(swar);
                        }

                        constexpr uint64_t lo7_mask = repeat_byte8(0b01111111);
                        const uint64_t lo7 = swar & lo7_mask;
                        const uint64_t backslash = (lo7 ^ repeat_byte8('\\')) + lo7_mask;
                        const uint64_t less_32 = (swar & repeat_byte8(0b01100000)) + lo7_mask;
                        uint64_t next = ~((backslash & less_32) | swar);

                        next &= repeat_byte8(0b10000000);
                        if (next == 0) {
                           start += 8;
                           p += 8;
                           continue;
                        }

                        next = countr_zero(next) >> 3;
                        start += next;
                        if (start >= it) {
                           break;
                        }

                        if ((*start & 0b11100000) == 0) [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                        ++start; // skip the escape
                        if (*start == 'u') {
                           ++start;
                           p += next;
                           const auto mark = start;
                           const auto offset = handle_unicode_code_point(start, p, end);
                           if (offset == 0) [[unlikely]] {
                              ctx.error = error_code::unicode_escape_conversion_failure;
                              return;
                           }
                           n += offset;
                           // escape + u + unicode code points
                           n -= 2 + uint32_t(start - mark);
                        }
                        else {
                           p += next;
                           *p = char_unescape_table[uint8_t(*start)];
                           if (*p == 0) [[unlikely]] {
                              ctx.error = error_code::invalid_escape;
                              return;
                           }
                           ++p;
                           ++start;
                           --n;
                        }
                     }

                     value.resize(n);
                     ++it;
                  }
                  else {
                     // For large inputs this case of running out of buffer is very rare
                     value.resize(n);
                     auto* p = value.data();

                     it = start;
                     while (it < end) [[likely]] {
                        if (*it == '"') {
                           value.resize(size_t(p - value.data()));
                           ++it;
                           return;
                        }

                        if ((*it & 0b11100000) == 0) [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }

                        *p = *it;

                        if (*it == '\\') {
                           ++it; // skip the escape
                           if (*it == 'u') {
                              ++it;
                              if (!handle_unicode_code_point(it, p, end)) [[unlikely]] {
                                 ctx.error = error_code::unicode_escape_conversion_failure;
                                 return;
                              }
                           }
                           else {
                              *p = char_unescape_table[uint8_t(*it)];
                              if (*p == 0) [[unlikely]] {
                                 ctx.error = error_code::invalid_escape;
                                 return;
                              }
                              ++p;
                              ++it;
                           }
                        }
                        else {
                           ++it;
                           ++p;
                        }
                     }

                     ctx.error = error_code::unexpected_end;
                  }
               }
               else {
                  // For short strings

                  std::array<char, 8> buffer{};

                  auto* p = buffer.data();

                  while (it < end) [[likely]] {
                     *p = *it;
                     if (*it == '"') {
                        value.assign(buffer.data(), size_t(p - buffer.data()));
                        ++it;
                        if constexpr (not Opts.null_terminated) {
                           if (it == end) {
                              ctx.error = error_code::end_reached;
                              return;
                           }
                        }
                        return;
                     }
                     else if (*it == '\\') {
                        ++it; // skip the escape
                        if constexpr (not Opts.null_terminated) {
                           if (it == end) [[unlikely]] {
                              ctx.error = error_code::unexpected_end;
                              return;
                           }
                        }
                        if (*it == 'u') {
                           ++it;
                           if constexpr (not Opts.null_terminated) {
                              if (it == end) [[unlikely]] {
                                 ctx.error = error_code::unexpected_end;
                                 return;
                              }
                           }
                           if (!handle_unicode_code_point(it, p, end)) [[unlikely]] {
                              ctx.error = error_code::unicode_escape_conversion_failure;
                              return;
                           }
                        }
                        else {
                           *p = char_unescape_table[uint8_t(*it)];
                           if (*p == 0) [[unlikely]] {
                              ctx.error = error_code::invalid_escape;
                              return;
                           }
                           ++p;
                           ++it;
                        }
                     }
                     else if ((*it & 0b11100000) == 0) [[unlikely]] {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                     else {
                        ++it;
                        ++p;
                     }
                  }

                  ctx.error = error_code::unexpected_end;
               }
            }
            else {
               // raw_string
               auto start = it;
               skip_string_view(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               value.assign(start, size_t(it - start));
               ++it;
            }
         }
      }
   };

   // Specialization for u8string types (std::u8string)
   template <class T>
      requires(u8str_t<T> && !string_view_t<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It, class End>
         requires(check_is_padded(Opts))
      static void op(auto& value, is_context auto&& ctx, It&& it, End end)
      {
         if constexpr (check_string_as_number(Opts)) {
            auto start = it;
            skip_number<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            value.append(reinterpret_cast<const char8_t*>(start), size_t(it - start));
         }
         else {
            if constexpr (!check_opening_handled(Opts)) {
               if constexpr (!check_ws_handled(Opts)) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }

               if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                  return;
               }
            }

            if constexpr (not check_raw_string(Opts)) {
               static constexpr auto string_padding_bytes = 8;

               auto start = it;
               while (true) {
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  uint64_t chunk;
                  std::memcpy(&chunk, it, 8);
                  if constexpr (std::endian::native == std::endian::big) {
                     chunk = std::byteswap(chunk);
                  }
                  const uint64_t test_chars = has_quote(chunk);
                  if (test_chars) {
                     it += (countr_zero(test_chars) >> 3);

                     auto* prev = it - 1;
                     while (*prev == '\\') {
                        --prev;
                     }
                     if (size_t(it - prev) % 2) {
                        break;
                     }
                     ++it; // skip the escaped quote
                  }
                  else {
                     it += 8;
                  }
               }

               auto n = size_t(it - start);
               value.resize(n + string_padding_bytes);

               auto* p = reinterpret_cast<char*>(value.data());

               while (true) {
                  if (start >= it) {
                     break;
                  }

                  std::memcpy(p, start, 8);
                  uint64_t swar;
                  std::memcpy(&swar, p, 8);
                  if constexpr (std::endian::native == std::endian::big) {
                     swar = std::byteswap(swar);
                  }

                  constexpr uint64_t lo7_mask = repeat_byte8(0b01111111);
                  const uint64_t lo7 = swar & lo7_mask;
                  const uint64_t backslash = (lo7 ^ repeat_byte8('\\')) + lo7_mask;
                  const uint64_t less_32 = (swar & repeat_byte8(0b01100000)) + lo7_mask;
                  uint64_t next = ~((backslash & less_32) | swar);

                  next &= repeat_byte8(0b10000000);
                  if (next == 0) {
                     start += 8;
                     p += 8;
                     continue;
                  }

                  next = countr_zero(next) >> 3;
                  start += next;
                  if (start >= it) {
                     break;
                  }

                  if ((*start & 0b11100000) == 0) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  ++start; // skip the escape
                  if (*start == 'u') {
                     ++start;
                     p += next;
                     const auto mark = start;
                     const auto offset = handle_unicode_code_point(start, p, end);
                     if (offset == 0) [[unlikely]] {
                        ctx.error = error_code::unicode_escape_conversion_failure;
                        return;
                     }
                     n += offset;
                     // escape + u + unicode code points
                     n -= 2 + uint32_t(start - mark);
                  }
                  else {
                     p += next;
                     *p = char_unescape_table[uint8_t(*start)];
                     if (*p == 0) [[unlikely]] {
                        ctx.error = error_code::invalid_escape;
                        return;
                     }
                     ++p;
                     ++start;
                     --n;
                  }
               }

               value.resize(n);
               ++it;
            }
            else {
               // raw_string
               auto start = it;
               skip_string_view(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               value.assign(reinterpret_cast<const char8_t*>(start), size_t(it - start));
               ++it;
            }
         }
      }

      template <auto Opts, class It, class End>
         requires(not check_is_padded(Opts))
      static void op(auto& value, is_context auto&& ctx, It&& it, End end)
      {
         if constexpr (check_string_as_number(Opts)) {
            auto start = it;
            skip_number<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (start == it) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }
            value.append(reinterpret_cast<const char8_t*>(start), size_t(it - start));
         }
         else {
            if constexpr (!check_opening_handled(Opts)) {
               if constexpr (!check_ws_handled(Opts)) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }

               if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                  return;
               }
            }

            if constexpr (not check_raw_string(Opts)) {
               static constexpr auto string_padding_bytes = 8;

               if (size_t(end - it) >= 8) {
                  auto start = it;
                  const auto end8 = end - 8;
                  while (true) {
                     if (it >= end8) [[unlikely]] {
                        break;
                     }

                     uint64_t chunk;
                     std::memcpy(&chunk, it, 8);
                     if constexpr (std::endian::native == std::endian::big) {
                        chunk = std::byteswap(chunk);
                     }
                     const uint64_t test_chars = has_quote(chunk);
                     if (test_chars) {
                        it += (countr_zero(test_chars) >> 3);

                        auto* prev = it - 1;
                        while (*prev == '\\') {
                           --prev;
                        }
                        if (size_t(it - prev) % 2) {
                           goto continue_decode_u8;
                        }
                        ++it; // skip the escaped quote
                     }
                     else {
                        it += 8;
                     }
                  }

                  while (it[-1] == '\\') [[unlikely]] {
                     // if we ended on an escape character then we need to rewind
                     // because we lost our context
                     --it;
                  }

                  for (; it < end; ++it) {
                     if (*it == '"') {
                        auto* prev = it - 1;
                        while (*prev == '\\') {
                           --prev;
                        }
                        if (size_t(it - prev) % 2) {
                           goto continue_decode_u8;
                        }
                     }
                  }

                  ctx.error = error_code::unexpected_end;
                  return;

               continue_decode_u8:

                  const auto available_padding = size_t(end - it);
                  auto n = size_t(it - start);
                  if (available_padding >= 8) [[likely]] {
                     value.resize(n + string_padding_bytes);

                     auto* p = reinterpret_cast<char*>(value.data());

                     while (true) {
                        if (start >= it) {
                           break;
                        }

                        std::memcpy(p, start, 8);
                        uint64_t swar;
                        std::memcpy(&swar, p, 8);
                        if constexpr (std::endian::native == std::endian::big) {
                           swar = std::byteswap(swar);
                        }

                        constexpr uint64_t lo7_mask = repeat_byte8(0b01111111);
                        const uint64_t lo7 = swar & lo7_mask;
                        const uint64_t backslash = (lo7 ^ repeat_byte8('\\')) + lo7_mask;
                        const uint64_t less_32 = (swar & repeat_byte8(0b01100000)) + lo7_mask;
                        uint64_t next = ~((backslash & less_32) | swar);

                        next &= repeat_byte8(0b10000000);
                        if (next == 0) {
                           start += 8;
                           p += 8;
                           continue;
                        }

                        next = countr_zero(next) >> 3;
                        start += next;
                        if (start >= it) {
                           break;
                        }

                        if ((*start & 0b11100000) == 0) [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                        ++start; // skip the escape
                        if (*start == 'u') {
                           ++start;
                           p += next;
                           const auto mark = start;
                           const auto offset = handle_unicode_code_point(start, p, end);
                           if (offset == 0) [[unlikely]] {
                              ctx.error = error_code::unicode_escape_conversion_failure;
                              return;
                           }
                           n += offset;
                           // escape + u + unicode code points
                           n -= 2 + uint32_t(start - mark);
                        }
                        else {
                           p += next;
                           *p = char_unescape_table[uint8_t(*start)];
                           if (*p == 0) [[unlikely]] {
                              ctx.error = error_code::invalid_escape;
                              return;
                           }
                           ++p;
                           ++start;
                           --n;
                        }
                     }

                     value.resize(n);
                     ++it;
                  }
                  else {
                     // For large inputs this case of running out of buffer is very rare
                     value.resize(n);
                     auto* p = reinterpret_cast<char*>(value.data());

                     it = start;
                     while (it < end) [[likely]] {
                        if (*it == '"') {
                           value.resize(size_t(p - reinterpret_cast<char*>(value.data())));
                           ++it;
                           return;
                        }

                        if ((*it & 0b11100000) == 0) [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }

                        *p = *it;

                        if (*it == '\\') {
                           ++it; // skip the escape
                           if (*it == 'u') {
                              ++it;
                              if (!handle_unicode_code_point(it, p, end)) [[unlikely]] {
                                 ctx.error = error_code::unicode_escape_conversion_failure;
                                 return;
                              }
                           }
                           else {
                              *p = char_unescape_table[uint8_t(*it)];
                              if (*p == 0) [[unlikely]] {
                                 ctx.error = error_code::invalid_escape;
                                 return;
                              }
                              ++p;
                              ++it;
                           }
                        }
                        else {
                           ++it;
                           ++p;
                        }
                     }

                     ctx.error = error_code::unexpected_end;
                  }
               }
               else {
                  // For short strings

                  std::array<char, 8> buffer{};

                  auto* p = buffer.data();

                  while (it < end) [[likely]] {
                     *p = *it;
                     if (*it == '"') {
                        value.assign(reinterpret_cast<const char8_t*>(buffer.data()), size_t(p - buffer.data()));
                        ++it;
                        if constexpr (not Opts.null_terminated) {
                           if (it == end) {
                              ctx.error = error_code::end_reached;
                              return;
                           }
                        }
                        return;
                     }
                     else if (*it == '\\') {
                        ++it; // skip the escape
                        if constexpr (not Opts.null_terminated) {
                           if (it == end) [[unlikely]] {
                              ctx.error = error_code::unexpected_end;
                              return;
                           }
                        }
                        if (*it == 'u') {
                           ++it;
                           if constexpr (not Opts.null_terminated) {
                              if (it == end) [[unlikely]] {
                                 ctx.error = error_code::unexpected_end;
                                 return;
                              }
                           }
                           if (!handle_unicode_code_point(it, p, end)) [[unlikely]] {
                              ctx.error = error_code::unicode_escape_conversion_failure;
                              return;
                           }
                        }
                        else {
                           *p = char_unescape_table[uint8_t(*it)];
                           if (*p == 0) [[unlikely]] {
                              ctx.error = error_code::invalid_escape;
                              return;
                           }
                           ++p;
                           ++it;
                        }
                     }
                     else if ((*it & 0b11100000) == 0) [[unlikely]] {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                     else {
                        ++it;
                        ++p;
                     }
                  }

                  ctx.error = error_code::unexpected_end;
               }
            }
            else {
               // raw_string
               auto start = it;
               skip_string_view(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               value.assign(reinterpret_cast<const char8_t*>(start), size_t(it - start));
               ++it;
            }
         }
      }
   };

   template <class T>
      requires(string_view_t<T> || char_array_t<T> || array_char_t<T> || static_string_t<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It, class End>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto&& ctx, It&& it, End end) noexcept
      {
         if constexpr (!check_opening_handled(Opts)) {
            if constexpr (!check_ws_handled(Opts)) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }

            if (match_invalid_end<'"', Opts>(ctx, it, end)) {
               return;
            }
         }

         auto start = it;
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if constexpr (string_view_t<T>) {
            using value_type = typename std::decay_t<T>::value_type;
            if constexpr (std::same_as<value_type, char8_t>) {
               value = {reinterpret_cast<const char8_t*>(start), size_t(it - start)};
            }
            else {
               value = {start, size_t(it - start)};
            }
         }
         else if constexpr (char_array_t<T>) {
            const size_t n = it - start;
            if ((n + 1) > sizeof(value)) {
               ctx.error = error_code::unexpected_end;
               return;
            }
            std::memcpy(value, start, n);
            value[n] = '\0';
         }
         else if constexpr (array_char_t<T>) {
            const size_t n = it - start;
            if ((n + 1) > value.size()) {
               ctx.error = error_code::unexpected_end;
               return;
            }
            std::memcpy(value.data(), start, n);
            value[n] = '\0';
         }
         else if constexpr (static_string_t<T>) {
            const size_t n = it - start;
            if (n > value.capacity()) {
               ctx.error = error_code::unexpected_end;
               return;
            }
            value.assign(start, n);
         }
         ++it; // skip closing quote
         if constexpr (not Opts.null_terminated) {
            if (it == end) {
               ctx.error = error_code::end_reached;
               return;
            }
         }
      }
   };

   template <char_t T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (!check_opening_handled(Opts)) {
            if constexpr (!check_ws_handled(Opts)) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }

            if (match_invalid_end<'"', Opts>(ctx, it, end)) {
               return;
            }
         }

         // Handle empty string: "" should map to null character '\0'
         if (*it == '"') {
            value = '\0';
            ++it; // consume closing quote
            if constexpr (not Opts.null_terminated) {
               if (it == end) {
                  ctx.error = error_code::end_reached;
                  return;
               }
            }
            return;
         }

         if (*it == '\\') [[unlikely]] {
            ++it;
            switch (*it) {
            case '\0': {
               ctx.error = error_code::unexpected_end;
               return;
            }
            case '"':
            case '\\':
            case '/':
               value = *it++;
               break;
            case 'b':
               value = '\b';
               ++it;
               break;
            case 'f':
               value = '\f';
               ++it;
               break;
            case 'n':
               value = '\n';
               ++it;
               break;
            case 'r':
               value = '\r';
               ++it;
               break;
            case 't':
               value = '\t';
               ++it;
               break;
            case 'u': {
               ctx.error = error_code::unicode_escape_conversion_failure;
               return;
            }
            default: {
               ctx.error = error_code::invalid_escape;
               return;
            }
            }
         }
         else {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
            value = *it++;
         }
         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }
         if (match<'"'>(ctx, it)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            if (it == end) {
               ctx.error = error_code::end_reached;
               return;
            }
         }
      }
   };

   template <class T>
      requires(is_named_enum<T>)
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         constexpr auto N = reflect<T>::size;

         if (*it != '"') [[unlikely]] {
            ctx.error = error_code::expected_quote;
            return;
         }
         ++it;
         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }

         if constexpr (N == 1) {
            decode_index<Opts, T, 0>(value, ctx, it, end);
         }
         else if constexpr (check_linear_search(Opts)) {
            // Linear search path for enums - decode_linear advances past closing quote
            const auto index = decode_linear<T>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (index >= N) [[unlikely]] {
               ctx.error = error_code::unexpected_enum;
               return;
            }

            // Simply assign the enum value - fold expression dispatch
            [&]<size_t... Is>(std::index_sequence<Is...>) {
               (void)(((index == Is ? (value = get<Is>(reflect<T>::values), true) : false) || ...));
            }(std::make_index_sequence<N>{});
         }
         else {
            static constexpr auto HashInfo = hash_info<T>;

            const auto index = decode_hash<JSON, T, HashInfo, HashInfo.type>::op(it, end);

            if (index >= N) [[unlikely]] {
               ctx.error = error_code::unexpected_enum;
               return;
            }

            visit<N>([&]<size_t I>() { decode_index<Opts, T, I>(value, ctx, it, end); }, index);
         }
      }
   };

   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T> && !meta_keys<T> && !custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         // TODO: use std::bit_cast???
         std::underlying_type_t<std::decay_t<T>> x{};
         parse<JSON>::op<Opts>(x, ctx, it, end);
         value = static_cast<std::decay_t<T>>(x);
      }
   };

   template <func_t T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& /*value*/, is_context auto&& ctx, auto&& it, auto end)
      {
         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }
         if (match_invalid_end<'"', Opts>(ctx, it, end)) {
            return;
         }
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         if (match<'"'>(ctx, it)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            if (it == end) {
               ctx.error = error_code::end_reached;
               return;
            }
         }
      }
   };

   template <class T>
   struct from<JSON, basic_raw_json<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         auto it_start = it;
         if (*it == 'n') {
            match<"null", Opts>(ctx, it, end);
         }
         else if (is_digit(uint8_t(*it))) {
            skip_number<Opts>(ctx, it, end);
         }
         else {
            skip_value<JSON>::op<Opts>(ctx, it, end);
         }
         if (bool(ctx.error)) [[unlikely]]
            return;
         value.str = {it_start, static_cast<size_t>(it - it_start)};
      }
   };

   template <class T>
   struct from<JSON, basic_text<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&&, auto&& it, auto end)
      {
         value.str = {it, static_cast<size_t>(end - it)}; // read entire contents as string
         it = end;
      }
   };

   // for set types
   template <class T>
      requires(readable_array_t<T> && !emplace_backable<T> && !resizable<T> && emplaceable<T>)
   struct from<JSON, T>
   {
      template <auto Options>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         value.clear();
         if (*it == ']') [[unlikely]] {
            if constexpr (not Opts.null_terminated) {
               --ctx.depth;
            }
            ++it;
            return;
         }

         while (true) {
            using V = range_value_t<T>;
            V v;
            parse<JSON>::op<Opts>(v, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value.emplace(std::move(v));
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (*it == ']') {
               if constexpr (not Opts.null_terminated) {
                  --ctx.depth;
               }
               ++it;
               return;
            }
            if (match_invalid_end<',', Opts>(ctx, it, end)) {
               return;
            }
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }
      }
   };

   // for types like std::vector, std::array, std::deque, etc.
   template <class T>
      requires(readable_array_t<T> && (emplace_backable<T> || is_inplace_vector<T> || !resizable<T>) && !emplaceable<T>)
   struct from<JSON, T>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }

         const auto ws_start = it;
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         if (*it == ']') {
            if constexpr (not Opts.null_terminated) {
               --ctx.depth;
            }
            ++it;
            if constexpr ((resizable<T> || is_inplace_vector<T>) && not check_append_arrays(Opts)) {
               value.clear();

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }
            return;
         }

         const size_t ws_size = size_t(it - ws_start);

         static constexpr bool should_append = (resizable<T> || is_inplace_vector<T>) && check_append_arrays(Opts);
         if constexpr (not should_append) {
            const auto n = value.size();

            auto value_it = value.begin();

            for (size_t i = 0; i < n; ++i) {
               parse<JSON>::op<ws_handled<Opts>()>(*value_it++, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
               if (*it == ',') {
                  ++it;

                  if constexpr (!Opts.minified && !Opts.comments) {
                     if (ws_size && ws_size < size_t(end - it)) {
                        skip_matching_ws(ws_start, it, ws_size);
                     }
                  }

                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }
               else if (*it == ']') {
                  if constexpr (not Opts.null_terminated) {
                     --ctx.depth;
                  }
                  ++it;
                  if constexpr (erasable<T>) {
                     value.erase(value_it,
                                 value.end()); // use erase rather than resize for non-default constructible elements

                     if constexpr (check_shrink_to_fit(Opts)) {
                        value.shrink_to_fit();
                     }
                  }
                  return;
               }
               else [[unlikely]] {
                  ctx.error = error_code::expected_bracket;
                  return;
               }
            }
         }

         if constexpr (Opts.partial_read) {
            return;
         }
         else {
            // growing
            if constexpr (emplace_backable<T> || has_try_emplace_back<T>) {
               while (it < end) {
                  // Streaming refill point: at start of each iteration, ensure buffer has data
                  if constexpr (has_streaming_state<decltype(ctx)>) {
                     if (ctx.stream.enabled()) {
                        const size_t consumed = static_cast<size_t>(it - ctx.stream.data());
                        const size_t remaining = ctx.stream.size() - consumed;
                        // Refill when less than half of buffer remains to ensure space for next element
                        if (remaining <= ctx.stream.size() / 2 || it >= end) {
                           const char* new_it;
                           const char* new_end;
                           ctx.stream.consume_and_refill(consumed, new_it, new_end);
                           it = new_it;
                           end = new_end;
                           if (it >= end && ctx.stream.at_eof()) {
                              break; // No more data, exit loop normally
                           }
                        }
                     }
                  }

                  if constexpr (has_try_emplace_back<T>) {
                     if (value.try_emplace_back() != nullptr)
                        parse<JSON>::op<ws_handled<Opts>()>(value.back(), ctx, it, end);
                     else
                        ctx.error = error_code::exceeded_static_array_size;
                  }
                  else {
                     parse<JSON>::op<ws_handled<Opts>()>(value.emplace_back(), ctx, it, end);
                  }

                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  // Streaming refill point: after parsing each element, refill if buffer is low
                  if constexpr (has_streaming_state<decltype(ctx)>) {
                     if (ctx.stream.enabled()) {
                        const size_t consumed = static_cast<size_t>(it - ctx.stream.data());
                        // Refill when less than 25% of buffer remains to ensure enough space for next element
                        if (ctx.stream.size() - consumed <= ctx.stream.size() / 4 || it >= end) {
                           const char* new_it;
                           const char* new_end;
                           ctx.stream.consume_and_refill(consumed, new_it, new_end);
                           it = new_it;
                           end = new_end;
                           if (it >= end && ctx.stream.at_eof()) {
                              ctx.error = error_code::unexpected_end;
                              return;
                           }
                        }
                     }
                  }

                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  if (*it == ',') [[likely]] {
                     ++it;

                     if constexpr (!Opts.minified && !Opts.comments) {
                        if (ws_size && ws_size < size_t(end - it)) {
                           skip_matching_ws(ws_start, it, ws_size);
                        }
                     }

                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                  }
                  else if (*it == ']') {
                     if constexpr (not Opts.null_terminated) {
                        --ctx.depth;
                     }
                     ++it;
                     return;
                  }
                  else [[unlikely]] {
                     ctx.error = error_code::expected_bracket;
                     return;
                  }
               }
            }
            else {
               ctx.error = error_code::exceeded_static_array_size;
            }
         }
      }

      // for types like std::vector<std::pair...> that can't look up with operator[]
      // Intead of hashing or linear searching, we just clear the input and overwrite the entire contents
      template <auto Options>
         requires(pair_t<range_value_t<T>> && check_concatenate(Options) == true)
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         static constexpr auto Opts = opening_handled_off<ws_handled_off<Options>()>();
         if constexpr (!check_opening_handled(Options)) {
            if constexpr (!check_ws_handled(Options)) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }
            if (match_invalid_end<'{', Opts>(ctx, it, end)) {
               return;
            }
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            if constexpr (not Opts.null_terminated) {
               ++ctx.depth;
            }
         }

         // clear all contents and repopulate
         value.clear();

         while (it < end) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }

            if (*it == '}') {
               ++it;
               if constexpr (not Opts.null_terminated) {
                  --ctx.depth;
               }
               if constexpr (not Opts.null_terminated) {
                  if (it == end) {
                     ctx.error = error_code::end_reached;
                     return;
                  }
               }
               return;
            }

            if constexpr (has_try_emplace_back<T>) {
               if (value.try_emplace_back() == nullptr) [[unlikely]] {
                  ctx.error = error_code::exceeded_static_array_size;
                  return;
               }
            }
            else {
               value.emplace_back();
            }
            auto& item = value.back();

            using V = std::decay_t<decltype(item)>;

            if constexpr (str_t<typename V::first_type> ||
                          (std::is_enum_v<typename V::first_type> && glaze_t<typename V::first_type>)) {
               parse<JSON>::op<Opts>(item.first, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
            else {
               std::string_view key;
               parse<JSON>::op<Opts>(key, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               if constexpr (Opts.null_terminated) {
                  parse<JSON>::op<Opts>(item.first, ctx, key.data(), key.data() + key.size());
               }
               else {
                  if (size_t(end - it) == key.size()) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  // For the non-null terminated case we just want one more character so that we don't parse
                  // until the end of the buffer and create an end_reached code (unless there is an error).
                  parse<JSON>::op<Opts>(item.first, ctx, key.data(), key.data() + key.size() + 1);
               }
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }

            if (parse_ws_colon<Opts>(ctx, it, end)) {
               return;
            }

            parse<JSON>::op<Opts>(item.second, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }

            if (*it == ',') {
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
            }
         }

         ctx.error = error_code::unexpected_end;
      }
   };

   // counts the number of JSON array elements
   // needed for classes that are resizable, but do not have an emplace_back
   // 'it' is copied so that it does not actually progress the iterator
   // expects the opening brace ([) to have already been consumed
   template <auto Opts>
   [[nodiscard]] size_t number_of_array_elements(is_context auto&& ctx, auto it, auto end) noexcept
   {
      skip_ws<Opts>(ctx, it, end);
      if (bool(ctx.error)) [[unlikely]]
         return {};

      if (*it == ']') [[unlikely]] {
         return 0;
      }
      size_t count = 1;
      while (true) {
         switch (*it) {
         case ',': {
            ++count;
            ++it;
            break;
         }
         case '/': {
            skip_comment(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return {};
            break;
         }
         case '{':
            ++it;
            skip_until_closed<Opts, '{', '}'>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return {};
            break;
         case '[':
            ++it;
            skip_until_closed<Opts, '[', ']'>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return {};
            break;
         case '"': {
            skip_string<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return {};
            break;
         }
         case ']': {
            return count;
         }
         case '\0': {
            ctx.error = error_code::unexpected_end;
            return {};
         }
         default:
            ++it;
         }
      }
      unreachable();
   }

   // For types like std::forward_list
   template <class T>
      requires readable_array_t<T> && (!emplace_backable<T> && resizable<T>)
   struct from<JSON, T>
   {
      template <auto Options>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }
         const auto n = number_of_array_elements<Opts>(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value.resize(n);
         size_t i = 0;
         for (auto& x : value) {
            parse<JSON>::op<Opts>(x, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (i < n - 1) {
               if (match_invalid_end<',', Opts>(ctx, it, end)) {
                  return;
               }
            }
            ++i;
         }
         match<']'>(ctx, it);
         if constexpr (not Opts.null_terminated) {
            --ctx.depth;
         }
      }
   };

   template <class T>
      requires glaze_array_t<T> || tuple_t<T> || is_std_tuple<T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         static constexpr auto N = []() constexpr {
            if constexpr (glaze_array_t<T>) {
               return reflect<T>::size;
            }
            else {
               return glz::tuple_size_v<T>;
            }
         }();

         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (*it == ']') {
               if constexpr (not Opts.null_terminated) {
                  --ctx.depth;
               }
               return;
            }
            if constexpr (I != 0) {
               if (match_invalid_end<',', Opts>(ctx, it, end)) {
                  return;
               }
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }
            if constexpr (is_std_tuple<T>) {
               parse<JSON>::op<ws_handled<Opts>()>(std::get<I>(value), ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
            else if constexpr (glaze_array_t<T>) {
               parse<JSON>::op<ws_handled<Opts>()>(get_member(value, glz::get<I>(meta_v<T>)), ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
            else {
               parse<JSON>::op<ws_handled<Opts>()>(glz::get<I>(value), ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         });

         if constexpr (Opts.partial_read) {
            return;
         }
         else {
            if (bool(ctx.error)) [[unlikely]]
               return;
            match<']'>(ctx, it);
            if constexpr (not Opts.null_terminated) {
               --ctx.depth;
            }
            if constexpr (not Opts.null_terminated) {
               if (it == end) {
                  ctx.error = error_code::end_reached;
                  return;
               }
            }
         }
      }
   };

   template <glaze_flags_t T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }

         std::string& s = string_buffer();

         constexpr auto& HashInfo = hash_info<T>;
         static_assert(bool(HashInfo.type));

         while (true) {
            parse<JSON>::op<ws_handled_off<Opts>()>(s, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;

            const auto index =
               decode_hash_with_size<JSON, T, HashInfo, HashInfo.type>::op(s.data(), s.data() + s.size(), s.size());

            constexpr auto N = reflect<T>::size;
            if (index < N) [[likely]] {
               visit<N>([&]<size_t I>() { get_member(value, get<I>(reflect<T>::values)) = true; }, index);
            }
            else [[unlikely]] {
               ctx.error = error_code::invalid_flag_input;
               return;
            }

            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (*it == ']') {
               if constexpr (not Opts.null_terminated) {
                  --ctx.depth;
               }
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) {
                     ctx.error = error_code::end_reached;
                     return;
                  }
               }
               return;
            }
            if (match_invalid_end<',', Opts>(ctx, it, end)) {
               return;
            }
         }
      }
   };

   template <class T>
   struct from<JSON, includer<T>>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         std::string buffer{};
         parse<JSON>::op<Opts>(buffer, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         const auto file_path = relativize_if_not_absolute(std::filesystem::path(ctx.current_file).parent_path(),
                                                           std::filesystem::path{buffer});

         const auto string_file_path = file_path.string();
         const auto ec = file_to_buffer(buffer, string_file_path);

         if (bool(ec)) [[unlikely]] {
            ctx.error = error_code::includer_error;
            auto& error_msg = error_buffer();
            error_msg = "file failed to open: " + string_file_path;
            ctx.custom_error_message = error_msg;
            return;
         }

         const auto current_file = ctx.current_file;
         ctx.current_file = string_file_path;

         // We need to allocate a new buffer here because we could call another includer that uses the buffer
         std::string nested_buffer = buffer;
         static constexpr auto NestedOpts = opt_true<disable_padding_on<Opts>(), &opts::null_terminated>;
         const auto ecode = glz::read<NestedOpts>(value.value, nested_buffer, ctx);
         if (bool(ctx.error)) [[unlikely]] {
            ctx.error = error_code::includer_error;
            auto& error_msg = error_buffer();
            error_msg = glz::format_error(ecode, nested_buffer);
            ctx.custom_error_message = error_msg;
            return;
         }

         ctx.current_file = current_file;
      }
   };

   template <pair_t T>
   struct from<JSON, T>
   {
      template <auto Options, string_literal tag = "">
      static void op(T& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = opening_handled_off<ws_handled_off<Options>()>();
         if constexpr (!check_opening_handled(Options)) {
            if constexpr (!check_ws_handled(Options)) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }
            if (match_invalid_end<'{', Opts>(ctx, it, end)) {
               return;
            }
            if constexpr (not Opts.null_terminated) {
               ++ctx.depth;
            }
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         if (*it == '}') {
            if constexpr (not Opts.null_terminated) {
               --ctx.depth;
            }
            if constexpr (Opts.error_on_missing_keys) {
               ctx.error = error_code::missing_key;
            }
            return;
         }

         using first_type = typename T::first_type;
         if constexpr (str_t<first_type> || is_named_enum<first_type>) {
            parse<JSON>::op<Opts>(value.first, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
         }
         else {
            std::string_view key;
            parse<JSON>::op<Opts>(key, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            if constexpr (Opts.null_terminated) {
               parse<JSON>::op<Opts>(value.first, ctx, key.data(), key.data() + key.size());
            }
            else {
               if (size_t(end - it) == key.size()) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               // For the non-null terminated case we just want one more character so that we don't parse
               // until the end of the buffer and create an end_reached code (unless there is an error).
               parse<JSON>::op<Opts>(value.first, ctx, key.data(), key.data() + key.size() + 1);
            }
            if (bool(ctx.error)) [[unlikely]]
               return;
         }

         if (parse_ws_colon<Opts>(ctx, it, end)) {
            return;
         }

         parse<JSON>::op<Opts>(value.second, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         match<'}'>(ctx, it);
         if constexpr (not Opts.null_terminated) {
            --ctx.depth;
         }
         if constexpr (not Opts.null_terminated) {
            if (it == end) {
               ctx.error = error_code::end_reached;
               return;
            }
         }
      }
   };

   template <class T, string_literal Tag>
   inline consteval bool contains_tag()
   {
      auto& keys = reflect<T>::keys;
      for (size_t i = 0; i < keys.size(); ++i) {
         if (Tag.sv() == keys[i]) {
            return true;
         }
      }
      return false;
   }

   template <class T>
      requires((readable_map_t<T> || glaze_object_t<T> || reflectable<T>) && not custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Options, string_literal tag = "">
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         static constexpr auto num_members = reflect<T>::size;

         static constexpr auto Opts = opening_handled_off<ws_handled_off<Options>()>();
         if constexpr (!check_opening_handled(Options)) {
            if constexpr (!check_ws_handled(Options)) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }
            if (match_invalid_end<'{', Opts>(ctx, it, end)) {
               return;
            }
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            if constexpr (not Opts.null_terminated) {
               ++ctx.depth;
            }
         }
         const auto ws_start = it;
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
         const size_t ws_size = size_t(it - ws_start);

         if constexpr ((glaze_object_t<T> || reflectable<T>) && num_members == 0 && Opts.error_on_unknown_keys) {
            if constexpr (not tag.sv().empty()) {
               if (*it == '"') {
                  ++it;
                  if constexpr (not Opts.null_terminated) {
                     if (it == end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                  }

                  const auto start = it;
                  skip_string_view(ctx, it, end);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
                  const sv key{start, size_t(it - start)};
                  ++it;
                  if constexpr (not Opts.null_terminated) {
                     if (it == end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                  }

                  if (key == tag.sv()) {
                     if (parse_ws_colon<Opts>(ctx, it, end)) {
                        return;
                     }

                     parse<JSON>::handle_unknown<Opts>(key, value, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;

                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                  }
                  else {
                     ctx.error = error_code::unknown_key;
                     return;
                  }
               }
            }

            if (*it == '}') [[likely]] {
               if constexpr (not Opts.null_terminated) {
                  --ctx.depth;
               }
               if constexpr (glaze_object_t<T> || reflectable<T>) {
                  if constexpr (has_self_constraint_v<T> && !check_skip_self_constraint(Opts)) {
                     auto wrapper = self_constraint_v<T>(value);
                     from<JSON, decltype(wrapper)>::template op<ws_handled<Opts>()>(wrapper, ctx, it, end);
                     if (bool(ctx.error)) {
                        return;
                     }
                  }
               }
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) {
                     ctx.error = error_code::end_reached;
                     return;
                  }
               }
               if constexpr (Opts.partial_read) {
                  ctx.error = error_code::partial_read_complete;
               }
               return;
            }
            ctx.error = error_code::unknown_key;
            return;
         }
         else {
            decltype(auto) fields = [&]() -> decltype(auto) {
               if constexpr ((glaze_object_t<T> || reflectable<T>) &&
                             (Opts.error_on_missing_keys || Opts.partial_read)) {
                  return bit_array<num_members>{};
               }
               else {
                  return nullptr;
               }
            }();

            size_t read_count{}; // for partial_read

            bool first = true;
            while (true) {
               if constexpr ((glaze_object_t<T> || reflectable<T>) && Opts.partial_read) {
                  static constexpr bit_array<num_members> all_fields = [] {
                     bit_array<num_members> arr{};
                     for (size_t i = 0; i < num_members; ++i) {
                        arr[i] = true;
                     }
                     return arr;
                  }();

                  if ((all_fields & fields) == all_fields) {
                     ctx.error = error_code::partial_read_complete;
                     return;
                  }
               }

               // Streaming refill point: at start of each iteration, ensure buffer has data
               if constexpr (has_streaming_state<decltype(ctx)>) {
                  if (ctx.stream.enabled()) {
                     const size_t consumed = static_cast<size_t>(it - ctx.stream.data());
                     const size_t remaining = ctx.stream.size() - consumed;
                     // Refill when less than half of buffer remains to ensure space for next key-value pair
                     if (remaining <= ctx.stream.size() / 2 || it >= end) {
                        const char* new_it;
                        const char* new_end;
                        ctx.stream.consume_and_refill(consumed, new_it, new_end);
                        it = new_it;
                        end = new_end;
                        if (it >= end && ctx.stream.at_eof()) {
                           ctx.error = error_code::unexpected_end;
                           return;
                        }
                     }
                  }
               }

               if (*it == '}') {
                  if constexpr (not Opts.null_terminated) {
                     --ctx.depth;
                  }
                  if constexpr ((glaze_object_t<T> || reflectable<T>) && Opts.error_on_missing_keys) {
                     constexpr auto req_fields = required_fields<T, Opts>();
                     if ((req_fields & fields) != req_fields) {
                        for (size_t i = 0; i < num_members; ++i) {
                           if (not fields[i] && req_fields[i]) {
                              ctx.custom_error_message = reflect<T>::keys[i];
                              // We just return the first missing key in order to avoid heap allocations
                              break;
                           }
                        }

                        ctx.error = error_code::missing_key;
                        return;
                     }
                  }
                  if constexpr (glaze_object_t<T> || reflectable<T>) {
                     if constexpr (has_self_constraint_v<T> && !check_skip_self_constraint(Opts)) {
                        auto wrapper = self_constraint_v<T>(value);
                        from<JSON, decltype(wrapper)>::template op<ws_handled<Opts>()>(wrapper, ctx, it, end);
                        if (bool(ctx.error)) {
                           return;
                        }
                     }
                  }
                  ++it; // Increment after checking for mising keys so errors are within buffer bounds
                  if constexpr (not Opts.null_terminated) {
                     if (it == end) {
                        ctx.error = error_code::end_reached;
                        return;
                     }
                  }
                  return;
               }
               else if (first) {
                  first = false;
               }
               else {
                  if (match_invalid_end<',', Opts>(ctx, it, end)) {
                     return;
                  }
                  if constexpr (not Opts.null_terminated) {
                     if (it == end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                  }

                  if constexpr ((not Opts.minified) && (num_members > 1 || not Opts.error_on_unknown_keys) &&
                                (!Opts.comments)) {
                     if (ws_size && ws_size < size_t(end - it)) {
                        skip_matching_ws(ws_start, it, ws_size);
                     }
                  }

                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }

               constexpr auto reflection_type = glaze_object_t<T> || reflectable<T>;

               if constexpr (reflection_type && (num_members == 0)) {
                  if constexpr (Opts.error_on_unknown_keys) {
                     static_assert(false_v<T>, "This should be unreachable");
                  }
                  else {
                     if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                        return;
                     }

                     // parsing to an empty object, but at this point the JSON presents keys

                     // Unknown key handler does not unescape keys. Unknown escaped keys are
                     // handled by the user.

                     const auto start = it;
                     skip_string_view(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     const sv key{start, size_t(it - start)};
                     ++it;
                     if constexpr (not Opts.null_terminated) {
                        if (it == end) [[unlikely]] {
                           ctx.error = error_code::unexpected_end;
                           return;
                        }
                     }

                     if (parse_ws_colon<Opts>(ctx, it, end)) {
                        return;
                     }

                     // Check if this key is the variant tag that should be skipped
                     if constexpr (not tag.sv().empty()) {
                        if (key == tag.sv()) {
                           // Skip the tag value
                           skip_value<JSON>::op<Opts>(ctx, it, end);
                           if (bool(ctx.error)) [[unlikely]]
                              return;
                           if (skip_ws<Opts>(ctx, it, end)) {
                              return;
                           }
                           continue;
                        }
                     }

                     parse<JSON>::handle_unknown<Opts>(key, value, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     if constexpr (not Opts.null_terminated) {
                        if (it == end) [[unlikely]] {
                           ctx.error = error_code::unexpected_end;
                           return;
                        }
                     }
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                  }
               }
               else if constexpr (reflection_type) {
                  static_assert(bool(hash_info<T>.type));

                  if (*it != '"') [[unlikely]] {
                     ctx.error = error_code::expected_quote;
                     return;
                  }
                  ++it;
                  if constexpr (not Opts.null_terminated) {
                     if (it == end) [[unlikely]] {
                        ctx.error = error_code::unexpected_end;
                        return;
                     }
                  }

                  if constexpr (not tag.sv().empty() && not contains_tag<T, tag>()) {
                     // For tagged variants we first check to see if the key matches the tag
                     // We only need to do this if the tag is not part of the keys

                     const auto start = it;
                     skip_string_view(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     const sv key{start, size_t(it - start)};
                     ++it;
                     if constexpr (not Opts.null_terminated) {
                        if (it == end) [[unlikely]] {
                           ctx.error = error_code::unexpected_end;
                           return;
                        }
                     }

                     if (key == tag.sv()) {
                        if (parse_ws_colon<Opts>(ctx, it, end)) {
                           return;
                        }

                        parse<JSON>::handle_unknown<Opts>(key, value, ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]]
                           return;

                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        continue;
                     }
                     else {
                        it = start; // reset the iterator
                     }
                  }

                  if constexpr (Opts.error_on_missing_keys || Opts.partial_read) {
                     size_t index = num_members;
                     parse_and_invoke<Opts, T>(value, ctx, it, end, index);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     if (index < num_members) {
                        fields[index] = true;
                     }
                  }
                  else {
                     parse_and_invoke<Opts, T>(value, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
               }
               else {
                  // For types like std::map, std::unordered_map

                  auto reading = [&](auto&& key) {
                     if constexpr (Opts.partial_read) {
                        if (auto element = value.find(key); element != value.end()) {
                           ++read_count;
                           parse<JSON>::op<ws_handled<Opts>()>(element->second, ctx, it, end);
                        }
                        else {
                           skip_value<JSON>::op<Opts>(ctx, it, end);
                        }
                     }
                     else {
                        parse<JSON>::op<ws_handled<Opts>()>(value[key], ctx, it, end);
                     }
                  };

                  // using Key = std::conditional_t<heterogeneous_map<T>, sv, typename T::key_type>;
                  using Key = typename T::key_type;
                  if constexpr (std::is_same_v<Key, std::string>) {
                     static thread_local Key key;
                     parse<JSON>::op<Opts>(key, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;

                     if (parse_ws_colon<Opts>(ctx, it, end)) {
                        return;
                     }

                     reading(key);
                     if constexpr (Opts.partial_read) {
                        if (read_count == value.size()) {
                           return;
                        }
                     }
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
                  else if constexpr (str_t<Key>) {
                     Key key;
                     parse<JSON>::op<Opts>(key, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;

                     if (parse_ws_colon<Opts>(ctx, it, end)) {
                        return;
                     }

                     reading(key);
                     if constexpr (Opts.partial_read) {
                        if (read_count == value.size()) {
                           return;
                        }
                     }
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
                  else {
                     Key key_value{};
                     if constexpr (glaze_enum_t<Key> || mimics_str_t<Key>) {
                        parse<JSON>::op<Opts>(key_value, ctx, it, end);
                     }
                     else if constexpr (std::is_arithmetic_v<Key>) {
                        // prefer over quoted_t below to avoid double parsing of quoted_t
                        parse<JSON>::op<opt_true<Opts, quoted_num_opt_tag{}>>(key_value, ctx, it, end);
                     }
                     else {
                        parse<JSON>::op<opt_false<Opts, raw_string_opt_tag{}>>(quoted_t<Key>{key_value}, ctx, it, end);
                     }
                     if (bool(ctx.error)) [[unlikely]]
                        return;

                     if (parse_ws_colon<Opts>(ctx, it, end)) {
                        return;
                     }

                     reading(key_value);
                     if constexpr (Opts.partial_read) {
                        if (read_count == value.size()) {
                           return;
                        }
                     }
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
               }

               // Streaming refill point: after parsing each key-value pair, refill if buffer is low
               if constexpr (has_streaming_state<decltype(ctx)>) {
                  if (ctx.stream.enabled()) {
                     const size_t consumed = static_cast<size_t>(it - ctx.stream.data());
                     // Refill when less than 25% of buffer remains to ensure enough space for next element
                     if (ctx.stream.size() - consumed <= ctx.stream.size() / 4 || it >= end) {
                        const char* new_it;
                        const char* new_end;
                        ctx.stream.consume_and_refill(consumed, new_it, new_end);
                        it = new_it;
                        end = new_end;
                        if (it >= end && ctx.stream.at_eof()) {
                           ctx.error = error_code::unexpected_end;
                           return;
                        }
                     }
                  }
               }

               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }
         }
      }
   };

   template <is_variant T>
   consteval auto variant_is_auto_deducible()
   {
      // Contains at most one each of the basic json types bool, numeric, string, object, array
      // If all objects are meta-objects then we can attempt to deduce them as well either through a type tag or
      // unique combinations of keys
      // Also considers types with mimic declarations or custom read/write with inferable input types
      int bools{}, numbers{}, strings{}, objects{}, meta_objects{}, arrays{};
      constexpr auto N = std::variant_size_v<T>;
      for_each<N>([&]<auto I>() {
         using V = std::decay_t<std::variant_alternative_t<I, T>>;
         // Custom takes precedence over mimic (consistent with runtime behavior)
         if constexpr (has_custom_meta_v<V>) {
            bools += custom_bool_t<V>;
            numbers += custom_num_t<V>;
            strings += custom_str_t<V>;
         }
         else {
            bools += bool_t<V> || mimics_bool_t<V>;
            numbers += num_t<V> || mimics_num_t<V>;
            strings += str_t<V> || mimics_str_t<V>;
            strings += glaze_enum_t<V>;
         }
         objects += pair_t<V>;
         objects += (writable_map_t<V> || readable_map_t<V> || is_memory_object<V>);
         objects += glaze_object_t<V>;
         meta_objects += glaze_object_t<V> || reflectable<V> || is_memory_object<V>;
         arrays += glaze_array_t<V>;
         arrays += array_t<V>;
         // TODO null
      });
      return bools < 2 && numbers < 2 && strings < 2 && (objects < 2 || meta_objects == objects) && arrays < 2;
   }

   // ============================================================================
   // Variant type category traits (index-based, avoids expensive tuple_cat)
   // ============================================================================

   // Concepts for variant type classification
   template <class T>
   concept variant_bool_type = bool_t<remove_meta_wrapper_t<T>> || mimics_bool_t<T> || custom_bool_t<T>;

   template <class T>
   concept variant_num_type = num_t<remove_meta_wrapper_t<T>> || mimics_num_t<T> || custom_num_t<T>;

   template <class T>
   concept variant_str_type = str_t<remove_meta_wrapper_t<T>> || glaze_enum_t<remove_meta_wrapper_t<T>> ||
                              glaze_enum_t<T> || mimics_str_t<T> || custom_str_t<T>;

   template <class T>
   concept variant_object_type = json_object<T>;

   template <class T>
   concept variant_array_type = array_t<remove_meta_wrapper_t<T>> || glaze_array_t<T> || tuple_t<T> || is_std_tuple<T>;

   template <class T>
   concept variant_null_type = null_t<T>;

   template <class T>
   concept variant_nullable_object_type = is_memory_object<T>;

   // Type traits wrapping concepts for template template parameter use
   template <class T>
   struct is_variant_bool : std::bool_constant<variant_bool_type<T>>
   {};
   template <class T>
   struct is_variant_num : std::bool_constant<variant_num_type<T>>
   {};
   template <class T>
   struct is_variant_str : std::bool_constant<variant_str_type<T>>
   {};
   template <class T>
   struct is_variant_object : std::bool_constant<variant_object_type<T>>
   {};
   template <class T>
   struct is_variant_array : std::bool_constant<variant_array_type<T>>
   {};
   template <class T>
   struct is_variant_null : std::bool_constant<variant_null_type<T>>
   {};
   template <class T>
   struct is_variant_nullable_object : std::bool_constant<variant_nullable_object_type<T>>
   {};

   // Count types in variant matching a trait (fold expression, very fast to compile)
   // Using helper struct instead of IIFE for MSVC compatibility
   template <class Variant, template <class> class Trait>
   struct variant_count_impl;

   template <template <class> class Trait, class... Ts>
   struct variant_count_impl<std::variant<Ts...>, Trait>
   {
      static constexpr size_t value = (size_t(Trait<Ts>::value) + ... + 0);
   };

   template <class Variant, template <class> class Trait>
   constexpr size_t variant_count_v = variant_count_impl<Variant, Trait>::value;

   // Get first index matching trait (or variant_npos if none)
   // Using helper struct instead of IIFE for MSVC compatibility
   template <class Variant, template <class> class Trait>
   struct variant_first_index_impl;

   template <template <class> class Trait, class... Ts>
   struct variant_first_index_impl<std::variant<Ts...>, Trait>
   {
      static constexpr size_t find()
      {
         size_t result = std::variant_npos;
         size_t idx = 0;
         // Short-circuit: find first match
         ((Trait<Ts>::value && result == std::variant_npos ? (result = idx, ++idx) : ++idx), ...);
         return result;
      }
      static constexpr size_t value = find();
   };

   template <class Variant, template <class> class Trait>
   constexpr size_t variant_first_index_v = variant_first_index_impl<Variant, Trait>::value;

   // Count types matching both category trait AND const/non-const filter
   // Using helper struct instead of IIFE for MSVC compatibility
   template <class Variant, template <class> class Trait, bool IsConst>
   struct variant_filtered_count_impl;

   template <template <class> class Trait, bool IsConst, class... Ts>
   struct variant_filtered_count_impl<std::variant<Ts...>, Trait, IsConst>
   {
      static constexpr size_t value = (size_t(Trait<Ts>::value && (glaze_const_value_t<Ts> == IsConst)) + ... + 0);
   };

   template <class Variant, template <class> class Trait, bool IsConst>
   constexpr size_t variant_filtered_count_v = variant_filtered_count_impl<Variant, Trait, IsConst>::value;

   // Variant type counts using fold expressions (replaces tuple-based variant_type_count)
   template <class T>
   struct variant_type_count
   {
      static constexpr auto n_bool = variant_count_v<T, is_variant_bool>;
      static constexpr auto n_number = variant_count_v<T, is_variant_num>;
      static constexpr auto n_string = variant_count_v<T, is_variant_str>;
      static constexpr auto n_nullable_object = variant_count_v<T, is_variant_nullable_object>;
      static constexpr auto n_object = variant_count_v<T, is_variant_object> + n_nullable_object;
      static constexpr auto n_array = variant_count_v<T, is_variant_array>;
      static constexpr auto n_null = variant_count_v<T, is_variant_null>;
   };

   // Process variant alternatives by iterating directly over variant indices
   // (replaces tuple_cat + tuple iteration approach)
   template <class Variant, template <class> class Trait>
   struct process_variant_alternatives
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto category_count = variant_count_v<Variant, Trait>;

         if constexpr (category_count == 0) {
            ctx.error = error_code::no_matching_variant_type;
         }
         else {
            constexpr auto N = std::variant_size_v<Variant>;
            constexpr auto const_count = variant_filtered_count_v<Variant, Trait, true>;
            constexpr auto non_const_count = variant_filtered_count_v<Variant, Trait, false>;

            bool found_match{};

            // First pass: const glaze types in this category
            if constexpr (const_count > 0) {
               for_each<N>([&]<size_t I>() {
                  if (found_match) {
                     return;
                  }
                  using V = std::variant_alternative_t<I, Variant>;
                  if constexpr (Trait<V>::value && glaze_const_value_t<V>) {
                     // run time substitute to compare to const value
                     std::remove_const_t<std::remove_pointer_t<std::remove_const_t<meta_wrapper_t<V>>>> substitute{};
                     auto copy_it{it};
                     parse<JSON>::op<ws_handled<Options>()>(substitute, ctx, it, end);
                     static constexpr auto const_value{*meta_wrapper_v<V>};
                     if (substitute == const_value) {
                        found_match = true;
                        if (!std::holds_alternative<V>(value)) {
                           value = V{};
                        }
                     }
                     else {
                        if constexpr (not Options.null_terminated) {
                           if (ctx.error == error_code::end_reached) {
                              ctx.error = error_code::none;
                           }
                        }
                        it = copy_it;
                     }
                  }
               });
               if (found_match) {
                  return;
               }
            }

            // Second pass: non-const types in this category
            if constexpr (non_const_count > 0) {
               // Track position within matching types for "is last" check.
               // We iterate over all variant indices but only process matching types,
               // so we need runtime tracking (vs the old tuple-based approach where
               // the loop index directly corresponded to position in filtered set).
               // This has negligible cost: one increment per matching type.
               size_t non_const_idx = 0;

               for_each<N>([&]<size_t I>() {
                  if (found_match) {
                     return;
                  }
                  using V = std::variant_alternative_t<I, Variant>;
                  if constexpr (Trait<V>::value && !glaze_const_value_t<V>) {
                     auto copy_it{it};
                     if (!std::holds_alternative<V>(value)) {
                        value = V{};
                     }
                     parse<JSON>::op<ws_handled<Options>()>(std::get<V>(value), ctx, it, end);
                     if (!bool(ctx.error)) {
                        found_match = true;
                     }
                     else if constexpr (not Options.null_terminated) {
                        constexpr bool is_complete_type =
                           num_t<V> || bool_t<V> || string_t<V> || std::is_enum_v<V> || tuple_t<V> || is_std_tuple<V>;

                        if constexpr (is_complete_type) {
                           if (ctx.error == error_code::end_reached && it > copy_it) {
                              found_match = true;
                              ctx.error = error_code::none;
                           }
                           else {
                              it = copy_it;
                              if (non_const_idx + 1 < non_const_count) {
                                 ctx.error = error_code::none;
                              }
                           }
                        }
                        else {
                           it = copy_it;
                           if (non_const_idx + 1 < non_const_count) {
                              ctx.error = error_code::none;
                           }
                        }
                     }
                     else {
                        it = copy_it;
                        if (non_const_idx + 1 < non_const_count) {
                           ctx.error = error_code::none;
                        }
                     }
                     ++non_const_idx;
                  }
               });
               if (!found_match) {
                  if constexpr (non_const_count == 1) {
                     // Keep the specific error from the single type we tried
                  }
                  else {
                     ctx.error = error_code::no_matching_variant_type;
                  }
               }
            }
            else {
               ctx.error = error_code::no_matching_variant_type;
            }
         }
      }
   };

   template <is_variant T>
   struct from<JSON, T>
   {
      // Note that items in the variant are required to be default constructable for us to switch types
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (variant_is_auto_deducible<T>()) {
            if constexpr (not check_ws_handled(Options)) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }

            switch (*it) {
            case '\0':
               ctx.error = error_code::unexpected_end;
               return;
            case '{':
               if (ctx.depth >= max_recursive_depth_limit) {
                  ctx.error = error_code::exceeded_max_recursive_depth;
                  return;
               }
               // In the null terminated case this guards for stack overflow
               // Depth counting is done at the object level when not null terminated
               ++ctx.depth;

               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               using type_counts = variant_type_count<T>;
               if constexpr ((type_counts::n_object < 1) //
                             && (type_counts::n_nullable_object < 1)) {
                  ctx.error = error_code::no_matching_variant_type;
                  return;
               }
               else if constexpr ((type_counts::n_object + type_counts::n_nullable_object) == 1 && tag_v<T>.empty()) {
                  constexpr auto first_idx = variant_first_index_v<T, is_variant_object>;
                  using V = std::variant_alternative_t<first_idx, T>;
                  if (!std::holds_alternative<V>(value)) value = V{};
                  parse<JSON>::op<opening_handled<Opts>()>(std::get<V>(value), ctx, it, end);
                  if constexpr (Opts.null_terminated) {
                     // In the null terminated case this guards for stack overflow
                     // Depth counting is done at the object level when not null terminated
                     --ctx.depth;
                  }
                  return;
               }
               else {
                  auto possible_types = bit_array<std::variant_size_v<T>>{}.flip();
                  static constexpr auto& deduction_bits = variant_deduction_bits<T>;
                  static constexpr size_t deduction_key_count = variant_deduction_key_count<T>;
                  static constexpr auto tag_literal = string_literal_from_view<tag_v<T>.size()>(tag_v<T>);

                  // Track if we've encountered a tag and what value it had
                  std::optional<size_t> tag_specified_index{};

                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  auto start = it;
                  while (*it != '}') {
                     if (it != start) {
                        if (match_invalid_end<',', Opts>(ctx, it, end)) {
                           return;
                        }
                     }

                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                        return;
                     }

                     auto* key_start = it;
                     skip_string_view(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     const sv key = {key_start, size_t(it - key_start)};

                     if (match_invalid_end<'"', Opts>(ctx, it, end)) {
                        return;
                     }

                     if constexpr (deduction_key_count > 0) {
                        // We first check if a tag is defined and see if the key matches the tag
                        if constexpr (not tag_v<T>.empty()) {
                           if (key == tag_v<T>) {
                              if (parse_ws_colon<Opts>(ctx, it, end)) {
                                 return;
                              }

                              using id_type = std::decay_t<decltype(ids_v<T>[0])>;

                              std::conditional_t<std::integral<id_type>, id_type, sv> type_id{};
                              if constexpr (std::integral<id_type>) {
                                 from<JSON, id_type>::template op<ws_handled<Opts>()>(type_id, ctx, it, end);
                              }
                              else {
                                 from<JSON, sv>::template op<ws_handled<Opts>()>(type_id, ctx, it, end);
                              }
                              if (bool(ctx.error)) [[unlikely]]
                                 return;
                              if (skip_ws<Opts>(ctx, it, end)) {
                                 return;
                              }
                              if (!(*it == ',' || *it == '}')) [[unlikely]] {
                                 ctx.error = error_code::syntax_error;
                                 return;
                              }

                              size_t type_index;
                              if constexpr (std::integral<id_type>) {
                                 type_index = variant_id_to_index<T>::op(type_id);
                              }
                              else {
                                 type_index = variant_id_to_index<T>::op(
                                    type_id.data(), type_id.data() + type_id.size(), type_id.size());
                              }
                              if (type_index < ids_v<T>.size()) [[likely]] {
                                 // Check if the deduced types include the tag-specified type
                                 // If not, the tag doesn't match the fields
                                 // We're already inside if constexpr (deduction_registry.size()), so we know deduction
                                 // is happening. At this point, possible_types has been narrowed by field deduction.
                                 // Check if the tag-specified type is still possible
                                 const bool type_is_possible = possible_types[type_index];
                                 if (!type_is_possible) {
                                    // Tag specifies a type that doesn't match the fields seen so far
                                    ctx.error = error_code::no_matching_variant_type;
                                    return;
                                 }

                                 it = start; // we restart our object parsing now that we know the target type
                                 tag_specified_index = type_index; // Store the tag-specified type
                                 if (value.index() != type_index) emplace_runtime_variant(value, type_index);
                                 std::visit(
                                    [&](auto&& v) {
                                       using V = std::decay_t<decltype(v)>;
                                       constexpr bool is_object = glaze_object_t<V> || reflectable<V>;
                                       if constexpr (is_object) {
                                          from<JSON, V>::template op<opening_handled<Opts>(), tag_literal>(v, ctx, it,
                                                                                                           end);
                                       }
                                       else if constexpr (is_memory_object<V>) {
                                          if (!v) {
                                             if constexpr (is_specialization_v<V, std::optional>) {
                                                if constexpr (requires { v.emplace(); }) {
                                                   v.emplace();
                                                }
                                                else {
                                                   v = typename V::value_type{};
                                                }
                                             }
                                             else if constexpr (is_specialization_v<V, std::unique_ptr>)
                                                v = std::make_unique<typename V::element_type>();
                                             else if constexpr (is_specialization_v<V, std::shared_ptr>)
                                                v = std::make_shared<typename V::element_type>();
                                             else if constexpr (constructible<V>) {
                                                v = meta_construct_v<V>();
                                             }
                                             else if constexpr (std::is_pointer_v<V> &&
                                                                can_allocate_raw_pointer<Opts,
                                                                                         std::decay_t<decltype(ctx)>>) {
                                                if (!try_allocate_raw_pointer<Opts>(v, ctx)) {
                                                   return;
                                                }
                                             }
                                             else {
                                                ctx.error = error_code::invalid_nullable_read;
                                                return;
                                                // Cannot read into unset nullable that is not std::optional,
                                                // std::unique_ptr, or std::shared_ptr
                                             }
                                          }
                                          from<JSON, memory_type<V>>::template op<opening_handled<Opts>(), tag_literal>(
                                             *v, ctx, it, end);
                                       }
                                    },
                                    value);

                                 if constexpr (Opts.null_terminated) {
                                    // In the null terminated case this guards for stack overflow
                                    // Depth counting is done at the object level when not null terminated
                                    --ctx.depth;
                                 }
                                 return; // we've decoded our target type
                              }
                              else [[unlikely]] {
                                 // Check if we have a default type (ids array shorter than variant)
                                 constexpr auto ids_size = ids_v<T>.size();
                                 constexpr auto variant_size = std::variant_size_v<T>;
                                 if constexpr (ids_size < variant_size) {
                                    // Use the first unlabeled type as the default
                                    const auto type_index = ids_size;

                                    it = start; // we restart our object parsing now that we know the target type
                                    tag_specified_index = type_index; // Store the default type index
                                    if (value.index() != type_index) emplace_runtime_variant(value, type_index);
                                    std::visit(
                                       [&](auto&& v) {
                                          using V = std::decay_t<decltype(v)>;
                                          constexpr bool is_object = glaze_object_t<V> || reflectable<V>;
                                          if constexpr (is_object) {
                                             from<JSON, V>::template op<opening_handled<Opts>()>(v, ctx, it, end);
                                          }
                                          else if constexpr (is_memory_object<V>) {
                                             if (!v) {
                                                if constexpr (is_specialization_v<V, std::optional>) {
                                                   if constexpr (requires { v.emplace(); }) {
                                                      v.emplace();
                                                   }
                                                   else {
                                                      v = typename V::value_type{};
                                                   }
                                                }
                                                else if constexpr (is_specialization_v<V, std::unique_ptr>)
                                                   v = std::make_unique<typename V::element_type>();
                                                else if constexpr (is_specialization_v<V, std::shared_ptr>)
                                                   v = std::make_shared<typename V::element_type>();
                                                else if constexpr (constructible<V>) {
                                                   v = meta_construct_v<V>();
                                                }
                                                else if constexpr (std::is_pointer_v<V> &&
                                                                   can_allocate_raw_pointer<
                                                                      Opts, std::decay_t<decltype(ctx)>>) {
                                                   if (!try_allocate_raw_pointer<Opts>(v, ctx)) {
                                                      return;
                                                   }
                                                }
                                                else {
                                                   ctx.error = error_code::invalid_nullable_read;
                                                   return;
                                                }
                                             }
                                             from<JSON, memory_type<V>>::template op<opening_handled<Opts>()>(*v, ctx,
                                                                                                              it, end);
                                          }
                                       },
                                       value);

                                    if constexpr (Opts.null_terminated) {
                                       --ctx.depth;
                                    }
                                    return;
                                 }
                                 else {
                                    ctx.error = error_code::no_matching_variant_type;
                                    return;
                                 }
                              }
                           }
                        }

                        // Inline decode_hash lookup for variant deduction
                        using deduction_keys_t = keys_wrapper<variant_deduction_keys<T>>;
                        constexpr auto& DeductionHashInfo = hash_info<deduction_keys_t>;
                        const auto deduction_index =
                           decode_hash_with_size<JSON, deduction_keys_t, DeductionHashInfo, DeductionHashInfo.type>::op(
                              key.data(), key.data() + key.size(), key.size());
                        if (deduction_index < deduction_key_count && variant_deduction_keys<T>[deduction_index] == key)
                           [[likely]] {
                           possible_types &= deduction_bits[deduction_index];
                        }
                        else if constexpr (Opts.error_on_unknown_keys) {
                           ctx.error = error_code::unknown_key;
                           return;
                        }
                     }
                     else if constexpr (not tag_v<T>.empty()) {
                        // empty object case for variant, if there are no normal elements
                        if (key == tag_v<T>) {
                           if (parse_ws_colon<Opts>(ctx, it, end)) {
                              return;
                           }

                           std::string_view type_id{};
                           parse<JSON>::op<ws_handled<Opts>()>(type_id, ctx, it, end);
                           if (bool(ctx.error)) [[unlikely]]
                              return;
                           if (skip_ws<Opts>(ctx, it, end)) {
                              return;
                           }

                           const auto type_index = variant_id_to_index<T>::op(
                              type_id.data(), type_id.data() + type_id.size(), type_id.size());
                           if (type_index < ids_v<T>.size()) [[likely]] {
                              it = start;
                              tag_specified_index = type_index; // Store the tag-specified type
                              if (value.index() != type_index) emplace_runtime_variant(value, type_index);
                           }
                           else {
                              // Check if we have a default type (ids array shorter than variant)
                              constexpr auto ids_size = ids_v<T>.size();
                              constexpr auto variant_size = std::variant_size_v<T>;
                              if constexpr (ids_size < variant_size) {
                                 // Use the first unlabeled type as the default
                                 it = start;
                                 const auto default_index = ids_size;
                                 tag_specified_index = default_index; // Store the default type index
                                 if (value.index() != default_index) emplace_runtime_variant(value, default_index);
                              }
                              else {
                                 ctx.error = error_code::no_matching_variant_type;
                                 return;
                              }
                           }
                           // Parse the empty type (handles tag skipping and unknown keys)
                           std::visit(
                              [&](auto&& v) {
                                 using V = std::decay_t<decltype(v)>;
                                 from<JSON, V>::template op<opening_handled<Opts>(), tag_literal>(v, ctx, it, end);
                              },
                              value);
                           if constexpr (Opts.null_terminated) {
                              --ctx.depth;
                           }
                           return;
                        }
                        else if constexpr (Opts.error_on_unknown_keys) {
                           ctx.error = error_code::unknown_key;
                           return;
                        }
                        else {
                           // Skip unknown key's value (non-tag key in empty variant)
                           if (parse_ws_colon<Opts>(ctx, it, end)) {
                              return;
                           }
                           skip_value<JSON>::op<Opts>(ctx, it, end);
                           if (bool(ctx.error)) [[unlikely]]
                              return;
                           if (skip_ws<Opts>(ctx, it, end)) {
                              return;
                           }
                           continue; // Continue loop to find the tag
                        }
                     }
                     else if constexpr (Opts.error_on_unknown_keys) {
                        ctx.error = error_code::unknown_key;
                        return;
                     }

                     auto matching_types = possible_types.popcount();
                     if (matching_types == 0) {
                        ctx.error = error_code::no_matching_variant_type;
                        return;
                     }
                     // Only short-circuit for variants without tags
                     else if constexpr (tag_v<T>.empty()) {
                        if (matching_types == 1) {
                           it = start;
                           const auto type_index = possible_types.countr_zero();

                           if (value.index() != static_cast<size_t>(type_index))
                              emplace_runtime_variant(value, type_index);
                           std::visit(
                              [&](auto&& v) {
                                 using V = std::decay_t<decltype(v)>;
                                 constexpr bool is_object = glaze_object_t<V> || reflectable<V>;
                                 if constexpr (is_object) {
                                    from<JSON, V>::template op<opening_handled<Opts>(), tag_literal>(v, ctx, it, end);
                                 }
                                 else if constexpr (is_memory_object<V>) {
                                    if (!v) {
                                       if constexpr (is_specialization_v<V, std::optional>) {
                                          if constexpr (requires { v.emplace(); }) {
                                             v.emplace();
                                          }
                                          else {
                                             v = typename V::value_type{};
                                          }
                                       }
                                       else if constexpr (is_specialization_v<V, std::unique_ptr>)
                                          v = std::make_unique<typename V::element_type>();
                                       else if constexpr (is_specialization_v<V, std::shared_ptr>)
                                          v = std::make_shared<typename V::element_type>();
                                       else if constexpr (constructible<V>) {
                                          v = meta_construct_v<V>();
                                       }
                                       else if constexpr (std::is_pointer_v<V> &&
                                                          can_allocate_raw_pointer<Opts, std::decay_t<decltype(ctx)>>) {
                                          if (!try_allocate_raw_pointer<Opts>(v, ctx)) {
                                             return;
                                          }
                                       }
                                       else {
                                          ctx.error = error_code::invalid_nullable_read;
                                          return;
                                          // Cannot read into unset nullable that is not std::optional,
                                          // std::unique_ptr, or std::shared_ptr
                                       }
                                    }
                                    from<JSON, memory_type<V>>::template op<opening_handled<Opts>(), tag_literal>(
                                       *v, ctx, it, end);
                                 }
                              },
                              value);

                           if constexpr (Opts.null_terminated) {
                              // In the null terminated case this guards for stack overflow
                              // Depth counting is done at the object level when not null terminated
                              --ctx.depth;
                           }
                           return; // we've decoded our target type
                        }
                     }
                     // For tagged variants, continue processing to validate tags
                     if (parse_ws_colon<Opts>(ctx, it, end)) {
                        return;
                     }

                     skip_value<JSON>::op<Opts>(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                  }
                  // Only apply ambiguous variant resolution if we have multiple object types
                  if constexpr ((type_counts::n_object + type_counts::n_nullable_object) > 1) {
                     // After parsing all keys, check if we have multiple matching types
                     // If so, choose the one with the fewest fields
                     auto final_matching = possible_types.popcount();
                     if (final_matching == 0) {
                        ctx.error = error_code::no_matching_variant_type;
                     }
                     else if (final_matching == 1) {
                        // Single type remains after field deduction
                        const auto type_index = possible_types.countr_zero();

                        // Validate against tag if one was specified
                        if (tag_specified_index.has_value() &&
                            tag_specified_index.value() != static_cast<size_t>(type_index)) {
                           ctx.error = error_code::no_matching_variant_type;
                           return;
                        }

                        it = start;
                        if (value.index() != static_cast<size_t>(type_index))
                           emplace_runtime_variant(value, type_index);
                        std::visit(
                           [&](auto&& v) {
                              using V = std::decay_t<decltype(v)>;
                              constexpr bool is_object = glaze_object_t<V> || reflectable<V>;
                              if constexpr (is_object) {
                                 from<JSON, V>::template op<opening_handled<Opts>(), tag_literal>(v, ctx, it, end);
                              }
                              else if constexpr (is_memory_object<V>) {
                                 if (!v) {
                                    if constexpr (is_specialization_v<V, std::optional>) {
                                       if constexpr (requires { v.emplace(); }) {
                                          v.emplace();
                                       }
                                       else {
                                          v = typename V::value_type{};
                                       }
                                    }
                                    else if constexpr (is_specialization_v<V, std::unique_ptr>)
                                       v = std::make_unique<typename V::element_type>();
                                    else if constexpr (is_specialization_v<V, std::shared_ptr>)
                                       v = std::make_shared<typename V::element_type>();
                                    else if constexpr (constructible<V>) {
                                       v = meta_construct_v<V>();
                                    }
                                    else if constexpr (std::is_pointer_v<V> &&
                                                       can_allocate_raw_pointer<Opts, std::decay_t<decltype(ctx)>>) {
                                       if (!try_allocate_raw_pointer<Opts>(v, ctx)) {
                                          return;
                                       }
                                    }
                                    else {
                                       ctx.error = error_code::invalid_nullable_read;
                                       return;
                                    }
                                 }
                                 from<JSON, memory_type<V>>::template op<opening_handled<Opts>(), tag_literal>(*v, ctx,
                                                                                                               it, end);
                              }
                           },
                           value);

                        if constexpr (Opts.null_terminated) {
                           --ctx.depth;
                        }
                     }
                     else if (final_matching > 1) {
                        constexpr auto N = std::variant_size_v<T>;

                        // Compile-time array of field counts for each variant type
                        constexpr auto field_counts = []<size_t... I>(std::index_sequence<I...>) {
                           return std::array<size_t, N> {
                              ([]<size_t J = I>() -> size_t {
                                 using V = std::decay_t<std::variant_alternative_t<J, T>>;
                                 if constexpr (glaze_object_t<V> || reflectable<V>) {
                                    return reflect<V>::size;
                                 }
                                 else if constexpr (is_memory_object<V>) {
                                    using X = memory_type<V>;
                                    if constexpr (glaze_object_t<X> || reflectable<X>) {
                                       return reflect<X>::size;
                                    }
                                    else {
                                       return std::numeric_limits<size_t>::max();
                                    }
                                 }
                                 else {
                                    return std::numeric_limits<size_t>::max();
                                 }
                              }.template operator()<I>())...
                           };
                        }(std::make_index_sequence<N>{});

                        // Find the type with minimum field count among the possible types
                        size_t min_fields = std::numeric_limits<size_t>::max();
                        size_t chosen_index = N; // Invalid index initially

                        for (size_t i = 0; i < N; ++i) {
                           if (possible_types[i] && field_counts[i] < min_fields) {
                              min_fields = field_counts[i];
                              chosen_index = i;
                           }
                        }

                        if (chosen_index < N) {
                           // Validate against tag if one was specified
                           if (tag_specified_index.has_value() && tag_specified_index.value() != chosen_index) {
                              ctx.error = error_code::no_matching_variant_type;
                              return;
                           }

                           it = start;
                           if (value.index() != chosen_index) emplace_runtime_variant(value, chosen_index);
                           std::visit(
                              [&](auto&& v) {
                                 using V = std::decay_t<decltype(v)>;
                                 constexpr bool is_object = glaze_object_t<V> || reflectable<V>;
                                 if constexpr (is_object) {
                                    from<JSON, V>::template op<opening_handled<Opts>(), tag_literal>(v, ctx, it, end);
                                 }
                                 else if constexpr (is_memory_object<V>) {
                                    if (!v) {
                                       if constexpr (is_specialization_v<V, std::optional>) {
                                          if constexpr (requires { v.emplace(); }) {
                                             v.emplace();
                                          }
                                          else {
                                             v = typename V::value_type{};
                                          }
                                       }
                                       else if constexpr (is_specialization_v<V, std::unique_ptr>)
                                          v = std::make_unique<typename V::element_type>();
                                       else if constexpr (is_specialization_v<V, std::shared_ptr>)
                                          v = std::make_shared<typename V::element_type>();
                                       else if constexpr (constructible<V>) {
                                          v = meta_construct_v<V>();
                                       }
                                       else if constexpr (std::is_pointer_v<V> &&
                                                          can_allocate_raw_pointer<Opts, std::decay_t<decltype(ctx)>>) {
                                          if (!try_allocate_raw_pointer<Opts>(v, ctx)) {
                                             return;
                                          }
                                       }
                                       else {
                                          ctx.error = error_code::invalid_nullable_read;
                                          return;
                                       }
                                    }
                                    from<JSON, memory_type<V>>::template op<opening_handled<Opts>(), tag_literal>(
                                       *v, ctx, it, end);
                                 }
                              },
                              value);

                           if constexpr (Opts.null_terminated) {
                              --ctx.depth;
                           }
                        }
                        else {
                           ctx.error = error_code::no_matching_variant_type;
                        }
                     }
                  }
                  else {
                     // For variants with 0 or 1 object types, use the original error handling
                     ctx.error = error_code::no_matching_variant_type;
                  }
               }
               break;
            case '[':
               if (ctx.depth >= max_recursive_depth_limit) {
                  ctx.error = error_code::exceeded_max_recursive_depth;
                  return;
               }
               if constexpr (Opts.null_terminated) {
                  // In the null terminated case this guards for stack overflow
                  // Depth counting is done at the object level when not null terminated
                  ++ctx.depth;
               }
               process_variant_alternatives<T, is_variant_array>::template op<Opts>(value, ctx, it, end);
               if constexpr (Opts.null_terminated) {
                  --ctx.depth;
               }
               break;
            case '"': {
               process_variant_alternatives<T, is_variant_str>::template op<Opts>(value, ctx, it, end);
               break;
            }
            case 't':
            case 'f': {
               process_variant_alternatives<T, is_variant_bool>::template op<Opts>(value, ctx, it, end);
               break;
            }
            case 'n':
               if constexpr (variant_count_v<T, is_variant_null> == 0) {
                  ctx.error = error_code::no_matching_variant_type;
               }
               else {
                  constexpr auto first_idx = variant_first_index_v<T, is_variant_null>;
                  using V = std::variant_alternative_t<first_idx, T>;
                  if (!std::holds_alternative<V>(value)) value = V{};
                  match<"null", Opts>(ctx, it, end);
               }
               break;
            default: {
               // Not bool, string, object, or array so must be number or null
               process_variant_alternatives<T, is_variant_num>::template op<Opts>(value, ctx, it, end);
            }
            }
         }
         else {
            // For non-auto-deducible variants, try each type until one succeeds
            constexpr auto N = std::variant_size_v<T>;
            bool parsed = false;

            for_each<N>([&]<size_t I>() {
               if (parsed) return;

               auto copy_it = it;

               // Try parsing as this type
               if (value.index() != I) {
                  emplace_runtime_variant(value, I);
               }

               std::visit([&](auto&& v) { parse<JSON>::op<Options>(v, ctx, it, end); }, value);

               if (!bool(ctx.error)) {
                  parsed = true;
               }
               else if constexpr (not Options.null_terminated) {
                  // In non-null-terminated mode, if we hit end_reached after advancing the iterator,
                  // it means we successfully parsed the value but couldn't skip trailing whitespace
                  if (ctx.error == error_code::end_reached && it > copy_it) {
                     // We advanced the iterator, so we did parse something
                     parsed = true;
                     ctx.error = error_code::none;
                  }
                  else {
                     // Reset for next attempt (unless this is the last type)
                     it = copy_it;
                     if constexpr (I + 1 < N) {
                        ctx.error = error_code::none; // Clear error for next attempt
                     }
                  }
               }
               else {
                  // Reset for next attempt (unless this is the last type)
                  it = copy_it;
                  if constexpr (I + 1 < N) {
                     ctx.error = error_code::none; // Clear error for next attempt
                  }
               }
            });
         }
      }
   };

   template <class T>
   struct from<JSON, array_variant_wrapper<T>>
   {
      template <auto Options>
      static void op(auto&& wrapper, is_context auto&& ctx, auto&& it, auto end)
      {
         auto& value = wrapper.value;

         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         // TODO Use key parsing for compiletime known keys
         if (match_invalid_end<'"', Opts>(ctx, it, end)) {
            return;
         }
         auto start = it;
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         sv type_id = {start, size_t(it - start)};
         if (match<'"'>(ctx, it)) {
            return;
         }
         if constexpr (not Opts.null_terminated) {
            if (it == end) {
               ctx.error = error_code::end_reached;
               return;
            }
         }

         const auto type_index =
            variant_id_to_index<T>::op(type_id.data(), type_id.data() + type_id.size(), type_id.size());
         if (type_index < ids_v<T>.size()) [[likely]] {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (match_invalid_end<',', Opts>(ctx, it, end)) {
               return;
            }
            if (value.index() != type_index) emplace_runtime_variant(value, type_index);
            std::visit([&](auto&& v) { parse<JSON>::op<Opts>(v, ctx, it, end); }, value);
            if (bool(ctx.error)) [[unlikely]]
               return;
         }
         else {
            ctx.error = error_code::no_matching_variant_type;
            return;
         }

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
         match<']'>(ctx, it);
         if constexpr (not Opts.null_terminated) {
            --ctx.depth;
         }
      }
   };

   template <is_expected T>
   struct from<JSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if constexpr (!check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (*it == '{') {
            if constexpr (not Opts.null_terminated) {
               ++ctx.depth;
            }
            auto start = it;
            ++it;
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (*it == '}') {
               it = start;
               // empty object
               if (value) {
                  parse<JSON>::op<Opts>(*value, ctx, it, end);
               }
               else {
                  value.emplace();
                  parse<JSON>::op<Opts>(*value, ctx, it, end);
               }
            }
            else {
               // either we have an unexpected value or we are decoding an object
               auto& key = string_buffer();
               parse<JSON>::op<Opts>(key, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               if (key == "unexpected") {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  if (match_invalid_end<':', Opts>(ctx, it, end)) {
                     return;
                  }
                  // read in unexpected value
                  if (!value) {
                     parse<JSON>::op<Opts>(value.error(), ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
                  else {
                     // set value to unexpected
                     using error_ctx = typename std::decay_t<decltype(value)>::error_type;
                     std::decay_t<error_ctx> error{};
                     parse<JSON>::op<Opts>(error, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     value = glz::unexpected(error);
                  }
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  match<'}'>(ctx, it);
                  if constexpr (not Opts.null_terminated) {
                     --ctx.depth;
                  }
               }
               else {
                  it = start;
                  if (value) {
                     parse<JSON>::op<Opts>(*value, ctx, it, end);
                  }
                  else {
                     value.emplace();
                     parse<JSON>::op<Opts>(*value, ctx, it, end);
                  }
               }
            }
         }
         else {
            // this is not an object and therefore cannot be an unexpected value
            if (value) {
               parse<JSON>::op<Opts>(*value, ctx, it, end);
            }
            else {
               value.emplace();
               parse<JSON>::op<Opts>(*value, ctx, it, end);
            }
         }
      }
   };

   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class V, size_t N>
      GLZ_ALWAYS_INLINE static void op(V (&value)[N], is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         parse<JSON>::op<Opts>(std::span{value, N}, ctx, it, end);
      }
   };

   template <class T>
      requires((nullable_t<T> || nullable_value_t<T>) && not is_expected<T> && not std::is_array_v<T> &&
               not custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }

         if (*it == 'n') {
            ++it;
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            match<"ull", Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            if constexpr (requires { value.reset(); }) {
               value.reset();
            }
         }
         else {
            if constexpr (nullable_value_t<T>) {
               if (not value.has_value()) {
                  if constexpr (constructible<T>) {
                     value = meta_construct_v<T>();
                  }
                  else if constexpr (requires { value.emplace(); }) {
                     value.emplace();
                  }
                  else {
                     static_assert(false_v<T>,
                                   "Your nullable type must have `emplace()` or be glz::meta constructible, or "
                                   "create a custom glz::from specialization");
                  }
               }
               parse<JSON>::op<Opts>(value.value(), ctx, it, end);
            }
            else {
               if (!value) {
                  if constexpr (optional_like<T>) {
                     if constexpr (requires { value.emplace(); }) {
                        value.emplace();
                     }
                     else {
                        value = typename T::value_type{};
                     }
                  }
                  else if constexpr (is_specialization_v<T, std::unique_ptr>)
                     value = std::make_unique<typename T::element_type>();
                  else if constexpr (is_specialization_v<T, std::shared_ptr>)
                     value = std::make_shared<typename T::element_type>();
                  else if constexpr (constructible<T>) {
                     value = meta_construct_v<T>();
                  }
                  else if constexpr (std::is_pointer_v<T> &&
                                     can_allocate_raw_pointer<Opts, std::decay_t<decltype(ctx)>>) {
                     if (!try_allocate_raw_pointer<Opts>(value, ctx)) {
                        return;
                     }
                  }
                  else {
                     // Cannot read into a null raw pointer
                     ctx.error = error_code::invalid_nullable_read;
                     return;
                  }
               }
               parse<JSON>::op<Opts>(*value, ctx, it, end);
            }
         }
      }
   };

   template <filesystem_path T>
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         std::string& buffer = string_buffer();
         parse<JSON>::op<Opts>(buffer, ctx, it, end);
         if constexpr (Opts.null_terminated) {
            if (bool(ctx.error)) [[unlikely]]
               return;
         }
         else {
            if (size_t(ctx.error) > size_t(error_code::end_reached)) [[unlikely]] {
               return;
            }
         }
         value = buffer;
      }
   };

   // ============================================
   // std::chrono deserialization
   // ============================================

   // Duration: parse from count
   template <is_duration T>
      requires(not custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         using Rep = typename std::remove_cvref_t<T>::rep;
         Rep count{};
         from<JSON, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value = std::remove_cvref_t<T>(count);
      }
   };

   // system_clock::time_point: parse from ISO 8601 string
   // Fast manual parser - no heap allocations, direct character parsing
   template <is_system_time_point T>
      requires(not custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It0, class It1>
      static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         std::string_view str;
         from<JSON, std::string_view>::template op<Opts>(str, ctx, it, end);

         if (bool(ctx.error)) [[unlikely]]
            return;

         // Minimum: YYYY-MM-DDTHH:MM:SSZ = 20 chars
         if (str.size() < 20) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         const char* s = str.data();
         const auto n = str.size();

         // Helper to parse N digits
         auto parse_digits = [&s](size_t start, size_t count) -> int {
            int val = 0;
            for (size_t i = 0; i < count; ++i) {
               const char c = s[start + i];
               if (c < '0' || c > '9') return -1;
               val = val * 10 + (c - '0');
            }
            return val;
         };

         // YYYY-MM-DDTHH:MM:SS
         const int yr = parse_digits(0, 4);
         const int mo = parse_digits(5, 2);
         const int dy = parse_digits(8, 2);
         const int hr = parse_digits(11, 2);
         const int mi = parse_digits(14, 2);
         const int sc = parse_digits(17, 2);

         if (yr < 0 || mo < 0 || dy < 0 || hr < 0 || mi < 0 || sc < 0 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
             s[13] != ':' || s[16] != ':') [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         // Validate ranges (leap seconds not supported - std::chrono::system_clock uses Unix time)
         if (mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr > 23 || mi > 59 || sc > 59) [[unlikely]] {
            ctx.error = error_code::parse_error;
            return;
         }

         // Parse optional fractional seconds
         size_t pos = 19;
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

         // Parse timezone: Z or +HH:MM or -HH:MM (defaults to UTC if missing)
         int tz_offset_seconds = 0;
         if (pos < n) {
            if (s[pos] == 'Z') {
               // UTC
            }
            else if (s[pos] == '+' || s[pos] == '-') {
               // +05:00 means local is ahead of UTC, so subtract to get UTC (multiply by -1)
               // -08:00 means local is behind UTC, so add to get UTC (multiply by +1)
               const int utc_adjustment = (s[pos] == '+') ? -1 : 1;
               ++pos;
               if (pos + 2 > n) [[unlikely]] {
                  ctx.error = error_code::parse_error;
                  return;
               }
               const int tz_hour = parse_digits(pos, 2);
               if (tz_hour < 0 || tz_hour > 23) [[unlikely]] {
                  ctx.error = error_code::parse_error;
                  return;
               }
               pos += 2;
               int tz_min = 0;
               if (pos < n && s[pos] == ':') ++pos;
               if (pos + 2 <= n) {
                  const int m = parse_digits(pos, 2);
                  if (m < 0 || m > 59) [[unlikely]] {
                     ctx.error = error_code::parse_error;
                     return;
                  }
                  tz_min = m;
                  pos += 2;
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

   // steady_clock::time_point: parse as count in the time_point's native duration
   template <is_steady_time_point T>
      requires(not custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         Rep count{};
         from<JSON, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value = std::remove_cvref_t<T>(Duration(count));
      }
   };

   // high_resolution_clock::time_point when it's a distinct type (rare)
   template <is_high_res_time_point T>
      requires(not custom_read<T>)
   struct from<JSON, T>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         // Treat like steady_clock - parse as count
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         Rep count{};
         from<JSON, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         value = std::remove_cvref_t<T>(Duration(count));
      }
   };

   // epoch_time wrapper: parse as numeric Unix timestamp
   template <class Duration>
      requires(not custom_read<epoch_time<Duration>>)
   struct from<JSON, epoch_time<Duration>>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& wrapper, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         using Rep = typename Duration::rep;
         Rep count{};
         from<JSON, Rep>::template op<Opts>(count, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         // Use duration_cast to handle precision differences between Duration and system_clock::duration
         using sys_duration = std::chrono::system_clock::duration;
         wrapper.value =
            std::chrono::system_clock::time_point{std::chrono::duration_cast<sys_duration>(Duration{count})};
      }
   };

   struct opts_validate : opts
   {
      bool validate_skipped = true;
      bool validate_trailing_whitespace = true;
   };

   template <is_buffer Buffer>
   [[nodiscard]] error_ctx validate_json(Buffer&& buffer) noexcept
   {
      context ctx{};
      glz::skip skip_value{};
      return read<opts_validate{}>(skip_value, std::forward<Buffer>(buffer), ctx);
   }

   template <is_buffer Buffer>
   [[nodiscard]] error_ctx validate_jsonc(Buffer&& buffer) noexcept
   {
      context ctx{};
      glz::skip skip_value{};
      return read<opts_validate{{opts{.comments = true}}}>(skip_value, std::forward<Buffer>(buffer), ctx);
   }

   template <read_supported<JSON> T, is_buffer Buffer>
      requires(!is_input_streaming<std::remove_reference_t<Buffer>>)
   [[nodiscard]] error_ctx read_json(T& value, Buffer&& buffer)
   {
      context ctx{};
      return read<opts{}>(value, std::forward<Buffer>(buffer), ctx);
   }

   // Overload for streaming input buffers (istream_buffer)
   template <read_supported<JSON> T, class Buffer>
      requires is_input_streaming<std::remove_reference_t<Buffer>>
   [[nodiscard]] error_ctx read_json(T& value, Buffer&& buffer)
   {
      return read_streaming<opts{}>(value, std::forward<Buffer>(buffer));
   }

   template <read_supported<JSON> T, is_buffer Buffer>
      requires(!is_input_streaming<std::remove_reference_t<Buffer>>)
   [[nodiscard]] expected<T, error_ctx> read_json(Buffer&& buffer)
   {
      T value{};
      context ctx{};
      const error_ctx ec = read<opts{}>(value, std::forward<Buffer>(buffer), ctx);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return value;
   }

   // Overload for streaming input buffers (istream_buffer)
   template <read_supported<JSON> T, class Buffer>
      requires is_input_streaming<std::remove_reference_t<Buffer>>
   [[nodiscard]] expected<T, error_ctx> read_json(Buffer&& buffer)
   {
      T value{};
      const error_ctx ec = read_streaming<opts{}>(value, std::forward<Buffer>(buffer));
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return value;
   }

   template <read_supported<JSON> T, is_buffer Buffer>
   [[nodiscard]] error_ctx read_jsonc(T& value, Buffer&& buffer)
   {
      context ctx{};
      return read<opts{.comments = true}>(value, std::forward<Buffer>(buffer), ctx);
   }

   template <read_supported<JSON> T, is_buffer Buffer>
   [[nodiscard]] expected<T, error_ctx> read_jsonc(Buffer&& buffer)
   {
      T value{};
      context ctx{};
      const error_ctx ec = read<opts{.comments = true}>(value, std::forward<Buffer>(buffer), ctx);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return value;
   }

   template <auto Opts = opts{}, read_supported<JSON> T, is_buffer Buffer>
   [[nodiscard]] error_ctx read_file_json(T& value, const sv file_name, Buffer&& buffer)
   {
      context ctx{};
      ctx.current_file = file_name;

      const auto ec = file_to_buffer(buffer, ctx.current_file);

      if (bool(ec)) {
         return {0, ec};
      }

      return read<set_json<Opts>()>(value, buffer, ctx);
   }

   template <auto Opts = opts{}, read_supported<JSON> T, is_buffer Buffer>
   [[nodiscard]] error_ctx read_file_jsonc(T& value, const sv file_name, Buffer&& buffer)
   {
      context ctx{};
      ctx.current_file = file_name;

      const auto ec = file_to_buffer(buffer, ctx.current_file);

      if (bool(ec)) {
         return {0, ec};
      }

      constexpr auto Options = opt_true<set_json<Opts>(), &opts::comments>;
      return read<Options>(value, buffer, ctx);
   }
}

#if defined(_MSC_VER) && !defined(__clang__)
// restore disabled warnings
#pragma warning(pop)
#endif
