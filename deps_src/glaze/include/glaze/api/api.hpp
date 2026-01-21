// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>

#include "glaze/api/std/string.hpp"
#include "glaze/api/trait.hpp"
#include "glaze/core/context.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/util/expected.hpp"

namespace glz
{
   inline namespace v0_0_3
   {
      template <class R>
      using func_return_t =
         std::conditional_t<std::is_lvalue_reference_v<R>, std::reference_wrapper<std::decay_t<R>>, R>;

      struct api
      {
         api() noexcept = default;
         api(const api&) noexcept = default;
         api(api&&) noexcept = default;
         api& operator=(const api&) noexcept = default;
         api& operator=(api&&) noexcept = default;
         virtual ~api() noexcept {}

         template <class T>
         [[nodiscard]] T* get(const sv path) noexcept;

         // Get a std::function from a member function or std::function across the API
         template <class T>
         [[nodiscard]] expected<T, error_code> get_fn(const sv path) noexcept;

         template <class Ret, class... Args>
         expected<func_return_t<Ret>, error_code> call(const sv path, Args&&... args) noexcept;

         [[nodiscard]] virtual bool contains(const sv path) noexcept = 0;

         virtual bool read(const uint32_t /*format*/, const sv /*path*/, const sv /*data*/) noexcept = 0;

         virtual bool write(const uint32_t /*format*/, const sv /*path*/, std::string& /*data*/) noexcept = 0;

         [[nodiscard]] virtual const sv last_error() const noexcept { return error; }

         /// unchecked `void*` access for low level programming (prefer templated get)
         [[nodiscard]] virtual std::pair<void*, glz::hash_t> get(const sv path) noexcept = 0;

        protected:
         virtual bool caller(const sv path, const glz::hash_t type_hash, void*& ret,
                             std::span<void*> args) noexcept = 0;

         virtual std::unique_ptr<void, void (*)(void*)> get_fn(const sv path, const glz::hash_t type_hash) noexcept = 0;

         std::string error{};
      };

      using iface = std::map<std::string, std::function<std::shared_ptr<api>()>, std::less<>>;

      template <class T>
      T* api::get(const sv path) noexcept
      {
         static constexpr auto hash = glz::hash<T>();
         auto p = get(path);
         if (p.first && p.second == hash) {
            return static_cast<T*>(p.first);
         }
         return nullptr;
      }

      template <class T>
      glz::expected<T, error_code> api::get_fn(const sv path) noexcept
      {
         static constexpr auto hash = glz::hash<T>();
         auto d = get_fn(path, hash);
         if (d) {
            T copy = *static_cast<T*>(d.get());
            return copy;
         }
         else {
            return glz::unexpected(error_code::invalid_get_fn);
         }
      }

      template <class Ret, class... Args>
      glz::expected<func_return_t<Ret>, error_code> api::call(const sv path, Args&&... args) noexcept
      {
         using F = std::function<Ret(Args...)>;
         static constexpr auto hash = glz::hash<F>();

         static constexpr auto N = sizeof...(Args);
         std::array<void*, N> arguments;

         auto tuple_args = std::forward_as_tuple(std::forward<Args>(args)...);

         for_each<N>([&]<auto I>() { std::get<I>(arguments) = &std::get<I>(tuple_args); });

         if constexpr (std::is_pointer_v<Ret>) {
            void* ptr{};
            const auto success = caller(path, hash, ptr, arguments);

            if (success) {
               return static_cast<Ret>(ptr);
            }
         }
         else if constexpr (std::is_void_v<Ret>) {
            void* ptr = nullptr;
            const auto success = caller(path, hash, ptr, arguments);

            if (success) {
               return expected<void, error_code>{};
            }
         }
         else if constexpr (std::is_lvalue_reference_v<Ret>) {
            void* ptr{};
            const auto success = caller(path, hash, ptr, arguments);

            if (success) {
               return std::ref(*static_cast<std::decay_t<Ret>*>(ptr));
            }
         }
         else {
            Ret value{};
            void* ptr = &value;
            const auto success = caller(path, hash, ptr, arguments);

            if (success) {
               return value;
            }
         }

         return glz::unexpected(error_code::invalid_call);
      }

      using iface_fn = std::shared_ptr<glz::iface> (*)();
   }
}

#if defined(_WIN32) || defined(__CYGWIN__)
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT
#endif

extern "C" DLL_EXPORT glz::iface_fn glz_iface() noexcept;
