// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <climits>

#include "glaze/api/xxh64.hpp"
#include "glaze/core/meta.hpp"

// Collision calculations done with the formula: e^((-k * (k - 1)/(2 * N)))
// The approximation error tends to zero as N increases, and we are dealing with a large N
// https://preshing.com/20110504/hash-collision-probabilities/
// With 10,000 registered types the probabilities of a collsion are:
// 64 bit hash: 2.71027645e-12 === 1 - 1/e^(6249375/2305843009213693952)
// 128 bit hash: 1.469221e-31 === 1 - 1/e^(6249375/42535295865117307932921825928971026432)
// 256 bit hash: 4.317652e-70 === 1 -
// 1/e^(6249375/14474011154664524427946373126085988481658748083205070504932198000989141204992) Probability of winning
// the Mega Millions lottery: 1 / 302,575,350 === 3.30496189e-9 Number of times winning to equal a collision chance: 64
// bit: 1220 times 128 bit: 2.2494655e22 times (or 22 sextillion times) 256 bit: 7.6545351e60 times From these
// calculations it is apparent that a 128 bit hash is more than sufficient

namespace glz
{
   template <class T, T Value>
   consteval auto make_array()
   {
      return []<size_t... I>(std::index_sequence<I...>) {
         return std::array<char, sizeof(T)>{static_cast<char>(((Value >> (CHAR_BIT * I)) & 0xff))...};
      }(std::make_index_sequence<sizeof(T)>{});
   }

   namespace detail
   {
      // convert an integer to a string_view at compile time

      constexpr uint64_t num_digits(auto x) noexcept // number of digits needed, including minus sign
      {
         return x < 10 ? 1 : 1 + num_digits(x / 10);
      }

      template <char... args>
      struct metastring
      {
         const char data[sizeof...(args)] = {args...};
      };

      // recursive number-printing template, general case (for three or more digits)
      template <uint64_t size, uint64_t x, char... args>
      struct numeric_builder
      {
         using type = typename numeric_builder<size - 1, x / 10, '0' + x % 10, args...>::type;
      };

      // special case for two digits; minus sign is handled here
      template <uint64_t x, char... args>
      struct numeric_builder<2, x, args...>
      {
         using type = metastring<'0' + x / 10, '0' + x % 10, args...>;
      };

      // special case for one digit (positive numbers only)
      template <uint64_t x, char... args>
      struct numeric_builder<1, x, args...>
      {
         using type = metastring<'0' + x, args...>;
      };

      // convenience wrapper for numeric_builder
      template <uint64_t x>
      struct numeric_string
      {
         // generate a unique string type representing this number
         using type = typename numeric_builder<num_digits(x), x, '\0'>::type;

         // declare a static string of that type (instantiated later at file scope)
         static constexpr type value{};
         static constexpr const std::string_view get() { return {value.data, num_digits(x)}; }
      };
   }

   template <class T, T Value>
   struct int_to_sv
   {
      static constexpr auto arr = make_array<T, Value>();
      static consteval std::string_view get() { return {arr.data(), arr.size()}; }
   };

   template <class T, T Value>
   inline constexpr std::string_view int_to_sv_v = int_to_sv<T, Value>{}.get();

   template <uint64_t I>
   inline consteval auto to_sv()
   {
      return detail::numeric_string<I>::get();
   }

   template <size_t I>
   struct hash128_i
   {
      static constexpr sv str = to_sv<I>();
      static constexpr sv h0 = int_to_sv_v<uint64_t, xxh64::hash(str.data(), str.size(), 0)>;
      static constexpr sv h1 = int_to_sv_v<uint64_t, xxh64::hash(str.data(), str.size(), 1)>;
      static constexpr sv value = join_v<h0, h1>;
   };

   template <size_t I>
   inline constexpr std::string_view hash128_i_v = hash128_i<I>::value;

   template <const std::string_view& Str>
   struct hash128
   {
      static constexpr sv h0 = int_to_sv_v<uint64_t, xxh64::hash(Str.data(), Str.size(), 0)>;
      static constexpr sv h1 = int_to_sv_v<uint64_t, xxh64::hash(Str.data(), Str.size(), 1)>;
      static constexpr sv value = join_v<h0, h1>;
   };

   template <const std::string_view& Str>
   constexpr std::string_view hash128_v = hash128<Str>::value;
}
