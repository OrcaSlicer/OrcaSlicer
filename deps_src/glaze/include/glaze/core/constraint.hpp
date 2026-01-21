// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <utility>

#include "glaze/core/common.hpp"
#include "glaze/core/context.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/tuplet/tuple.hpp"
#include "glaze/util/string_literal.hpp"

namespace glz
{
   // read_constraint allows a user to register a contraint lambda or member function
   // that returns a boolean, which indicates true for success and false for failure
   // this allows arguments to be validated
   template <class T, auto Target, auto Constraint, string_literal Message>
   struct read_constraint_t
   {
      static constexpr bool glaze_wrapper = true;
      static constexpr auto glaze_reflect = false;
      static constexpr std::string_view message = Message;
      using target_t = decltype(Target);
      using constraint_t = decltype(Constraint);
      using value_type = std::remove_cvref_t<member_t<T, target_t>>;

      T& val;
      static constexpr auto target = Target;
      static constexpr auto constraint = Constraint;
   };

   template <class T>
   concept is_read_constraint = requires {
      requires !T::glaze_reflect;
      typename T::target_t;
      typename T::constraint_t;
   };

   template <uint32_t Format, is_read_constraint T>
   struct from<Format, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using V = std::decay_t<decltype(value)>;
         using Constraint = typename V::constraint_t;

         auto assign_to_target = [&]<class Input>(Input&& input) {
            using Target = typename V::target_t;
            if constexpr (std::is_member_object_pointer_v<Target>) {
               (value.val).*(value.target) = std::forward<Input>(input);
            }
            else if constexpr (std::invocable<Target, decltype(value.val)>) {
               std::invoke(value.target, value.val) = std::forward<Input>(input);
            }
            else {
               static_assert(false_v<Target>,
                             "expected invocable function or member object pointer, perhaps you need const qualified "
                             "input on your lambda");
            }
         };

         if constexpr (std::is_member_pointer_v<Constraint>) {
            if constexpr (std::is_member_function_pointer_v<Constraint>) {
               using Ret = typename return_type<Constraint>::type;
               if constexpr (std::same_as<Ret, bool>) {
                  using Tuple = typename inputs_as_tuple<Constraint>::type;
                  if constexpr (glz::tuple_size_v<Tuple> == 1) {
                     std::decay_t<glz::tuple_element_t<0, Tuple>> input{};
                     parse<Format>::template op<Opts>(input, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     auto success = (value.val.*(value.constraint))(input);
                     if (not success) {
                        ctx.error = error_code::constraint_violated;
                        ctx.custom_error_message = V::message;
                        return;
                     }
                     assign_to_target(std::move(input));
                  }
                  else {
                     static_assert(false_v<T>, "function must have a single input");
                  }
               }
               else {
                  static_assert(false_v<T>, "function must return a boolean (true for success)");
               }
            }
            else if constexpr (std::is_member_object_pointer_v<Constraint>) {
               auto& constraint = value.val.*(value.constraint);
               using Func = std::decay_t<decltype(constraint)>;
               if constexpr (is_specialization_v<Func, std::function>) {
                  using Ret = typename function_traits<Func>::result_type;

                  if constexpr (std::same_as<Ret, bool>) {
                     using Tuple = typename function_traits<Func>::arguments;
                     if constexpr (glz::tuple_size_v<Tuple> == 1) {
                        std::decay_t<glz::tuple_element_t<0, Tuple>> input{};
                        parse<Format>::template op<Opts>(input, ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]]
                           return;
                        auto success = constraint(input);
                        if (not success) {
                           ctx.error = error_code::constraint_violated;
                           ctx.custom_error_message = V::message;
                           return;
                        }
                        assign_to_target(std::move(input));
                     }
                     else {
                        static_assert(false_v<T>, "function must have a single input");
                     }
                  }
                  else {
                     static_assert(false_v<T>, "std::function must return a boolean (true for success)");
                  }
               }
               else {
                  static_assert(false_v<T>, "invalid type for read_constraint_t");
               }
            }
            else {
               static_assert(false_v<T>, "invalid type for read_constraint_t");
            }
         }
         else {
            if constexpr (is_invocable_concrete<Constraint>) {
               using Ret = invocable_result_t<Constraint>;
               if constexpr (std::same_as<Ret, bool>) {
                  using Tuple = invocable_args_t<Constraint>;
                  constexpr auto N = glz::tuple_size_v<Tuple>;
                  if constexpr (N == 0) {
                     static_assert(false_v<T>,
                                   "lambda must take in the class as the first argument and the type to deserialize as "
                                   "the second");
                  }
                  else if constexpr (N == 2) {
                     std::decay_t<glz::tuple_element_t<1, Tuple>> input{};
                     parse<Format>::template op<Opts>(input, ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     auto success = value.constraint(value.val, input);
                     if (not success) {
                        ctx.error = error_code::constraint_violated;
                        ctx.custom_error_message = V::message;
                        return;
                     }
                     assign_to_target(std::move(input));
                  }
                  else {
                     static_assert(false_v<T>, "lambda must have two inputs, the class and the type to deserialize");
                  }
               }
               else {
                  static_assert(false_v<T>, "lambda must return a boolean (true for success)");
               }
            }
            else {
               static_assert(
                  false_v<T>,
                  "IMPORTANT: Two arguments are required in your lambda (e.g. [](my_struct&, const std::string& "
                  "input)) you must make all the arguments concrete types. None of the inputs can be `auto`. Also, "
                  "you probably cannot define these lambdas within a local `struct glaze`, but instead need to use "
                  "`glz::meta` outside your class so that your lambda can operate on a defined class.");
            }
         }
      }
   };

   template <uint32_t Format, is_read_constraint T>
   struct to<Format, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         using V = std::decay_t<decltype(value)>;
         using Target = typename V::target_t;

         if constexpr (std::is_member_pointer_v<Target>) {
            if constexpr (std::is_member_object_pointer_v<Target>) {
               auto& target = value.val.*(value.target);
               serialize<Format>::template op<Opts>(target, ctx, args...);
            }
            else {
               static_assert(false_v<T>, "invalid type for read_constraint_t");
            }
         }
         else {
            if constexpr (std::invocable<Target, decltype(value.val)>) {
               serialize<Format>::template op<Opts>(std::invoke(value.target, value.val), ctx, args...);
            }
            else {
               static_assert(false_v<Target>,
                             "expected invocable function, perhaps you need const qualified input on your lambda");
            }
         }
      }
   };

   template <auto Target, auto Constraint, string_literal Message>
   constexpr auto read_constraint_impl() noexcept
   {
      return
         [](auto&& v) { return read_constraint_t<std::remove_cvref_t<decltype(v)>, Target, Constraint, Message>{v}; };
   }

   template <auto Target, auto Constraint, string_literal Message>
   constexpr auto read_constraint = read_constraint_impl<Target, Constraint, Message>();

   template <class T, auto Constraint, string_literal Message>
   struct self_constraint_t
   {
      static constexpr auto glaze_reflect = false;
      static constexpr std::string_view message = Message;
      using value_type = T;
      using constraint_t = decltype(Constraint);
      static constexpr auto constraint = Constraint;
      T& val;
   };

   template <class T>
   concept is_self_constraint = requires(T t) {
      requires !T::glaze_reflect;
      requires(!is_read_constraint<T>);
      typename T::constraint_t;
      typename T::value_type;
      t.val;
   };

   template <uint32_t Format, is_self_constraint T>
   struct from<Format, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&, auto&&)
      {
         using V = std::decay_t<decltype(value)>;
         using Value = typename V::value_type;
         using Constraint = typename V::constraint_t;

         bool success{};
         if constexpr (std::is_invocable_r_v<bool, Constraint, Value&>) {
            success = std::invoke(V::constraint, value.val);
         }
         else if constexpr (std::is_invocable_r_v<bool, Constraint, const Value&>) {
            const Value& const_val = value.val;
            success = std::invoke(V::constraint, const_val);
         }
         else {
            static_assert(false_v<Constraint>,
                          "self constraint must accept the enclosing type by reference or const reference and return a "
                          "boolean");
         }

         if (!success) {
            ctx.error = error_code::constraint_violated;
            ctx.custom_error_message = V::message;
         }
      }
   };

   template <auto Constraint, string_literal Message>
   constexpr auto self_constraint_impl() noexcept
   {
      return [](auto&& v) {
         using Value = std::remove_reference_t<decltype(v)>;
         return self_constraint_t<Value, Constraint, Message>{v};
      };
   }

   template <auto Constraint, string_literal Message>
   constexpr auto self_constraint = self_constraint_impl<Constraint, Message>();
}
