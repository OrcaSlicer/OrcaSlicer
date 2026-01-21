// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

/**
 * @class guard
 * @brief A copyable and comparable wrapper for std::atomic
 * @tparam T The underlying value type
 *
 * The guard<T> class provides a wrapper around std::atomic<T> that adds copyability
 * and comparability while maintaining thread-safety for individual operations.
 * This allows for more intuitive usage patterns where atomic values need to be
 * treated occasionally as regular values.
 *
 * Unlike std::atomic, guard<T> can be:
 * - Copied (using atomic load operations)
 * - Compared (between atoms and with raw values)
 * - Used with familiar operator syntax
 *
 * @note While individual operations remain atomic, operations like comparison
 * are not atomic as a whole, as they involve multiple loads. This wrapper is
 * appropriate when you need the performance of atomics but occasionally need
 * value semantics.
 *
 * Typical use cases:
 * - Configuration values that are rarely updated but frequently read
 * - Statistics and counters that need occasional comparison
 * - Scenarios where atomic operations are needed but with more familiar syntax
 *
 * Example usage:
 * @code
 * guard<int> counter(0);
 *
 * // Thread-safe operations
 * counter++;
 * counter += 5;
 *
 * // Copyable
 * guard<int> copy = counter;
 *
 * // Comparable
 * if (counter > 10) { ... }
 * if (counter == copy) { ... }
 * @endcode
 */

#include <atomic>
#include <concepts>

#include "glaze/thread/atomic.hpp"

namespace glz
{
   template <typename T>
   class guard
   {
     private:
      std::atomic<T> value;

     public:
      using value_type = T;

      // Default constructor
      constexpr guard() noexcept : value() {}

      // Constructor with initial value
      explicit constexpr guard(T desired) noexcept : value(desired) {}

      guard(const guard& other) noexcept : value(other.load()) {}

      guard(guard&& other) noexcept : value(other.load()) {}

      guard& operator=(const guard& other) noexcept
      {
         store(other.load());
         return *this;
      }

      guard& operator=(guard&& other) noexcept
      {
         store(other.load());
         return *this;
      }

      // Assignment from T
      guard& operator=(T desired) noexcept
      {
         store(desired);
         return *this;
      }

      // Comparison operators
      bool operator==(const guard& other) const noexcept { return load() == other.load(); }

      bool operator!=(const guard& other) const noexcept { return load() != other.load(); }

      bool operator<(const guard& other) const noexcept { return load() < other.load(); }

      bool operator<=(const guard& other) const noexcept { return load() <= other.load(); }

      bool operator>(const guard& other) const noexcept { return load() > other.load(); }

      bool operator>=(const guard& other) const noexcept { return load() >= other.load(); }

      // Comparison with T
      bool operator==(const T& other) const noexcept { return load() == other; }

      bool operator!=(const T& other) const noexcept { return load() != other; }

      bool operator<(const T& other) const noexcept { return load() < other; }

      bool operator<=(const T& other) const noexcept { return load() <= other; }

      bool operator>(const T& other) const noexcept { return load() > other; }

      bool operator>=(const T& other) const noexcept { return load() >= other; }

      // Core atomic operations
      T load(std::memory_order order = std::memory_order_seq_cst) const noexcept { return value.load(order); }

      void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept
      {
         value.store(desired, order);
      }

      T exchange(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept
      {
         return value.exchange(desired, order);
      }

      bool compare_exchange_weak(T& expected, T desired, std::memory_order success = std::memory_order_seq_cst,
                                 std::memory_order failure = std::memory_order_seq_cst) noexcept
      {
         return value.compare_exchange_weak(expected, desired, success, failure);
      }

      bool compare_exchange_strong(T& expected, T desired, std::memory_order success = std::memory_order_seq_cst,
                                   std::memory_order failure = std::memory_order_seq_cst) noexcept
      {
         return value.compare_exchange_strong(expected, desired, success, failure);
      }

      // Conversion operator to T
      operator T() const noexcept { return load(); }

      // Additional atomic operations for arithmetic types
      T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
         requires std::integral<T>
      {
         return value.fetch_add(arg, order);
      }

      T fetch_sub(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
         requires std::integral<T>
      {
         return value.fetch_sub(arg, order);
      }

      T fetch_and(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
         requires std::integral<T>
      {
         return value.fetch_and(arg, order);
      }

      T fetch_or(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
         requires std::integral<T>
      {
         return value.fetch_or(arg, order);
      }

      T fetch_xor(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
         requires std::integral<T>
      {
         return value.fetch_xor(arg, order);
      }

      // Operator overloads for arithmetic types
      T operator+=(T arg) noexcept
         requires std::integral<T> || std::floating_point<T>
      {
         return value.fetch_add(arg) + arg;
      }

      T operator-=(T arg) noexcept
         requires std::integral<T> || std::floating_point<T>
      {
         return value.fetch_sub(arg) - arg;
      }

      T operator&=(T arg) noexcept
         requires std::integral<T>
      {
         return value.fetch_and(arg) & arg;
      }

      T operator|=(T arg) noexcept
         requires std::integral<T>
      {
         return value.fetch_or(arg) | arg;
      }

      T operator^=(T arg) noexcept
         requires std::integral<T>
      {
         return value.fetch_xor(arg) ^ arg;
      }

      // Increment/decrement operators
      T operator++() noexcept
         requires std::integral<T> || std::floating_point<T>
      {
         return fetch_add(1) + 1;
      }

      T operator++(int) noexcept
         requires std::integral<T> || std::floating_point<T>
      {
         return fetch_add(1);
      }

      T operator--() noexcept
         requires std::integral<T> || std::floating_point<T>
      {
         return fetch_sub(1) - 1;
      }

      T operator--(int) noexcept
         requires std::integral<T> || std::floating_point<T>
      {
         return fetch_sub(1);
      }

      bool is_lock_free() const noexcept { return value.is_lock_free(); }

      bool is_lock_free() const volatile noexcept { return value.is_lock_free(); }
   };

   // Non-member comparison operators
   template <typename T>
   bool operator==(const T& lhs, const guard<T>& rhs) noexcept
   {
      return lhs == rhs.load();
   }

   template <typename T>
   bool operator!=(const T& lhs, const guard<T>& rhs) noexcept
   {
      return lhs != rhs.load();
   }

   template <typename T>
   bool operator<(const T& lhs, const guard<T>& rhs) noexcept
   {
      return lhs < rhs.load();
   }

   template <typename T>
   bool operator<=(const T& lhs, const guard<T>& rhs) noexcept
   {
      return lhs <= rhs.load();
   }

   template <typename T>
   bool operator>(const T& lhs, const guard<T>& rhs) noexcept
   {
      return lhs > rhs.load();
   }

   template <typename T>
   bool operator>=(const T& lhs, const guard<T>& rhs) noexcept
   {
      return lhs >= rhs.load();
   }
}
