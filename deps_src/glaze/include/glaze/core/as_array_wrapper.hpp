// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/opts.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/write.hpp"
#include "glaze/reflection/to_tuple.hpp"

namespace glz
{
   template <uint32_t Format, class T>
      requires(is_specialization_v<T, as_array_wrapper>)
   struct from<Format, T>
   {
      template <auto Opts>
      static void op(auto&& wrapper, is_context auto&& ctx, auto&& it, auto end)
      {
         auto tie = to_tie(wrapper.value);
         parse<Format>::template op<Opts>(tie, ctx, it, end);
      }
   };

   template <uint32_t Format, class T>
      requires(is_specialization_v<T, as_array_wrapper>)
   struct to<Format, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& wrapper, is_context auto&& ctx, Args&&... args)
      {
         auto tie = to_tie(wrapper.value);
         serialize<Format>::template op<Opts>(tie, ctx, std::forward<Args>(args)...);
      }
   };
}
