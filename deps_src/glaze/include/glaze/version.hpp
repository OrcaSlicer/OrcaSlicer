// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>

namespace glz
{
   /**
    * @struct version_t
    * @brief Represents the version of the Glaze Library
    *
    * Provides version information and comparison operators to check version compatibility.
    */
   struct version_t
   {
      uint8_t major = 7;
      uint8_t minor = 0;
      uint8_t patch = 1;

      constexpr auto operator<=>(const version_t& other) const noexcept = default;

      // In C++23, this is optional when we have a custom <=> operator,
      // but included for clarity
      constexpr bool operator==(const version_t& other) const noexcept = default;
   };

   /**
    * @var version
    * @brief Global constant instance of the current library version
    *
    * Provides access to the current version of the Glaze Library.
    */
   inline constexpr version_t version{};
}
