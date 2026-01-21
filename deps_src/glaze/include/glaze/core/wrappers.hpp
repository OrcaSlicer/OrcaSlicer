// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <type_traits>

#include "glaze/core/opts.hpp"

namespace glz
{
   // treat a value as quoted to avoid double parsing into a value
   template <class T>
   struct quoted_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class T>
   struct escape_bytes_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class T>
   escape_bytes_t(T&) -> escape_bytes_t<T>;

   template <class T, auto OptsMemPtr>
   struct opts_wrapper_t
   {
      static constexpr bool glaze_wrapper = true;
      static constexpr auto glaze_reflect = false;
      static constexpr auto opts_member = OptsMemPtr;
      using value_type = T;
      T& val;
   };

   template <class T>
   concept is_opts_wrapper = requires {
      requires T::glaze_wrapper == true;
      requires T::glaze_reflect == false;
      T::opts_member;
      typename T::value_type;
      requires std::is_lvalue_reference_v<decltype(T::val)>;
   };

   template <auto MemPtr, auto OptsMemPtr>
   inline constexpr decltype(auto) opts_wrapper() noexcept
   {
      return [](auto&& val) {
         using V = std::remove_reference_t<decltype(val.*MemPtr)>;
         return opts_wrapper_t<V, OptsMemPtr>{val.*MemPtr};
      };
   }

   // custom_t allows a user to register member functions, std::function members, and member variables
   // to implement custom reading and writing
   template <class T, class From, class To>
   struct custom_t final
   {
      static constexpr auto glaze_reflect = false;
      using from_t = From;
      using to_t = To;
      T& val;
      From from;
      To to;
   };

   template <class T, class From, class To>
   custom_t(T&, From, To) -> custom_t<T, From, To>;

   template <auto From, auto To>
   constexpr auto custom_impl() noexcept
   {
      return [](auto&& v) { return custom_t{v, From, To}; };
   }

   // When reading into an array that is appendable, the new data will be appended rather than overwrite
   template <auto MemPtr>
   constexpr auto append_arrays = opts_wrapper<MemPtr, append_arrays_opt_tag{}>();

   // Read and write booleans as numbers
   template <auto MemPtr>
   constexpr auto bools_as_numbers = opts_wrapper<MemPtr, bools_as_numbers_opt_tag{}>();

   // Read and write numbers as strings
   template <auto MemPtr>
   constexpr auto quoted_num = opts_wrapper<MemPtr, quoted_num_opt_tag{}>();

   // Treat types like std::string as numbers: read and write them without quotes
   template <auto MemPtr>
   constexpr auto string_as_number = opts_wrapper<MemPtr, string_as_number_opt_tag{}>();

   // Deprecated: use string_as_number instead
   template <auto MemPtr>
   [[deprecated("Use glz::string_as_number instead of glz::number")]]
   constexpr auto number = opts_wrapper<MemPtr, string_as_number_opt_tag{}>();

   // Write out string like types without quotes
   template <auto MemPtr>
   constexpr auto unquoted = opts_wrapper<MemPtr, unquoted_opt_tag{}>();

   // Deprecated: use unquoted instead
   template <auto MemPtr>
   [[deprecated("Use glz::unquoted instead of glz::raw")]]
   constexpr auto raw = opts_wrapper<MemPtr, unquoted_opt_tag{}>();

   // Reads into only existing fields and elements and then exits without parsing the rest of the input
   template <auto MemPtr>
   constexpr auto partial_read = opts_wrapper<MemPtr, &opts::partial_read>();

   // Customize reading and writing
   template <auto From, auto To>
   constexpr auto custom = custom_impl<From, To>();

   template <auto MemPtr>
   inline constexpr decltype(auto) escape_bytes_impl() noexcept
   {
      return [](auto&& val) { return escape_bytes_t<std::remove_reference_t<decltype(val.*MemPtr)>>{val.*MemPtr}; };
   }

   // Treat char arrays as byte sequences to be fully escaped
   template <auto MemPtr>
   constexpr auto escape_bytes = escape_bytes_impl<MemPtr>();

   // Limit string length or array size when reading
   // For strings: limits max_string_length
   // For arrays/vectors: limits max_array_size
   template <class T, size_t MaxLen>
   struct max_length_t
   {
      static constexpr bool glaze_wrapper = true;
      static constexpr auto glaze_reflect = false;
      static constexpr size_t max_len = MaxLen;
      using value_type = T;
      T& val;
   };

   template <auto MemPtr, size_t MaxLen>
   inline constexpr decltype(auto) max_length_impl() noexcept
   {
      return [](auto&& val) {
         using V = std::remove_reference_t<decltype(val.*MemPtr)>;
         return max_length_t<V, MaxLen>{val.*MemPtr};
      };
   }

   // Limit string length or array size when reading from binary formats
   // Usage: glz::max_length<&T::field, 100>
   template <auto MemPtr, size_t MaxLen>
   constexpr auto max_length = max_length_impl<MemPtr, MaxLen>();
}
