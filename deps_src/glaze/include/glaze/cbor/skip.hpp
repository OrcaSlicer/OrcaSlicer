// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/cbor/header.hpp"
#include "glaze/core/context.hpp"
#include "glaze/core/opts.hpp"

namespace glz
{
   template <>
   struct skip_value<CBOR>
   {
      // Skip argument bytes and return the argument value
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static uint64_t skip_argument(is_context auto& ctx, auto& it, auto end,
                                                                    uint8_t additional_info) noexcept
      {
         using namespace cbor;

         if (additional_info < 24) {
            return additional_info; // Inline value
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
         case info::indefinite:
            return 0; // Caller handles indefinite specially
         default:
            ctx.error = error_code::syntax_error; // Reserved (28-30)
            return 0;
         }
      }

      // Main entry point: skip one complete CBOR data item
      template <auto Opts>
      static void op(is_context auto& ctx, auto& it, auto end) noexcept
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

         switch (major_type) {
         case major::uint:
         case major::nint: {
            // Integer: just skip the argument bytes
            (void)skip_argument<Opts>(ctx, it, end, additional_info);
            break;
         }

         case major::bstr:
         case major::tstr: {
            // String: skip argument, then skip content bytes
            if (additional_info == info::indefinite) {
               // Indefinite-length: skip chunks until break
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

                  ++it;
                  const uint8_t chunk_major = get_major_type(chunk_initial);
                  const uint8_t chunk_info = get_additional_info(chunk_initial);

                  // Chunks must be same major type and definite length
                  if (chunk_major != major_type) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  if (chunk_info == info::indefinite) [[unlikely]] {
                     ctx.error = error_code::syntax_error; // Nested indefinite
                     return;
                  }

                  uint64_t chunk_len = skip_argument<Opts>(ctx, it, end, chunk_info);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if (static_cast<uint64_t>(end - it) < chunk_len) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  it += chunk_len;
               }
            }
            else {
               uint64_t length = skip_argument<Opts>(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               if (static_cast<uint64_t>(end - it) < length) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               it += length;
            }
            break;
         }

         case major::array: {
            if (additional_info == info::indefinite) {
               // Skip items until break
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
                  op<Opts>(ctx, it, end); // Recursive skip
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }
            else {
               uint64_t count = skip_argument<Opts>(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               for (uint64_t i = 0; i < count; ++i) {
                  op<Opts>(ctx, it, end);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }
            break;
         }

         case major::map: {
            if (additional_info == info::indefinite) {
               // Skip key-value pairs until break
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
                  op<Opts>(ctx, it, end); // Skip key
                  if (bool(ctx.error)) [[unlikely]]
                     return;
                  op<Opts>(ctx, it, end); // Skip value
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }
            else {
               uint64_t count = skip_argument<Opts>(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               for (uint64_t i = 0; i < count; ++i) {
                  op<Opts>(ctx, it, end); // Skip key
                  if (bool(ctx.error)) [[unlikely]]
                     return;
                  op<Opts>(ctx, it, end); // Skip value
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }
            break;
         }

         case major::tag: {
            // Skip tag number argument
            (void)skip_argument<Opts>(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            // Skip tagged content
            op<Opts>(ctx, it, end);
            break;
         }

         case major::simple: {
            switch (additional_info) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case simple::false_value:
            case simple::true_value:
            case simple::null_value:
            case simple::undefined:
               break; // Value in additional_info
            case 24:
               if (it >= end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               ++it; // Simple value in next byte
               break;
            case simple::float16:
               if ((it + 2) > end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               it += 2;
               break;
            case simple::float32:
               if ((it + 4) > end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               it += 4;
               break;
            case simple::float64:
               if ((it + 8) > end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
               it += 8;
               break;
            case simple::break_code:
               ctx.error = error_code::syntax_error; // Unexpected break
               break;
            default:
               ctx.error = error_code::syntax_error; // Reserved (28-30)
               break;
            }
            break;
         }

         default:
            ctx.error = error_code::syntax_error;
            break;
         }
      }
   };
}
