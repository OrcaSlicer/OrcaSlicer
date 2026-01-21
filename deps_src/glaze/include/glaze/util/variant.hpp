// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <variant>

#include "glaze/util/type_traits.hpp"

namespace glz
{
   template <class T>
   concept is_variant = is_specialization_v<T, std::variant>;

   // Check if all variant alternatives are default constructible
   template <class T>
   concept variant_alternatives_default_constructible = []<size_t... I>(std::index_sequence<I...>) {
      return (std::is_default_constructible_v<std::variant_alternative_t<I, T>> && ...);
   }(std::make_index_sequence<std::variant_size_v<T>>{});

   // Emplace a variant alternative at a runtime-determined index
   // This works with move-only types like std::unique_ptr by using emplace instead of assignment
   // Requires all variant alternatives to be default constructible
   template <is_variant T>
      requires variant_alternatives_default_constructible<T>
   GLZ_ALWAYS_INLINE void emplace_runtime_variant(T& variant, size_t index)
   {
      constexpr auto N = std::variant_size_v<T>;
      [&]<size_t... I>(std::index_sequence<I...>) {
         // Use a fold expression to generate if-else chain at compile time
         ((I == index ? (variant.template emplace<I>(), void()) : void()), ...);
      }(std::make_index_sequence<N>{});
   }
}
