// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/common.hpp"

namespace glz
{
   template <class T>
   concept json_object = glaze_object_t<T> || reflectable<T> || writable_map_t<T> || readable_map_t<T>;

   template <class T>
   concept json_array = array_t<T>;

   template <class T>
   concept json_string = str_t<T>;

   template <class T>
   concept json_boolean = boolean_like<T>;

   template <class T>
   concept json_number = num_t<T>;

   template <class T>
   concept json_integer = int_t<T>;

   template <class T>
   concept json_null = null_t<T>;
}
