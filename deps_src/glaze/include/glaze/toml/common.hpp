#pragma once

#include <cctype>

#include "glaze/core/common.hpp"

namespace glz
{
   // Skip whitespace and comments
   template <class It, class End>
   GLZ_ALWAYS_INLINE void skip_ws_and_comments(It&& it, End end) noexcept
   {
      while (it != end) {
         if (*it == ' ' || *it == '\t') {
            ++it;
         }
         else if (*it == '#') {
            while (it != end && *it != '\n' && *it != '\r') {
               ++it;
            }
         }
         else {
            break;
         }
      }
   }

   // Skip to next line
   template <class Ctx, class It, class End>
   GLZ_ALWAYS_INLINE bool skip_to_next_line(Ctx&, It&& it, End end) noexcept
   {
      while (it != end && *it != '\n' && *it != '\r') {
         ++it;
      }

      if (it == end) {
         return false;
      }

      if (*it == '\r') {
         ++it;
         if (it != end && *it == '\n') {
            ++it;
         }
      }
      else if (*it == '\n') {
         ++it;
      }

      return true;
   }
} // namespace glz
