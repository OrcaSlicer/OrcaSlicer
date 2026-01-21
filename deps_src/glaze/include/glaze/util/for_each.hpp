// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <tuple>
#include <utility>

#include "glaze/util/inline.hpp"
#include "glaze/util/utility.hpp"

// We do not mark these functions noexcept so that it can be used in exception contexts
// Furthermore, adding noexcept can increase assembly size because exceptions need to cause termination

namespace glz
{
   // There is no benefit to perfectly forward the lambda (in the internal lambda) because it is immediately invoked
   // It actually removes an assembly instruction on GCC and Clang to pass by reference with O0
   // With O0 GCC and Clang produce better assembly using the templated I approach rather than
   // the approach that passes std::integral_constant

   // Compile time iterate over I indices
   template <size_t N>
   inline constexpr void for_each(auto&& lambda)
   {
      if constexpr (N > 0) {
         // Explicit sizes for small N to help the compiler and make debugging easier
         // These generate less assembly with O0, but make no difference for higher optimization
         if constexpr (N == 1) {
            lambda.template operator()<0>();
         }
         else if constexpr (N == 2) {
            lambda.template operator()<0>();
            lambda.template operator()<1>();
         }
         else if constexpr (N == 3) {
            lambda.template operator()<0>();
            lambda.template operator()<1>();
            lambda.template operator()<2>();
         }
         else if constexpr (N == 4) {
            lambda.template operator()<0>();
            lambda.template operator()<1>();
            lambda.template operator()<2>();
            lambda.template operator()<3>();
         }
         else {
            [&]<size_t... I>(std::index_sequence<I...>) {
               (void)(lambda.template operator()<I>(), ...);
            }(std::make_index_sequence<N>{});
         }
      }
   }

   // Runtime short circuiting if function returns true, return false to continue evaluation
   template <size_t N>
   constexpr void for_each_short_circuit(auto&& lambda)
   {
      if constexpr (N > 0) {
         [&]<size_t... I>(std::index_sequence<I...>) {
            (lambda.template operator()<I>() || ...);
         }(std::make_index_sequence<N>{});
      }
   }

   template <class Func, class Tuple>
   constexpr void for_each_apply(Func&& f, Tuple&& t)
   {
      constexpr size_t N = std::tuple_size_v<std::decay_t<Tuple>>;
      [&]<size_t... I>(std::index_sequence<I...>) { (f(std::get<I>(t)), ...); }(std::make_index_sequence<N>{});
   }

   template <size_t I, class Lambda>
   constexpr auto make_jump_function()
   {
      return +[](Lambda& l) { l.template operator()<I>(); };
   }

   // Important: index must be less than N
   template <size_t N>
   inline constexpr void visit(auto&& lambda, const size_t index)
   {
      if constexpr (N > 0) {
         // Explicit sizes for small N to help the compiler and make debugging easier
         if constexpr (N == 1) {
            (void)index;
            (void)(lambda.template operator()<0>());
         }
         else if constexpr (N == 2) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 3) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 4) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 5) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            case 4:
               lambda.template operator()<4>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 6) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            case 4:
               lambda.template operator()<4>();
               break;
            case 5:
               lambda.template operator()<5>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 7) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            case 4:
               lambda.template operator()<4>();
               break;
            case 5:
               lambda.template operator()<5>();
               break;
            case 6:
               lambda.template operator()<6>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 8) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            case 4:
               lambda.template operator()<4>();
               break;
            case 5:
               lambda.template operator()<5>();
               break;
            case 6:
               lambda.template operator()<6>();
               break;
            case 7:
               lambda.template operator()<7>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 9) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            case 4:
               lambda.template operator()<4>();
               break;
            case 5:
               lambda.template operator()<5>();
               break;
            case 6:
               lambda.template operator()<6>();
               break;
            case 7:
               lambda.template operator()<7>();
               break;
            case 8:
               lambda.template operator()<8>();
               break;
            default:
               std::unreachable();
            }
         }
         else if constexpr (N == 10) {
            switch (index) {
            case 0:
               lambda.template operator()<0>();
               break;
            case 1:
               lambda.template operator()<1>();
               break;
            case 2:
               lambda.template operator()<2>();
               break;
            case 3:
               lambda.template operator()<3>();
               break;
            case 4:
               lambda.template operator()<4>();
               break;
            case 5:
               lambda.template operator()<5>();
               break;
            case 6:
               lambda.template operator()<6>();
               break;
            case 7:
               lambda.template operator()<7>();
               break;
            case 8:
               lambda.template operator()<8>();
               break;
            case 9:
               lambda.template operator()<9>();
               break;
            default:
               std::unreachable();
            }
         }
         else {
#ifdef _MSC_VER
            using Lambda = std::decay_t<decltype(lambda)>;
            static const auto jump_table = []<size_t... I>(std::index_sequence<I...>) {
               return std::array{make_jump_function<I, Lambda>()...};
            }(std::make_index_sequence<N>{});
#else
            static constexpr auto jump_table = []<size_t... I>(std::index_sequence<I...>) {
               return std::array{+[](std::decay_t<decltype(lambda)>& l) { l.template operator()<I>(); }...};
            }(std::make_index_sequence<N>{});
#endif

#if defined(__clang_major__) && (__clang_major__ >= 19)
            [[assume(index < N)]];
#endif
            jump_table[index](lambda);

            // This code runs significantly slower on Clang
            // Simple tests with assembly show that this should be faster,
            // but full Glaze assembly need to be looked into
            /*[&, index]<size_t... I>(std::index_sequence<I...>) {
               (void)((index == I ? lambda.template operator()<I>() : void()), ...);
            }(std::make_index_sequence<N>{});*/
         }
      }
   }
}
