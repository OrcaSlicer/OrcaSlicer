#pragma once

#include <ei.h>

#include <array>
#include <cstdint>
#include <string>

#include "cmp.hpp"

namespace glz::eetf
{
   struct tag_atom
   {};

   struct tag_string
   {};

   template <typename Tag>
   struct tagged_string : std::string
   {
      using tag = Tag;
   };

   using atom = tagged_string<tag_atom>;
   constexpr atom operator""_atom(const char* str, std::size_t sz)
   {
      // TODO check if valid atom
      return atom(std::string(str, sz));
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_atom(const Tag& tag)
   {
      return cmp::is<in, ERL_ATOM_EXT, ERL_SMALL_ATOM_EXT, ERL_ATOM_UTF8_EXT, ERL_SMALL_ATOM_UTF8_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_integer(const Tag& tag)
   {
      return cmp::is<in, ERL_INTEGER_EXT, ERL_SMALL_INTEGER_EXT, ERL_SMALL_BIG_EXT, ERL_LARGE_BIG_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_floating_point(const Tag& tag)
   {
      return cmp::is<in, ERL_FLOAT_EXT, NEW_FLOAT_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_string(const Tag& tag)
   {
      return cmp::is<in, ERL_STRING_EXT, ERL_NIL_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_tuple(const Tag& tag)
   {
      return cmp::is<in, ERL_SMALL_TUPLE_EXT, ERL_LARGE_TUPLE_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_list(const Tag& tag)
   {
      return cmp::is<in, ERL_LIST_EXT, ERL_STRING_EXT, ERL_NIL_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_map(const Tag& tag)
   {
      return cmp::is<in, ERL_MAP_EXT>(tag);
   }

   template <typename Tag>
   [[nodiscard]] constexpr bool is_binary(const Tag& tag)
   {
      return cmp::is<in, ERL_BINARY_EXT>(tag);
   }

} // namespace glz::eetf
