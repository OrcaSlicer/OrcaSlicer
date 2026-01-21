// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "glaze/concepts/container_concepts.hpp"
#include "glaze/core/array_apply.hpp"
#include "glaze/core/cast.hpp"
#include "glaze/core/context.hpp"
#include "glaze/core/error_category.hpp"
#include "glaze/core/feature_test.hpp"
#include "glaze/core/meta.hpp"
#include "glaze/util/bit_array.hpp"
#include "glaze/util/expected.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/help.hpp"
#include "glaze/util/string_literal.hpp"
#include "glaze/util/tuple.hpp"
#include "glaze/util/type_traits.hpp"
#include "glaze/util/utility.hpp"
#include "glaze/util/validate.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   // write out a string like type without quoting it
   template <class T>
   struct raw_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class... T>
   struct obj final
   {
      glz::tuple<std::conditional_t<std::is_convertible_v<std::decay_t<T>, sv>, sv, T>...> value;
      static constexpr auto glaze_reflect = false;
   };

   template <class... T>
   obj(T&&...) -> obj<T...>;

   template <class... T>
   struct obj_copy final
   {
      glz::tuple<T...> value;
      static constexpr auto glaze_reflect = false;
   };

   template <class... T>
   obj_copy(T...) -> obj_copy<T...>;

   template <class... T>
   struct arr final
   {
      glz::tuple<std::conditional_t<std::is_convertible_v<std::decay_t<T>, sv>, sv, T>...> value;
      static constexpr auto glaze_reflect = false;
   };

   template <class... T>
   arr(T&&...) -> arr<T...>;

   template <class... T>
   struct arr_copy final
   {
      glz::tuple<T...> value;
   };

   template <class... T>
   arr_copy(T...) -> arr_copy<T...>;

   // used to merge multiple JSON objects into a single JSON object
   template <class... T>
   struct merge final
   {
      glz::tuple<std::conditional_t<std::is_convertible_v<std::decay_t<T>, sv>, sv, T>...> value;
      static constexpr auto glaze_reflect = false;
   };

   template <class... T>
   merge(T&&...) -> merge<T...>;

   template <class... T>
   struct overload : T...
   {
      using T::operator()...;
   };

   struct hidden
   {};

   // hide class is a wrapper to denote that the exposed variable should be excluded or hidden for serialization output
   template <class T>
   struct hide final
   {
      T value;

      constexpr decltype(auto) operator()(auto&&) const { return hidden{}; }
   };

   template <class T>
   hide(T) -> hide<T>;

   struct skip
   {}; // to skip a keyed value in input

   template <class T>
   struct includer
   {
      T& value;

      static constexpr auto glaze_includer = true;
      static constexpr auto glaze_reflect = false;
   };

   template <class T>
   struct meta<includer<T>>
   {
      static constexpr std::string_view name = join_v<chars<"includer<">, name_v<T>, chars<">">>;
   };

   // Register this with an object to allow file including (direct writes) to the meta object
   struct file_include final
   {
      bool reflection_helper{}; // needed for count_members
      static constexpr auto glaze_includer = true;
      static constexpr auto glaze_reflect = false;

      constexpr decltype(auto) operator()(auto&& value) const noexcept
      {
         return includer<decay_keep_volatile_t<decltype(value)>>{value};
      }
   };

   template <class T>
   concept is_includer = requires(T t) { requires T::glaze_includer == true; };

   template <class T>
   concept is_member_function_pointer = std::is_member_function_pointer_v<T>;

   template <class T>
   using core_t = std::remove_cvref_t<T>;

   // Use std::stringview if you know the buffer is going to outlive this
   template <class string_type = std::string>
   struct basic_raw_json
   {
      string_type str;

      basic_raw_json() = default;

      template <class T>
         requires(!std::same_as<std::decay_t<T>, basic_raw_json> && std::constructible_from<string_type, T>)
      basic_raw_json(T&& s) : str(std::forward<T>(s))
      {}

      basic_raw_json(const basic_raw_json&) = default;
      basic_raw_json(basic_raw_json&&) = default;
      basic_raw_json& operator=(const basic_raw_json&) = default;
      basic_raw_json& operator=(basic_raw_json&&) = default;
   };

   using raw_json = basic_raw_json<std::string>;
   using raw_json_view = basic_raw_json<std::string_view>;

   template <class T>
   struct meta<basic_raw_json<T>>
   {
      static constexpr std::string_view name = "raw_json";
   };

   // glz::text (i.e. glz::basic_text<std::string>) just reads in the entire contents of the message or writes out
   // whatever contents it holds it avoids JSON reading/writing and is a pass through mechanism
   template <class string_type>
   struct basic_text
   {
      string_type str;

      basic_text() = default;

      template <class T>
         requires(!std::same_as<std::decay_t<T>, basic_text>)
      basic_text(T&& s) : str(std::forward<T>(s))
      {}

      basic_text(const basic_text&) = default;
      basic_text(basic_text&&) = default;
      basic_text& operator=(const basic_text&) = default;
      basic_text& operator=(basic_text&&) = default;
   };

   using text = basic_text<std::string>;
   using text_view = basic_text<std::string_view>;

   template <class T>
   concept constructible = requires { meta<std::decay_t<T>>::construct; } || local_construct_t<std::decay_t<T>>;

   template <class T>
   concept meta_value_t = glaze_t<std::decay_t<T>>;

   // Concept for byte-sized string views (std::string_view, std::u8string_view)
   // Excludes wide string views (wstring_view, u16string_view, u32string_view)
   template <class T>
   concept string_view_t =
      is_specialization_v<std::decay_t<T>, std::basic_string_view> && sizeof(typename std::decay_t<T>::value_type) == 1;

   // Concept for byte-sized basic_string types (std::string, std::u8string)
   // Excludes wide strings (wstring, u16string, u32string)
   template <class T>
   concept basic_string_t =
      is_specialization_v<std::decay_t<T>, std::basic_string> && sizeof(typename std::decay_t<T>::value_type) == 1;

   template <class T>
   concept array_char_t =
      requires { std::tuple_size<T>::value; } && std::same_as<T, std::array<char, std::tuple_size_v<T>>>;

   // Concept for char8_t-based string types (std::u8string, std::u8string_view)
   template <class T>
   concept u8str_t =
      (basic_string_t<T> || string_view_t<T>) && std::same_as<typename std::decay_t<T>::value_type, char8_t>;

   template <class T>
   concept str_t = (!std::same_as<std::nullptr_t, T> && std::constructible_from<std::string_view, std::decay_t<T>>) ||
                   array_char_t<T> || u8str_t<T>;

   template <class T>
   concept is_static_string =
      requires { meta<std::decay_t<T>>::glaze_static_string == true; } || std::decay_t<T>::glaze_static_string == true;

   // this concept requires that T is a writeable string. It can be resized, appended to, or assigned to
   template <class T>
   concept string_t = str_t<T> && !string_view_t<T> &&
                      (has_assign<T> || (resizable<T> && has_data<T>) || has_append<T>) && !is_static_string<T>;

   // static string; very like `string_t`, but with a fixed max capacity
   template <class T>
   concept static_string_t = str_t<T> && !string_view_t<T> &&
                             (has_assign<T> || (resizable<T> && has_data<T>) || has_append<T>) && is_static_string<T>;

   template <class T>
   concept char_array_t = str_t<T> && std::is_array_v<std::remove_pointer_t<std::remove_reference_t<T>>>;

   // Concept: does T's mimic type satisfy str_t?
   // This allows checking if a custom type mimics string behavior.
   // Prevents double-quoting when used as a map key.
   template <class T>
   concept mimics_str_t = has_mimic<T> && str_t<mimic_type<T>>;

   // Concept: does T's mimic type satisfy num_t?
   // This allows checking if a custom type mimics numeric behavior.
   // Enables proper variant type deduction for custom numeric types.
   template <class T>
   concept mimics_num_t = has_mimic<T> && num_t<mimic_type<T>>;

   // Concept: does T's mimic type satisfy bool_t?
   // This allows checking if a custom type mimics boolean behavior.
   template <class T>
   concept mimics_bool_t = has_mimic<T> && bool_t<mimic_type<T>>;

   // Note: custom_num_t, custom_str_t, custom_bool_t concepts are defined in core/custom_meta.hpp
   // (requires custom_t from wrappers.hpp which is not included here)

   template <class T>
   concept readable_map_t = !custom_read<T> && !meta_value_t<T> && !str_t<T> && range<T> && pair_t<range_value_t<T>> &&
                            map_subscriptable<std::decay_t<T>>;

   template <class T>
   concept writable_map_t = !custom_write<T> && !meta_value_t<T> && !str_t<T> && range<T> && pair_t<range_value_t<T>> &&
                            map_subscriptable<std::decay_t<T>>;

   template <class Map>
   concept heterogeneous_map = requires {
      typename Map::key_compare;
      requires(std::same_as<typename Map::key_compare, std::less<>> ||
               std::same_as<typename Map::key_compare, std::greater<>> ||
               requires { typename Map::key_compare::is_transparent; });
   };

   template <class T>
   concept array_t = (!meta_value_t<T> && !str_t<T> && !(readable_map_t<T> || writable_map_t<T>) && range<T>);

   template <class T>
   concept readable_array_t =
      (range<T> && !custom_read<T> && !meta_value_t<T> && !str_t<T> && !readable_map_t<T> && !filesystem_path<T>);

   template <class T>
   concept writable_array_t =
      (range<T> && !custom_write<T> && !meta_value_t<T> && !str_t<T> && !writable_map_t<T> && !filesystem_path<T>);

   template <class T>
   concept fixed_array_value_t =
      array_t<std::decay_t<decltype(std::declval<T>()[0])>> && !resizable<std::decay_t<decltype(std::declval<T>()[0])>>;

   template <class T>
   concept boolean_like = std::same_as<std::remove_cvref_t<T>, bool> || std::same_as<T, std::vector<bool>::reference> ||
                          std::same_as<T, std::vector<bool>::const_reference>;

   template <class T>
   concept is_no_reflect = requires(T t) { requires std::remove_cvref_t<T>::glaze_reflect == false; };

   /// \brief check if container has fixed size and its subsequent T::value_type
   template <class T>
   concept has_static_size =
      (is_span<T> && !is_dynamic_span<T>) || (
                                                requires(T container) {
                                                   {
                                                      std::bool_constant<(std::decay_t<T>{}.size(), true)>()
                                                   } -> std::same_as<std::true_type>;
                                                } && std::decay_t<T>{}.size() > 0 &&
                                                requires {
                                                   typename T::value_type;
                                                   requires std::is_trivially_copyable_v<typename T::value_type>;
                                                });
   static_assert(has_static_size<std::array<int, 2>>);
   static_assert(!has_static_size<std::array<std::string, 2>>);

   template <class T>
   constexpr bool is_std_array = false;
   template <class T, std::size_t N>
   constexpr bool is_std_array<std::array<T, N>> = true;

   template <class T>
   concept has_fixed_size_container = std::is_array_v<T> || is_std_array<T>;
   static_assert(has_fixed_size_container<std::array<std::string, 2>>);
   static_assert(has_fixed_size_container<int[54]>);

   template <class T>
   constexpr size_t get_size() noexcept
   {
      if constexpr (is_span<T>) {
         return T::extent;
      }
      else if constexpr (std::is_array_v<T>) {
         return std::extent_v<T>;
      }
      else if constexpr (is_std_array<T>) {
         return glz::tuple_size_v<T>;
      }
      else {
         return std::decay_t<T>{}.size();
      }
   }

   template <class T>
   concept is_reference_wrapper = is_specialization_v<T, std::reference_wrapper>;

   template <class T>
   concept tuple_t = requires(T t) {
      glz::tuple_size<T>::value;
      glz::get<0>(t);
   } && !meta_value_t<T> && !range<T>;

   template <class T>
   concept glaze_wrapper = requires { requires T::glaze_wrapper == true; };

   template <class T>
   concept always_null_t =
      std::same_as<T, std::nullptr_t> || std::convertible_to<T, std::monostate> || std::same_as<T, std::nullopt_t>;

   template <class T>
   concept always_skipped = is_includer<T> || std::same_as<T, hidden> || std::same_as<T, skip>;

   template <class T>
   concept nullable_t = !meta_value_t<T> && !str_t<T> && requires(T t) {
      bool(t);
      { *t };
   };

   // Detect function pointers and function references (which should not be treated as nullable)
   template <class T>
   concept is_function_ptr_or_ref =
      std::is_function_v<std::remove_cvref_t<T>> || // function reference: void(&)(int)
      (std::is_pointer_v<std::remove_cvref_t<T>> &&
       std::is_function_v<std::remove_pointer_t<std::remove_cvref_t<T>>>); // function pointer: void(*)(int)

   // Combined concept for any function pointer type (member or non-member)
   template <class T>
   concept is_any_function_ptr = is_member_function_pointer<T> || is_function_ptr_or_ref<T>;

   template <class T>
   concept nullable_like = nullable_t<T> && !is_expected<T> && !std::is_array_v<T> && !is_function_ptr_or_ref<T>;

   // For optional like types that cannot overload `operator bool()`
   template <class T>
   concept nullable_value_t = !meta_value_t<T> && requires(T t) {
      t.value();
      { t.has_value() } -> std::convertible_to<bool>;
   };

   template <class T>
   concept nullable_wrapper = glaze_wrapper<T> && nullable_t<typename T::value_type>;

   template <class T>
   concept null_t = nullable_t<T> || nullable_value_t<T> || always_null_t<T> || nullable_wrapper<T>;

   template <class T>
   concept func_t = requires(T t) {
      typename T::result_type;
      std::function(t);
   } && !glaze_t<T>;

   template <class T>
   concept glaze_array_t = glaze_t<T> && is_specialization_v<meta_wrapper_t<T>, detail::Array>;

   template <class T>
   concept glaze_object_t = glaze_t<T> && (is_specialization_v<meta_wrapper_t<T>, detail::Object> ||
                                           (not std::is_enum_v<std::decay_t<T>> && meta_keys<T>));

   template <class T>
   concept glaze_enum_t = glaze_t<T> && is_specialization_v<meta_wrapper_t<T>, detail::Enum>;

   template <class T>
   concept is_named_enum = ((glaze_enum_t<T> || (meta_keys<T> && std::is_enum_v<T>)) && !custom_read<T>);

   template <class T>
   concept glaze_flags_t = glaze_t<T> && is_specialization_v<meta_wrapper_t<T>, detail::Flags>;

   template <class T>
   concept glaze_value_t =
      glaze_t<T> && !(glaze_array_t<T> || glaze_object_t<T> || glaze_enum_t<T> || meta_keys<T> || glaze_flags_t<T>);

   template <class T>
   concept reflectable = std::is_aggregate_v<std::remove_cvref_t<T>> && std::is_class_v<std::remove_cvref_t<T>> &&
                         !(is_no_reflect<T> || glaze_value_t<T> || glaze_object_t<T> || glaze_array_t<T> ||
                           glaze_flags_t<T> || range<T> || pair_t<T> || null_t<T> || meta_keys<T>);

   template <class T>
   concept is_memory_object = is_memory_type<T> && (glaze_object_t<memory_type<T>> || reflectable<memory_type<T>>);

   template <class T>
   concept glaze_const_value_t = glaze_value_t<T> && std::is_pointer_v<glz::meta_wrapper_t<T>> &&
                                 std::is_const_v<std::remove_pointer_t<glz::meta_wrapper_t<T>>>;

   template <class From, class To>
   concept non_narrowing_convertable = requires(From from, To to) {
#if __GNUC__
      // TODO: guard gcc against narrowing conversions when fixed
      to = from;
#else
      To{from};
#endif
   };

   /**
    * @brief Extracts the underlying member from a struct.
    *
    * Core Glaze function to extract the underlying member. Typically, the `value` parameter
    * is the struct containing the member field. The `element` parameter denotes a member object
    * pointer or an invocable function, which allows member extraction from the struct.
    *
    * @tparam Value   The type of the value, usually a struct containing the member.
    * @tparam Element The type of the element, either a member pointer, an invocable function, or reference/pointer.
    * @param value    The struct from which to extract the member.
    * @param element  The member pointer or invocable function used to extract the member.
    * @return         The extracted member.
    */
   template <class Value, class Element>
   GLZ_ALWAYS_INLINE decltype(auto) get_member(Value&& value, Element&& element)
   {
      using V = std::decay_t<decltype(element)>;
      if constexpr (std::is_member_object_pointer_v<V>) {
         return value.*element;
      }
      else if constexpr (std::is_member_function_pointer_v<V>) {
         return element;
      }
      else if constexpr (std::invocable<Element, Value> && not eigen_t<Element>) {
         // Eigen places a static_assert inside of the operator()(), so we must target
         // matrix types and reject them from the invocable check
         // Eigen ought to put the check in the `enable_if` for operator()()
         return std::invoke(std::forward<Element>(element), std::forward<Value>(value));
      }
      else if constexpr (std::is_pointer_v<V> && !std::is_reference_v<Element>) {
         if constexpr (std::invocable<decltype(*element), Value>) {
            return std::invoke(*element, std::forward<Value>(value));
         }
         else {
            return *element;
         }
      }
      else {
         return element;
      }
   }

   /**
    * @brief Alias for the expected return type of `get_member`.
    *
    * This template alias deduces the return type of the `get_member` function when called with
    * the specified `Value` and `Element` types.
    *
    * @tparam Value   The type of the value parameter, typically a struct containing the member.
    * @tparam Element The type of the element, either a member pointer, an invocable function, or reference/pointer.
    *
    * @see get_member
    */
   template <class Value, class Element>
   using member_t = decltype(get_member(std::declval<std::add_lvalue_reference_t<Value>>(), std::declval<Element>()));

   // member_ptr and lambda wrapper helper
   template <template <class> class Wrapper, class Wrapped>
   struct wrap
   {
      Wrapped wrapped;
      constexpr decltype(auto) operator()(auto&& value) const
      {
         return Wrapper<std::decay_t<decltype(get_member(value, wrapped))>>{get_member(value, wrapped)};
      }

      constexpr decltype(auto) unwrap(auto&& value) const { return get_member(value, wrapped); }
   };

   // Output variants in the following format  ["variant_type", variant_json_data] with
   // glz::detail:array_variant(&T::var);
   template <is_variant T>
   struct array_variant_wrapper final
   {
      T& value;
   };
   // TODO: Could do this if the compiler supports alias template deduction
   // template <class T>
   // using array_var = wrap<array_var_wrapper, T>;
   template <class T>
   struct array_variant : wrap<array_variant_wrapper, T>
   {};
   template <class T>
   array_variant(T) -> array_variant<T>; // Only needed on older compilers until we move to template alias deduction

   template <class T>
   struct as_array_wrapper final
   {
      static constexpr auto glaze_reflect = false;
      T& value;
   };

   template <auto MemPtr>
   constexpr auto as_array_impl() noexcept
   {
      return [](auto&& value) {
         using Value = std::decay_t<decltype(value)>;
         using Member = std::decay_t<member_t<Value, decltype(MemPtr)>>;
         return as_array_wrapper<Member>{get_member(value, MemPtr)};
      };
   }

   template <auto MemPtr>
   inline constexpr auto as_array = as_array_impl<MemPtr>();

   constexpr decltype(auto) conv_sv(auto&& value) noexcept
   {
      using V = std::decay_t<decltype(value)>;
      if constexpr (std::is_convertible_v<V, sv>) {
         return sv{value};
      }
      else {
         return value;
      }
   }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif
   constexpr auto array(auto&&... args) noexcept { return detail::Array{glz::tuple{conv_sv(args)...}}; }

   template <class... Args>
   constexpr auto object(Args&&... args) noexcept
   {
      return detail::Object{tuple{std::forward<Args>(args)...}};
   }

   constexpr auto enumerate(auto&&... args) noexcept { return detail::Enum{tuple{args...}}; }

   constexpr auto flags(auto&&... args) noexcept { return detail::Flags{tuple{args...}}; }

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

namespace glz
{
   // This wraps glz::expected error (unexpected) values in an object with an "error" key
   // This makes them discernable from the expected value
   template <class T>
   struct unexpected_wrapper final
   {
      T* unexpected;

      struct glaze
      {
         using V = unexpected_wrapper;
         static constexpr auto value = glz::object("unexpected", &V::unexpected);
      };
   };

   template <class T>
   unexpected_wrapper(T*) -> unexpected_wrapper<T>;

   template <auto Opts, class Value>
   [[nodiscard]] GLZ_ALWAYS_INLINE constexpr bool skip_member(const Value& value) noexcept
   {
      if constexpr (null_t<Value> && Opts.skip_null_members) {
         if constexpr (always_null_t<Value>)
            return true;
         else {
            return !bool(value);
         }
      }
      else {
         return false;
      }
   }
}

namespace glz
{
   /// Determine whether the member with the given name is required for type T.
   ///
   /// A member is required, if the option error_on_missing_keys is set and the member is not null-able.
   /// This behaviour can be overridden on a per-type basis via the customization point
   /// `meta<T>::requires_key(key, is_nullable)`
   /// @code
   /// struct person {
   ///    std::string first_name{};
   ///    std::string last_name{};
   /// };
   ///
   /// template <>
   /// struct glz::meta<person> {
   ///    static constexpr bool requires_key(std::string_view key, bool nullable) {
   ///       if (not nullable) {
   ///          return true;
   ///       }
   ///       return false;
   ///    }
   /// };
   /// @endcode
   /// @tparam T      The user‑defined type for which metadata may be provided.
   /// @tparam Val_T  The value type associated with the key; used to determine nullability.
   /// @tparam Opts   A policy struct with a boolean member `error_on_missing_keys`.
   ///
   /// @param key          The string identifier whose requirement status is being queried.
   /// @returns `true` if:
   ///            - `meta<T>::requires_key(key, is_nullable)` exists and returns true, or
   ///            - `Opts.error_on_missing_keys` is true *and* Val_T is not nullable;
   ///          otherwise returns `false`.
   template <typename T, typename Val_T, auto Opts>
   constexpr bool requires_key(const std::string_view key)
   {
      if constexpr (meta_has_requires_key<T>) {
         if (meta<T>::requires_key(key, nullable_like<Val_T>)) return true;
      }
      else if constexpr (Opts.error_on_missing_keys && !nullable_like<Val_T>) {
         return true;
      }
      return false;
   }
}
