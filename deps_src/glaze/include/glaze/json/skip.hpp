// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/util/parse.hpp"

namespace glz
{
   template <>
   struct skip_value<JSON>
   {
      template <auto Opts>
         requires(not Opts.comments)
      GLZ_ALWAYS_INLINE static void op(is_context auto&& ctx, auto&& it, auto end) noexcept;

      template <auto Opts>
         requires(bool(Opts.comments))
      GLZ_ALWAYS_INLINE static void op(is_context auto&& ctx, auto&& it, auto end) noexcept;
   };

   template <auto Opts>
   void skip_object(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      if constexpr (!check_validate_skipped(Opts)) {
         ++it;
         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }
         skip_until_closed<Opts, '{', '}'>(ctx, it, end);
      }
      else {
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }
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
         while (true) {
            if (*it != '"') {
               ctx.error = error_code::syntax_error;
               return;
            }
            skip_string<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (match_invalid_end<':', Opts>(ctx, it, end)) {
               return;
            }
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            skip_value<JSON>::op<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (*it != ',') break;
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
   }

   template <auto Opts>
      requires(Opts.format == JSON || Opts.format == NDJSON)
   void skip_array(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      if constexpr (!check_validate_skipped(Opts)) {
         ++it;
         if constexpr (not Opts.null_terminated) {
            if (it == end) [[unlikely]] {
               ctx.error = error_code::unexpected_end;
               return;
            }
         }
         skip_until_closed<Opts, '[', ']'>(ctx, it, end);
      }
      else {
         if constexpr (not Opts.null_terminated) {
            ++ctx.depth;
         }
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
         while (true) {
            skip_value<JSON>::op<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (*it != ',') break;
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
         }
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

   // parse_value is used for JSON pointer reading
   // we want the JSON pointer access to not care about trailing whitespace
   // so we use validate_skipped for precise validation and value skipping
   // expects opening whitespace to be handled
   template <auto Opts>
   GLZ_ALWAYS_INLINE auto parse_value(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      auto start = it;
      struct opts_validate_skipped : std::decay_t<decltype(Opts)>
      {
         bool validate_skipped = true;
      };
      skip_value<JSON>::op<opts_validate_skipped{{Opts}}>(ctx, it, end);
      return std::span{start, size_t(it - start)};
   }

   template <auto Opts>
      requires(not Opts.comments)
   GLZ_ALWAYS_INLINE void skip_value<JSON>::op(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      using namespace glz::detail;

      if constexpr (not check_validate_skipped(Opts)) {
         if constexpr (not check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }
         while (true) {
            switch (*it) {
            case '{':
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               skip_until_closed<Opts, '{', '}'>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               break;
            case '[':
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               skip_until_closed<Opts, '[', ']'>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               break;
            case '"':
               skip_string<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
               break;
            case ',':
            case '}':
            case ']':
               break;
            case '\0':
               ctx.error = error_code::unexpected_end;
               return;
            default: {
               ++it;
               if constexpr (not Opts.null_terminated) {
                  if (it == end) [[unlikely]] {
                     ctx.error = error_code::unexpected_end;
                     return;
                  }
               }
               continue;
            }
            }

            break;
         }
      }
      else {
         if constexpr (not check_ws_handled(Opts)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
         }
         switch (*it) {
         case '{': {
            skip_object<Opts>(ctx, it, end);
            break;
         }
         case '[': {
            skip_array<Opts>(ctx, it, end);
            break;
         }
         case '"': {
            skip_string<Opts>(ctx, it, end);
            break;
         }
         case 'n': {
            ++it;
            match<"ull", Opts>(ctx, it, end);
            break;
         }
         case 'f': {
            ++it;
            match<"alse", Opts>(ctx, it, end);
            break;
         }
         case 't': {
            ++it;
            match<"rue", Opts>(ctx, it, end);
            break;
         }
         case '\0': {
            ctx.error = error_code::unexpected_end;
            break;
         }
         default: {
            skip_number<Opts>(ctx, it, end);
         }
         }
      }
   }

   template <auto Opts>
      requires(bool(Opts.comments))
   GLZ_ALWAYS_INLINE void skip_value<JSON>::op(is_context auto&& ctx, auto&& it, auto end) noexcept
   {
      using namespace glz::detail;

      if constexpr (not check_ws_handled(Opts)) {
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
      }
      switch (*it) {
      case '{': {
         skip_object<Opts>(ctx, it, end);
         break;
      }
      case '[': {
         skip_array<Opts>(ctx, it, end);
         break;
      }
      case '"': {
         skip_string<Opts>(ctx, it, end);
         break;
      }
      case '/': {
         skip_comment(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]]
            return;
         break;
      }
      case 'n': {
         ++it;
         match<"ull", Opts>(ctx, it, end);
         break;
      }
      case 'f': {
         ++it;
         match<"alse", Opts>(ctx, it, end);
         break;
      }
      case 't': {
         ++it;
         match<"rue", Opts>(ctx, it, end);
         break;
      }
      case '\0': {
         ctx.error = error_code::unexpected_end;
         break;
      }
      default: {
         skip_number<Opts>(ctx, it, end);
      }
      }
   }
}
