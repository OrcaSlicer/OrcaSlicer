// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#if __cpp_exceptions

#include "glaze/exceptions/core_exceptions.hpp"
#include "glaze/glaze.hpp"

namespace glz::ex
{
   template <uint32_t layout = rowwise, class T, class Buffer>
   inline void read_csv(T&& value, Buffer&& buffer)
   {
      const auto ec = glz::read_csv<layout>(std::forward<T>(value), std::forward<Buffer>(buffer));
      if (ec) {
         throw std::runtime_error("read_csv error");
      }
   }

   template <uint32_t layout = rowwise, class T, class Buffer>
   inline auto read_csv(Buffer&& buffer)
   {
      auto ex = glz::read<T, opts_csv{.layout = layout}>(std::forward<Buffer>(buffer));
      if (ex) {
         throw std::runtime_error("read_csv error");
      }
      return ex.value();
   }

   template <uint32_t layout = rowwise, class T>
   inline void read_file_csv(T& value, const sv file_name, auto&& buffer)
   {
      const auto ec = read_file_csv<layout>(value, file_name, buffer);
      if (ec) {
         throw std::runtime_error("read_file_csv error");
      }
   }
}

namespace glz::ex
{
   template <class T, class Buffer>
   inline auto write_csv(T&& value, Buffer&& buffer)
   {
      const auto ec = write<opts_csv{}>(std::forward<T>(value), std::forward<Buffer>(buffer));
      if (ec) {
         throw std::runtime_error("write_csv error");
      }
   }

   template <class T>
   inline auto write_csv(T&& value)
   {
      return write_csv(std::forward<T>(value));
   }

   template <uint32_t layout = rowwise, class T>
   inline void write_file_csv(T&& value, const sv file_name, auto&& buffer)
   {
      const auto ec = write_file_csv<layout>(std::forward<T>(value), file_name, buffer);
      if (ec) {
         throw std::runtime_error("write_file_csv error");
      }
   }
}

#endif
