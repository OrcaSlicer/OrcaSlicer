// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#if __cpp_exceptions

#include "glaze/core/read.hpp"
#include "glaze/core/write.hpp"

namespace glz::ex
{
   template <auto Opts, class T>
      requires read_supported<T, Opts.format>
   void read(T&& value, auto&& buffer)
   {
      const auto ec = glz::read<Opts>(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         throw std::runtime_error(format_error(ec, buffer));
      }
   }
}

namespace glz::ex
{
   // For writing to a std::string, std::vector<char>, std::deque<char> and the like
   template <auto Opts, class T, output_buffer Buffer>
      requires write_supported<T, Opts.format>
   void write(T&& value, Buffer& buffer, is_context auto&& ctx)
   {
      const auto ec = glz::write<Opts>(std::forward<T>(value), buffer, ctx);
      if (bool(ec)) [[unlikely]] {
         throw std::runtime_error(format_error(ec, buffer));
      }
   }

   template <auto& Partial, auto Opts, class T, output_buffer Buffer>
      requires write_supported<T, Opts.format>
   void write(T&& value, Buffer& buffer)
   {
      const auto ec = glz::write(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         throw std::runtime_error(format_error(ec, buffer));
      }
   }

   template <auto Opts, class T, output_buffer Buffer>
      requires write_supported<T, Opts.format>
   void write(T&& value, Buffer& buffer)
   {
      glz::context ctx{};
      glz::ex::write<Opts>(std::forward<T>(value), buffer, ctx);
   }

   template <auto Opts, class T>
      requires write_supported<T, Opts.format>
   [[nodiscard]] std::string write(T&& value)
   {
      const auto e = glz::write<Opts>(std::forward<T>(value));
      if (not e) [[unlikely]] {
         throw std::runtime_error(format_error(e));
      }
      return e.value();
   }

   template <auto Opts, class T, raw_buffer Buffer>
      requires write_supported<T, Opts.format>
   [[nodiscard]] size_t write(T&& value, Buffer&& buffer)
   {
      const auto e = write<Opts>(std::forward<T>(value), std::forward<Buffer>(buffer));
      if (not e) [[unlikely]] {
         throw std::runtime_error(format_error(e, buffer));
      }
      return e.value();
   }

   // requires file_name to be null terminated
   void buffer_to_file(auto&& buffer, const sv file_name)
   {
      const auto ec = buffer_to_file(buffer, file_name);
      if (bool(ec)) [[unlikely]] {
         throw std::runtime_error(format_error(ec, buffer));
      }
   }
}

#endif
