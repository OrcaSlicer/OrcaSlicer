// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/custom.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/write.hpp"
#include "glaze/util/glaze_fast_float.hpp"

// Use JSON Pointer syntax to seek to a specific element
// https://github.com/stephenberry/JSON-Pointer

namespace glz
{
   namespace detail
   {
      // Parse a single JSON pointer token, handling RFC 6901 escapes (~0 -> ~, ~1 -> /)
      // Writes the unescaped token to `out` and returns the remaining json_ptr
      // json_ptr must start with '/' (or be empty for root)
      // Returns empty string_view on parse error (invalid escape sequence)
      inline sv parse_json_ptr_token_into(sv json_ptr, std::string& out)
      {
         if (json_ptr.empty() || json_ptr[0] != '/') {
            return json_ptr; // No token to parse (at root or invalid)
         }

         out.clear();
         size_t i = 1; // Skip leading '/'
         for (; i < json_ptr.size(); ++i) {
            auto c = json_ptr[i];
            if (c == '/') {
               break;
            }
            else if (c == '~') {
               if (++i >= json_ptr.size()) {
                  return {}; // Error: incomplete escape sequence
               }
               c = json_ptr[i];
               if (c == '0') {
                  c = '~';
               }
               else if (c == '1') {
                  c = '/';
               }
               else {
                  return {}; // Error: invalid escape sequence
               }
            }
            out.push_back(c);
         }

         return json_ptr.substr(i);
      }

      // Convenience wrapper that returns {token, remaining}
      inline std::pair<std::string, sv> parse_json_ptr_token(sv json_ptr)
      {
         std::string token;
         sv remaining = parse_json_ptr_token_into(json_ptr, token);
         return {std::move(token), remaining};
      }
   } // namespace detail

   template <class T>
   struct seek_op;

   // Call a function on an value at the location of a json_ptr
   template <class F, class T>
   bool seek(F&& func, T&& value, sv json_ptr)
   {
      return seek_op<std::remove_cvref_t<T>>::op(std::forward<F>(func), std::forward<T>(value), json_ptr);
   }

   // Default implementation for types that don't have specific specializations
   template <class T>
   struct seek_op
   {
      template <class F>
      static bool op(F&& func, auto&& value, sv json_ptr)
      {
         if (json_ptr.empty()) {
            func(value);
            return true;
         }
         return false;
      }
   };

   // Specialization for glaze_value_t types
   template <class T>
      requires glaze_value_t<T>
   struct seek_op<T>
   {
      template <class F>
      static bool op(F&& func, auto&& value, sv json_ptr)
      {
         decltype(auto) member = get_member(value, meta_wrapper_v<T>);
         if (json_ptr.empty()) {
            func(member);
            return true;
         }
         return seek(std::forward<F>(func), member, json_ptr);
      }
   };

   // Specialization for array-like types
   template <class T>
      requires(glaze_array_t<T> || tuple_t<T> || array_t<T> || is_std_tuple<T>)
   struct seek_op<T>
   {
      template <class F>
      static bool op(F&& func, auto&& value, sv json_ptr)
      {
         if (json_ptr.empty()) {
            func(value);
            return true;
         }
         if (json_ptr[0] != '/' || json_ptr.size() < 2) return false;

         size_t index{};
         auto [p, ec] = std::from_chars(&json_ptr[1], json_ptr.data() + json_ptr.size(), index);
         if (ec != std::errc{}) return false;
         json_ptr = json_ptr.substr(p - json_ptr.data());

         if constexpr (glaze_array_t<T>) {
            static constexpr auto member_array = glz::detail::make_array<T>();
            if (index >= member_array.size()) return false;
            return std::visit(
               [&](auto&& element) { return seek(std::forward<F>(func), get_member(value, element), json_ptr); },
               member_array[index]);
         }
         else if constexpr (tuple_t<T> || is_std_tuple<T>) {
            if (index >= glz::tuple_size_v<T>) return false;
            auto tuple_element_ptr = detail::get_runtime(value, index);
            return std::visit([&](auto&& element_ptr) { return seek(std::forward<F>(func), *element_ptr, json_ptr); },
                              tuple_element_ptr);
         }
         else {
            if (index >= value.size()) return false;
            return seek(std::forward<F>(func), *std::next(value.begin(), index), json_ptr);
         }
      }
   };

   // Specialization for nullable types
   template <class T>
      requires nullable_t<T>
   struct seek_op<T>
   {
      template <class F>
      static bool op(F&& func, auto&& value, sv json_ptr)
      {
         if (json_ptr.empty()) {
            func(value);
            return true;
         }
         if (!value) return false;
         return seek(std::forward<F>(func), *value, json_ptr);
      }
   };

   // Specialization for object/map types
   template <class T>
      requires(readable_map_t<T> || glaze_object_t<T> || reflectable<T>)
   struct seek_op<T>
   {
      template <class F>
      static bool op(F&& func, auto&& value, sv json_ptr)
      {
         if (json_ptr.empty()) {
            func(value);
            return true;
         }
         if (json_ptr[0] != '/') return false;

         // Determine key type: maps use their key_type, objects/reflectables use string
         auto key = []() {
            if constexpr (writable_map_t<T>) {
               return typename T::key_type{};
            }
            else {
               return std::string{};
            }
         }();
         using Key = decltype(key);
         static_assert(std::is_same_v<Key, std::string> || num_t<Key>);

         if constexpr (std::is_same_v<Key, std::string>) {
            sv remaining = detail::parse_json_ptr_token_into(json_ptr, key);
            if (remaining.data() == nullptr) return false; // Parse error (invalid escape)
            json_ptr = remaining;
         }
         else if constexpr (std::floating_point<Key>) {
            auto [ptr, ec] = glz::from_chars<false>(json_ptr.data(), json_ptr.data() + json_ptr.size(), key);
            if (ec != std::errc()) [[unlikely]] {
               return false;
            }
            json_ptr = json_ptr.substr(size_t(ptr - json_ptr.data()));
         }
         else {
            auto [p, ec] = std::from_chars(&json_ptr[1], json_ptr.data() + json_ptr.size(), key);
            if (ec != std::errc{}) return false;
            json_ptr = json_ptr.substr(p - json_ptr.data());
         }

         if constexpr (glaze_object_t<T> || reflectable<T>) {
            static constexpr auto N = reflect<T>::size;
            static constexpr auto HashInfo = hash_info<T>;
            static_assert(HashInfo.type != hash_type::invalid, "Hashing failed");

            const auto index = decode_hash_with_size<JSON_PTR, T, HashInfo, HashInfo.type>::op(
               key.data(), key.data() + key.size(), key.size());

            if (index < N) [[likely]] {
               bool ret{};
               visit<N>(
                  [&]<size_t I>() {
                     static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                     if (key == TargetKey) {
                        if constexpr (reflectable<T>) {
                           ret = seek(std::forward<F>(func), get_member(value, get<I>(to_tie(value))), json_ptr);
                        }
                        else {
                           ret = seek(std::forward<F>(func), get_member(value, get<I>(reflect<T>::values)), json_ptr);
                        }
                     }
                  },
                  index);
               return ret;
            }
            else [[unlikely]] {
               return false;
            }
         }
         else {
            auto it = value.find(key);
            if (it == value.end()) {
               return false;
            }
            return seek(std::forward<F>(func), it->second, json_ptr);
         }
      }
   };

   // Helper to transfer cv-qualifiers from source type to target type
   template <class Source, class Target>
   using copy_cv_t = std::conditional_t<
      std::is_const_v<Source>,
      std::conditional_t<std::is_volatile_v<Source>, std::add_cv_t<Target>, std::add_const_t<Target>>,
      std::conditional_t<std::is_volatile_v<Source>, std::add_volatile_t<Target>, Target>>;

   // Helper to determine the correct pointer type based on value type
   template <class V, class T>
   using get_pointer_type = copy_cv_t<std::remove_reference_t<T>, V>*;

   // Helper to determine the correct reference wrapper type
   template <class V, class T>
   using get_reference_type = std::reference_wrapper<copy_cv_t<std::remove_reference_t<T>, V>>;

   // Get a reference to a value at the location of a json_ptr. Will error if
   // value doesnt exist or is wrong type
   template <class V, class T>
   expected<get_reference_type<V, T>, error_ctx> get(T&& root_value, sv json_ptr)
   {
      get_pointer_type<V, T> result{};
      error_code ec{};
      seek(
         [&](auto&& val) {
            if constexpr (not std::same_as<V, decay_keep_volatile_t<decltype(val)>>) {
               ec = error_code::get_wrong_type;
            }
            else if constexpr (not std::is_lvalue_reference_v<decltype(val)>) {
               ec = error_code::cannot_be_referenced;
            }
            else {
               result = &val;
            }
         },
         std::forward<T>(root_value), json_ptr);
      if (!result) {
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }
      else if (bool(ec)) {
         return unexpected(error_ctx{0, ec});
      }
      return std::ref(*result);
   }

   // Get a pointer to a value at the location of a json_ptr. Will return
   // nullptr if value doesnt exist or is wrong type
   template <class V, class T>
   get_pointer_type<V, T> get_if(T&& root_value, sv json_ptr) noexcept
   {
      get_pointer_type<V, T> result{};
      seek(
         [&](auto&& val) {
            if constexpr (std::same_as<V, std::decay_t<decltype(val)>>) {
               result = &val;
            }
         },
         std::forward<T>(root_value), json_ptr);
      return result;
   }

   // Get a value at the location of a json_ptr. Will error if
   // value doesnt exist or is not assignable or is a narrowing conversion.
   template <class V, class T>
   expected<V, error_code> get_value(T&& root_value, sv json_ptr) noexcept
   {
      V result{};
      bool found{};
      error_code ec{};
      seek(
         [&](auto&& val) {
            if constexpr (std::is_assignable_v<V&, decltype(val)> &&
                          non_narrowing_convertable<std::decay_t<decltype(val)>, V>) {
               found = true;
               result = val;
            }
            else {
               ec = error_code::get_wrong_type;
            }
         },
         std::forward<T>(root_value), json_ptr);
      if (!found) {
         return unexpected(error_code::nonexistent_json_ptr);
      }
      else if (bool(ec)) {
         return unexpected(ec);
      }

      return result;
   }

   // Assign to a value at the location of a json_ptr with respect to the root_value
   // if assignable and not a narrowing conversion
   template <class T, class V>
   bool set(T&& root_value, const sv json_ptr, V&& value) noexcept
   {
      bool result{};
      seek(
         [&](auto&& val) {
            if constexpr (std::is_assignable_v<decltype(val), decltype(value)> &&
                          non_narrowing_convertable<std::decay_t<decltype(value)>, std::decay_t<decltype(val)>>) {
               result = true;
#if defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
#endif
               val = value;
#if defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic pop
#endif
            }
         },
         std::forward<T>(root_value), json_ptr);
      return result;
   }

   template <class R>
   using call_result_t = std::conditional_t<std::is_reference_v<R> || std::is_pointer_v<R>, std::decay_t<R>*, R>;

   template <class R>
   using call_return_t =
      std::conditional_t<std::is_reference_v<R>, expected<std::reference_wrapper<std::decay_t<R>>, error_code>,
                         expected<R, error_code>>;

   // call a member function
   template <class R, class T, class... Args>
   call_return_t<R> call(T&& root_value, sv json_ptr, Args&&... args)
   {
      call_result_t<R> result{};

      error_code ec{};

      const auto valid = seek(
         [&ec, &result, &root_value, ... args = std::forward<Args>(args)](auto&& val) {
            using V = std::decay_t<decltype(val)>;
            if constexpr (std::is_member_function_pointer_v<V>) {
               auto f = std::mem_fn(val);
               using F = decltype(f);
               if constexpr (std::invocable<F, T, Args...>) {
                  if constexpr (!std::is_assignable_v<R, std::invoke_result_t<F, T, Args...>>) {
                     ec = error_code::invalid_call; // type not assignable
                  }
                  else {
                     if constexpr (std::is_reference_v<R>) {
                        result = &f(root_value, std::forward<Args>(args)...);
                     }
                     else {
                        result = f(root_value, std::forward<Args>(args)...);
                     }
                  }
               }
               else {
                  ec = error_code::invalid_call; // not invocable with given inputs
               }
            }
            else {
               ec = error_code::invalid_call; // seek did not find a function
            }
         },
         std::forward<T>(root_value), json_ptr);

      if (!valid) {
         return unexpected(error_code::nonexistent_json_ptr);
      }

      if (bool(ec)) {
         return unexpected(ec);
      }

      if constexpr (std::is_reference_v<R>) {
         return *result;
      }
      else {
         return result;
      }
   }
}

namespace glz
{
   constexpr size_t json_ptr_depth(const auto s)
   {
      size_t count = 0;
      for (size_t i = 0; (i = s.find('/', i)) != std::string::npos; ++i) {
         ++count;
      }
      return count;
   }

   // TODO: handle ~ and / characters for full JSON pointer support
   constexpr std::pair<sv, sv> tokenize_json_ptr(sv s)
   {
      if (s.empty()) {
         return {"", ""};
      }
      s.remove_prefix(1);
      if (s.find('/') == std::string::npos) {
         return {s, ""};
      }
      const auto i = s.find_first_of('/');
      return {s.substr(0, i), s.substr(i, s.size() - i)};
   }

   inline constexpr auto first_key(sv s) { return tokenize_json_ptr(s).first; }

   inline constexpr auto remove_first_key(sv s) { return tokenize_json_ptr(s).second; }

   inline constexpr std::pair<sv, sv> parent_last_json_ptrs(const sv s)
   {
      const auto i = s.find_last_of('/');
      return {s.substr(0, i), s.substr(i, s.size())};
   }

   inline auto split_json_ptr(sv s, std::vector<sv>& v)
   {
      const auto n = std::count(s.begin(), s.end(), '/');
      v.resize(n);
      for (auto i = 0; i < n; ++i) {
         std::tie(v[i], s) = tokenize_json_ptr(s);
      }
   }

   // TODO: handle ~ and / characters for full JSON pointer support
   template <auto& Str>
   constexpr auto split_json_ptr()
   {
      constexpr auto str = Str;
      constexpr auto N = std::count(str.begin(), str.end(), '/');
      std::array<sv, N> arr;
      sv s = str;
      for (auto i = 0; i < N; ++i) {
         std::tie(arr[i], s) = tokenize_json_ptr(s);
      }
      return arr;
   }

   namespace detail
   {
      inline std::pair<sv, sv> tokenize_json_ptr_children(sv s)
      {
         if (s.empty()) {
            return {"", ""};
         }
         auto next = s.substr(1);
         if (next.find('/') == std::string::npos) {
            return {s, ""};
         }
         const auto i = next.find_first_of('/');
         return {s.substr(0, i + 1), next.substr(i, next.size() - i)};
      };

      // Get each full JSON pointer path at increasing depth
      inline auto json_ptr_children(sv s)
      {
         std::vector<sv> v{};
         const auto n = std::count(s.begin(), s.end(), '/') + 1; // +1 for root ""
         v.resize(n);
         v[0] = "";
         auto* start = s.data();
         for (auto i = 1; i < n; ++i) {
            std::tie(v[i], s) = tokenize_json_ptr_children(s);
         }

         for (auto i = 1; i < n; ++i) {
            v[i] = sv{start, size_t((v[i].data() + v[i].size()) - start)};
         }
         return v;
      }

      template <auto& Str>
      constexpr auto json_ptr_children()
      {
         constexpr auto str = Str;
         constexpr auto N = std::count(str.begin(), str.end(), '/') + 1; // +1 for root ""
         auto* start = str.data();
         std::array<sv, N> v;
         v[0] = "";
         sv s = str;
         for (auto i = 1; i < N; ++i) {
            std::tie(v[i], s) = tokenize_json_ptr_children(s);
         }

         for (auto i = 1; i < N; ++i) {
            v[i] = sv{start, size_t((v[i].data() + v[i].size()) - start)};
         }
         return v;
      }
   }

   constexpr auto json_ptrs(auto&&... args) { return std::array{sv{args}...}; }

   // must copy to allow mutation in constexpr context
   constexpr auto sort_json_ptrs(auto arr)
   {
      std::sort(arr.begin(), arr.end());
      return arr;
   }

   // input array must be sorted
   constexpr auto group_json_ptrs_impl(const auto& arr)
   {
      constexpr auto N = glz::tuple_size_v<std::decay_t<decltype(arr)>>;

      std::array<sv, N> first_keys;
      std::transform(arr.begin(), arr.end(), first_keys.begin(), first_key);

      std::array<sv, N> unique_keys{};
      const auto it = std::unique_copy(first_keys.begin(), first_keys.end(), unique_keys.begin());

      const auto n_unique = static_cast<size_t>(std::distance(unique_keys.begin(), it));

      std::array<size_t, N> n_items_per_group{};

      for (size_t i = 0; i < n_unique; ++i) {
         n_items_per_group[i] = std::count(first_keys.begin(), first_keys.end(), unique_keys[i]);
      }

      return glz::tuple{n_items_per_group, n_unique, unique_keys};
   }

   template <auto Arr, std::size_t... Is>
   constexpr auto make_arrays(std::index_sequence<Is...>)
   {
      return glz::tuplet::make_tuple(pair{sv{}, std::array<sv, Arr[Is]>{}}...);
   }

   template <size_t N, auto& Arr>
   constexpr auto sub_group(const auto start)
   {
      std::array<sv, N> ret;
      std::transform(Arr.begin() + start, Arr.begin() + start + N, ret.begin(), remove_first_key);
      return ret;
   }

   template <auto& Arr>
   constexpr auto group_json_ptrs()
   {
      constexpr auto group_info = group_json_ptrs_impl(Arr);
      constexpr auto n_items_per_group = glz::get<0>(group_info);
      constexpr auto n_unique = glz::get<1>(group_info);
      constexpr auto unique_keys = glz::get<2>(group_info);

      auto arrs = make_arrays<n_items_per_group>(std::make_index_sequence<n_unique>{});
      size_t start{};

      for_each<n_unique>([&]<auto I>() {
         constexpr size_t n_items = n_items_per_group[I];

         glz::get<I>(arrs).first = unique_keys[I];
         glz::get<I>(arrs).second = glz::sub_group<n_items, Arr>(start);
         start += n_items;
      });

      return arrs;
   }

   // TODO support custom types
   template <class Root, string_literal ptr, class Expected = void>
   constexpr bool valid()
   {
      using V = std::decay_t<Root>;
      if constexpr (ptr.sv() == sv{""}) {
         return std::same_as<Expected, void> || std::same_as<V, Expected>;
      }
      else {
         constexpr auto tokens = tokenize_json_ptr(ptr.sv());
         constexpr auto key_str = tokens.first;
         constexpr auto rem_ptr = glz::string_literal_from_view<tokens.second.size()>(tokens.second);
         if constexpr (glz::glaze_object_t<V>) {
            constexpr auto& HashInfo = hash_info<V>;
            constexpr auto I = decode_hash_with_size<JSON, V, HashInfo, HashInfo.type>::op(
               key_str.data(), key_str.data() + key_str.size(), key_str.size());

            if constexpr (I < reflect<V>::size) {
               if constexpr (key_str != reflect<V>::keys[I]) {
                  return false;
               }
               using T = refl_t<V, I>;
               if constexpr (is_specialization_v<T, includer>) {
                  return valid<file_include, rem_ptr, Expected>();
               }
               else {
                  return valid<T, rem_ptr, Expected>();
               }
            }
            else {
               return false;
            }
         }
         else if constexpr (glz::writable_map_t<V>) {
            return valid<typename V::mapped_type, rem_ptr, Expected>();
         }
         else if constexpr (glz::glaze_array_t<V>) {
            constexpr auto member_array = glz::detail::make_array<std::decay_t<V>>();
            constexpr auto optional_index = stoui(key_str); // TODO: Will not build if not int
            if constexpr (optional_index) {
               constexpr auto index = *optional_index;
               if constexpr (index >= 0 && index < member_array.size()) {
                  constexpr auto member = member_array[index];
                  constexpr auto member_ptr = std::get<member.index()>(member);
                  using sub_t = decltype(glz::get_member(std::declval<V>(), member_ptr));
                  return valid<sub_t, rem_ptr, Expected>();
               }
               else {
                  return false;
               }
            }
            else {
               return false;
            }
         }
         else if constexpr (glz::array_t<V>) {
            if (stoui(key_str)) {
               return valid<range_value_t<V>, rem_ptr, Expected>();
            }
            return false;
         }
         else if constexpr (glz::nullable_t<V>) {
            using sub_t = decltype(*std::declval<V>());
            return valid<sub_t, ptr, Expected>();
         }
         else {
            return false;
         }
      }
   }
}
