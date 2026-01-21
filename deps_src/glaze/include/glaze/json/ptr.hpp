// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/ptr.hpp"

namespace glz
{
   template <class T, class B>
   bool read_as_json(T&& root_value, const sv json_ptr, B&& buffer)
   {
      return read_as<opts{}>(std::forward<T>(root_value), json_ptr, buffer);
   }

   template <class T, class B>
   bool write_as_json(T&& root_value, const sv json_ptr, B&& buffer)
   {
      return write_as<opts{}>(std::forward<T>(root_value), json_ptr, buffer);
   }
}
