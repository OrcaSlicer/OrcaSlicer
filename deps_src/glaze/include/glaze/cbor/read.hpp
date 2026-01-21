// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/cbor/header.hpp"
#include "glaze/cbor/skip.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/file/file_ops.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/for_each.hpp"

namespace glz
{
   namespace cbor_detail
   {
      // Decode CBOR argument (variable-length unsigned integer)
      [[nodiscard]] GLZ_ALWAYS_INLINE uint64_t decode_arg(is_context auto& ctx, auto& it, auto end,
                                                          uint8_t additional_info) noexcept
      {
         using namespace cbor;

         if (additional_info < 24) {
            return additional_info;
         }

         switch (additional_info) {
         case info::uint8_follows: {
            if (it >= end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return 0;
            }
            uint8_t val;
            std::memcpy(&val, it, 1);
            ++it;
            return val;
         }
         case info::uint16_follows: {
            if ((it + 2) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return 0;
            }
            uint16_t val;
            std::memcpy(&val, it, 2);
            if constexpr (std::endian::native == std::endian::little) {
               val = std::byteswap(val);
            }
            it += 2;
            return val;
         }
         case info::uint32_follows: {
            if ((it + 4) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return 0;
            }
            uint32_t val;
            std::memcpy(&val, it, 4);
            if constexpr (std::endian::native == std::endian::little) {
               val = std::byteswap(val);
            }
            it += 4;
            return val;
         }
         case info::uint64_follows: {
            if ((it + 8) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return 0;
            }
            uint64_t val;
            std::memcpy(&val, it, 8);
            if constexpr (std::endian::native == std::endian::little) {
               val = std::byteswap(val);
            }
            it += 8;
            return val;
         }
         default:
            ctx.error = error_code::syntax_error;
            return 0;
         }
      }
   }

   template <>
   struct parse<CBOR>
   {
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         if constexpr (const_value_v<T>) {
            if constexpr (check_error_on_const_read(Opts)) {
               ctx.error = error_code::attempt_const_read;
            }
            else {
               skip_value<CBOR>::op<Opts>(std::forward<Ctx>(ctx), std::forward<It0>(it), end);
            }
         }
         else {
            using V = std::remove_cvref_t<T>;
            from<CBOR, V>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it),
                                             end);
         }
      }
   };

   // Null
   template <always_null_t T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);

         if (initial != initial_byte(major::simple, simple::null_value)) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;
      }
   };

   // Skip type
   template <>
   struct from<CBOR, skip>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&... args) noexcept
      {
         skip_value<CBOR>::op<Opts>(ctx, args...);
      }
   };

   // Bitset - read from byte string
   template <is_bitset T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::bstr) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         const uint64_t num_bytes = cbor_detail::decode_arg(ctx, it, end, additional_info);
         if (bool(ctx.error)) [[unlikely]]
            return;

         const auto expected_bytes = (value.size() + 7) / 8;
         if (num_bytes != expected_bytes) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         if (static_cast<uint64_t>(end - it) < num_bytes) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // Unpack bytes into bits (LSB first within each byte)
         for (size_t byte_i = 0, bit_idx = 0; byte_i < num_bytes; ++byte_i, ++it) {
            uint8_t byte_val;
            std::memcpy(&byte_val, it, 1);
            for (size_t bit_i = 0; bit_i < 8 && bit_idx < value.size(); ++bit_i, ++bit_idx) {
               value[bit_idx] = (byte_val >> bit_i) & uint8_t(1);
            }
         }
      }
   };

   // Complex numbers - tag 43000 with 2-element array [real, imag]
   template <class T>
      requires complex_t<T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         // Expect tag 43000 (complex number)
         if (get_major_type(initial) != major::tag) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         const uint64_t tag = cbor_detail::decode_arg(ctx, it, end, get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (tag != semantic_tag::complex_number) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Read array header
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         std::memcpy(&initial, it, 1);
         ++it;

         if (get_major_type(initial) != major::array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Expect exactly 2 elements
         uint64_t count = cbor_detail::decode_arg(ctx, it, end, get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (count != 2) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Read real and imaginary parts
         using V = typename std::remove_cvref_t<T>::value_type;
         V real_part{}, imag_part{};
         from<CBOR, V>::template op<Opts>(real_part, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         from<CBOR, V>::template op<Opts>(imag_part, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;

         value = std::remove_cvref_t<T>{real_part, imag_part};
      }
   };

   // Boolean
   template <boolean_like T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         if (initial == initial_byte(major::simple, simple::false_value)) {
            value = false;
         }
         else if (initial == initial_byte(major::simple, simple::true_value)) {
            value = true;
         }
         else [[unlikely]] {
            ctx.error = error_code::syntax_error;
         }
      }
   };

   // Unsigned integers
   template <class T>
      requires(std::unsigned_integral<T> && !std::same_as<T, bool>)
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::uint) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         uint64_t result = cbor_detail::decode_arg(ctx, it, end, additional_info);
         if (bool(ctx.error)) [[unlikely]]
            return;

         value = static_cast<T>(result);
      }
   };

   // Signed integers
   template <class T>
      requires(std::signed_integral<T> && !std::same_as<T, bool>)
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type == major::uint) {
            uint64_t n = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            // Range check: n must fit in T's positive range
            constexpr auto max_val = static_cast<uint64_t>(std::numeric_limits<T>::max());
            if (n > max_val) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }
            value = static_cast<T>(n);
         }
         else if (major_type == major::nint) {
            uint64_t n = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            // CBOR negative value = -1 - n
            // For T's range [-2^(bits-1), 2^(bits-1)-1], max valid n = 2^(bits-1) - 1
            constexpr auto max_n = static_cast<uint64_t>(std::numeric_limits<T>::max());

            if (n > max_n) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }

            // Safe computation using two's complement identity:
            // ~n = -1 - n (bitwise NOT)
            value = static_cast<T>(~n);
         }
         else [[unlikely]] {
            ctx.error = error_code::syntax_error;
         }
      }
   };

   // Floating-point
   template <std::floating_point T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::simple) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         switch (additional_info) {
         case simple::float16: {
            if ((it + 2) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
            uint16_t half;
            std::memcpy(&half, it, 2);
            if constexpr (std::endian::native == std::endian::little) {
               half = std::byteswap(half);
            }
            it += 2;
            value = static_cast<T>(decode_half(half));
            break;
         }
         case simple::float32: {
            if ((it + 4) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
            uint32_t bits;
            std::memcpy(&bits, it, 4);
            if constexpr (std::endian::native == std::endian::little) {
               bits = std::byteswap(bits);
            }
            float f;
            std::memcpy(&f, &bits, 4);
            it += 4;
            value = static_cast<T>(f);
            break;
         }
         case simple::float64: {
            if ((it + 8) > end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
            uint64_t bits;
            std::memcpy(&bits, it, 8);
            if constexpr (std::endian::native == std::endian::little) {
               bits = std::byteswap(bits);
            }
            double d;
            std::memcpy(&d, &bits, 8);
            it += 8;
            value = static_cast<T>(d);
            break;
         }
         default:
            ctx.error = error_code::syntax_error;
         }
      }
   };

   // Text strings (UTF-8)
   template <str_t T>
   struct from<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::tstr) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         if (additional_info == info::indefinite) {
            // Indefinite-length text string
            if constexpr (string_view_t<T>) {
               // Cannot read indefinite string into string_view
               ctx.error = error_code::syntax_error;
               return;
            }
            else {
               value.clear();
               while (true) {
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  uint8_t chunk_initial;
                  std::memcpy(&chunk_initial, it, 1);

                  // Check for break code
                  if (chunk_initial == initial_byte(major::simple, simple::break_code)) {
                     ++it;
                     break;
                  }

                  const uint8_t chunk_major = get_major_type(chunk_initial);
                  const uint8_t chunk_info = get_additional_info(chunk_initial);

                  // Chunks must be text strings with definite length
                  if (chunk_major != major::tstr) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  if (chunk_info == info::indefinite) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  ++it;
                  uint64_t chunk_len = cbor_detail::decode_arg(ctx, it, end, chunk_info);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if (static_cast<uint64_t>(end - it) < chunk_len) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  value.append(reinterpret_cast<const char*>(it), chunk_len);
                  it += chunk_len;
               }
            }
         }
         else {
            // Definite-length text string
            uint64_t length = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (static_cast<uint64_t>(end - it) < length) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check user-configured string length limit
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

            if constexpr (string_view_t<T>) {
               value = {reinterpret_cast<const char*>(it), static_cast<size_t>(length)};
            }
            else {
               value.assign(reinterpret_cast<const char*>(it), length);
            }
            it += length;
         }
      }
   };

   // Byte strings - std::vector<std::byte>
   template <class T>
      requires(std::same_as<typename T::value_type, std::byte> && resizable<T>)
   struct from<CBOR, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::bstr) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         if (additional_info == info::indefinite) {
            // Indefinite-length byte string
            value.clear();
            while (true) {
               if (it >= end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               uint8_t chunk_initial;
               std::memcpy(&chunk_initial, it, 1);

               if (chunk_initial == initial_byte(major::simple, simple::break_code)) {
                  ++it;
                  break;
               }

               const uint8_t chunk_major = get_major_type(chunk_initial);
               const uint8_t chunk_info = get_additional_info(chunk_initial);

               if (chunk_major != major::bstr) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }
               if (chunk_info == info::indefinite) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }

               ++it;
               uint64_t chunk_len = cbor_detail::decode_arg(ctx, it, end, chunk_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               if (static_cast<uint64_t>(end - it) < chunk_len) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               const size_t old_size = value.size();
               value.resize(old_size + static_cast<size_t>(chunk_len));
               std::memcpy(value.data() + old_size, it, chunk_len);
               it += chunk_len;
            }
         }
         else {
            uint64_t length = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (static_cast<uint64_t>(end - it) < length) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check user-configured array size limit
            if constexpr (check_max_array_size(Opts) > 0) {
               if (length > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && length > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            value.resize(static_cast<size_t>(length));
            std::memcpy(value.data(), it, length);
            it += length;
         }
      }
   };

   // Byte strings - std::vector<uint8_t>
   template <>
   struct from<CBOR, std::vector<uint8_t>>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::bstr) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         if (additional_info == info::indefinite) {
            value.clear();
            while (true) {
               if (it >= end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               uint8_t chunk_initial;
               std::memcpy(&chunk_initial, it, 1);

               if (chunk_initial == initial_byte(major::simple, simple::break_code)) {
                  ++it;
                  break;
               }

               const uint8_t chunk_major = get_major_type(chunk_initial);
               const uint8_t chunk_info = get_additional_info(chunk_initial);

               if (chunk_major != major::bstr) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }
               if (chunk_info == info::indefinite) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }

               ++it;
               uint64_t chunk_len = cbor_detail::decode_arg(ctx, it, end, chunk_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               if (static_cast<uint64_t>(end - it) < chunk_len) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               const size_t old_size = value.size();
               value.resize(old_size + static_cast<size_t>(chunk_len));
               std::memcpy(value.data() + old_size, it, chunk_len);
               it += chunk_len;
            }
         }
         else {
            uint64_t length = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (static_cast<uint64_t>(end - it) < length) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check user-configured array size limit
            if constexpr (check_max_array_size(Opts) > 0) {
               if (length > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && length > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            value.resize(static_cast<size_t>(length));
            std::memcpy(value.data(), it, length);
            it += length;
         }
      }
   };

   // Arrays (std::vector, std::deque, etc.)
   // Note: eigen_t types have their own specialization in glaze/ext/eigen.hpp
   template <readable_array_t T>
      requires(!eigen_t<T>)
   struct from<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;
         using V = range_value_t<std::remove_cvref_t<T>>;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         // Check for RFC 8746 typed array (tag + byte string)
         if constexpr (num_t<V> && !std::same_as<V, bool> && contiguous<T>) {
            if (major_type == major::tag) {
               ++it; // consume the tag initial byte

               // Decode the tag number
               const uint64_t tag_num = cbor_detail::decode_arg(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               // Verify it's a valid typed array tag for our element type
               const auto ta_info = typed_array::get_info(tag_num);
               if (ta_info.valid && ta_info.element_size == sizeof(V)) {
                  // Read the byte string
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  uint8_t bstr_initial;
                  std::memcpy(&bstr_initial, it, 1);
                  ++it;

                  if (get_major_type(bstr_initial) != major::bstr) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  const uint64_t byte_len = cbor_detail::decode_arg(ctx, it, end, get_additional_info(bstr_initial));
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if (byte_len % sizeof(V) != 0) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  if (static_cast<uint64_t>(end - it) < byte_len) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  const size_t count = byte_len / sizeof(V);

                  // Check user-configured array size limit
                  if constexpr (check_max_array_size(Opts) > 0) {
                     if (count > check_max_array_size(Opts)) [[unlikely]] {
                        ctx.error = error_code::invalid_length;
                        return;
                     }
                  }
                  if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
                     if (ctx.max_array_size > 0 && count > ctx.max_array_size) [[unlikely]] {
                        ctx.error = error_code::invalid_length;
                        return;
                     }
                  }

                  if constexpr (resizable<T>) {
                     value.resize(count);
                     if constexpr (check_shrink_to_fit(Opts)) {
                        value.shrink_to_fit();
                     }
                  }
                  else {
                     if (count != value.size()) [[unlikely]] {
                        ctx.error = error_code::exceeded_static_array_size;
                        return;
                     }
                  }

                  // Check if we need to byteswap
                  const bool need_swap = typed_array::needs_byteswap(tag_num);

                  if (need_swap && sizeof(V) > 1) {
                     // Need to byteswap each element
                     for (size_t i = 0; i < count; ++i) {
                        V elem;
                        std::memcpy(&elem, it, sizeof(V));
                        if constexpr (sizeof(V) == 2) {
                           uint16_t bits;
                           std::memcpy(&bits, &elem, sizeof(V));
                           bits = std::byteswap(bits);
                           std::memcpy(&elem, &bits, sizeof(V));
                        }
                        else if constexpr (sizeof(V) == 4) {
                           uint32_t bits;
                           std::memcpy(&bits, &elem, sizeof(V));
                           bits = std::byteswap(bits);
                           std::memcpy(&elem, &bits, sizeof(V));
                        }
                        else if constexpr (sizeof(V) == 8) {
                           uint64_t bits;
                           std::memcpy(&bits, &elem, sizeof(V));
                           bits = std::byteswap(bits);
                           std::memcpy(&elem, &bits, sizeof(V));
                        }
                        value[i] = elem;
                        it += sizeof(V);
                     }
                  }
                  else {
                     // Native endianness or single-byte: bulk read
                     if (byte_len > 0) {
                        std::memcpy(value.data(), it, byte_len);
                        it += byte_len;
                     }
                  }
                  return; // Done with typed array
               }
               else {
                  // Not a matching typed array tag - error
                  ctx.error = error_code::syntax_error;
                  return;
               }
            }
         }

         // Check for complex array (tag 43001 with nested typed array)
         if constexpr (complex_t<V> && contiguous<T>) {
            if (major_type == major::tag) {
               ++it; // consume the tag initial byte

               // Decode the tag number
               const uint64_t tag_num = cbor_detail::decode_arg(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               // Check for tag 43001 (complex array)
               if (tag_num == semantic_tag::complex_array) {
                  using Scalar = typename V::value_type;

                  // Read nested typed array tag
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  uint8_t ta_initial;
                  std::memcpy(&ta_initial, it, 1);
                  ++it;

                  if (get_major_type(ta_initial) != major::tag) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  const uint64_t scalar_tag = cbor_detail::decode_arg(ctx, it, end, get_additional_info(ta_initial));
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  // Verify it's a valid typed array tag for the scalar type
                  const auto ta_info = typed_array::get_info(scalar_tag);
                  if (!ta_info.valid || ta_info.element_size != sizeof(Scalar)) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  // Read the byte string
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  uint8_t bstr_initial;
                  std::memcpy(&bstr_initial, it, 1);
                  ++it;

                  if (get_major_type(bstr_initial) != major::bstr) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  const uint64_t byte_len = cbor_detail::decode_arg(ctx, it, end, get_additional_info(bstr_initial));
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  // Each complex has 2 scalars
                  constexpr size_t complex_byte_size = sizeof(V); // sizeof(complex<T>) = 2 * sizeof(T)
                  if (byte_len % complex_byte_size != 0) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  if (static_cast<uint64_t>(end - it) < byte_len) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  const size_t count = byte_len / complex_byte_size;

                  // Check user-configured array size limit
                  if constexpr (check_max_array_size(Opts) > 0) {
                     if (count > check_max_array_size(Opts)) [[unlikely]] {
                        ctx.error = error_code::invalid_length;
                        return;
                     }
                  }
                  if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
                     if (ctx.max_array_size > 0 && count > ctx.max_array_size) [[unlikely]] {
                        ctx.error = error_code::invalid_length;
                        return;
                     }
                  }

                  if constexpr (resizable<T>) {
                     value.resize(count);
                     if constexpr (check_shrink_to_fit(Opts)) {
                        value.shrink_to_fit();
                     }
                  }
                  else {
                     if (count != value.size()) [[unlikely]] {
                        ctx.error = error_code::exceeded_static_array_size;
                        return;
                     }
                  }

                  // Check if we need to byteswap
                  const bool need_swap = typed_array::needs_byteswap(scalar_tag);

                  if (need_swap && sizeof(Scalar) > 1) {
                     // Need to byteswap each scalar in the interleaved data
                     auto* dest = reinterpret_cast<Scalar*>(value.data());
                     const size_t num_scalars = count * 2; // 2 scalars per complex
                     for (size_t i = 0; i < num_scalars; ++i) {
                        Scalar elem;
                        std::memcpy(&elem, it, sizeof(Scalar));
                        if constexpr (sizeof(Scalar) == 2) {
                           uint16_t bits;
                           std::memcpy(&bits, &elem, sizeof(Scalar));
                           bits = std::byteswap(bits);
                           std::memcpy(&elem, &bits, sizeof(Scalar));
                        }
                        else if constexpr (sizeof(Scalar) == 4) {
                           uint32_t bits;
                           std::memcpy(&bits, &elem, sizeof(Scalar));
                           bits = std::byteswap(bits);
                           std::memcpy(&elem, &bits, sizeof(Scalar));
                        }
                        else if constexpr (sizeof(Scalar) == 8) {
                           uint64_t bits;
                           std::memcpy(&bits, &elem, sizeof(Scalar));
                           bits = std::byteswap(bits);
                           std::memcpy(&elem, &bits, sizeof(Scalar));
                        }
                        dest[i] = elem;
                        it += sizeof(Scalar);
                     }
                  }
                  else {
                     // Native endianness or single-byte: bulk read
                     if (byte_len > 0) {
                        std::memcpy(value.data(), it, byte_len);
                        it += byte_len;
                     }
                  }
                  return; // Done with complex array
               }
               else {
                  // Not a complex array tag - error
                  ctx.error = error_code::syntax_error;
                  return;
               }
            }
         }

         // Regular CBOR array (major type 4)
         ++it; // consume the initial byte

         if (major_type != major::array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         if (additional_info == info::indefinite) {
            // Indefinite-length array
            if constexpr (resizable<T>) {
               value.clear();
            }
            size_t i = 0;
            while (true) {
               if (it >= end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               uint8_t peek;
               std::memcpy(&peek, it, 1);

               if (peek == initial_byte(major::simple, simple::break_code)) {
                  ++it;
                  break;
               }

               if constexpr (resizable<T>) {
                  value.emplace_back();
                  parse<CBOR>::op<Opts>(value.back(), ctx, it, end);
               }
               else {
                  if (i >= value.size()) [[unlikely]] {
                     ctx.error = error_code::exceeded_static_array_size;
                     return;
                  }
                  parse<CBOR>::op<Opts>(value[i], ctx, it, end);
                  ++i;
               }

               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
         }
         else {
            // Definite-length array
            uint64_t count = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            // Validate count against remaining buffer size (minimum 1 byte per element)
            if (count > static_cast<uint64_t>(end - it)) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check user-configured array size limit
            if constexpr (check_max_array_size(Opts) > 0) {
               if (count > check_max_array_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_array_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_array_size > 0 && count > ctx.max_array_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            if constexpr (resizable<T>) {
               value.resize(static_cast<size_t>(count));

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }
            else {
               if (count > value.size()) [[unlikely]] {
                  ctx.error = error_code::exceeded_static_array_size;
                  return;
               }
            }

            for (size_t i = 0; i < count; ++i) {
               parse<CBOR>::op<Opts>(value[i], ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
         }
      }
   };

   // Maps (std::map, std::unordered_map, etc.)
   template <readable_map_t T>
   struct from<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;
         using Key = typename T::key_type;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::map) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         value.clear();

         if (additional_info == info::indefinite) {
            // Indefinite-length map
            while (true) {
               if (it >= end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               uint8_t peek;
               std::memcpy(&peek, it, 1);

               if (peek == initial_byte(major::simple, simple::break_code)) {
                  ++it;
                  break;
               }

               Key key{};
               parse<CBOR>::op<Opts>(key, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               parse<CBOR>::op<Opts>(value[std::move(key)], ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
         }
         else {
            uint64_t count = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            // Validate count against remaining buffer size (minimum 2 bytes per key-value pair)
            if (count * 2 > static_cast<uint64_t>(end - it)) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check user-configured map size limit
            if constexpr (check_max_map_size(Opts) > 0) {
               if (count > check_max_map_size(Opts)) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }
            if constexpr (has_runtime_max_map_size<std::decay_t<decltype(ctx)>>) {
               if (ctx.max_map_size > 0 && count > ctx.max_map_size) [[unlikely]] {
                  ctx.error = error_code::invalid_length;
                  return;
               }
            }

            for (size_t i = 0; i < count; ++i) {
               Key key{};
               parse<CBOR>::op<Opts>(key, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               parse<CBOR>::op<Opts>(value[std::move(key)], ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
         }
      }
   };

   // Pairs
   template <pair_t T>
   struct from<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(T& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::map) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         uint64_t count = cbor_detail::decode_arg(ctx, it, end, additional_info);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (count != 1) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         parse<CBOR>::op<Opts>(value.first, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         parse<CBOR>::op<Opts>(value.second, ctx, it, end);
      }
   };

   // Glaze objects (structs with reflection)
   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && !custom_read<T>)
   struct from<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::map) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         static constexpr auto N = reflect<T>::size;

         uint64_t n_keys;
         if (additional_info == info::indefinite) {
            // Handle indefinite map by counting as we go
            n_keys = std::numeric_limits<uint64_t>::max();
         }
         else {
            n_keys = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
         }

         for (uint64_t key_idx = 0; key_idx < n_keys; ++key_idx) {
            if (it >= end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            // Check for break in indefinite map
            if (additional_info == info::indefinite) {
               uint8_t peek;
               std::memcpy(&peek, it, 1);
               if (peek == initial_byte(major::simple, simple::break_code)) {
                  ++it;
                  break;
               }
            }

            // Read key
            uint8_t key_initial;
            std::memcpy(&key_initial, it, 1);
            ++it;

            const uint8_t key_major = get_major_type(key_initial);
            const uint8_t key_info = get_additional_info(key_initial);

            if (key_major != major::tstr) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            uint64_t key_len = cbor_detail::decode_arg(ctx, it, end, key_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (static_cast<uint64_t>(end - it) < key_len) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }

            if constexpr (N > 0) {
               static constexpr auto HashInfo = hash_info<T>;

               const auto index = decode_hash_with_size<CBOR, T, HashInfo, HashInfo.type>::op(it, end, key_len);

               if (index < N) [[likely]] {
                  const sv key{reinterpret_cast<const char*>(it), static_cast<size_t>(key_len)};
                  it += key_len;

                  visit<N>(
                     [&]<size_t I>() {
                        static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                        static constexpr auto Length = TargetKey.size();
                        if ((Length == key_len) && compare<Length>(TargetKey.data(), key.data())) [[likely]] {
                           if constexpr (reflectable<T>) {
                              parse<CBOR>::op<Opts>(get_member(value, get<I>(to_tie(value))), ctx, it, end);
                           }
                           else {
                              parse<CBOR>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, it, end);
                           }
                        }
                        else {
                           if constexpr (Opts.error_on_unknown_keys) {
                              ctx.error = error_code::unknown_key;
                              return;
                           }
                           else {
                              skip_value<CBOR>::op<Opts>(ctx, it, end);
                              if (bool(ctx.error)) [[unlikely]]
                                 return;
                           }
                        }
                     },
                     index);

                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
               else [[unlikely]] {
                  if constexpr (Opts.error_on_unknown_keys) {
                     ctx.error = error_code::unknown_key;
                     return;
                  }
                  else {
                     it += key_len;
                     skip_value<CBOR>::op<Opts>(ctx, it, end);
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
               it += key_len;
               skip_value<CBOR>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
         }
      }
   };

   // Tuples
   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct from<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type != major::array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         using V = std::decay_t<T>;
         static constexpr auto N = glz::tuple_size_v<V>;

         uint64_t count = cbor_detail::decode_arg(ctx, it, end, additional_info);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (count != N) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         if constexpr (is_std_tuple<T>) {
            for_each<N>([&]<size_t I>() { parse<CBOR>::op<Opts>(std::get<I>(value), ctx, it, end); });
         }
         else {
            for_each<N>([&]<size_t I>() { parse<CBOR>::op<Opts>(glz::get<I>(value), ctx, it, end); });
         }
      }
   };

   // Glaze arrays
   template <class T>
      requires glaze_array_t<T>
   struct from<CBOR, T> final
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         if (initial != (major::array << 5 | reflect<T>::size)) [[unlikely]] {
            // Allow longer forms too
            const uint8_t major_type = get_major_type(initial);
            const uint8_t additional_info = get_additional_info(initial);

            if (major_type != major::array) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }

            uint64_t count = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            if (count != reflect<T>::size) [[unlikely]] {
               ctx.error = error_code::syntax_error;
               return;
            }
         }

         for_each<reflect<T>::size>(
            [&]<size_t I>() { parse<CBOR>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, it, end); });
      }
   };

   // Nullable types
   template <nullable_t T>
      requires(!std::is_array_v<T>)
   struct from<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t peek;
         std::memcpy(&peek, it, 1);

         if (peek == initial_byte(major::simple, simple::null_value)) {
            ++it;
            if constexpr (is_specialization_v<T, std::optional>) {
               value = std::nullopt;
            }
            else if constexpr (is_specialization_v<T, std::unique_ptr>) {
               value = nullptr;
            }
            else if constexpr (is_specialization_v<T, std::shared_ptr>) {
               value = nullptr;
            }
         }
         else {
            if (!value) {
               if constexpr (is_specialization_v<T, std::optional>) {
                  value = std::make_optional<typename T::value_type>();
               }
               else if constexpr (is_specialization_v<T, std::unique_ptr>) {
                  value = std::make_unique<typename T::element_type>();
               }
               else if constexpr (is_specialization_v<T, std::shared_ptr>) {
                  value = std::make_shared<typename T::element_type>();
               }
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
               }
            }
            parse<CBOR>::op<Opts>(*value, ctx, it, end);
         }
      }
   };

   // C-style arrays
   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct from<CBOR, T> final
   {
      template <auto Opts, class V, size_t N>
      GLZ_ALWAYS_INLINE static void op(V (&value)[N], is_context auto& ctx, auto& it, auto end) noexcept
      {
         parse<CBOR>::op<Opts>(std::span{value, N}, ctx, it, end);
      }
   };

   // Variants
   template <is_variant T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         // Expect array of [index, value]
         if (major_type != major::array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         uint64_t count = cbor_detail::decode_arg(ctx, it, end, additional_info);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (count != 2) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Read index
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t idx_initial;
         std::memcpy(&idx_initial, it, 1);
         ++it;

         const uint8_t idx_major = get_major_type(idx_initial);
         const uint8_t idx_info = get_additional_info(idx_initial);

         if (idx_major != major::uint) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         uint64_t type_index = cbor_detail::decode_arg(ctx, it, end, idx_info);
         if (bool(ctx.error)) [[unlikely]]
            return;

         if (value.index() != type_index) {
            emplace_runtime_variant(value, type_index);
         }

         std::visit([&](auto& v) { parse<CBOR>::op<Opts>(v, ctx, it, end); }, value);
      }
   };

   // Glaze value wrapper
   template <class T>
      requires(glaze_value_t<T> && !custom_read<T>)
   struct from<CBOR, T>
   {
      template <auto Opts, class Value, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(Value&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = std::decay_t<decltype(get_member(std::declval<Value>(), meta_wrapper_v<T>))>;
         from<CBOR, V>::template op<Opts>(get_member(std::forward<Value>(value), meta_wrapper_v<T>),
                                          std::forward<Ctx>(ctx), std::forward<It0>(it), end);
      }
   };

   // Enums with glaze reflection
   template <glaze_enum_t T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type == major::uint) {
            uint64_t n = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value = static_cast<std::decay_t<T>>(n);
         }
         else if (major_type == major::nint) {
            uint64_t n = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value = static_cast<std::decay_t<T>>(~n);
         }
         else [[unlikely]] {
            ctx.error = error_code::syntax_error;
         }
      }
   };

   // Plain enums
   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T>)
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end) noexcept
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t initial;
         std::memcpy(&initial, it, 1);
         ++it;

         const uint8_t major_type = get_major_type(initial);
         const uint8_t additional_info = get_additional_info(initial);

         if (major_type == major::uint) {
            uint64_t n = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value = static_cast<std::decay_t<T>>(n);
         }
         else if (major_type == major::nint) {
            uint64_t n = cbor_detail::decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value = static_cast<std::decay_t<T>>(~n);
         }
         else [[unlikely]] {
            ctx.error = error_code::syntax_error;
         }
      }
   };

   // Member function pointers (no-op)
   template <is_member_function_pointer T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&&, auto&&, auto&&) noexcept
      {}
   };

   // Hidden type
   template <>
   struct from<CBOR, hidden>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, auto&&...) noexcept
      {
         ctx.error = error_code::attempt_read_hidden;
      }
   };

   // Nullable value types
   template <class T>
      requires(nullable_value_t<T> && !nullable_like<T> && !is_expected<T> && !custom_read<T>)
   struct from<CBOR, T> final
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto& value, is_context auto& ctx, auto& it, auto end)
      {
         using namespace cbor;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         uint8_t peek;
         std::memcpy(&peek, it, 1);

         if (peek == initial_byte(major::simple, simple::null_value)) {
            ++it;
            if constexpr (requires { value.reset(); }) {
               value.reset();
            }
         }
         else {
            if (!value.has_value()) {
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
            parse<CBOR>::op<Opts>(value.value(), ctx, it, end);
         }
      }
   };

   // Filesystem paths
   template <filesystem_path T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto& ctx, auto&... args)
      {
         static thread_local std::string buffer{};
         parse<CBOR>::op<Opts>(buffer, ctx, args...);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }
         value = buffer;
      }
   };

   // ===== High-level read APIs =====

   template <read_supported<CBOR> T, class Buffer>
   [[nodiscard]] inline error_ctx read_cbor(T&& value, Buffer&& buffer)
   {
      return read<opts{.format = CBOR}>(value, std::forward<Buffer>(buffer));
   }

   template <read_supported<CBOR> T, class Buffer>
   [[nodiscard]] inline expected<T, error_ctx> read_cbor(Buffer&& buffer)
   {
      T value{};
      const auto pe = read<opts{.format = CBOR}>(value, std::forward<Buffer>(buffer));
      if (pe) [[unlikely]] {
         return unexpected(pe);
      }
      return value;
   }

   template <auto Opts = opts{}, read_supported<CBOR> T>
   [[nodiscard]] inline error_ctx read_file_cbor(T& value, const sv file_name, auto&& buffer)
   {
      context ctx{};
      ctx.current_file = file_name;

      const auto file_error = file_to_buffer(buffer, ctx.current_file);

      if (bool(file_error)) [[unlikely]] {
         return error_ctx{file_error};
      }

      return read<set_cbor<Opts>()>(value, buffer, ctx);
   }
}
