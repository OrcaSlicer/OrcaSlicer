// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <filesystem>

#include "glaze/core/read.hpp"
#include "glaze/file/file_ops.hpp"

namespace glz
{
   namespace detail
   {
      // TODO: When Apple Clang supports hashing on std::filesystem::path with precompiled headers, we can remove this
      // std::hash<std::filesystem::path> not in the C++20 standard by default
      struct fs_path_hash
      {
         [[nodiscard]] auto operator()(const std::filesystem::path& path) const noexcept
         {
            return std::filesystem::hash_value(path);
         }
      };
   }

   // files should be a map of std::filesystem::path to std::string buffers
   [[nodiscard]] inline error_ctx directory_to_buffers(auto& files, const sv directory_path,
                                                       const sv target_extension = ".json")
   {
      for (const auto& entry : std::filesystem::directory_iterator(directory_path)) {
         if (entry.is_regular_file() && (target_extension.empty() || (entry.path().extension() == target_extension))) {
            if (auto ec = file_to_buffer(files[entry.path()], entry.path().string()); bool(ec)) {
               return {0, ec};
            }
         }
      }
      return {};
   }

   template <auto Opts = opts{}, readable_map_t T>
   [[nodiscard]] error_ctx read_directory(T& value, const sv directory_path, const sv target_extension = ".json")
   {
      std::unordered_map<std::filesystem::path, std::string, detail::fs_path_hash> files{};
      if (auto ec = directory_to_buffers(files, directory_path, target_extension)) {
         return ec;
      }

      for (auto& [path, content] : files) {
         if (auto ec = read<Opts>(value[path], content)) {
            return ec;
         }
      }

      return {};
   }
}
