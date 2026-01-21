// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <filesystem>
#include <map>
#include <string_view>

#include "glaze/api/api.hpp"

#if defined(_WIN32) || defined(__CYGWIN__)
#ifndef GLAZE_API_ON_WINDOWS
#define GLAZE_API_ON_WINDOWS
#endif
#endif

#ifdef GLAZE_API_ON_WINDOWS
#ifdef NOMINMAX
#include <windows.h>
#else
#define NOMINMAX
#include <windows.h>
#undef NOMINMAX
#endif
#define SHARED_LIBRARY_EXTENSION ".dll"
#define SHARED_LIBRARY_PREFIX ""
#elif __APPLE__
#include <dlfcn.h>
#define SHARED_LIBRARY_EXTENSION ".dylib"
#define SHARED_LIBRARY_PREFIX "lib"
#elif __has_include(<dlfcn.h>)
#include <dlfcn.h>
#define SHARED_LIBRARY_EXTENSION ".so"
#define SHARED_LIBRARY_PREFIX "lib"
#endif

namespace glz
{
#ifdef GLAZE_API_ON_WINDOWS
   using lib_t = HINSTANCE;
#else
   using lib_t = void*;
#endif

   struct lib_loader final
   {
      using create = glz::iface_fn (*)(void) noexcept;

      iface api_map{};
      std::vector<lib_t> loaded_libs{};

      void load(const sv path)
      {
         const std::filesystem::path libpath(path);
         if (std::filesystem::is_directory(libpath)) {
            load_libs(path);
         }
         else if (libpath.extension() == SHARED_LIBRARY_EXTENSION) {
            load_lib(libpath.string());
         }
         else {
            load_lib_by_name(libpath.string());
         }
      }

      void load_libs(const sv directory)
      {
         std::filesystem::directory_entry dir(directory);
         for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == SHARED_LIBRARY_EXTENSION) {
               load_lib(entry.path().string());
            }
         }
      }

      auto& operator[](const sv lib_name) { return api_map[std::string(lib_name)]; }

      lib_loader() = default;
      lib_loader(const lib_loader&) = delete;
      lib_loader(lib_loader&&) = delete;
      lib_loader& operator=(const lib_loader&) = delete;
      lib_loader& operator=(lib_loader&&) = delete;

      lib_loader(const std::string_view directory) { load(directory); }

      ~lib_loader()
      {
         api_map.clear();
         for (const auto& lib : loaded_libs) {
#ifdef GLAZE_API_ON_WINDOWS
            FreeLibrary(lib);
#else
            dlclose(lib);
#endif
         }
      }

     private:
      bool load_lib(const std::string& path) noexcept
      {
#ifdef GLAZE_API_ON_WINDOWS
         std::filesystem::path file_path(path);
         lib_t loaded_lib = LoadLibraryW(file_path.native().c_str());
#else
         lib_t loaded_lib = dlopen(path.c_str(), RTLD_LAZY);
#endif
         if (loaded_lib) {
            loaded_libs.emplace_back(loaded_lib);

#ifdef GLAZE_API_ON_WINDOWS
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            auto* ptr = reinterpret_cast<create>(GetProcAddress(loaded_lib, "glz_iface"));
#pragma GCC diagnostic pop
#else
            auto* ptr = reinterpret_cast<create>(GetProcAddress(loaded_lib, "glz_iface"));
#endif
#else
            auto* ptr = reinterpret_cast<create>(dlsym(dlopen(path.c_str(), RTLD_NOW), "glz_iface"));
#endif
            if (ptr) {
               std::shared_ptr<glz::iface> shared_iface_ptr = (*ptr)()();
               api_map.merge(*shared_iface_ptr);
               return true;
            }
         }

         return false;
      }

      bool load_lib_by_name(const std::string& path)
      {
#ifdef NDEBUG
         static std::string suffix = "";
#else
         static std::string suffix = "_d";
#endif
         const std::filesystem::path combined_path(path + suffix + SHARED_LIBRARY_EXTENSION);

         return (load_lib(std::filesystem::canonical(combined_path).string()));
      }
   };

}
