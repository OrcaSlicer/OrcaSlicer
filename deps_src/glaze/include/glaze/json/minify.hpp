// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// Minified JSONC only works with /**/ style comments, so we only supports this

#include "glaze/json/json_format.hpp"

namespace glz
{
   namespace detail
   {
      // We can use unchecked dumping to the output because we know minifying will not make the output any larger
      template <auto Opts>
      inline void minify_json(is_context auto&& ctx, auto&& it, auto&& end, auto&& b, auto& ix) noexcept
      {
         using enum json_type;

         auto ws_start = it;
         uint64_t ws_size{};

         auto skip_expected_whitespace = [&] {
            auto new_ws_start = it;
            if (ws_size && ws_size < size_t(end - it)) [[likely]] {
               skip_matching_ws(ws_start, it, ws_size);
            }

            if constexpr (Opts.null_terminated) {
               while (whitespace_table[uint8_t(*it)]) {
                  ++it;
               }
            }
            else {
               while ((it < end) && whitespace_table[uint8_t(*it)]) {
                  ++it;
               }
            }
            ws_start = new_ws_start;
            ws_size = size_t(it - new_ws_start);
         };

         auto skip_whitespace = [&] {
            if constexpr (Opts.null_terminated) {
               while (whitespace_table[uint8_t(*it)]) {
                  ++it;
               }
            }
            else {
               while ((it < end) && whitespace_table[uint8_t(*it)]) {
                  ++it;
               }
            }
         };

         skip_whitespace();

         while ([&]() -> bool {
            if constexpr (Opts.null_terminated) {
               return true;
            }
            else {
               return it < end;
            }
         }()) {
            switch (json_types[uint8_t(*it)]) {
            case String: {
               const auto value = read_json_string<Opts>(it, end);
               dump_maybe_empty<false>(value, b, ix);
               skip_whitespace();
               break;
            }
            case Comma: {
               dump<false>(',', b, ix);
               ++it;
               skip_expected_whitespace();
               break;
            }
            case Number: {
               const auto value = read_json_number<Opts.null_terminated>(it, end);
               dump<false>(value, b, ix); // we couldn't have gotten here without one valid character
               skip_whitespace();
               break;
            }
            case Colon: {
               dump<false>(':', b, ix);
               ++it;
               skip_whitespace();
               break;
            }
            case Array_Start: {
               dump<false>('[', b, ix);
               ++it;
               skip_expected_whitespace();
               break;
            }
            case Array_End: {
               dump<false>(']', b, ix);
               ++it;
               skip_whitespace();
               break;
            }
            case Null: {
               dump<false>("null", b, ix);
               it += 4;
               skip_whitespace();
               break;
            }
            case Bool: {
               if (*it == 't') {
                  dump<false>("true", b, ix);
                  it += 4;
                  skip_whitespace();
                  break;
               }
               else {
                  dump<false>("false", b, ix);
                  it += 5;
                  skip_whitespace();
                  break;
               }
            }
            case Object_Start: {
               dump<false>('{', b, ix);
               ++it;
               skip_expected_whitespace();
               break;
            }
            case Object_End: {
               dump<false>('}', b, ix);
               ++it;
               skip_whitespace();
               break;
            }
            case Comment: {
               if constexpr (Opts.comments) {
                  const auto value = read_jsonc_comment(it, end);
                  if (value.size()) [[likely]] {
                     dump<false>(value, b, ix);
                  }
                  skip_whitespace();
                  break;
               }
               else {
                  [[fallthrough]];
               }
            }
            [[unlikely]] default: {
               ctx.error = error_code::syntax_error;
               return;
            }
            }
         }
      }

      template <auto Opts, class In, output_buffer Out>
         requires(contiguous<In> && resizable<In>)
      inline void minify_json(is_context auto&& ctx, In&& in, Out&& out)
      {
         if (in.empty()) {
            return;
         }
         in.resize(in.size() + padding_bytes);

         if constexpr (resizable<Out>) {
            out.resize(in.size() + padding_bytes);
         }
         size_t ix = 0;
         auto [it, end] = read_iterators<Opts, true>(in);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         static constexpr auto O = is_padded_on<Opts>();
         if constexpr (string_t<In>) {
            minify_json<opt_true<O, &opts::null_terminated>>(ctx, it, end, out, ix);
         }
         else {
            minify_json<opt_false<O, &opts::null_terminated>>(ctx, it, end, out, ix);
         }

         if constexpr (resizable<Out>) {
            out.resize(ix);
         }
         in.resize(in.size() - padding_bytes);
      }
   }

   // We don't return errors from minifying even though they are handled because the error case
   // should not happen since we minify auto-generated JSON.
   // The detail version can be used if error context is needed

   template <auto Opts = opts{}>
   inline void minify_json(resizable auto& in, auto& out)
   {
      context ctx{};
      detail::minify_json<Opts>(ctx, in, out);
   }

   template <auto Opts = opts{}>
   inline std::string minify_json(resizable auto& in)
   {
      context ctx{};
      std::string out{};
      detail::minify_json<Opts>(ctx, in, out);
      return out;
   }

   template <auto Opts = opts{}>
   inline void minify_jsonc(const auto& in, auto& out)
   {
      context ctx{};
      detail::minify_json<opt_true<Opts, &opts::comments>>(ctx, in, out);
   }

   template <auto Opts = opts{}>
   inline std::string minify_jsonc(resizable auto& in)
   {
      context ctx{};
      std::string out{};
      detail::minify_json<opt_true<Opts, &opts::comments>>(ctx, in, out);
      return out;
   }
}
