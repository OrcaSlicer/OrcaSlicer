// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/common.hpp"
#include "glaze/core/reflect.hpp"

namespace glz
{
   /**
    * @brief Provides generic struct to struct conversion based on reflected fields.
    *
    * Uses reflected fields to perform a generic conversion from `In` to `Out`.
    * Additional conversion rules can be added in the future, but optional like types are supported.
    *
    * @tparam In  Type of the input struct with optional fields.
    * @tparam Out Type of the output struct with non-optional fields.
    * @param[in] in   The input struct instance.
    * @param[out] out The output struct instance to be populated.
    */
   template <class In, class Out>
   void convert_struct(In&& in, Out&& out)
   {
      auto in_tuple = to_tie(std::forward<In>(in));
      auto out_tuple = to_tie(std::forward<Out>(out));

      constexpr auto N = tuple_size_v<std::decay_t<decltype(in_tuple)>>;
      static_assert(N == tuple_size_v<std::decay_t<decltype(out_tuple)>>);

      constexpr auto in_keys = reflect<In>::keys;
      constexpr auto out_keys = reflect<Out>::keys;

      for_each<N>([&]<auto I>() {
         static_assert(in_keys[I] == out_keys[I]);
         decltype(auto) l = get<I>(out_tuple);
         decltype(auto) r = get<I>(in_tuple);
         if constexpr (requires { l = r; }) {
            l = r;
         }
         else if constexpr (requires { l = r.value(); }) {
            l = r.value();
         }
         else {
            static_assert(false_v<pair<In, Out>>, "Types are not convertible");
         }
      });
   }
}
