// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/opts.hpp"
#include "glaze/msgpack/common.hpp"

namespace glz
{
   template <>
   struct skip_value<MSGPACK>
   {
      template <auto Opts, class It, class End>
      static void op(is_context auto& ctx, It& it, const End& end) noexcept
      {
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         const uint8_t tag = static_cast<uint8_t>(*it++);
         skip_with_tag<Opts>(ctx, tag, it, end);
      }

     private:
      template <auto Opts, class It, class End>
      static void skip_with_tag(is_context auto& ctx, const uint8_t tag, It& it, const End& end) noexcept
      {
         if (msgpack::is_positive_fixint(tag) || msgpack::is_negative_fixint(tag)) {
            return;
         }

         switch (tag) {
         case msgpack::nil:
         case msgpack::bool_false:
         case msgpack::bool_true:
            return;

         case msgpack::uint8:
         case msgpack::int8:
            msgpack::skip_bytes(ctx, it, end, 1);
            return;
         case msgpack::uint16:
         case msgpack::int16:
            msgpack::skip_bytes(ctx, it, end, 2);
            return;
         case msgpack::uint32:
         case msgpack::int32:
            msgpack::skip_bytes(ctx, it, end, 4);
            return;
         case msgpack::uint64:
         case msgpack::int64:
         case msgpack::float64:
            msgpack::skip_bytes(ctx, it, end, 8);
            return;
         case msgpack::float32:
            msgpack::skip_bytes(ctx, it, end, 4);
            return;

         case msgpack::str8:
         case msgpack::str16:
         case msgpack::str32:
         default:
            break;
         }

         if (msgpack::is_fixstr(tag) || tag == msgpack::str8 || tag == msgpack::str16 || tag == msgpack::str32) {
            size_t len{};
            if (!msgpack::read_str_length(ctx, tag, it, end, len)) {
               return;
            }
            msgpack::skip_bytes(ctx, it, end, len);
            return;
         }

         if (tag == msgpack::bin8 || tag == msgpack::bin16 || tag == msgpack::bin32) {
            size_t len{};
            if (!msgpack::read_bin_length(ctx, tag, it, end, len)) {
               return;
            }
            msgpack::skip_bytes(ctx, it, end, len);
            return;
         }

         if (msgpack::is_fixarray(tag) || tag == msgpack::array16 || tag == msgpack::array32) {
            size_t len{};
            if (!msgpack::read_array_length(ctx, tag, it, end, len)) {
               return;
            }
            for (size_t i = 0; i < len && ctx.error == error_code::none; ++i) {
               op<Opts>(ctx, it, end);
            }
            return;
         }

         if (msgpack::is_fixmap(tag) || tag == msgpack::map16 || tag == msgpack::map32) {
            size_t len{};
            if (!msgpack::read_map_length(ctx, tag, it, end, len)) {
               return;
            }
            for (size_t i = 0; i < len && ctx.error == error_code::none; ++i) {
               op<Opts>(ctx, it, end); // key
               if (ctx.error != error_code::none) {
                  return;
               }
               op<Opts>(ctx, it, end); // value
            }
            return;
         }

         if (tag == msgpack::fixext1) {
            msgpack::skip_bytes(ctx, it, end, 1 + 1);
            return;
         }
         if (tag == msgpack::fixext2) {
            msgpack::skip_bytes(ctx, it, end, 1 + 2);
            return;
         }
         if (tag == msgpack::fixext4) {
            msgpack::skip_bytes(ctx, it, end, 1 + 4);
            return;
         }
         if (tag == msgpack::fixext8) {
            msgpack::skip_bytes(ctx, it, end, 1 + 8);
            return;
         }
         if (tag == msgpack::fixext16) {
            msgpack::skip_bytes(ctx, it, end, 1 + 16);
            return;
         }
         if (tag == msgpack::ext8 || tag == msgpack::ext16 || tag == msgpack::ext32) {
            size_t len{};
            int8_t type{};
            if (!msgpack::read_ext_header(ctx, tag, it, end, len, type)) {
               return;
            }
            msgpack::skip_bytes(ctx, it, end, len);
            return;
         }

         ctx.error = error_code::syntax_error;
      }
   };
}
