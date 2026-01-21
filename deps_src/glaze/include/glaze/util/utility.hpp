// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstddef>

namespace glz
{
   // TODO: replace with std::unreachable
   [[noreturn]] inline void unreachable() noexcept
   {
      // Uses compiler specific extensions if possible.
      // Even if no extension is used, undefined behavior is still raised by
      // an empty function body and the noreturn attribute.
#ifdef __GNUC__
      // GCC, Clang, ICC
      __builtin_unreachable();
#else
#ifdef _MSC_VER
      // MSVC
      __assume(false);
#endif
#endif
   }
}
