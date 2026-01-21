// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/cbor/header.hpp"
#include "glaze/core/buffer_traits.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/to.hpp"
#include "glaze/core/write.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   template <>
   struct serialize<CBOR>
   {
      template <auto Opts, class T, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<CBOR, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                             std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   namespace cbor_detail
   {
      // Dump a single byte to the buffer (context-aware version with overflow checking)
      template <class B, class IX>
      GLZ_ALWAYS_INLINE bool dump_byte(is_context auto& ctx, uint8_t byte, B& b, IX& ix)
      {
         if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
            return false;
         }
         b[ix] = static_cast<typename std::decay_t<B>::value_type>(byte);
         ++ix;
         return true;
      }

      // Legacy dump_byte for internal use (no context)
      template <class B, class IX>
      GLZ_ALWAYS_INLINE void dump_byte(uint8_t byte, B& b, IX& ix)
      {
         if (ix >= b.size()) [[unlikely]] {
            b.resize(b.size() == 0 ? 128 : b.size() * 2);
         }
         b[ix] = static_cast<typename std::decay_t<B>::value_type>(byte);
         ++ix;
      }

      // Dump multiple bytes to the buffer (big-endian for CBOR, context-aware)
      template <class T, class B, class IX>
      GLZ_ALWAYS_INLINE bool dump_be(is_context auto& ctx, T value, B& b, IX& ix)
      {
         constexpr auto n = sizeof(T);
         if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
            return false;
         }

         if constexpr (std::endian::native == std::endian::little && n > 1) {
            value = std::byteswap(value);
         }
         std::memcpy(&b[ix], &value, n);
         ix += n;
         return true;
      }

      // Legacy dump_be for internal use (no context)
      template <class T, class B, class IX>
      GLZ_ALWAYS_INLINE void dump_be(T value, B& b, IX& ix)
      {
         constexpr auto n = sizeof(T);
         if (const auto k = ix + n; k > b.size()) [[unlikely]] {
            b.resize(2 * k);
         }

         if constexpr (std::endian::native == std::endian::little && n > 1) {
            value = std::byteswap(value);
         }
         std::memcpy(&b[ix], &value, n);
         ix += n;
      }

      // Encode CBOR argument with minimal bytes (context-aware version)
      template <class B, class IX>
      GLZ_ALWAYS_INLINE bool encode_arg(is_context auto& ctx, uint8_t major_type, uint64_t value, B& b, IX& ix)
      {
         using namespace cbor;

         if (value < 24) {
            return dump_byte(ctx, initial_byte(major_type, static_cast<uint8_t>(value)), b, ix);
         }
         else if (value <= 0xFF) {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint8_follows), b, ix)) return false;
            return dump_byte(ctx, static_cast<uint8_t>(value), b, ix);
         }
         else if (value <= 0xFFFF) {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint16_follows), b, ix)) return false;
            return dump_be(ctx, static_cast<uint16_t>(value), b, ix);
         }
         else if (value <= 0xFFFFFFFF) {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint32_follows), b, ix)) return false;
            return dump_be(ctx, static_cast<uint32_t>(value), b, ix);
         }
         else {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint64_follows), b, ix)) return false;
            return dump_be(ctx, value, b, ix);
         }
      }

      // Legacy encode_arg for internal use (no context)
      template <class B, class IX>
      GLZ_ALWAYS_INLINE void encode_arg(uint8_t major_type, uint64_t value, B& b, IX& ix)
      {
         using namespace cbor;

         if (value < 24) {
            dump_byte(initial_byte(major_type, static_cast<uint8_t>(value)), b, ix);
         }
         else if (value <= 0xFF) {
            dump_byte(initial_byte(major_type, info::uint8_follows), b, ix);
            dump_byte(static_cast<uint8_t>(value), b, ix);
         }
         else if (value <= 0xFFFF) {
            dump_byte(initial_byte(major_type, info::uint16_follows), b, ix);
            dump_be(static_cast<uint16_t>(value), b, ix);
         }
         else if (value <= 0xFFFFFFFF) {
            dump_byte(initial_byte(major_type, info::uint32_follows), b, ix);
            dump_be(static_cast<uint32_t>(value), b, ix);
         }
         else {
            dump_byte(initial_byte(major_type, info::uint64_follows), b, ix);
            dump_be(value, b, ix);
         }
      }

      // Compile-time version for known sizes (context-aware)
      template <uint64_t value, class B, class IX>
      GLZ_ALWAYS_INLINE bool encode_arg_cx(is_context auto& ctx, uint8_t major_type, B& b, IX& ix)
      {
         using namespace cbor;

         if constexpr (value < 24) {
            return dump_byte(ctx, initial_byte(major_type, static_cast<uint8_t>(value)), b, ix);
         }
         else if constexpr (value <= 0xFF) {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint8_follows), b, ix)) return false;
            return dump_byte(ctx, static_cast<uint8_t>(value), b, ix);
         }
         else if constexpr (value <= 0xFFFF) {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint16_follows), b, ix)) return false;
            return dump_be(ctx, static_cast<uint16_t>(value), b, ix);
         }
         else if constexpr (value <= 0xFFFFFFFF) {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint32_follows), b, ix)) return false;
            return dump_be(ctx, static_cast<uint32_t>(value), b, ix);
         }
         else {
            if (!dump_byte(ctx, initial_byte(major_type, info::uint64_follows), b, ix)) return false;
            return dump_be(ctx, value, b, ix);
         }
      }

      // Legacy compile-time version for internal use (no context)
      template <uint64_t value, class B, class IX>
      GLZ_ALWAYS_INLINE void encode_arg_cx(uint8_t major_type, B& b, IX& ix)
      {
         using namespace cbor;

         if constexpr (value < 24) {
            dump_byte(initial_byte(major_type, static_cast<uint8_t>(value)), b, ix);
         }
         else if constexpr (value <= 0xFF) {
            dump_byte(initial_byte(major_type, info::uint8_follows), b, ix);
            dump_byte(static_cast<uint8_t>(value), b, ix);
         }
         else if constexpr (value <= 0xFFFF) {
            dump_byte(initial_byte(major_type, info::uint16_follows), b, ix);
            dump_be(static_cast<uint16_t>(value), b, ix);
         }
         else if constexpr (value <= 0xFFFFFFFF) {
            dump_byte(initial_byte(major_type, info::uint32_follows), b, ix);
            dump_be(static_cast<uint32_t>(value), b, ix);
         }
         else {
            dump_byte(initial_byte(major_type, info::uint64_follows), b, ix);
            dump_be(value, b, ix);
         }
      }
   }

   // Null
   template <always_null_t T>
   struct to<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&& b, auto& ix)
      {
         cbor_detail::dump_byte(ctx, cbor::initial_byte(cbor::major::simple, cbor::simple::null_value), b, ix);
      }
   };

   // Bitset - stored as byte string
   template <is_bitset T>
   struct to<CBOR, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         const auto num_bytes = (value.size() + 7) / 8;

         // Write byte string header
         if (!cbor_detail::encode_arg(ctx, cbor::major::bstr, num_bytes, b, ix)) [[unlikely]] {
            return;
         }

         // Pack bits into bytes (LSB first within each byte)
         for (size_t byte_i = 0, bit_idx = 0; byte_i < num_bytes; ++byte_i) {
            uint8_t byte_val = 0;
            for (size_t bit_i = 0; bit_i < 8 && bit_idx < value.size(); ++bit_i, ++bit_idx) {
               byte_val |= uint8_t(value[bit_idx]) << uint8_t(bit_i);
            }
            if (!cbor_detail::dump_byte(ctx, byte_val, b, ix)) [[unlikely]] {
               return;
            }
         }
      }
   };

   // Complex numbers - tag 43000 with 2-element array [real, imag]
   template <class T>
      requires complex_t<T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         // Write tag 43000 (complex number)
         if (!cbor_detail::encode_arg(ctx, cbor::major::tag, cbor::semantic_tag::complex_number, b, ix)) [[unlikely]] {
            return;
         }
         // Write array header for 2 elements
         if (!cbor_detail::dump_byte(ctx, cbor::initial_byte(cbor::major::array, 2), b, ix)) [[unlikely]] {
            return;
         }
         // Write real part
         using V = typename std::remove_cvref_t<T>::value_type;
         to<CBOR, V>::template op<Opts>(value.real(), ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]]
            return;
         // Write imaginary part
         to<CBOR, V>::template op<Opts>(value.imag(), ctx, b, ix);
      }
   };

   // Boolean
   template <boolean_like T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(const bool value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using namespace cbor;
         const uint8_t byte =
            value ? initial_byte(major::simple, simple::true_value) : initial_byte(major::simple, simple::false_value);
         cbor_detail::dump_byte(ctx, byte, b, ix);
      }
   };

   // Unsigned integers
   template <class T>
      requires(std::unsigned_integral<T> && !std::same_as<T, bool>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         cbor_detail::encode_arg(ctx, cbor::major::uint, static_cast<uint64_t>(value), b, ix);
      }
   };

   // Signed integers
   template <class T>
      requires(std::signed_integral<T> && !std::same_as<T, bool>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (value >= 0) {
            cbor_detail::encode_arg(ctx, cbor::major::uint, static_cast<uint64_t>(value), b, ix);
         }
         else {
            // CBOR negative: encode n where value = -1 - n, so n = ~value
            // Using two's complement identity: ~value = -value - 1 = -1 - value
            // This safely handles INT64_MIN without overflow
            const uint64_t n = static_cast<uint64_t>(~value);
            cbor_detail::encode_arg(ctx, cbor::major::nint, n, b, ix);
         }
      }
   };

   // Floating-point with preferred serialization (smallest representation)
   template <std::floating_point T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using namespace cbor;

         const double d = static_cast<double>(value);

         // Try half precision first (preferred serialization)
         if (can_encode_half(d)) {
            if (!cbor_detail::dump_byte(ctx, initial_byte(major::simple, simple::float16), b, ix)) [[unlikely]] {
               return;
            }
            uint16_t half = encode_half(d);
            cbor_detail::dump_be(ctx, half, b, ix);
            return;
         }

         // Try single precision
         if (can_encode_float(d)) {
            if (!cbor_detail::dump_byte(ctx, initial_byte(major::simple, simple::float32), b, ix)) [[unlikely]] {
               return;
            }
            const float f = static_cast<float>(d);
            uint32_t bits;
            std::memcpy(&bits, &f, sizeof(float));
            cbor_detail::dump_be(ctx, bits, b, ix);
            return;
         }

         // Fall back to double precision
         if (!cbor_detail::dump_byte(ctx, initial_byte(major::simple, simple::float64), b, ix)) [[unlikely]] {
            return;
         }
         uint64_t bits;
         std::memcpy(&bits, &d, sizeof(double));
         cbor_detail::dump_be(ctx, bits, b, ix);
      }
   };

   // Text strings (UTF-8)
   template <str_t T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         const sv str = [&]() -> const sv {
            if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
               return value ? value : "";
            }
            else {
               return sv{value};
            }
         }();

         if (!cbor_detail::encode_arg(ctx, cbor::major::tstr, str.size(), b, ix)) [[unlikely]] {
            return;
         }

         const auto n = str.size();
         if (!ensure_space(ctx, b, ix + n + write_padding_bytes)) [[unlikely]] {
            return;
         }

         if (n) {
            std::memcpy(&b[ix], str.data(), n);
            ix += n;
         }
      }
   };

   // Byte strings (std::vector<std::byte>, std::span<std::byte>, etc.)
   template <class T>
      requires(std::same_as<typename T::value_type, std::byte> && !str_t<T>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      static void op(const auto& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (!cbor_detail::encode_arg(ctx, cbor::major::bstr, value.size(), b, ix)) [[unlikely]] {
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
   };

   // std::vector<uint8_t> as byte string
   template <>
   struct to<CBOR, std::vector<uint8_t>> final
   {
      template <auto Opts>
      static void op(const auto& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (!cbor_detail::encode_arg(ctx, cbor::major::bstr, value.size(), b, ix)) [[unlikely]] {
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
   };

   // std::array<std::byte, N> as byte string
   template <size_t N>
   struct to<CBOR, std::array<std::byte, N>> final
   {
      template <auto Opts>
      static void op(const auto& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (!cbor_detail::encode_arg_cx<N>(ctx, cbor::major::bstr, b, ix)) [[unlikely]] {
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

   // std::array<uint8_t, N> as byte string
   template <size_t N>
   struct to<CBOR, std::array<uint8_t, N>> final
   {
      template <auto Opts>
      static void op(const auto& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (!cbor_detail::encode_arg_cx<N>(ctx, cbor::major::bstr, b, ix)) [[unlikely]] {
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

   // Arrays (std::vector, std::array, std::deque, etc.)
   // Note: eigen_t types have their own specialization in glaze/ext/eigen.hpp
   template <writable_array_t T>
      requires(!eigen_t<T>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using V = range_value_t<std::remove_cvref_t<T>>;

         // Use RFC 8746 typed arrays for numeric types (bulk memcpy)
         if constexpr (num_t<V> && !std::same_as<V, bool> && contiguous<T>) {
            // Write the tag for this type using native endianness
            constexpr uint64_t tag = cbor::typed_array::native_tag<V>();
            if (!cbor_detail::encode_arg(ctx, cbor::major::tag, tag, b, ix)) [[unlikely]] {
               return;
            }

            // Write byte string header
            const size_t byte_len = value.size() * sizeof(V);
            if (!cbor_detail::encode_arg(ctx, cbor::major::bstr, byte_len, b, ix)) [[unlikely]] {
               return;
            }

            // Ensure buffer space
            if (!ensure_space(ctx, b, ix + byte_len + write_padding_bytes)) [[unlikely]] {
               return;
            }

            // Bulk write: native endianness matches tag
            if (byte_len > 0) {
               std::memcpy(&b[ix], value.data(), byte_len);
               ix += byte_len;
            }
         }
         else if constexpr (complex_t<V> && contiguous<T>) {
            // Complex array: tag 43001 with interleaved typed array [r0, i0, r1, i1, ...]
            // std::complex<T> stores data as [real, imag] pairs contiguously
            using Scalar = typename V::value_type;

            // Write tag 43001 (complex array)
            if (!cbor_detail::encode_arg(ctx, cbor::major::tag, cbor::semantic_tag::complex_array, b, ix))
               [[unlikely]] {
               return;
            }

            // Write typed array tag for the underlying scalar type
            constexpr uint64_t scalar_tag = cbor::typed_array::native_tag<Scalar>();
            if (!cbor_detail::encode_arg(ctx, cbor::major::tag, scalar_tag, b, ix)) [[unlikely]] {
               return;
            }

            // Write byte string header (2 scalars per complex: real and imag)
            const size_t byte_len = value.size() * sizeof(V); // sizeof(complex<T>) = 2 * sizeof(T)
            if (!cbor_detail::encode_arg(ctx, cbor::major::bstr, byte_len, b, ix)) [[unlikely]] {
               return;
            }

            // Ensure buffer space
            if (!ensure_space(ctx, b, ix + byte_len + write_padding_bytes)) [[unlikely]] {
               return;
            }

            // Bulk write: std::complex stores [real, imag] contiguously
            if (byte_len > 0) {
               std::memcpy(&b[ix], value.data(), byte_len);
               ix += byte_len;
            }
         }
         else {
            // Generic CBOR array for non-numeric or non-contiguous types
            if (!cbor_detail::encode_arg(ctx, cbor::major::array, value.size(), b, ix)) [[unlikely]] {
               return;
            }

            for (auto&& item : value) {
               serialize<CBOR>::op<Opts>(item, ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               if constexpr (is_output_streaming<decltype(b)>) {
                  flush_buffer(b, ix);
               }
            }
         }
      }
   };

   // Maps (std::map, std::unordered_map, etc.)
   template <writable_map_t T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (!cbor_detail::encode_arg(ctx, cbor::major::map, value.size(), b, ix)) [[unlikely]] {
            return;
         }

         for (auto&& [k, v] : value) {
            serialize<CBOR>::op<Opts>(k, ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]]
               return;
            serialize<CBOR>::op<Opts>(v, ctx, b, ix);
            if (bool(ctx.error)) [[unlikely]]
               return;
            if constexpr (is_output_streaming<decltype(b)>) {
               flush_buffer(b, ix);
            }
         }
      }
   };

   // Pairs
   template <pair_t T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (!cbor_detail::encode_arg_cx<1>(ctx, cbor::major::map, b, ix)) [[unlikely]] {
            return;
         }
         const auto& [k, v] = value;
         serialize<CBOR>::op<Opts>(k, ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]]
            return;
         serialize<CBOR>::op<Opts>(v, ctx, b, ix);
      }
   };

   // Glaze objects (structs with reflection)
   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && !custom_write<T>)
   struct to<CBOR, T> final
   {
      static constexpr auto N = reflect<T>::size;

      template <auto Opts, size_t I>
      static consteval bool should_skip_field()
      {
         using V = field_t<T, I>;

         if constexpr (std::same_as<V, hidden> || std::same_as<V, skip>) {
            return true;
         }
         else if constexpr (is_member_function_pointer<V>) {
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

      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         if constexpr (maybe_skipped<Opts, T>) {
            // Dynamic path: count members at runtime to handle skip_null_members
            size_t member_count = 0;

            // First pass: count members that will be written
            for_each<N>([&]<size_t I>() {
               if constexpr (should_skip_field<Opts, I>()) {
                  return;
               }
               else {
                  using val_t = field_t<T, I>;

                  if constexpr (null_t<val_t> && Opts.skip_null_members) {
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

            // Write map header with dynamic count
            if (!cbor_detail::encode_arg(ctx, cbor::major::map, member_count, b, ix)) [[unlikely]] {
               return;
            }

            // Second pass: write members
            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]]
                  return;
               if constexpr (should_skip_field<Opts, I>()) {
                  return;
               }
               else {
                  using val_t = field_t<T, I>;

                  if constexpr (null_t<val_t> && Opts.skip_null_members) {
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
                  // Write key as text string
                  if (!cbor_detail::encode_arg_cx<key.size()>(ctx, cbor::major::tstr, b, ix)) [[unlikely]] {
                     return;
                  }
                  if (!ensure_space(ctx, b, ix + key.size() + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  if constexpr (key.size() > 0) {
                     std::memcpy(&b[ix], key.data(), key.size());
                     ix += key.size();
                  }

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  serialize<CBOR>::op<Opts>(get_member(value, member), ctx, b, ix);
                  if constexpr (is_output_streaming<decltype(b)>) {
                     flush_buffer(b, ix);
                  }
               }
            });
         }
         else {
            // Static path: use compile-time count for better performance
            if (!cbor_detail::encode_arg_cx<count_to_write<Opts>()>(ctx, cbor::major::map, b, ix)) [[unlikely]] {
               return;
            }

            for_each<N>([&]<size_t I>() {
               if (bool(ctx.error)) [[unlikely]]
                  return;
               if constexpr (should_skip_field<Opts, I>()) {
                  return;
               }
               else {
                  static constexpr sv key = reflect<T>::keys[I];
                  // Write key as text string
                  if (!cbor_detail::encode_arg_cx<key.size()>(ctx, cbor::major::tstr, b, ix)) [[unlikely]] {
                     return;
                  }
                  if (!ensure_space(ctx, b, ix + key.size() + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  if constexpr (key.size() > 0) {
                     std::memcpy(&b[ix], key.data(), key.size());
                     ix += key.size();
                  }

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  serialize<CBOR>::op<Opts>(get_member(value, member), ctx, b, ix);
                  if constexpr (is_output_streaming<decltype(b)>) {
                     flush_buffer(b, ix);
                  }
               }
            });
         }
      }
   };

   // Tuples
   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         static constexpr auto N = glz::tuple_size_v<T>;
         if (!cbor_detail::encode_arg_cx<N>(ctx, cbor::major::array, b, ix)) [[unlikely]] {
            return;
         }

         if constexpr (is_std_tuple<T>) {
            [&]<size_t... I>(std::index_sequence<I...>) {
               (serialize<CBOR>::op<Opts>(std::get<I>(value), ctx, b, ix), ...);
            }(std::make_index_sequence<N>{});
         }
         else {
            [&]<size_t... I>(std::index_sequence<I...>) {
               (serialize<CBOR>::op<Opts>(glz::get<I>(value), ctx, b, ix), ...);
            }(std::make_index_sequence<N>{});
         }
      }
   };

   // Glaze arrays
   template <class T>
      requires glaze_array_t<T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         static constexpr auto N = reflect<T>::size;
         if (!cbor_detail::encode_arg_cx<N>(ctx, cbor::major::array, b, ix)) [[unlikely]] {
            return;
         }

         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]]
               return;
            serialize<CBOR>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, b, ix);
         });
      }
   };

   // Nullable types (std::optional, std::unique_ptr, std::shared_ptr)
   template <nullable_t T>
      requires(!std::is_array_v<T>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (value) {
            serialize<CBOR>::op<Opts>(*value, ctx, b, ix);
         }
         else {
            cbor_detail::dump_byte(ctx, cbor::initial_byte(cbor::major::simple, cbor::simple::null_value), b, ix);
         }
      }
   };

   // C-style arrays
   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct to<CBOR, T>
   {
      template <auto Opts, class V, size_t N>
      GLZ_ALWAYS_INLINE static void op(const V (&value)[N], is_context auto&& ctx, auto&& b, auto& ix)
      {
         serialize<CBOR>::op<Opts>(std::span{value, N}, ctx, b, ix);
      }
   };

   // Variants
   template <is_variant T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using Variant = std::decay_t<decltype(value)>;

         std::visit(
            [&](auto&& v) {
               using V = std::decay_t<decltype(v)>;

               static constexpr uint64_t index = []<size_t... I>(std::index_sequence<I...>) {
                  return ((std::is_same_v<V, std::variant_alternative_t<I, Variant>> * I) + ...);
               }(std::make_index_sequence<std::variant_size_v<Variant>>{});

               // Encode variant as array [index, value]
               if (!cbor_detail::encode_arg_cx<2>(ctx, cbor::major::array, b, ix)) [[unlikely]] {
                  return;
               }
               if (!cbor_detail::encode_arg(ctx, cbor::major::uint, index, b, ix)) [[unlikely]] {
                  return;
               }
               serialize<CBOR>::op<Opts>(v, ctx, b, ix);
            },
            value);
      }
   };

   // Glaze value wrapper
   template <class T>
      requires(glaze_value_t<T> && !custom_write<T>)
   struct to<CBOR, T>
   {
      template <auto Opts, class Value, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         to<CBOR, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                        std::forward<Ctx>(ctx), std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   // Enums with glaze reflection
   template <glaze_enum_t T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using V = std::underlying_type_t<std::decay_t<T>>;
         if constexpr (std::is_signed_v<V>) {
            const auto v = static_cast<V>(value);
            if (v >= 0) {
               cbor_detail::encode_arg(ctx, cbor::major::uint, static_cast<uint64_t>(v), b, ix);
            }
            else {
               cbor_detail::encode_arg(ctx, cbor::major::nint, static_cast<uint64_t>(~v), b, ix);
            }
         }
         else {
            cbor_detail::encode_arg(ctx, cbor::major::uint, static_cast<uint64_t>(value), b, ix);
         }
      }
   };

   // Plain enums (non-glaze)
   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using V = std::underlying_type_t<std::decay_t<T>>;
         if constexpr (std::is_signed_v<V>) {
            const auto v = static_cast<V>(value);
            if (v >= 0) {
               cbor_detail::encode_arg(ctx, cbor::major::uint, static_cast<uint64_t>(v), b, ix);
            }
            else {
               cbor_detail::encode_arg(ctx, cbor::major::nint, static_cast<uint64_t>(~v), b, ix);
            }
         }
         else {
            cbor_detail::encode_arg(ctx, cbor::major::uint, static_cast<uint64_t>(value), b, ix);
         }
      }
   };

   // Member function pointers (no-op)
   template <is_member_function_pointer T>
   struct to<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&&, auto&&, auto&&) noexcept
      {}
   };

   // Includers (write as empty string)
   template <is_includer T>
   struct to<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&& b, auto& ix)
      {
         cbor_detail::encode_arg_cx<0>(ctx, cbor::major::tstr, b, ix);
      }
   };

   // Function type names
   template <func_t T>
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         serialize<CBOR>::op<Opts>(name_v<std::decay_t<decltype(value)>>, ctx, b, ix);
      }
   };

   // Raw JSON (write as text string)
   template <class T>
   struct to<CBOR, basic_raw_json<T>> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         serialize<CBOR>::op<Opts>(value.str, ctx, b, ix);
      }
   };

   // Text (write as text string)
   template <class T>
   struct to<CBOR, basic_text<T>> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         serialize<CBOR>::op<Opts>(value.str, ctx, b, ix);
      }
   };

   // Nullable value types
   template <class T>
      requires(nullable_value_t<T> && !nullable_like<T> && !is_expected<T>)
   struct to<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         if (value.has_value()) {
            serialize<CBOR>::op<Opts>(value.value(), ctx, b, ix);
         }
         else {
            cbor_detail::dump_byte(ctx, cbor::initial_byte(cbor::major::simple, cbor::simple::null_value), b, ix);
         }
      }
   };

   // ===== High-level write APIs =====

   template <write_supported<CBOR> T, class Buffer>
   [[nodiscard]] error_ctx write_cbor(T&& value, Buffer&& buffer)
   {
      return write<opts{.format = CBOR}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <auto Opts = opts{}, write_supported<CBOR> T>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_cbor(T&& value)
   {
      return write<set_cbor<Opts>()>(std::forward<T>(value));
   }

   template <auto Opts = opts{}, write_supported<CBOR> T>
   [[nodiscard]] inline error_code write_file_cbor(T&& value, const sv file_name, auto&& buffer)
   {
      const auto ec = write<set_cbor<Opts>()>(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         return ec.ec;
      }
      return buffer_to_file(buffer, file_name);
   }
}
