// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/beve/header.hpp"
#include "glaze/core/common.hpp"

namespace glz
{
   template <class Member, bool = is_cast<std::remove_cvref_t<Member>>>
   struct beve_member_resolver
   {
      using type = std::remove_cvref_t<Member>;
   };

   template <class Member>
   struct beve_member_resolver<Member, true>
   {
      using cast_member = std::remove_cvref_t<Member>;
      using type = std::remove_cvref_t<typename cast_member::cast_type>;
   };

   template <class T, bool = std::is_enum_v<std::remove_cvref_t<T>>>
   struct beve_numeric_type
   {
      using type = std::remove_cvref_t<T>;
   };

   template <class T>
   struct beve_numeric_type<T, true>
   {
      using type = std::underlying_type_t<std::remove_cvref_t<T>>;
   };

   template <class T, class = void>
   struct beve_underlying_member
   {
      using type = std::remove_cvref_t<T>;
   };

   template <class T>
   struct beve_underlying_member<T, std::enable_if_t<glaze_value_t<std::remove_cvref_t<T>>>>
   {
      using key_t = std::remove_cvref_t<T>;
      using member = std::remove_cvref_t<member_t<key_t, decltype(meta_wrapper_v<key_t>)>>;
      using type = typename beve_member_resolver<member>::type;
   };

   template <class Key>
   struct beve_key_traits
   {
      using key_t = std::remove_cvref_t<Key>;
      using underlying_member = typename beve_underlying_member<key_t>::type;
      using underlying = std::remove_cvref_t<underlying_member>;
      using numeric_type = typename beve_numeric_type<underlying>::type;

      static constexpr bool numeric = std::is_arithmetic_v<numeric_type>;
      static constexpr bool as_string = str_t<underlying> || !numeric;
      static constexpr bool as_number = !as_string;

      static constexpr uint8_t type =
         as_string ? uint8_t(0) : (std::is_signed_v<numeric_type> ? uint8_t(0b000'01'000) : uint8_t(0b000'10'000));

      static constexpr uint8_t width = as_string ? uint8_t(0) : glz::byte_count<numeric_type>;

      static constexpr uint8_t header = uint8_t(tag::object | type | (width << 5));

      static constexpr uint8_t key_tag = as_string ? uint8_t(tag::string) : uint8_t(tag::number | type | (width << 5));
   };
}
