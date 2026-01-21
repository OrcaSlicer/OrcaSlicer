// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <unordered_set>

#include "glaze/core/meta.hpp"

namespace glz
{
   template <class T>
   struct meta<std::unordered_set<T>>
   {
      static constexpr std::string_view name = join_v<chars<"std::unordered_set<">, name_v<T>, chars<">">>;
   };
}
