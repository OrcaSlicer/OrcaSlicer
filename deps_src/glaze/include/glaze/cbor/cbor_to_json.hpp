// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/cbor/header.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   namespace detail
   {
      // Decode CBOR argument (variable-length unsigned integer)
      inline uint64_t cbor_to_json_decode_arg(is_context auto& ctx, auto& it, auto end,
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

      template <auto Opts, class Buffer>
      inline void cbor_to_json_value(auto&& ctx, auto&& it, auto&& end, Buffer& out, auto&& ix,
                                     uint32_t recursive_depth)
      {
         using namespace cbor;

         // Check recursion depth limit
         if (recursive_depth >= max_recursive_depth_limit) [[unlikely]] {
            ctx.error = error_code::exceeded_max_recursive_depth;
            return;
         }

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
         case major::uint: {
            // Unsigned integer
            const uint64_t value = cbor_to_json_decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            to<JSON, uint64_t>::template op<Opts>(value, ctx, out, ix);
            break;
         }

         case major::nint: {
            // Negative integer: -1 - n
            const uint64_t n = cbor_to_json_decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;
            // Use two's complement trick for safe conversion
            const int64_t value = static_cast<int64_t>(~n);
            to<JSON, int64_t>::template op<Opts>(value, ctx, out, ix);
            break;
         }

         case major::bstr: {
            // Byte string - encode as base64 in JSON
            if (additional_info == info::indefinite) {
               // Indefinite-length byte string - collect chunks first
               std::vector<uint8_t> bytes;
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

                  if (chunk_major != major::bstr || chunk_info == info::indefinite) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  const uint64_t chunk_len = cbor_to_json_decode_arg(ctx, it, end, chunk_info);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if (static_cast<uint64_t>(end - it) < chunk_len) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  if (chunk_len > 0) {
                     const size_t old_size = bytes.size();
                     bytes.resize(old_size + chunk_len);
                     std::memcpy(bytes.data() + old_size, it, chunk_len);
                     it += chunk_len;
                  }
               }
               // Write as base64-encoded string
               dump('"', out, ix);
               // Simple hex encoding for now (TODO: proper base64)
               for (uint8_t b : bytes) {
                  static constexpr char hex[] = "0123456789abcdef";
                  dump(hex[(b >> 4) & 0xf], out, ix);
                  dump(hex[b & 0xf], out, ix);
               }
               dump('"', out, ix);
            }
            else {
               const uint64_t length = cbor_to_json_decode_arg(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               if (static_cast<uint64_t>(end - it) < length) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               // Write as hex-encoded string
               dump('"', out, ix);
               for (uint64_t i = 0; i < length; ++i) {
                  static constexpr char hex[] = "0123456789abcdef";
                  uint8_t b;
                  std::memcpy(&b, it + i, 1);
                  dump(hex[(b >> 4) & 0xf], out, ix);
                  dump(hex[b & 0xf], out, ix);
               }
               dump('"', out, ix);
               it += length;
            }
            break;
         }

         case major::tstr: {
            // Text string
            if (additional_info == info::indefinite) {
               // Indefinite-length text string - concatenate chunks
               std::string str;
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

                  if (chunk_major != major::tstr || chunk_info == info::indefinite) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }

                  const uint64_t chunk_len = cbor_to_json_decode_arg(ctx, it, end, chunk_info);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if (static_cast<uint64_t>(end - it) < chunk_len) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }

                  str.append(reinterpret_cast<const char*>(it), chunk_len);
                  it += chunk_len;
               }
               to<JSON, std::string_view>::template op<Opts>(str, ctx, out, ix);
            }
            else {
               const uint64_t length = cbor_to_json_decode_arg(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               if (static_cast<uint64_t>(end - it) < length) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               const sv value{reinterpret_cast<const char*>(it), static_cast<size_t>(length)};
               to<JSON, sv>::template op<Opts>(value, ctx, out, ix);
               it += length;
            }
            break;
         }

         case major::array: {
            dump('[', out, ix);

            if (additional_info == info::indefinite) {
               // Indefinite-length array
               bool first = true;
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

                  if (!first) {
                     dump(',', out, ix);
                     if constexpr (Opts.prettify) {
                        dump(' ', out, ix);
                     }
                  }
                  first = false;

                  cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }
            else {
               const uint64_t count = cbor_to_json_decode_arg(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               for (uint64_t i = 0; i < count; ++i) {
                  if (i > 0) {
                     dump(',', out, ix);
                     if constexpr (Opts.prettify) {
                        dump(' ', out, ix);
                     }
                  }
                  cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }

            dump(']', out, ix);
            break;
         }

         case major::map: {
            dump('{', out, ix);
            if constexpr (Opts.prettify) {
               ctx.depth += check_indentation_width(Opts);
            }

            if (additional_info == info::indefinite) {
               // Indefinite-length map
               bool first = true;
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

                  if (!first) {
                     dump(',', out, ix);
                  }
                  if constexpr (Opts.prettify) {
                     dump('\n', out, ix);
                     dumpn(check_indentation_char(Opts), ctx.depth, out, ix);
                  }
                  first = false;

                  // Key (must be string for JSON compatibility)
                  cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if constexpr (Opts.prettify) {
                     dump(": ", out, ix);
                  }
                  else {
                     dump(':', out, ix);
                  }

                  // Value
                  cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }
            else {
               const uint64_t count = cbor_to_json_decode_arg(ctx, it, end, additional_info);
               if (bool(ctx.error)) [[unlikely]]
                  return;

               for (uint64_t i = 0; i < count; ++i) {
                  if (i > 0) {
                     dump(',', out, ix);
                  }
                  if constexpr (Opts.prettify) {
                     dump('\n', out, ix);
                     dumpn(check_indentation_char(Opts), ctx.depth, out, ix);
                  }

                  // Key
                  cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if constexpr (Opts.prettify) {
                     dump(": ", out, ix);
                  }
                  else {
                     dump(':', out, ix);
                  }

                  // Value
                  cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
               }
            }

            if constexpr (Opts.prettify) {
               ctx.depth -= check_indentation_width(Opts);
               if (additional_info != 0 || additional_info == info::indefinite) {
                  dump('\n', out, ix);
                  dumpn(check_indentation_char(Opts), ctx.depth, out, ix);
               }
            }
            dump('}', out, ix);
            break;
         }

         case major::tag: {
            // Semantic tag - for JSON output, we skip the tag and output the content
            // Some tags could have special handling (e.g., datetime)
            const uint64_t tag_num = cbor_to_json_decode_arg(ctx, it, end, additional_info);
            if (bool(ctx.error)) [[unlikely]]
               return;

            // Check for typed arrays (RFC 8746)
            const auto ta_info = typed_array::get_info(tag_num);
            if (ta_info.valid) {
               // It's a typed array - read the byte string and convert elements
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

               const uint64_t byte_len = cbor_to_json_decode_arg(ctx, it, end, get_additional_info(bstr_initial));
               if (bool(ctx.error)) [[unlikely]]
                  return;

               if (static_cast<uint64_t>(end - it) < byte_len) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               if (byte_len % ta_info.element_size != 0) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }

               const size_t count = byte_len / ta_info.element_size;
               const bool need_swap = typed_array::needs_byteswap(tag_num);

               dump('[', out, ix);

               for (size_t i = 0; i < count; ++i) {
                  if (i > 0) {
                     dump(',', out, ix);
                  }

                  // Read and optionally byteswap the element
                  if (ta_info.is_float) {
                     if (ta_info.element_size == 4) {
                        float val;
                        std::memcpy(&val, it, 4);
                        if (need_swap) {
                           uint32_t bits;
                           std::memcpy(&bits, &val, 4);
                           bits = std::byteswap(bits);
                           std::memcpy(&val, &bits, 4);
                        }
                        to<JSON, float>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 8) {
                        double val;
                        std::memcpy(&val, it, 8);
                        if (need_swap) {
                           uint64_t bits;
                           std::memcpy(&bits, &val, 8);
                           bits = std::byteswap(bits);
                           std::memcpy(&val, &bits, 8);
                        }
                        to<JSON, double>::template op<Opts>(val, ctx, out, ix);
                     }
                     else {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                  }
                  else if (ta_info.is_signed) {
                     if (ta_info.element_size == 1) {
                        int8_t val;
                        std::memcpy(&val, it, 1);
                        to<JSON, int8_t>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 2) {
                        int16_t val;
                        std::memcpy(&val, it, 2);
                        if (need_swap) {
                           uint16_t bits;
                           std::memcpy(&bits, &val, 2);
                           bits = std::byteswap(bits);
                           std::memcpy(&val, &bits, 2);
                        }
                        to<JSON, int16_t>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 4) {
                        int32_t val;
                        std::memcpy(&val, it, 4);
                        if (need_swap) {
                           uint32_t bits;
                           std::memcpy(&bits, &val, 4);
                           bits = std::byteswap(bits);
                           std::memcpy(&val, &bits, 4);
                        }
                        to<JSON, int32_t>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 8) {
                        int64_t val;
                        std::memcpy(&val, it, 8);
                        if (need_swap) {
                           uint64_t bits;
                           std::memcpy(&bits, &val, 8);
                           bits = std::byteswap(bits);
                           std::memcpy(&val, &bits, 8);
                        }
                        to<JSON, int64_t>::template op<Opts>(val, ctx, out, ix);
                     }
                  }
                  else {
                     // Unsigned
                     if (ta_info.element_size == 1) {
                        uint8_t val;
                        std::memcpy(&val, it, 1);
                        to<JSON, uint8_t>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 2) {
                        uint16_t val;
                        std::memcpy(&val, it, 2);
                        if (need_swap) {
                           val = std::byteswap(val);
                        }
                        to<JSON, uint16_t>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 4) {
                        uint32_t val;
                        std::memcpy(&val, it, 4);
                        if (need_swap) {
                           val = std::byteswap(val);
                        }
                        to<JSON, uint32_t>::template op<Opts>(val, ctx, out, ix);
                     }
                     else if (ta_info.element_size == 8) {
                        uint64_t val;
                        std::memcpy(&val, it, 8);
                        if (need_swap) {
                           val = std::byteswap(val);
                        }
                        to<JSON, uint64_t>::template op<Opts>(val, ctx, out, ix);
                     }
                  }

                  it += ta_info.element_size;
               }

               dump(']', out, ix);
            }
            else {
               // Other tags - just output the tagged content
               cbor_to_json_value<Opts>(ctx, it, end, out, ix, recursive_depth + 1);
            }
            break;
         }

         case major::simple: {
            switch (additional_info) {
            case simple::false_value:
               dump("false", out, ix);
               break;
            case simple::true_value:
               dump("true", out, ix);
               break;
            case simple::null_value:
            case simple::undefined:
               dump("null", out, ix);
               break;
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
               const double value = decode_half(half);
               to<JSON, double>::template op<Opts>(value, ctx, out, ix);
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
               float value;
               std::memcpy(&value, &bits, 4);
               it += 4;
               to<JSON, float>::template op<Opts>(value, ctx, out, ix);
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
               double value;
               std::memcpy(&value, &bits, 8);
               it += 8;
               to<JSON, double>::template op<Opts>(value, ctx, out, ix);
               break;
            }
            case simple::break_code:
               ctx.error = error_code::syntax_error; // Unexpected break
               break;
            default:
               if (additional_info < 24) {
                  // Simple value 0-23 - output as number
                  to<JSON, uint8_t>::template op<Opts>(additional_info, ctx, out, ix);
               }
               else if (additional_info == 24) {
                  // Simple value in next byte
                  if (it >= end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
                  uint8_t val;
                  std::memcpy(&val, it, 1);
                  ++it;
                  to<JSON, uint8_t>::template op<Opts>(val, ctx, out, ix);
               }
               else {
                  ctx.error = error_code::syntax_error;
               }
               break;
            }
            break;
         }

         default:
            ctx.error = error_code::syntax_error;
            break;
         }
      }
   }

   // Convert CBOR buffer directly to JSON without intermediate C++ types
   template <auto Opts = glz::opts{}, class CBORBuffer, class JSONBuffer>
   [[nodiscard]] inline error_ctx cbor_to_json(const CBORBuffer& cbor, JSONBuffer& out)
   {
      size_t ix{}; // write index

      auto* it = cbor.data();
      auto* end = it + cbor.size();

      context ctx{};

      while (it < end) {
         detail::cbor_to_json_value<Opts>(ctx, it, end, out, ix, 0);
         if (bool(ctx.error)) {
            return {0, ctx.error};
         }
      }

      if constexpr (resizable<JSONBuffer>) {
         out.resize(ix);
      }

      return {};
   }

   // Convenience function returning string
   template <auto Opts = glz::opts{}, class CBORBuffer>
   [[nodiscard]] inline expected<std::string, error_ctx> cbor_to_json(const CBORBuffer& cbor)
   {
      std::string out;
      auto ec = cbor_to_json<Opts>(cbor, out);
      if (ec) {
         return unexpected(ec);
      }
      return out;
   }
}
