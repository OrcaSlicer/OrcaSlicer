// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <list>

#include "glaze/core/meta.hpp"

namespace glz
{
   template <class T>
   struct meta<std::list<T>>
   {
      static constexpr std::string_view name = join_v<chars<"std::list<">, name_v<T>, chars<">">>;
   };
}
