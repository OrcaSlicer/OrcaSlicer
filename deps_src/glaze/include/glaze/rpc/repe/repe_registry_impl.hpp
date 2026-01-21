// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/glaze.hpp"
#include "glaze/rpc/repe/repe.hpp"

namespace glz
{
   // Forward declaration of the registry template
   template <auto Opts, uint32_t Proto>
   struct registry;

   template <>
   struct protocol_storage<REPE>
   {
      using type = std::unordered_map<sv, std::function<void(repe::state_view&)>, detail::string_hash, std::equal_to<>>;
   };

   // Implementation for REPE protocol
   template <auto Opts>
   struct registry_impl<Opts, REPE>
   {
      template <class T, class RegistryType>
      static void register_endpoint(const sv path, T& value, RegistryType& reg)
      {
         reg.endpoints[path] = [&value](repe::state_view& state) mutable {
            if (state.has_body()) {
               if (read_params<Opts>(value, state) == 0) {
                  return;
               }
            }

            if (state.notify()) {
               return;
            }

            if (not state.has_body()) {
               write_response<Opts>(value, state);
            }
            else {
               write_response<Opts>(state);
            }
         };
      }

      template <class Func, class Result, class RegistryType>
      static void register_function_endpoint(const sv path, Func& func, RegistryType& reg)
      {
         if constexpr (std::same_as<Result, void>) {
            reg.endpoints[path] = [&func](repe::state_view& state) mutable {
               func();
               if (state.notify()) {
                  return;
               }
               write_response<Opts>(state);
            };
         }
         else {
            reg.endpoints[path] = [&func](repe::state_view& state) mutable {
               if (state.notify()) {
                  std::ignore = func();
                  return;
               }
               write_response<Opts>(func(), state);
            };
         }
      }

      template <class Func, class Params, class RegistryType>
      static void register_param_function_endpoint(const sv path, Func& func, RegistryType& reg)
      {
         reg.endpoints[path] = [&func](repe::state_view& state) mutable {
            static thread_local std::decay_t<Params> params{};
            if (read_params<Opts>(params, state) == 0) {
               return;
            }

            using Result = std::invoke_result_t<decltype(func), Params>;

            if (state.notify()) {
               if constexpr (std::same_as<Result, void>) {
                  func(params);
               }
               else {
                  std::ignore = func(params);
               }
               return;
            }
            if constexpr (std::same_as<Result, void>) {
               func(params);
               write_response<Opts>(state);
            }
            else {
               auto ret = func(params);
               write_response<Opts>(ret, state);
            }
         };
      }

      template <class Obj, class RegistryType>
      static void register_object_endpoint(const sv path, Obj& obj, RegistryType& reg)
      {
         reg.endpoints[path] = [&obj](repe::state_view& state) mutable {
            if (state.has_body()) {
               if (read_params<Opts>(obj, state) == 0) {
                  return;
               }
            }

            if (state.notify()) {
               return;
            }

            if (not state.has_body()) {
               write_response<Opts>(obj, state);
            }
            else {
               write_response<Opts>(state);
            }
         };
      }

      template <class Value, class RegistryType>
      static void register_value_endpoint(const sv path, Value& value, RegistryType& reg)
      {
         reg.endpoints[path] = [value](repe::state_view& state) mutable {
            if (state.has_body()) {
               if (read_params<Opts>(value, state) == 0) {
                  return;
               }
            }

            if (state.notify()) {
               return;
            }

            if (not state.has_body()) {
               write_response<Opts>(value, state);
            }
            else {
               write_response<Opts>(state);
            }
         };
      }

      template <class Var, class RegistryType>
      static void register_variable_endpoint(const sv path, Var& var, RegistryType& reg)
      {
         reg.endpoints[path] = [&var](repe::state_view& state) mutable {
            if (state.has_body()) {
               if (read_params<Opts>(var, state) == 0) {
                  return;
               }
            }

            if (state.notify()) {
               return;
            }

            if (not state.has_body()) {
               write_response<Opts>(var, state);
            }
            else {
               write_response<Opts>(state);
            }
         };
      }

      template <class T, class F, class Ret, class RegistryType>
      static void register_member_function_endpoint(const sv path, T& value, F func, RegistryType& reg)
      {
         reg.endpoints[path] = [&value, func](repe::state_view& state) mutable {
            if constexpr (std::same_as<Ret, void>) {
               (value.*func)();

               if (state.notify()) {
                  return;
               }

               write_response<Opts>(state);
            }
            else {
               if (state.notify()) {
                  std::ignore = (value.*func)();
                  return;
               }

               write_response<Opts>((value.*func)(), state);
            }
         };
      }

      template <class T, class F, class Input, class Ret, class RegistryType>
      static void register_member_function_with_params_endpoint(const sv path, T& value, F func, RegistryType& reg)
      {
         reg.endpoints[path] = [&value, func](repe::state_view& state) mutable {
            static thread_local Input input{};
            if (state.has_body()) {
               if (read_params<Opts>(input, state) == 0) {
                  return;
               }
            }

            if constexpr (std::same_as<Ret, void>) {
               (value.*func)(input);

               if (state.notify()) {
                  return;
               }

               write_response<Opts>(state);
            }
            else {
               if (state.notify()) {
                  std::ignore = (value.*func)(input);
                  return;
               }

               write_response<Opts>((value.*func)(input), state);
            }
         };
      }

      // Register a merged endpoint that combines multiple objects into a single response
      // Note: This endpoint is read-only; writing to it is not supported
      template <class... Ts, class RegistryType>
      static void register_merge_endpoint(const sv path, glz::merge<Ts...>& merged, RegistryType& reg)
      {
         reg.endpoints[path] = [&merged](repe::state_view& state) mutable {
            // Merged endpoints are read-only
            if (state.has_body()) {
               state.out.reset(state.in);
               state.out.set_error(error_code::invalid_body, "writing to merged endpoint is not supported");
               return;
            }

            if (state.notify()) {
               return;
            }

            write_response<Opts>(merged, state);
         };
      }
   };
}
