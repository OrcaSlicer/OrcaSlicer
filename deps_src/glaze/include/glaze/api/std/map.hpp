// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <map>

#include "glaze/core/meta.hpp"

namespace glz
{
   template <class Key, class Mapped>
   struct meta<std::map<Key, Mapped>>
   {
      static constexpr std::string_view name =
         join_v<chars<"std::map<">, name_v<Key>, chars<",">, name_v<Mapped>, chars<">">>;
   };
}
