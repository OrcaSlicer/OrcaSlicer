// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/common.hpp"
#include "glaze/core/wrappers.hpp"

namespace glz
{
   // ============================================================================
   // Auto-inference of JSON type from custom read/write functions
   // ============================================================================
   // These traits allow automatic variant type deduction and schema generation
   // for types with glz::custom read/write functions by inspecting the input parameter type.
   //
   // NOTE: If a type has both `mimic` and `glz::custom`, `custom` takes precedence
   // for JSON type inference (consistent with runtime behavior where custom read/write is used).

   namespace detail
   {
      // Helper to check if invoking meta<T>::value with T& returns a custom_t
      template <class T, class = void>
      struct has_custom_meta_impl : std::false_type
      {};

      template <class T>
      struct has_custom_meta_impl<T, std::void_t<decltype(glz::meta<T>::value(std::declval<T&>()))>>
      {
         using result_type = decltype(glz::meta<T>::value(std::declval<T&>()));
         static constexpr bool value = is_specialization_v<result_type, custom_t>;
      };
   }

   template <class T>
   constexpr bool has_custom_meta_v = detail::has_custom_meta_impl<T>::value;

   namespace detail
   {
      // Extract the input type from a custom read function
      // The second parameter of the read lambda determines the JSON type
      template <class T, class = void>
      struct custom_read_input_type
      {
         static constexpr bool has_custom = false;
         using type = void;
      };

      template <class T>
      struct custom_read_input_type<
         T,
         std::enable_if_t<has_custom_meta_v<T> &&
                          is_invocable_concrete<typename decltype(glz::meta<T>::value(std::declval<T&>()))::from_t> &&
                          (glz::tuple_size_v<invocable_args_t<typename decltype(glz::meta<T>::value(
                              std::declval<T&>()))::from_t>> >= 2)>>
      {
         static constexpr bool has_custom = true;
         using CustomT = decltype(glz::meta<T>::value(std::declval<T&>()));
         using From = typename CustomT::from_t;
         using Args = invocable_args_t<From>;
         using type = std::decay_t<glz::tuple_element_t<1, Args>>;
      };
   }

   template <class T>
   using custom_read_input_t = typename detail::custom_read_input_type<T>::type;

   // Concept: type has custom read that takes a numeric input
   template <class T>
   concept custom_num_t = has_custom_meta_v<T> && num_t<custom_read_input_t<T>>;

   // Concept: type has custom read that takes a string input
   template <class T>
   concept custom_str_t = has_custom_meta_v<T> && str_t<custom_read_input_t<T>>;

   // Concept: type has custom read that takes a bool input
   template <class T>
   concept custom_bool_t = has_custom_meta_v<T> && bool_t<custom_read_input_t<T>>;

} // namespace glz
