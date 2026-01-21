// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/beve/header.hpp"
#include "glaze/core/common.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/wrappers.hpp"
#include "glaze/util/primes_64.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
// Turn off MSVC warning for unreferenced formal parameter, which is referenced in a constexpr branch
#pragma warning(push)
#pragma warning(disable : 4100 4189)
#endif

namespace glz
{
   // Check if a size_t value exists in an array (used for hash collision detection)
   constexpr bool contains(const size_t* data, const size_t size, const size_t val) noexcept
   {
      for (size_t i = 0; i < size; ++i) {
         if (data[i] == val) {
            return true;
         }
      }
      return false;
   }

   // Convert up to 7 bytes to uint64_t (for short key hashing)
   inline constexpr uint64_t to_uint64_n_below_8(const char* bytes, const size_t N) noexcept
   {
      uint64_t res{};
      if consteval {
         // Compile-time: build value byte-by-byte in little-endian order
         for (size_t i = 0; i < N; ++i) {
            res |= (uint64_t(uint8_t(bytes[i])) << (i << 3));
         }
      }
      else {
         switch (N) {
         case 1: {
            std::memcpy(&res, bytes, 1);
            break;
         }
         case 2: {
            std::memcpy(&res, bytes, 2);
            break;
         }
         case 3: {
            std::memcpy(&res, bytes, 3);
            break;
         }
         case 4: {
            std::memcpy(&res, bytes, 4);
            break;
         }
         case 5: {
            std::memcpy(&res, bytes, 5);
            break;
         }
         case 6: {
            std::memcpy(&res, bytes, 6);
            break;
         }
         case 7: {
            std::memcpy(&res, bytes, 7);
            break;
         }
         default: {
            // zero size case
            break;
         }
         }
         // On big endian: byteswap to match little-endian layout
         // Example for "abc" (N=3):
         //   LE memcpy: res = 0x0000000000636261 (bytes in LSB positions)
         //   BE memcpy: res = 0x6162630000000000 (bytes in MSB positions)
         //   BE after byteswap: res = 0x0000000000636261 (matches LE)
         if constexpr (std::endian::native == std::endian::big) {
            res = std::byteswap(res);
         }
      }
      return res;
   }

   // Convert N bytes to uint64_t (template version for compile-time N)
   template <size_t N = 8>
   constexpr uint64_t to_uint64(const char* bytes) noexcept
   {
      static_assert(N <= sizeof(uint64_t));
      if consteval {
         uint64_t res{};
         for (size_t i = 0; i < N; ++i) {
            res |= (uint64_t(uint8_t(bytes[i])) << (i << 3));
         }
         return res;
      }
      else {
         if constexpr (N == 8) {
            uint64_t res;
            std::memcpy(&res, bytes, N);
            if constexpr (std::endian::native == std::endian::big) {
               res = std::byteswap(res);
            }
            return res;
         }
         else {
            uint64_t res{};
            std::memcpy(&res, bytes, N);
            if constexpr (std::endian::native == std::endian::big) {
               res = std::byteswap(res);
            }
            return res;
         }
      }
   }

   // Get indices of elements satisfying a predicate
   template <class Tuple, template <class> class Predicate>
   consteval auto filter_indices()
   {
      constexpr auto N = tuple_size_v<Tuple>;
      if constexpr (N == 0) {
         return std::array<size_t, 0>{};
      }
      else {
         return []<size_t... Is>(std::index_sequence<Is...>) constexpr {
            constexpr bool matches[] = {Predicate<glz::tuple_element_t<Is, Tuple>>::value...};
            constexpr size_t count = (matches[Is] + ...);

            std::array<size_t, count> indices{};
            size_t index = 0;
            ((void)((matches[Is] ? (indices[index++] = Is, true) : false)), ...);
            return indices;
         }(std::make_index_sequence<N>{});
      }
   }

   template <class T>
   concept is_object_key_type = std::convertible_to<T, std::string_view>;

   template <class T>
   using object_key_type = std::bool_constant<is_object_key_type<T>>;

   template <class T>
   using not_object_key_type = std::bool_constant<not is_object_key_type<T>>;

   namespace detail
   {
      // The purpose of this is to allocate a new string_view to only the portion of memory
      // that we are concerned with. This lets the compiler reduce the binary on
      // reflected names;
      template <size_t I, class V>
      struct get_name_alloc
      {
         static constexpr auto alias = get_name<get<I>(meta_v<V>)>();
         static constexpr auto value = join_v<alias>;
      };
   }

   template <class T, size_t I>
   consteval sv get_key_element()
   {
      using V = std::decay_t<T>;
      if constexpr (I == 0) {
         if constexpr (is_object_key_type<decltype(get<0>(meta_v<V>))>) {
            return get<0>(meta_v<V>);
         }
         else {
            return detail::get_name_alloc<0, V>::value;
            // return get_name<get<0>(meta_v<V>)>();
         }
      }
      else if constexpr (is_object_key_type<decltype(get<I - 1>(meta_v<V>))>) {
         return get<I - 1>(meta_v<V>);
      }
      else {
         return detail::get_name_alloc<I, V>::value;
         // return get_name<get<I>(meta_v<V>)>();
      }
   };

   template <class T>
   struct reflect;

   // ============================================================================
   // keys_wrapper: A pseudo-type that provides reflect<> interface for arbitrary key arrays
   // Used to enable hash-based lookup for variant IDs and other key sources
   // ============================================================================

   template <const auto& Keys>
   struct keys_wrapper
   {
      // This type exists only to satisfy template parameters for hash_info and decode_hash
   };

   template <class T>
   struct is_keys_wrapper_t : std::false_type
   {};

   template <const auto& Keys>
   struct is_keys_wrapper_t<keys_wrapper<Keys>> : std::true_type
   {};

   template <class T>
   inline constexpr bool is_keys_wrapper_v = is_keys_wrapper_t<T>::value;

   // Specialize reflect for keys_wrapper to provide the keys interface
   template <const auto& Keys>
   struct reflect<keys_wrapper<Keys>>
   {
      static constexpr auto size = Keys.size();

      // Convert keys to array of string_view
      static constexpr auto keys = []() {
         std::array<sv, size> result{};
         for (size_t i = 0; i < size; ++i) {
            result[i] = sv(Keys[i]);
         }
         return result;
      }();

      // Provide values as indices for completeness (needed by some hash paths)
      static constexpr auto values = []() {
         std::array<size_t, size> result{};
         for (size_t i = 0; i < size; ++i) {
            result[i] = i;
         }
         return result;
      }();
   };

   // ============================================================================
   // End of keys_wrapper
   // ============================================================================

   // MSVC requires this template specialization for when the tuple size if zero,
   // otherwise MSVC tries to instantiate calls of get<0> in invalid branches
   template <class T>
      requires((glaze_object_t<T> || glaze_flags_t<T> || glaze_enum_t<T>) && (tuple_size_v<meta_t<T>> == 0))
   struct reflect<T>
   {
      static constexpr auto size = 0;
      static constexpr auto values = tuple{};
      static constexpr std::array<sv, 0> keys{};

      template <size_t I>
      using type = std::nullptr_t;
   };

   template <class T>
      requires(!meta_keys<T> && (glaze_object_t<T> || glaze_flags_t<T> || glaze_enum_t<T>) &&
               (tuple_size_v<meta_t<T>> != 0))
   struct reflect<T>
   {
      using V = std::remove_cvref_t<T>;
      static constexpr auto value_indices = filter_indices<meta_t<V>, not_object_key_type>();

      static constexpr auto values = [] {
         return [&]<size_t... I>(std::index_sequence<I...>) { //
            return tuple{get<value_indices[I]>(meta_v<T>)...}; //
         }(std::make_index_sequence<value_indices.size()>{}); //
      }();

      static constexpr auto size = tuple_size_v<decltype(values)>;

      static constexpr auto keys = [] {
         std::array<sv, size> res{};
         [&]<size_t... I>(std::index_sequence<I...>) { //
            ((res[I] = get_key_element<T, value_indices[I]>()), ...);
         }(std::make_index_sequence<value_indices.size()>{});
         return res;
      }();

      template <size_t I>
      using elem = decltype(get<I>(values));

      template <size_t I>
      using type = member_t<V, decltype(get<I>(values))>;
   };

   template <class T, size_t N>
   inline constexpr auto c_style_to_sv(const std::array<T, N>& arr)
   {
      std::array<sv, N> ret{};
      for (size_t i = 0; i < N; ++i) {
         ret[i] = arr[i];
      }
      return ret;
   }

   template <class T>
      requires(meta_keys<T> && glaze_t<T>)
   struct reflect<T>
   {
      using V = std::remove_cvref_t<T>;
      static constexpr auto size = tuple_size_v<meta_keys_t<T>>;

      static constexpr auto values = meta_wrapper_v<T>;

      static constexpr auto keys = c_style_to_sv(meta_keys_v<T>);

      template <size_t I>
      using elem = decltype(get<I>(values));

      template <size_t I>
      using type = member_t<V, decltype(get<I>(values))>;
   };

   template <class T>
      requires(is_memory_object<T>)
   struct reflect<T> : reflect<memory_type<T>>
   {};

   template <class T>
      requires(glaze_array_t<T>)
   struct reflect<T>
   {
      using V = std::remove_cvref_t<T>;

      static constexpr auto values = meta_v<V>;

      static constexpr auto size = tuple_size_v<decltype(values)>;

      template <size_t I>
      using elem = decltype(get<I>(values));

      template <size_t I>
      using type = member_t<V, decltype(get<I>(values))>;
   };

   template <class T>
      requires reflectable<T>
   struct reflect<T>
   {
      using V = std::remove_cvref_t<T>;
      using tie_type = decltype(to_tie(std::declval<T&>()));

      static constexpr auto keys = member_names<V>;
      static constexpr auto size = keys.size();

      template <size_t I>
      using elem = decltype(get<I>(std::declval<tie_type>()));

      template <size_t I>
      using type = member_t<V, decltype(get<I>(std::declval<tie_type>()))>;
   };

   template <class T>
      requires readable_map_t<T>
   struct reflect<T>
   {
      static constexpr auto size = 0;
   };

   // The type of the field before get_member is applied
   template <class T, size_t I>
   using elem_t = reflect<T>::template elem<I>;

   // The type of the field after get_member is applied
   template <class T, size_t I>
   using refl_t = reflect<T>::template type<I>;

   // The decayed type after get_member is called
   template <class T, size_t I>
   using field_t = std::remove_cvref_t<refl_t<T, I>>;

   template <auto Opts, class T>
   inline constexpr bool maybe_skipped = [] {
      if constexpr (reflect<T>::size > 0) {
         constexpr auto N = reflect<T>::size;
         if constexpr (meta_has_skip<T> || meta_has_skip_if<T>) {
            return true;
         }
         else if constexpr (Opts.skip_null_members) {
            // if any type could be null then we might skip
            constexpr bool write_function_pointers = check_write_function_pointers(Opts);
            return [&]<size_t... I>(std::index_sequence<I...>) {
               return ((always_skipped<field_t<T, I>> ||
                        (!write_function_pointers && is_member_function_pointer<field_t<T, I>>) ||
                        null_t<field_t<T, I>>) ||
                       ...);
            }(std::make_index_sequence<N>{});
         }
         else {
            // if we have an always_skipped type then we return true
            constexpr bool write_function_pointers = check_write_function_pointers(Opts);
            return [&]<size_t... I>(std::index_sequence<I...>) {
               return ((always_skipped<field_t<T, I>> ||
                        (!write_function_pointers && is_member_function_pointer<field_t<T, I>>)) ||
                       ...);
            }(std::make_index_sequence<N>{});
         }
      }
      else {
         return false;
      }
   }();
}

namespace glz
{
   template <size_t I, class T>
   constexpr auto key_name_v = [] {
      if constexpr (reflectable<T>) {
         return get<I>(member_names<T>);
      }
      else {
         return reflect<T>::keys[I];
      }
   }();

   template <class V, class From>
   consteval bool custom_type_is_nullable()
   {
      if constexpr (std::is_member_pointer_v<From>) {
         if constexpr (std::is_member_function_pointer_v<From>) {
            using Ret = typename return_type<From>::type;
            if constexpr (std::is_void_v<Ret>) {
               using Tuple = typename inputs_as_tuple<From>::type;
               if constexpr (glz::tuple_size_v<Tuple> == 1) {
                  using Input = std::decay_t<glz::tuple_element_t<0, Tuple>>;
                  return bool(null_t<Input>);
               }
            }
         }
         else if constexpr (std::is_member_object_pointer_v<From>) {
            using Value = std::decay_t<decltype(std::declval<V>().val.*(std::declval<V>().from))>;
            if constexpr (is_specialization_v<Value, std::function>) {
               using Ret = typename function_traits<Value>::result_type;

               if constexpr (std::is_void_v<Ret>) {
                  using Tuple = typename function_traits<Value>::arguments;
                  if constexpr (glz::tuple_size_v<Tuple> == 1) {
                     using Input = std::decay_t<glz::tuple_element_t<0, Tuple>>;
                     return bool(null_t<Input>);
                  }
               }
            }
            else {
               return bool(null_t<Value>);
            }
         }
      }
      else {
         if constexpr (is_invocable_concrete<From>) {
            using Ret = invocable_result_t<From>;
            if constexpr (std::is_void_v<Ret>) {
               using Tuple = invocable_args_t<From>;
               constexpr auto N = glz::tuple_size_v<Tuple>;
               if constexpr (N == 2) {
                  using Input = std::decay_t<glz::tuple_element_t<1, Tuple>>;
                  return bool(null_t<Input>);
               }
            }
         }
      }

      return false;
   }

   template <class T, auto Opts>
   constexpr auto required_fields()
   {
      constexpr auto N = reflect<T>::size;

      bit_array<N> fields{};
      if constexpr (Opts.error_on_missing_keys) {
         for_each<N>([&]<auto I>() constexpr {
            using V = std::decay_t<refl_t<T, I>>;

            // Check if meta<T>::requires_key customization point exists
            if constexpr (meta_has_requires_key<T>) {
               constexpr auto key = reflect<T>::keys[I];
               constexpr bool is_nullable = [] {
                  if constexpr (is_specialization_v<V, custom_t>) {
                     using From = typename V::from_t;
                     return custom_type_is_nullable<V, From>();
                  }
                  else if constexpr (is_cast<V>) {
                     using CastType = typename V::cast_type;
                     return null_t<CastType>;
                  }
                  else {
                     return null_t<V>;
                  }
               }();
               fields[I] = meta<T>::requires_key(key, is_nullable);
            }
            else if constexpr (is_specialization_v<V, custom_t>) {
               using From = typename V::from_t;

               // If we are reading a glz::custom_t, we must deduce the input argument and not require the key if it is
               // optional This allows error_on_missing_keys to work properly with glz::custom_t wrapping optional types
               constexpr bool nullable_in_custom = custom_type_is_nullable<V, From>();

               fields[I] = !Opts.skip_null_members || !(std::same_as<From, skip> || nullable_in_custom);
            }
            else if constexpr (is_cast<V>) {
               // Handle cast_t by checking if the cast type is nullable
               using CastType = typename V::cast_type;
               fields[I] = !Opts.skip_null_members || !null_t<CastType>;
            }
            else {
               fields[I] = !Opts.skip_null_members || !null_t<V>;
            }
         });
      }
      return fields;
   }
}

namespace glz::detail
{
   // from
   // https://stackoverflow.com/questions/55941964/how-to-filter-duplicate-types-from-tuple-c
   template <class T, class... Ts>
   struct unique
   {
      using type = T;
   };

   template <template <class...> class T, class... Ts, class U, class... Us>
   struct unique<T<Ts...>, U, Us...>
      : std::conditional_t<(std::is_same_v<U, Ts> || ...), unique<T<Ts...>, Us...>, unique<T<Ts..., U>, Us...>>
   {};

   template <class... Ts>
   struct unique_variant : unique<std::variant<>, Ts...>
   {};

   template <class T>
   struct tuple_ptr_variant;

   template <class... Ts>
   struct tuple_ptr_variant<glz::tuple<Ts...>> : unique<std::variant<>, std::add_pointer_t<Ts>...>
   {};

   template <class... Ts>
   struct tuple_ptr_variant<std::tuple<Ts...>> : unique<std::variant<>, std::add_pointer_t<Ts>...>
   {};

   template <class... Ts>
   struct tuple_ptr_variant<std::pair<Ts...>> : unique<std::variant<>, std::add_pointer_t<Ts>...>
   {};

   template <class T, class = std::make_index_sequence<reflect<T>::size>>
   struct member_tuple_type;

   template <class T, size_t... I>
   struct member_tuple_type<T, std::index_sequence<I...>>
   {
      using type =
         std::conditional_t<sizeof...(I) == 0, tuple<>, tuple<std::remove_cvref_t<member_t<T, refl_t<T, I>>>...>>;
   };

   template <class T>
   using member_tuple_t = typename member_tuple_type<T>::type;

   template <class T, class = std::make_index_sequence<reflect<T>::size>>
   struct value_variant;

   template <class T, size_t... I>
   struct value_variant<T, std::index_sequence<I...>>
   {
      using type = typename unique_variant<std::remove_cvref_t<elem_t<T, I>>...>::type;
   };

   template <class T>
   using value_variant_t = typename value_variant<T>::type;

   template <class T>
   inline constexpr auto make_array()
   {
      return []<size_t... I>(std::index_sequence<I...>) {
         using value_t = value_variant_t<T>;
         return std::array<value_t, reflect<T>::size>{get<I>(reflect<T>::values)...};
      }(std::make_index_sequence<reflect<T>::size>{});
   }

   template <class Tuple, std::size_t... Is>
   inline constexpr auto tuple_runtime_getter(std::index_sequence<Is...>)
   {
      using value_t = typename tuple_ptr_variant<Tuple>::type;
      using tuple_ref = std::add_lvalue_reference_t<Tuple>;
      using getter_t = value_t (*)(tuple_ref);
      return std::array<getter_t, glz::tuple_size_v<Tuple>>{+[](tuple_ref t) -> value_t {
         if constexpr (is_std_tuple<Tuple>) {
            return &std::get<Is>(t);
         }
         else {
            return &glz::get<Is>(t);
         }
      }...};
   }

   template <class Tuple>
   inline auto get_runtime(Tuple&& t, const size_t index)
   {
      using T = std::decay_t<Tuple>;
      static constexpr auto indices = std::make_index_sequence<glz::tuple_size_v<T>>{};
      static constexpr auto runtime_getter = tuple_runtime_getter<T>(indices);
      return runtime_getter[index](t);
   }
}

namespace glz
{
   template <auto Enum>
      requires(std::is_enum_v<decltype(Enum)>)
   constexpr sv enum_name_v = []() -> std::string_view {
      using T = std::decay_t<decltype(Enum)>;

      if constexpr (glaze_t<T>) {
         using U = std::underlying_type_t<T>;
         return reflect<T>::keys[static_cast<U>(Enum)];
      }
      else {
         static_assert(false_v<decltype(Enum)>, "Enum requires glaze metadata for name");
      }
   }();
}

namespace glz
{
   // ============================================================================
   // Integer key hashing for enum values and integral variant IDs
   // ============================================================================

   enum struct int_hash_type {
      direct, // Sequential values starting at 0: value as index
      offset, // Sequential values with offset: value - min_value
      power_of_two, // Powers of 2 (flags): countr_zero(value)
      small_range, // Sparse lookup table for small ranges
      modular, // Perfect hash: (value * seed) % table_size
      modular_shifted // Perfect hash with shift: ((value >> shift) * seed) % table_size
   };

   template <size_t N, size_t TableSize>
   struct int_keys_info_t
   {
      // Note: table must be first to work around Apple clang NTTP bug where
      // arrays at the end of structs get corrupted when passed as template parameters
      std::array<uint8_t, TableSize> table{}; // For small_range/modular: maps key → index
      int_hash_type type{};
      int64_t min_value{};
      int64_t max_value{};
      uint64_t seed{};
      size_t table_size{};
      uint8_t shift{}; // Right shift to apply before modular hash (handles common power-of-2 factors)
   };

   // Specialization for when no table is needed
   template <size_t N>
   struct int_keys_info_t<N, 0>
   {
      int_hash_type type{};
      int64_t min_value{};
      int64_t max_value{};
      uint64_t seed{};
      size_t table_size{};
      uint8_t shift{}; // Right shift to apply before modular hash (handles common power-of-2 factors)
   };

   template <class T>
      requires std::is_enum_v<T>
   constexpr auto make_int_keys_info()
   {
      using U = std::underlying_type_t<T>;
      constexpr auto N = reflect<T>::size;

      if constexpr (N == 0) {
         return int_keys_info_t<0, 0>{};
      }
      else if constexpr (N == 1) {
         // For single-element enums, check if value is 0 (direct) or needs offset
         constexpr auto value = static_cast<U>(glz::get<0>(reflect<T>::values));
         if constexpr (value == 0) {
            return int_keys_info_t<1, 0>{.type = int_hash_type::direct};
         }
         else {
            return int_keys_info_t<1, 0>{.type = int_hash_type::offset, .min_value = static_cast<int64_t>(value)};
         }
      }
      else {
         // Extract values into array for analysis
         constexpr auto vals = []() constexpr {
            constexpr auto size = reflect<T>::size;
            std::array<U, size> result{};
            for_each<size>([&]<size_t I>() { result[I] = static_cast<U>(glz::get<I>(reflect<T>::values)); });
            return result;
         }();

         // Find min/max
         constexpr auto min_max = [&]() {
            U min_val = vals[0], max_val = vals[0];
            for (const auto v : vals) {
               if (v < min_val) min_val = v;
               if (v > max_val) max_val = v;
            }
            return std::pair{min_val, max_val};
         }();

         constexpr U min_val = min_max.first;
         constexpr U max_val = min_max.second;
         constexpr auto range = static_cast<size_t>(max_val - min_val);

         // Strategy 1: Dense sequential (most common case)
         constexpr bool is_sequential = [&]() {
            if (range + 1 != N) return false;
            std::array<bool, N> seen{};
            for (const auto v : vals) {
               const auto idx = static_cast<size_t>(v - min_val);
               if (idx >= N || seen[idx]) return false;
               seen[idx] = true;
            }
            return true;
         }();

         // Strategy 2: Powers of 2 (flag enums)
         constexpr auto power_of_two_info = [&]() {
            using UnsignedU = std::make_unsigned_t<U>;
            size_t max_bit = 0;

            for (const auto v : vals) {
               const auto uv = static_cast<UnsignedU>(v);
               if (uv == 0 || !std::has_single_bit(uv)) {
                  return std::pair{false, size_t{0}};
               }
               const auto bit = static_cast<size_t>(std::countr_zero(uv));
               if (bit > max_bit) max_bit = bit;
            }

            // Verify no collisions in bit positions
            std::array<bool, 64> used{};
            for (const auto v : vals) {
               const auto bit = std::countr_zero(static_cast<UnsignedU>(v));
               if (used[bit]) return std::pair{false, size_t{0}};
               used[bit] = true;
            }

            return std::pair{true, max_bit + 1};
         }();

         constexpr size_t sparse_threshold = 256;
         constexpr auto table_size = std::bit_ceil(N * 2);

         // Use proper if-else-if chain so only one return type is deduced
         if constexpr (is_sequential) {
            if constexpr (min_val == 0) {
               return int_keys_info_t<N, 0>{.type = int_hash_type::direct};
            }
            else {
               return int_keys_info_t<N, 0>{.type = int_hash_type::offset, .min_value = min_val};
            }
         }
         else if constexpr (power_of_two_info.first) {
            constexpr auto tbl_size = power_of_two_info.second;
            int_keys_info_t<N, tbl_size> info{.type = int_hash_type::power_of_two, .table_size = tbl_size};
            info.table.fill(static_cast<uint8_t>(N));

            using UnsignedU = std::make_unsigned_t<U>;
            for (size_t i = 0; i < N; ++i) {
               const auto bit = std::countr_zero(static_cast<UnsignedU>(vals[i]));
               info.table[bit] = static_cast<uint8_t>(i);
            }
            return info;
         }
         else if constexpr (range < sparse_threshold) {
            // Strategy 3: Small range → sparse lookup table
            int_keys_info_t<N, sparse_threshold> info{
               .type = int_hash_type::small_range, .min_value = min_val, .max_value = max_val, .table_size = range + 1};
            info.table.fill(static_cast<uint8_t>(N));

            for (size_t i = 0; i < N; ++i) {
               info.table[static_cast<size_t>(vals[i] - min_val)] = static_cast<uint8_t>(i);
            }
            return info;
         }
         else {
            // Strategy 4: Modular perfect hash (fallback)
            // First try standard modular hash (faster lookup)
            constexpr auto modular_info = [&]() {
               for (const auto prime : primes_64) {
                  std::array<bool, table_size> used{};
                  bool collision = false;

                  for (size_t i = 0; i < N; ++i) {
                     const auto h = (static_cast<uint64_t>(vals[i]) * prime) % table_size;
                     if (used[h]) {
                        collision = true;
                        break;
                     }
                     used[h] = true;
                  }

                  if (!collision) {
                     return std::pair{true, prime};
                  }
               }
               return std::pair{false, uint64_t{0}};
            }();

            if constexpr (modular_info.first) {
               // Standard modular hash works
               int_keys_info_t<N, table_size> info{
                  .type = int_hash_type::modular, .seed = modular_info.second, .table_size = table_size};
               info.table.fill(static_cast<uint8_t>(N));

               for (size_t i = 0; i < N; ++i) {
                  const auto h = (static_cast<uint64_t>(vals[i]) * info.seed) % table_size;
                  info.table[h] = static_cast<uint8_t>(i);
               }
               return info;
            }
            else {
               // Strategy 5: Modular hash with shift for sparse enums with common power-of-2 factors
               constexpr uint8_t common_shift = [&]() -> uint8_t {
                  uint8_t min_trailing = 64;
                  for (const auto v : vals) {
                     if (v != 0) {
                        const auto trailing = static_cast<uint8_t>(std::countr_zero(static_cast<uint64_t>(v)));
                        if (trailing < min_trailing) {
                           min_trailing = trailing;
                        }
                     }
                  }
                  return min_trailing == 64 ? 0 : min_trailing;
               }();

               constexpr auto shifted_info = [&]() {
                  for (const auto prime : primes_64) {
                     std::array<bool, table_size> used{};
                     bool collision = false;

                     for (size_t i = 0; i < N; ++i) {
                        const auto shifted = static_cast<uint64_t>(vals[i]) >> common_shift;
                        const auto h = (shifted * prime) % table_size;
                        if (used[h]) {
                           collision = true;
                           break;
                        }
                        used[h] = true;
                     }

                     if (!collision) {
                        return std::pair{true, prime};
                     }
                  }
                  return std::pair{false, uint64_t{0}};
               }();

               static_assert(shifted_info.first, "Failed to find perfect hash seed for enum");

               int_keys_info_t<N, table_size> info{.type = int_hash_type::modular_shifted,
                                                   .seed = shifted_info.second,
                                                   .table_size = table_size,
                                                   .shift = common_shift};
               info.table.fill(static_cast<uint8_t>(N));

               for (size_t i = 0; i < N; ++i) {
                  const auto shifted = static_cast<uint64_t>(vals[i]) >> common_shift;
                  const auto h = (shifted * info.seed) % table_size;
                  info.table[h] = static_cast<uint8_t>(i);
               }
               return info;
            }
         }
      }
   }

   template <class T>
      requires std::is_enum_v<T>
   constexpr auto enum_index_info = make_int_keys_info<T>();

   // Array of enum underlying values for runtime indexing
   template <class T>
      requires std::is_enum_v<T>
   constexpr auto enum_values_array = []<size_t... I>(std::index_sequence<I...>) {
      using U = std::underlying_type_t<T>;
      return std::array<U, sizeof...(I)>{static_cast<U>(glz::get<I>(reflect<T>::values))...};
   }(std::make_index_sequence<reflect<T>::size>{});

   template <class T, auto Info>
   struct enum_value_to_index
   {
      using U = std::underlying_type_t<T>;
      static constexpr auto N = reflect<T>::size;

      GLZ_ALWAYS_INLINE static constexpr size_t op(U value) noexcept
      {
         using enum int_hash_type;

         if constexpr (Info.type == direct) {
            // Bounds check done by caller
            return static_cast<size_t>(value);
         }
         else if constexpr (Info.type == offset) {
            return static_cast<size_t>(value - Info.min_value);
         }
         else if constexpr (Info.type == power_of_two) {
            using UnsignedU = std::make_unsigned_t<U>;
            const auto uv = static_cast<UnsignedU>(value);
            // Must be a non-zero power of 2, otherwise countr_zero gives wrong result
            if (uv == 0 || !std::has_single_bit(uv)) [[unlikely]] {
               return N; // Invalid: not a power of 2
            }
            const auto bit = static_cast<size_t>(std::countr_zero(uv));
            if (bit >= Info.table_size) [[unlikely]] {
               return N; // Invalid: bit position out of range
            }
            return Info.table[bit];
         }
         else if constexpr (Info.type == small_range) {
            const auto idx = static_cast<int64_t>(value) - Info.min_value;
            if (idx < 0 || static_cast<size_t>(idx) >= Info.table_size) [[unlikely]] {
               return N; // Invalid: out of range
            }
            return Info.table[static_cast<size_t>(idx)]; // Returns N if slot is empty
         }
         else if constexpr (Info.type == modular) {
            const auto h = (static_cast<uint64_t>(value) * Info.seed) % Info.table_size;
            return Info.table[h]; // Returns N if slot is empty
         }
         else { // modular_shifted
            const auto shifted = static_cast<uint64_t>(value) >> Info.shift;
            const auto h = (shifted * Info.seed) % Info.table_size;
            return Info.table[h]; // Returns N if slot is empty
         }
      }
   };

   // ============================================================================
   // End of integer key hashing
   // ============================================================================

   // get a std::string_view from an enum value
   template <class T>
      requires(glaze_t<T> && std::is_enum_v<std::decay_t<T>>)
   constexpr auto get_enum_name(T&& enum_value)
   {
      using V = std::decay_t<T>;
      using U = std::underlying_type_t<V>;
      constexpr auto N = reflect<V>::size;

      if constexpr (N == 0) {
         return std::string_view{};
      }
      else {
         constexpr auto& info = enum_index_info<V>;
         const auto index = enum_value_to_index<V, info>::op(static_cast<U>(enum_value));

         if (index >= N) [[unlikely]] {
            return std::string_view{};
         }

         // For direct/offset strategies, values are guaranteed sequential
         // For hash-based strategies, verify the value matches to avoid false positives
         if constexpr (info.type == int_hash_type::direct || info.type == int_hash_type::offset) {
            return reflect<V>::keys[index];
         }
         else {
            // Verify match for power_of_two/small_range/modular lookups
            constexpr auto& values = enum_values_array<V>;
            if (values[index] == static_cast<U>(enum_value)) {
               return reflect<V>::keys[index];
            }
            return std::string_view{};
         }
      }
   }

   template <glaze_flags_t T>
   consteval auto byte_length() noexcept
   {
      constexpr auto N = reflect<T>::size;

      if constexpr (N % 8 == 0) {
         return N / 8;
      }
      else {
         return (N / 8) + 1;
      }
   }
}

#include <initializer_list>

#include "glaze/core/common.hpp"
#include "glaze/reflection/get_name.hpp"
#include "glaze/reflection/to_tuple.hpp"

namespace glz
{
   // Use a dummy struct for make_reflectable so that we don't conflict with any user defined constructors
   struct dummy final
   {};

   // If you want to make an empty struct or a struct with constructors visible in reflected structs,
   // add the folllwing constructor to your type:
   // my_struct(glz::make_reflectable) {}
   using make_reflectable = std::initializer_list<dummy>;
}

namespace glz
{
   // TODO: This is returning the total keys and not the max keys for a particular variant object
   template <class T, size_t N>
   constexpr size_t get_max_keys = [] {
      size_t res{};
      for_each<N>([&]<auto I>() {
         using V = std::decay_t<std::variant_alternative_t<I, T>>;
         if constexpr (glaze_object_t<V> || reflectable<V>) {
            res += reflect<V>::size;
         }
         else if constexpr (is_memory_object<V>) {
            res += reflect<memory_type<V>>::size;
         }
      });
      return res;
   }();

   template <class T>
   constexpr auto get_combined_keys_from_variant()
   {
      constexpr auto N = std::variant_size_v<T>;

      std::array<sv, get_max_keys<T, N>> keys{};
      // This intermediate pointer is necessary for GCC 13 (otherwise segfaults with reflection logic)
      auto* data_ptr = &keys;
      size_t index = 0;
      for_each<N>([&]<auto I>() {
         using V = std::decay_t<std::variant_alternative_t<I, T>>;
         if constexpr (glaze_object_t<V> || reflectable<V> || is_memory_object<V>) {
            using X = std::conditional_t<is_memory_object<V>, memory_type<V>, V>;
            for (size_t i = 0; i < reflect<X>::size; ++i) {
               (*data_ptr)[index++] = reflect<X>::keys[i];
            }
         }
      });

      std::sort(keys.begin(), keys.end());
      const auto end = std::unique(keys.begin(), keys.end());
      const auto size = std::distance(keys.begin(), end);

      return pair{keys, size};
   }
}

namespace glz
{
   template <class T>
   consteval size_t key_index(const std::string_view key)
   {
      const auto n = reflect<T>::keys.size();
      for (size_t i = 0; i < n; ++i) {
         if (key == reflect<T>::keys[i]) {
            return i;
         }
      }
      return n;
   }
}

namespace glz
{
   GLZ_ALWAYS_INLINE constexpr uint64_t bitmix(uint64_t h, const uint64_t seed) noexcept
   {
      h *= seed;
      return h ^ std::rotr(h, 49);
   };

   // Use when hashing large chunks of characters that are likely very similar
   GLZ_ALWAYS_INLINE constexpr uint64_t rich_bitmix(uint64_t h, const uint64_t seed) noexcept
   {
      h ^= h >> 23;
      h *= 0x2127599bf4325c37ULL;
      h ^= seed;
      h *= 0x880355f21e6d1965ULL;
      h ^= h >> 47;
      return h;
   }

   template <size_t N>
   using bucket_value_t = std::conditional_t < N<256, uint8_t, uint16_t>;

   // The larger the underlying bucket the more we avoid collisions with invalid keys.
   // This improves performance of rejecting invalid keys because we don't have to do
   // string comparisons in these cases.
   // However, there are obvious memory costs with increasing the bucket size.

   enum struct hash_type { //
      invalid, // hashing failed
      unique_index, // A unique character index is used
      front_hash, // Hash on the front bytes of the key
      single_element, // Map is a single element
      mod4, // c % 4
      xor_mod4, // (c ^ c0) % 4
      minus_mod4, // (c - c0) % 4
      three_element_unique_index,
      unique_per_length, // Hash on a unique character index and the length of the key
      full_flat // Full key hash with a single table
   };

   // For N == 3 and N == 4 it is cheap to check mod4, xor_mod4, and minus_mod4 hashes.
   // Consecuative values like "x", "y", "z" for keys work with minus_mod4

   struct unique_per_length_t
   {
      bool valid{};
      std::array<uint8_t, 256> unique_index{};
   };

   inline constexpr unique_per_length_t unique_per_length_info(const auto& input_strings)
   {
      // TODO: MSVC fixed the related compiler bug, but GitHub Actions has not caught up yet
#if !defined(_MSC_VER)
      const auto N = input_strings.size();
      if (N == 0) {
         return {};
      }

      // This could be a std::array, but each length N for std::array causes unique template instaniations
      // This propagates to std::ranges::sort, so using std::vector means less template instaniations
      std::vector<std::string_view> strings{};
      strings.reserve(N);
      for (size_t i = 0; i < N; ++i) {
         strings.emplace_back(input_strings[i]);
      }

      std::ranges::sort(strings, [](const auto& a, const auto& b) { return a.size() < b.size(); });

      if (strings.front().empty() || strings.back().size() >= 255) {
         return {};
      }

      unique_per_length_t info{.valid = true};
      info.unique_index.fill(255);

      // Process each unique length
      for (size_t len = strings.front().size(); len <= strings.back().size(); ++len) {
         auto range_begin = std::lower_bound(strings.begin(), strings.end(), len,
                                             [](const auto& s, size_t l) { return s.length() < l; });

         auto range_end =
            std::upper_bound(range_begin, strings.end(), len, [](size_t l, const auto& s) { return l < s.length(); });

         auto range = std::ranges::subrange(range_begin, range_end);

         if (range.begin() == range.end()) continue;

         // Find the first unique character for this length
         bool found = false;
         for (size_t pos = 0; pos < len; ++pos) {
            std::array<int, 256> char_count = {};

            // Count occurrences of each character at this position
            for (auto it = range.begin(); it != range.end(); ++it) {
               ++char_count[uint8_t((*it)[pos])];
            }

            bool collision = false;
            for (const auto count : char_count) {
               if (count > 1) {
                  collision = true;
                  break;
               }
            }
            if (not collision) {
               info.unique_index[len] = uint8_t(pos);
               found = true;
               break;
            }
         }
         if (not found) {
            info.valid = false;
            return info;
         }
      }

      return info;
#else
      return {};
#endif
   }

   template <class T>
   inline constexpr auto per_length_info = unique_per_length_info(reflect<T>::keys);

   consteval size_t bucket_size(hash_type type, size_t N)
   {
      using enum hash_type;
      switch (type) {
      case invalid: {
         return 0;
      }
      case unique_index: {
         return 256;
      }
      case front_hash: {
         return (N == 1) ? 1 : std::bit_ceil(N * N) / 2;
      }
      case single_element: {
         return 0;
      }
      case mod4: {
         return 0;
      }
      case xor_mod4: {
         return 0;
      }
      case minus_mod4: {
         return 0;
      }
      case three_element_unique_index: {
         return 0;
      }
      case unique_per_length: {
         return (N == 1) ? 1 : std::bit_ceil(N * N) / 2;
      }
      case full_flat: {
         return (N == 1) ? 1 : std::bit_ceil(N * N) / 2;
      }
      default: {
         return 0;
      }
      }
   }

   struct keys_info_t
   {
      size_t N{};
      hash_type type{};
      size_t min_length = (std::numeric_limits<size_t>::max)();
      size_t max_length{};
      // uint8_t min_diff = (std::numeric_limits<uint8_t>::max)();
      uint64_t seed{};
      size_t unique_index = (std::numeric_limits<size_t>::max)();
      bool sized_hash = false;
      size_t front_hash_bytes{};
   };

   // For hash algorithm a value of the seed indicates an invalid hash

   // A value of N in the bucket indicates an invalid hash
   template <class T, size_t Slots>
   struct hash_info_t
   {
      hash_type type{};

      static constexpr auto N = reflect<T>::size;
      using V = bucket_value_t<N>;
      static constexpr auto invalid = static_cast<V>(N);

      std::array<V, Slots> table{}; // hashes to switch-case indices
      size_t min_length = (std::numeric_limits<size_t>::max)();
      size_t max_length{};
      uint64_t seed{};
      size_t unique_index = (std::numeric_limits<size_t>::max)();
      bool sized_hash = false;
      size_t front_hash_bytes{};
   };

   constexpr std::optional<size_t> find_unique_index(const auto& strings)
   {
      namespace ranges = std::ranges;

      const auto N = strings.size();

      if (N == 0) {
         return {};
      }

      size_t min_length = (std::numeric_limits<size_t>::max)();
      for (auto& s : strings) {
         const auto n = s.size();
         if (n < min_length) {
            min_length = n;
         }
      }

      if (min_length == 0) {
         return {};
      }

      std::vector<std::vector<uint8_t>> cols(min_length);

      for (const auto& s : strings) {
         // for each character in the string
         for (size_t c = 0; c < min_length; ++c) {
            cols[c].emplace_back(s[c]);
         }
      }

      // sort colums so that we can determine
      // if the column is unique
      size_t best_index{};
      size_t best_count{};
      for (size_t i = 0; i < min_length; ++i) {
         auto& col = cols[i];
         ranges::sort(col);
         if (auto it = ranges::adjacent_find(col); it == col.end()) {
            // no duplicates found
            best_count = col.size();
            best_index = i;
            break;
         }
      }

      if (best_count == 0) {
         return {};
      }

      return best_index;
   }

   constexpr std::optional<size_t> find_unique_sized_index(const auto& strings)
   {
      namespace ranges = std::ranges;

      const auto N = strings.size();

      if (N == 0) {
         return {};
      }

      size_t min_length = (std::numeric_limits<size_t>::max)();
      for (auto& s : strings) {
         if (s.contains('"')) {
            return {}; // Sized hashing requires looking for terminating quote
         }

         const auto n = s.size();
         if (n < min_length) {
            min_length = n;
         }
      }

      if (min_length == 0) {
         return {};
      }

      std::vector<std::vector<uint16_t>> cols(min_length);

      for (size_t i = 0; i < N; ++i) {
         const auto& s = strings[i];
         // for each character in the string
         for (size_t c = 0; c < min_length; ++c) {
            const auto k = uint16_t(uint16_t(s[c]) | (uint16_t(s.size()) << 8));
            cols[c].emplace_back(k);
         }
      }

      // sort colums so that we can determine
      // if the column is unique
      size_t best_index{};
      size_t best_count{};
      for (size_t i = 0; i < min_length; ++i) {
         auto& col = cols[i];
         ranges::sort(col);
         if (auto it = ranges::adjacent_find(col); it == col.end()) {
            // no duplicates found
            best_count = col.size();
            best_index = i;
            break;
         }
      }

      if (best_count == 0) {
         return {};
      }

      return best_index;
   }

   // For full hashing we perform a rich_bitmix on the tail end
   // This is because most cases that require full hashes are because
   // the tail end is the only unique part

   // Do not call this at runtime, it is assumes the key lies within min_length and max_length
   inline constexpr uint64_t full_hash_impl(const sv key, const uint64_t seed, const auto min_length,
                                            const auto max_length) noexcept
   {
      if (max_length < 8) {
         return bitmix(to_uint64_n_below_8(key.data(), key.size()), seed);
      }
      else if (min_length > 7) {
         const auto n = key.size();
         uint64_t h = seed;
         const auto* data = key.data();
         const auto* end7 = data + n - 7;
         for (auto d0 = data; d0 < end7; d0 += 8) {
            h = bitmix(to_uint64(d0), h);
         }
         // Handle potential tail. We know we have at least 8
         return rich_bitmix(to_uint64(data + n - 8), h);
      }
      else {
         const auto n = key.size();
         const auto* data = key.data();

         if (n < 8) {
            return bitmix(to_uint64_n_below_8(data, n), seed);
         }

         uint64_t h = seed;
         const auto* end7 = data + n - 7;
         for (auto d0 = data; d0 < end7; d0 += 8) {
            h = bitmix(to_uint64(d0), h);
         }
         // Handle potential tail. We know we have at least 8
         return rich_bitmix(to_uint64(data + n - 8), h);
      }
   }

   // runtime full hash algorithm
   template <uint64_t min_length, uint64_t max_length, uint64_t seed>
   inline constexpr uint64_t full_hash(const auto* it, const size_t n) noexcept
   {
      if constexpr (max_length < 8) {
         if (n > 7) {
            return seed;
         }
         return bitmix(to_uint64_n_below_8(it, n), seed);
      }
      else if constexpr (min_length > 7) {
         if (n < 8) {
            return seed;
         }
         uint64_t h = seed;
         const auto* end7 = it + n - 7;
         for (auto d0 = it; d0 < end7; d0 += 8) {
            h = bitmix(to_uint64(d0), h);
         }
         // Handle potential tail. We know we have at least 8
         return rich_bitmix(to_uint64(it + n - 8), h);
      }
      else {
         if (n < 8) {
            return bitmix(to_uint64_n_below_8(it, n), seed);
         }

         uint64_t h = seed;
         const auto* end7 = it + n - 7;
         for (auto d0 = it; d0 < end7; d0 += 8) {
            h = bitmix(to_uint64(d0), h);
         }
         // Handle potential tail. We know we have at least 8
         return rich_bitmix(to_uint64(it + n - 8), h);
      }
   }

   template <std::integral ChunkType, size_t N>
   constexpr bool front_bytes_hash_info(const std::array<sv, N>& keys, keys_info_t& info) noexcept
   {
      if (info.min_length < sizeof(ChunkType)) {
         return false;
      }

      // check for uniqueness
      std::array<ChunkType, N> k;
      for (size_t i = 0; i < N; ++i) {
         if constexpr (std::same_as<ChunkType, uint16_t>) {
            k[i] = uint16_t(keys[i][0]) | (uint16_t(keys[i][1]) << 8);
         }
         else if constexpr (std::same_as<ChunkType, uint32_t>) {
            k[i] = uint32_t(keys[i][0]) //
                   | (uint32_t(keys[i][1]) << 8) //
                   | (uint32_t(keys[i][2]) << 16) //
                   | (uint32_t(keys[i][3]) << 24);
         }
         else if constexpr (std::same_as<ChunkType, uint64_t>) {
            k[i] = uint64_t(keys[i][0]) //
                   | (uint64_t(keys[i][1]) << 8) //
                   | (uint64_t(keys[i][2]) << 16) //
                   | (uint64_t(keys[i][3]) << 24) //
                   | (uint64_t(keys[i][4]) << 32) //
                   | (uint64_t(keys[i][5]) << 40) //
                   | (uint64_t(keys[i][6]) << 48) //
                   | (uint64_t(keys[i][7]) << 56);
         }
         else {
            static_assert(false_v<ChunkType>);
         }
      }

      std::ranges::sort(k);

      for (size_t i = 0; i < N - 1; ++i) {
         const auto diff = k[i + 1] - k[i];
         if (diff == 0) {
            return false;
         }
      }

      using enum hash_type;
      constexpr uint64_t invalid_seed = 0;
      auto& seed = info.seed;
      auto hash_alg = [&] {
         std::array<size_t, N> bucket_index{};
         constexpr auto bsize = bucket_size(front_hash, N);

         for (size_t i = 0; i < primes_64.size(); ++i) {
            seed = primes_64[i];
            size_t index = 0;
            for (const auto& key : keys) {
               const auto hash = [&]() -> size_t {
                  if constexpr (std::same_as<ChunkType, uint16_t>) {
                     return bitmix(uint16_t(key[0]) | (uint16_t(key[1]) << 8), seed);
                  }
                  else if constexpr (std::same_as<ChunkType, uint32_t>) {
                     return bitmix(uint32_t(key[0]) //
                                      | (uint32_t(key[1]) << 8) //
                                      | (uint32_t(key[2]) << 16) //
                                      | (uint32_t(key[3]) << 24),
                                   seed);
                  }
                  else if constexpr (std::same_as<ChunkType, uint64_t>) {
                     return rich_bitmix(uint64_t(key[0]) //
                                           | (uint64_t(key[1]) << 8) //
                                           | (uint64_t(key[2]) << 16) //
                                           | (uint64_t(key[3]) << 24) //
                                           | (uint64_t(key[4]) << 32) //
                                           | (uint64_t(key[5]) << 40) //
                                           | (uint64_t(key[6]) << 48) //
                                           | (uint64_t(key[7]) << 56),
                                        seed);
                  }
                  else {
                     static_assert(false_v<ChunkType>);
                  }
               }();
               if (hash == seed) {
                  break;
               }
               const auto bucket = hash % bsize;
               if (contains(bucket_index.data(), index, bucket)) {
                  break;
               }
               bucket_index[index] = bucket;
               ++index;
            }

            if (index == N) {
               // make sure the seed does not collide with any hashes
               const auto bucket = seed % bsize;
               if (not contains(bucket_index.data(), N, bucket)) {
                  return; // found working seed
               }
            }
         }

         seed = invalid_seed;
      };

      hash_alg();
      if (seed != invalid_seed) {
         info.type = front_hash;
         info.front_hash_bytes = sizeof(ChunkType);
         return true;
      }

      return false;
   }

   // The sequence of hashing algorithms written here determines the selection preference
   template <size_t N>
   constexpr auto make_keys_info(const std::array<sv, N>& keys)
   {
      namespace ranges = std::ranges;

      keys_info_t info{N};

      if constexpr (N == 0) {
         return info;
      }

      for (size_t i = 0; i < N; ++i) {
         const auto n = keys[i].size();
         if (n < info.min_length) {
            info.min_length = n;
         }
         if (n > info.max_length) {
            info.max_length = n;
         }
      }

      using enum hash_type;

      if constexpr (N == 1) {
         info.type = single_element;
         return info;
      }

      // N == 2 is optimized within other hashing methods

      if constexpr (N == 3 || N == 4) {
         if (info.min_length > 0) {
            bool valid = true;
            for (size_t i = 0; i < N; ++i) {
               if (keys[i][0] % 4 != uint8_t(i)) {
                  valid = false;
               }
            }
            if (valid) {
               info.type = mod4;
               return info;
            }

            const auto c0 = keys[0][0];

            valid = true;
            for (size_t i = 0; i < N; ++i) {
               if ((keys[i][0] ^ c0) % 4 != uint8_t(i)) {
                  valid = false;
               }
            }
            if (valid) {
               info.type = xor_mod4;
               return info;
            }

            valid = true;
            for (size_t i = 0; i < N; ++i) {
               if ((keys[i][0] - c0) % 4 != uint8_t(i)) {
                  valid = false;
               }
            }
            if (valid) {
               info.type = minus_mod4;
               return info;
            }
         }
      }

      auto& seed = info.seed;
      constexpr uint64_t invalid_seed = 0;

      if (const auto uindex = find_unique_index(keys)) {
         info.unique_index = uindex.value();

         if constexpr (N == 3) {
            // An xor of the first unique character with itself will result in 0 (our desired index)
            // We use a hash algorithm that will produce zero if zero is given, so we can avoid a branch
            // We need a seed produces hashes of [1, 2] for the 2nd and 3rd keys

            const auto u = info.unique_index;
            const auto first = uint8_t(keys[0][u]);
            const auto mix1 = uint8_t(keys[1][u]) ^ first;
            const auto mix2 = uint8_t(keys[2][u]) ^ first;

            for (size_t i = 0; i < primes_64.size(); ++i) {
               seed = primes_64[i];
               uint8_t h1 = (mix1 * seed) % 4;
               uint8_t h2 = (mix2 * seed) % 4;

               if (h1 == 1 && h2 == 2) {
                  info.type = three_element_unique_index;
                  return info;
               }
            }
            // Otherwise we failed to find a seed and we'll use a normal unique_index hash
         }

         info.type = unique_index;
         return info;
      }

      if (front_bytes_hash_info<uint16_t>(keys, info)) {
         return info;
      }
      else if (front_bytes_hash_info<uint32_t>(keys, info)) {
         return info;
      }
      else if (front_bytes_hash_info<uint64_t>(keys, info)) {
         return info;
      }

      if (const auto uindex = find_unique_sized_index(keys)) {
         info.unique_index = uindex.value();
         info.sized_hash = true;

         auto sized_unique_hash = [&] {
            std::array<size_t, N> bucket_index{};
            constexpr auto bsize = bucket_size(unique_index, N);

            for (size_t i = 0; i < primes_64.size(); ++i) {
               seed = primes_64[i];
               size_t index = 0;
               for (const auto& key : keys) {
                  const auto hash = bitmix(uint16_t(key[info.unique_index]) | (uint16_t(key.size()) << 8), seed);
                  if (hash == seed) {
                     break;
                  }
                  const auto bucket = hash % bsize;
                  if (contains(bucket_index.data(), index, bucket)) {
                     break;
                  }
                  bucket_index[index] = bucket;
                  ++index;
               }

               if (index == N) {
                  // make sure the seed does not collide with any hashes
                  const auto bucket = seed % bsize;
                  if (not contains(bucket_index.data(), N, bucket)) {
                     return; // found working seed
                  }
               }
            }

            seed = invalid_seed;
         };

         sized_unique_hash();
         if (seed != invalid_seed) {
            info.type = unique_index;
            return info;
         }
      }

      // TODO: MSVC fixed the related compiler bug, but GitHub Actions has not caught up yet
#if !defined(_MSC_VER)
      // TODO: Use meta-programming to cache this value
      const auto per_length_data = unique_per_length_info(keys);
      if (per_length_data.valid) {
         auto sized_unique_hash = [&] {
            std::array<size_t, N> bucket_index{};
            constexpr auto bsize = bucket_size(unique_per_length, N);

            for (size_t i = 0; i < primes_64.size(); ++i) {
               seed = primes_64[i];
               size_t index = 0;
               for (const auto& key : keys) {
                  const auto n = uint8_t(key.size());
                  const auto hash = bitmix(uint16_t(key[per_length_data.unique_index[n]]) | (uint16_t(n) << 8), seed);
                  if (hash == seed) {
                     break;
                  }
                  const auto bucket = hash % bsize;
                  if (contains(bucket_index.data(), index, bucket)) {
                     break;
                  }
                  bucket_index[index] = bucket;
                  ++index;
               }

               if (index == N) {
                  // make sure the seed does not collide with any hashes
                  const auto bucket = seed % bsize;
                  if (not contains(bucket_index.data(), N, bucket)) {
                     return; // found working seed
                  }
               }
            }

            seed = invalid_seed;
         };

         sized_unique_hash();
         if (seed != invalid_seed) {
            info.type = unique_per_length;
            return info;
         }
      }
#endif

      // full_flat
      {
         auto full_flat_hash = [&] {
            std::array<size_t, N> bucket_index{};
            constexpr auto bsize = bucket_size(full_flat, N);

            for (size_t i = 0; i < primes_64.size(); ++i) {
               seed = primes_64[i];
               size_t index = 0;
               for (const auto& key : keys) {
                  const auto hash = full_hash_impl(key, seed, info.min_length, info.max_length);
                  if (hash == seed) {
                     break;
                  }
                  const auto bucket = hash % bsize;
                  if (contains(bucket_index.data(), index, bucket)) {
                     break;
                  }
                  bucket_index[index] = bucket;
                  ++index;
               }

               if (index == N) {
                  // make sure the seed does not collide with any hashes
                  const auto bucket = seed % bsize;
                  if (not contains(bucket_index.data(), N, bucket)) {
                     return; // found working seed
                  }
               }
            }

            seed = invalid_seed;
         };

         full_flat_hash();
         if (seed != invalid_seed) {
            info.type = full_flat;
            return info;
         }
      }

      return info;
   }

   template <class T>
   constexpr auto keys_info = make_keys_info(reflect<T>::keys);

   template <class T>
   constexpr auto hash_info = [] {
      if constexpr ((glaze_object_t<T> || reflectable<T> || glaze_flags_t<T> ||
                     ((std::is_enum_v<std::remove_cvref_t<T>> && meta_keys<T>) || glaze_enum_t<T>) ||
                     is_keys_wrapper_v<T>) &&
                    (reflect<T>::size > 0)) {
         constexpr auto& k_info = keys_info<T>;
         constexpr auto& type = k_info.type;
         constexpr auto& N = reflect<T>::size;
         constexpr auto& keys = reflect<T>::keys;

         using enum hash_type;
         if constexpr (type == single_element) {
            hash_info_t<T, bucket_size(single_element, N)> info{.type = single_element};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            return info;
         }
         else if constexpr (type == mod4) {
            hash_info_t<T, bucket_size(mod4, N)> info{.type = mod4};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            return info;
         }
         else if constexpr (type == xor_mod4) {
            hash_info_t<T, bucket_size(xor_mod4, N)> info{.type = xor_mod4};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            return info;
         }
         else if constexpr (type == minus_mod4) {
            hash_info_t<T, bucket_size(minus_mod4, N)> info{.type = minus_mod4};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            return info;
         }
         else if constexpr (type == three_element_unique_index) {
            hash_info_t<T, bucket_size(three_element_unique_index, N)> info{.type = three_element_unique_index};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            info.seed = k_info.seed;
            info.unique_index = k_info.unique_index;
            return info;
         }
         else if constexpr (type == front_hash) {
            constexpr auto bsize = bucket_size(front_hash, N);
            hash_info_t<T, bsize> info{.type = front_hash, .seed = k_info.seed};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            info.front_hash_bytes = k_info.front_hash_bytes;
            info.table.fill(uint8_t(N));

            for (uint8_t i = 0; i < N; ++i) {
               auto& key = keys[i];
               const auto h = [&]() -> size_t {
                  if (info.front_hash_bytes == sizeof(uint16_t)) {
                     return bitmix(uint16_t(key[0]) | (uint16_t(key[1]) << 8), info.seed) % bsize;
                  }
                  else if (info.front_hash_bytes == sizeof(uint32_t)) {
                     return bitmix(uint32_t(key[0]) //
                                      | (uint32_t(key[1]) << 8) //
                                      | (uint32_t(key[2]) << 16) //
                                      | (uint32_t(key[3]) << 24),
                                   info.seed) %
                            bsize;
                  }
                  else if (info.front_hash_bytes == sizeof(uint64_t)) {
                     return rich_bitmix(uint64_t(key[0]) //
                                           | (uint64_t(key[1]) << 8) //
                                           | (uint64_t(key[2]) << 16) //
                                           | (uint64_t(key[3]) << 24) //
                                           | (uint64_t(key[4]) << 32) //
                                           | (uint64_t(key[5]) << 40) //
                                           | (uint64_t(key[6]) << 48) //
                                           | (uint64_t(key[7]) << 56),
                                        info.seed) %
                            bsize;
                  }
                  else {
                     return 0; // MSVC has a compiler bug that prevents us from returning N, but this is unreachable
                  }
               }();
               info.table[h] = i;
            }

            return info;
         }
         else if constexpr (type == unique_index && N < 256) {
            hash_info_t<T, bucket_size(unique_index, N)> info{.type = unique_index, .seed = k_info.seed};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            info.table.fill(bucket_value_t<N>(N));
            info.unique_index = k_info.unique_index;

            if constexpr (k_info.sized_hash) {
               info.sized_hash = true;
               constexpr auto bsize = bucket_size(unique_index, N);
               for (uint8_t i = 0; i < N; ++i) {
                  const auto x = uint16_t(keys[i][k_info.unique_index]) | (uint16_t(keys[i].size()) << 8);
                  const auto h = bitmix(x, info.seed) % bsize;
                  info.table[h] = i;
               }
            }
            else {
               for (uint8_t i = 0; i < N; ++i) {
                  const auto h = uint8_t(keys[i][k_info.unique_index]);
                  info.table[h] = i;
               }
            }
            return info;
         }
         // TODO: MSVC fixed the related compiler bug, but GitHub Actions has not caught up yet
#if !defined(_MSC_VER)
         else if constexpr (type == unique_per_length) {
            hash_info_t<T, bucket_size(unique_per_length, N)> info{.type = unique_per_length, .seed = k_info.seed};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            info.table.fill(uint8_t(N));
            info.sized_hash = true;
            constexpr auto bsize = bucket_size(unique_per_length, N);
            constexpr auto& data = per_length_info<T>;
            for (uint8_t i = 0; i < N; ++i) {
               const auto n = keys[i].size();
               const auto x = uint16_t(keys[i][data.unique_index[n]]) | (uint16_t(n) << 8);
               const auto h = bitmix(x, info.seed) % bsize;
               info.table[h] = i;
            }
            return info;
         }
#endif
         else if constexpr (type == full_flat) {
            hash_info_t<T, bucket_size(full_flat, N)> info{.type = full_flat, .seed = k_info.seed};
            info.min_length = k_info.min_length;
            info.max_length = k_info.max_length;
            info.table.fill(uint8_t(N));
            constexpr auto bsize = bucket_size(full_flat, N);
            for (uint8_t i = 0; i < N; ++i) {
               const auto h = full_hash_impl(keys[i], info.seed, info.min_length, info.max_length) % bsize;
               info.table[h] = i;
            }
            return info;
         }
         else {
            // invalid
            return hash_info_t<T, 0>{};
         }
      }
      else {
         return hash_info_t<T, 0>{};
      }
   }();

   template <size_t min_length>
   GLZ_ALWAYS_INLINE constexpr const void* quote_memchr(auto&& it, auto end) noexcept
   {
      if consteval {
         const auto count = size_t(end - it);
         for (std::size_t i = 0; i < count; ++i) {
            if (it[i] == '"') {
               return it + i;
            }
         }
         return nullptr;
      }
      else {
         if constexpr (min_length >= 4) {
            // Skipping makes the bifurcation worth it
            const auto* start = it + min_length;
            if (start >= end) [[unlikely]] {
               return nullptr;
            }
            else [[likely]] {
               return std::memchr(start, '"', size_t(end - start));
            }
         }
         else {
            return std::memchr(it, '"', size_t(end - it));
         }
      }
   }

   template <uint32_t Format, class T, auto HashInfo, hash_type Type>
   struct decode_hash;

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::single_element>
   {
      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& /*it*/, auto&& /*end*/) noexcept { return 0; }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::mod4>
   {
      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&& /*end*/) noexcept { return uint8_t(*it) % 4; }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::xor_mod4>
   {
      static constexpr auto first_key_char = reflect<T>::keys[0][0];

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&& /*end*/) noexcept
      {
         return (uint8_t(*it) ^ first_key_char) % 4;
      }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::minus_mod4>
   {
      static constexpr auto first_key_char = reflect<T>::keys[0][0];

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&& /*end*/) noexcept
      {
         return (uint8_t(*it) - first_key_char) % 4;
      }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::unique_index>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::unique_index, N);
      static constexpr auto uindex = HashInfo.unique_index;

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto end) noexcept
      {
         if constexpr (HashInfo.sized_hash) {
            const auto* c = quote_memchr<HashInfo.min_length>(it, end);
            if (c) [[likely]] {
               const auto n = size_t(static_cast<std::decay_t<decltype(it)>>(c) - it);
               if (n == 0 || n > HashInfo.max_length) [[unlikely]] {
                  return N; // error
               }

               const auto h = bitmix(uint16_t(it[HashInfo.unique_index]) | (uint16_t(n) << 8), HashInfo.seed);
               return HashInfo.table[h % bsize];
            }
            else [[unlikely]] {
               return N;
            }
         }
         else {
            if constexpr (N == 2) {
               if constexpr (uindex > 0) {
                  if ((it + uindex) >= end) [[unlikely]] {
                     return N; // error
                  }
               }
               // Avoids using a hash table
               constexpr auto first_key_char = reflect<T>::keys[0][uindex];
               return size_t(bool(it[uindex] ^ first_key_char));
            }
            else {
               if constexpr (uindex > 0) {
                  if ((it + uindex) >= end) [[unlikely]] {
                     return N; // error
                  }
               }
               return HashInfo.table[uint8_t(it[uindex])];
            }
         }
      }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::three_element_unique_index>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto uindex = HashInfo.unique_index;

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto end) noexcept
      {
         if constexpr (uindex > 0) {
            if ((it + uindex) >= end) [[unlikely]] {
               return N; // error
            }
         }
         // Avoids using a hash table
         constexpr auto first_key_char = reflect<T>::keys[0][uindex];
         return (uint8_t(it[uindex] ^ first_key_char) * HashInfo.seed) % 4;
      }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::front_hash>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::front_hash, N);

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto end) noexcept
      {
         if constexpr (HashInfo.front_hash_bytes == 2) {
            if ((it + 2) >= end) [[unlikely]] {
               return N; // error
            }
            uint16_t h;
            if consteval {
               h = 0;
               for (size_t i = 0; i < 2; ++i) {
                  h |= static_cast<uint16_t>(static_cast<uint8_t>(it[i])) << (8 * i);
               }
            }
            else {
               std::memcpy(&h, it, 2);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
            }
            return HashInfo.table[bitmix(h, HashInfo.seed) % bsize];
         }
         else if constexpr (HashInfo.front_hash_bytes == 4) {
            if ((it + 4) >= end) [[unlikely]] {
               return N;
            }
            uint32_t h;
            if consteval {
               h = 0;
               for (size_t i = 0; i < 4; ++i) {
                  h |= static_cast<uint32_t>(static_cast<uint8_t>(it[i])) << (8 * i);
               }
            }
            else {
               std::memcpy(&h, it, 4);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
            }
            return HashInfo.table[bitmix(h, HashInfo.seed) % bsize];
         }
         else if constexpr (HashInfo.front_hash_bytes == 8) {
            if ((it + 8) >= end) [[unlikely]] {
               return N;
            }
            uint64_t h;
            if consteval {
               h = 0;
               for (size_t i = 0; i < 8; ++i) {
                  h |= static_cast<uint64_t>(static_cast<uint8_t>(it[i])) << (8 * i);
               }
            }
            else {
               std::memcpy(&h, it, 8);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
            }
            return HashInfo.table[rich_bitmix(h, HashInfo.seed) % bsize];
         }
         else {
            static_assert(false_v<T>, "invalid hash algorithm");
         }
      }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::unique_per_length>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::unique_per_length, N);

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto end) noexcept
      {
         const auto* c = quote_memchr<HashInfo.min_length>(it, end);
         if (c) [[likely]] {
            const auto n = uint8_t(static_cast<std::decay_t<decltype(it)>>(c) - it);
            const auto pos = per_length_info<T>.unique_index[n];
            if ((it + pos) >= end) [[unlikely]] {
               return N; // error
            }
            const auto h = bitmix(uint16_t(it[pos]) | (uint16_t(n) << 8), HashInfo.seed);
            return HashInfo.table[h % bsize];
         }
         else [[unlikely]] {
            return N;
         }
      }
   };

   template <class T, auto HashInfo>
   struct decode_hash<JSON, T, HashInfo, hash_type::full_flat>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::full_flat, N);
      static constexpr auto min_length = HashInfo.min_length;
      static constexpr auto max_length = HashInfo.max_length;
      static constexpr auto length_range = max_length - min_length;

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto end) noexcept
      {
         // For JSON we require at a minimum ":1} characters after a key (1 being a single char number)
         // This means that we can require all these characters to exist for SWAR parsing

         if constexpr (length_range == 0) {
            if ((it + min_length) >= end) [[unlikely]] {
               return N;
            }
            const auto h = full_hash<HashInfo.min_length, HashInfo.max_length, HashInfo.seed>(it, min_length);
            return HashInfo.table[h % bsize];
         }
         else {
            if constexpr (length_range == 1) {
               auto quote = it + min_length;
               if ((quote + 1) >= end) [[unlikely]] {
                  return N;
               }

               const auto n = min_length + uint8_t(*quote != '"');
               const auto h = full_hash<HashInfo.min_length, HashInfo.max_length, HashInfo.seed>(it, n);
               return HashInfo.table[h % bsize];
            }
            else {
               const auto* c = quote_memchr<HashInfo.min_length>(it, end);
               if (c) [[likely]] {
                  const auto n = uint8_t(static_cast<std::decay_t<decltype(it)>>(c) - it);
                  const auto h = full_hash<HashInfo.min_length, HashInfo.max_length, HashInfo.seed>(it, n);
                  return HashInfo.table[h % bsize];
               }
               else [[unlikely]] {
                  return N;
               }
            }
         }
      }
   };

   template <uint32_t Format, class T, auto HashInfo, hash_type Type>
   struct decode_hash_with_size;

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::single_element>
   {
      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&&, auto&&, const size_t) noexcept { return 0; }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::mod4>
   {
      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&&, const size_t) noexcept
      {
         return uint8_t(*it) % 4;
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::xor_mod4>
   {
      static constexpr auto first_key_char = reflect<T>::keys[0][0];

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&&, const size_t) noexcept
      {
         return (uint8_t(*it) ^ first_key_char) % 4;
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::minus_mod4>
   {
      static constexpr auto first_key_char = reflect<T>::keys[0][0];

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&&, const size_t) noexcept
      {
         return (uint8_t(*it) - first_key_char) % 4;
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::unique_index>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::unique_index, N);
      static constexpr auto uindex = HashInfo.unique_index;

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&& end, const size_t n) noexcept
      {
         if constexpr (HashInfo.sized_hash) {
            if (n == 0 || n > HashInfo.max_length) {
               return N; // error
            }

            const auto h = bitmix(uint16_t(it[HashInfo.unique_index]) | (uint16_t(n) << 8), HashInfo.seed);
            return HashInfo.table[h % bsize];
         }
         else {
            if constexpr (N == 2) {
               if ((it + uindex) >= end) [[unlikely]] {
                  return N; // error
               }
               // Avoids using a hash table
               constexpr auto first_key_char = reflect<T>::keys[0][uindex];
               return size_t(bool(it[uindex] ^ first_key_char));
            }
            else {
               if ((it + uindex) >= end) [[unlikely]] {
                  return N; // error
               }
               return HashInfo.table[uint8_t(it[uindex])];
            }
         }
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::three_element_unique_index>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto uindex = HashInfo.unique_index;

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&& end, const size_t) noexcept
      {
         if constexpr (uindex > 0) {
            if ((it + uindex) >= end) [[unlikely]] {
               return N; // error
            }
         }
         // Avoids using a hash table
         constexpr auto first_key_char = reflect<T>::keys[0][uindex];
         return (uint8_t(it[uindex] ^ first_key_char) * HashInfo.seed) % 4;
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::front_hash>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::front_hash, N);

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&&, const size_t) noexcept
      {
         if constexpr (HashInfo.front_hash_bytes == 2) {
            uint16_t h;
            if consteval {
               h = 0;
               for (size_t i = 0; i < 2; ++i) {
                  h |= static_cast<uint16_t>(static_cast<uint8_t>(it[i])) << (8 * i);
               }
            }
            else {
               std::memcpy(&h, it, 2);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
            }
            return HashInfo.table[bitmix(h, HashInfo.seed) % bsize];
         }
         else if constexpr (HashInfo.front_hash_bytes == 4) {
            uint32_t h;
            if consteval {
               h = 0;
               for (size_t i = 0; i < 4; ++i) {
                  h |= static_cast<uint32_t>(static_cast<uint8_t>(it[i])) << (8 * i);
               }
            }
            else {
               std::memcpy(&h, it, 4);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
            }
            return HashInfo.table[bitmix(h, HashInfo.seed) % bsize];
         }
         else if constexpr (HashInfo.front_hash_bytes == 8) {
            uint64_t h;
            if consteval {
               h = 0;
               for (size_t i = 0; i < 8; ++i) {
                  h |= static_cast<uint64_t>(static_cast<uint8_t>(it[i])) << (8 * i);
               }
            }
            else {
               std::memcpy(&h, it, 8);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
            }
            return HashInfo.table[rich_bitmix(h, HashInfo.seed) % bsize];
         }
         else {
            static_assert(false_v<T>, "invalid hash algorithm");
         }
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::unique_per_length>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::unique_per_length, N);

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&& end, const size_t n) noexcept
      {
         const auto pos = per_length_info<T>.unique_index[uint8_t(n)];
         if ((it + pos) >= end) [[unlikely]] {
            return N; // error
         }
         const auto h = bitmix(uint16_t(it[pos]) | (uint16_t(n) << 8), HashInfo.seed);
         return HashInfo.table[h % bsize];
      }
   };

   template <uint32_t Format, class T, auto HashInfo>
   struct decode_hash_with_size<Format, T, HashInfo, hash_type::full_flat>
   {
      static constexpr auto N = reflect<T>::size;
      static constexpr auto bsize = bucket_size(hash_type::full_flat, N);

      GLZ_ALWAYS_INLINE static constexpr size_t op(auto&& it, auto&&, const size_t n) noexcept
      {
         const auto h = full_hash<HashInfo.min_length, HashInfo.max_length, HashInfo.seed>(it, n);
         return HashInfo.table[h % bsize];
      }
   };

   // ============================================================================
   // variant_deduction: Maps field keys to bit arrays for variant type deduction
   // Uses compile-time decode_hash for O(1) lookup
   // ============================================================================

   // Number of unique keys from all variant types
   template <is_variant T>
   constexpr size_t variant_deduction_key_count = get_combined_keys_from_variant<T>().second;

   // Array of unique keys (sorted) from all variant types
   template <is_variant T>
   constexpr auto variant_deduction_keys = []() {
      constexpr auto pair = get_combined_keys_from_variant<T>();
      std::array<sv, variant_deduction_key_count<T>> result{};
      for (size_t i = 0; i < variant_deduction_key_count<T>; ++i) {
         result[i] = pair.first[i];
      }
      return result;
   }();

   // Variant deduction bits - for each unique key, tracks which variant types contain it
   template <is_variant T>
   constexpr auto variant_deduction_bits = []() {
      static constexpr size_t K = variant_deduction_key_count<T>;
      using bits_type = bit_array<std::variant_size_v<T>>;
      std::array<bits_type, K> bits{};

      if constexpr (K > 0) {
         using keys_t = keys_wrapper<variant_deduction_keys<T>>;
         constexpr auto& HashInfo = hash_info<keys_t>;

         // Populate bit arrays - for each key, set bits for variant types that have it
         for_each<std::variant_size_v<T>>([&]<auto I>() {
            using V = decay_keep_volatile_t<std::variant_alternative_t<I, T>>;
            if constexpr (glaze_object_t<V> || reflectable<V> || is_memory_object<V>) {
               using X = std::conditional_t<is_memory_object<V>, memory_type<V>, V>;
               constexpr auto Size = reflect<X>::size;
               if constexpr (Size > 0) {
                  for (size_t J = 0; J < Size; ++J) {
                     sv key = reflect<X>::keys[J];
                     const auto index = decode_hash_with_size<JSON, keys_t, HashInfo, HashInfo.type>::op(
                        key.data(), key.data() + key.size(), key.size());
                     if (index < K) {
                        bits[index][I] = true;
                     }
                  }
               }
            }
         });
      }

      return bits;
   }();

   // ============================================================================
   // variant_id_to_index: Maps variant type IDs to indices using perfect hashing
   // ============================================================================

   // Helper to create int_keys_info from ids_v<T> for integral variant IDs
   template <is_variant T>
      requires(std::integral<std::decay_t<decltype(ids_v<T>[0])>>)
   constexpr auto make_variant_int_keys_info()
   {
      using U = std::decay_t<decltype(ids_v<T>[0])>;
      constexpr auto N = ids_v<T>.size();

      if constexpr (N == 0) {
         return int_keys_info_t<0, 0>{};
      }
      else if constexpr (N == 1) {
         // For single-element IDs, check if value is 0 (direct) or needs offset
         constexpr auto value = static_cast<U>(ids_v<T>[0]);
         if constexpr (value == 0) {
            return int_keys_info_t<1, 0>{.type = int_hash_type::direct};
         }
         else {
            return int_keys_info_t<1, 0>{.type = int_hash_type::offset, .min_value = static_cast<int64_t>(value)};
         }
      }
      else {
         // Extract values from ids_v<T>
         constexpr auto vals = []() constexpr {
            constexpr auto size = ids_v<T>.size();
            std::array<U, size> result{};
            for (size_t i = 0; i < size; ++i) {
               result[i] = ids_v<T>[i];
            }
            return result;
         }();

         // Find min/max
         constexpr auto min_max = [&]() {
            U min_val = vals[0], max_val = vals[0];
            for (const auto v : vals) {
               if (v < min_val) min_val = v;
               if (v > max_val) max_val = v;
            }
            return std::pair{min_val, max_val};
         }();

         constexpr U min_val = min_max.first;
         constexpr U max_val = min_max.second;
         constexpr auto range = static_cast<size_t>(max_val - min_val);

         // Check for sequential IDs
         constexpr bool is_sequential = [&]() {
            if (range + 1 != N) return false;
            std::array<bool, N> seen{};
            for (const auto v : vals) {
               const auto idx = static_cast<size_t>(v - min_val);
               if (idx >= N || seen[idx]) return false;
               seen[idx] = true;
            }
            return true;
         }();

         // Check for powers of 2
         constexpr auto power_of_two_info = [&]() {
            using UnsignedU = std::make_unsigned_t<U>;
            size_t max_bit = 0;

            for (const auto v : vals) {
               const auto uv = static_cast<UnsignedU>(v);
               if (uv == 0 || !std::has_single_bit(uv)) {
                  return std::pair{false, size_t{0}};
               }
               const auto bit = static_cast<size_t>(std::countr_zero(uv));
               if (bit > max_bit) max_bit = bit;
            }

            std::array<bool, 64> used{};
            for (const auto v : vals) {
               const auto bit = std::countr_zero(static_cast<UnsignedU>(v));
               if (used[bit]) return std::pair{false, size_t{0}};
               used[bit] = true;
            }

            return std::pair{true, max_bit + 1};
         }();

         constexpr size_t sparse_threshold = 256;
         constexpr auto table_size = std::bit_ceil(N * 2);

         if constexpr (is_sequential) {
            if constexpr (min_val == 0) {
               return int_keys_info_t<N, 0>{.type = int_hash_type::direct};
            }
            else {
               return int_keys_info_t<N, 0>{.type = int_hash_type::offset, .min_value = min_val};
            }
         }
         else if constexpr (power_of_two_info.first) {
            constexpr auto tbl_size = power_of_two_info.second;
            int_keys_info_t<N, tbl_size> info{.type = int_hash_type::power_of_two, .table_size = tbl_size};
            info.table.fill(static_cast<uint8_t>(N));

            using UnsignedU = std::make_unsigned_t<U>;
            for (size_t i = 0; i < N; ++i) {
               const auto bit = std::countr_zero(static_cast<UnsignedU>(vals[i]));
               info.table[bit] = static_cast<uint8_t>(i);
            }
            return info;
         }
         else if constexpr (range < sparse_threshold) {
            int_keys_info_t<N, sparse_threshold> info{
               .type = int_hash_type::small_range, .min_value = min_val, .max_value = max_val, .table_size = range + 1};
            info.table.fill(static_cast<uint8_t>(N));

            for (size_t i = 0; i < N; ++i) {
               info.table[static_cast<size_t>(vals[i] - min_val)] = static_cast<uint8_t>(i);
            }
            return info;
         }
         else {
            // Strategy 4: Modular perfect hash (fallback)
            // First try standard modular hash (faster lookup)
            constexpr auto modular_info = [&]() {
               for (const auto prime : primes_64) {
                  std::array<bool, table_size> used{};
                  bool collision = false;

                  for (size_t i = 0; i < N; ++i) {
                     const auto h = (static_cast<uint64_t>(vals[i]) * prime) % table_size;
                     if (used[h]) {
                        collision = true;
                        break;
                     }
                     used[h] = true;
                  }

                  if (!collision) {
                     return std::pair{true, prime};
                  }
               }
               return std::pair{false, uint64_t{0}};
            }();

            if constexpr (modular_info.first) {
               // Standard modular hash works
               int_keys_info_t<N, table_size> info{
                  .type = int_hash_type::modular, .seed = modular_info.second, .table_size = table_size};
               info.table.fill(static_cast<uint8_t>(N));

               for (size_t i = 0; i < N; ++i) {
                  const auto h = (static_cast<uint64_t>(vals[i]) * info.seed) % table_size;
                  info.table[h] = static_cast<uint8_t>(i);
               }
               return info;
            }
            else {
               // Strategy 5: Modular hash with shift for sparse values with common power-of-2 factors
               constexpr uint8_t common_shift = [&]() -> uint8_t {
                  uint8_t min_trailing = 64;
                  for (const auto v : vals) {
                     if (v != 0) {
                        const auto trailing = static_cast<uint8_t>(std::countr_zero(static_cast<uint64_t>(v)));
                        if (trailing < min_trailing) {
                           min_trailing = trailing;
                        }
                     }
                  }
                  return min_trailing == 64 ? 0 : min_trailing;
               }();

               constexpr auto shifted_info = [&]() {
                  for (const auto prime : primes_64) {
                     std::array<bool, table_size> used{};
                     bool collision = false;

                     for (size_t i = 0; i < N; ++i) {
                        const auto shifted = static_cast<uint64_t>(vals[i]) >> common_shift;
                        const auto h = (shifted * prime) % table_size;
                        if (used[h]) {
                           collision = true;
                           break;
                        }
                        used[h] = true;
                     }

                     if (!collision) {
                        return std::pair{true, prime};
                     }
                  }
                  return std::pair{false, uint64_t{0}};
               }();

               static_assert(shifted_info.first, "Failed to find perfect hash seed for variant int IDs");

               int_keys_info_t<N, table_size> info{.type = int_hash_type::modular_shifted,
                                                   .seed = shifted_info.second,
                                                   .table_size = table_size,
                                                   .shift = common_shift};
               info.table.fill(static_cast<uint8_t>(N));

               for (size_t i = 0; i < N; ++i) {
                  const auto shifted = static_cast<uint64_t>(vals[i]) >> common_shift;
                  const auto h = (shifted * info.seed) % table_size;
                  info.table[h] = static_cast<uint8_t>(i);
               }
               return info;
            }
         }
      }
   }

   // Primary template for variant_id_to_index
   template <is_variant T, bool IsIntegral = std::integral<std::decay_t<decltype(ids_v<T>[0])>>>
   struct variant_id_to_index;

   // Specialization for string IDs
   template <is_variant T>
   struct variant_id_to_index<T, false>
   {
      using keys_t = keys_wrapper<ids_v<T>>;
      static constexpr auto& HashInfo = hash_info<keys_t>;
      static constexpr auto N = ids_v<T>.size();

      GLZ_ALWAYS_INLINE static constexpr size_t op(const char* data, const char* end, size_t n) noexcept
      {
         const auto index = decode_hash_with_size<JSON, keys_t, HashInfo, HashInfo.type>::op(data, end, n);
         // Verify the key matches to avoid false positives from hash collisions
         if (index < N) {
            const sv key{data, n};
            if (ids_v<T>[index] == key) {
               return index;
            }
         }
         return N; // Not found
      }
   };

   // Specialization for integral IDs
   template <is_variant T>
   struct variant_id_to_index<T, true>
   {
      using U = std::decay_t<decltype(ids_v<T>[0])>;
      static constexpr auto Info = make_variant_int_keys_info<T>();
      static constexpr auto N = ids_v<T>.size();

      GLZ_ALWAYS_INLINE static constexpr size_t op(U id) noexcept
      {
         using enum int_hash_type;

         if constexpr (Info.type == direct) {
            return static_cast<size_t>(id);
         }
         else if constexpr (Info.type == offset) {
            return static_cast<size_t>(id - Info.min_value);
         }
         else if constexpr (Info.type == power_of_two) {
            using UnsignedU = std::make_unsigned_t<U>;
            const auto uv = static_cast<UnsignedU>(id);
            if (uv == 0 || !std::has_single_bit(uv)) [[unlikely]] {
               return N;
            }
            const auto bit = static_cast<size_t>(std::countr_zero(uv));
            if (bit >= Info.table_size) [[unlikely]] {
               return N;
            }
            return Info.table[bit];
         }
         else if constexpr (Info.type == small_range) {
            const auto idx = static_cast<int64_t>(id) - Info.min_value;
            if (idx < 0 || static_cast<size_t>(idx) >= Info.table_size) [[unlikely]] {
               return N;
            }
            return Info.table[static_cast<size_t>(idx)];
         }
         else if constexpr (Info.type == modular) {
            const auto h = (static_cast<uint64_t>(id) * Info.seed) % Info.table_size;
            return Info.table[h];
         }
         else { // modular_shifted
            const auto shifted = static_cast<uint64_t>(id) >> Info.shift;
            const auto h = (shifted * Info.seed) % Info.table_size;
            return Info.table[h];
         }
      }
   };

   // ============================================================================
   // End of variant_id_to_index
   // ============================================================================
}

namespace glz
{
   [[nodiscard]] inline std::string format_error(const error_code& ec)
   {
      return std::string{meta<error_code>::keys[uint32_t(ec)]};
   }

   [[nodiscard]] inline std::string format_error(const error_ctx& pe)
   {
      std::string error_str{meta<error_code>::keys[uint32_t(pe.ec)]};
      if (pe.custom_error_message.size()) {
         error_str.append(" ");
         error_str.append(pe.custom_error_message.begin(), pe.custom_error_message.end());
      }
      return error_str;
   }

   [[nodiscard]] inline std::string format_error(const error_ctx& pe, const auto& buffer)
   {
      const auto error_type_str = meta<error_code>::keys[uint32_t(pe.ec)];

      const auto info = detail::get_source_info(buffer, pe.count);
      auto error_str = detail::generate_error_string(error_type_str, info);
      if (pe.custom_error_message.size()) {
         error_str.append(" ");
         error_str.append(pe.custom_error_message.begin(), pe.custom_error_message.end());
      }
      return error_str;
   }

   template <class T>
   [[nodiscard]] std::string format_error(const expected<T, error_ctx>& pe, const auto& buffer)
   {
      if (not pe) {
         return format_error(pe.error(), buffer);
      }
      else {
         return "";
      }
   }

   template <class T>
   [[nodiscard]] std::string format_error(const expected<T, error_ctx>& pe)
   {
      if (not pe) {
         return format_error(pe.error());
      }
      else {
         return "";
      }
   }

   template <class T>
   inline constexpr size_t maximum_key_size = [] {
      constexpr auto N = reflect<T>::size;
      size_t maximum{};
      for (size_t i = 0; i < N; ++i) {
         if (reflect<T>::keys[i].size() > maximum) {
            maximum = reflect<T>::keys[i].size();
         }
      }
      return maximum + 2; // add quotes for JSON
   }();

   inline constexpr uint64_t round_up_to_nearest_16(const uint64_t value) noexcept { return (value + 15) & ~15ull; }
}

namespace glz
{
   // The Callable comes second as ranges::for_each puts the callable at the end

   template <class Callable, reflectable T>
   void for_each_field(T&& value, Callable&& callable)
   {
      constexpr auto N = reflect<T>::size;
      if constexpr (N > 0) {
         [&]<size_t... I>(std::index_sequence<I...>) constexpr {
            (callable(get_member(value, get<I>(to_tie(value)))), ...);
         }(std::make_index_sequence<N>{});
      }
   }

   template <class Callable, glaze_object_t T>
   void for_each_field(T&& value, Callable&& callable)
   {
      constexpr auto N = reflect<T>::size;
      if constexpr (N > 0) {
         [&]<size_t... I>(std::index_sequence<I...>) constexpr {
            (callable(get_member(value, get<I>(reflect<T>::values))), ...);
         }(std::make_index_sequence<N>{});
      }
   }

   // Check if a type has a member with a specific name
   template <class T>
   consteval bool has_member_with_name(const sv& name) noexcept
   {
      if constexpr (reflectable<T> || glaze_object_t<T>) {
         constexpr auto N = reflect<T>::size;
         for (size_t i = 0; i < N; ++i) {
            if (reflect<T>::keys[i] == name) {
               return true;
            }
         }
      }
      return false;
   }

   // Concept to check if glz::reflect<T> can be instantiated and used
   // This concept is automatically satisfied by any type that has a valid reflect<T> specialization
   template <class T>
   concept has_reflect = requires {
      sizeof(reflect<T>); // Ensure reflect<T> is complete
      { reflect<T>::size } -> std::convertible_to<std::size_t>;
   };
}

#if defined(_MSC_VER) && !defined(__clang__)
// restore disabled warnings
#pragma warning(pop)
#endif
