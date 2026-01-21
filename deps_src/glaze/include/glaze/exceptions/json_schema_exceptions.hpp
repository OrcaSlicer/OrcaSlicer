// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#if __cpp_exceptions

#include "glaze/exceptions/core_exceptions.hpp"
#include "glaze/json/schema.hpp"

namespace glz::ex
{
   template <class T, auto Opts = opts{}, class Buffer>
   void write_json_schema(Buffer&& buffer)
   {
      const auto ec = glz::write_json_schema<T, Opts>(buffer);
      if (bool(ec)) [[unlikely]] {
         throw std::runtime_error(format_error(ec, buffer));
      }
   }

   template <class T, auto Opts = opts{}>
   [[nodiscard]] std::string write_json_schema()
   {
      std::string buffer{};
      glz::ex::write_json_schema<T, Opts>(buffer);
      return buffer;
   }
}

#endif
