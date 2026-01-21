// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <set>

#include "glaze/core/meta.hpp"

namespace glz
{
   template <class T>
   struct meta<std::set<T>>
   {
      static constexpr std::string_view name = join_v<chars<"std::set<">, name_v<T>, chars<">">>;
   };
}
