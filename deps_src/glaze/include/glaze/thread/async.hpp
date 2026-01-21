// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <concepts>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>

#include "glaze/util/type_traits.hpp"

// The purpose of glz::async is to create a thread-safe wrapper around a type
// The only way to access the data is by supplying lambdas to `read` or `write`
// methods, which feed underlying data into the lambda.
// A lock is held for the duration of the call.

// Example:
// struct foo { int x{}; };
// async<foo> s{};
// s.write([](auto& value) { value.x = 42; });
// s.read([](const auto& value) { std::cout << value.x << '\n'; });

namespace glz
{
   template <class T, class Callable>
   concept const_callable = std::invocable<Callable, const T&>;

   template <class T, class Callable>
   concept non_const_callable = std::invocable<Callable, T&&> || std::invocable<Callable, T&>;

   template <class Arg, class Callable>
   concept void_return = std::same_as<std::invoke_result_t<Callable, Arg>, void>;

   template <class T>
   class async
   {
      T data{};
      mutable std::shared_mutex mtx{};

     public:
      async() = default;

      template <class U>
         requires(!is_specialization_v<std::decay_t<U>, async>)
      async(U&& initial_value) : data(std::forward<U>(initial_value))
      {}

      async(const async& other)
         requires(std::copy_constructible<T>)
      {
         std::shared_lock lock(other.mtx);
         data = other.data;
      }

      async(async&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
         requires(std::move_constructible<T>)
      {
         std::unique_lock lock(other.mtx);
         data = std::move(other.data);
      }

      async& operator=(const async& other)
         requires(std::is_copy_assignable_v<T>)
      {
         if (this != &other) {
            std::scoped_lock lock(mtx, other.mtx);
            data = other.data;
         }
         return *this;
      }

      async& operator=(async&& other) noexcept(std::is_nothrow_move_assignable_v<T>)
         requires(std::is_move_assignable_v<T>)
      {
         if (this != &other) {
            std::scoped_lock lock(mtx, other.mtx);
            data = std::move(other.data);
         }
         return *this;
      }

      T copy() const
      {
         std::shared_lock lock(mtx);
         return data;
      }

      // Read with non-void return value.
      template <class Callable>
         requires(const_callable<T, Callable> && !void_return<const T&, Callable>)
      auto read(Callable&& f) const -> std::invoke_result_t<Callable, const T&>
      {
         std::shared_lock lock(mtx);
         return std::forward<Callable>(f)(data);
      }

      // Read with void return.
      template <class Callable>
         requires(const_callable<T, Callable> && void_return<const T&, Callable>)
      void read(Callable&& f) const
      {
         std::shared_lock lock(mtx);
         std::forward<Callable>(f)(data);
      }

      // Write with non-void return value.
      template <class Callable>
         requires(non_const_callable<T, Callable> && !void_return<T&, Callable>)
      auto write(Callable&& f) -> std::invoke_result_t<Callable, T&>
      {
         std::unique_lock lock(mtx);
         return std::forward<Callable>(f)(data);
      }

      // Write with void return.
      template <class Callable>
         requires(non_const_callable<T, Callable> && void_return<T&, Callable>)
      void write(Callable&& f)
      {
         std::unique_lock lock(mtx);
         std::forward<Callable>(f)(data);
      }
   };
}
