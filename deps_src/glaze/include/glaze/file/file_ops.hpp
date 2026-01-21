// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

#include "glaze/core/context.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
// Turn off MSVC warning for unsafe fopen
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

namespace glz
{
   template <class T>
   [[nodiscard]] error_code file_to_buffer(T& buffer, auto* file, const std::string_view path)
   {
      if (!file) {
         return error_code::file_open_failure;
      }

      std::error_code ec{};
      const auto n = std::filesystem::file_size(path, ec);
      if (ec) {
         std::fclose(file);
         return error_code::file_open_failure;
      }
      buffer.resize(n);

      if (n != std::fread(static_cast<void*>(buffer.data()), 1, n, file)) {
         std::fclose(file);
         return error_code::file_open_failure;
      }

      if (std::fclose(file)) {
         return error_code::file_close_failure;
      }

      return {};
   }

   template <class T>
   [[nodiscard]] error_code file_to_buffer(T& buffer, const std::string_view file_name)
   {
      auto* file = std::fopen(file_name.data(), "rb");
      return file_to_buffer(buffer, file, file_name);
   }

   template <class T>
   std::string file_to_buffer(T&& file_name)
   {
      std::string buffer{};
      file_to_buffer(buffer, std::forward<T>(file_name));
      return buffer;
   }

   inline std::filesystem::path relativize_if_not_absolute(const std::filesystem::path& working_directory,
                                                           const std::filesystem::path& filepath)
   {
      if (filepath.is_absolute()) {
         return filepath;
      }

      return working_directory / filepath;
   }
}

#if defined(_MSC_VER) && !defined(__clang__)
// restore disabled warning
#pragma warning(pop)
#endif
