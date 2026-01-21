// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>
#include <concepts>
#include <initializer_list>
#include <type_traits>
#include <utility>

#include "glaze/reflection/get_name.hpp"
#include "glaze/reflection/requires_key.hpp"
#include "glaze/reflection/to_tuple.hpp"
#include "glaze/tuplet/tuple.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/string_literal.hpp"
#include "glaze/util/type_traits.hpp"
#include "glaze/util/variant.hpp"
#include "glaze/version.hpp"

namespace glz
{
   enum struct operation { serialize, parse };

   // The meta_context provides compile time data about the serialization context.
   // This include whether we are serializing or parsing.
   struct meta_context
   {
      operation op = operation::serialize;
   };

   template <class T>
   struct meta;

   template <>
   struct meta<std::string_view>
   {
      static constexpr std::string_view name = "std::string_view";
   };

   template <>
   struct meta<const std::string_view>
   {
      static constexpr std::string_view name = "const std::string_view";
   };

   template <class T>
   struct json_schema
   {};

   namespace detail
   {
      template <class T>
      struct Array
      {
         T value;
      };

      template <class T>
      Array(T) -> Array<T>;

      template <class T>
      struct Object
      {
         T value;
      };

      template <class T>
      Object(T) -> Object<T>;

      template <class T>
      struct Enum
      {
         T value;
      };

      template <class T>
      Enum(T) -> Enum<T>;

      template <class T>
      struct Flags
      {
         T value;
      };

      template <class T>
      Flags(T) -> Flags<T>;
   }

   template <class T>
   concept local_construct_t = requires { T::glaze::construct; };

   template <class T>
   concept global_construct_t = requires { meta<T>::construct; };

   template <class T>
   concept local_meta_t = requires { T::glaze::value; };

   template <class T>
   concept local_keys_t = requires { T::glaze::keys; };

   template <class T>
   concept global_meta_t = requires { meta<T>::value; };

   template <class T>
   concept local_modify_t = requires { std::decay_t<T>::glaze::modify; };

   template <class T>
   concept global_modify_t = requires { meta<std::decay_t<T>>::modify; };

   template <class T>
   concept modify_t = local_modify_t<T> || global_modify_t<T>;

   template <class T>
   concept glaze_t =
      requires { meta<std::decay_t<T>>::value; } || local_meta_t<std::decay_t<T>> || modify_t<std::decay_t<T>>;

   template <class T>
   concept meta_keys = requires { meta<std::decay_t<T>>::keys; } || local_keys_t<std::decay_t<T>>;

   template <class T>
   concept has_unknown_writer = requires { meta<T>::unknown_write; } || requires { T::glaze::unknown_write; };

   template <class T>
   concept has_unknown_reader = requires { meta<T>::unknown_read; } || requires { T::glaze::unknown_read; };

   template <class T>
   concept local_json_schema_t = requires { typename std::decay_t<T>::glaze_json_schema; };

   template <class T>
   concept global_json_schema_t = requires { typename json_schema<T>; };

   template <class T>
   concept json_schema_t = local_json_schema_t<T> || global_json_schema_t<T>;

   struct empty
   {
      static constexpr glz::tuple<> value{};
   };

   namespace detail
   {
      template <class T>
      concept object_key_like = std::convertible_to<T, std::string_view>;

      template <class Tuple>
      consteval auto compute_value_indices()
      {
         return []<size_t... I>(std::index_sequence<I...>) {
            constexpr size_t count = (static_cast<size_t>(!object_key_like<glz::tuple_element_t<I, Tuple>>) + ... + 0);
            std::array<size_t, count> result{};
            size_t idx = 0;
            (([&] {
                if constexpr (!object_key_like<glz::tuple_element_t<I, Tuple>>) {
                   result[idx++] = I;
                }
             }()),
             ...);
            return result;
         }(std::make_index_sequence<glz::tuple_size_v<Tuple>>{});
      }

      inline constexpr size_t modify_npos = static_cast<size_t>(-1);

      template <class T, size_t I>
      struct aggregate_accessor
      {
         template <class V>
         constexpr decltype(auto) operator()(V&& value) const
         {
            return glz::get<I>(to_tie(std::forward<V>(value)));
         }
      };

      template <auto Ptr>
      struct pointer_name_storage
      {
         static constexpr auto alias = get_name<Ptr>();
         static constexpr auto value = join_v<alias>;
      };

      template <class T>
      consteval size_t find_member_index(std::string_view name)
      {
         constexpr auto names = member_names<T>;
         for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
               return i;
            }
         }
         return modify_npos;
      }

      template <class T>
      consteval auto get_modify_object()
      {
         if constexpr (local_modify_t<T>) {
            return std::decay_t<T>::glaze::modify;
         }
         else {
            return meta<std::decay_t<T>>::modify;
         }
      }

      template <class T, bool HasModify>
      struct modify_data_impl;

      template <class T>
      struct modify_data_impl<T, false>
      {
         static constexpr size_t value_count = 0;
      };

      template <class T>
      struct modify_data_impl<T, true>
      {
         using value_type = std::decay_t<T>;
         static constexpr auto object = get_modify_object<value_type>();
         static constexpr auto tuple = object.value;
         using tuple_t = std::remove_cvref_t<decltype(tuple)>;
         static constexpr auto indices = compute_value_indices<tuple_t>();
         static constexpr size_t value_count = indices.size();

         template <size_t EntryPos>
         static constexpr size_t tuple_index_v = indices[EntryPos];

         template <size_t EntryPos>
         using element_type = glz::tuple_element_t<tuple_index_v<EntryPos>, tuple_t>;

         template <size_t EntryPos>
         static consteval bool has_key()
         {
            constexpr size_t idx = tuple_index_v<EntryPos>;
            if constexpr (idx == 0) {
               return false;
            }
            else {
               return object_key_like<glz::tuple_element_t<idx - 1, tuple_t>>;
            }
         }

         template <size_t EntryPos>
         static consteval sv key()
         {
            constexpr size_t idx = tuple_index_v<EntryPos>;
            if constexpr (idx == 0) {
               return sv{};
            }
            else {
               return sv(glz::get<idx - 1>(tuple));
            }
         }

         template <size_t EntryPos>
         static consteval auto element()
         {
            return glz::get<tuple_index_v<EntryPos>>(tuple);
         }

         template <size_t EntryPos>
         static consteval sv pointer_name()
         {
            return pointer_name_storage<glz::get<tuple_index_v<EntryPos>>(tuple)>::value;
         }

         template <size_t EntryPos>
         static consteval size_t base_index()
         {
            using elem_t = element_type<EntryPos>;
            if constexpr (std::is_member_pointer_v<elem_t>) {
               return find_member_index<value_type>(pointer_name<EntryPos>());
            }
            else if constexpr (has_key<EntryPos>()) {
               return find_member_index<value_type>(key<EntryPos>());
            }
            else {
               return modify_npos;
            }
         }

         template <size_t EntryPos>
         static consteval sv new_name()
         {
            using elem_t = element_type<EntryPos>;
            if constexpr (has_key<EntryPos>()) {
               return key<EntryPos>();
            }
            else if constexpr (std::is_member_pointer_v<elem_t>) {
               return pointer_name<EntryPos>();
            }
            else {
               static_assert(has_key<EntryPos>(),
                             "modify entry must provide a key when not referencing a member pointer");
               return pointer_name<EntryPos>();
            }
         }

         template <size_t BaseIndex, size_t Pos>
         static consteval size_t find_replacement_impl()
         {
            if constexpr (base_index<Pos>() == BaseIndex) {
               return Pos;
            }
            else if constexpr (Pos == 0) {
               return modify_npos;
            }
            else {
               return find_replacement_impl<BaseIndex, Pos - 1>();
            }
         }

         template <size_t BaseIndex>
         static consteval size_t find_replacement()
         {
            if constexpr (value_count == 0) {
               return modify_npos;
            }
            else {
               return find_replacement_impl<BaseIndex, value_count - 1>();
            }
         }

         template <size_t... I>
         static consteval auto compute_extra_indices_impl(std::index_sequence<I...>)
         {
            constexpr size_t count = (static_cast<size_t>(base_index<I>() == modify_npos) + ... + 0);
            std::array<size_t, count> result{};
            size_t idx = 0;
            ((base_index<I>() == modify_npos ? (result[idx++] = I, 0) : 0), ...);
            return result;
         }

         static constexpr auto extra_indices = compute_extra_indices_impl(std::make_index_sequence<value_count>{});
         static constexpr size_t extra_count = extra_indices.size();
      };

      template <class T>
      using modify_data = modify_data_impl<T, modify_t<T>>;

      template <class T, size_t BaseIndex>
      consteval auto base_entry_key()
      {
         if constexpr (modify_t<T>) {
            constexpr auto entry = modify_data<T>::template find_replacement<BaseIndex>();
            if constexpr (entry != modify_npos) {
               return modify_data<T>::template new_name<entry>();
            }
         }
         return member_names<T>[BaseIndex];
      }

      template <class T, size_t BaseIndex>
      consteval auto base_entry_value()
      {
         if constexpr (modify_t<T>) {
            constexpr auto entry = modify_data<T>::template find_replacement<BaseIndex>();
            if constexpr (entry != modify_npos) {
               return modify_data<T>::template element<entry>();
            }
            else {
               return aggregate_accessor<T, BaseIndex>{};
            }
         }
         else {
            return aggregate_accessor<T, BaseIndex>{};
         }
      }

      template <class T, size_t ElementIdx>
      consteval auto base_entry_element()
      {
         constexpr size_t base_index = ElementIdx / 2;
         if constexpr ((ElementIdx % 2) == 0) {
            return base_entry_key<T, base_index>();
         }
         else {
            return base_entry_value<T, base_index>();
         }
      }

      template <class T, size_t ExtraOffset>
      consteval auto extra_entry_element_offset()
      {
         static_assert(modify_t<T>);
         constexpr size_t entry_pos = modify_data<T>::extra_indices[ExtraOffset / 2];
         if constexpr ((ExtraOffset % 2) == 0) {
            return modify_data<T>::template new_name<entry_pos>();
         }
         else {
            return modify_data<T>::template element<entry_pos>();
         }
      }

      template <class T, size_t ElementIdx>
      consteval auto combined_entry_element()
      {
         constexpr size_t base_total = 2 * detail::count_members<T>;
         if constexpr (ElementIdx < base_total) {
            return base_entry_element<T, ElementIdx>();
         }
         else {
            constexpr size_t extra_offset = ElementIdx - base_total;
            return extra_entry_element_offset<T, extra_offset>();
         }
      }

      template <class T, size_t... I>
      consteval auto make_entries_impl(std::index_sequence<I...>)
      {
         if constexpr (sizeof...(I) == 0) {
            return glz::tuple<>();
         }
         else {
            return glz::tuple{combined_entry_element<T, I>()...};
         }
      }

      template <class T>
      consteval auto make_modified_object()
      {
         using decayed_t = std::remove_cvref_t<T>;
         static_assert(std::is_class_v<decayed_t> && std::is_aggregate_v<decayed_t>,
                       "glz::meta modify requires aggregate class types");

         constexpr size_t base_total = 2 * detail::count_members<T>;
         constexpr size_t total_elements = [] {
            if constexpr (modify_t<T>) {
               return base_total + 2 * modify_data<T>::extra_count;
            }
            else {
               return base_total;
            }
         }();

         constexpr auto entries = make_entries_impl<T>(std::make_index_sequence<total_elements>{});
         return Object{entries};
      }
   }

   template <class T>
   inline constexpr decltype(auto) meta_wrapper_v = [] {
      if constexpr (local_meta_t<T>) {
         return T::glaze::value;
      }
      else if constexpr (global_meta_t<T>) {
         return meta<T>::value;
      }
      else if constexpr (modify_t<T>) {
         return detail::make_modified_object<std::decay_t<T>>();
      }
      else {
         return empty{};
      }
   }();

   template <class T>
   inline constexpr auto meta_construct_v = [] {
      if constexpr (local_construct_t<T>) {
         return T::glaze::construct;
      }
      else if constexpr (global_construct_t<T>) {
         return meta<T>::construct;
      }
      else {
         return empty{};
      }
   }();

   template <class T>
   inline constexpr auto meta_v = []() -> decltype(auto) {
      if constexpr (meta_keys<T>) {
         return meta_wrapper_v<decay_keep_volatile_t<T>>;
      }
      else {
         return meta_wrapper_v<decay_keep_volatile_t<T>>.value;
      }
   }();

   template <class T>
   using meta_t = decay_keep_volatile_t<decltype(meta_v<T>)>;

   template <class T>
   using meta_wrapper_t = decay_keep_volatile_t<decltype(meta_wrapper_v<std::decay_t<T>>)>;

   template <class T>
   inline constexpr auto meta_keys_v = [] {
      if constexpr (local_meta_t<T>) {
         return T::glaze::keys;
      }
      else if constexpr (global_meta_t<T>) {
         return meta<T>::keys;
      }
      else {
         static_assert(false_v<T>, "no keys or values provided");
      }
   }();

   template <class T>
   using meta_keys_t = decay_keep_volatile_t<decltype(meta_keys_v<T>)>;

   template <class T>
   struct remove_meta_wrapper
   {
      using type = T;
   };
   template <glaze_t T>
   struct remove_meta_wrapper<T>
   {
      using type = std::remove_pointer_t<std::remove_const_t<meta_wrapper_t<T>>>;
   };
   template <class T>
   using remove_meta_wrapper_t = typename remove_meta_wrapper<T>::type;

   template <class T>
   inline constexpr auto meta_unknown_write_v = [] {
      if constexpr (local_meta_t<T>) {
         return T::glaze::unknown_write;
      }
      else if constexpr (global_meta_t<T>) {
         return meta<T>::unknown_write;
      }
      else {
         return empty{};
      }
   }();

   template <class T>
   using meta_unknown_write_t = std::decay_t<decltype(meta_unknown_write_v<std::decay_t<T>>)>;

   template <class T>
   inline constexpr auto meta_unknown_read_v = [] {
      if constexpr (local_meta_t<T>) {
         return T::glaze::unknown_read;
      }
      else if constexpr (global_meta_t<T>) {
         return meta<T>::unknown_read;
      }
      else {
         return empty{};
      }
   }();

   template <class T>
   using meta_unknown_read_t = std::decay_t<decltype(meta_unknown_read_v<std::decay_t<T>>)>;

   template <class T>
   inline constexpr auto self_constraint_v = [] {
      using Decayed = std::decay_t<T>;
      if constexpr (requires { Decayed::glaze::self_constraint; }) {
         return Decayed::glaze::self_constraint;
      }
      else if constexpr (requires { meta<Decayed>::self_constraint; }) {
         return meta<Decayed>::self_constraint;
      }
      else {
         return empty{};
      }
   }();

   template <class T>
   inline constexpr bool has_self_constraint_v =
      !std::is_same_v<std::decay_t<decltype(self_constraint_v<std::decay_t<T>>)>, empty>;

   template <class T>
   concept named = requires { meta<T>::name; } || requires { T::glaze::name; };

   template <class T>
   inline constexpr std::string_view name_v = [] {
      if constexpr (named<T>) {
         if constexpr (requires { T::glaze::name; }) {
            return T::glaze::name;
         }
         else {
            return meta<T>::name;
         }
      }
      else if constexpr (std::is_void_v<T>) {
         return "void";
      }
      else {
         return type_name<T>;
      }
   }();

   template <class T>
   concept tagged = requires { meta<std::decay_t<T>>::tag; } || requires { std::decay_t<T>::glaze::tag; };

   template <class T>
   concept ided = requires { meta<std::decay_t<T>>::ids; } || requires { std::decay_t<T>::glaze::ids; };

   // Concept when skip is specified for the type
   template <class T>
   concept meta_has_skip = requires(T t, const std::string_view s, const meta_context& mctx) {
      { glz::meta<std::remove_cvref_t<T>>::skip(s, mctx) } -> std::same_as<bool>;
   };

   // Concept when skip_if is specified for the type (value-based, runtime)
   template <class T>
   concept meta_has_skip_if = requires {
      {
         meta<std::remove_cvref_t<T>>::skip_if(std::declval<int>(), std::declval<std::string_view>(),
                                               std::declval<meta_context>())
      } -> std::convertible_to<bool>;
   };

   template <class T>
   inline constexpr std::string_view tag_v = [] {
      if constexpr (tagged<T>) {
         if constexpr (local_meta_t<T>) {
            return std::decay_t<T>::glaze::tag;
         }
         else {
            return meta<std::decay_t<T>>::tag;
         }
      }
      else {
         return "";
      }
   }();

   namespace detail
   {
      template <class T, size_t N>
         requires(not std::integral<T>)
      inline constexpr auto convert_ids_to_array(const std::array<T, N>& arr)
      {
         std::array<std::string_view, N> result;
         for (size_t i = 0; i < N; ++i) {
            result[i] = arr[i];
         }
         return result;
      }

      template <class T, size_t N>
         requires(std::integral<T>)
      inline constexpr auto convert_ids_to_array(const std::array<T, N>& arr)
      {
         std::array<T, N> result;
         for (size_t i = 0; i < N; ++i) {
            result[i] = arr[i];
         }
         return result;
      }
   }

   template <is_variant T>
   inline constexpr auto ids_v = [] {
      if constexpr (ided<T>) {
         if constexpr (local_meta_t<T>) {
            return detail::convert_ids_to_array(std::decay_t<T>::glaze::ids);
         }
         else {
            return detail::convert_ids_to_array(meta<std::decay_t<T>>::ids);
         }
      }
      else {
         constexpr auto N = std::variant_size_v<T>;
         std::array<std::string_view, N> ids{};
         for_each<N>([&]<auto I>() { ids[I] = glz::name_v<std::decay_t<std::variant_alternative_t<I, T>>>; });
         return ids;
      }
   }();

   template <class T>
   concept versioned = requires { meta<std::decay_t<T>>::version; };

   template <class T>
   inline constexpr version_t version_v = []() -> version_t {
      if constexpr (versioned<T>) {
         return meta<std::decay_t<T>>::version;
      }
      else {
         return {0, 0, 1};
      }
   }();

   // We don't make this constexpr so that we can have heap allocated values like std::string
   // IMPORTANT: GCC has a bug that doesn't default instantiate this object when it isn't constexpr
   // The solution is to use the json_schema_type defined below to instantiate where used.
   template <json_schema_t T>
   inline const auto json_schema_v = [] {
      if constexpr (local_json_schema_t<T>) {
         return typename std::decay_t<T>::glaze_json_schema{};
      }
      else if constexpr (global_json_schema_t<T>) {
         return json_schema<std::decay_t<T>>{};
      }
   }();

   template <class T>
   using json_schema_type = std::decay_t<decltype(json_schema_v<T>)>;

   // Allows developers to add `static constexpr auto custom_read = true;` to their glz::meta to prevent ambiguous
   // partial specialization for custom parsers
   template <class T>
   concept custom_read = requires { requires meta<T>::custom_read == true; };

   template <class T>
   concept custom_write = requires { requires meta<T>::custom_write == true; };

   // Check if T has a mimic type defined in glz::meta or T::glaze
   // Usage: using mimic = std::string;
   template <class T>
   concept local_mimic_t = requires { typename T::glaze::mimic; };

   template <class T>
   concept global_mimic_t = requires { typename meta<T>::mimic; };

   template <class T>
   concept has_mimic = local_mimic_t<T> || global_mimic_t<T>;

   // Extract the mimic type from T's glz::meta or T::glaze
   namespace detail
   {
      template <class T>
      struct mimic_type_impl
      {
         using type = typename meta<T>::mimic;
      };

      template <local_mimic_t T>
      struct mimic_type_impl<T>
      {
         using type = typename T::glaze::mimic;
      };
   }

   template <has_mimic T>
   using mimic_type = typename detail::mimic_type_impl<T>::type;

   // Generic concept: does T mimic Target type exactly?
   // Allows developers to add `using mimic = std::string;` to their glz::meta
   // to indicate that a custom type mimics another type's serialization behavior.
   template <class T, class Target>
   concept mimics = has_mimic<T> && std::same_as<mimic_type<T>, Target>;
}
