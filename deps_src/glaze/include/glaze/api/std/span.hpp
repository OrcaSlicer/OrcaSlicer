// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <span>

#include "glaze/api/hash.hpp"
#include "glaze/core/meta.hpp"

namespace glz
{
   template <class, template <class, size_t> class>
   inline constexpr bool is_span_v = false;
   template <template <class, size_t> class T, class Element, size_t Extent>
   inline constexpr bool is_span_v<T<Element, Extent>, T> = true;

   template <class T>
   concept span = is_span_v<T, std::span>;

   template <span T>
   struct meta<T>
   {
      using V = typename T::element_type;
      static constexpr std::string_view extent = to_sv<T::extent>();
      static constexpr std::string_view name = join_v<chars<"std::span<">, name_v<V>, chars<",">, extent, chars<">">>;
   };
}
