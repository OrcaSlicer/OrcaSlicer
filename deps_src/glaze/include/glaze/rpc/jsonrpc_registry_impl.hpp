// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/opts.hpp"
#include "glaze/ext/jsonrpc.hpp"
#include "glaze/glaze.hpp"

namespace glz
{
   // Forward declaration of the registry template
   template <auto Opts, uint32_t Proto>
   struct registry;

   namespace jsonrpc
   {
      // State for JSON RPC request handling
      struct state final
      {
         rpc::id_t id{};
         std::string& response;
         bool is_notification{false};
         bool has_params{false};
         std::string_view params_json{};

         bool notify() const { return is_notification; }
         bool has_body() const { return has_params; }
      };

      template <class T>
      concept is_state = std::same_as<std::decay_t<T>, state>;

      // Write a successful JSON RPC response
      template <auto Opts, class Value>
      void write_response(Value&& value, is_state auto&& state)
      {
         if (state.notify()) {
            return; // No response for notifications
         }

         std::string result_json;
         auto ec = write<Opts>(std::forward<Value>(value), result_json);
         if (ec) {
            // Write error response
            state.response =
               R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Internal error","data":"Failed to serialize result"},"id":)";
            state.response += write_json(state.id).value_or("null");
            state.response += "}";
            return;
         }

         state.response = R"({"jsonrpc":"2.0","result":)";
         state.response += result_json;
         state.response += R"(,"id":)";
         state.response += write_json(state.id).value_or("null");
         state.response += "}";
      }

      // Write a successful JSON RPC response with null result
      template <auto Opts>
      void write_response(is_state auto&& state)
      {
         if (state.notify()) {
            return; // No response for notifications
         }

         state.response = R"({"jsonrpc":"2.0","result":null,"id":)";
         state.response += write_json(state.id).value_or("null");
         state.response += "}";
      }

      // Write an error response
      inline void write_error(is_state auto&& state, rpc::error_e code, const std::string& message,
                              const std::optional<std::string>& data = std::nullopt)
      {
         if (state.notify()) {
            return; // No response for notifications
         }

         state.response = R"({"jsonrpc":"2.0","error":{"code":)";
         state.response += std::to_string(static_cast<int>(code));
         state.response += R"(,"message":)";
         state.response += write_json(message).value_or("\"\"");
         if (data) {
            state.response += R"(,"data":)";
            state.response += write_json(*data).value_or("null");
         }
         state.response += R"(},"id":)";
         state.response += write_json(state.id).value_or("null");
         state.response += "}";
      }

      // Read params from JSON RPC request
      template <auto Opts, class Value>
      bool read_params(Value&& value, is_state auto&& state)
      {
         if (!state.has_body()) {
            return true; // No params to read
         }

         auto ec = read<Opts>(std::forward<Value>(value), state.params_json);
         if (ec) {
            write_error(state, rpc::error_e::invalid_params, "Invalid params", format_error(ec, state.params_json));
            return false;
         }
         return true;
      }
   }

   // Storage for JSON RPC protocol - similar to REPE but with JSON RPC state
   template <>
   struct protocol_storage<JSONRPC>
   {
      using type = std::unordered_map<sv, std::function<void(jsonrpc::state&&)>, detail::string_hash, std::equal_to<>>;
   };

   // Implementation for JSON RPC protocol
   template <auto Opts>
   struct registry_impl<Opts, JSONRPC>
   {
     private:
      // Helper for registering read/write endpoints (used by register_endpoint, register_object_endpoint,
      // register_variable_endpoint)
      template <class T, class RegistryType>
      static void register_read_write_endpoint(const sv path, T& value, RegistryType& reg)
      {
         reg.endpoints[path] = [&value](jsonrpc::state&& state) mutable {
            if (state.has_body()) {
               if (!jsonrpc::read_params<Opts>(value, state)) {
                  return;
               }
            }

            if (state.notify()) {
               return;
            }

            if (!state.has_body()) {
               jsonrpc::write_response<Opts>(value, state);
            }
            else {
               jsonrpc::write_response<Opts>(state);
            }
         };
      }

     public:
      template <class T, class RegistryType>
      static void register_endpoint(const sv path, T& value, RegistryType& reg)
      {
         register_read_write_endpoint(path, value, reg);
      }

      template <class Func, class Result, class RegistryType>
      static void register_function_endpoint(const sv path, Func& func, RegistryType& reg)
      {
         if constexpr (std::same_as<Result, void>) {
            reg.endpoints[path] = [&func](jsonrpc::state&& state) mutable {
               func();
               if (state.notify()) {
                  return;
               }
               jsonrpc::write_response<Opts>(state);
            };
         }
         else {
            reg.endpoints[path] = [&func](jsonrpc::state&& state) mutable {
               if (state.notify()) {
                  std::ignore = func();
                  return;
               }
               jsonrpc::write_response<Opts>(func(), state);
            };
         }
      }

      template <class Func, class Params, class RegistryType>
      static void register_param_function_endpoint(const sv path, Func& func, RegistryType& reg)
      {
         reg.endpoints[path] = [&func](jsonrpc::state&& state) mutable {
            static thread_local std::decay_t<Params> params{};
            if (!jsonrpc::read_params<Opts>(params, state)) {
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
               jsonrpc::write_response<Opts>(state);
            }
            else {
               auto ret = func(params);
               jsonrpc::write_response<Opts>(ret, state);
            }
         };
      }

      template <class Obj, class RegistryType>
      static void register_object_endpoint(const sv path, Obj& obj, RegistryType& reg)
      {
         register_read_write_endpoint(path, obj, reg);
      }

      template <class Value, class RegistryType>
      static void register_value_endpoint(const sv path, Value& value, RegistryType& reg)
      {
         reg.endpoints[path] = [value](jsonrpc::state&& state) mutable {
            if (state.has_body()) {
               if (!jsonrpc::read_params<Opts>(value, state)) {
                  return;
               }
            }

            if (state.notify()) {
               return;
            }

            if (!state.has_body()) {
               jsonrpc::write_response<Opts>(value, state);
            }
            else {
               jsonrpc::write_response<Opts>(state);
            }
         };
      }

      template <class Var, class RegistryType>
      static void register_variable_endpoint(const sv path, Var& var, RegistryType& reg)
      {
         register_read_write_endpoint(path, var, reg);
      }

      template <class T, class F, class Ret, class RegistryType>
      static void register_member_function_endpoint(const sv path, T& value, F func, RegistryType& reg)
      {
         reg.endpoints[path] = [&value, func](jsonrpc::state&& state) mutable {
            if constexpr (std::same_as<Ret, void>) {
               (value.*func)();

               if (state.notify()) {
                  return;
               }

               jsonrpc::write_response<Opts>(state);
            }
            else {
               if (state.notify()) {
                  std::ignore = (value.*func)();
                  return;
               }

               jsonrpc::write_response<Opts>((value.*func)(), state);
            }
         };
      }

      template <class T, class F, class Input, class Ret, class RegistryType>
      static void register_member_function_with_params_endpoint(const sv path, T& value, F func, RegistryType& reg)
      {
         reg.endpoints[path] = [&value, func](jsonrpc::state&& state) mutable {
            static thread_local Input input{};
            if (state.has_body()) {
               if (!jsonrpc::read_params<Opts>(input, state)) {
                  return;
               }
            }

            if constexpr (std::same_as<Ret, void>) {
               (value.*func)(input);

               if (state.notify()) {
                  return;
               }

               jsonrpc::write_response<Opts>(state);
            }
            else {
               if (state.notify()) {
                  std::ignore = (value.*func)(input);
                  return;
               }

               jsonrpc::write_response<Opts>((value.*func)(input), state);
            }
         };
      }

      // Register a merged endpoint that combines multiple objects into a single response
      // Note: This endpoint is read-only; writing to it is not supported
      template <class... Ts, class RegistryType>
      static void register_merge_endpoint(const sv path, glz::merge<Ts...>& merged, RegistryType& reg)
      {
         reg.endpoints[path] = [&merged](jsonrpc::state&& state) mutable {
            // Merged endpoints are read-only
            if (state.has_body()) {
               jsonrpc::write_error(state, rpc::error_e::invalid_params, "Invalid params",
                                    "writing to merged endpoint is not supported");
               return;
            }

            if (state.notify()) {
               return;
            }

            jsonrpc::write_response<Opts>(merged, state);
         };
      }
   };
}
