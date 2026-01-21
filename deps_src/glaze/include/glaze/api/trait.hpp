// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>

#include "glaze/api/hash.hpp"
#include "glaze/core/common.hpp"
#include "glaze/core/meta.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/util/string_literal.hpp"

namespace glz
{
   template <class T>
   struct trait
   {
      using sv = std::string_view;
      static constexpr sv type_name_unhashed = name_v<T>;
      static constexpr sv type_name_hash = hash128_v<type_name_unhashed>; // must hash for consistent length

      static constexpr sv type_size_hash = hash128_v<int_to_sv_v<size_t, sizeof(T)>>; // must hash for consistent length

      static constexpr sv major_version = hash128_i_v<version_v<T>.major>; // must hash for consistent length
      static constexpr sv minor_version = hash128_i_v<version_v<T>.minor>; // must hash for consistent length
      static constexpr sv revision = hash128_i_v<version_v<T>.patch>; // must hash for consistent length

#define std_trait(x) static constexpr sv x = to_sv<std::x##_v<T>>()
      std_trait(is_trivial);
      std_trait(is_standard_layout);

      std_trait(is_default_constructible);
      std_trait(is_trivially_default_constructible);
      std_trait(is_nothrow_default_constructible);

      std_trait(is_trivially_copyable);

      std_trait(is_move_constructible);
      std_trait(is_trivially_move_constructible);
      std_trait(is_nothrow_move_constructible);

      std_trait(is_destructible);
      std_trait(is_trivially_destructible);
      std_trait(is_nothrow_destructible);

      std_trait(has_unique_object_representations);

      std_trait(is_polymorphic);
      std_trait(has_virtual_destructor);
      std_trait(is_aggregate);
#undef std_trait

#ifdef __clang__
      static constexpr sv clang = "clang";
#endif
#ifdef __GNUC__
      static constexpr sv gnuc = "gnuc";
#endif
#ifdef _MSC_VER
      static constexpr sv msvc = "msvc";
#endif

      static constexpr sv blank = ""; // to end possible macros

      static constexpr sv members = [] {
         if constexpr (glaze_object_t<T> || reflectable<T>) {
            return glz::name_v<detail::member_tuple_t<T>>;
         }
         else {
            return "";
         }
      }();

      static constexpr sv to_hash =
         join_v<type_name_hash,

                type_size_hash,

                major_version, minor_version, revision,

                is_trivial, is_standard_layout,

                is_default_constructible, is_trivially_default_constructible, is_nothrow_default_constructible,

                is_trivially_copyable,

                is_move_constructible, is_trivially_move_constructible, is_nothrow_move_constructible,

                is_destructible, is_trivially_destructible, is_nothrow_destructible,

                has_unique_object_representations,

                is_polymorphic, has_virtual_destructor, is_aggregate,

#ifdef __clang__
                clang,
#endif
#ifdef __GNUC__
                gnuc,
#endif
#ifdef _MSC_VER
                msvc,
#endif
                blank,

                members>;

     private:
      static constexpr sv v = "v";
      static constexpr sv comma = ",";

     public:
      static constexpr sv version_sv = join_v<v, major_version, comma, minor_version, comma, revision>;
      static constexpr version_t version = ::glz::version_v<T>;

      static constexpr sv hash = hash128_v<to_hash>;
   };

   namespace detail
   {
      template <std::unsigned_integral T, size_t N>
      constexpr std::array<T, N> uint_array_from_sv(sv str)
      {
         std::array<T, N> res{};
         constexpr auto bytes = sizeof(T);
         for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < bytes; ++j) {
               res[i] |= T(str[i * bytes + j]) << (8 * j);
            }
         }
         return res;
      }
   }

   using hash_t = std::array<uint64_t, 2>;
   template <class T>
   consteval hash_t hash()
   {
      return detail::uint_array_from_sv<uint64_t, 2>(trait<T>::hash);
   }
}
