// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <type_traits>

#include "glaze/core/custom.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/wrappers.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   template <is_opts_wrapper T>
   struct from<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, auto&&... args)
      {
         parse<BEVE>::op<opt_true<Opts, T::opts_member>>(value.val, args...);
      }
   };

   template <is_opts_wrapper T>
   struct to<BEVE, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         serialize<BEVE>::op<opt_true<Opts, T::opts_member>>(value.val, ctx, args...);
      }
   };

   // max_length wrapper for limiting string/array sizes when reading
   template <class T, size_t MaxLen>
   struct from<BEVE, max_length_t<T, MaxLen>>
   {
     private:
      template <auto Opts>
      static consteval auto make_limited_opts()
      {
         if constexpr (str_t<T>) {
            if constexpr (requires { Opts.max_string_length; }) {
               auto ret = Opts;
               ret.max_string_length = MaxLen;
               return ret;
            }
            else {
               struct extended : std::decay_t<decltype(Opts)>
               {
                  size_t max_string_length = MaxLen;
               };
               return extended{Opts};
            }
         }
         else if constexpr (readable_array_t<T>) {
            if constexpr (requires { Opts.max_array_size; }) {
               auto ret = Opts;
               ret.max_array_size = MaxLen;
               return ret;
            }
            else {
               struct extended : std::decay_t<decltype(Opts)>
               {
                  size_t max_array_size = MaxLen;
               };
               return extended{Opts};
            }
         }
         else {
            return Opts;
         }
      }

     public:
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& wrapper, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto limited = make_limited_opts<Opts>();
         from<BEVE, T>::template op<limited>(wrapper.val, ctx, it, end);
      }

      template <auto Opts>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& wrapper, const uint8_t tag, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto limited = make_limited_opts<Opts>();
         from<BEVE, T>::template op<limited>(wrapper.val, tag, ctx, it, end);
      }
   };

   // max_length wrapper for writing (just passes through without modification)
   template <class T, size_t MaxLen>
   struct to<BEVE, max_length_t<T, MaxLen>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& wrapper, is_context auto&& ctx, auto&&... args)
      {
         to<BEVE, T>::template op<Opts>(wrapper.val, ctx, args...);
      }
   };
}
