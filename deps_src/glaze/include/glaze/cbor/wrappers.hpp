// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <type_traits>

#include "glaze/cbor/read.hpp"
#include "glaze/cbor/write.hpp"
#include "glaze/core/custom.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/wrappers.hpp"

namespace glz
{
   template <is_opts_wrapper T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, auto&&... args)
      {
         parse<CBOR>::op<opt_true<Opts, T::opts_member>>(value.val, args...);
      }
   };

   template <is_opts_wrapper T>
   struct to<CBOR, T>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         serialize<CBOR>::op<opt_true<Opts, T::opts_member>>(value.val, ctx, args...);
      }
   };

   // max_length wrapper for limiting string/array sizes when reading
   template <class T, size_t MaxLen>
   struct from<CBOR, max_length_t<T, MaxLen>>
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
         from<CBOR, T>::template op<limited>(wrapper.val, ctx, it, end);
      }
   };

   // max_length wrapper for writing (just passes through without modification)
   template <class T, size_t MaxLen>
   struct to<CBOR, max_length_t<T, MaxLen>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& wrapper, is_context auto&& ctx, auto&&... args)
      {
         to<CBOR, T>::template op<Opts>(wrapper.val, ctx, args...);
      }
   };
}
