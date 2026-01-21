// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// TODO: Use std::source_location when deprecating clang 14
// #include <source_location>
#include <array>
#include <string>
#include <string_view>

#include "glaze/core/feature_test.hpp"
#include "glaze/reflection/to_tuple.hpp"
#include "glaze/util/string_literal.hpp"

#if defined(__clang__) || defined(__GNUC__)
#define GLZ_PRETTY_FUNCTION __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
#define GLZ_PRETTY_FUNCTION __FUNCSIG__
#endif

// For struct fields
namespace glz::detail
{
   // Do not const qualify this value to avoid duplicate `to_tie` template instantiations with rest of Glaze
   // Temporary fix: const qualify it despite. Caused issues with reflect begin/end indices
   // See https://github.com/stephenberry/glaze/issues/1568
   template <class T>
   extern const T external;

   // using const char* simplifies the complier's output and should improve compile times
   template <auto Ptr>
   [[nodiscard]] consteval auto mangled_name()
   {
      // return std::source_location::current().function_name();
      return GLZ_PRETTY_FUNCTION;
   }

   template <class T>
   [[nodiscard]] consteval auto mangled_name()
   {
      // return std::source_location::current().function_name();
      return GLZ_PRETTY_FUNCTION;
   }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
   template <auto N, class T>
   constexpr std::string_view get_name_impl = mangled_name<get_ptr<N>(external<std::remove_volatile_t<T>>)>();
#pragma clang diagnostic pop
#elif __GNUC__
   template <auto N, class T>
   constexpr std::string_view get_name_impl = mangled_name<get_ptr<N>(external<std::remove_volatile_t<T>>)>();
#else
   template <auto N, class T>
   constexpr std::string_view get_name_impl = mangled_name<get_ptr<N>(external<std::remove_volatile_t<T>>)>();
#endif

   struct GLAZE_REFLECTOR
   {
      int GLAZE_FIELD;
   };

   struct reflect_field
   {
      static constexpr auto name = get_name_impl<0, GLAZE_REFLECTOR>;
      static constexpr auto end = name.substr(name.find("GLAZE_FIELD") + sizeof("GLAZE_FIELD") - 1);
      static constexpr auto begin = name[name.find("GLAZE_FIELD") - 1];
   };

   struct reflect_type
   {
      static constexpr std::string_view name = mangled_name<GLAZE_REFLECTOR>();
      static constexpr auto end = name.substr(name.find("GLAZE_REFLECTOR") + sizeof("GLAZE_REFLECTOR") - 1);
#if defined(__GNUC__) || defined(__clang__)
      static constexpr auto begin = std::string_view{"T = "};
#else
      static constexpr auto begin = std::string_view{"glz::detail::mangled_name<"};
#endif
   };
}

namespace glz
{
   template <auto N, class T>
   struct member_nameof_impl
   {
      static constexpr auto name = detail::get_name_impl<N, T>;
      static constexpr auto begin = name.find(detail::reflect_field::end);
      static constexpr auto tmp = name.substr(0, begin);
      static constexpr auto stripped = tmp.substr(tmp.find_last_of(detail::reflect_field::begin) + 1);
      // making static memory to stripped to help the compiler optimize away prettified function signature
      static constexpr std::string_view stripped_literal = join_v<stripped>;
   };

   template <auto N, class T>
   inline constexpr auto member_nameof = []() constexpr { return member_nameof_impl<N, T>::stripped_literal; }();

   template <class T>
   constexpr auto type_name = [] {
      constexpr std::string_view name = detail::mangled_name<T>();
      constexpr auto begin = name.find(detail::reflect_type::end);
      constexpr auto tmp = name.substr(0, begin);
#if defined(__GNUC__) || defined(__clang__)
      return tmp.substr(tmp.rfind(detail::reflect_type::begin) + detail::reflect_type::begin.size());
#else
      constexpr auto name_with_keyword =
         tmp.substr(tmp.rfind(detail::reflect_type::begin) + detail::reflect_type::begin.size());
      return name_with_keyword.substr(name_with_keyword.find(' ') + 1);
#endif
   }();

   template <class T, size_t... I>
   [[nodiscard]] constexpr auto member_names_impl(std::index_sequence<I...>)
   {
      if constexpr (sizeof...(I) == 0) {
         return std::array<sv, 0>{};
      }
      else {
         return std::array{member_nameof<I, T>...};
      }
   }

   template <class T>
   struct meta;

   template <std::pair V>
   struct make_static
   {
      static constexpr auto value = V;
   };

#if GLZ_HAS_CONSTEXPR_STRING
   // Concept for when rename_key returns exactly std::string (allocates)
   // Requires constexpr std::string support (not available with _GLIBCXX_USE_CXX11_ABI=0)
   template <class T>
   concept meta_has_rename_key_string = requires(T t, const std::string_view s) {
      { glz::meta<std::remove_cvref_t<T>>::rename_key(s) } -> std::same_as<std::string>;
   };

   // Helper to compute renamed key size at compile time
   // Using consteval forces evaluation before template instantiation
   // This unified approach works for both GCC and Clang (including Clang 19+)
   template <class T, size_t I>
   consteval size_t renamed_key_size()
   {
      return meta<std::remove_cvref_t<T>>::rename_key(member_nameof<I, T>).size();
   }

   // Storage for renamed key with exact size determined at compile time
   template <class T, size_t I, size_t N = renamed_key_size<T, I>()>
   struct renamed_key_storage
   {
      static constexpr auto value = [] {
         std::array<char, N + 1> arr{};
         auto str = meta<std::remove_cvref_t<T>>::rename_key(member_nameof<I, T>);
         for (size_t i = 0; i < N; ++i) {
            arr[i] = str[i];
         }
         arr[N] = '\0';
         return arr;
      }();
   };

   template <meta_has_rename_key_string T, size_t... I>
   [[nodiscard]] constexpr auto member_names_impl(std::index_sequence<I...>)
   {
      if constexpr (sizeof...(I) == 0) {
         return std::array<sv, 0>{};
      }
      else {
         return std::array<sv, sizeof...(I)>{
            sv{renamed_key_storage<T, I>::value.data(), renamed_key_storage<T, I>::value.size() - 1}...};
      }
   }
#else
   // When constexpr std::string is not available, this concept is always false
   template <class T>
   concept meta_has_rename_key_string = false;
#endif

   // Concept for when rename_key returns anything convertible to std::string_view EXCEPT std::string (non-allocating)
   template <class T>
   concept meta_has_rename_key_convertible = requires(T t, const std::string_view s) {
      { glz::meta<std::remove_cvref_t<T>>::rename_key(s) } -> std::convertible_to<std::string_view>;
   } && !meta_has_rename_key_string<T>;

   template <meta_has_rename_key_convertible T, size_t... I>
   [[nodiscard]] constexpr auto member_names_impl(std::index_sequence<I...>)
   {
      if constexpr (sizeof...(I) == 0) {
         return std::array<sv, 0>{};
      }
      else {
         return std::array{glz::meta<std::remove_cvref_t<T>>::rename_key(member_nameof<I, T>)...};
      }
   }

   // Concept for indexed rename_key that provides type information
   template <class T>
   concept meta_has_rename_key_indexed = requires {
      { glz::meta<std::remove_cvref_t<T>>::template rename_key<0>() } -> std::convertible_to<std::string_view>;
   } && !meta_has_rename_key_string<T> && !meta_has_rename_key_convertible<T>;

   template <meta_has_rename_key_indexed T, size_t... I>
   [[nodiscard]] constexpr auto member_names_impl(std::index_sequence<I...>)
   {
      if constexpr (sizeof...(I) == 0) {
         return std::array<sv, 0>{};
      }
      else {
         return std::array<sv, sizeof...(I)>{sv{glz::meta<std::remove_cvref_t<T>>::template rename_key<I>()}...};
      }
   }

   template <class T>
   inline constexpr auto member_names =
      [] { return member_names_impl<T>(std::make_index_sequence<detail::count_members<T>>{}); }();

   // Forward declaration of refl_t for member type extraction
   // The actual definition is in glaze/core/reflect.hpp
   // This allows users to get the type of a member at a given index in rename_key
   namespace detail
   {
      template <class T, size_t I>
      struct member_type_at_index
      {
         using tie_type = decltype(to_tie(std::declval<std::remove_cvref_t<T>&>()));
         using type = std::remove_cvref_t<tuple_element_t<I, tie_type>>;
      };
   }

   // Helper to get the type of a member at a given index
   // Usage in rename_key: using member_type = glz::member_type_t<T, Index>;
   template <class T, size_t Index>
   using member_type_t = typename detail::member_type_at_index<T, Index>::type;
}

// For member object pointers
namespace glz
{
   template <class T>
   struct remove_member_pointer
   {
      using type = T;
   };

   template <class C, class T>
   struct remove_member_pointer<T C::*>
   {
      using type = C;
   };

   template <class C, class R, class... Args>
   struct remove_member_pointer<R (C::*)(Args...)>
   {
      using type = C;
   };

   template <class T, auto P>
   consteval std::string_view get_name_msvc()
   {
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.find("->") + 2);
      return str.substr(0, str.find(">"));
   }

   template <class T, auto P>
   consteval std::string_view func_name_msvc()
   {
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.rfind(type_name<T>) + type_name<T>.size());
      str = str.substr(str.find("::") + 2);
      return str.substr(0, str.find("("));
   }

#if defined(__clang__)
   inline constexpr auto pretty_function_tail = "]";
#elif defined(__GNUC__) || defined(__GNUG__)
   inline constexpr auto pretty_function_tail = ";";
#elif defined(_MSC_VER)
#endif

   template <auto P>
      requires(std::is_member_pointer_v<decltype(P)>)
   consteval std::string_view get_name()
   {
#if defined(_MSC_VER) && !defined(__clang__)
      if constexpr (std::is_member_object_pointer_v<decltype(P)>) {
         using T = remove_member_pointer<std::decay_t<decltype(P)>>::type;
         constexpr auto p = P;
         return get_name_msvc<T, &(detail::external<T>.*p)>();
      }
      else {
         using T = remove_member_pointer<std::decay_t<decltype(P)>>::type;
         return func_name_msvc<T, P>();
      }
#else
      // TODO: Use std::source_location when deprecating clang 14
      // std::string_view str = std::source_location::current().function_name();
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.find("&") + 1);
      str = str.substr(0, str.find(pretty_function_tail));
      return str.substr(str.rfind("::") + 2);
#endif
   }

   template <auto E>
      requires(std::is_enum_v<decltype(E)> && std::is_scoped_enum_v<decltype(E)>)
   consteval auto get_name()
   {
#if defined(_MSC_VER) && !defined(__clang__)
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.rfind("::") + 2);
      return str.substr(0, str.find('>'));
#else
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.rfind("::") + 2);
      return str.substr(0, str.find(']'));
#endif
   }

   template <auto E>
      requires(std::is_enum_v<decltype(E)> && not std::is_scoped_enum_v<decltype(E)>)
   consteval auto get_name()
   {
#if defined(_MSC_VER) && !defined(__clang__)
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.rfind("= ") + 2);
      return str.substr(0, str.find('>'));
#else
      std::string_view str = GLZ_PRETTY_FUNCTION;
      str = str.substr(str.rfind("= ") + 2);
      return str.substr(0, str.find(']'));
#endif
   }
}
