// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <chrono>
#include <type_traits>

namespace glz
{
   // Concept for std::chrono::duration types
   template <class T>
   concept is_duration = requires {
      typename T::rep;
      typename T::period;
      requires std::is_same_v<std::remove_cvref_t<T>, std::chrono::duration<typename T::rep, typename T::period>>;
   };

   // Concept for std::chrono::time_point types
   template <class T>
   concept is_time_point = requires {
      typename T::clock;
      typename T::duration;
      requires std::is_same_v<std::remove_cvref_t<T>, std::chrono::time_point<typename T::clock, typename T::duration>>;
   };

   // Concept for system_clock time_points (serialize as ISO 8601 string)
   template <class T>
   concept is_system_time_point =
      is_time_point<T> && std::is_same_v<typename std::remove_cvref_t<T>::clock, std::chrono::system_clock>;

   // Concept for steady_clock time_points (serialize as numeric count)
   template <class T>
   concept is_steady_time_point =
      is_time_point<T> && std::is_same_v<typename std::remove_cvref_t<T>::clock, std::chrono::steady_clock>;

   // Detect if high_resolution_clock is a true alias or a distinct type
   inline constexpr bool hrc_is_system = std::is_same_v<std::chrono::high_resolution_clock, std::chrono::system_clock>;
   inline constexpr bool hrc_is_steady = std::is_same_v<std::chrono::high_resolution_clock, std::chrono::steady_clock>;

   // Concept for high_resolution_clock when it's a distinct type (rare)
   template <class T>
   concept is_high_res_time_point =
      is_time_point<T> && std::is_same_v<typename std::remove_cvref_t<T>::clock, std::chrono::high_resolution_clock> &&
      !hrc_is_system && !hrc_is_steady;

   // ============================================
   // epoch_time wrapper for Unix timestamp format
   // ============================================

   // Wrapper that controls serialization format, not storage
   // Internally stores system_clock::time_point with native precision
   // Template parameter specifies the OUTPUT format precision
   template <class Duration>
   struct epoch_time
   {
      std::chrono::system_clock::time_point value{};

      // Implicit conversions for ergonomic use
      epoch_time() = default;
      epoch_time(std::chrono::system_clock::time_point tp) : value(tp) {}
      operator std::chrono::system_clock::time_point() const { return value; }

      // Comparison operators
      bool operator==(const epoch_time&) const = default;
      auto operator<=>(const epoch_time&) const = default;
   };

   // Convenience aliases - name indicates OUTPUT format
   using epoch_seconds = epoch_time<std::chrono::seconds>;
   using epoch_millis = epoch_time<std::chrono::milliseconds>;
   using epoch_micros = epoch_time<std::chrono::microseconds>;
   using epoch_nanos = epoch_time<std::chrono::nanoseconds>;

   // Concept to detect epoch_time wrapper
   template <class T>
   concept is_epoch_time = requires(T t) {
      { t.value } -> std::convertible_to<std::chrono::system_clock::time_point>;
   };

   // ============================================
   // TOML Local Date/Time support
   // ============================================

   // Concept for std::chrono::year_month_day (TOML Local Date)
   template <class T>
   concept is_year_month_day = std::is_same_v<std::remove_cvref_t<T>, std::chrono::year_month_day>;

   // Concept for std::chrono::hh_mm_ss (TOML Local Time)
   template <class T>
   concept is_hh_mm_ss = requires {
      typename std::remove_cvref_t<T>::precision;
      requires requires(T t) {
         { t.hours() } -> std::convertible_to<std::chrono::hours>;
         { t.minutes() } -> std::convertible_to<std::chrono::minutes>;
         { t.seconds() } -> std::convertible_to<std::chrono::seconds>;
         { t.subseconds() };
         { t.is_negative() } -> std::convertible_to<bool>;
      };
   };
}
