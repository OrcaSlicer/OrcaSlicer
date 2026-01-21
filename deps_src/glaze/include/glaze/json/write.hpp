// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <charconv>
#include <iterator>
#include <ostream>
#include <variant>

#include "glaze/core/chrono.hpp"
#include "glaze/util/parse.hpp"

#if !defined(GLZ_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif

#if defined(__AVX2__)
#define GLZ_USE_AVX2
#endif
#endif

#if defined(_MSC_VER) && !defined(__clang__)
// disable "unreachable code" warnings, which are often invalid due to constexpr branching
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

#include "glaze/core/buffer_traits.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/to.hpp"
#include "glaze/core/write.hpp"
#include "glaze/core/write_chars.hpp"
#include "glaze/json/ptr.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/itoa.hpp"

namespace glz
{
   // This serialize<JSON> indirection only exists to call std::remove_cvref_t on the type
   // so that type matching doesn't depend on qualifiers.
   // It is recommended to directly call to<JSON, std::remove_cvref_t<T>> to reduce compilation overhead.
   // TODO: Long term this can probably be [[deprecated]]
   // but it is useful for when getting the value type would be verbose
   template <>
   struct serialize<JSON>
   {
      template <auto Opts, class T, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<JSON, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                             std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   template <auto& Partial, auto Opts, class T, class Ctx, class B, class IX>
   concept write_json_partial_invocable = requires(T&& value, Ctx&& ctx, B&& b, IX&& ix) {
      to_partial<JSON, std::remove_cvref_t<T>>::template op<Partial, Opts>(
         std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
   };

   template <>
   struct serialize_partial<JSON>
   {
      template <auto& Partial, auto Opts, class T, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         if constexpr (std::count(Partial.begin(), Partial.end(), "") > 0) {
            serialize<JSON>::op<Opts>(value, ctx, b, ix);
         }
         else if constexpr (write_json_partial_invocable<Partial, Opts, T, Ctx, B, IX>) {
            to_partial<JSON, std::remove_cvref_t<T>>::template op<Partial, Opts>(
               std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
         }
         else {
            static_assert(false_v<T>, "Glaze metadata is probably needed for your type");
         }
      }
   };

   template <auto Opts, bool minified_check = true, class B>
      requires(Opts.format == JSON || Opts.format == NDJSON)
   GLZ_ALWAYS_INLINE void write_object_entry_separator(is_context auto&& ctx, B&& b, auto& ix)
   {
      if constexpr (Opts.prettify) {
         if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
            return;
         }
         std::memcpy(&b[ix], ",\n", 2);
         ix += 2;
         std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
         ix += ctx.depth;
      }
      else {
         if constexpr (minified_check) {
            if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
               return;
            }
         }
         std::memcpy(&b[ix], ",", 1);
         ++ix;
      }
      if constexpr (is_output_streaming<B>) {
         flush_buffer(b, ix);
      }
   }

   // Only object types are supported for partial
   template <class T>
      requires(glaze_object_t<T> || writable_map_t<T> || reflectable<T>)
   struct to_partial<JSON, T> final
   {
      template <auto& Partial, auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if constexpr (!check_opening_handled(Opts)) {
            dump('{', b, ix);
            if constexpr (Opts.prettify) {
               ctx.depth += check_indentation_width(Opts);
               dump('\n', b, ix);
               dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
            }
         }

         static constexpr auto sorted = sort_json_ptrs(Partial);
         static constexpr auto groups = glz::group_json_ptrs<sorted>();
         static constexpr auto N = glz::tuple_size_v<std::decay_t<decltype(groups)>>;

         static constexpr auto num_members = reflect<T>::size;

         if constexpr ((num_members > 0) && (glaze_object_t<T> || reflectable<T>)) {
            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               static constexpr auto group = glz::get<I>(groups);

               static constexpr auto key = get<0>(group);
               static constexpr auto quoted_key = quoted_key_v<key, Opts.prettify>;
               dump<quoted_key>(b, ix);

               static constexpr auto sub_partial = get<1>(group);
               static constexpr auto index = key_index<T>(key);
               static_assert(index < num_members, "Invalid key passed to partial write");
               if constexpr (glaze_object_t<T>) {
                  static constexpr auto member = get<index>(reflect<T>::values);

                  serialize_partial<JSON>::op<sub_partial, Opts>(get_member(value, member), ctx, b, ix);
                  if constexpr (I != N - 1) {
                     write_object_entry_separator<Opts>(ctx, b, ix);
                  }
               }
               else {
                  serialize_partial<JSON>::op<sub_partial, Opts>(get_member(value, get<index>(to_tie(value))), ctx, b,
                                                                 ix);
                  if constexpr (I != N - 1) {
                     write_object_entry_separator<Opts>(ctx, b, ix);
                  }
               }
            });
         }
         else if constexpr (writable_map_t<T>) {
            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               static constexpr auto group = glz::get<I>(groups);

               static constexpr auto key = std::get<0>(group);
               static constexpr auto quoted_key = quoted_key_v<key, Opts.prettify>;
               dump<key>(b, ix);

               static constexpr auto sub_partial = std::get<1>(group);
               if constexpr (findable<std::decay_t<T>, decltype(key)>) {
                  auto it = value.find(key);
                  if (it != value.end()) {
                     serialize_partial<JSON>::op<sub_partial, Opts>(it->second, ctx, b, ix);
                  }
                  else {
                     ctx.error = error_code::invalid_partial_key;
                     return;
                  }
               }
               else {
                  static thread_local auto k = typename std::decay_t<T>::key_type(key);
                  auto it = value.find(k);
                  if (it != value.end()) {
                     serialize_partial<JSON>::op<sub_partial, Opts>(it->second, ctx, b, ix);
                  }
                  else {
                     ctx.error = error_code::invalid_partial_key;
                     return;
                  }
               }
               if constexpr (I != N - 1) {
                  write_object_entry_separator<Opts>(ctx, b, ix);
               }
            });
         }

         if (not bool(ctx.error)) [[likely]] {
            dump('}', b, ix);
         }
      }
   };

   // Runtime partial write - writes only the keys specified at runtime
   // Outputs keys in the order they appear in the input container
   template <class T>
      requires(glaze_object_t<T> || reflectable<T>)
   struct to_runtime_partial
   {
      template <auto Opts, class Keys, class Value, is_context Ctx, class B, class IX>
      static void op(const Keys& keys, Value&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         if constexpr (!check_opening_handled(Opts)) {
            dump('{', b, ix);
            if constexpr (Opts.prettify) {
               ctx.depth += check_indentation_width(Opts);
               dump('\n', b, ix);
               dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
            }
         }

         using V = std::remove_cvref_t<Value>;
         static constexpr auto N = reflect<V>::size;

         if constexpr (N == 0) {
            // Empty struct - any key is unknown
            for (const auto& key_ref : keys) {
               (void)key_ref;
               ctx.error = error_code::unknown_key;
               return;
            }
         }
         else {
            static constexpr auto& HashInfo = hash_info<V>;

            decltype(auto) t = [&]() -> decltype(auto) {
               if constexpr (reflectable<V>) {
                  return to_tie(value);
               }
               else {
                  return nullptr;
               }
            }();

            bool first = true;

            for (const auto& key_ref : keys) {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               const sv key{key_ref};

               // Use hash-based lookup to find the member index
               const auto index = decode_hash_with_size<JSON, V, HashInfo, HashInfo.type>::op(
                  key.data(), key.data() + key.size(), key.size());

               if (index >= N) {
                  ctx.error = error_code::unknown_key;
                  return;
               }

               // Verify the key matches (hash collision check)
               const sv actual_key = reflect<V>::keys[index];
               if (key != actual_key) {
                  ctx.error = error_code::unknown_key;
                  return;
               }

               // Write separator if not first
               if (first) {
                  first = false;
               }
               else {
                  write_object_entry_separator<Opts>(ctx, b, ix);
               }

               // Write the quoted key at runtime
               if (!ensure_space(ctx, b, ix + key.size() + 4 + (Opts.prettify ? 1 : 0))) [[unlikely]] {
                  return;
               }
               dump('"', b, ix);
               dump(key, b, ix);
               if constexpr (Opts.prettify) {
                  dump("\": ", b, ix);
               }
               else {
                  dump("\":", b, ix);
               }

               // Use visit to dispatch to correct member and serialize value
               visit<N>(
                  [&]<size_t I>() {
                     using val_t = field_t<V, I>;
                     if constexpr (reflectable<V>) {
                        to<JSON, val_t>::template op<Opts>(get_member(value, get<I>(t)), ctx, b, ix);
                     }
                     else {
                        to<JSON, val_t>::template op<Opts>(get_member(value, get<I>(reflect<V>::values)), ctx, b, ix);
                     }
                  },
                  index);
            }
         }

         if constexpr (Opts.prettify) {
            ctx.depth -= check_indentation_width(Opts);
            dump('\n', b, ix);
            dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
         }

         if (not bool(ctx.error)) [[likely]] {
            dump('}', b, ix);
         }
      }
   };

   // Runtime exclude write - writes all keys EXCEPT those specified at runtime
   // Outputs keys in struct definition order, skipping excluded keys
   template <class T>
      requires(glaze_object_t<T> || reflectable<T>)
   struct to_runtime_exclude
   {
      template <auto Opts, class Keys, class Value, is_context Ctx, class B, class IX>
      static void op(const Keys& exclude_keys, Value&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         if constexpr (!check_opening_handled(Opts)) {
            dump('{', b, ix);
            if constexpr (Opts.prettify) {
               ctx.depth += check_indentation_width(Opts);
               dump('\n', b, ix);
               dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
            }
         }

         using V = std::remove_cvref_t<Value>;
         static constexpr auto N = reflect<V>::size;

         if constexpr (N == 0) {
            // Empty struct - any exclude key is unknown
            for (const auto& key_ref : exclude_keys) {
               (void)key_ref;
               ctx.error = error_code::unknown_key;
               return;
            }
         }
         else {
            static constexpr auto& HashInfo = hash_info<V>;

            // First, validate all exclude keys exist and build a bitset of excluded indices
            std::array<bool, N> excluded{};
            for (const auto& key_ref : exclude_keys) {
               const sv key{key_ref};

               // Use hash-based lookup to find the member index
               const auto index = decode_hash_with_size<JSON, V, HashInfo, HashInfo.type>::op(
                  key.data(), key.data() + key.size(), key.size());

               if (index >= N) {
                  ctx.error = error_code::unknown_key;
                  return;
               }

               // Verify the key matches (hash collision check)
               const sv actual_key = reflect<V>::keys[index];
               if (key != actual_key) {
                  ctx.error = error_code::unknown_key;
                  return;
               }

               excluded[index] = true;
            }

            decltype(auto) t = [&]() -> decltype(auto) {
               if constexpr (reflectable<V>) {
                  return to_tie(value);
               }
               else {
                  return nullptr;
               }
            }();

            bool first = true;

            // Iterate over all keys in struct order, skipping excluded ones
            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               if (excluded[I]) {
                  return; // Skip this key
               }

               // Write separator if not first
               if (first) {
                  first = false;
               }
               else {
                  write_object_entry_separator<Opts>(ctx, b, ix);
               }

               // Write the quoted key
               static constexpr sv key = reflect<V>::keys[I];
               if (!ensure_space(ctx, b, ix + key.size() + 4 + (Opts.prettify ? 1 : 0))) [[unlikely]] {
                  return;
               }
               dump('"', b, ix);
               dump<key>(b, ix);
               if constexpr (Opts.prettify) {
                  dump("\": ", b, ix);
               }
               else {
                  dump("\":", b, ix);
               }

               // Serialize the value
               using val_t = field_t<V, I>;
               if constexpr (reflectable<V>) {
                  to<JSON, val_t>::template op<Opts>(get_member(value, get<I>(t)), ctx, b, ix);
               }
               else {
                  to<JSON, val_t>::template op<Opts>(get_member(value, get<I>(reflect<V>::values)), ctx, b, ix);
               }
            });
         }

         if constexpr (Opts.prettify) {
            ctx.depth -= check_indentation_width(Opts);
            dump('\n', b, ix);
            dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
         }

         if (not bool(ctx.error)) [[likely]] {
            dump('}', b, ix);
         }
      }
   };

   template <class T>
      requires(glaze_value_t<T> && !custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class Value, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         to<JSON, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                        std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   // Returns 0 if we cannot determine the required padding,
   // in which case the `to` specialization must allocate buffer space
   // Some types like numbers must have space to be quoted
   // All types must have space for a trailing comma
   template <class T>
   constexpr size_t required_padding()
   {
      constexpr auto value = []() -> size_t {
         if constexpr (boolean_like<T>) {
            return 8;
         }
         else if constexpr (num_t<T>) {
            if constexpr (std::floating_point<T>) {
               if constexpr (sizeof(T) > 8) {
                  return 64;
               }
               else if constexpr (sizeof(T) > 4) {
                  return 32;
               }
               else {
                  return 24;
               }
            }
            else if constexpr (sizeof(T) > 4) {
               return 24;
            }
            else if constexpr (sizeof(T) > 2) {
               return 16;
            }
            else {
               return 8;
            }
         }
         else if constexpr (nullable_like<T>) {
            if constexpr (has_value_type<T>) {
               return required_padding<typename T::value_type>();
            }
            else if constexpr (has_element_type<T>) {
               return required_padding<typename T::element_type>();
            }
            else {
               return 0;
            }
         }
         else if constexpr (always_null_t<T>) {
            return 8;
         }
         else {
            return 0;
         }
      }();

      if constexpr (value >= (write_padding_bytes - 16)) {
         // we always require 16 bytes available from write_padding_bytes
         // for opening and closing characters
         return 0;
      }
      return value;
   }

   template <is_bitset T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (!ensure_space(ctx, b, ix + 2 + value.size())) [[unlikely]] {
            return;
         }

         std::memcpy(&b[ix], "\"", 1);
         ++ix;
         for (size_t i = value.size(); i > 0; --i) {
            if (value[i - 1]) {
               std::memcpy(&b[ix], "1", 1);
            }
            else {
               std::memcpy(&b[ix], "0", 1);
            }
            ++ix;
         }
         std::memcpy(&b[ix], "\"", 1);
         ++ix;
      }
   };

   template <glaze_flags_t T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto N = reflect<T>::size;

         static constexpr auto max_length = [] {
            size_t length{};
            [&]<size_t... I>(std::index_sequence<I...>) {
               ((length += reflect<T>::keys[I].size()), ...);
            }(std::make_index_sequence<N>{});
            return length;
         }() + 4 + 4 * N; // add extra characters

         if (!ensure_space(ctx, b, ix + max_length)) [[unlikely]] {
            return;
         }

         std::memcpy(&b[ix], "[", 1);
         ++ix;

         for_each<N>([&]<size_t I>() {
            if (get_member(value, get<I>(reflect<T>::values))) {
               std::memcpy(&b[ix], "\"", 1);
               ++ix;
               constexpr auto& key = reflect<T>::keys[I];
               if constexpr (not key.empty()) {
                  constexpr auto n = key.size();
                  std::memcpy(&b[ix], key.data(), n);
                  ix += n;
               }
               std::memcpy(&b[ix], "\",", 2);
               ix += 2;
            }
         });

         if (b[ix - 1] == ',') {
            b[ix - 1] = ']';
         }
         else {
            std::memcpy(&b[ix], "]", 1);
            ++ix;
         }
      }
   };

   template <is_member_function_pointer T>
   struct to<JSON, T>
   {
      template <auto Opts>
      static void op(auto&&, is_context auto&&, auto&& b, auto& ix) noexcept
      {
         if constexpr (check_write_function_pointers(Opts)) {
            constexpr sv type_name = name_v<T>;
            dump('"', b, ix);
            dump(type_name, b, ix);
            dump('"', b, ix);
         }
      }
   };

   template <is_reference_wrapper T>
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args)
      {
         using V = std::remove_cvref_t<decltype(value.get())>;
         to<JSON, V>::template op<Opts>(value.get(), std::forward<Args>(args)...);
      }
   };

   template <complex_t T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static_assert(num_t<typename T::value_type>);
         // we need to know it is a number type to allocate buffer space

         static constexpr size_t max_length = 256;
         if (!ensure_space(ctx, b, ix + max_length)) [[unlikely]] {
            return;
         }

         static constexpr auto O = write_unchecked_on<Opts>();

         std::memcpy(&b[ix], "[", 1);
         ++ix;
         using Value = core_t<typename T::value_type>;
         to<JSON, Value>::template op<O>(value.real(), ctx, b, ix);
         std::memcpy(&b[ix], ",", 1);
         ++ix;
         to<JSON, Value>::template op<O>(value.imag(), ctx, b, ix);
         std::memcpy(&b[ix], "]", 1);
         ++ix;
      }
   };

   template <boolean_like T>
   struct to<JSON, T>
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
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (not check_write_unchecked(Opts)) {
            static_assert(required_padding<T>());
            if (!ensure_space(ctx, b, ix + required_padding<T>())) [[unlikely]] {
               return;
            }
         }

         static constexpr auto O = write_unchecked_on<Opts>();

         if constexpr (check_quoted_num(Opts)) {
            std::memcpy(&b[ix], "\"", 1);
            ++ix;
            write_chars::op<O>(value, ctx, b, ix);
            std::memcpy(&b[ix], "\"", 1);
            ++ix;
         }
         else {
            write_chars::op<O>(value, ctx, b, ix);
         }
      }
   };

   template <class T>
      requires str_t<T> || char_t<T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (check_string_as_number(Opts)) {
            const sv str = [&]() -> const sv {
               if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
                  return value ? value : "";
               }
               else if constexpr (u8str_t<T>) {
                  return sv{reinterpret_cast<const char*>(value.data()), value.size()};
               }
               else {
                  return sv{value};
               }
            }();
            if (!ensure_space(ctx, b, ix + str.size() + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump_maybe_empty<false>(str, b, ix);
         }
         else if constexpr (char_t<T>) {
            if constexpr (check_unquoted(Opts)) {
               dump(value, b, ix);
            }
            else {
               // 8 bytes is enough for quotes and escaped character (worst case: \uXXXX = 6 + 2 quotes)
               if (!ensure_space(ctx, b, ix + 8)) [[unlikely]] {
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
               else if constexpr (check_escape_control_characters(Opts)) {
                  if (uint8_t(value) < 0x20) {
                     // Write as \uXXXX format
                     char unicode_escape[6] = {'\\', 'u', '0', '0', '0', '0'};
                     constexpr char hex_digits[] = "0123456789ABCDEF";
                     unicode_escape[4] = hex_digits[(value >> 4) & 0xF];
                     unicode_escape[5] = hex_digits[value & 0xF];
                     std::memcpy(&b[ix], unicode_escape, 6);
                     ix += 6;
                  }
                  else {
                     std::memcpy(&b[ix], &value, 1);
                     ++ix;
                  }
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
                  else if constexpr (u8str_t<T>) {
                     return sv{reinterpret_cast<const char*>(value.data()), value.size()};
                  }
                  else {
                     return sv{value};
                  }
               }();

               // We need space for quotes and the string length: 2 + n.
               // Use +8 for extra buffer
               if (!ensure_space(ctx, b, ix + 8 + str.size())) [[unlikely]] {
                  return;
               }
               // now we don't have to check writing

               if constexpr (not check_unquoted(Opts)) {
                  std::memcpy(&b[ix], "\"", 1);
                  ++ix;
               }
               if (str.size()) [[likely]] {
                  const auto n = str.size();
                  std::memcpy(&b[ix], str.data(), n);
                  ix += n;
               }
               if constexpr (not check_unquoted(Opts)) {
                  std::memcpy(&b[ix], "\"", 1);
                  ++ix;
               }
            }
            else {
               const sv str = [&]() -> const sv {
                  if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
                     return value ? value : "";
                  }
                  else if constexpr (array_char_t<T>) {
                     return sv{value.data(), value.size()};
                  }
                  else if constexpr (u8str_t<T>) {
                     return sv{reinterpret_cast<const char*>(value.data()), value.size()};
                  }
                  else {
                     return sv{value};
                  }
               }();
               const auto n = str.size();

               // In the case n == 0 we need two characters for quotes.
               // For each individual character we need room for two characters to handle escapes.
               // When using Unicode escapes, we might need up to 6 characters (\uXXXX) per character
               if constexpr (check_escape_control_characters(Opts)) {
                  // We need 2 + 6 * n characters in the worst case (all control chars)
                  if (!ensure_space(ctx, b, ix + 10 + 6 * n)) [[unlikely]] {
                     return;
                  }
               }
               else {
                  // Using the original sizing
                  if (!ensure_space(ctx, b, ix + 10 + 2 * n)) [[unlikely]] {
                     return;
                  }
               }
               // now we don't have to check writing

               if constexpr (check_unquoted(Opts)) {
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

                  // We don't check for writing out invalid characters as this can be tested by the user if
                  // necessary. In the case of invalid JSON characters we write out null characters to
                  // showcase the error and make the JSON invalid. These would then be detected upon reading
                  // the JSON.

#if defined(GLZ_USE_AVX2)
                  // Optimization for systems with AVX2 support
                  if (n > 31) {
                     const __m256i lo7_mask = _mm256_set1_epi8(0b01111111);
                     const __m256i quote_char = _mm256_set1_epi8('"');
                     const __m256i backslash_char = _mm256_set1_epi8('\\');
                     const __m256i less_32_mask = _mm256_set1_epi8(0b01100000);
                     const __m256i high_bit_mask = _mm256_set1_epi8(static_cast<int8_t>(0b10000000));

                     for (const char* end_m31 = e - 31; c < end_m31;) {
                        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c));

                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(data), v);

                        const __m256i lo7 = _mm256_and_si256(v, lo7_mask);
                        const __m256i quote = _mm256_add_epi8(_mm256_xor_si256(lo7, quote_char), lo7_mask);
                        const __m256i backslash = _mm256_add_epi8(_mm256_xor_si256(lo7, backslash_char), lo7_mask);
                        const __m256i less_32 = _mm256_add_epi8(_mm256_and_si256(v, less_32_mask), lo7_mask);

                        __m256i temp = _mm256_and_si256(quote, backslash);
                        temp = _mm256_and_si256(temp, less_32);
                        temp = _mm256_or_si256(temp, v);
                        __m256i next = _mm256_andnot_si256(temp, _mm256_set1_epi8(-1)); // Equivalent to ~temp
                        next = _mm256_and_si256(next, high_bit_mask);

                        uint32_t mask = _mm256_movemask_epi8(next);

                        if (mask == 0) {
                           data += 32;
                           c += 32;
                           continue;
                        }

                        uint32_t length = countr_zero(mask);

                        c += length;
                        data += length;

                        if constexpr (check_escape_control_characters(Opts)) {
                           if (const auto escaped = char_escape_table[uint8_t(*c)]; escaped) {
                              std::memcpy(data, &escaped, 2);
                              data += 2;
                           }
                           else {
                              // Write as \uXXXX format for control characters
                              char unicode_escape[6] = {'\\', 'u', '0', '0', '0', '0'};
                              constexpr char hex_digits[] = "0123456789ABCDEF";
                              unicode_escape[4] = hex_digits[(uint8_t(*c) >> 4) & 0xF];
                              unicode_escape[5] = hex_digits[uint8_t(*c) & 0xF];
                              std::memcpy(data, unicode_escape, 6);
                              data += 6;
                           }
                        }
                        else {
                           std::memcpy(data, &char_escape_table[uint8_t(*c)], 2);
                           data += 2;
                        }
                        ++c;
                     }
                  }
#endif

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

                        if constexpr (check_escape_control_characters(Opts)) {
                           if (const auto escaped = char_escape_table[uint8_t(*c)]; escaped) {
                              std::memcpy(data, &escaped, 2);
                              data += 2;
                           }
                           else {
                              // Write as \uXXXX format for control characters
                              char unicode_escape[6] = {'\\', 'u', '0', '0', '0', '0'};
                              constexpr char hex_digits[] = "0123456789ABCDEF";
                              unicode_escape[4] = hex_digits[(uint8_t(*c) >> 4) & 0xF];
                              unicode_escape[5] = hex_digits[uint8_t(*c) & 0xF];
                              std::memcpy(data, unicode_escape, 6);
                              data += 6;
                           }
                        }
                        else {
                           std::memcpy(data, &char_escape_table[uint8_t(*c)], 2);
                           data += 2;
                        }
                        ++c;
                     }
                  }

                  // Tail end of buffer. Uncommon for long strings.
                  for (; c < e; ++c) {
                     if (const auto escaped = char_escape_table[uint8_t(*c)]; escaped) {
                        std::memcpy(data, &escaped, 2);
                        data += 2;
                     }
                     else if constexpr (check_escape_control_characters(Opts)) {
                        if (uint8_t(*c) < 0x20) {
                           // Write as \uXXXX format for control characters
                           char unicode_escape[6] = {'\\', 'u', '0', '0', '0', '0'};
                           constexpr char hex_digits[] = "0123456789ABCDEF";
                           unicode_escape[4] = hex_digits[(uint8_t(*c) >> 4) & 0xF];
                           unicode_escape[5] = hex_digits[uint8_t(*c) & 0xF];
                           std::memcpy(data, unicode_escape, 6);
                           data += 6;
                        }
                        else {
                           std::memcpy(data, c, 1);
                           ++data;
                        }
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

   template <class T>
      requires((glaze_enum_t<T> || (meta_keys<T> && std::is_enum_v<std::decay_t<T>>)) && not custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         const sv str = get_enum_name(value);
         if (!str.empty()) {
            // TODO: Assumes people dont use strings with chars that need to be escaped for their enum names
            // TODO: Could create a pre quoted map for better performance
            if constexpr (not check_unquoted(Opts)) {
               dump('"', args...);
            }
            dump_maybe_empty(str, args...);
            if constexpr (not check_unquoted(Opts)) {
               dump('"', args...);
            }
         }
         else [[unlikely]] {
            // Value doesn't have a mapped string, serialize as underlying number
            serialize<JSON>::op<Opts>(static_cast<std::underlying_type_t<T>>(value), ctx, std::forward<Args>(args)...);
         }
      }
   };

   template <class T>
      requires(!meta_keys<T> && std::is_enum_v<std::decay_t<T>> && !glaze_enum_t<T> && !custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         // serialize as underlying number
         serialize<JSON>::op<Opts>(static_cast<std::underlying_type_t<std::decay_t<T>>>(value), ctx,
                                   std::forward<Args>(args)...);
      }
   };

   template <func_t T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto name = name_v<std::decay_t<decltype(value)>>;
         constexpr auto n = name.size();

         if (!ensure_space(ctx, b, ix + 8 + n)) [[unlikely]] {
            return;
         }

         std::memcpy(&b[ix], "\"", 1);
         ++ix;
         if constexpr (not name.empty()) {
            std::memcpy(&b[ix], name.data(), n);
            ix += n;
         }
         std::memcpy(&b[ix], "\"", 1);
         ++ix;
      }
   };

   // Handle function pointers and function references (serialize as their type name)
   template <class T>
      requires is_function_ptr_or_ref<T>
   struct to<JSON, T>
   {
      template <auto Opts>
      static void op(auto&&, is_context auto&&, auto&& b, auto& ix) noexcept
      {
         if constexpr (check_write_function_pointers(Opts)) {
            constexpr sv type_name = name_v<T>;
            dump('"', b, ix);
            dump(type_name, b, ix);
            dump('"', b, ix);
         }
      }
   };

   template <class T>
   struct to<JSON, basic_raw_json<T>>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         const auto n = value.str.size();
         if (n) {
            if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
               return;
            }

            std::memcpy(&b[ix], value.str.data(), n);
            ix += n;
         }
      }
   };

   template <class T>
   struct to<JSON, basic_text<T>>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         const auto n = value.str.size();
         if (n) {
            if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
               return;
            }

            std::memcpy(&b[ix], value.str.data(), n);
            ix += n;
         }
      }
   };

   template <auto Opts, bool minified_check = true, class B>
   GLZ_ALWAYS_INLINE void write_array_entry_separator(is_context auto&& ctx, B&& b, auto& ix)
   {
      if constexpr (Opts.prettify) {
         if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
            return;
         }
         if constexpr (check_new_lines_in_arrays(Opts)) {
            std::memcpy(&b[ix], ",\n", 2);
            ix += 2;
            std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
            ix += ctx.depth;
         }
         else {
            std::memcpy(&b[ix], ", ", 2);
            ix += 2;
         }
      }
      else {
         if constexpr (minified_check) {
            if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
               return;
            }
         }
         std::memcpy(&b[ix], ",", 1);
         ++ix;
      }
      if constexpr (is_output_streaming<B>) {
         flush_buffer(b, ix);
      }
   }

   // "key":value pair output
   template <auto Opts, class Key, class Value, is_context Ctx, class B>
   GLZ_ALWAYS_INLINE void write_pair_content(const Key& key, Value&& value, Ctx& ctx, B&& b, auto& ix)
   {
      if constexpr (str_t<Key> || char_t<Key> || glaze_enum_t<Key> || mimics_str_t<Key> || custom_str_t<Key> ||
                    check_quoted_num(Opts)) {
         to<JSON, core_t<Key>>::template op<Opts>(key, ctx, b, ix);
      }
      else if constexpr (num_t<Key>) {
         serialize<JSON>::op<opt_true<Opts, quoted_num_opt_tag{}>>(key, ctx, b, ix);
      }
      else {
         serialize<JSON>::op<opt_false<Opts, raw_string_opt_tag{}>>(quoted_t<const Key>{key}, ctx, b, ix);
      }
      if (bool(ctx.error)) [[unlikely]] {
         return;
      }
      if constexpr (Opts.prettify) {
         dump(": ", b, ix);
      }
      else {
         dump(':', b, ix);
      }

      using V = core_t<decltype(value)>;
      to<JSON, V>::template op<opening_and_closing_handled_off<Opts>()>(value, ctx, b, ix);
   }

   template <class T>
   concept array_padding_known =
      requires { typename T::value_type; } && (required_padding<typename T::value_type>() > 0);

   template <class T>
      requires(writable_array_t<T> || writable_map_t<T>)
   struct to<JSON, T>
   {
      static constexpr bool map_like_array = writable_array_t<T> && pair_t<range_value_t<T>>;

      template <auto Opts, class B>
         requires(writable_array_t<T> && (map_like_array ? check_concatenate(Opts) == false : true))
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (empty_range(value)) {
            dump("[]", b, ix);
         }
         else {
            // For bounded buffers, skip pre-allocation path since value_padding is for SIMD, not actual size
            if constexpr (has_size<T> && array_padding_known<T> && !has_bounded_capacity<B>) {
               const auto n = value.size();

               static constexpr auto value_padding = required_padding<typename T::value_type>();

               if constexpr (Opts.prettify) {
                  if constexpr (check_new_lines_in_arrays(Opts)) {
                     ctx.depth += check_indentation_width(Opts);
                  }

                  // add space for '\n' and ',' characters for each element, hence `+ 2`
                  // use n + 1 because we put the end array character after the last element with whitespace
                  if (!ensure_space(ctx, b, ix + (n + 1) * (value_padding + ctx.depth + 2) + write_padding_bytes))
                     [[unlikely]] {
                     return;
                  }

                  if constexpr (check_new_lines_in_arrays(Opts)) {
                     std::memcpy(&b[ix], "[\n", 2);
                     ix += 2;
                     std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                     ix += ctx.depth;
                  }
                  else {
                     std::memcpy(&b[ix], "[", 1);
                     ++ix;
                  }
               }
               else {
                  static constexpr auto comma_padding = 1;
                  if (!ensure_space(ctx, b, ix + n * (value_padding + comma_padding) + write_padding_bytes))
                     [[unlikely]] {
                     return;
                  }
                  std::memcpy(&b[ix], "[", 1);
                  ++ix;
               }

               auto it = std::begin(value);
               using val_t = std::remove_cvref_t<decltype(*it)>;
               to<JSON, val_t>::template op<write_unchecked_on<Opts>()>(*it, ctx, b, ix);

               ++it;
               for (const auto fin = std::end(value); it != fin; ++it) {
                  if constexpr (Opts.prettify) {
                     if constexpr (check_new_lines_in_arrays(Opts)) {
                        std::memcpy(&b[ix], ",\n", 2);
                        ix += 2;
                        std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                        ix += ctx.depth;
                     }
                     else {
                        std::memcpy(&b[ix], ", ", 2);
                        ix += 2;
                     }
                  }
                  else {
                     std::memcpy(&b[ix], ",", 1);
                     ++ix;
                  }
                  if constexpr (is_output_streaming<B>) {
                     flush_buffer(b, ix);
                  }

                  to<JSON, val_t>::template op<write_unchecked_on<Opts>()>(*it, ctx, b, ix);
               }
               if constexpr (Opts.prettify && check_new_lines_in_arrays(Opts)) {
                  ctx.depth -= check_indentation_width(Opts);
                  std::memcpy(&b[ix], "\n", 1);
                  ++ix;
                  std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                  ix += ctx.depth;
               }

               std::memcpy(&b[ix], "]", 1);
               ++ix;
            }
            else {
               // we either can't get the size (std::forward_list) or we cannot compute the allocation size

               if constexpr (Opts.prettify) {
                  if constexpr (check_new_lines_in_arrays(Opts)) {
                     ctx.depth += check_indentation_width(Opts);
                  }

                  if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                     return;
                  }

                  if constexpr (check_new_lines_in_arrays(Opts)) {
                     std::memcpy(&b[ix], "[\n", 2);
                     ix += 2;
                     std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                     ix += ctx.depth;
                  }
                  else {
                     std::memcpy(&b[ix], "[", 1);
                     ++ix;
                  }
               }
               else {
                  if (!ensure_space(ctx, b, ix + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  std::memcpy(&b[ix], "[", 1);
                  ++ix;
               }

               auto it = std::begin(value);
               using val_t = std::remove_cvref_t<decltype(*it)>;
               if constexpr (required_padding<val_t>()) {
                  to<JSON, val_t>::template op<write_unchecked_on<Opts>()>(*it, ctx, b, ix);
               }
               else {
                  to<JSON, val_t>::template op<Opts>(*it, ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }

               ++it;
               for (const auto fin = std::end(value); it != fin; ++it) {
                  if constexpr (required_padding<val_t>()) {
                     if constexpr (Opts.prettify) {
                        if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                           return;
                        }
                     }
                     else {
                        if (!ensure_space(ctx, b, ix + write_padding_bytes)) [[unlikely]] {
                           return;
                        }
                     }

                     if constexpr (Opts.prettify) {
                        if constexpr (check_new_lines_in_arrays(Opts)) {
                           std::memcpy(&b[ix], ",\n", 2);
                           ix += 2;
                           std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                           ix += ctx.depth;
                        }
                        else {
                           std::memcpy(&b[ix], ", ", 2);
                           ix += 2;
                        }
                     }
                     else {
                        std::memcpy(&b[ix], ",", 1);
                        ++ix;
                     }
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }

                     to<JSON, val_t>::template op<write_unchecked_on<Opts>()>(*it, ctx, b, ix);
                  }
                  else {
                     write_array_entry_separator<Opts>(ctx, b, ix);
                     to<JSON, val_t>::template op<Opts>(*it, ctx, b, ix);
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }
                  }
               }
               if constexpr (Opts.prettify && check_new_lines_in_arrays(Opts)) {
                  ctx.depth -= check_indentation_width(Opts);
                  dump_newline_indent(check_indentation_char(Opts), ctx.depth, b, ix);
               }

               dump(']', b, ix);
            }
         }
      }

      template <auto Opts, class B>
         requires(writable_map_t<T> || (map_like_array && check_concatenate(Opts) == true))
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (not check_opening_handled(Opts)) {
            dump('{', b, ix);
         }

         if (!empty_range(value)) {
            if constexpr (!check_opening_handled(Opts)) {
               if constexpr (Opts.prettify) {
                  ctx.depth += check_indentation_width(Opts);
                  if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  std::memcpy(&b[ix], "\n", 1);
                  ++ix;
                  std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                  ix += ctx.depth;
               }
            }

            using val_t = detail::iterator_second_type<T>; // the type of value in each [key, value] pair
            constexpr bool write_function_pointers = check_write_function_pointers(Opts);
            if constexpr (!always_skipped<val_t> && (write_function_pointers || !is_any_function_ptr<val_t>)) {
               if constexpr (null_t<val_t> && Opts.skip_null_members) {
                  auto write_first_entry = [&](auto&& it) {
                     auto&& [key, entry_val] = *it;
                     if (skip_member<Opts>(entry_val)) {
                        return true;
                     }
                     write_pair_content<Opts>(key, entry_val, ctx, b, ix);
                     return bool(ctx.error);
                  };

                  auto it = std::begin(value);
                  bool first = write_first_entry(it);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  ++it;
                  for (const auto end = std::end(value); it != end; ++it) {
                     auto&& [key, entry_val] = *it;
                     if (skip_member<Opts>(entry_val)) {
                        continue;
                     }

                     // When Opts.skip_null_members, *any* entry may be skipped, meaning separator dumping must be
                     // conditional for every entry.
                     // Alternatively, write separator after each entry except last but then branch is permanent
                     if (not first) {
                        write_object_entry_separator<Opts>(ctx, b, ix);
                     }

                     write_pair_content<Opts>(key, entry_val, ctx, b, ix);
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }

                     first = false;
                  }
               }
               else {
                  auto write_first_entry = [&](auto&& it) {
                     auto&& [key, entry_val] = *it;
                     write_pair_content<Opts>(key, entry_val, ctx, b, ix);
                  };

                  auto it = std::begin(value);
                  write_first_entry(it);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  ++it;
                  for (const auto end = std::end(value); it != end; ++it) {
                     auto&& [key, entry_val] = *it;
                     write_object_entry_separator<Opts>(ctx, b, ix);
                     write_pair_content<Opts>(key, entry_val, ctx, b, ix);
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }
                  }
               }
            }

            if constexpr (!check_closing_handled(Opts)) {
               if constexpr (Opts.prettify) {
                  ctx.depth -= check_indentation_width(Opts);
                  if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  std::memcpy(&b[ix], "\n", 1);
                  ++ix;
                  std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                  ix += ctx.depth;
               }
            }
         }

         if constexpr (!check_closing_handled(Opts)) {
            dump('}', b, ix);
         }
      }
   };

   template <pair_t T>
   struct to<JSON, T>
   {
      template <auto Opts, class B, class Ix>
      static void op(const T& value, is_context auto&& ctx, B&& b, Ix&& ix)
      {
         const auto& [key, val] = value;
         if (skip_member<Opts>(val)) {
            return dump("{}", b, ix);
         }

         if constexpr (Opts.prettify) {
            ctx.depth += check_indentation_width(Opts);
            if (!ensure_space(ctx, b, ix + ctx.depth + 2)) [[unlikely]] {
               return;
            }
            dump<false>("{\n", b, ix);
            std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
            ix += ctx.depth;
         }
         else {
            dump('{', b, ix);
         }

         write_pair_content<Opts>(key, val, ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if constexpr (Opts.prettify) {
            ctx.depth -= check_indentation_width(Opts);
            dump_newline_indent(check_indentation_char(Opts), ctx.depth, b, ix);
            dump<false>('}', b, ix);
         }
         else {
            dump('}', b, ix);
         }
      }
   };

   template <is_expected T>
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         if (value) {
            serialize<JSON>::op<Opts>(*value, ctx, std::forward<Args>(args)...);
         }
         else {
            serialize<JSON>::op<Opts>(unexpected_wrapper{&value.error()}, ctx, std::forward<Args>(args)...);
         }
      }
   };

   // for C style arrays
   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class V, size_t N, class... Args>
      GLZ_ALWAYS_INLINE static void op(const V (&value)[N], is_context auto&& ctx, Args&&... args)
      {
         serialize<JSON>::op<Opts>(std::span{value, N}, ctx, std::forward<Args>(args)...);
      }
   };

   template <nullable_like T>
   struct to<JSON, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (value) {
            if constexpr (required_padding<T>()) {
               serialize<JSON>::op<Opts>(*value, ctx, b, ix);
            }
            else {
               serialize<JSON>::op<write_unchecked_off<Opts>()>(*value, ctx, b, ix);
            }
         }
         else {
            dump<not check_write_unchecked(Opts)>("null", b, ix);
         }
      }
   };

   template <class T>
      requires(nullable_value_t<T> && not nullable_like<T> && not is_expected<T>)
   struct to<JSON, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (value.has_value()) {
            serialize<JSON>::op<Opts>(value.value(), ctx, b, ix);
         }
         else {
            dump("null", b, ix);
         }
      }
   };

   template <always_null_t T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&&, B&& b, auto& ix)
      {
         if constexpr (not check_write_unchecked(Opts)) {
            if (const auto k = ix + 4; k > b.size()) [[unlikely]] {
               b.resize(2 * k);
            }
         }
         if constexpr (std::endian::native == std::endian::big) {
            static constexpr char null_v[] = "null";
            std::memcpy(&b[ix], null_v, 4);
         }
         else {
            static constexpr uint32_t null_v = 1819047278;
            std::memcpy(&b[ix], &null_v, 4);
         }
         ix += 4;
      }
   };

   template <is_variant T>
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         std::visit(
            [&](auto&& val) {
               using V = std::decay_t<decltype(val)>;

               if constexpr (check_write_type_info(Opts) && not tag_v<T>.empty() &&
                             (glaze_object_t<V> || (reflectable<V> && !has_member_with_name<V>(tag_v<T>)) ||
                              is_memory_object<V>)) {
                  constexpr auto N = []() {
                     if constexpr (is_memory_object<V>) {
                        return reflect<memory_type<V>>::size;
                     }
                     else {
                        return reflect<V>::size;
                     }
                  }();

                  // must first write out type
                  if constexpr (Opts.prettify) {
                     dump("{\n", b, ix);
                     ctx.depth += check_indentation_width(Opts);
                     dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
                     dump('"', b, ix);
                     dump_maybe_empty(tag_v<T>, b, ix);

                     using id_type = std::decay_t<decltype(ids_v<T>[value.index()])>;

                     if constexpr (std::integral<id_type>) {
                        dump("\": ", b, ix);
                        serialize<JSON>::op<Opts>(ids_v<T>[value.index()], ctx, b, ix);
                        if constexpr (N > 0) {
                           dump(",\n", b, ix);
                           dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
                        }
                     }
                     else {
                        dump("\": \"", b, ix);
                        dump_maybe_empty(ids_v<T>[value.index()], b, ix);
                        if constexpr (N == 0) {
                           dump('"', b, ix);
                        }
                        else {
                           dump("\",\n", b, ix);
                           dumpn(check_indentation_char(Opts), ctx.depth, b, ix);
                        }
                     }
                  }
                  else {
                     using id_type = std::decay_t<decltype(ids_v<T>[value.index()])>;

                     dump("{\"", b, ix);
                     dump_maybe_empty(tag_v<T>, b, ix);

                     if constexpr (std::integral<id_type>) {
                        dump("\":", b, ix);
                        serialize<JSON>::op<Opts>(ids_v<T>[value.index()], ctx, b, ix);
                        if constexpr (N > 0) {
                           dump(',', b, ix);
                        }
                     }
                     else {
                        dump("\":\"", b, ix);
                        dump_maybe_empty(ids_v<T>[value.index()], b, ix);
                        if constexpr (N == 0) {
                           dump('"', b, ix);
                        }
                        else {
                           dump("\",", b, ix);
                        }
                     }
                  }
                  if constexpr (is_memory_object<V>) {
                     if (!val) [[unlikely]] {
                        ctx.error = error_code::invalid_variant_object;
                        return;
                     }
                     to<JSON, memory_type<V>>::template op<opening_and_closing_handled<Opts>()>(*val, ctx, b, ix);
                  }
                  else {
                     to<JSON, V>::template op<opening_and_closing_handled<Opts>()>(val, ctx, b, ix);
                  }
                  // If we skip everything then we may have an extra comma, which we want to revert
                  if constexpr (Opts.skip_null_members) {
                     if (b[ix - 1] == ',') {
                        --ix;
                     }
                  }

                  if constexpr (Opts.prettify) {
                     ctx.depth -= check_indentation_width(Opts);
                     if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     std::memcpy(&b[ix], "\n", 1);
                     ++ix;
                     std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                     ix += ctx.depth;
                     std::memcpy(&b[ix], "}", 1);
                     ++ix;
                  }
                  else {
                     dump('}', b, ix);
                  }
               }
               else {
                  to<JSON, V>::template op<Opts>(val, ctx, b, ix);
               }
            },
            value);
      }
   };

   template <class T>
   struct to<JSON, array_variant_wrapper<T>>
   {
      template <auto Opts, class... Args>
      static void op(auto&& wrapper, is_context auto&& ctx, Args&&... args)
      {
         auto& value = wrapper.value;
         dump('[', args...);
         if constexpr (Opts.prettify) {
            ctx.depth += check_indentation_width(Opts);
            dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
         }
         dump('"', args...);
         dump_maybe_empty(ids_v<T>[value.index()], args...);
         dump("\",", args...);
         if constexpr (Opts.prettify) {
            dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
         }
         std::visit([&](auto&& v) { serialize<JSON>::op<Opts>(v, ctx, args...); }, value);
         if constexpr (Opts.prettify) {
            ctx.depth -= check_indentation_width(Opts);
            dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
         }
         dump(']', args...);
      }
   };

   template <class T>
      requires is_specialization_v<T, arr>
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V>;

         dump('[', args...);
         if constexpr (N > 0 && Opts.prettify) {
            if constexpr (check_new_lines_in_arrays(Opts)) {
               ctx.depth += check_indentation_width(Opts);
               dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
            }
         }
         for_each<N>([&]<size_t I>() {
            if constexpr (glaze_array_t<V>) {
               serialize<JSON>::op<Opts>(get_member(value.value, glz::get<I>(meta_v<T>)), ctx, args...);
            }
            else {
               serialize<JSON>::op<Opts>(glz::get<I>(value.value), ctx, args...);
            }
            constexpr bool needs_comma = I < N - 1;
            if constexpr (needs_comma) {
               write_array_entry_separator<Opts>(ctx, args...);
            }
         });
         if constexpr (N > 0 && Opts.prettify) {
            if constexpr (check_new_lines_in_arrays(Opts)) {
               ctx.depth -= check_indentation_width(Opts);
               dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
            }
         }
         dump(']', args...);
      }
   };

   template <class T>
      requires glaze_array_t<T> || tuple_t<std::decay_t<T>> || is_std_tuple<T>
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         static constexpr auto N = []() constexpr {
            if constexpr (glaze_array_t<std::decay_t<T>>) {
               return glz::tuple_size_v<meta_t<std::decay_t<T>>>;
            }
            else {
               return glz::tuple_size_v<std::decay_t<T>>;
            }
         }();

         dump('[', args...);
         if constexpr (N > 0 && Opts.prettify) {
            if constexpr (check_new_lines_in_arrays(Opts)) {
               ctx.depth += check_indentation_width(Opts);
               dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
            }
         }
         using V = std::decay_t<T>;
         for_each<N>([&]<size_t I>() {
            if constexpr (glaze_array_t<V>) {
               serialize<JSON>::op<Opts>(get_member(value, glz::get<I>(meta_v<T>)), ctx, args...);
            }
            else if constexpr (is_std_tuple<T>) {
               using Value = core_t<decltype(std::get<I>(value))>;
               to<JSON, Value>::template op<Opts>(std::get<I>(value), ctx, args...);
            }
            else {
               using Value = core_t<decltype(glz::get<I>(value))>;
               to<JSON, Value>::template op<Opts>(glz::get<I>(value), ctx, args...);
            }
            constexpr bool needs_comma = I < N - 1;
            if constexpr (needs_comma) {
               write_array_entry_separator<Opts>(ctx, args...);
            }
         });
         if constexpr (N > 0 && Opts.prettify) {
            if constexpr (check_new_lines_in_arrays(Opts)) {
               ctx.depth -= check_indentation_width(Opts);
               dump_newline_indent(check_indentation_char(Opts), ctx.depth, args...);
            }
         }
         dump(']', args...);
      }
   };

   template <is_includer T>
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&&, Args&&... args)
      {
         dump<R"("")">(args...); // dump an empty string
      }
   };

   template <const std::string_view& S>
   GLZ_ALWAYS_INLINE constexpr auto array_from_sv() noexcept
   {
      constexpr auto N = S.size();
      std::array<char, N> arr;
      std::copy_n(S.data(), N, arr.data());
      return arr;
   }

   template <class T>
      requires is_specialization_v<T, glz::obj> || is_specialization_v<T, glz::obj_copy>
   struct to<JSON, T>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if constexpr (!check_opening_handled(Options)) {
            dump('{', b, ix);
            if constexpr (Options.prettify) {
               ctx.depth += check_indentation_width(Options);
               dump('\n', b, ix);
               dumpn(check_indentation_char(Options), ctx.depth, b, ix);
            }
         }

         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V> / 2;

         bool first = true;
         for_each<N>([&]<size_t I>() {
            constexpr auto Opts = opening_and_closing_handled_off<ws_handled_off<Options>()>();
            decltype(auto) item = glz::get<2 * I + 1>(value.value);
            using val_t = std::decay_t<decltype(item)>;

            if (skip_member<Opts>(item)) {
               return;
            }

            // skip
            constexpr bool write_function_pointers = check_write_function_pointers(Opts);
            if constexpr (always_skipped<val_t> || (!write_function_pointers && is_any_function_ptr<val_t>)) {
               return;
            }
            else {
               if (first) {
                  first = false;
               }
               else {
                  // Null members may be skipped so we can't just write it out for all but the last member unless
                  // trailing commas are allowed
                  write_object_entry_separator<Opts>(ctx, b, ix);
               }

               using Key = typename std::decay_t<glz::tuple_element_t<2 * I, V>>;

               if constexpr (str_t<Key> || char_t<Key>) {
                  const sv key = glz::get<2 * I>(value.value);
                  to<JSON, decltype(key)>::template op<Opts>(key, ctx, b, ix);
                  dump(':', b, ix);
                  if constexpr (Opts.prettify) {
                     dump(' ', b, ix);
                  }
               }
               else {
                  dump('"', b, ix);
                  to<JSON, val_t>::template op<Opts>(item, ctx, b, ix);
                  dump_not_empty(Opts.prettify ? "\": " : "\":", b, ix);
               }

               to<JSON, val_t>::template op<Opts>(item, ctx, b, ix);
            }
         });

         if constexpr (!check_closing_handled(Options)) {
            if constexpr (Options.prettify) {
               ctx.depth -= check_indentation_width(Options);
               dump('\n', b, ix);
               dumpn(check_indentation_char(Options), ctx.depth, b, ix);
            }
            dump('}', b, ix);
         }
      }
   };

   template <class T>
      requires is_specialization_v<T, glz::merge>
   struct to<JSON, T>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if constexpr (!check_opening_handled(Options)) {
            dump('{', b, ix);
            if constexpr (Options.prettify) {
               ctx.depth += check_indentation_width(Options);
               dump('\n', b, ix);
               dumpn(check_indentation_char(Options), ctx.depth, b, ix);
            }
         }

         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V>;

         [[maybe_unused]] static constexpr auto Opts = opening_and_closing_handled<Options>();

         // When merging it is possible that objects are completed empty
         // and therefore behave like skipped members even when skip_null_members is off

         for_each<N>([&]<size_t I>() {
            // We don't want to dump a comma when nothing is written
            const auto ix_start = ix;
            using Value = core_t<decltype(get<I>(value.value))>;
            to<JSON, Value>::template op<Opts>(get<I>(value.value), ctx, b, ix);
            if (ix > ix_start) // we wrote something
            {
               dump(',', b, ix);
            }
         });

         // we may have a trailing comma, which needs to be removed
         if (b[ix - 1] == ',') {
            --ix;
         }

         if constexpr (Options.prettify) {
            ctx.depth -= check_indentation_width(Options);
            dump('\n', b, ix);
            dumpn(check_indentation_char(Options), ctx.depth, b, ix);
         }
         dump('}', b, ix);
      }
   };

   // Only use this if you are not prettifying
   // Returns zero if the fixed size cannot be determined
   template <class T>
   inline constexpr size_t fixed_padding = [] {
      constexpr auto N = reflect<T>::size;
      size_t fixed = 2 + 16; // {} + extra padding
      for_each_short_circuit<N>([&]<auto I>() -> bool {
         using val_t = field_t<T, I>;
         if constexpr (required_padding<val_t>()) {
            fixed += required_padding<val_t>();
            fixed += reflect<T>::keys[I].size() + 2; // quoted key length
            fixed += 2; // colon and comma
            return false; // continue
         }
         else {
            fixed = 0;
            return true; // break
         }
      });
      if (fixed) {
         fixed = round_up_to_nearest_16(fixed);
      }
      return fixed;
   }();

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Options, class V, class B>
         requires(not std::is_pointer_v<std::remove_cvref_t<V>>)
      static void op(V&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using ValueType = std::decay_t<V>;
         if constexpr (has_unknown_writer<ValueType> && not check_disable_write_unknown(Options)) {
            constexpr auto& writer = meta_unknown_write_v<ValueType>;

            using WriterType = meta_unknown_write_t<ValueType>;
            if constexpr (std::is_member_object_pointer_v<WriterType>) {
               decltype(auto) unknown_writer = value.*writer;
               if (unknown_writer.size() > 0) {
                  // TODO: This intermediate is added to get GCC 14 to build
                  decltype(auto) merged = glz::merge{value, unknown_writer};
                  serialize<JSON>::op<disable_write_unknown_on<Options>()>(std::move(merged), ctx, b, ix);
               }
               else {
                  serialize<JSON>::op<disable_write_unknown_on<Options>()>(value, ctx, b, ix);
               }
            }
            else if constexpr (std::is_member_function_pointer_v<WriterType>) {
               decltype(auto) unknown_writer = (value.*writer)();
               if (unknown_writer.size() > 0) {
                  // TODO: This intermediate is added to get GCC 14 to build
                  decltype(auto) merged = glz::merge{value, unknown_writer};
                  serialize<JSON>::op<disable_write_unknown_on<Options>()>(std::move(merged), ctx, b, ix);
               }
               else {
                  serialize<JSON>::op<disable_write_unknown_on<Options>()>(value, ctx, b, ix);
               }
            }
            else {
               static_assert(false_v<T>, "unknown_write type not handled");
            }
         }
         else {
            // handles glaze_object_t without extra unknown fields
            static constexpr auto Opts =
               disable_write_unknown_off<opening_and_closing_handled_off<ws_handled_off<Options>()>()>();

            if constexpr (not check_opening_handled(Options)) {
               if constexpr (Options.prettify) {
                  ctx.depth += check_indentation_width(Options);
                  if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  std::memcpy(&b[ix], "{\n", 2);
                  ix += 2;
                  std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                  ix += ctx.depth;
               }
               else {
                  if (!ensure_space(ctx, b, ix + 1)) [[unlikely]] {
                     return;
                  }
                  dump('{', b, ix);
               }
            }

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
            if constexpr (maybe_skipped<Opts, T>) {
               bool first = true;
               for_each<N>([&]<size_t I>() {
                  using val_t = field_t<T, I>;

                  if constexpr (meta_has_skip<T>) {
                     static constexpr meta_context mctx{.op = operation::serialize};
                     if constexpr (meta<T>::skip(reflect<T>::keys[I], mctx)) return;
                  }
                  if constexpr (meta_has_skip_if<T>) {
                     static constexpr auto key = glz::get<I>(reflect<T>::keys);
                     static constexpr meta_context mctx{.op = operation::serialize};
                     decltype(auto) field_value = [&]() -> decltype(auto) {
                        if constexpr (reflectable<T>) {
                           return get<I>(t);
                        }
                        else {
                           return get_member(value, glz::get<I>(reflect<T>::values));
                        }
                     }();
                     if (meta<T>::skip_if(field_value, key, mctx)) return;
                  }

                  constexpr bool write_function_pointers = check_write_function_pointers(Opts);
                  if constexpr (always_skipped<val_t> || (!write_function_pointers && is_any_function_ptr<val_t>)) {
                     return;
                  }
                  else {
                     if constexpr (null_t<val_t> && Opts.skip_null_members) {
                        if constexpr (always_null_t<val_t>)
                           return;
                        else {
                           const auto is_null = [&]() {
                              decltype(auto) element = [&]() -> decltype(auto) {
                                 if constexpr (reflectable<T>) {
                                    return get<I>(t);
                                 }
                                 else {
                                    return get<I>(reflect<T>::values);
                                 }
                              };

                              if constexpr (nullable_wrapper<val_t>) {
                                 return !bool(element()(value).val);
                              }
                              else if constexpr (nullable_value_t<val_t>) {
                                 return !get_member(value, element()).has_value();
                              }
                              else {
                                 return !bool(get_member(value, element()));
                              }
                           }();
                           if (is_null) return;
                        }
                     }

                     if constexpr (Opts.prettify) {
                        if (!ensure_space(ctx, b, ix + padding + ctx.depth)) [[unlikely]] {
                           return;
                        }
                     }
                     else {
                        if (!ensure_space(ctx, b, ix + padding)) [[unlikely]] {
                           return;
                        }
                     }

                     if (first) {
                        first = false;
                     }
                     else {
                        // Null members may be skipped so we can't just write it out for all but the last member
                        if constexpr (Opts.prettify) {
                           std::memcpy(&b[ix], ",\n", 2);
                           ix += 2;
                           std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                           ix += ctx.depth;
                        }
                        else {
                           std::memcpy(&b[ix], ",", 1);
                           ++ix;
                        }
                        if constexpr (is_output_streaming<B>) {
                           flush_buffer(b, ix);
                        }
                     }

                     // MSVC requires get<I> rather than keys[I]
                     static constexpr auto key = glz::get<I>(reflect<T>::keys); // GCC 14 requires auto here
                     static constexpr auto quoted_key = quoted_key_v<key, Opts.prettify>;
                     static constexpr auto n = quoted_key.size();
                     std::memcpy(&b[ix], quoted_key.data(), n);
                     ix += n;

                     static constexpr auto check_opts = required_padding<val_t>() ? write_unchecked_on<Opts>() : Opts;
                     if constexpr (reflectable<T>) {
                        to<JSON, val_t>::template op<check_opts>(get_member(value, get<I>(t)), ctx, b, ix);
                     }
                     else {
                        to<JSON, val_t>::template op<check_opts>(get_member(value, get<I>(reflect<T>::values)), ctx, b,
                                                                 ix);
                     }
                  }
               });
            }
            else {
               static constexpr size_t fixed_max_size = fixed_padding<T>;
               if constexpr (fixed_max_size) {
                  if (!ensure_space(ctx, b, ix + fixed_max_size)) [[unlikely]] {
                     return;
                  }
               }

               for_each<N>([&]<size_t I>() {
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  if constexpr (not fixed_max_size) {
                     if constexpr (Opts.prettify) {
                        if (!ensure_space(ctx, b, ix + padding + ctx.depth)) [[unlikely]] {
                           return;
                        }
                     }
                     else {
                        if (!ensure_space(ctx, b, ix + padding)) [[unlikely]] {
                           return;
                        }
                     }
                  }

                  if constexpr (I != 0 && Opts.prettify) {
                     std::memcpy(&b[ix], ",\n", 2);
                     ix += 2;
                     std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                     ix += ctx.depth;
                  }

                  using val_t = field_t<T, I>;

                  // MSVC requires get<I> rather than keys[I]
                  static constexpr auto key = glz::get<I>(reflect<T>::keys); // GCC 14 requires auto here
                  if constexpr (always_null_t<val_t>) {
                     if constexpr (I == 0 || Opts.prettify) {
                        static constexpr auto quoted_key = join_v<quoted_key_v<key, Opts.prettify>, chars<"null">>;
                        static constexpr auto n = quoted_key.size();
                        std::memcpy(&b[ix], quoted_key.data(), n);
                        ix += n;
                     }
                     else {
                        static constexpr auto quoted_key = join_v<chars<",">, quoted_key_v<key>, chars<"null">>;
                        static constexpr auto n = quoted_key.size();
                        std::memcpy(&b[ix], quoted_key.data(), n);
                        ix += n;
                     }
                  }
                  else {
                     if constexpr (I == 0 || Opts.prettify) {
                        static constexpr auto quoted_key = quoted_key_v<key, Opts.prettify>;
                        static constexpr auto n = quoted_key.size();
                        std::memcpy(&b[ix], quoted_key.data(), n);
                        ix += n;
                     }
                     else {
                        static constexpr auto quoted_key = join_v<chars<",">, quoted_key_v<key>>;
                        static constexpr auto n = quoted_key.size();
                        std::memcpy(&b[ix], quoted_key.data(), n);
                        ix += n;
                     }

                     static constexpr auto check_opts = required_padding<val_t>() ? write_unchecked_on<Opts>() : Opts;
                     if constexpr (reflectable<T>) {
                        to<JSON, val_t>::template op<check_opts>(get_member(value, get<I>(t)), ctx, b, ix);
                     }
                     else {
                        to<JSON, val_t>::template op<check_opts>(get_member(value, get<I>(reflect<T>::values)), ctx, b,
                                                                 ix);
                     }
                  }
               });
            }

            // Options is required here, because it must be the top level
            if constexpr (not check_closing_handled(Options)) {
               if constexpr (Options.prettify) {
                  ctx.depth -= check_indentation_width(Options);
                  if (!ensure_space(ctx, b, ix + ctx.depth + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  std::memcpy(&b[ix], "\n", 1);
                  ++ix;
                  std::memset(&b[ix], check_indentation_char(Opts), ctx.depth);
                  ix += ctx.depth;
                  std::memcpy(&b[ix], "}", 1);
                  ++ix;
               }
               else {
                  dump('}', b, ix);
               }
            }
         }
      }
   };

   // ============================================
   // std::chrono serialization
   // ============================================

   // Duration: serialize as count
   template <is_duration T>
      requires(not custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using Rep = typename std::remove_cvref_t<T>::rep;
         to<JSON, Rep>::template op<Opts>(value.count(), ctx, b, ix);
      }
   };

   // system_clock::time_point: serialize as ISO 8601 string
   // Zero-allocation implementation writing directly to buffer
   template <is_system_time_point T>
      requires(not custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, [[maybe_unused]] is_context auto&& ctx, B&& b, auto& ix) noexcept
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

         // Max size: "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ" = 30 + quotes = 32
         constexpr size_t max_size = 22 + (frac_digits > 0 ? 1 + frac_digits : 0);
         if (!ensure_space(ctx, b, ix + max_size)) [[unlikely]] {
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

         if constexpr (not check_unquoted(Opts)) {
            b[ix++] = '"';
         }
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

         b[ix++] = 'Z';
         if constexpr (not check_unquoted(Opts)) {
            b[ix++] = '"';
         }
      }
   };

   // steady_clock::time_point: serialize as count in the time_point's native duration
   template <is_steady_time_point T>
      requires(not custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         const auto count = value.time_since_epoch().count();
         to<JSON, Rep>::template op<Opts>(count, ctx, b, ix);
      }
   };

   // high_resolution_clock::time_point when it's a distinct type (rare)
   template <is_high_res_time_point T>
      requires(not custom_write<T>)
   struct to<JSON, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         // Treat like steady_clock - serialize as count since epoch is implementation-defined
         using Duration = typename std::remove_cvref_t<T>::duration;
         using Rep = typename Duration::rep;
         const auto count = value.time_since_epoch().count();
         to<JSON, Rep>::template op<Opts>(count, ctx, b, ix);
      }
   };

   // epoch_time wrapper: serialize as numeric Unix timestamp
   template <class Duration>
      requires(not custom_write<epoch_time<Duration>>)
   struct to<JSON, epoch_time<Duration>>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& wrapper, is_context auto&& ctx, B&& b, auto& ix) noexcept
      {
         using Rep = typename Duration::rep;
         const auto count = std::chrono::duration_cast<Duration>(wrapper.value.time_since_epoch()).count();
         to<JSON, Rep>::template op<Opts>(count, ctx, b, ix);
      }
   };

   template <write_supported<JSON> T, output_buffer Buffer>
   [[nodiscard]] error_ctx write_json(T&& value, Buffer&& buffer)
   {
      return write<opts{}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <write_supported<JSON> T, raw_buffer Buffer>
   [[nodiscard]] error_ctx write_json(T&& value, Buffer&& buffer)
   {
      return write<opts{}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <write_supported<JSON> T>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_json(T&& value)
   {
      return write<opts{}>(std::forward<T>(value));
   }

   template <auto& Partial, write_supported<JSON> T, output_buffer Buffer>
   [[nodiscard]] error_ctx write_json(T&& value, Buffer&& buffer)
   {
      return write<Partial, opts{}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <auto& Partial, write_supported<JSON> T, raw_buffer Buffer>
   [[nodiscard]] error_ctx write_json(T&& value, Buffer&& buffer)
   {
      return write<Partial, opts{}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   // Runtime partial write - writes only the keys specified at runtime
   // Keys is a range of string-like types (e.g., std::vector<std::string>, std::array<std::string_view, N>)
   // Output key order matches the order in the input container
   // Returns error_code::unknown_key if any key doesn't exist in the struct

   template <auto Opts = opts{}, class T, class Keys, output_buffer Buffer>
      requires((glaze_object_t<T> || reflectable<T>) && range<Keys>)
   [[nodiscard]] error_ctx write_json_partial(T&& value, const Keys& keys, Buffer&& buffer)
   {
      using traits = buffer_traits<std::remove_cvref_t<Buffer>>;

      if constexpr (traits::is_resizable) {
         if (buffer.size() < 2 * write_padding_bytes) {
            buffer.resize(2 * write_padding_bytes);
         }
      }
      context ctx{};
      size_t ix = 0;
      to_runtime_partial<std::remove_cvref_t<T>>::template op<set_json<Opts>()>(keys, std::forward<T>(value), ctx,
                                                                                buffer, ix);
      if (bool(ctx.error)) [[unlikely]] {
         return {ix, ctx.error, ctx.custom_error_message};
      }

      traits::finalize(buffer, ix);
      return {ix, error_code::none, ctx.custom_error_message};
   }

   template <auto Opts = opts{}, class T, class Keys, raw_buffer Buffer>
      requires((glaze_object_t<T> || reflectable<T>) && range<Keys>)
   [[nodiscard]] error_ctx write_json_partial(T&& value, const Keys& keys, Buffer&& buffer)
   {
      context ctx{};
      size_t ix = 0;
      to_runtime_partial<std::remove_cvref_t<T>>::template op<set_json<Opts>()>(keys, std::forward<T>(value), ctx,
                                                                                buffer, ix);
      if (bool(ctx.error)) [[unlikely]] {
         return {ix, ctx.error, ctx.custom_error_message};
      }
      return {ix, error_code::none, ctx.custom_error_message};
   }

   template <auto Opts = opts{}, class T, class Keys>
      requires((glaze_object_t<T> || reflectable<T>) && range<Keys>)
   [[nodiscard]] glz::expected<std::string, error_ctx> write_json_partial(T&& value, const Keys& keys)
   {
      std::string buffer{};
      const auto result = write_json_partial<Opts>(std::forward<T>(value), keys, buffer);
      if (result) [[unlikely]] {
         return glz::unexpected(static_cast<error_ctx>(result));
      }
      return {std::move(buffer)};
   }

   // Runtime exclude write - writes all keys EXCEPT those specified at runtime
   // Keys is a range of string-like types (e.g., std::vector<std::string>, std::array<std::string_view, N>)
   // Output key order matches the struct definition order
   // Returns error_code::unknown_key if any exclude key doesn't exist in the struct

   template <auto Opts = opts{}, class T, class Keys, output_buffer Buffer>
      requires((glaze_object_t<T> || reflectable<T>) && range<Keys>)
   [[nodiscard]] error_ctx write_json_exclude(T&& value, const Keys& exclude_keys, Buffer&& buffer)
   {
      using traits = buffer_traits<std::remove_cvref_t<Buffer>>;

      if constexpr (traits::is_resizable) {
         if (buffer.size() < 2 * write_padding_bytes) {
            buffer.resize(2 * write_padding_bytes);
         }
      }
      context ctx{};
      size_t ix = 0;
      to_runtime_exclude<std::remove_cvref_t<T>>::template op<set_json<Opts>()>(exclude_keys, std::forward<T>(value),
                                                                                ctx, buffer, ix);
      if (bool(ctx.error)) [[unlikely]] {
         return {ix, ctx.error, ctx.custom_error_message};
      }

      traits::finalize(buffer, ix);
      return {ix, error_code::none, ctx.custom_error_message};
   }

   template <auto Opts = opts{}, class T, class Keys, raw_buffer Buffer>
      requires((glaze_object_t<T> || reflectable<T>) && range<Keys>)
   [[nodiscard]] error_ctx write_json_exclude(T&& value, const Keys& exclude_keys, Buffer&& buffer)
   {
      context ctx{};
      size_t ix = 0;
      to_runtime_exclude<std::remove_cvref_t<T>>::template op<set_json<Opts>()>(exclude_keys, std::forward<T>(value),
                                                                                ctx, buffer, ix);
      if (bool(ctx.error)) [[unlikely]] {
         return {ix, ctx.error, ctx.custom_error_message};
      }
      return {ix, error_code::none, ctx.custom_error_message};
   }

   template <auto Opts = opts{}, class T, class Keys>
      requires((glaze_object_t<T> || reflectable<T>) && range<Keys>)
   [[nodiscard]] glz::expected<std::string, error_ctx> write_json_exclude(T&& value, const Keys& exclude_keys)
   {
      std::string buffer{};
      const auto result = write_json_exclude<Opts>(std::forward<T>(value), exclude_keys, buffer);
      if (result) [[unlikely]] {
         return glz::unexpected(static_cast<error_ctx>(result));
      }
      return {std::move(buffer)};
   }

   template <write_supported<JSON> T, class Buffer>
   [[nodiscard]] error_ctx write_jsonc(T&& value, Buffer&& buffer)
   {
      return write<opts{.comments = true}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <write_supported<JSON> T>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_jsonc(T&& value)
   {
      return write<opts{.comments = true}>(std::forward<T>(value));
   }

   template <auto Opts = opts{}, write_supported<JSON> T>
   [[nodiscard]] error_ctx write_file_json(T&& value, const sv file_name, auto&& buffer)
   {
      const auto ec = write<set_json<Opts>()>(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         return ec;
      }
      if (auto file_ec = buffer_to_file(buffer, file_name); bool(file_ec)) {
         return {0, file_ec};
      }
      return {};
   }
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
