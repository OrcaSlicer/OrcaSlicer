// Glaze Library
// For the license information refer to glaze.hpp

// original source (significantly refactored): https://github.com/codeinred/tuplet

#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "glaze/util/inline.hpp"

#if (__has_cpp_attribute(no_unique_address))
#define GLZ_NO_UNIQUE_ADDRESS [[no_unique_address]]
#elif (__has_cpp_attribute(msvc::no_unique_address)) || ((defined _MSC_VER) && (!defined __clang__))
// Note __has_cpp_attribute(msvc::no_unique_address) itself doesn't work as
// of 19.30.30709, even though the attribute itself is supported. See
// https://github.com/llvm/llvm-project/issues/49358#issuecomment-981041089
#define GLZ_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
// no_unique_address is not available.
#define GLZ_NO_UNIQUE_ADDRESS
#endif

namespace glz
{
   // tuplet concepts and traits
   namespace tuplet
   {
      template <class T>
      using identity_t = T;

      template <class T>
      using type_t = typename T::type;

      template <size_t I>
      using tag = std::integral_constant<size_t, I>;

      template <size_t N>
      using tag_range = std::make_index_sequence<N>;

      template <class T, class U>
      concept other_than = !std::is_same_v<std::decay_t<T>, U>;

      template <class Tup>
      using base_list_t = typename std::decay_t<Tup>::base_list;

      template <class Tuple>
      concept base_list_tuple = requires() { typename std::decay_t<Tuple>::base_list; };

      template <class T>
      concept stateless = std::is_empty_v<std::decay_t<T>>;

      template <class T>
      concept indexable = stateless<T> || requires(T t) { t[tag<0>()]; };

      template <class U, class T>
      concept assignable_to = requires(U u, T t) { t = u; };

      template <class T>
      concept ordered = requires(const T& t) {
         { t <=> t };
      };
      template <class T>
      concept equality_comparable = requires(const T& t) {
         { t == t } -> std::same_as<bool>;
      };
   } // namespace tuplet

   template <class... T>
   struct tuple;

   // tuplet::type_list implementation
   // tuplet::type_map implementation
   // tuplet::tuple_elem implementation
   // tuplet::deduce_elems
   namespace tuplet
   {
      template <class... T>
      struct type_list
      {};

      template <class... Ls, class... Rs>
      constexpr auto operator+(type_list<Ls...>, type_list<Rs...>)
      {
         return type_list<Ls..., Rs...>{};
      }

      template <class... Bases>
      struct type_map : Bases...
      {
         using base_list = type_list<Bases...>;
         using Bases::operator[]...;
         using Bases::decl_elem...;
         auto operator<=>(const type_map&) const = default;
         bool operator==(const type_map&) const = default;
      };

      template <size_t I, class T>
      struct tuple_elem
      {
         // Like declval, but with the element
         static T decl_elem(tag<I>);
         using type = T;

         GLZ_NO_UNIQUE_ADDRESS T value;

         constexpr decltype(auto) operator[](tag<I>) & { return (value); }
         constexpr decltype(auto) operator[](tag<I>) const& { return (value); }
         constexpr decltype(auto) operator[](tag<I>) && { return (std::move(*this).value); }
         auto operator<=>(const tuple_elem&) const = default;
         bool operator==(const tuple_elem&) const = default;
         // Implements comparison for tuples containing reference types
         constexpr auto operator<=>(const tuple_elem& other) const noexcept(noexcept(value <=> other.value))
            requires(std::is_reference_v<T> && ordered<T>)
         {
            return value <=> other.value;
         }
         constexpr bool operator==(const tuple_elem& other) const noexcept(noexcept(value == other.value))
            requires(std::is_reference_v<T> && equality_comparable<T>)
         {
            return value == other.value;
         }
      };
   } // namespace tuplet

   template <class T>
   using unwrap_ref_decay_t = typename std::unwrap_ref_decay<T>::type;

   // tuplet::get_tuple_base implementation
   // tuplet::apply_impl
   namespace tuplet
   {
      template <class A, class... T>
      struct get_tuple_base;

      template <size_t... I, class... T>
      struct get_tuple_base<std::index_sequence<I...>, T...>
      {
         using type = type_map<tuple_elem<I, T>...>;
      };

      template <class F, class T, class... Bases>
      constexpr decltype(auto) apply_impl(F&& f, T&& t, type_list<Bases...>)
      {
         return static_cast<F&&>(f)(static_cast<T&&>(t).identity_t<Bases>::value...);
      }

      template <class First, class>
      using first_t = First;

      template <class T, class... Q>
      constexpr auto repeat_type(type_list<Q...>)
      {
         return type_list<first_t<T, Q>...>{};
      }
      template <class... Outer>
      constexpr auto get_outer_bases(type_list<Outer...>)
      {
         return (repeat_type<Outer>(base_list_t<type_t<Outer>>{}) + ...);
      }
      template <class... Outer>
      constexpr auto get_inner_bases(type_list<Outer...>)
      {
         return (base_list_t<type_t<Outer>>{} + ...);
      }

      // This takes a forwarding tuple as a parameter. The forwarding tuple only
      // contains references, so it should just be taken by value.
      template <class T, class... Outer, class... Inner>
      constexpr auto cat_impl([[maybe_unused]] T tup, type_list<Outer...>, type_list<Inner...>)
         -> tuple<type_t<Inner>...>
      {
         return {{{static_cast<type_t<Outer>&&>(tup.identity_t<Outer>::value).identity_t<Inner>::value}...}};
      }

      template <class... T>
      using tuple_base_t = typename get_tuple_base<tag_range<sizeof...(T)>, T...>::type;
   }

   template <class... T>
   struct tuple : tuplet::tuple_base_t<T...>
   {
      static constexpr auto glaze_reflect = false;
      static constexpr size_t N = sizeof...(T);
      using super = tuplet::tuple_base_t<T...>;
      using super::operator[];
      using base_list = typename super::base_list;
      using super::decl_elem;

      template <tuplet::other_than<tuple> U> // Preserves default assignments
      constexpr auto& operator=(U&& tup)
      {
         using tuple2 = std::decay_t<U>;
         if constexpr (tuplet::base_list_tuple<tuple2>) {
            eq_impl(static_cast<U&&>(tup), base_list(), typename tuple2::base_list());
         }
         else {
            eq_impl(static_cast<U&&>(tup), tuplet::tag_range<N>());
         }
         return *this;
      }

      // TODO: currently segfaults clang
      /*template <assignable_to<T>... U>
      constexpr auto& assign(U&&... values) {
         assign_impl(base_list(), static_cast<U&&>(values)...);
         return *this;
      }*/

      auto operator<=>(const tuple&) const = default;
      bool operator==(const tuple&) const = default;

      // Applies a function to every element of the tuple. The order is the
      // declaration order, so first the function will be applied to element 0,
      // then element 1, then element 2, and so on, where element N is identified
      // by get<N>
      template <class F>
      constexpr void for_each(F&& func) &
      {
         for_each_impl(base_list(), static_cast<F&&>(func));
      }
      template <class F>
      constexpr void for_each(F&& func) const&
      {
         for_each_impl(base_list(), static_cast<F&&>(func));
      }
      template <class F>
      constexpr void for_each(F&& func) &&
      {
         static_cast<tuple&&>(*this).for_each_impl(base_list(), static_cast<F&&>(func));
      }

      // Applies a function to each element successively, until one returns a
      // truthy value. Returns true if any application returned a truthy value,
      // and false otherwise
      template <class F>
      constexpr bool any(F&& func) &
      {
         return any_impl(base_list(), static_cast<F&&>(func));
      }
      template <class F>
      constexpr bool any(F&& func) const&
      {
         return any_impl(base_list(), static_cast<F&&>(func));
      }
      template <class F>
      constexpr bool any(F&& func) &&
      {
         return static_cast<tuple&&>(*this).any_impl(base_list(), static_cast<F&&>(func));
      }

      // Applies a function to each element successively, until one returns a
      // falsy value. Returns true if every application returned a truthy value,
      // and false otherwise
      template <class F>
      constexpr bool all(F&& func) &
      {
         return all_impl(base_list(), static_cast<F&&>(func));
      }
      template <class F>
      constexpr bool all(F&& func) const&
      {
         return all_impl(base_list(), static_cast<F&&>(func));
      }
      template <class F>
      constexpr bool all(F&& func) &&
      {
         return static_cast<tuple&&>(*this).all_impl(base_list(), static_cast<F&&>(func));
      }

     private:
      template <class U, class... B1, class... B2>
      constexpr void eq_impl(U&& u, tuplet::type_list<B1...>, tuplet::type_list<B2...>)
      {
         // See:
         // https://developercommunity.visualstudio.com/t/fold-expressions-unreliable-in-171-with-c20/1676476
         (void(B1::value = static_cast<U&&>(u).B2::value), ...);
      }
      template <class U, size_t... I>
      constexpr void eq_impl(U&& u, std::index_sequence<I...>)
      {
         (void(tuplet::tuple_elem<I, T>::value = get<I>(static_cast<U&&>(u))), ...);
      }
      template <class... U, class... B>
      constexpr void assign_impl(tuplet::type_list<B...>, U&&... u)
      {
         (void(B::value = static_cast<U&&>(u)), ...);
      }

      template <class F, class... B>
      constexpr void for_each_impl(tuplet::type_list<B...>, F&& func) &
      {
         (void(func(B::value)), ...);
      }
      template <class F, class... B>
      constexpr void for_each_impl(tuplet::type_list<B...>, F&& func) const&
      {
         (void(func(B::value)), ...);
      }
      template <class F, class... B>
      constexpr void for_each_impl(tuplet::type_list<B...>, F&& func) &&
      {
         (void(func(static_cast<T&&>(B::value))), ...);
      }

      template <class F, class... B>
      constexpr bool any_impl(tuplet::type_list<B...>, F&& func) &
      {
         return (bool(func(B::value)) || ...);
      }
      template <class F, class... B>
      constexpr bool any_impl(tuplet::type_list<B...>, F&& func) const&
      {
         return (bool(func(B::value)) || ...);
      }
      template <class F, class... B>
      constexpr bool any_impl(tuplet::type_list<B...>, F&& func) &&
      {
         return (bool(func(static_cast<T&&>(B::value))) || ...);
      }

      template <class F, class... B>
      constexpr bool all_impl(tuplet::type_list<B...>, F&& func) &
      {
         return (bool(func(B::value)) && ...);
      }
      template <class F, class... B>
      constexpr bool all_impl(tuplet::type_list<B...>, F&& func) const&
      {
         return (bool(func(B::value)) && ...);
      }
      template <class F, class... B>
      constexpr bool all_impl(tuplet::type_list<B...>, F&& func) &&
      {
         return (bool(func(static_cast<T&&>(B::value))) && ...);
      }
   };
   template <>
   struct tuple<> : tuplet::tuple_base_t<>
   {
      constexpr static size_t N = 0;
      using super = tuplet::tuple_base_t<>;
      using base_list = tuplet::type_list<>;

      template <tuplet::other_than<tuple> U> // Preserves default assignments
         requires tuplet::stateless<U> // Check that U is similarly stateless
      constexpr auto& operator=(U&&) noexcept
      {
         return *this;
      }

      constexpr auto& assign() noexcept { return *this; }
      auto operator<=>(const tuple&) const = default;
      bool operator==(const tuple&) const = default;

      // Applies a function to every element of the tuple. The order is the
      // declaration order, so first the function will be applied to element 0,
      // then element 1, then element 2, and so on, where element N is identified
      // by get<N>
      //
      // (Does nothing when invoked on empty tuple)
      template <class F>
      constexpr void for_each(F&&) const noexcept
      {}

      // Applies a function to each element successively, until one returns a
      // truthy value. Returns true if any application returned a truthy value,
      // and false otherwise
      //
      // (Returns false for empty tuple)
      template <class F>
      constexpr bool any(F&&) const noexcept
      {
         return false;
      }

      // Applies a function to each element successively, until one returns a
      // falsy value. Returns true if every application returned a truthy value,
      // and false otherwise
      //
      // (Returns true for empty tuple)
      template <class F>
      constexpr bool all(F&&) const noexcept
      {
         return true;
      }
   };
   template <class... Ts>
   tuple(Ts...) -> tuple<unwrap_ref_decay_t<Ts>...>;

   // tuplet::convert implementation
   namespace tuplet
   {
      // Converts from one tuple type to any other tuple or U
      template <class Tuple>
      struct convert final
      {
         using base_list = typename std::decay_t<Tuple>::base_list;
         Tuple tuple;
         template <class U>
         constexpr operator U() &&
         {
            return convert_impl<U>(base_list{});
         }

        private:
         template <class U, class... Bases>
         constexpr U convert_impl(type_list<Bases...>)
         {
            return U{static_cast<Tuple&&>(tuple).identity_t<Bases>::value...};
         }
      };
      template <class Tuple>
      convert(Tuple&) -> convert<Tuple&>;
      template <class Tuple>
      convert(const Tuple&) -> convert<const Tuple&>;
      template <class Tuple>
      convert(Tuple&&) -> convert<Tuple>;
   } // namespace tuplet

   // glz::get implementation
   // glz::tie implementation
   // glz::apply implementation
   template <size_t I, tuplet::indexable Tup>
   GLZ_ALWAYS_INLINE constexpr decltype(auto) get(Tup&& tup) noexcept
   {
      return static_cast<Tup&&>(tup)[tuplet::tag<I>()];
   }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif
   template <class... T>
   constexpr tuple<T&...> tie(T&... t)
   {
      return {t...};
   }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

   template <class F, tuplet::base_list_tuple Tup>
   constexpr decltype(auto) apply(F&& func, Tup&& tup)
   {
      return tuplet::apply_impl(static_cast<F&&>(func), static_cast<Tup&&>(tup),
                                typename std::decay_t<Tup>::base_list());
   }

   // tuplet::tuple_cat implementation
   // tuplet::make_tuple implementation
   // tuplet::forward_as_tuple implementation
   namespace tuplet
   {
      template <base_list_tuple... T>
      constexpr auto tuple_cat(T&&... ts)
      {
         if constexpr (sizeof...(T) == 0) {
            return tuple<>();
         }
         else {
            /**
             * It appears that Clang produces better assembly when
             * TUPLET_CAT_BY_FORWARDING_TUPLE == 0, while GCC produces better assembly when
             * TUPLET_CAT_BY_FORWARDING_TUPLE == 1. MSVC always produces terrible assembly
             * in either case. This will set TUPLET_CAT_BY_FORWARDING_TUPLE to the correct
             * value (0 for clang, 1 for everyone else)
             *
             * See: https://github.com/codeinred/tuplet/discussions/14
             */
#if !defined(TUPLET_CAT_BY_FORWARDING_TUPLE)
#if defined(__clang__)
#define TUPLET_CAT_BY_FORWARDING_TUPLE 0
#else
#define TUPLET_CAT_BY_FORWARDING_TUPLE 1
#endif
#endif
#if TUPLET_CAT_BY_FORWARDING_TUPLE
            using big_tuple = tuple<T&&...>;
#else
            using big_tuple = tuple<std::decay_t<T>...>;
#endif
            using outer_bases = base_list_t<big_tuple>;
            constexpr auto outer = get_outer_bases(outer_bases{});
            constexpr auto inner = get_inner_bases(outer_bases{});
            return cat_impl(big_tuple{{{std::forward<T>(ts)}...}}, outer, inner);
         }
      }

      template <typename... Ts>
      constexpr auto make_tuple(Ts&&... args)
      {
         return tuple<unwrap_ref_decay_t<Ts>...>{{{std::forward<Ts>(args)}...}};
      }

      template <typename... T>
      constexpr auto forward_as_tuple(T&&... a) noexcept
      {
         return tuple<T&&...>{static_cast<T&&>(a)...};
      }
   } // namespace tuplet
} // namespace glz

#include <array>
#include <tuple>
#include <variant>

namespace glz
{
   template <class... T>
   struct tuple_size;

   template <class T>
   constexpr size_t tuple_size_v = tuple_size<std::remove_const_t<T>>::value;

   template <class T, size_t N>
   struct tuple_size<std::array<T, N>>
   {
      static constexpr size_t value = N;
   };

   template <class... Types>
   struct tuple_size<std::tuple<Types...>>
   {
      static constexpr size_t value = sizeof...(Types);
   };

   template <size_t I, class... T>
   struct tuple_element;

   template <size_t I, class Tuple>
   using tuple_element_t = typename tuple_element<I, Tuple>::type;

   template <size_t I, class... T>
   struct tuple_element<I, std::tuple<T...>>
   {
      using type = typename std::tuple_element<I, std::tuple<T...>>::type;
   };

   template <std::size_t I, typename T1, typename T2>
   struct tuple_element<I, std::pair<T1, T2>>
   {
      using type = typename std::conditional<I == 0, T1, T2>::type;
   };

   template <class... T>
   struct tuple_size<glz::tuple<T...>> : std::integral_constant<size_t, sizeof...(T)>
   {};

   template <size_t I, class... T>
   struct tuple_element<I, glz::tuple<T...>>
   {
      using type = decltype(glz::tuple<T...>::decl_elem(glz::tuplet::tag<I>()));
   };
}
