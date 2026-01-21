// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <vector>

#include "glaze/core/meta.hpp"

namespace glz
{
   template <class T>
   struct meta<std::vector<T>>
   {
      static constexpr std::string_view name = join_v<chars<"std::vector<">, name_v<T>, chars<">">>;
   };
}
