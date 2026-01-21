// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <deque>

#include "glaze/core/meta.hpp"

namespace glz
{
   template <class T>
   struct meta<std::deque<T>>
   {
      static constexpr std::string_view name = join_v<chars<"std::deque<">, name_v<T>, chars<">">>;
   };
}
