// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cstdint>

namespace glz
{
   // Creates a uint16_t from two chars that matches memcpy behavior on any endianness
   consteval uint16_t to_uint16_t(const char chars[2])
   {
      if constexpr (std::endian::native == std::endian::little) {
         return uint16_t(chars[0]) | (uint16_t(chars[1]) << 8);
      }
      else {
         return (uint16_t(chars[0]) << 8) | uint16_t(chars[1]);
      }
   }
}
