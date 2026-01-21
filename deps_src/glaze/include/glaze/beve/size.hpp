// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/beve/header.hpp"
#include "glaze/beve/key_traits.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/util/expected.hpp"
#include "glaze/util/for_each.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   // Calculate the number of bytes needed to store a compressed integer
   [[nodiscard]] GLZ_ALWAYS_INLINE constexpr size_t compressed_int_size(uint64_t i) noexcept
   {
      if (i < 64) return 1;
      if (i < 16384) return 2;
      if (i < 1073741824) return 4;
      return 8;
   }

   // Compile-time version for known values
   template <uint64_t i>
   [[nodiscard]] consteval size_t compressed_int_size() noexcept
   {
      if constexpr (i < 64)
         return 1;
      else if constexpr (i < 16384)
         return 2;
      else if constexpr (i < 1073741824)
         return 4;
      else
         return 8;
   }

   // Forward declaration for the size calculation template
   template <uint32_t Format, class T>
   struct calculate_size;

   // Primary template for BEVE size calculation dispatch
   template <>
   struct calculate_size<BEVE, void>
   {
      template <auto Opts, class T>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(T&& value)
      {
         return calculate_size<BEVE, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value));
      }

      template <auto Opts, class T>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t no_header(T&& value)
      {
         return calculate_size<BEVE, std::remove_cvref_t<T>>::template no_header<Opts>(std::forward<T>(value));
      }
   };

   template <class T>
      requires(glaze_value_t<T> && !custom_write<T>)
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<decltype(value)>(), meta_wrapper_v<T>))>;
         return calculate_size<BEVE, V>::template op<Opts>(get_member(value, meta_wrapper_v<T>));
      }

      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t no_header(auto&& value)
      {
         using V = std::remove_cvref_t<decltype(get_member(std::declval<decltype(value)>(), meta_wrapper_v<T>))>;
         return calculate_size<BEVE, V>::template no_header<Opts>(get_member(value, meta_wrapper_v<T>));
      }
   };

   template <always_null_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         return 1; // null tag
      }
   };

   template <is_bitset T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value) noexcept
      {
         const auto num_bytes = (value.size() + 7) / 8;
         return 1 + compressed_int_size(value.size()) + num_bytes; // tag + size + data
      }
   };

   template <glaze_flags_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&&) noexcept
      {
         static constexpr auto data_size = byte_length<T>();
         return data_size; // flags are written directly without header
      }
   };

   template <is_member_function_pointer T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         return 0; // member function pointers are not serialized
      }
   };

   template <is_includer T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         return 1 + 1; // tag + compressed_int(0) for empty string
      }
   };

   template <boolean_like T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         return 1; // bool tag contains the value
      }
   };

   template <func_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         return calculate_size<BEVE, void>::template op<Opts>(name_v<std::decay_t<decltype(value)>>);
      }
   };

   template <class T>
   struct calculate_size<BEVE, basic_raw_json<T>>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         return calculate_size<BEVE, void>::template op<Opts>(value.str);
      }
   };

   template <class T>
   struct calculate_size<BEVE, basic_text<T>>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         return calculate_size<BEVE, void>::template op<Opts>(value.str);
      }
   };

   template <is_variant T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value)
      {
         return std::visit(
            [&](auto&& v) -> size_t {
               using V = std::decay_t<decltype(v)>;
               using Variant = std::decay_t<decltype(value)>;
               static constexpr uint64_t index = variant_index_v<V, Variant>;

               // 1 byte tag + compressed index + value size
               return 1 + compressed_int_size<index>() + calculate_size<BEVE, void>::template op<Opts>(v);
            },
            value);
      }
   };

   template <class T>
      requires num_t<T> || char_t<T> || glaze_enum_t<T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         return 1 + sizeof(T); // tag + value
      }

      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t no_header(auto&&) noexcept
      {
         return sizeof(T); // value only
      }
   };

   template <class T>
      requires(std::is_enum_v<T> && !glaze_enum_t<T>)
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         using V = std::underlying_type_t<std::decay_t<T>>;
         return 1 + sizeof(V); // tag + value
      }

      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t no_header(auto&&) noexcept
      {
         using V = std::underlying_type_t<std::decay_t<T>>;
         return sizeof(V); // value only
      }
   };

   template <complex_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&&) noexcept
      {
         using V = typename T::value_type;
         return 2 + 2 * sizeof(V); // extension tag + complex header + real + imag
      }

      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t no_header(auto&&) noexcept
      {
         using V = typename T::value_type;
         return 2 * sizeof(V); // real + imag
      }
   };

   template <str_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         const sv str = [&]() -> const sv {
            if constexpr (!char_array_t<T> && std::is_pointer_v<std::decay_t<T>>) {
               return value ? value : "";
            }
            else {
               return sv{value};
            }
         }();

         const auto n = str.size();
         return 1 + compressed_int_size(n) + n; // tag + length + data
      }

      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t no_header(auto&& value)
      {
         const auto n = value.size();
         return compressed_int_size(n) + n; // length + data
      }

      template <uint64_t N, auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static constexpr size_t no_header_cx() noexcept
      {
         return compressed_int_size<N>() + N; // length + data
      }
   };

   template <writable_array_t T>
   struct calculate_size<BEVE, T>
   {
      static constexpr bool map_like_array = pair_t<range_value_t<T>>;

      template <auto Opts>
         requires(map_like_array ? check_concatenate(Opts) == false : true)
      [[nodiscard]] static size_t op(auto&& value)
      {
         using V = range_value_t<std::decay_t<T>>;

         size_t result = 1; // tag byte
         result += compressed_int_size(value.size()); // element count

         if constexpr (boolean_like<V>) {
            const auto num_bytes = (value.size() + 7) / 8;
            result += num_bytes;
         }
         else if constexpr (num_t<V>) {
            result += value.size() * sizeof(V);
         }
         else if constexpr (str_t<V>) {
            for (auto& x : value) {
               result += compressed_int_size(x.size()) + x.size();
            }
         }
         else if constexpr (complex_t<V>) {
            // extension tag + complex header + count + data
            using X = typename V::value_type;
            result += 1; // complex_header byte
            result += value.size() * 2 * sizeof(X);
         }
         else {
            // generic array - need to sum element sizes
            for (auto&& x : value) {
               result += calculate_size<BEVE, void>::template op<Opts>(x);
            }
         }

         return result;
      }

      template <auto Opts>
         requires(map_like_array && check_concatenate(Opts) == true)
      [[nodiscard]] static size_t op(auto&& value)
      {
         size_t result = 1; // tag byte
         result += compressed_int_size(value.size()); // element count

         for (auto&& [k, v] : value) {
            result += calculate_size<BEVE, void>::template no_header<Opts>(k);
            result += calculate_size<BEVE, void>::template op<Opts>(v);
         }

         return result;
      }
   };

   template <pair_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         size_t result = 1; // tag byte
         result += compressed_int_size<1>(); // count = 1

         const auto& [k, v] = value;
         result += calculate_size<BEVE, void>::template no_header<Opts>(k);
         result += calculate_size<BEVE, void>::template op<Opts>(v);

         return result;
      }
   };

   template <writable_map_t T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value)
      {
         size_t result = 1; // tag byte
         result += compressed_int_size(value.size()); // element count

         for (auto&& [k, v] : value) {
            result += calculate_size<BEVE, void>::template no_header<Opts>(k);
            result += calculate_size<BEVE, void>::template op<Opts>(v);
         }

         return result;
      }
   };

   template <nullable_t T>
      requires(std::is_array_v<T>)
   struct calculate_size<BEVE, T>
   {
      template <auto Opts, class V, size_t N>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(const V (&value)[N])
      {
         return calculate_size<BEVE, void>::template op<Opts>(std::span{value, N});
      }
   };

   template <nullable_t T>
      requires(!std::is_array_v<T>)
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         if (value) {
            return calculate_size<BEVE, void>::template op<Opts>(*value);
         }
         else {
            return 1; // null tag
         }
      }
   };

   template <class T>
      requires(nullable_value_t<T> && not nullable_like<T> && not is_expected<T>)
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] GLZ_ALWAYS_INLINE static size_t op(auto&& value)
      {
         if (value.has_value()) {
            return calculate_size<BEVE, void>::template op<Opts>(value.value());
         }
         else {
            return 1; // null tag
         }
      }
   };

   template <class T>
      requires is_specialization_v<T, glz::obj> || is_specialization_v<T, glz::obj_copy>
   struct calculate_size<BEVE, T>
   {
      template <auto Options>
      [[nodiscard]] static size_t op(auto&& value)
      {
         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V> / 2;

         size_t result = 0;

         if constexpr (!check_opening_handled(Options)) {
            result += 1; // object tag
            result += compressed_int_size<N>(); // member count
         }

         for_each<N>([&]<size_t I>() {
            constexpr auto Opts = opening_handled_off<Options>();
            result += calculate_size<BEVE, void>::template no_header<Opts>(get<2 * I>(value.value)); // key
            result += calculate_size<BEVE, void>::template op<Opts>(get<2 * I + 1>(value.value)); // value
         });

         return result;
      }
   };

   template <class T>
      requires is_specialization_v<T, glz::merge>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts, class Value, size_t I>
      static consteval bool should_skip_field()
      {
         using V = field_t<Value, I>;

         if constexpr (std::same_as<V, hidden> || std::same_as<V, skip>) {
            return true;
         }
         else if constexpr (is_member_function_pointer<V>) {
            return !check_write_function_pointers(Opts);
         }
         else {
            return false;
         }
      }

      template <auto Opts, class Value>
      static consteval size_t count_fields_for_type()
      {
         constexpr auto N = reflect<Value>::size;
         return []<size_t... I>(std::index_sequence<I...>) {
            return (size_t{} + ... + (should_skip_field<Opts, Value, I>() ? size_t{} : size_t{1}));
         }(std::make_index_sequence<N>{});
      }

      template <auto Opts>
      static consteval size_t merge_element_count()
      {
         size_t count{};
         using Tuple = std::decay_t<decltype(std::declval<T>().value)>;
         for_each<glz::tuple_size_v<Tuple>>([&]<auto I>() constexpr {
            using Value = std::decay_t<glz::tuple_element_t<I, Tuple>>;
            if constexpr (is_specialization_v<Value, glz::obj> || is_specialization_v<Value, glz::obj_copy>) {
               count += glz::tuple_size_v<decltype(std::declval<Value>().value)> / 2;
            }
            else {
               count += count_fields_for_type<Opts, Value>();
            }
         });
         return count;
      }

      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value)
      {
         using V = std::decay_t<decltype(value.value)>;
         static constexpr auto N = glz::tuple_size_v<V>;

         size_t result = 1; // object tag
         result += compressed_int_size<merge_element_count<Opts>()>(); // member count

         [&]<size_t... I>(std::index_sequence<I...>) {
            ((result += calculate_size<BEVE, void>::template op<opening_handled<Opts>()>(glz::get<I>(value.value))),
             ...);
         }(std::make_index_sequence<N>{});

         return result;
      }
   };

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_write<T>)
   struct calculate_size<BEVE, T>
   {
      static constexpr auto N = reflect<T>::size;

      template <auto Opts, size_t I>
      static consteval bool should_skip_field()
      {
         using V = field_t<T, I>;

         if constexpr (std::same_as<V, hidden> || std::same_as<V, skip>) {
            return true;
         }
         else if constexpr (is_member_function_pointer<V>) {
            return !check_write_function_pointers(Opts);
         }
         else {
            return false;
         }
      }

      template <auto Opts>
      static consteval size_t count_to_write()
      {
         return []<size_t... I>(std::index_sequence<I...>) {
            return (size_t{} + ... + (should_skip_field<Opts, I>() ? size_t{} : size_t{1}));
         }(std::make_index_sequence<N>{});
      }

      template <auto Opts>
         requires(check_structs_as_arrays(Opts) == true)
      [[nodiscard]] static size_t op(auto&& value)
      {
         size_t result = 1; // generic_array tag
         result += compressed_int_size<count_to_write<Opts>()>(); // element count

         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         for_each<N>([&]<size_t I>() {
            if constexpr (should_skip_field<Opts, I>()) {
               return;
            }
            else {
               if constexpr (reflectable<T>) {
                  result += calculate_size<BEVE, void>::template op<Opts>(get_member(value, get<I>(t)));
               }
               else {
                  result +=
                     calculate_size<BEVE, void>::template op<Opts>(get_member(value, get<I>(reflect<T>::values)));
               }
            }
         });

         return result;
      }

      template <auto Options>
         requires(check_structs_as_arrays(Options) == false)
      [[nodiscard]] static size_t op(auto&& value)
      {
         constexpr auto Opts = opening_handled_off<Options>();

         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         size_t result = 0;

         if constexpr (maybe_skipped<Options, T>) {
            // Dynamic path: count members at runtime to handle skip_null_members
            size_t member_count = 0;

            // First pass: count members that will be written
            for_each<N>([&]<size_t I>() {
               if constexpr (should_skip_field<Options, I>()) {
                  return;
               }
               else {
                  using val_t = field_t<T, I>;

                  if constexpr (null_t<val_t> && Options.skip_null_members) {
                     if constexpr (always_null_t<val_t>) {
                        return;
                     }
                     else {
                        const auto is_null = [&]() {
                           decltype(auto) element = [&]() -> decltype(auto) {
                              if constexpr (reflectable<T>) {
                                 return get<I>(t);
                              }
                              else {
                                 return get<I>(reflect<T>::values);
                              }
                           };

                           if constexpr (nullable_wrapper<val_t>) {
                              return !bool(element()(value).val);
                           }
                           else if constexpr (nullable_value_t<val_t>) {
                              return !get_member(value, element()).has_value();
                           }
                           else {
                              return !bool(get_member(value, element()));
                           }
                        }();
                        if (!is_null) {
                           ++member_count;
                        }
                     }
                  }
                  else {
                     ++member_count;
                  }
               }
            });

            // Write header with dynamic count
            if constexpr (!check_opening_handled(Options)) {
               result += 1; // object tag
               result += compressed_int_size(member_count); // member count
            }

            // Second pass: calculate member sizes
            for_each<N>([&]<size_t I>() {
               if constexpr (should_skip_field<Options, I>()) {
                  return;
               }
               else {
                  using val_t = field_t<T, I>;

                  if constexpr (null_t<val_t> && Options.skip_null_members) {
                     if constexpr (always_null_t<val_t>) {
                        return;
                     }
                     else {
                        const auto is_null = [&]() {
                           decltype(auto) element = [&]() -> decltype(auto) {
                              if constexpr (reflectable<T>) {
                                 return get<I>(t);
                              }
                              else {
                                 return get<I>(reflect<T>::values);
                              }
                           };

                           if constexpr (nullable_wrapper<val_t>) {
                              return !bool(element()(value).val);
                           }
                           else if constexpr (nullable_value_t<val_t>) {
                              return !get_member(value, element()).has_value();
                           }
                           else {
                              return !bool(get_member(value, element()));
                           }
                        }();
                        if (is_null) {
                           return;
                        }
                     }
                  }

                  static constexpr sv key = reflect<T>::keys[I];
                  result += calculate_size<BEVE, std::remove_cvref_t<decltype(key)>>::template no_header_cx<key.size(),
                                                                                                            Opts>();

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  result += calculate_size<BEVE, void>::template op<Opts>(get_member(value, member));
               }
            });
         }
         else {
            // Static path: use compile-time count for better performance
            if constexpr (!check_opening_handled(Options)) {
               result += 1; // object tag
               result += compressed_int_size<count_to_write<Options>()>(); // member count
            }

            for_each<N>([&]<size_t I>() {
               if constexpr (should_skip_field<Options, I>()) {
                  return;
               }
               else {
                  static constexpr sv key = reflect<T>::keys[I];
                  result += calculate_size<BEVE, std::remove_cvref_t<decltype(key)>>::template no_header_cx<key.size(),
                                                                                                            Opts>();

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  result += calculate_size<BEVE, void>::template op<Opts>(get_member(value, member));
               }
            });
         }

         return result;
      }
   };

   template <class T>
      requires glaze_array_t<T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value)
      {
         static constexpr auto N = reflect<T>::size;

         size_t result = 1; // generic_array tag
         result += compressed_int_size<N>(); // element count

         for_each<N>([&]<size_t I>() {
            result += calculate_size<BEVE, void>::template op<Opts>(get_member(value, get<I>(reflect<T>::values)));
         });

         return result;
      }
   };

   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value)
      {
         static constexpr auto N = glz::tuple_size_v<T>;

         size_t result = 1; // generic_array tag
         result += compressed_int_size<N>(); // element count

         if constexpr (is_std_tuple<T>) {
            [&]<size_t... I>(std::index_sequence<I...>) {
               ((result += calculate_size<BEVE, void>::template op<Opts>(std::get<I>(value))), ...);
            }(std::make_index_sequence<N>{});
         }
         else {
            [&]<size_t... I>(std::index_sequence<I...>) {
               ((result += calculate_size<BEVE, void>::template op<Opts>(glz::get<I>(value))), ...);
            }(std::make_index_sequence<N>{});
         }

         return result;
      }
   };

   // Filesystem path support
   template <filesystem_path T>
   struct calculate_size<BEVE, T>
   {
      template <auto Opts>
      [[nodiscard]] static size_t op(auto&& value)
      {
         return calculate_size<BEVE, decltype(value.string())>::template op<Opts>(value.string());
      }
   };

   // ============ Public API ============

   // Calculate the size in bytes needed to serialize a value to BEVE format
   template <auto Opts = opts{}, class T>
   [[nodiscard]] size_t beve_size(T&& value)
   {
      return calculate_size<BEVE, std::remove_cvref_t<T>>::template op<set_beve<Opts>()>(std::forward<T>(value));
   }

   // Calculate size for untagged BEVE (structs_as_arrays = true)
   template <auto Opts = opts{}, class T>
   [[nodiscard]] size_t beve_size_untagged(T&& value)
   {
      return calculate_size<BEVE, std::remove_cvref_t<T>>::template op<
         opt_true<set_beve<Opts>(), structs_as_arrays_opt_tag{}>>(std::forward<T>(value));
   }
}
