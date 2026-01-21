// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <concepts>
#include <expected>
#include <utility>
#include <version>

namespace glz
{
   template <class Expected, class Unexpected>
   using expected = std::expected<Expected, Unexpected>;

   template <class T>
   concept is_expected =
      std::same_as<std::remove_cvref_t<T>, expected<typename T::value_type, typename T::error_type> >;

#ifdef __clang__
   template <class Unexpected>
   struct unexpected : public std::unexpected<Unexpected>
   {
      using std::unexpected<Unexpected>::unexpected;
   };
   template <class Unexpected>
   unexpected(Unexpected) -> unexpected<Unexpected>;
#else
   template <class Unexpected>
   using unexpected = std::unexpected<Unexpected>;
#endif
}