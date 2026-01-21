// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <utility>

#include "glaze/beve/header.hpp"
#include "glaze/beve/key_traits.hpp"
#include "glaze/core/buffer_traits.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/seek.hpp"
#include "glaze/core/to.hpp"
#include "glaze/core/write.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   template <>
   struct serialize<BEVE>
   {
      template <auto Opts, class T, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<BEVE, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                             std::forward<B>(b), std::forward<IX>(ix));
      }

      template <auto Opts, class T, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void no_header(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<BEVE, std::remove_cvref_t<T>>::template no_header<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                                    std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   // Context-aware dump_type: sets ctx.error on buffer overflow
   template <class B>
   GLZ_ALWAYS_INLINE void dump_type(is_context auto& ctx, auto&& value, B& b, auto& ix)
   {
      using V = std::decay_t<decltype(value)>;
      constexpr auto n = sizeof(V);

      if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
         return;
      }

      if constexpr (n == 1) {
         b[ix] = static_cast<std::decay_t<decltype(b[0])>>(value);
         ++ix;
      }
      else {
         constexpr auto is_volatile = std::is_volatile_v<std::remove_reference_t<decltype(value)>>;

         if constexpr (is_volatile) {
            V temp = value;
            if constexpr (std::endian::native == std::endian::big) {
               byteswap_le(temp);
            }
            std::memcpy(&b[ix], &temp, n);
         }
         else {
            if constexpr (std::endian::native == std::endian::big &&
                          (std::integral<V> || std::floating_point<V> || std::is_enum_v<V>)) {
               V temp = value;
               byteswap_le(temp);
               std::memcpy(&b[ix], &temp, n);
            }
            else {
               std::memcpy(&b[ix], &value, n);
            }
         }
         ix += n;
      }
   }

   // Legacy version for internal use where context is not available
   GLZ_ALWAYS_INLINE void dump_type(auto&& value, auto&& b, auto& ix)
   {
      using V = std::decay_t<decltype(value)>;
      using Buffer = std::remove_cvref_t<decltype(b)>;
      constexpr auto n = sizeof(V);

      if constexpr (n == 1) {
         // Optimized single-byte path: direct assignment instead of memcpy
         if constexpr (vector_like<Buffer>) {
            if (ix == b.size()) [[unlikely]] {
               b.resize(b.size() == 0 ? 128 : b.size() * 2);
            }
         }
         b[ix] = static_cast<std::decay_t<decltype(b[0])>>(value);
         ++ix;
      }
      else {
         if constexpr (vector_like<Buffer>) {
            if (const auto k = ix + n; k > b.size()) [[unlikely]] {
               b.resize(2 * k);
            }
         }

         constexpr auto is_volatile = std::is_volatile_v<std::remove_reference_t<decltype(value)>>;

         if constexpr (is_volatile) {
            V temp = value;
            if constexpr (std::endian::native == std::endian::big) {
               byteswap_le(temp);
            }
            std::memcpy(&b[ix], &temp, n);
         }
         else {
            if constexpr (std::endian::native == std::endian::big &&
                          (std::integral<V> || std::floating_point<V> || std::is_enum_v<V>)) {
               V temp = value;
               byteswap_le(temp);
               std::memcpy(&b[ix], &temp, n);
            }
            else {
               std::memcpy(&b[ix], &value, n);
            }
         }
         ix += n;
      }
   }

   template <uint64_t i, class... Args>
   GLZ_ALWAYS_INLINE void dump_compressed_int(Args&&... args)
   {
      if constexpr (i < 64) {
         const uint8_t c = uint8_t(i) << 2;
         dump_type(c, args...);
      }
      else if constexpr (i < 16384) {
         const uint16_t c = uint16_t(1) | (uint16_t(i) << 2);
         dump_type(c, args...);
      }
      else if constexpr (i < 1073741824) {
         const uint32_t c = uint32_t(2) | (uint32_t(i) << 2);
         dump_type(c, args...);
      }
      else if constexpr (i < 4611686018427387904) {
         const uint64_t c = uint64_t(3) | (uint64_t(i) << 2);
         dump_type(c, args...);
      }
      else {
         static_assert(i >= 4611686018427387904, "size not supported");
      }
   }

   // Context-aware version of dump_compressed_int (compile-time known size)
   // Sets ctx.error on buffer overflow
   template <uint64_t i, class B>
   GLZ_ALWAYS_INLINE void dump_compressed_int(is_context auto& ctx, B& b, size_t& ix)
   {
      if constexpr (i < 64) {
         const uint8_t c = uint8_t(i) << 2;
         dump_type(ctx, c, b, ix);
      }
      else if constexpr (i < 16384) {
         const uint16_t c = uint16_t(1) | (uint16_t(i) << 2);
         dump_type(ctx, c, b, ix);
      }
      else if constexpr (i < 1073741824) {
         const uint32_t c = uint32_t(2) | (uint32_t(i) << 2);
         dump_type(ctx, c, b, ix);
      }
      else if constexpr (i < 4611686018427387904) {
         const uint64_t c = uint64_t(3) | (uint64_t(i) << 2);
         dump_type(ctx, c, b, ix);
      }
      else {
         static_assert(i >= 4611686018427387904, "size not supported");
      }
   }

   // Context-aware version of dump_compressed_int (runtime size)
   // Sets ctx.error on buffer overflow
   template <class B>
   GLZ_ALWAYS_INLINE void dump_compressed_int(is_context auto& ctx, uint64_t i, B& b, size_t& ix)
   {
      if (i < 64) {
         const uint8_t c = uint8_t(i) << 2;
         dump_type(ctx, c, b, ix);
      }
      else if (i < 16384) {
         const uint16_t c = uint16_t(1) | (uint16_t(i) << 2);
         dump_type(ctx, c, b, ix);
      }
      else if (i < 1073741824) {
         const uint32_t c = uint32_t(2) | (uint32_t(i) << 2);
         dump_type(ctx, c, b, ix);
      }
      else if (i < 4611686018427387904) {
         const uint64_t c = uint64_t(3) | (uint64_t(i) << 2);
         dump_type(ctx, c, b, ix);
      }
      else {
         std::abort(); // this should never happen because we should never allocate containers of this size
      }
   }

   template <auto Opts, class... Args>
   GLZ_ALWAYS_INLINE void dump_compressed_int(uint64_t i, Args&&... args)
   {
      if (i < 64) {
         const uint8_t c = uint8_t(i) << 2;
         dump_type(c, args...);
      }
      else if (i < 16384) {
         const uint16_t c = uint16_t(1) | (uint16_t(i) << 2);
         dump_type(c, args...);
      }
      else if (i < 1073741824) {
         const uint32_t c = uint32_t(2) | (uint32_t(i) << 2);
         dump_type(c, args...);
      }
      else if (i < 4611686018427387904) {
         const uint64_t c = uint64_t(3) | (uint64_t(i) << 2);
         dump_type(c, args...);
      }
      else {
         std::abort(); // this should never happen because we should never allocate containers of this size
      }
   }

   template <auto& Partial, auto Opts, class T, class Ctx, class B, class IX>
   concept write_beve_partial_invocable = requires(T&& value, Ctx&& ctx, B&& b, IX&& ix) {
      to_partial<BEVE, std::remove_cvref_t<T>>::template op<Partial, Opts>(
         std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
   };

   template <>
   struct serialize_partial<BEVE>
   {
      template <auto& Partial, auto Opts, class T, is_context Ctx, class B, class IX>
      static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         if constexpr (std::count(Partial.begin(), Partial.end(), "") > 0) {
            serialize<BEVE>::op<Opts>(value, ctx, b, ix);
         }
         else if constexpr (write_beve_partial_invocable<Partial, Opts, T, Ctx, B, IX>) {
            to_partial<BEVE, std::remove_cvref_t<T>>::template op<Partial, Opts>(
               std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
         }
         else {
            static_assert(false_v<T>, "Glaze metadata is probably needed for your type");
         }
      }
   };

   // Only object types are supported for partial
   template <class T>
      requires(glaze_object_t<T> || writable_map_t<T> || reflectable<T>)
   struct to_partial<BEVE, T> final
   {
      template <auto& Partial, auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         static constexpr auto sorted = sort_json_ptrs(Partial);
         static constexpr auto groups = glz::group_json_ptrs<sorted>();
         static constexpr auto N = glz::tuple_size_v<std::decay_t<decltype(groups)>>;

         constexpr uint8_t type = 0; // string
         constexpr uint8_t tag = tag::object | type;
         dump_type(tag, b, ix);

         dump_compressed_int<N>(b, ix);

         if constexpr (glaze_object_t<T>) {
            for_each<N>([&]<auto I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               static constexpr auto group = glz::get<I>(groups);

               static constexpr auto key = get<0>(group);
               static constexpr auto sub_partial = get<1>(group);
               static constexpr auto index = key_index<T>(key);
               static_assert(index < reflect<T>::size, "Invalid key passed to partial write");

               if constexpr (glaze_object_t<T>) {
                  static constexpr auto member = get<index>(reflect<T>::values);
                  to<BEVE, std::remove_cvref_t<decltype(key)>>::template no_header_cx<key.size()>(key, ctx, b, ix);
                  serialize_partial<BEVE>::op<sub_partial, Opts>(get_member(value, member), ctx, b, ix);
               }
               else {
                  to<BEVE, std::remove_cvref_t<decltype(key)>>::template no_header_cx<key.size()>(key, ctx, b, ix);
                  serialize_partial<BEVE>::op<sub_partial, Opts>(get_member(value, get<index>(to_tie(value))), ctx, b,
                                                                 ix);
               }
            });
         }
         else if constexpr (writable_map_t<T>) {
            for_each<N>([&]<auto I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               static constexpr auto group = glz::get<I>(groups);

               static constexpr auto key_value = get<0>(group);
               static constexpr auto sub_partial = get<1>(group);
               if constexpr (findable<std::decay_t<T>, decltype(key_value)>) {
                  to<BEVE, std::remove_cvref_t<decltype(key_value)>>::template no_header_cx<key_value.size()>(
                     key_value, ctx, b, ix);
                  auto it = value.find(key_value);
                  if (it != value.end()) {
                     serialize_partial<BEVE>::op<sub_partial, Opts>(it->second, ctx, b, ix);
                  }
                  else {
                     ctx.error = error_code::invalid_partial_key;
                     return;
                  }
               }
               else {
                  static thread_local auto key =
                     typename std::decay_t<T>::key_type(key_value); // TODO handle numeric keys
                  serialize<BEVE>::no_header<Opts>(key, ctx, b, ix);
                  auto it = value.find(key);
                  if (it != value.end()) {
                     serialize_partial<BEVE>::op<sub_partial, Opts>(it->second, ctx, b, ix);
                  }
                  else {
                     ctx.error = error_code::invalid_partial_key;
                     return;
                  }
               }
            });
         }
      }
   };

   template <class T>
      requires(glaze_value_t<T> && !custom_write<T>)
   struct to<BEVE, T>
   {
      template <auto Opts, class Value, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         to<BEVE, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                        std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
      }

      template <auto Opts, class Value, is_context Ctx, class... Args>
      GLZ_ALWAYS_INLINE static void no_header(Value&& value, Ctx&& ctx, Args&&... args)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         to<BEVE, V>::template no_header<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                               std::forward<Ctx>(ctx), std::forward<Args>(args)...);
      }
   };

   template <always_null_t T>
   struct to<BEVE, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_type(ctx, uint8_t{0}, b, ix);
      }
   };

   template <is_bitset T>
   struct to<BEVE, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         constexpr uint8_t type = uint8_t(3) << 3;
         constexpr uint8_t tag = tag::typed_array | type;
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_compressed_int(ctx, value.size(), b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         // constexpr auto num_bytes = (value.size() + 7) / 8;
         const auto num_bytes = (value.size() + 7) / 8;
         // .size() should be constexpr, but clang doesn't support this
         std::vector<uint8_t> bytes(num_bytes);
         // std::array<uint8_t, num_bytes> bytes{};
         for (size_t byte_i{}, i{}; byte_i < num_bytes; ++byte_i) {
            for (size_t bit_i = 0; bit_i < 8 && i < value.size(); ++bit_i, ++i) {
               bytes[byte_i] |= uint8_t(value[i]) << uint8_t(bit_i);
            }
         }
         if (!ensure_space(ctx, b, ix + bytes.size() + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump(bytes, b, ix);
      }
   };

   template <glaze_flags_t T>
   struct to<BEVE, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto N = reflect<T>::size;
         static constexpr auto data_size = byte_length<T>();

         std::array<uint8_t, data_size> data{};

         for_each<N>([&]<size_t I>() {
            data[I / 8] |= static_cast<uint8_t>(get_member(value, get<I>(reflect<T>::values))) << (7 - (I % 8));
         });

         if (!ensure_space(ctx, b, ix + data_size + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump(data, b, ix);
      }
   };

   template <is_member_function_pointer T>
   struct to<BEVE, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&&, Args&&...) noexcept
      {}
   };

   // Handle function pointers and function references (no-op like member function pointers)
   template <class T>
      requires is_function_ptr_or_ref<T>
   struct to<BEVE, T>
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&&, Args&&...) noexcept
      {}
   };

   // write includers as empty strings
   template <is_includer T>
   struct to<BEVE, T>
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, B&& b, auto& ix)
      {
         constexpr uint8_t tag = tag::string;

         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_compressed_int<0>(ctx, b, ix);
      }
   };

   template <boolean_like T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(const bool value, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_type(ctx, value ? tag::bool_true : tag::bool_false, b, ix);
      }
   };

   template <func_t T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         serialize<BEVE>::op<Opts>(name_v<std::decay_t<decltype(value)>>, ctx, args...);
      }
   };

   template <class T>
   struct to<BEVE, basic_raw_json<T>> final
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         serialize<BEVE>::op<Opts>(value.str, ctx, std::forward<Args>(args)...);
      }
   };

   template <class T>
   struct to<BEVE, basic_text<T>> final
   {
      template <auto Opts, class... Args>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         serialize<BEVE>::op<Opts>(value.str, ctx, std::forward<Args>(args)...);
      }
   };

   template <class T, class V>
   constexpr size_t variant_index_v = []<size_t... I>(std::index_sequence<I...>) {
      return ((std::is_same_v<T, std::variant_alternative_t<I, V>> * I) + ...);
   }(std::make_index_sequence<std::variant_size_v<V>>{});

   template <is_variant T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using Variant = std::decay_t<decltype(value)>;

         std::visit(
            [&](auto&& v) {
               using V = std::decay_t<decltype(v)>;

               static constexpr uint64_t index = variant_index_v<V, Variant>;

               constexpr uint8_t tag = tag::extensions | 0b00001'000;

               dump_type(ctx, tag, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               dump_compressed_int<index>(ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               serialize<BEVE>::op<Opts>(v, ctx, b, ix);
            },
            value);
      }
   };

   template <class T>
      requires num_t<T> || char_t<T> || glaze_enum_t<T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         constexpr uint8_t type = std::floating_point<T> ? 0 : (std::is_signed_v<T> ? 0b000'01'000 : 0b000'10'000);
         constexpr uint8_t tag = tag::number | type | (byte_count<T> << 5);
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_type(ctx, value, b, ix);
      }

      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void no_header(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_type(ctx, value, b, ix);
      }
   };

   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T>)
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using V = std::underlying_type_t<std::decay_t<T>>;

         constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
         constexpr uint8_t tag = tag::number | type | (byte_count<V> << 5);
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_type(ctx, value, b, ix);
      }

      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void no_header(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_type(ctx, value, b, ix);
      }
   };

   template <class T>
      requires complex_t<T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         constexpr uint8_t tag = tag::extensions | 0b00011'000;
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         using V = typename T::value_type;
         constexpr uint8_t complex_number = 0;
         constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
         constexpr uint8_t complex_header = complex_number | type | (byte_count<V> << 5);
         dump_type(ctx, complex_header, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_type(ctx, value.real(), b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_type(ctx, value.imag(), b, ix);
      }

      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void no_header(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_type(ctx, value.real(), b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_type(ctx, value.imag(), b, ix);
      }
   };

   template <str_t T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         const sv str = [&]() -> const sv {
            if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
               return value ? value : "";
            }
            else {
               return sv{value};
            }
         }();

         constexpr uint8_t tag = tag::string;

         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         const auto n = str.size();
         dump_compressed_int(ctx, n, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
            return;
         }

         if (n) {
            std::memcpy(&b[ix], str.data(), n);
            ix += n;
         }
      }

      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void no_header(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_compressed_int(ctx, value.size(), b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         const auto n = value.size();
         if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
            return;
         }

         if (n) {
            std::memcpy(&b[ix], value.data(), n);
            ix += n;
         }
      }

      // Compile-time optimized version for known string sizes
      template <uint64_t N, class B>
      GLZ_ALWAYS_INLINE static void no_header_cx(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         dump_compressed_int<N>(ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (!ensure_space(ctx, b, ix + N + write_padding_bytes)) [[unlikely]] {
            return;
         }

         if constexpr (N > 0) {
            std::memcpy(&b[ix], value.data(), N);
            ix += N;
         }
      }
   };

   template <writable_array_t T>
   struct to<BEVE, T> final
   {
      static constexpr bool map_like_array = pair_t<range_value_t<T>>;

      template <auto Opts, class B>
         requires(map_like_array ? check_concatenate(Opts) == false : true)
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using V = range_value_t<std::decay_t<T>>;

         if constexpr (boolean_like<V>) {
            constexpr uint8_t type = uint8_t(3) << 3;
            constexpr uint8_t tag = tag::typed_array | type;
            dump_type(ctx, tag, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            dump_compressed_int(ctx, value.size(), b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            // booleans must be dumped using single bits
            if constexpr (has_static_size<T>) {
               constexpr auto N = std::tuple_size_v<std::decay_t<T>>;
               constexpr auto num_bytes = (N + 7) / 8;
               std::array<uint8_t, num_bytes> bytes{};
               for (size_t byte_i{}, i{}; byte_i < num_bytes; ++byte_i) {
                  for (size_t bit_i = 7; bit_i < 8 && i < N; --bit_i, ++i) {
                     bytes[byte_i] |= uint8_t(value[i]) << uint8_t(bit_i);
                  }
               }
               if (!ensure_space(ctx, b, ix + num_bytes + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump(bytes, b, ix);
            }
            else if constexpr (accessible<T>) {
               const auto num_bytes = (value.size() + 7) / 8;
               if (!ensure_space(ctx, b, ix + num_bytes + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               for (size_t byte_i{}, i{}; byte_i < num_bytes; ++byte_i) {
                  uint8_t byte{};
                  for (size_t bit_i = 7; bit_i < 8 && i < value.size(); --bit_i, ++i) {
                     byte |= uint8_t(value[i]) << uint8_t(bit_i);
                  }
                  dump_type(ctx, byte, b, ix);
               }
            }
            else {
               static_assert(false_v<T>);
            }
         }
         else if constexpr (num_t<V>) {
            constexpr uint8_t type = std::floating_point<V> ? 0 : (std::is_signed_v<V> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t tag = tag::typed_array | type | (byte_count<V> << 5);
            dump_type(ctx, tag, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            dump_compressed_int(ctx, value.size(), b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (contiguous<T>) {
               constexpr auto is_volatile =
                  std::is_volatile_v<std::remove_reference_t<std::remove_pointer_t<decltype(value.data())>>>;

               const auto n = value.size() * sizeof(V);
               if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
                  return;
               }

               if constexpr (is_volatile) {
                  const auto n_elements = value.size();
                  for (size_t i = 0; i < n_elements; ++i) {
                     V temp = value[i];
                     if constexpr (std::endian::native == std::endian::big) {
                        byteswap_le(temp);
                     }
                     std::memcpy(&b[ix], &temp, sizeof(V));
                     ix += sizeof(V);
                  }
               }
               else if constexpr (std::endian::native == std::endian::big && sizeof(V) > 1) {
                  // On big endian, swap each element
                  const auto n_elements = static_cast<size_t>(value.size());
                  for (size_t i = 0; i < n_elements; ++i) {
                     V temp = value[i];
                     byteswap_le(temp);
                     std::memcpy(&b[ix], &temp, sizeof(V));
                     ix += sizeof(V);
                  }
               }
               else {
                  // Little endian or single-byte: bulk memcpy
                  if (n) {
                     std::memcpy(&b[ix], value.data(), n);
                     ix += n;
                  }
               }
            }
            else {
               // Non-contiguous: need to check space for each element
               if (!ensure_space(ctx, b, ix + value.size() * sizeof(V) + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               for (auto& x : value) {
                  dump_type(ctx, x, b, ix);
               }
            }
         }
         else if constexpr (str_t<V>) {
            constexpr uint8_t type = uint8_t(3) << 3;
            constexpr uint8_t string_indicator = uint8_t(1) << 5;
            constexpr uint8_t tag = tag::typed_array | type | string_indicator;
            dump_type(ctx, tag, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            dump_compressed_int(ctx, value.size(), b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            for (auto& x : value) {
               dump_compressed_int(ctx, x.size(), b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               const auto n = x.size();
               if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
                  return;
               }

               if (n) {
                  std::memcpy(&b[ix], x.data(), n);
                  ix += n;
               }
            }
         }
         else if constexpr (complex_t<V>) {
            constexpr uint8_t tag = tag::extensions | 0b00011'000;
            dump_type(ctx, tag, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            using X = typename V::value_type;
            constexpr uint8_t complex_array = 1;
            constexpr uint8_t type = std::floating_point<X> ? 0 : (std::is_signed_v<X> ? 0b000'01'000 : 0b000'10'000);
            constexpr uint8_t complex_header = complex_array | type | (byte_count<X> << 5);
            dump_type(ctx, complex_header, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            dump_compressed_int(ctx, value.size(), b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if constexpr (contiguous<T>) {
               const auto n = value.size() * sizeof(V);
               if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
                  return;
               }

               if constexpr (std::endian::native == std::endian::big) {
                  // On big endian, swap each component
                  for (auto&& x : value) {
                     X real_val = x.real();
                     X imag_val = x.imag();
                     byteswap_le(real_val);
                     byteswap_le(imag_val);
                     std::memcpy(&b[ix], &real_val, sizeof(X));
                     ix += sizeof(X);
                     std::memcpy(&b[ix], &imag_val, sizeof(X));
                     ix += sizeof(X);
                  }
               }
               else {
                  // Little endian: bulk memcpy
                  if (n) {
                     std::memcpy(&b[ix], value.data(), n);
                     ix += n;
                  }
               }
            }
            else {
               for (auto&& x : value) {
                  serialize<BEVE>::no_header<Opts>(x, ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }
            }
         }
         else {
            constexpr uint8_t tag = tag::generic_array;
            dump_type(ctx, tag, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            dump_compressed_int(ctx, value.size(), b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            for (auto&& x : value) {
               serialize<BEVE>::op<Opts>(x, ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if constexpr (is_output_streaming<std::remove_cvref_t<B>>) {
                  flush_buffer(b, ix);
               }
            }
         }
      }

      template <auto Opts, class B>
         requires(map_like_array && check_concatenate(Opts) == true)
      static auto op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using Element = typename T::value_type;
         using Key = typename Element::first_type;

         constexpr uint8_t tag = beve_key_traits<Key>::header;
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         dump_compressed_int(ctx, value.size(), b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         for (auto&& [k, v] : value) {
            serialize<BEVE>::no_header<Opts>(k, ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            serialize<BEVE>::op<Opts>(v, ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
         }
      }
   };

   template <pair_t T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static auto op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using Key = typename T::first_type;

         constexpr uint8_t tag = beve_key_traits<Key>::header;
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         dump_compressed_int<1>(ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         const auto& [k, v] = value;
         serialize<BEVE>::no_header<Opts>(k, ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         serialize<BEVE>::op<Opts>(v, ctx, b, ix);
      }
   };

   template <writable_map_t T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      static auto op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using Key = typename T::key_type;

         constexpr uint8_t tag = beve_key_traits<Key>::header;
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         dump_compressed_int(ctx, value.size(), b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         for (auto&& [k, v] : value) {
            serialize<BEVE>::no_header<Opts>(k, ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            serialize<BEVE>::op<Opts>(v, ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if constexpr (is_output_streaming<std::remove_cvref_t<B>>) {
               flush_buffer(b, ix);
            }
         }
      }
   };

   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct to<BEVE, T>
   {
      template <auto Opts, class V, size_t N, class... Args>
      GLZ_ALWAYS_INLINE static void op(const V (&value)[N], is_context auto&& ctx, Args&&... args)
      {
         serialize<BEVE>::op<Opts>(std::span{value, N}, ctx, std::forward<Args>(args)...);
      }
   };

   template <nullable_t T>
      requires(!std::is_array_v<T>)
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (value) {
            serialize<BEVE>::op<Opts>(*value, ctx, b, ix);
         }
         else {
            if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump<tag::null>(b, ix);
         }
      }
   };

   template <class T>
      requires(nullable_value_t<T> && not nullable_like<T> && not is_expected<T>)
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (value.has_value()) {
            serialize<BEVE>::op<Opts>(value.value(), ctx, b, ix);
         }
         else {
            if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump<tag::null>(b, ix);
         }
      }
   };

   template <class T>
      requires is_specialization_v<T, glz::obj> || is_specialization_v<T, glz::obj_copy>
   struct to<BEVE, T>
   {
      template <auto Options, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V> / 2;

         if constexpr (!check_opening_handled(Options)) {
            constexpr uint8_t type = 0; // string key
            constexpr uint8_t tag = tag::object | type;
            dump_type(ctx, tag, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            dump_compressed_int<N>(ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
         }

         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            constexpr auto Opts = opening_handled_off<Options>();
            serialize<BEVE>::no_header<Opts>(get<2 * I>(value.value), ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            serialize<BEVE>::op<Opts>(get<2 * I + 1>(value.value), ctx, b, ix);
         });
      }
   };

   template <class T>
      requires is_specialization_v<T, glz::merge>
   struct to<BEVE, T>
   {
      template <auto Opts, class Value, size_t I>
      static consteval bool should_skip_field()
      {
         using V = field_t<Value, I>;

         if constexpr (std::same_as<V, hidden> || std::same_as<V, skip>) {
            return true;
         }
         else if constexpr (is_any_function_ptr<V>) {
            return !check_write_function_pointers(Opts);
         }
         else {
            return false;
         }
      }

      template <auto Opts, class Value>
      static consteval size_t count_fields_for_type()
      {
         constexpr auto N = reflect<Value>::size;
         return []<size_t... I>(std::index_sequence<I...>) {
            return (size_t{} + ... + (should_skip_field<Opts, Value, I>() ? size_t{} : size_t{1}));
         }(std::make_index_sequence<N>{});
      }

      template <auto Opts>
      static consteval size_t merge_element_count()
      {
         size_t count{};
         using Tuple = std::decay_t<decltype(std::declval<T>().value)>;
         for_each<glz::tuple_size_v<Tuple>>([&]<auto I>() constexpr {
            using Value = std::decay_t<glz::tuple_element_t<I, Tuple>>;
            if constexpr (is_specialization_v<Value, glz::obj> || is_specialization_v<Value, glz::obj_copy>) {
               count += glz::tuple_size_v<decltype(std::declval<Value>().value)> / 2;
            }
            else {
               count += count_fields_for_type<Opts, Value>();
            }
         });
         return count;
      }

      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V>;

         constexpr uint8_t type = 0; // string key
         constexpr uint8_t tag = tag::object | type;
         dump_type(ctx, tag, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         dump_compressed_int<merge_element_count<Opts>()>(ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         [&]<size_t... I>(std::index_sequence<I...>) {
            ((serialize<BEVE>::op<opening_handled<Opts>()>(glz::get<I>(value.value), ctx, b, ix),
              bool(ctx.error) ? void() : void()),
             ...);
         }(std::make_index_sequence<N>{});
      }
   };

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_write<T>)
   struct to<BEVE, T> final
   {
      static constexpr auto N = reflect<T>::size;

      template <auto Opts, size_t I>
      static consteval bool should_skip_field()
      {
         using V = field_t<T, I>;

         if constexpr (std::same_as<V, hidden> || std::same_as<V, skip>) {
            return true;
         }
         else if constexpr (is_any_function_ptr<V>) {
            return !check_write_function_pointers(Opts);
         }
         else {
            return false;
         }
      }

      template <auto Opts>
      static consteval size_t count_to_write()
      {
         return []<size_t... I>(std::index_sequence<I...>) {
            return (size_t{} + ... + (should_skip_field<Opts, I>() ? size_t{} : size_t{1}));
         }(std::make_index_sequence<N>{});
      }

      template <auto Opts, class B>
         requires(check_structs_as_arrays(Opts) == true)
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump<tag::generic_array>(b, ix);
         dump_compressed_int<count_to_write<Opts>()>(ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if constexpr (should_skip_field<Opts, I>()) {
               return;
            }
            else {
               if constexpr (reflectable<T>) {
                  serialize<BEVE>::op<Opts>(get_member(value, get<I>(t)), ctx, b, ix);
               }
               else {
                  serialize<BEVE>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, b, ix);
               }
            }
         });
      }

      template <auto Options, class B>
         requires(check_structs_as_arrays(Options) == false)
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         constexpr auto Opts = opening_handled_off<Options>();

         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         if constexpr (maybe_skipped<Options, T>) {
            // Dynamic path: count members at runtime to handle skip_null_members
            size_t member_count = 0;

            // First pass: count members that will be written
            for_each<N>([&]<size_t I>() {
               if constexpr (should_skip_field<Options, I>()) {
                  return;
               }
               else {
                  using val_t = field_t<T, I>;

                  if constexpr (null_t<val_t> && Options.skip_null_members) {
                     if constexpr (always_null_t<val_t>) {
                        return;
                     }
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
                        if (!is_null) {
                           ++member_count;
                        }
                     }
                  }
                  else {
                     ++member_count;
                  }
               }
            });

            // Write header with dynamic count
            if constexpr (!check_opening_handled(Options)) {
               constexpr uint8_t type = 0; // string key
               constexpr uint8_t tag = tag::object | type;
               dump_type(ctx, tag, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               dump_compressed_int(ctx, member_count, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
            }

            // Second pass: write members
            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if constexpr (should_skip_field<Options, I>()) {
                  return;
               }
               else {
                  using val_t = field_t<T, I>;

                  if constexpr (null_t<val_t> && Options.skip_null_members) {
                     if constexpr (always_null_t<val_t>) {
                        return;
                     }
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
                        if (is_null) {
                           return;
                        }
                     }
                  }

                  static constexpr sv key = reflect<T>::keys[I];
                  to<BEVE, std::remove_cvref_t<decltype(key)>>::template no_header_cx<key.size()>(key, ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  serialize<BEVE>::op<Opts>(get_member(value, member), ctx, b, ix);
                  if constexpr (is_output_streaming<std::remove_cvref_t<B>>) {
                     flush_buffer(b, ix);
                  }
               }
            });
         }
         else {
            // Static path: use compile-time count for better performance
            if constexpr (!check_opening_handled(Options)) {
               constexpr uint8_t type = 0; // string key
               constexpr uint8_t tag = tag::object | type;
               dump_type(ctx, tag, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               dump_compressed_int<count_to_write<Options>()>(ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
            }

            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               if constexpr (should_skip_field<Options, I>()) {
                  return;
               }
               else {
                  static constexpr sv key = reflect<T>::keys[I];
                  to<BEVE, std::remove_cvref_t<decltype(key)>>::template no_header_cx<key.size()>(key, ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  serialize<BEVE>::op<Opts>(get_member(value, member), ctx, b, ix);
                  if constexpr (is_output_streaming<std::remove_cvref_t<B>>) {
                     flush_buffer(b, ix);
                  }
               }
            });
         }
      }
   };

   template <class T>
      requires glaze_array_t<T>
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump<tag::generic_array>(b, ix);

         static constexpr auto N = reflect<T>::size;
         dump_compressed_int<N>(ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         for_each<reflect<T>::size>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            serialize<BEVE>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, b, ix);
         });
      }
   };

   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct to<BEVE, T> final
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump<tag::generic_array>(b, ix);

         static constexpr auto N = glz::tuple_size_v<T>;
         dump_compressed_int<N>(ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if constexpr (is_std_tuple<T>) {
            [&]<size_t... I>(std::index_sequence<I...>) {
               ((serialize<BEVE>::op<Opts>(std::get<I>(value), ctx, b, ix), bool(ctx.error) ? void() : void()), ...);
            }(std::make_index_sequence<N>{});
         }
         else {
            [&]<size_t... I>(std::index_sequence<I...>) {
               ((serialize<BEVE>::op<Opts>(glz::get<I>(value), ctx, b, ix), bool(ctx.error) ? void() : void()), ...);
            }(std::make_index_sequence<N>{});
         }
      }
   };

   template <write_supported<BEVE> T, class Buffer>
   [[nodiscard]] error_ctx write_beve(T&& value, Buffer&& buffer)
   {
      return write<opts{.format = BEVE}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <auto Opts = opts{}, write_supported<BEVE> T>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_beve(T&& value)
   {
      return write<set_beve<Opts>()>(std::forward<T>(value));
   }

   template <auto& Partial, write_supported<BEVE> T, class Buffer>
   [[nodiscard]] error_ctx write_beve(T&& value, Buffer&& buffer)
   {
      return write<Partial, opts{.format = BEVE}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   // requires file_name to be null terminated
   template <auto Opts = opts{}, write_supported<BEVE> T>
   [[nodiscard]] error_ctx write_file_beve(T&& value, const sv file_name, auto&& buffer)
   {
      static_assert(sizeof(decltype(*buffer.data())) == 1);

      const auto ec = write<set_beve<Opts>()>(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         return ec;
      }

      std::ofstream file(file_name.data(), std::ios::binary);

      if (file) {
         file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
      }
      else {
         return {0, error_code::file_open_failure};
      }

      return {};
   }

   template <write_supported<BEVE> T, class Buffer>
   [[nodiscard]] error_ctx write_beve_untagged(T&& value, Buffer&& buffer)
   {
      return write<opt_true<opts{.format = BEVE}, structs_as_arrays_opt_tag{}>>(std::forward<T>(value),
                                                                                std::forward<Buffer>(buffer));
   }

   template <write_supported<BEVE> T>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_beve_untagged(T&& value)
   {
      return write<opt_true<opts{.format = BEVE}, structs_as_arrays_opt_tag{}>>(std::forward<T>(value));
   }

   template <auto Opts = opts{}, write_supported<BEVE> T>
   [[nodiscard]] error_ctx write_file_beve_untagged(T&& value, const std::string& file_name, auto&& buffer)
   {
      return write_file_beve<opt_true<Opts, structs_as_arrays_opt_tag{}>>(std::forward<T>(value), file_name, buffer);
   }

   // ===== Delimited BEVE support for multiple objects in one buffer =====

   // Write the BEVE delimiter byte to a buffer
   // Used to separate multiple BEVE values in a stream/buffer (like NDJSON's newline)
   template <class Buffer>
   void write_beve_delimiter(Buffer& buffer)
   {
      buffer.push_back(static_cast<typename Buffer::value_type>(tag::delimiter));
   }

   // Append a BEVE value to an existing buffer without clearing it
   // Returns the number of bytes written via result.count
   template <auto Opts = opts{}, write_supported<BEVE> T, class Buffer>
      requires output_buffer<Buffer>
   [[nodiscard]] error_ctx write_beve_append(T&& value, Buffer& buffer)
   {
      using traits = buffer_traits<std::remove_cvref_t<Buffer>>;
      const size_t start_ix = buffer.size();

      if constexpr (traits::is_resizable) {
         if (buffer.size() < start_ix + 2 * write_padding_bytes) {
            buffer.resize(start_ix + 2 * write_padding_bytes);
         }
      }

      context ctx{};
      size_t ix = start_ix;
      to<BEVE, std::remove_cvref_t<T>>::template op<set_beve<Opts>()>(std::forward<T>(value), ctx, buffer, ix);

      if (bool(ctx.error)) [[unlikely]] {
         return {ix - start_ix, ctx.error, ctx.custom_error_message};
      }

      traits::finalize(buffer, ix);
      return {ix - start_ix, error_code::none, ctx.custom_error_message}; // bytes written
   }

   // Append a BEVE value to an existing buffer with a delimiter prefix
   // Useful for streaming multiple values
   template <auto Opts = opts{}, write_supported<BEVE> T, class Buffer>
      requires output_buffer<Buffer>
   [[nodiscard]] error_ctx write_beve_append_with_delimiter(T&& value, Buffer& buffer)
   {
      write_beve_delimiter(buffer);
      auto result = write_beve_append<Opts>(std::forward<T>(value), buffer);
      if (!result) {
         result.count += 1; // +1 for delimiter
      }
      return result;
   }

   // Write multiple BEVE values to a buffer with delimiters between them
   template <auto Opts = opts{}, class Container, class Buffer>
      requires output_buffer<Buffer> && readable_array_t<Container>
   [[nodiscard]] error_ctx write_beve_delimited(const Container& values, Buffer& buffer)
   {
      using traits = buffer_traits<std::remove_cvref_t<Buffer>>;
      context ctx{};

      if constexpr (traits::is_resizable) {
         if (buffer.size() < 2 * write_padding_bytes) {
            buffer.resize(2 * write_padding_bytes);
         }
      }

      size_t ix = 0;
      bool first = true;

      for (const auto& value : values) {
         if (!first) {
            // Write delimiter between values
            dump_type(tag::delimiter, buffer, ix);
         }
         first = false;

         to<BEVE, std::remove_cvref_t<decltype(value)>>::template op<set_beve<Opts>()>(value, ctx, buffer, ix);

         if (bool(ctx.error)) [[unlikely]] {
            traits::finalize(buffer, ix);
            return {ix, ctx.error, ctx.custom_error_message};
         }
      }

      traits::finalize(buffer, ix);
      return {ix, error_code::none, ctx.custom_error_message};
   }

   // Write multiple BEVE values to a buffer with delimiters, returning the buffer
   template <auto Opts = opts{}, class Container>
      requires readable_array_t<Container>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_beve_delimited(const Container& values)
   {
      std::string buffer{};
      const auto result = write_beve_delimited<Opts>(values, buffer);
      if (result) [[unlikely]] {
         return glz::unexpected(static_cast<error_ctx>(result));
      }
      return buffer;
   }
}
