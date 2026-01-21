// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/common.hpp"

namespace glz
{
   template <class T>
   concept is_value_proxy = requires { T::glaze_value_proxy; };

   template <uint32_t Format, is_value_proxy T>
   struct from<Format, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         parse<JSON>::op<Opts>(value.value(), ctx, it, end);
      }
   };
}
