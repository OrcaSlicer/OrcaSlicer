// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/context.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/util/inline.hpp"

namespace glz
{
   template <>
   struct skip_value<CSV>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (it != end && *it == '"') {
            ++it;

            while (true) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }

               if (*it == '"') {
                  ++it;

                  if (it == end) {
                     return;
                  }

                  if (*it == '"') {
                     ++it;
                     continue;
                  }

                  if (*it == ',' || *it == '\n' || *it == '\r') {
                     return;
                  }

                  ctx.error = error_code::syntax_error;
                  return;
               }

               ++it;
            }
         }
         else {
            while (it != end) {
               const auto ch = *it;
               if (ch == ',' || ch == '\n' || ch == '\r') {
                  return;
               }
               ++it;
            }
         }
      }
   };
}
